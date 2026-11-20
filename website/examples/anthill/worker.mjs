// The anthill in a dedicated Worker: the host and the world run here, and
// the page only draws. A module Worker has no import map, so it imports the
// package by the path the website serves it at, and it does so inside
// start(): the package loads its WebAssembly with a top-level await, and a
// message that arrives while a statically imported module awaits finds no
// handler and is lost. So the handler is installed first, and imports wait.
import { FRAME } from './world.mjs';

const text = (name) => fetch(new URL(`./${name}.scxml`, import.meta.url)).then((response) => response.text());
let anthill;
const inbox = [];

onmessage = ({ data }) => {
  if (data.start) start(data.population, data.seed).catch((error) => postMessage({ error: String(error?.stack ?? error) }));
  else inbox.push(data);
};

async function start(population, seed) {
  const [tinyactors, { createAnthill }] = await Promise.all([import('/vendor/tinyactors-wasm/index.mjs'), import('./world.mjs')]);
  const charts = { ant: await text('ant'), queen: await text('queen') };
  anthill = await createAnthill(tinyactors, { charts, population, seed });
  await anthill.populate((done) => postMessage({ progress: done }));
  postMessage({ ready: true, capacity: anthill.capacity });
  run();
}

// The simulation keeps to the wall clock, never faster. When a frame's
// steps take longer than it lasts, the simulation falls behind and runs
// slower than real time, and the page says so: no ant's decision is dropped.
async function run() {
  let wallStart = performance.now(), simulated = 0;
  let window = { wall: performance.now(), simulated: 0, decisions: 0 };
  let speed = 1, rate = 0;
  for (;;) {
    for (const message of inbox.splice(0)) {
      if (message.food) anthill.addFood(message.x, message.y);
      if (message.squash) anthill.squash(message.x, message.y, message.radius);
    }
    const began = performance.now();
    while (simulated < began - wallStart && performance.now() - began < 40) {
      await anthill.step();
      simulated += FRAME;
    }
    const behind = began - wallStart - simulated;
    if (behind > 250) wallStart += behind - 250; // owe at most a quarter second
    const now = performance.now();
    if (now - window.wall >= 1000) {
      const stats = anthill.stats();
      speed = (simulated - window.simulated) / (now - window.wall);
      rate = ((stats.decisions - window.decisions) * 1000) / (now - window.wall);
      window = { wall: now, simulated, decisions: stats.decisions };
    }
    const delta = anthill.delta();
    postMessage({ delta, stats: anthill.stats(), speed, rate, wall: now }, [delta.buffer]);
    await new Promise((resolve) => setTimeout(resolve, simulated < performance.now() - wallStart ? 0 : 16));
  }
}
