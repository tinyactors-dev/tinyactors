// How many session actors fit on this machine?
//
// Grows the number of sessions step by step. After each step: settle, then
// measure probe latency, whether the host's touches are processed, and the
// bench's resident memory (RSS). It stops at the last healthy step before the
// memory budget — 80% of what this machine had available at start (or the
// cgroup limit, whichever is tighter) — or the latency SLO is exceeded.

const P = bench.params;
const M = bench.machine;
const cap = M.memoryLimit ? Math.min(M.memoryLimit, M.memAvailable + M.rss) : M.memAvailable + M.rss;
const cfg = {
  budget: P.memoryMB ? P.memoryMB * 1048576 : cap * (P.memoryFraction ?? 0.8),
  sloMs: P.sloMs ?? 100,
  touchRate: P.touchRate ?? 0.1, // touches per session per second
  idleMs: P.idleMs ?? 30000,
  start: P.start ?? bench.scaled(2000),
  fastGrowth: 2,
  growth: 1.25,
  settleMs: (P.settle ?? 1) * 1000,
  measureMs: (P.measure ?? 2) * 1000,
  maxActors: P.maxActors ?? 2000000,
};
const MB = (b) => Math.round(b / 1048576);
bench.log(`machine: ${MB(M.memTotal)} MB total, ${MB(M.memAvailable)} MB available, cgroup limit ${M.memoryLimit ? `${MB(M.memoryLimit)} MB` : 'none'}, ${M.cpus} CPUs → budget ${MB(cfg.budget)} MB of resident memory`);

bench.addSystem('fit', {
  charts: ['charts/probe.scxml', 'charts/session.scxml'],
  config: { maxActors: 4096, maxEvents: 16384, maxTimers: 4096, quantum: 400, jsMemoryMB: MB(cfg.budget) },
});
const probe = bench.spawn('fit', 'probe')[0];
const r = bench.rng(31);
const sessions = [];
const st = { phase: 'growing', target: cfg.start, chunk: 200, best: null, strikes: 0, slowed: false, rss0: bench.rss(), done: false };
globalThis.fit = st;

const latencies = [];
let probesSent = 0;
bench.on('probe', (d) => {
  if (st.phase === 'measuring' && d.t >= st.measureStart) latencies.push(bench.now() - d.t);
});
bench.every(100, () => {
  if (st.done) return;
  bench.send(probe, 'probe', { t: bench.now() });
  probesSent++;
  if (st.phase === 'measuring' && bench.now() - st.measureStart < cfg.measureMs - 200) st.probesExpected++;
});

// the host's traffic: touch random sessions with random ids
let touchAcc = 0;
bench.every(50, () => {
  if (st.done || !sessions.length) return;
  touchAcc += (sessions.length * cfg.touchRate * 50) / 1000;
  const n = Math.floor(touchAcc);
  touchAcc -= n;
  for (let i = 0; i < n; i++) bench.send(sessions[r.int(0, sessions.length - 1)], 'touch', { id: r.int(1, 100000) });
});

function spawnSome() {
  const need = st.target - sessions.length;
  if (need <= 0) return true;
  const n = Math.min(need, st.chunk);
  const t0 = bench.now();
  const ids = bench.spawn('fit', 'session', { count: n, data: () => ({ ids: Array.from({ length: 8 }, () => r.int(1, 100000)), idleMs: cfg.idleMs }) });
  for (const id of ids) sessions.push(id);
  const dt = Math.max(0.1, bench.now() - t0);
  st.chunk = Math.max(50, Math.min(20000, Math.round(st.chunk * Math.min(2, 20 / dt))));
  return false;
}

function percentile(v, p) {
  if (!v.length) return Infinity;
  const s = [...v].sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.floor(p * s.length))];
}

function finish(reason) {
  if (st.done) return;
  st.done = true;
  const b = st.best;
  const row = b
    ? {
        actors: b.actors,
        fitsInBudget: Math.floor((cfg.budget - st.rss0) / b.bytesPerActor), // extrapolated from the cost per actor
        bytesPerActor: Math.round(b.bytesPerActor),
        regionBytesPerActor: Math.round(b.regionPerActor),
        jsBytesPerActor: Math.round(b.jsPerActor),
        rssMB: MB(b.rss),
        budgetMB: MB(cfg.budget),
        probeP99Ms: Math.round(b.p99 * 100) / 100,
        stoppedBy: reason,
      }
    : { actors: 0, stoppedBy: reason };
  bench.result(row);
  if (b) bench.log(`${row.actors.toLocaleString("en-US")} sessions measured healthy; ≈ ${row.fitsInBudget.toLocaleString("en-US")} fit in the ${row.budgetMB} MB budget on this machine (${row.bytesPerActor} B each: ${row.jsBytesPerActor} B of ECMAScript heap, ${row.regionBytesPerActor} B in the actor system)`);
  bench.after(500, () => bench.stop(`fit: ${reason}`));
}
globalThis.fitFinish = finish;

function judge() {
  const s = bench.stats('fit');
  const actors = sessions.length;
  const secs = (bench.now() - st.measureStart) / 1000;
  const delivered = (s.delivered - st.deliveredAt) / secs;
  const touched = (s.hostSent - st.sentAt) / secs; // touches + probes the host sent
  const served = touched ? delivered / touched : 1;
  const lost = Math.max(0, st.probesExpected - latencies.length);
  const p99 = lost > 1 ? Infinity : percentile(latencies, 0.99);
  const rss = bench.rss();
  const healthy = p99 <= cfg.sloMs && served >= 0.85 && s.faults === 0;
  const perActor = (rss - st.rss0) / Math.max(1, actors);
  bench.metric('fit.actors', actors);
  bench.metric('fit.rssMB', MB(rss));
  bench.metric('fit.budgetMB', MB(cfg.budget));
  bench.metric('probeP99Ms.sessions', Number.isFinite(p99) ? p99 : cfg.sloMs * 4);
  bench.metric('bytesPerActor.total', perActor);
  bench.metric('bytesPerActor.jsHeap', s.jsHeap / Math.max(1, actors));
  bench.metric('bytesPerActor.region', s.regionBytes / Math.max(1, actors));
  bench.log(`${actors} sessions · RSS ${MB(rss)} / ${MB(cfg.budget)} MB · ${Math.round(perActor)} B/actor · probe p99 ${Number.isFinite(p99) ? p99.toFixed(1) : '∞'} ms · ${Math.round(served * 100)}% of touches processed ${healthy ? '✓' : '✗'}`);
  if (healthy) {
    st.best = { actors, rss, p99, bytesPerActor: perActor, regionPerActor: s.regionBytes / actors, jsPerActor: s.jsHeap / actors };
    st.strikes = 0;
  } else if (++st.strikes >= 2) {
    return finish(p99 > cfg.sloMs ? `latency (p99 ${Number.isFinite(p99) ? p99.toFixed(0) : '∞'} ms)` : served < 0.85 ? 'touches not processed' : 'faults');
  }
  if (actors >= cfg.maxActors) return finish(`maxActors (${cfg.maxActors})`);
  if (st.strikes) {
    st.phase = 'settling';
    st.settleUntil = bench.now() + cfg.settleMs;
    return;
  }
  const far = p99 < cfg.sloMs / 5 && !st.slowed && rss < cfg.budget / 2;
  if (!far) st.slowed = true;
  const next = Math.min(cfg.maxActors, Math.max(actors + 1000, Math.round(actors * (far ? cfg.fastGrowth : cfg.growth))));
  // memory after the next step: per-actor cost, plus the actor region doubling when it grows
  const projected = rss + perActor * (next - actors) + (next > s.actorsCap ? s.regionBytes : 0);
  if (projected > cfg.budget) return finish(`memory budget (${MB(cfg.budget)} MB)`);
  st.target = next;
  st.phase = 'growing';
}

bench.every(50, () => {
  if (st.done) return;
  const now = bench.now();
  if (st.phase === 'growing' && spawnSome()) {
    st.phase = 'settling';
    st.settleUntil = now + cfg.settleMs;
  } else if (st.phase === 'settling' && now >= st.settleUntil) {
    const s = bench.stats('fit');
    st.phase = 'measuring';
    st.measureStart = now;
    st.deliveredAt = s.delivered;
    st.sentAt = s.hostSent;
    st.probesExpected = 0;
    latencies.length = 0;
  } else if (st.phase === 'measuring' && now - st.measureStart >= cfg.measureMs) {
    judge();
  }
});

bench.describeMetric('fit', { title: 'Sessions and memory', unit: 'sessions · MB' });
bench.describeMetric('bytesPerActor', { title: 'Memory per session', unit: 'bytes', format: 'bytes' });
bench.describeMetric('probeP99Ms', { title: 'Probe latency p99', unit: 'milliseconds', format: 'ms' });
bench.onFinish(() => finish('run ended'));
bench.check('at least one healthy measurement', () => !!st.best);
