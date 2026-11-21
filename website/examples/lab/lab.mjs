// The measurement lab: one host, one scenario, as many actors as the
// reader asks for. The page runs it in a Worker (worker.mjs) and charts
// what it reports; main.mjs runs every scenario small, seeded, in Node.js,
// for the website's tests. The lab takes the tinyactors package as an
// argument, so the same code runs in both.
//
// Every scenario first fills the host, a chunk of spawns at a time. Then:
//
// - fill: nothing more. The host holds its actors, waiting.
// - events: the page sends batches of `add` to counters drawn at random,
//   and each counter adds one in its datamodel.
// - rally: pairs of players send a ball back and forth, a hundred hits a
//   rally, with no JavaScript between hits.
// - sleep: the host keeps at most `resident` actors in memory and the rest
//   in a store; each event goes to a counter drawn at random, which is
//   almost always asleep, so the host wakes it from the store first.

export const SCENARIOS = ['fill', 'events', 'rally', 'sleep'];
export const BATCH = 1000; // events a unit of work sends
export const HITS = 100; // hits a rally lasts
const CHUNK = 4096; // spawns between runs to idle

/**
 * A lab of `actors` actors running `scenario` on `datamodel` ('native' or
 * 'quickjs'). `charts` holds the text of counter.scxml and player.scxml.
 */
export function createLab(tinyactors, { scenario, actors, datamodel = 'native', charts, resident = 1000, seed = 1 }) {
  const { compileCharts, Host, SimulatedClock, MemoryStore } = tinyactors;
  if (!SCENARIOS.includes(scenario)) throw new TypeError(`scenario is one of ${SCENARIOS.join(', ')}`);
  const random = mulberry32(seed);
  const rally = scenario === 'rally';
  const total = rally ? actors - (actors % 2) : actors;
  const counts = { spawned: 0, messages: 0, points: 0, wakes: 0, saves: 0 };

  // The store of the sleep scenario counts what it holds, and its wakes.
  let store;
  if (scenario === 'sleep') {
    store = new MemoryStore();
    store.bytes = 0;
    const { save, load, delete: remove } = store;
    store.save = function (session, blob) { this.bytes += blob.byteLength - (this.blobs.get(session)?.byteLength ?? 0); counts.saves++; save.call(this, session, blob); };
    store.load = function (session) { counts.wakes++; return load.call(this, session); };
    store.delete = function (session) { this.bytes -= this.blobs.get(session)?.byteLength ?? 0; remove.call(this, session); };
  }

  const compiled = compileCharts([
    { name: 'counter.scxml', text: charts.counter },
    { name: 'player.scxml', text: charts.player },
  ]);
  const host = new Host(compiled, {
    system: 'lab',
    clock: new SimulatedClock(),
    datamodel,
    // quickjs keeps every actor's variables in its own heap, 1 GiB unless
    // a host sets more; WebAssembly addresses 4 GiB in all.
    ...(datamodel === 'quickjs' ? { quickjs: { memoryLimit: 3 * 2 ** 30 } } : {}),
    // Sized for the population up front: a host that grows copies its memory.
    config: { system: { maxActors: total + 64, maxEvents: 4 * BATCH, tracing: false } },
    ioProcessors: { 'urn:lab': { receive() { counts.points++; } } },
    ...(store ? { store, resident } : {}),
  });
  compiled.close();
  const sessions = new Float64Array(total); // a counter's session, or a pair's first player's

  /** Spawns the population, a chunk at a time; progress(spawned) after each. */
  async function populate(progress = () => {}) {
    while (counts.spawned < total) {
      const end = Math.min(total, counts.spawned + CHUNK);
      const meets = [];
      for (let i = counts.spawned; i < end; i += rally ? 2 : 1) {
        if (rally) {
          const first = host.spawn('player');
          const second = host.spawn('player', { params: { peer: first.address.text } });
          meets.push({ target: first.id, event: 'meet', data: { peer: second.address.text } });
          sessions[i] = Number(first.id);
        } else sessions[i] = Number(host.spawn('counter').id);
      }
      if (meets.length) host.sendMany(meets);
      counts.spawned = end;
      await host.idle();
      progress(counts.spawned);
    }
  }

  /** One unit of the scenario's work, run until the host is idle; answers the messages it handled. */
  async function work() {
    if (scenario === 'fill') return 0;
    const items = [];
    let messages;
    if (rally) {
      const pairs = total / 2;
      for (let i = 0; i < Math.min(pairs, BATCH / 10); i++) {
        items.push({ target: sessions[2 * Math.floor(random() * pairs)], event: 'serve', data: { hits: HITS } });
      }
      messages = items.length * (HITS + 1); // the serve and every hit
    } else {
      for (let i = 0; i < BATCH; i++) items.push({ target: sessions[Math.floor(random() * total)], event: 'add' });
      messages = items.length;
    }
    host.sendMany(items);
    await host.idle();
    counts.messages += messages;
    return messages;
  }

  /** What the lab has done so far. */
  function stats() {
    return {
      scenario, datamodel, target: total,
      actors: counts.spawned,
      messages: counts.messages,
      points: counts.points,
      wakes: counts.wakes,
      saves: counts.saves,
      stored: store ? store.blobs.size : 0,
      storeBytes: store ? store.bytes : 0,
      quickjsHeap: host.quickjsHeapBytes ?? null,
    };
  }

  /** The variables of a counter or player drawn at random (for main.mjs's checks). */
  const sample = (i) => host.actor(BigInt(sessions[i])).context();

  return { populate, work, stats, sample, host, close: () => host.close() };
}

function mulberry32(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
