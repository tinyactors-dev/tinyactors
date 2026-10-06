// Hibernate: far more sessions than stay resident.
//
// ramp     spawn sessions in adaptive chunks up to `sessions` while the host
//          sweeps (every sweepMs) and hibernates actors that did not run since
//          the previous sweep — the system never does I/O; the bench writes the
//          blobs to a file and reads them back on TA_EFFECT_WAKE
// steady   skewed touches: hotShare of them go to a small hot set (resident),
//          the rest to random sessions (mostly hibernated: woken first). One in
//          `replyEvery` touches answers, measuring hot vs random latency.

const P = bench.params;
const cfg = {
  sessions: P.sessions ?? bench.scaled(2000000),
  hotFraction: P.hotFraction ?? 0.01,
  hotShare: P.hotShare ?? 0.9,
  touchRate: P.touchRate ?? bench.scaled(20000), // touches per second (steady phase)
  replyEvery: P.replyEvery ?? 50,
  sweepMs: P.sweepMs ?? 500,
  idleMs: P.idleMs ?? 600000,
};
const MB = (b) => Math.round(b / 1048576);
const fmt = (n) => String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ',');

bench.addSystem('sessions', {
  charts: ['charts/probe.scxml', 'charts/session.scxml'],
  config: { maxActors: 65536, maxEvents: 65536, maxTimers: 65536, quantum: 400 },
});
const probe = bench.spawn('sessions', 'probe')[0];
const r = bench.rng(7);
const ids = [];
let hot = [];
const st = { phase: 'ramp', chunk: 500, rss0: bench.rss(), rampStart: bench.now(), peakResident: 0, steadyStart: 0 };
const hotLat = [], randomLat = [], probeLat = [];
const all = { hot: [], random: [] }; // steady-phase samples for the result
globalThis.hib = st;

function pickHot() {
  const n = Math.max(1, Math.round(ids.length * cfg.hotFraction));
  hot = Array.from({ length: n }, () => ids[r.int(0, ids.length - 1)]);
}
st.newHotSet = () => {
  pickHot();
  bench.log(`new hot set: ${fmt(hot.length)} sessions`);
};
st.flashCrowd = (fraction) => {
  const n = Math.round(ids.length * fraction);
  for (let i = 0; i < n; i++) bench.send(ids[r.int(0, ids.length - 1)], 'touch', { id: r.int(1, 100000) });
  bench.log(`flash crowd: touched ${fmt(n)} random sessions`);
};

function percentile(v, p) {
  if (!v.length) return 0;
  const s = [...v].sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.floor(p * s.length))];
}

bench.on('probe', (d) => probeLat.push(bench.now() - d.t));
bench.on('touched', (d) => {
  const ms = bench.now() - d.t;
  (d.cold ? randomLat : hotLat).push(ms);
  if (st.phase === 'steady') (d.cold ? all.random : all.hot).push(ms);
});
bench.every(100, () => bench.send(probe, 'probe', { t: bench.now() }));

// ramp: spawn toward the target, ~20 ms of spawning per tick
bench.every(50, () => {
  if (st.phase !== 'ramp') return;
  const need = cfg.sessions - ids.length;
  if (need <= 0) {
    st.phase = 'steady';
    st.steadyStart = bench.now();
    pickHot();
    bench.log(`ramp done: ${fmt(ids.length)} sessions in ${((bench.now() - st.rampStart) / 1000).toFixed(1)} s; hot set ${fmt(hot.length)}`);
    return;
  }
  const n = Math.min(need, st.chunk);
  const t0 = bench.now();
  for (const id of bench.spawn('sessions', 'session', { count: n, data: () => ({ ids: Array.from({ length: 8 }, () => r.int(1, 100000)), idleMs: cfg.idleMs }) }))
    ids.push(id);
  const dt = Math.max(0.1, bench.now() - t0);
  st.chunk = Math.max(50, Math.min(50000, Math.round(st.chunk * Math.min(2, 20 / dt))));
});

// the host's eviction policy: hibernate whatever did not run since the last sweep
// (CLOCK: an actor is taken on the second sweep after it last ran, so while
// ramping only sessions spawned before the previous sweep are eligible)
st.swept = [];
bench.every(cfg.sweepMs, () => {
  st.swept.push(ids.length);
  bench.hibernate('sessions');
});

// steady traffic
let acc = 0, sent = 0;
bench.every(50, () => {
  if (st.phase !== 'steady') return;
  acc += (cfg.touchRate * 50) / 1000;
  const n = Math.floor(acc);
  acc -= n;
  for (let i = 0; i < n; i++) {
    const isHot = r() < cfg.hotShare;
    const target = isHot ? hot[r.int(0, hot.length - 1)] : ids[r.int(0, ids.length - 1)];
    const data = { id: r.int(1, 100000) };
    if (++sent % cfg.replyEvery === 0) Object.assign(data, { reply: true, t: bench.now(), cold: !isHot });
    bench.send(target, 'touch', data);
  }
});

let prev = null;
bench.every(1000, () => {
  const s = bench.stats('sessions');
  const sessions = s.actors - 1;
  const resident = sessions - s.hibernated;
  const rss = bench.rss();
  if (st.phase === 'steady') st.peakResident = Math.max(st.peakResident, resident);
  bench.metric('sessions.total', sessions);
  bench.metric('sessions.resident', resident);
  bench.metric('sessions.hibernated', s.hibernated);
  bench.metric('memory.rssMB', MB(rss));
  bench.metric('memory.storeMB', MB(s.storeBytes));
  bench.metric('bytesPerSession.rss', sessions ? (rss - st.rss0) / sessions : 0);
  bench.metric('bytesPerSession.blob', s.hibernations ? s.storeBytes / s.hibernations : 0);
  if (prev) {
    bench.metric('perSecond.wakes', s.wakes - prev.wakes);
    bench.metric('perSecond.hibernations', s.hibernations - prev.hibernations);
  }
  bench.metric('latencyMs.hot', percentile(hotLat, 0.99));
  bench.metric('latencyMs.random', percentile(randomLat, 0.99));
  bench.metric('latencyMs.probe', percentile(probeLat, 0.99));
  hotLat.length = randomLat.length = probeLat.length = 0;
  prev = s;
});

bench.describeMetric('sessions', { title: 'Sessions', unit: 'resident vs hibernated' });
bench.describeMetric('memory', { title: 'Memory', unit: 'MB (resident set, blob file)' });
bench.describeMetric('bytesPerSession', { title: 'Bytes per session', unit: 'RSS growth / sessions; blob size', format: 'bytes' });
bench.describeMetric('perSecond', { title: 'Wakes and hibernations', unit: 'per second' });
bench.describeMetric('latencyMs', { title: 'Touch latency p99', unit: 'ms: hot (resident) vs random (mostly hibernated)', format: 'ms' });

bench.onFinish(() => {
  const s = bench.stats('sessions');
  const sessions = s.actors - 1;
  const rss = bench.rss();
  const row = {
    sessions,
    hibernated: s.hibernated,
    peakResident: st.peakResident,
    rssMB: MB(rss),
    bytesPerSession: Math.round((rss - st.rss0) / Math.max(1, sessions)),
    blobBytes: Math.round(s.storeBytes / Math.max(1, s.hibernations)),
    storeMB: MB(s.storeBytes),
    hibernations: s.hibernations,
    wakes: s.wakes,
    hotP99Ms: Math.round(percentile(all.hot, 0.99) * 10) / 10,
    randomP99Ms: Math.round(percentile(all.random, 0.99) * 10) / 10,
    faults: s.faults,
  };
  bench.result(row);
  bench.log(`${fmt(sessions)} sessions, ${fmt(s.hibernated)} hibernated (peak ${fmt(st.peakResident)} resident in steady state) · RSS ${row.rssMB} MB = ${row.bytesPerSession} B/session · blobs ${row.blobBytes} B · touch p99 hot ${row.hotP99Ms} ms, random ${row.randomP99Ms} ms · ${fmt(s.wakes)} wakes`);
});
bench.check('most sessions are hibernated', () => {
  const s = bench.stats('sessions');
  const eligible = st.phase === 'ramp' ? (st.swept.at(-2) ?? 0) : s.actors - 1;
  return s.hibernated > 0.5 * eligible;
});
bench.check('touches of hibernated sessions are answered', () => all.random.length > 0 || st.phase === 'ramp');
bench.check('no faults', () => bench.stats('sessions').faults === 0);
