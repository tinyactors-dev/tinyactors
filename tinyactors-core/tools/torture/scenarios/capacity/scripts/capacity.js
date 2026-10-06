// Capacity search: for each messaging pattern, determine the largest actor
// count this machine sustains, and what stops it.
//
// One step:  resize (spawn / terminate in adaptive chunks) → settle → measure → judge
//   healthy  = probe p99 ≤ sloMs, no lost probes, delivered ≥ minRate × expected,
//              no faults
//   search   healthy → grow by max(minStep, growth × actors), fastGrowth while
//            far from the limit; at most to what fits the memory budget
//   refine   unhealthy twice at N → the limit lies in (last healthy, N]: bisect
//            until the bracket is within `tolerance`. A smaller midpoint starts
//            from a fresh system: an overloaded one does not recover on its own
//            (timers re-armed during the overload stay clustered — metastable)
//   ends at  the bracket, the memory budget (no room for `tolerance` more),
//            the runtime's per-system limit, or maxActors / phaseSeconds if set
// The memory budget is resident memory: 80% of what the machine had available
// at start, or of the cgroup limit when tighter (--param memoryFraction, memoryMB).

const P = bench.params;
const cfg = {
  patterns: String(P.patterns || 'idle,heartbeat,cells,gossip,ingress').split(',').map((s) => s.trim()).filter(Boolean),
  start: P.start ?? bench.scaled(1000),
  growth: P.growth ?? 1.25, // near the limit
  fastGrowth: P.fastGrowth ?? 2, // while far from it (p99 < SLO/5, ≥ 95% delivered)
  minStep: P.minStep ?? bench.scaled(1000),
  sloMs: P.sloMs ?? 100,
  minRate: P.minRate ?? 0.85,
  settleMs: (P.settle ?? 1) * 1000,
  measureMs: (P.measure ?? 2) * 1000,
  maxActors: P.maxActors ?? Infinity,
  phaseSeconds: P.phaseSeconds ?? 900,
  tolerance: P.tolerance ?? 0.05, // the limit is determined to within this fraction
  probeEveryMs: 100,
};
const M = bench.machine;
const machineCap = M.memoryLimit ? Math.min(M.memoryLimit, M.memAvailable + M.rss) : M.memAvailable + M.rss;
cfg.budget = P.memoryMB ? P.memoryMB * 1048576 : machineCap * (P.memoryFraction ?? 0.8);
cfg.memoryMB = Math.round(cfg.budget / 1048576);
const SYSTEM_MAX = M.systemMaxActors - 1; // minus the probe
const MB = (b) => Math.round(b / 1048576);
const fmt = (n) => String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ','); // QuickJS has no Intl

/** expected: events delivered per actor per second (excluding probes) */
const PATTERNS = {
  idle: { chart: 'idle', charts: ['charts/idle.scxml'], expected: 0, about: 'no messages' },
  heartbeat: { chart: 'heartbeat', charts: ['charts/heartbeat.scxml'], expected: 1, about: '1 s delayed send to itself' },
  cells: {
    chart: 'cell',
    charts: [{ name: 'cell.scxml', script: 'charts/cells.js', params: { fanout: 8 } }],
    actorsPerSpawn: 9,
    expected: 17 / 9,
    about: 'parent ticks 8 invoked children every second, each replies',
  },
  gossip: { chart: 'gossip', charts: ['charts/gossip.scxml'], expected: 2, peers: 4, about: 'ECMAScript: ~1 msg/s to a random peer by session address' },
  ingress: { chart: 'sink', charts: ['charts/sink.scxml'], expected: 1, host: true, about: 'host sends 1 event per actor per second' },
};
for (const name of cfg.patterns) if (!PATTERNS[name]) throw new Error(`unknown pattern "${name}"`);

bench.log(`capacity search: ${cfg.patterns.join(', ')} · SLO p99 ≤ ${cfg.sloMs} ms · ≥ ${cfg.minRate * 100}% of expected traffic · limit ±${cfg.tolerance * 100}% · budget ${cfg.memoryMB} MB resident (${M.memoryLimit ? `cgroup ${MB(M.memoryLimit)} MB` : `${MB(M.memAvailable)} MB available`}) · ≤ ${fmt(SYSTEM_MAX)} actors per system`);

const capacity = (globalThis.capacity = { hold: false, results: [], finishPhase: () => {} });
let phase = null; // current pattern run
let queue = [...cfg.patterns];
const latencies = [];
let probesSent = 0, probesBack = 0;

bench.on('probe', (d) => {
  probesBack++;
  if (phase && phase.state === 'measuring' && d.t >= phase.measureStart) latencies.push(bench.now() - d.t);
});

function percentile(values, p) {
  if (!values.length) return Infinity;
  const v = [...values].sort((a, b) => a - b);
  return v[Math.min(v.length - 1, Math.floor(p * v.length))];
}

let systemSeq = 0;
function newSystem(name, def) {
  const sys = `cap-${name}-${++systemSeq}`;
  bench.addSystem(sys, {
    charts: ['charts/probe.scxml', ...def.charts],
    config: { maxActors: 4096, maxEvents: 16384, maxTimers: 4096, quantum: 400, jsMemoryMB: cfg.memoryMB },
  });
  return { sys, probe: bench.spawn(sys, 'probe')[0] };
}

/** Bisecting downwards: measure the midpoint on a fresh system. */
function rebuild(target) {
  bench.removeSystem(phase.sys);
  Object.assign(phase, newSystem(phase.name, phase.def), { ids: [], target, state: 'growing' });
}

function startPhase(name) {
  const def = PATTERNS[name];
  const { sys, probe } = newSystem(name, def);
  phase = {
    name, def, sys, probe,
    ids: [],
    target: cfg.start,
    best: null, // { actors, p99, rate, bytesPerActor }
    high: null, // smallest unhealthy count: the limit lies in (best.actors, high]
    highReason: null,
    strikes: 0,
    rss0: bench.rss(),
    state: 'growing',
    started: bench.now(),
    chunk: 500,
    hostAcc: 0,
  };
  bench.log(`pattern ${name}: ${def.about}`);
}

function actorsNow() {
  const s = bench.stats(phase.sys);
  return s ? s.actors - 1 : 0; // minus the probe
}

function spawnSome() {
  // count what was requested: invoked children appear asynchronously
  const per = phase.def.actorsPerSpawn || 1;
  const need = phase.target - Math.max(actorsNow(), phase.ids.length * per);
  if (need <= 0) return true;
  const n = Math.max(1, Math.min(Math.ceil(need / per), phase.chunk));
  const t0 = bench.now();
  let data;
  if (phase.def.peers) {
    const ids = phase.ids;
    const probe = phase.probe; // the first actors gossip to the probe (it ignores rumors)
    data = () => ({ peers: ids.length ? Array.from({ length: Math.min(phase.def.peers, ids.length) }, () => bench.address(ids[Math.floor(bench.random() * ids.length)])) : [bench.address(probe)] });
  }
  let ids;
  try {
    ids = bench.spawn(phase.sys, phase.def.chart, { count: n, data });
  } catch (e) {
    finishPhase(`runtime: ${e.message}`); // the system could not grow any more
    return false;
  }
  for (const id of ids) phase.ids.push(id);
  // adapt the chunk so one tick spends ~20 ms spawning
  const dt = Math.max(0.1, bench.now() - t0);
  phase.chunk = Math.max(50, Math.min(50000, Math.round(phase.chunk * Math.min(2, 20 / dt))));
  return false;
}

function finishPhase(reason) {
  if (!phase || phase.state === 'done') return;
  const b = phase.best;
  const row = {
    pattern: phase.name,
    capacity: b ? b.actors : 0,
    eventsPerSecond: b ? Math.round(b.rate) : 0,
    probeP99Ms: b ? Math.round(b.p99 * 100) / 100 : null,
    bytesPerActor: b ? Math.round(b.bytesPerActor) : null,
    jsBytesPerActor: b ? Math.round(b.jsBytesPerActor) : null,
    rssBytesPerActor: b ? Math.round(b.rssPerActor) : null,
    fitsInBudget: b ? Math.floor((cfg.budget - phase.rss0) / b.rssPerActor) : null,
    unhealthyAt: phase.high,
    stoppedBy: reason,
  };
  bench.log(b ? `${phase.name}: limit ${fmt(row.capacity)} actors${phase.high ? ` (unhealthy at ${fmt(phase.high)})` : ''} · ${reason} · ${row.rssBytesPerActor} B resident per actor, ≈ ${fmt(row.fitsInBudget)} would fit the memory budget` : `${phase.name}: no healthy measurement · ${reason}`);
  capacity.results.push(row);
  bench.result(row);
  bench.metric(`capacity.${phase.name}`, row.capacity);
  phase.state = 'done';
  const sys = phase.sys;
  phase = null;
  bench.after(200, () => {
    bench.removeSystem(sys); // frees the region and its JS runtime before the next pattern
    if (queue.length) bench.after(300, () => startPhase(queue.shift()));
    else bench.after(300, () => bench.stop('capacity search complete'));
  });
}
capacity.finishPhase = finishPhase;

function judge() {
  const s = bench.stats(phase.sys);
  const actors = s.actors - 1;
  const secs = (bench.now() - phase.measureStart) / 1000;
  const delivered = (s.delivered - phase.deliveredAtStart) / secs;
  const probes = (probesSent - phase.probesAtStart) / secs;
  const rate = Math.max(0, delivered - probes);
  const expected = phase.def.expected * actors;
  const lost = Math.max(0, phase.probesExpected - latencies.length);
  const p99 = lost > 1 ? Infinity : percentile(latencies, 0.99);
  const served = expected ? rate / expected : 1;
  const faults = s.faults - phase.faultsAtStart;
  const healthy = p99 <= cfg.sloMs && served >= cfg.minRate && faults === 0;
  bench.metric(`probeP99Ms.${phase.name}`, Number.isFinite(p99) ? p99 : cfg.sloMs * 4);
  bench.metric(`servedPct.${phase.name}`, Math.min(200, served * 100));
  bench.log(`${phase.name}: ${actors} actors · ${Math.round(rate)} events/s (${Math.round(served * 100)}% of expected) · probe p99 ${Number.isFinite(p99) ? p99.toFixed(1) : '∞'} ms ${healthy ? '✓' : '✗'}`);
  const rss = bench.rss();
  const rssPerActor = Math.max(1, (rss - phase.rss0) / Math.max(1, actors));
  bench.metric(`rssMB.${phase.name}`, MB(rss));
  if (healthy) {
    phase.best = { actors, p99, rate, bytesPerActor: s.regionBytes / Math.max(1, actors), jsBytesPerActor: s.jsHeap / Math.max(1, actors), rssPerActor };
    phase.strikes = 0;
    bench.metric(`capacity.${phase.name}`, actors);
  } else if (++phase.strikes >= 2) {
    phase.strikes = 0;
    const why = p99 > cfg.sloMs ? `latency (p99 ${Number.isFinite(p99) ? p99.toFixed(0) : '∞'} ms)` : served < cfg.minRate ? `throughput (${Math.round(served * 100)}% delivered)` : 'faults';
    if (phase.high === null || actors < phase.high) {
      phase.high = actors;
      phase.highReason = why;
    }
    if (!phase.best) return finishPhase(why);
  }
  if (phase.strikes) {
    phase.state = 'settling'; // measure again at the same size
    phase.settleUntil = bench.now() + cfg.settleMs;
    return;
  }
  if ((bench.now() - phase.started) / 1000 > cfg.phaseSeconds) return finishPhase('phase time limit');
  const low = phase.best.actors;
  if (phase.high !== null) {
    // refine: bisect (low, high]
    if (phase.high - low <= Math.max(cfg.minStep / 4, low * cfg.tolerance)) return finishPhase(`${phase.highReason} at ${fmt(phase.high)}`);
    if (capacity.hold) phase.state = 'growing';
    else if (Math.round((low + phase.high) / 2) < actors) rebuild(Math.round((low + phase.high) / 2));
    else {
      phase.target = Math.round((low + phase.high) / 2);
      phase.state = 'growing';
    }
    return;
  }
  if (actors >= cfg.maxActors) return finishPhase(`maxActors (${fmt(cfg.maxActors)})`);
  if (actors >= SYSTEM_MAX * (1 - cfg.tolerance)) return finishPhase(`runtime limit (${fmt(SYSTEM_MAX + 1)} actors per system)`);
  const far = p99 < cfg.sloMs / 5 && served >= 0.95 && !phase.slowed;
  if (!far) phase.slowed = true; // once close, stay careful
  let next = Math.min(cfg.maxActors, SYSTEM_MAX, Math.max(actors + cfg.minStep, Math.round(actors * (far ? cfg.fastGrowth : cfg.growth))));
  // memory: resident now + the measured cost per actor, + a copy of the region
  // while it grows (relocation keeps the old one until the copy is done)
  const cap = s.actorsCap - 1;
  const room = cfg.budget - rss;
  const noRegrow = Math.min(cap, actors + Math.floor(room / rssPerActor));
  const withRegrow = actors + Math.floor((room - s.regionBytes) / rssPerActor);
  const fits = Math.max(noRegrow, withRegrow > cap ? withRegrow : 0);
  if (next > fits) {
    if (fits < actors * (1 + cfg.tolerance)) return finishPhase(`memory budget (${cfg.memoryMB} MB resident)`);
    next = fits;
    phase.slowed = true;
  }
  if (!capacity.hold) phase.target = next;
  phase.state = 'growing';
}

bench.every(50, () => {
  if (!phase) return;
  const now = bench.now();
  // host ingress: 1 event per actor per second to random actors
  if (phase.def.host && phase.ids.length) {
    phase.hostAcc += (phase.ids.length * 50) / 1000;
    const n = Math.floor(phase.hostAcc);
    phase.hostAcc -= n;
    const ids = phase.ids;
    for (let i = 0; i < n; i++) bench.send(ids[Math.floor(bench.random() * ids.length)], 'msg');
  }
  switch (phase.state) {
    case 'growing':
      if (spawnSome()) {
        phase.state = 'settling';
        phase.settleUntil = now + cfg.settleMs;
      }
      break;
    case 'settling':
      if (now >= phase.settleUntil) {
        const s = bench.stats(phase.sys);
        phase.state = 'measuring';
        phase.measureStart = now;
        phase.deliveredAtStart = s.delivered;
        phase.faultsAtStart = s.faults;
        phase.probesAtStart = probesSent;
        phase.probesExpected = 0;
        latencies.length = 0;
      }
      break;
    case 'measuring':
      if (now - phase.measureStart >= cfg.measureMs) judge();
      break;
  }
});

bench.every(cfg.probeEveryMs, () => {
  if (!phase) return;
  bench.send(phase.probe, 'probe', { t: bench.now() });
  probesSent++;
  if (phase.state === 'measuring' && bench.now() - phase.measureStart < cfg.measureMs - cfg.probeEveryMs * 2) phase.probesExpected++;
});

bench.describeMetric('capacity', { title: 'Capacity', unit: 'healthy actors per pattern' });
bench.describeMetric('probeP99Ms', { title: 'Probe latency p99', unit: 'milliseconds per pattern', format: 'ms' });
bench.describeMetric('rssMB', { title: 'Resident memory', unit: 'MB per pattern (budget: ' + cfg.memoryMB + ' MB)' });
bench.describeMetric('servedPct', { title: 'Traffic delivered', unit: '% of the pattern’s expected events', format: 'pct' });

bench.onFinish(() => {
  if (phase) finishPhase('run ended');
  for (const name of queue) bench.result({ pattern: name, capacity: 0, stoppedBy: 'not reached' });
  queue = [];
});
bench.check('at least one pattern sustained a measured load', () => capacity.results.some((r) => r.capacity > 0));

startPhase(queue.shift());
