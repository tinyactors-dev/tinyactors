// One colony in a dedicated Worker: its host and its part of the world run
// here, the page only draws. The page starts two, and joins them with a
// MessageChannel: each frame a colony waits for the other's last frame,
// steps, and sends its own (world.mjs, step()), so the two keep in step on
// two cores.
//
// A module Worker has no import map, so it imports the package by the path
// the website serves it at, and it does so inside start(): the package
// loads its WebAssembly with a top-level await, and a message that arrives
// while a statically imported module awaits finds no handler and is lost.
// So the handlers are installed first, and imports wait.
import { FRAME } from './world.mjs';

const text = (name) => fetch(new URL(`./${name}.scxml`, import.meta.url)).then((response) => response.text());
let colony;
const inbox = [];
const fromOther = [];
let wake = null; // resolves when the other colony's next frame arrives

onmessage = ({ data }) => {
  if (data.start) {
    data.port.onmessage = ({ data: frame }) => { fromOther.push(frame); wake?.(); wake = null; };
    start(data).catch((error) => postMessage({ error: String(error?.stack ?? error) }));
  } else inbox.push(data);
};
const nextFrame = async () => {
  while (fromOther.length === 0) await new Promise((resolve) => { wake = resolve; });
  return fromOther.shift();
};

// The size of tinyactors's WebAssembly memory, for the page's chart. The
// package keeps its instance to itself, so the instance is watched as it is
// made: this wraps the constructor before the package loads, and keeps the
// first instance's memory.
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

async function start({ population, colony: index, seed, offset, rest, port }) {
  watchMemory();
  const [tinyactors, { createColony }] = await Promise.all([import('/vendor/tinyactors-wasm/index.mjs'), import('./world.mjs')]);
  const charts = { ant: await text('ant'), queen: await text('queen') };
  colony = await createColony(tinyactors, { charts, population, colony: index, seed, offset, rest });
  const base = memory?.buffer.byteLength ?? null; // the module and an empty host, before any ant
  await colony.populate((done) => postMessage({ progress: done }));
  postMessage({ ready: true, base });
  run(port);
}

// The simulation keeps to the wall clock, never faster. When a frame's
// steps take longer than it lasts, the simulation falls behind and runs
// slower than real time, and the page says so: no ant's decision is dropped.
async function run(port) {
  let wallStart = performance.now(), simulated = 0;
  let window = { wall: performance.now(), simulated: 0, decisions: 0, messages: 0, busy: 0 };
  let speed = 1, rate = 0, messages = 0, busy = 0, perMessage = null;
  let other = null; // the other colony's last frame (none before the first)
  for (;;) {
    for (const message of inbox.splice(0)) {
      if (message.food) colony.placeFood(message.x, message.y);
      if (message.squash) colony.squash(message.x, message.y, message.radius);
      if (message.rest) colony.setRest(message.rest);
      if ('follow' in message) colony.follow(message.follow);
      if (message.pick) postMessage({ picked: message.any ? colony.pickAny() : colony.pick(message.x, message.y, message.radius), request: message.request });
    }
    const began = performance.now();
    while (simulated < began - wallStart && performance.now() - began < 40) {
      if (simulated > 0) other = await nextFrame();
      const stepped = performance.now();
      const frame = await colony.step(other);
      busy += performance.now() - stepped; // the host's and the world's work, the wait for the other colony left out
      port.postMessage(frame, [frame.arrivals.buffer, frame.struck.buffer, frame.eaten.buffer]);
      simulated += FRAME;
    }
    const behind = began - wallStart - simulated;
    if (behind > 250) wallStart += behind - 250; // owe at most a quarter second
    const now = performance.now();
    if (now - window.wall >= 1000) {
      const stats = colony.stats();
      speed = (simulated - window.simulated) / (now - window.wall);
      rate = ((stats.decisions - window.decisions) * 1000) / (now - window.wall);
      messages = ((stats.messages - window.messages) * 1000) / (now - window.wall);
      const handled = stats.messages - window.messages;
      perMessage = handled > 0 ? ((busy - window.busy) * 1000) / handled : null; // microseconds
      window = { wall: now, simulated, decisions: stats.decisions, messages: stats.messages, busy };
    }
    const followed = await colony.inspect(); // the followed ant, read from its host; null when none
    const delta = colony.delta();
    postMessage({ delta, stats: colony.stats(), speed, rate, messages, perMessage, wall: now, memory: memory?.buffer.byteLength ?? null, followed }, [delta.buffer]);
    await new Promise((resolve) => setTimeout(resolve, simulated < performance.now() - wallStart ? 0 : 16));
  }
}
