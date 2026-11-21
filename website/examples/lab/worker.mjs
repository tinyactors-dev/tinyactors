// One lab in a dedicated Worker: its host runs here, the page only charts.
// It spawns the population (a colony's first cells), then runs the
// scenario's work as fast as it goes, and once a second it reports: the
// actors, births and deaths, messages and the time per message, the
// simulated time, and the size of tinyactors's WebAssembly memory. The page ends a lab by
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

async function start({ scenario, actors, datamodel }) {
  watchMemory();
  const [tinyactors, { createLab }] = await Promise.all([import('/vendor/tinyactors-wasm/index.mjs'), import('./lab.mjs')]);
  const charts = Object.fromEntries(await Promise.all(['cell', 'counter', 'player', 'timer'].map(async (name) => [name, await text(name)])));
  const base = bytes(); // the module before any host
  const lab = createLab(tinyactors, { scenario, actors, datamodel, charts, seed: Math.floor(Math.random() * 2 ** 31) });
  postMessage({ ready: true, base });

  const began = performance.now();
  let busy = 0; // milliseconds of work: the host's and the lab's, the spawning left out
  let filling = true;
  let window = { wall: began, spawned: 0, ended: 0, messages: 0, wakes: 0, busy: 0 };
  let second = 0;
  for (;;) {
    const turn = performance.now();
    while (performance.now() - turn < 50) {
      if (filling && (await lab.populate()) !== 0) continue;
      filling = false;
      if (scenario === 'idle') break;
      const unit = performance.now();
      await lab.work();
      busy += performance.now() - unit;
    }
    const now = performance.now();
    if (now - window.wall >= 1000) {
      await lab.refresh();
      const stats = lab.stats();
      const elapsed = (now - window.wall) / 1000;
      const messages = stats.messages - window.messages;
      second++;
      postMessage({
        second, stats, base, memory: bytes(), wall: (now - began) / 1000,
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
