// The measurement lab: one host, one scenario. The page runs it in a
// Worker (worker.mjs) and charts what it reports; main.mjs runs every
// scenario small, seeded, in Node.js, for the website's tests. The lab
// takes the tinyactors package as an argument, so the same code runs in
// both.
//
// - cells: a colony (cell.scxml) whose population is the cells' own doing.
//   Each cell burns a unit of energy a simulated second and ages; the
//   light, the lab's I/O processor urn:lab, feeds cells drawn at random,
//   as much food a second as `actors` cells burn, and names a neighbour
//   to each; a cell with energy to spare gives some to its neighbour, one
//   with a full store divides (the light spawns the daughter), and a cell
//   dies of hunger or of age in a final state. The lab moves a simulated
//   clock on by STEP at a time, as fast as the host keeps up.
//
// The other scenarios hold `actors` actors (adjust() spawns them), and
// their work goes to the actors alive (work()):
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

export const SCENARIOS = ['cells', 'idle', 'events', 'rally', 'sleep', 'timers'];
export const BATCH = 1000; // events a unit of work sends
export const HITS = 100; // hits a rally lasts
export const STEP = 100; // simulated milliseconds a unit of timers moves the clock
const PERIODS = 91; // a timer's period: 1 s to 10 s, in steps of STEP
const CHUNK = 4096; // spawns between runs to idle
const FOOD = 10; // energy a feed of the light carries
const ROOM = 2; // a colony may grow to this many times the cells its light feeds

/**
 * A lab of `actors` actors (for cells: as many as its light feeds) running
 * `scenario` on `datamodel` ('native' or 'quickjs'). `charts` holds the
 * text of cell.scxml, counter.scxml, player.scxml and timer.scxml.
 */
export function createLab(tinyactors, { scenario, actors, datamodel = 'native', charts, resident = 1000, seed = 1 }) {
  const { compileCharts, Host, SimulatedClock, MemoryStore } = tinyactors;
  if (!SCENARIOS.includes(scenario)) throw new TypeError(`scenario is one of ${SCENARIOS.join(', ')}`);
  const random = mulberry32(seed);
  const rally = scenario === 'rally', timers = scenario === 'timers', cells = scenario === 'cells';
  const per = rally ? 2 : 1; // actors a unit of the population holds: a pair of players, or one actor
  const peak = cells ? ROOM * actors : actors - (actors % per); // the most actors alive at once
  const counts = { spawned: 0, ended: 0, messages: 0, points: 0, wakes: 0, starved: 0, aged: 0, refused: 0 };

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
    { name: 'cell.scxml', text: charts.cell },
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
    // due in each step. A cell holds its tick, the same. A chunk of new
    // pairs of players queues a meet each. Actors in the middle of a step
    // hold a workspace each, a sender until its send is handled.
    config: { system: {
      maxActors: peak + 64,
      maxEvents: 4 * BATCH + CHUNK + (timers || cells ? peak + Math.ceil(peak / 16) : 0),
      ...(timers || cells ? { maxTimers: peak + 64 } : {}),
      workspaces: 1024,
      tracing: false,
    } },
    ioProcessors: { 'urn:lab': { receive(send) {
      if (send.event === 'point') counts.points++;
      else if (send.event === 'divide') {
        // The daughter, with half the mother's energy; none past the room.
        if (alive >= peak) { counts.refused++; send.fail(); return; }
        birth(send.data.energy);
      }
    } } },
    // A cell's end: its final state's donedata names why.
    ...(cells ? { onDone: (actor, why) => { death(Number(actor.id), why); } } : {}),
    ...(store ? { store, resident } : {}),
  });
  compiled.close();

  // The population alive: a counter's or a timer's session, or a pair's two
  // players, in slots 0 to alive - 1; an end moves the last into the gap.
  const sessions = new Float64Array(peak / per), partners = rally ? new Float64Array(peak / per) : null;
  const slot = cells ? new Map() : null; // a cell's session -> its slot
  let alive = 0;

  // A cell is born with `energy`, a life of 40 to 80 seconds and its first
  // tick within a second, so the cells tick spread out.
  function birth(energy) {
    const life = 40 + Math.floor(random() * 41), tick = 100 + Math.floor(random() * 900);
    const session = Number(host.spawn('cell', { params: { energy, life, first: `${tick}ms` } }).id);
    sessions[alive] = session;
    slot.set(session, alive++);
    counts.spawned++;
  }
  function death(session, why) {
    const i = slot.get(session);
    if (i === undefined) return;
    slot.delete(session);
    alive--;
    if (i !== alive) { sessions[i] = sessions[alive]; slot.set(sessions[i], i); }
    counts.ended++;
    if (why === 'starved') counts.starved++; else counts.aged++;
  }

  function spawn() {
    if (rally) {
      const a = host.spawn('player');
      const b = host.spawn('player', { params: { peer: a.address.text } });
      host.send(a.id, 'meet', { peer: b.address.text });
      sessions[alive] = Number(a.id); partners[alive] = Number(b.id);
    } else if (timers) {
      // The first delay falls within the period, so the timers fall due spread out.
      const p = 10 + Math.floor(random() * PERIODS), delay = 1 + Math.floor(random() * p);
      due[offset(p) + ((clock.now / STEP + delay) % p)]++;
      sessions[alive] = Number(host.spawn('timer', { params: { period: `${p * STEP}ms`, first: `${delay * STEP}ms` } }).id);
    } else sessions[alive] = Number(host.spawn('counter').id);
    alive++;
    counts.spawned += per;
  }

  /**
   * Spawns the population, a chunk at a time, and runs the host until idle;
   * answers how many actors it spawned, 0 once there are all of them. A
   * colony starts from a fiftieth of the cells its light feeds (at least
   * 16), and grows on its own.
   */
  async function populate() {
    let spawned = 0;
    if (cells) {
      if (counts.spawned > 0) return 0;
      for (let i = 0; i < Math.max(16, Math.ceil(actors / 50)); i++) birth(20);
      spawned = alive;
    } else {
      const change = Math.min(CHUNK, peak / per - alive);
      for (let i = 0; i < change; i++) spawn();
      spawned = change * per;
    }
    if (spawned > 0) await host.idle();
    return spawned;
  }

  /** One unit of the scenario's work, run until the host is idle; answers the messages it handled. */
  async function work() {
    if (scenario === 'idle' || alive === 0) return 0;
    if (cells) {
      // The light: as much food in each step as `actors` cells burn in it,
      // in feeds of FOOD to cells drawn at random, each naming a neighbour.
      clock.increment(STEP);
      const items = [];
      for (let i = 0; i < Math.round((actors * STEP) / 1000 / FOOD); i++) {
        items.push({ target: sessions[Math.floor(random() * alive)], event: 'feed', data: { food: FOOD, neighbour: sessions[Math.floor(random() * alive)] } });
      }
      host.sendMany(items);
      await host.idle();
      return items.length; // the feeds; refresh() counts every event the cells handled
    }
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
      for (let i = 0; i < BATCH / 10; i++) items.push({ target: sessions[Math.floor(random() * alive)], event: 'serve', data: { hits: HITS } });
    } else {
      for (let i = 0; i < BATCH; i++) items.push({ target: sessions[Math.floor(random() * alive)], event: 'add' });
    }
    const points = counts.points;
    host.sendMany(items);
    await host.idle();
    // A rally is a serve and its hits, counted when it finishes.
    const messages = rally ? (counts.points - points) * (HITS + 1) : items.length;
    counts.messages += messages;
    return messages;
  }

  /**
   * For cells, the events they handled, from the core's counters: a cell
   * takes a macrostep at its birth and one for each event it handles.
   */
  async function refresh() {
    if (!cells) return;
    const cell = (await host.hostStats()).charts.find((chart) => chart.name === 'cell');
    counts.messages = cell.macrosteps - cell.spawned;
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
      starved: counts.starved,
      aged: counts.aged,
      refused: counts.refused,
      asleep: store ? store.blobs.size : 0,
      storeBytes: store ? store.bytes : 0,
      quickjsHeap: host.quickjsHeapBytes ?? null,
      simulated: clock.now / 1000, // seconds
    };
  }

  /** The variables of the i-th actor alive (a pair's first player), for main.mjs's checks. */
  const sample = (i) => host.actor(BigInt(sessions[i])).context();

  return { populate, work, refresh, stats, sample, host, close: () => host.close() };
}

function mulberry32(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
