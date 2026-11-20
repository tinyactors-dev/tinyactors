// node bench.mjs --n 100000 --datamodel native --mode forage --seconds 20 --leg 5000
// Spawns n ants, reports spawn time and memory, then (mode forage) runs the
// world loop for `seconds` of simulated time in frames of `frame` ms and
// reports events per second of wall time and the sim speed.
import { wasmBytes } from './wasm-memory.mjs';
const { compileCharts, Host, SimulatedClock } = await import('@tinyactors/wasm');
import { antChart } from './ant.mjs';

const arg = (name, fallback) => {
  const i = process.argv.indexOf(`--${name}`);
  return i < 0 ? fallback : process.argv[i + 1];
};
const N = Number(arg('n', 10000));
const datamodel = arg('datamodel', 'native');
const mode = arg('mode', 'forage');         // idle | egg | forage
const SECONDS = Number(arg('seconds', 10));
const LEG = Number(arg('leg', 5000));       // mean leg, simulated ms
const FRAME = Number(arg('frame', 50));     // simulated ms per step
const tracing = arg('tracing', 'false') === 'true';
const ids = arg('ids', 'session');          // session | data

const mb = (b) => (b / 1048576).toFixed(0);
const mem = () => {
  global.gc?.(); global.gc?.();
  const m = process.memoryUsage();
  return { rss: m.rss, heap: m.heapUsed, wasm: wasmBytes() };
};
const base = mem();

const compiled = compileCharts([{ name: 'ant.scxml', text: antChart }]);
const clock = new SimulatedClock();

// The world: one slot per ant, its current leg (from, to, start, end).
const CAP = N + 16;
const x0 = new Float32Array(CAP), y0 = new Float32Array(CAP), x1 = new Float32Array(CAP), y1 = new Float32Array(CAP);
const t0 = new Float32Array(CAP), t1 = new Float32Array(CAP), kind = new Uint8Array(CAP);
const RING = Math.ceil((LEG * 3) / FRAME) + 4;
const ring = Array.from({ length: RING }, () => []);
let rand = 12345;
const rnd = () => ((rand = (rand * 1103515245 + 12345) >>> 0) / 4294967296);
let receives = 0, receiveMs = 0;
const schedule = (session, slot, ms, ev) => {
  const now = clock.now;
  x0[slot] = x1[slot]; y0[slot] = y1[slot];
  x1[slot] = rnd() * 1000; y1[slot] = rnd() * 1000;
  t0[slot] = now; t1[slot] = now + ms; kind[slot] = ev;
  ring[Math.floor((now + ms) / FRAME) % RING].push(session, ev);
};
const world = {
  receive(send) {
    const a = performance.now();
    receives++;
    const session = send.session;
    const slot = ids === 'data' ? send.data.i : Number(session);
    const leg = LEG * (0.5 + rnd());
    switch (send.event) {
      case 'forage': schedule(session, slot, leg, rnd() < 0.3 ? 1 : 0); break;
      case 'home': schedule(session, slot, leg, 0); break;
      case 'rest': schedule(session, slot, leg / 2, 0); break;
      default: break;
    }
    receiveMs += performance.now() - a;
  },
};
const host = new Host(compiled, {
  system: 'anthill', clock, datamodel,
  ioProcessors: { 'urn:world': world },
  config: { system: { maxActors: N + 64, maxEvents: Math.max(256, N + 64), maxTimers: mode === 'egg' ? N + 64 : 64, tracing } },
  ...(datamodel === 'quickjs' ? { quickjs: { memoryLimit: 3 * 1024 ** 3 } } : {}),
});
compiled.close();

const stage = mode === 'forage' ? 'worker' : mode;
let t = performance.now();
for (let i = 0; i < N; i++) host.spawn('ant', { params: { colony: i & 1, stage } });
const spawnMs = performance.now() - t;
t = performance.now();
await host.idle();
const startMs = performance.now() - t;
const m1 = mem();
const out = {
  n: N, datamodel, mode, tracing,
  spawn_us_per_actor: +(spawnMs * 1000 / N).toFixed(2),
  first_step_us_per_actor: +(startMs * 1000 / N).toFixed(2),
  rss_mb: +mb(m1.rss), rss_b_per_actor: Math.round((m1.rss - base.rss) / N),
  js_heap_b_per_actor: Math.round((m1.heap - base.heap) / N),
  wasm_mb: +mb(m1.wasm), wasm_b_per_actor: Math.round((m1.wasm - base.wasm) / N),
};
if (mode === 'forage') {
  const NAMES = ['arrive', 'found'];
  let events = 0;
  receives = 0; receiveMs = 0;
  let sendManyMs = 0, idleMs = 0;
  const wall = performance.now();
  const frames = Math.round((SECONDS * 1000) / FRAME);
  for (let f = 0; f < frames; f++) {
    clock.increment(FRAME);
    const b = Math.floor(clock.now / FRAME) % RING;
    const due = ring[b]; ring[b] = [];
    const items = new Array(due.length / 2);
    for (let k = 0; k < due.length; k += 2) items[k / 2] = { target: due[k], event: NAMES[due[k + 1]] };
    const a = performance.now();
    const taken = host.sendMany(items);
    if (taken !== items.length) throw new Error(`sendMany took ${taken} of ${items.length}`);
    sendManyMs += performance.now() - a;
    events += items.length;
    const c = performance.now();
    await host.idle();
    idleMs += performance.now() - c;
  }
  const wallMs = performance.now() - wall;
  Object.assign(out, {
    sim_s: SECONDS, wall_s: +(wallMs / 1000).toFixed(2), sim_speed: +((SECONDS * 1000) / wallMs).toFixed(3),
    events, events_per_wall_s: Math.round(events / (wallMs / 1000)),
    us_per_decision: +((wallMs * 1000) / events).toFixed(2),
    receive_us: +((receiveMs * 1000) / receives).toFixed(2),
    sendMany_us_per_event: +((sendManyMs * 1000) / events).toFixed(2),
    idle_us_per_event: +((idleMs * 1000) / events).toFixed(2),
    rss_after_mb: +mb(mem().rss), wasm_after_mb: +mb(wasmBytes()),
  });
}
host.close();
console.log(JSON.stringify(out));
