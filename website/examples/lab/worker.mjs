// One lab in a dedicated Worker: its host runs here, the page only charts.
// It fills the host, then runs the scenario's work as fast as it goes, and
// reports once a second: the actors, the rate and the time per spawn or
// message, and the size of tinyactors's WebAssembly memory. The page ends
// a lab by terminating its Worker, whose memory goes with it.
//
// A module Worker has no import map, so it imports the package by the path
// the website serves it at, inside start(): the package loads its
// WebAssembly with a top-level await, and a message that arrives while a
// statically imported module awaits finds no handler and is lost.

const text = (name) => fetch(new URL(`./${name}.scxml`, import.meta.url)).then((response) => response.text());

onmessage = ({ data }) => {
  if (data.start) start(data).catch((error) => postMessage({ error: String(error?.message ?? error) }));
};

// The size of tinyactors's WebAssembly memory. The package keeps its
// instance to itself, so the instance is watched as it is made: this wraps
// the constructor before the package loads, and keeps the first instance's
// memory.
let memory = null;
function watchMemory() {
  const Instance = WebAssembly.Instance;
  function Watched(module, imports) {
    const instance = new Instance(module, imports);
    memory ??= instance.exports.memory ?? null;
    return instance;
  }
  Watched.prototype = Instance.prototype;
  WebAssembly.Instance = Watched;
}
const bytes = () => memory?.buffer.byteLength ?? null;

async function start({ scenario, actors, datamodel }) {
  watchMemory();
  const [tinyactors, { createLab }] = await Promise.all([import('/vendor/tinyactors-wasm/index.mjs'), import('./lab.mjs')]);
  const charts = { counter: await text('counter'), player: await text('player') };
  const base = bytes(); // the module before any host
  const lab = createLab(tinyactors, { scenario, actors, datamodel, charts, seed: Math.floor(Math.random() * 2 ** 31) });
  const empty = bytes(); // and with an empty host, sized for the population

  // A report a second: the rates over that second.
  let window = { wall: performance.now(), spawned: 0, messages: 0, wakes: 0, busy: 0, spawnBusy: 0 };
  let busy = 0, phase = 'fill', spawnBusy = 0;
  const report = (force = false) => {
    const now = performance.now();
    if (!force && now - window.wall < 1000) return;
    const stats = lab.stats();
    const seconds = (now - window.wall) / 1000;
    const spawned = stats.actors - window.spawned, messages = stats.messages - window.messages;
    postMessage({
      phase, stats, base, empty, memory: bytes(),
      spawns: spawned / seconds,
      perSpawn: spawned > 0 ? ((spawnBusy - window.spawnBusy) * 1000) / spawned : null, // microseconds
      messages: messages / seconds,
      perMessage: messages > 0 ? ((busy - window.busy) * 1000) / messages : null,
      wakes: (stats.wakes - window.wakes) / seconds,
    });
    window = { wall: now, spawned: stats.actors, messages: stats.messages, wakes: stats.wakes, busy, spawnBusy };
  };

  // Filling: the time of each chunk is the host's (spawns and their first steps).
  let chunkStart = performance.now();
  await lab.populate(() => {
    spawnBusy += performance.now() - chunkStart;
    report();
    chunkStart = performance.now();
  });
  report(true);
  phase = scenario === 'fill' ? 'holding' : 'working';
  postMessage({ phase, stats: lab.stats(), base, empty, memory: bytes(), filled: true });

  // Working: units of work for up to 50 ms, then a turn of the Worker's
  // event loop, so it stays responsive and its reports go out.
  for (;;) {
    const began = performance.now();
    while (performance.now() - began < 50) {
      const unit = performance.now();
      if ((await lab.work()) === 0) break;
      busy += performance.now() - unit;
    }
    report();
    await new Promise((resolve) => setTimeout(resolve, scenario === 'fill' ? 250 : 0));
  }
}
