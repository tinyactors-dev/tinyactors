// The measurement lab: one host, one scenario, a population that follows a
// pattern over time. The page runs it in a Worker (worker.mjs) and charts
// what it reports; main.mjs runs every scenario small, seeded, in Node.js,
// for the website's tests. The lab takes the tinyactors package as an
// argument, so the same code runs in both.
//
// The population follows its pattern (PATTERNS): the lab spawns actors
// when there are fewer than the pattern asks for and ends actors drawn at
// random when there are more (adjust()). The scenario's work goes to the
// actors alive (work()):
//
// - idle: none. The actors wait for an event.
// - events: batches of `add` to counters drawn at random; each adds one.
// - rally: pairs of players send a ball back and forth, a hundred hits a
//   rally, with no JavaScript between hits.
// - sleep: at most `resident` actors in memory and the rest in a store;
//   each event goes to a counter drawn at random, which is almost always
//   asleep, so the host wakes it from the store first.
// - timers: every actor holds a delayed send to itself, due every 1 to 10
//   seconds; the lab moves a simulated clock on by STEP at a time, as fast
//   as the host keeps up, and each send that falls due is a tick.

export const SCENARIOS = ['idle', 'events', 'rally', 'sleep', 'timers'];
export const BATCH = 1000; // events a unit of work sends
export const HITS = 100; // hits a rally lasts
export const STEP = 100; // simulated milliseconds a unit of timers moves the clock
const PERIODS = 91; // a timer's period: 1 s to 10 s, in steps of STEP
const CHUNK = 4096; // spawns or ends between runs to idle

/**
 * The share of the peak population a pattern asks for at `t` seconds into
 * a run:
 * - steady: all of them, all the time;
 * - ramp: from a tenth up to all of them over a minute, and down again;
 * - waves: a wave a minute, from a tenth to all of them;
 * - bursts: a fifth, and all of them for 10 seconds out of every 40.
 */
export const PATTERNS = {
  steady: () => 1,
  ramp: (t) => { const phase = (t % 120) / 60; return 0.1 + 0.9 * (phase <= 1 ? phase : 2 - phase); },
  waves: (t) => 0.55 - 0.45 * Math.cos((2 * Math.PI * t) / 60),
  bursts: (t) => (t % 40 >= 20 && t % 40 < 30 ? 1 : 0.2),
};
export const target = (pattern, peak, t) => Math.round(peak * PATTERNS[pattern](t));

/**
 * A lab of at most `actors` actors running `scenario` on `datamodel`
 * ('native' or 'quickjs'). `charts` holds the text of counter.scxml,
 * player.scxml and timer.scxml.
 */
export function createLab(tinyactors, { scenario, actors, datamodel = 'native', charts, resident = 1000, seed = 1 }) {
  const { compileCharts, Host, SimulatedClock, MemoryStore } = tinyactors;
  if (!SCENARIOS.includes(scenario)) throw new TypeError(`scenario is one of ${SCENARIOS.join(', ')}`);
  const random = mulberry32(seed);
  const rally = scenario === 'rally', timers = scenario === 'timers';
  const per = rally ? 2 : 1; // actors a unit of the population holds: a pair of players, or one actor
  const peak = actors - (actors % per);
  const counts = { spawned: 0, ended: 0, messages: 0, points: 0, wakes: 0 };

  // The store of the sleep scenario counts what it holds, and its wakes.
  let store;
  if (scenario === 'sleep') {
    store = new MemoryStore();
    store.bytes = 0;
    const { save, load, delete: remove } = store;
    store.save = function (session, blob) { this.bytes += blob.byteLength - (this.blobs.get(session)?.byteLength ?? 0); save.call(this, session, blob); };
    store.load = function (session) { counts.wakes++; return load.call(this, session); };
    store.delete = function (session) { this.bytes -= this.blobs.get(session)?.byteLength ?? 0; remove.call(this, session); };
  }

  // The timers, counted by period and by when in it they fall due: what
  // falls due at each step, without a look at every timer. A timer of a
  // period of p steps that first falls due at step d falls due at every
  // step s with s % p === d % p.
  const offset = (p) => (p * (p - 1)) / 2 - 45; // where period p's residues start
  const due = new Float64Array(offset(10 + PERIODS));
  const clock = new SimulatedClock();

  const compiled = compileCharts([
    { name: 'counter.scxml', text: charts.counter },
    { name: 'player.scxml', text: charts.player },
    { name: 'timer.scxml', text: charts.timer },
  ]);
  const host = new Host(compiled, {
    system: 'lab',
    clock,
    datamodel,
    // quickjs keeps every actor's variables in its own heap, 1 GiB unless
    // a host sets more; WebAssembly addresses 4 GiB in all.
    ...(datamodel === 'quickjs' ? { quickjs: { memoryLimit: 3 * 2 ** 30 } } : {}),
    // Sized for the peak up front: a pool that runs out grows by copying
    // the region, and WebAssembly memory keeps its peak. Every pending
    // delayed send holds a timer and an event, and a timer that falls due
    // holds a second event for a moment: about one timer in fifty falls
    // due in each step. A chunk of new pairs of players queues a meet
    // each. Actors in the middle of a step hold a workspace each, a sender
    // until its send is handled.
    config: { system: {
      maxActors: peak + 64,
      maxEvents: 4 * BATCH + CHUNK + (timers ? peak + Math.ceil(peak / 16) : 0),
      ...(timers ? { maxTimers: peak + 64 } : {}),
      workspaces: 1024,
      tracing: false,
    } },
    ioProcessors: { 'urn:lab': { receive() { counts.points++; } } },
    ...(store ? { store, resident } : {}),
  });
  compiled.close();

  // The population alive: a counter's or a timer's session, or a pair's two
  // players, in slots 0 to alive - 1; an end moves the last into the gap.
  const first = new Float64Array(peak / per), second = rally ? new Float64Array(peak / per) : null;
  const residue = timers ? new Int32Array(peak) : null; // a timer's place in `due`
  let alive = 0;

  function spawn() {
    if (rally) {
      const a = host.spawn('player');
      const b = host.spawn('player', { params: { peer: a.address.text } });
      host.send(a.id, 'meet', { peer: b.address.text });
      first[alive] = Number(a.id); second[alive] = Number(b.id);
    } else if (timers) {
      // The first delay falls within the period, so the timers fall due spread out.
      const p = 10 + Math.floor(random() * PERIODS), delay = 1 + Math.floor(random() * p);
      residue[alive] = offset(p) + ((clock.now / STEP + delay) % p);
      due[residue[alive]]++;
      first[alive] = Number(host.spawn('timer', { params: { period: `${p * STEP}ms`, first: `${delay * STEP}ms` } }).id);
    } else first[alive] = Number(host.spawn('counter').id);
    alive++;
    counts.spawned += per;
  }

  function end() {
    const i = Math.floor(random() * alive);
    host.terminate(first[i]);
    if (rally) host.terminate(second[i]);
    if (timers) due[residue[i]]--;
    alive--;
    first[i] = first[alive];
    if (rally) second[i] = second[alive];
    if (timers) residue[i] = residue[alive];
    counts.ended += per;
  }

  /**
   * Spawns or ends actors toward `wanted`, at most a chunk of them, and
   * runs the host until idle; answers how many actors it spawned (or,
   * negative, ended).
   */
  async function adjust(wanted) {
    const goal = Math.floor(Math.min(peak, wanted) / per);
    const change = Math.max(-CHUNK, Math.min(CHUNK, goal - alive));
    for (let i = 0; i < change; i++) spawn();
    for (let i = 0; i < -change; i++) end();
    if (change !== 0) await host.idle();
    return change * per;
  }

  /** One unit of the scenario's work, run until the host is idle; answers the messages it handled. */
  async function work() {
    if (scenario === 'idle' || alive === 0) return 0;
    if (timers) {
      clock.increment(STEP);
      const step = clock.now / STEP;
      let ticks = 0;
      for (let p = 10; p < 10 + PERIODS; p++) ticks += due[offset(p) + (step % p)];
      await host.idle();
      counts.messages += ticks;
      return ticks;
    }
    const items = [];
    if (rally) {
      for (let i = 0; i < BATCH / 10; i++) items.push({ target: first[Math.floor(random() * alive)], event: 'serve', data: { hits: HITS } });
    } else {
      for (let i = 0; i < BATCH; i++) items.push({ target: first[Math.floor(random() * alive)], event: 'add' });
    }
    const points = counts.points;
    host.sendMany(items);
    await host.idle();
    // A rally is a serve and its hits, counted when it finishes.
    const messages = rally ? (counts.points - points) * (HITS + 1) : items.length;
    counts.messages += messages;
    return messages;
  }

  /** What the lab has done so far. */
  function stats() {
    return {
      scenario, datamodel, peak,
      actors: alive * per,
      spawned: counts.spawned,
      ended: counts.ended,
      messages: counts.messages,
      points: counts.points,
      wakes: counts.wakes,
      asleep: store ? store.blobs.size : 0,
      storeBytes: store ? store.bytes : 0,
      quickjsHeap: host.quickjsHeapBytes ?? null,
      simulated: clock.now / 1000, // seconds
    };
  }

  /** The variables of the i-th actor alive (a pair's first player), for main.mjs's checks. */
  const sample = (i) => host.actor(BigInt(first[i])).context();

  return { adjust, work, stats, sample, host, close: () => host.close() };
}

function mulberry32(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
