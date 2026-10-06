// PostgreSQL — durable actors under load (see scenario.json).
//
// waiting   until the PostgreSQL service is available (the dashboard's Start
//           button, or `mise run pg:start`)
// setup     a fresh set of account actors (the chart is defined once more:
//           a new version; earlier runs' accounts are removed)
// load      pgbench clients send transfers; two more pgbench processes are the
//           application's workers: a scheduler calling tinyactors.run() (timers,
//           deferred deposits) and an audit worker draining the outbox with
//           claim()/ack(). Every second the bench only measures: one snapshot
//           query checks that money is conserved, plus a one-event latency probe.

const P = bench.params;
const cfg = {
  accounts: Number(P.accounts ?? 2000),
  clients: Number(P.clients ?? 8),
  opening: 1000,
};
const TOTAL = cfg.accounts * cfg.opening;

const ACCOUNT = `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="account" initial="open">
  <datamodel>
    <data id="balance" expr="${cfg.opening}"/>
    <data id="sent" expr="0"/>
    <data id="received" expr="0"/>
    <data id="declined" expr="0"/>
    <data id="statements" expr="0"/>
  </datamodel>
  <state id="open">
    <onentry><send event="statement" delay="5s" id="statement"/></onentry>
    <transition event="transfer" cond="_event.data.amount &lt;= balance">
      <assign location="balance" expr="balance - _event.data.amount"/>
      <assign location="sent" expr="sent + 1"/>
      <send targetexpr="_event.data.to" event="deposit"><param name="amount" expr="_event.data.amount"/></send>
      <if cond="_event.data.amount &gt;= 95">
        <send type="outbox" target="audit" event="large-transfer"><param name="amount" expr="_event.data.amount"/></send>
      </if>
    </transition>
    <transition event="transfer"><assign location="declined" expr="declined + 1"/></transition>
    <transition event="deposit">
      <assign location="balance" expr="balance + _event.data.amount"/>
      <assign location="received" expr="received + 1"/>
    </transition>
    <transition event="statement" target="open"><assign location="statements" expr="statements + 1"/></transition>
  </state>
</scxml>`;

const st = {
  phase: 'waiting',
  base: 0,
  checks: 0,
  violations: 0,
  worstDrift: 0,
  restarts: 0,
  down: false,
  paused: false,
  maxInbox: 0,
  outboxDone: 0,
  peakEvents: 0,
  loadUntil: 0,
  events0: 0,
  events: 0,
  xacts: null,
  statements: null,
  lastWait: -1e9,
};
const sql = (s) => bench.pg.query(s);
const one = async (s) => (await sql(s))[0] || {};
const q = (s) => s.replace(/'/g, "''");

function transferScript(lo, n) {
  return [
    `\\set a random(0, ${n - 1})`,
    `\\set b random(0, ${n - 1})`,
    `\\set amt random(1, 100)`,
    `SELECT 1 FROM tinyactors.send(${lo} + :a, 'transfer', json_build_object('to', '#_scxml_' || (${lo} + :b), 'amount', :amt)::jsonb);`,
    '',
  ].join('\n');
}

// the application's workers, as pgbench scripts: what a real deployment runs
// next to its database (a cron-like scheduler, a queue consumer)
const SCHEDULER = 'SELECT tinyactors.run(1000);\n\\sleep 200 ms\n';
const AUDITOR = "SELECT tinyactors.ack(id) FROM tinyactors.claim(500, 'audit-worker');\n\\sleep 200 ms\n";

function startLoad() {
  const seconds = Math.max(1, Math.round(bench.duration() - bench.now() / 1000));
  bench.pg.load({ clients: cfg.clients, seconds, script: transferScript(st.base, cfg.accounts) });
  bench.pg.load({ clients: 1, seconds, script: SCHEDULER });
  bench.pg.load({ clients: 1, seconds, script: AUDITOR });
}

async function setup() {
  st.phase = 'setup';
  const t0 = bench.now();
  await sql(`SET client_min_messages = warning;
       DELETE FROM tinyactors.inbox WHERE actor IN (SELECT id FROM tinyactors.actors WHERE chart = 'account');
       DELETE FROM tinyactors.actors WHERE chart = 'account';
       DELETE FROM tinyactors.outbox WHERE target = 'audit';`);
  const v = (await one(`SELECT tinyactors.define('account', '${q(ACCOUNT)}') AS v`)).v;
  const r = await one(`SELECT min(id) AS lo, max(id) AS hi FROM (SELECT tinyactors.spawn('account') AS id FROM generate_series(1, ${cfg.accounts})) s`);
  st.base = Number(r.lo);
  if (Number(r.hi) - st.base + 1 !== cfg.accounts) throw new Error('accounts are not contiguous (another writer?)');
  bench.log(`chart account v${v}; ${cfg.accounts} accounts (ids ${r.lo}–${r.hi}) in ${Math.round(bench.now() - t0)} ms; ${cfg.clients} pgbench clients transferring`);
  st.events0 = Number((await one(`SELECT coalesce(sum(events), 0) AS e FROM tinyactors.actors WHERE chart = 'account'`)).e);
  startLoad();
  st.phase = 'load';
}

/* the API the mutations use */
globalThis.pgbank = {
  hotSpot(seconds) {
    if (st.phase !== 'load' || st.down) return bench.log('hot spot: not running yet');
    bench.pg.load({ clients: 8, seconds, script: transferScript(st.base, 3) });
    bench.log(`hot spot: 8 more clients on accounts ${st.base}–${st.base + 2} for ${seconds} s`);
  },
  restart() {
    if (st.phase !== 'load') return bench.log('restart: not running yet');
    bench.log('restarting PostgreSQL (fast shutdown, start)');
    bench.pg.stopLoad();
    st.down = true;
    bench.pg.restart();
  },
  togglePause() {
    st.paused = !st.paused;
    if (st.paused) bench.pg.stopLoad();
    else startLoad();
    bench.log(st.paused ? 'load paused' : 'load resumed');
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
    if (st.down) { /* back after a restart: nothing may be lost */
      st.down = false;
      st.restarts++;
      bench.log('PostgreSQL is back: checking what survived, resuming the load');
      if (!st.paused) startLoad();
    }
    // one snapshot: money, queues, progress, timers (the workers do the rest)
    const m = await one(`SELECT
        (SELECT coalesce(sum((datamodel->>'balance')::numeric), 0) FROM tinyactors.actors WHERE chart = 'account') AS balances,
        (SELECT coalesce(sum((i.data->>'amount')::numeric), 0) FROM tinyactors.inbox i WHERE i.event = 'deposit') AS in_flight,
        (SELECT count(*) FROM tinyactors.inbox) AS inbox,
        (SELECT count(*) FROM tinyactors.outbox WHERE delivered_at IS NULL) AS outbox,
        (SELECT coalesce(sum(events), 0) FROM tinyactors.actors WHERE chart = 'account') AS events,
        (SELECT coalesce(sum((datamodel->>'declined')::int), 0) FROM tinyactors.actors WHERE chart = 'account') AS declined,
        (SELECT coalesce(sum((datamodel->>'statements')::int), 0) FROM tinyactors.actors WHERE chart = 'account') AS statements,
        (SELECT count(*) FROM tinyactors.outbox WHERE target = 'audit' AND delivered_at IS NOT NULL) AS audited,
        (SELECT count(*) FROM tinyactors.actors WHERE chart = 'account' AND next_due_at < tinyactors.now() - interval '2 seconds') AS late,
        (SELECT xact_commit FROM pg_stat_database WHERE datname = current_database()) AS xacts`);
    const drift = Number(m.balances) + Number(m.in_flight) - TOTAL;
    st.checks++;
    if (drift !== 0) {
      st.violations++;
      if (Math.abs(drift) > Math.abs(st.worstDrift)) st.worstDrift = drift;
      bench.warn(`money is not conserved: drift ${drift}`);
    }
    const events = Number(m.events) - st.events0;
    const eps = Math.max(0, events - st.events);
    st.events = events;
    st.peakEvents = Math.max(st.peakEvents, eps);
    st.maxInbox = Math.max(st.maxInbox, Number(m.inbox));
    const xacts = Number(m.xacts);
    // latency of one event, end to end, from here
    const t0 = bench.now();
    await sql(`SELECT 1 FROM tinyactors.send(${st.base + cfg.accounts - 1}, 'deposit', '{"amount": 0}')`);
    bench.metric('latencyMs.send', bench.now() - t0);
    bench.metric('throughput.events', eps);
    if (st.xacts !== null) bench.metric('throughput.transactions', Math.max(0, xacts - st.xacts));
    st.xacts = xacts;
    bench.metric('queues.inbox', Number(m.inbox));
    bench.metric('queues.outbox', Number(m.outbox));
    const statements = Number(m.statements);
    if (st.statements !== null) bench.metric('timers.processed', Math.max(0, statements - st.statements));
    st.statements = statements;
    st.outboxDone = Number(m.audited);
    bench.metric('timers.late', Number(m.late));
    bench.metric('money.drift', drift);
    bench.metric('money.inFlight', Number(m.in_flight));
    st.declined = Number(m.declined);
  } catch (e) {
    if (!st.down) bench.warn(e.message); // already "PostgreSQL: ..."
  }
});

bench.describeMetric('throughput', { title: 'Throughput', unit: 'per second: events processed, transactions committed' });
bench.describeMetric('latencyMs', { title: 'Send latency', unit: 'milliseconds (one event, end to end)', format: 'ms' });
bench.describeMetric('queues', { title: 'Queues', unit: 'inbox: deferred events; outbox: effects not yet delivered' });
bench.describeMetric('timers', { title: 'Timers', unit: 'statements filed per second (run() worker); actors late by > 2 s' });
bench.describeMetric('money', { title: 'Money', unit: 'drift (must stay 0) and in flight (deposits in inboxes)' });
bench.describeMetric('postgres', { title: 'PostgreSQL', unit: '1 = available' });

bench.onFinish(() => {
  bench.pg.stopLoad();
  const seconds = Math.max(1, bench.now() / 1000);
  bench.result({
    accounts: cfg.accounts,
    events: st.events,
    perSecond: Math.round(st.events / seconds),
    peakPerSecond: st.peakEvents,
    declined: st.declined || 0,
    audited: st.outboxDone,
    maxInbox: st.maxInbox,
    restarts: st.restarts,
    moneyChecks: st.checks,
    drift: st.worstDrift,
  });
  bench.log(st.phase === 'waiting'
    ? 'PostgreSQL never became available'
    : `${st.events.toLocaleString()} events; money checked ${st.checks} times, drift ${st.worstDrift}; ${st.outboxDone} audit effects delivered; ${st.restarts} restart(s) survived`);
});

bench.check('PostgreSQL was available', () => st.phase !== 'waiting');
bench.check('money was conserved at every check (balances + deposits in flight)', () => st.checks > 0 && st.violations === 0);
bench.check('events were processed', () => st.events > 0);
bench.check('the outbox kept up (fewer than 2000 audit effects pending at the end)', async () => {
  try {
    return st.phase !== 'load' || Number((await one(`SELECT count(*) AS n FROM tinyactors.outbox WHERE delivered_at IS NULL AND target = 'audit'`)).n) < 2000;
  } catch {
    return false;
  }
});
