// PostgreSQL millions — durable sessions, a few hot (see scenario.json).
//
// waiting   until the PostgreSQL service is available
// setup     the scenario's own database (tinyactors_millions: actor ids are
//           contiguous there, so the hot set and the population are id
//           ranges); the population is durable and kept between runs
// ramp      4 pgbench spawners grow the population to the target while the
//           traffic runs; then they stop
// traffic   pgbench processes as an application would run them: clients
//           touching the hot set, a trickle of touches anywhere in the
//           population, a scheduler calling tinyactors.run(). Every second
//           the bench measures with cheap queries only (never a count over
//           millions of rows) and probes one hot event, one cold event and
//           one run().

const P = bench.params;
const cfg = {
  target: bench.scaled(Number(P.population ?? 2000000)),
  hot: Number(P.hot ?? 10000),
  db: String(P.database ?? 'tinyactors_millions'),
};
if (!/^[a-z][a-z0-9_]{0,62}$/.test(cfg.db)) throw new Error('invalid database name');

// The common timeout pattern: a small set of ids, an expiry a day after the
// last touch. Every idle actor has a pending timer (in the actors_due index).
const CHART = `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="websession" initial="active">
  <datamodel>
    <data id="owner" expr="0"/>
    <data id="ids" expr="[]"/>
    <data id="touches" expr="0"/>
  </datamodel>
  <state id="active">
    <onentry><send event="timeout" delay="86400s" id="timeout"/></onentry>
    <transition event="touch" target="active">
      <cancel sendid="timeout"/>
      <if cond="ids.indexOf(_event.data.id) &lt; 0 &amp;&amp; ids.length &lt; 16">
        <script>ids.push(_event.data.id)</script>
      </if>
      <assign location="touches" expr="touches + 1"/>
    </transition>
    <transition event="timeout" target="expired"/>
  </state>
  <final id="expired"/>
</scxml>`;

const st = {
  phase: 'waiting',
  lo: 0,
  stored: 0,
  stored0: 0,
  spawning: false,
  rampStart: 0,
  rampEnd: 0,
  rampFrom: 0,
  down: false,
  restarts: 0,
  upd: null,
  ins: null,
  popSamples: [], /* {t, stored} while the spawners run: growth, from the id sequence */
  events: 0,
  errors: 0,
  lastWait: -1e9,
  hotMs: [],
  coldMs: [],
  runMs: [],
};
const sql = (s) => bench.pg.query(s);
const one = async (s) => (await sql(s))[0] || {};
const q = (s) => s.replace(/'/g, "''");
const median = (a) => (a.length ? [...a].sort((x, y) => x - y)[Math.floor(a.length / 2)] : null);

const HOT = (lo, n) =>
  `\\set id random(${lo}, ${lo + n - 1})\n\\set k random(1, 1000)\nSELECT 1 FROM tinyactors.send(:id, 'touch', jsonb_build_object('id', :k));\n`;
// deliver(): ids not spawned yet are simply false (no error, no wait)
const COLD = (lo, n, pause) =>
  `\\set id random(${lo}, ${lo + n - 1})\n\\set k random(1, 1000)\nSELECT tinyactors.deliver(:id, 'touch', jsonb_build_object('id', :k));\n${pause ? `\\sleep ${pause} ms\n` : ''}`;
const SCHEDULER = 'SELECT tinyactors.run(1000);\n\\sleep 200 ms\n';
const SPAWN = "SELECT count(tinyactors.spawn('websession', jsonb_build_object('owner', :client_id))) FROM generate_series(1, 1000);\n";

const remaining = () => Math.max(1, Math.round(bench.duration() - bench.now() / 1000));

function startLoads() {
  const seconds = remaining();
  bench.pg.load({ clients: 6, seconds, script: HOT(st.lo, cfg.hot) });
  bench.pg.load({ clients: 2, seconds, script: COLD(st.lo, cfg.target, 4) });
  bench.pg.load({ clients: 1, seconds, script: SCHEDULER });
  if (st.stored < cfg.target) {
    bench.pg.load({ clients: 4, seconds, script: SPAWN });
    if (!st.spawning) {
      st.rampStart = bench.now();
      st.rampFrom = st.stored;
    }
    st.spawning = true;
  }
}

async function setup() {
  st.phase = 'setup';
  const t0 = bench.now();
  const exists = (await sql(`SELECT 1 FROM pg_database WHERE datname = '${cfg.db}'`)).length > 0;
  if (exists && P.reset) {
    await sql(`DROP DATABASE ${cfg.db} WITH (FORCE)`);
    bench.log(`reset: dropped ${cfg.db}`);
  }
  if (!exists || P.reset) await sql(`CREATE DATABASE ${cfg.db}`);
  bench.pg.use(cfg.db);
  await sql(`SET client_min_messages = warning; CREATE EXTENSION IF NOT EXISTS tinyactors`);
  const cur = (await one(`SELECT scxml FROM tinyactors.charts WHERE name = 'websession' ORDER BY version DESC LIMIT 1`)).scxml;
  if (cur !== CHART) bench.log(`chart websession v${(await one(`SELECT tinyactors.define('websession', '${q(CHART)}') AS v`)).v}`);
  let r = await one(`SELECT min(id) AS lo, max(id) AS hi FROM tinyactors.actors`); // primary key ends: instant
  if (r.lo == null || Number(r.hi) - Number(r.lo) + 1 < cfg.hot) {
    const need = cfg.hot - (r.lo == null ? 0 : Number(r.hi) - Number(r.lo) + 1);
    await sql(`SELECT count(tinyactors.spawn('websession')) FROM generate_series(1, ${need})`);
    r = await one(`SELECT min(id) AS lo, max(id) AS hi FROM tinyactors.actors`);
  }
  st.lo = Number(r.lo);
  st.stored = st.stored0 = Number(r.hi) - st.lo + 1;
  bench.log(`${st.stored.toLocaleString()} session actors stored (target ${cfg.target.toLocaleString()}, hot set ${cfg.hot.toLocaleString()}) — setup ${Math.round(bench.now() - t0)} ms`);
  startLoads();
  st.phase = 'traffic';
}

async function probe(list, statement) {
  const t0 = bench.now();
  await sql(statement);
  const ms = bench.now() - t0;
  list.push(ms);
  return ms;
}

/* the API the mutations use */
globalThis.pgmillions = {
  flash(seconds) {
    if (st.phase !== 'traffic' || st.down) return bench.log('flash crowd: not running yet');
    const n = Math.min(st.stored, cfg.hot * 10);
    bench.pg.load({ clients: 4, seconds, script: HOT(st.lo, n) });
    bench.log(`flash crowd: 4 more clients over ${n.toLocaleString()} actors for ${seconds} s`);
  },
  coldSweep(seconds) {
    if (st.phase !== 'traffic' || st.down) return bench.log('cold sweep: not running yet');
    bench.pg.load({ clients: 4, seconds, script: COLD(st.lo, st.stored, 0) });
    bench.log(`cold sweep: 4 clients touching random actors among ${st.stored.toLocaleString()} for ${seconds} s`);
  },
  oneHot(seconds) {
    if (st.phase !== 'traffic' || st.down) return bench.log('one hot actor: not running yet');
    bench.pg.load({ clients: 8, seconds, script: HOT(st.lo, 1) });
    bench.log(`one hot actor: 8 clients on actor ${st.lo} for ${seconds} s (its row lock serializes them)`);
  },
  restart() {
    if (st.phase !== 'traffic') return bench.log('restart: not running yet');
    bench.log('restarting PostgreSQL (fast shutdown, start)');
    bench.pg.stopLoad();
    st.down = true;
    bench.pg.restart();
  },
};

bench.pg.status(); // request the asynchronous probe before the first tick
bench.every(1000, async () => {
  const s = bench.pg.status();
  bench.metric('postgres.available', s.state === 'available' ? 1 : 0);
  if (s.state !== 'available') {
    if (st.phase === 'waiting' && bench.now() - st.lastWait > 5000) {
      st.lastWait = bench.now();
      bench.log(`waiting for PostgreSQL (${s.state}${s.detail ? `: ${s.detail}` : ''}) — press "Start PostgreSQL" above, or run mise run pg:start`);
    }
    return;
  }
  try {
    if (st.phase === 'waiting') return await setup();
    if (st.down) {
      st.down = false;
      st.restarts++;
      bench.log('PostgreSQL is back: resuming the traffic');
      st.upd = st.ins = null; /* the downtime must not look like a stalled second */
      startLoads();
    }
    const m = await one(`SELECT pg_stat_clear_snapshot(); /* the connection's stats snapshot may be seconds old */
        SELECT (SELECT last_value FROM tinyactors.actor_id_seq) AS seq,
        (SELECT n_tup_upd FROM pg_stat_user_tables WHERE relid = 'tinyactors.actors'::regclass) AS upd,
        (SELECT n_tup_ins FROM pg_stat_user_tables WHERE relid = 'tinyactors.actors'::regclass) AS ins,
        pg_total_relation_size('tinyactors.actors') AS bytes`);
    st.stored = Math.max(st.stored, Number(m.seq) - st.lo + 1);
    if (st.spawning) st.popSamples.push({ t: bench.now(), stored: st.stored });
    if (st.spawning && st.stored >= cfg.target) { /* ramp done: the traffic carries on without spawners */
      st.spawning = false;
      st.rampEnd = bench.now();
      bench.pg.stopLoad();
      startLoads();
      const rate = (st.stored - st.rampFrom) / Math.max(0.001, (st.rampEnd - st.rampStart) / 1000);
      bench.log(`population reached ${st.stored.toLocaleString()} (${Math.round(rate).toLocaleString()} spawns/s under traffic); spawners stopped`);
    }
    const upd = Number(m.upd), ins = Number(m.ins);
    if (st.upd !== null) {
      const eps = Math.max(0, upd - st.upd);
      const spawns = Math.max(0, ins - st.ins);
      st.events += eps;
      bench.metric('throughput.events', eps);
      bench.metric('throughput.spawns', spawns);
    }
    st.upd = upd;
    st.ins = ins;
    const bytes = Number(m.bytes);
    bench.metric('population.stored', st.stored);
    bench.metric('population.target', cfg.target);
    bench.metric('storage.MB', bytes / 1048576);
    bench.metric('storage.bytesPerActor', bytes / st.stored);
    const hot = st.lo + Math.floor(Math.random() * cfg.hot);
    const cold = st.lo + Math.floor(Math.random() * st.stored);
    bench.metric('latencyMs.hot', await probe(st.hotMs, `SELECT 1 FROM tinyactors.send(${hot}, 'touch', '{"id": 0}')`));
    bench.metric('latencyMs.cold', await probe(st.coldMs, `SELECT tinyactors.deliver(${cold}, 'touch', '{"id": 0}')`));
    bench.metric('latencyMs.run', await probe(st.runMs, `SELECT tinyactors.run(100)`));
  } catch (e) {
    if (!st.down) {
      st.errors++;
      bench.warn(e.message);
    }
  }
});

bench.describeMetric('population', { title: 'Population', unit: 'session actors stored (durable; kept between runs) and the target' });
bench.describeMetric('throughput', { title: 'Throughput', unit: 'per second: events processed (actor updates), actors spawned' });
bench.describeMetric('latencyMs', { title: 'Latency', unit: 'milliseconds: one event to a hot actor, to a random (cold) actor; one run()', format: 'ms' });
bench.describeMetric('storage', { title: 'Storage', unit: 'actors table with indexes (MB) and bytes per actor' });
bench.describeMetric('postgres', { title: 'PostgreSQL', unit: '1 = available' });

bench.onFinish(async () => {
  bench.pg.stopLoad();
  try {
    if (st.spawning) { /* the last tick may be a second old: one more reading for the growth check */
      const r = await one(`SELECT last_value AS seq FROM tinyactors.actor_id_seq`);
      st.stored = Math.max(st.stored, Number(r.seq) - st.lo + 1);
      st.popSamples.push({ t: bench.now(), stored: st.stored });
    }
  } catch (e) { /* the last tick's reading stands */ }
  const spawned = st.stored - st.stored0;
  const rampSecs = ((st.rampEnd || bench.now()) - st.rampStart) / 1000;
  bench.result({
    target: cfg.target,
    stored: st.stored,
    spawned,
    spawnsPerSecond: st.rampStart && spawned ? Math.round(spawned / Math.max(0.001, rampSecs)) : 0,
    events: st.events,
    eventsPerSecond: Math.round(st.events / Math.max(1, bench.now() / 1000)),
    hotMs: median(st.hotMs),
    coldMs: median(st.coldMs),
    runMs: median(st.runMs),
    restarts: st.restarts,
  });
  bench.log(st.phase === 'waiting'
    ? 'PostgreSQL never became available'
    : `${st.stored.toLocaleString()} actors stored (${spawned.toLocaleString()} spawned this run); median latency hot ${median(st.hotMs)?.toFixed(1)} ms, cold ${median(st.coldMs)?.toFixed(1)} ms, run() ${median(st.runMs)?.toFixed(2)} ms`);
});

bench.check('PostgreSQL was available', () => st.phase !== 'waiting');
bench.check('the population reached its target, or kept growing as the table grew (late spawn rate at least half the early one)', () => {
  if (st.stored >= cfg.target) return true;
  if (st.stored <= st.stored0) return false; /* the spawners moved nothing */
  /* Growth is judged against this run's own early rate — not an absolute
     number, which would only measure the machine and its load. The
     population (from the id sequence, visible at once) is sampled every
     second while the spawners run. Warmup is the time before the first
     sample that shows progress; the rest of the window is split in half by
     time, and the rate over the later half — the table at its largest —
     must stay above half the rate over the earlier half. With too short a
     window to split (torture:test runs 4 s), the growth itself is the
     check. */
  const s = st.popSamples;
  const first = s.findIndex((p) => p.stored > s[0].stored);
  if (first < 0 || s.length - first < 3) return true;
  const w = s.slice(first);
  const midT = (w[0].t + w[w.length - 1].t) / 2;
  let k = w.findIndex((p) => p.t >= midT);
  if (k <= 0) k = 1;
  if (k > w.length - 1) k = w.length - 1;
  const a = w[k - 1], b = w[k];
  const mid = b.t > a.t ? a.stored + ((b.stored - a.stored) * (midT - a.t)) / (b.t - a.t) : b.stored;
  const early = (mid - w[0].stored) / Math.max(0.001, (midT - w[0].t) / 1000);
  const late = (w[w.length - 1].stored - mid) / Math.max(0.001, (w[w.length - 1].t - midT) / 1000);
  return early <= 0 || late >= early / 2;
});
bench.check('a hot event takes under 50 ms (median)', () => st.hotMs.length > 0 && median(st.hotMs) < 50);
bench.check('a cold event takes under 50 ms (median)', () => st.coldMs.length > 0 && median(st.coldMs) < 50);
bench.check('run() costs what it finds, not what is stored: under 20 ms (median)', () => st.runMs.length > 0 && median(st.runMs) < 20);
bench.check('no errors', () => st.errors === 0);
