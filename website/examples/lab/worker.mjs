// One lab in a dedicated Worker: its host runs here, the page only charts.
// Each turn it brings the population to what its pattern asks for at this
// second of the run (lab.mjs, adjust), then runs the scenario's work as
// fast as it goes, and once a second it reports: the actors and their
// target, spawns and ends, messages and the time per message, and the
// size of tinyactors's WebAssembly memory. The page ends a lab by
// terminating its Worker, whose memory goes with it.
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

async function start({ scenario, actors, datamodel, pattern }) {
  watchMemory();
  const [tinyactors, { createLab, target }] = await Promise.all([import('/vendor/tinyactors-wasm/index.mjs'), import('./lab.mjs')]);
  const charts = { counter: await text('counter'), player: await text('player'), timer: await text('timer') };
  const base = bytes(); // the module before any host
  const lab = createLab(tinyactors, { scenario, actors, datamodel, charts, seed: Math.floor(Math.random() * 2 ** 31) });
  postMessage({ ready: true, base });

  const began = performance.now();
  const seconds = () => (performance.now() - began) / 1000;
  let busy = 0; // milliseconds of work: the host's and the lab's, spawns and ends left out
  let window = { wall: began, spawned: 0, ended: 0, messages: 0, wakes: 0, busy: 0 };
  let second = 0;
  for (;;) {
    const turn = performance.now();
    while (performance.now() - turn < 50) {
      // The population first: as many chunks as it takes, the work between them.
      if ((await lab.adjust(target(pattern, lab.stats().peak, seconds()))) !== 0) continue;
      if (scenario === 'idle') break;
      const unit = performance.now();
      await lab.work();
      busy += performance.now() - unit;
    }
    const now = performance.now();
    if (now - window.wall >= 1000) {
      const stats = lab.stats();
      const elapsed = (now - window.wall) / 1000;
      const messages = stats.messages - window.messages;
      second++;
      postMessage({
        second, stats, base, memory: bytes(),
        target: target(pattern, stats.peak, seconds()),
        spawns: (stats.spawned - window.spawned) / elapsed,
        ends: (stats.ended - window.ended) / elapsed,
        messages: messages / elapsed,
        perMessage: messages > 0 ? ((busy - window.busy) * 1000) / messages : null, // microseconds
        wakes: (stats.wakes - window.wakes) / elapsed,
      });
      window = { wall: now, spawned: stats.spawned, ended: stats.ended, messages: stats.messages, wakes: stats.wakes, busy };
    }
    await new Promise((resolve) => setTimeout(resolve, scenario === 'idle' ? 100 : 0));
  }
}
