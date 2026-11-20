// The forage loop of bench.mjs, in a page or a Worker: ?n=100000&seconds=10.
// Reports through window.result / postMessage. Memory: the wasm module's
// linear memory and, in Chromium, performance.memory's JS heap.
import { wasmBytes } from './wasm-memory.mjs';
const { compileCharts, Host, SimulatedClock } = await import('./node_modules/@tinyactors/wasm/index.mjs');
import { antChart } from './ant.mjs';

export async function bench({ n: N, seconds: SECONDS = 10, leg: LEG = 5000, frame: FRAME = 50, chunk: CHUNK = 4096, datamodel = 'native' }) {
  const compiled = compileCharts([{ name: 'ant.scxml', text: antChart }]);
  const clock = new SimulatedClock();
  const CAP = N + 16;
  const x0 = new Float32Array(CAP), y0 = new Float32Array(CAP), x1 = new Float32Array(CAP), y1 = new Float32Array(CAP);
  const t0 = new Float32Array(CAP), t1 = new Float32Array(CAP);
  const RING = Math.ceil((LEG * 3) / FRAME) + 4;
  const ring = Array.from({ length: RING }, () => []);
  let rand = 12345;
  const rnd = () => ((rand = (rand * 1103515245 + 12345) >>> 0) / 4294967296);
  const schedule = (session, slot, ms, ev) => {
    const now = clock.now;
    x0[slot] = x1[slot]; y0[slot] = y1[slot]; x1[slot] = rnd() * 1000; y1[slot] = rnd() * 1000;
    t0[slot] = now; t1[slot] = now + ms;
    ring[Math.floor((now + ms) / FRAME) % RING].push(session, ev);
  };
  let receives = 0;
  const world = { receive(send) {
    receives++;
    const session = send.session, slot = Number(session), leg = LEG * (0.5 + rnd());
    if (send.event === 'forage') schedule(session, slot, leg, rnd() < 0.3 ? 1 : 0);
    else if (send.event === 'home') schedule(session, slot, leg, 0);
    else if (send.event === 'rest') schedule(session, slot, leg / 2, 0);
  } };
  const host = new Host(compiled, { system: 'anthill', clock, datamodel, ioProcessors: { 'urn:world': world },
    config: { system: { maxActors: N + 64, maxEvents: 65536, tracing: false } } });
  compiled.close();
  const heap = () => (globalThis.performance?.memory ? performance.memory.usedJSHeapSize : NaN);
  const h0 = heap(), w0 = wasmBytes();
  let t = performance.now();
  for (let i = 0; i < N; i += CHUNK) {
    for (let j = i; j < Math.min(N, i + CHUNK); j++) host.spawn('ant', { params: { colony: j & 1 } });
    await host.idle();
  }
  const populateMs = performance.now() - t;
  const out = { where: typeof window === 'undefined' ? 'worker' : 'page', n: N, datamodel,
    populate_us_per_actor: +(populateMs * 1000 / N).toFixed(2),
    wasm_mb: Math.round(wasmBytes() / 1048576), wasm_b_per_actor: Math.round((wasmBytes() - w0) / N),
    js_heap_mb: Math.round(heap() / 1048576), js_heap_b_per_actor: Math.round((heap() - h0) / N) };
  const NAMES = ['arrive', 'found'];
  let events = 0; const frames = Math.round(SECONDS * 1000 / FRAME);
  const wall = performance.now();
  let worstFrameMs = 0;
  for (let f = 0; f < frames; f++) {
    const a = performance.now();
    clock.increment(FRAME);
    const b = Math.floor(clock.now / FRAME) % RING;
    const due = ring[b]; ring[b] = [];
    const items = new Array(due.length / 2);
    for (let k = 0; k < due.length; k += 2) items[k / 2] = { target: due[k], event: NAMES[due[k + 1]] };
    host.sendMany(items); events += items.length;
    await host.idle();
    worstFrameMs = Math.max(worstFrameMs, performance.now() - a);
  }
  const wallMs = performance.now() - wall;
  Object.assign(out, { sim_speed: +((SECONDS * 1000) / wallMs).toFixed(3), events_per_wall_s: Math.round(events / (wallMs / 1000)),
    us_per_decision: +((wallMs * 1000) / events).toFixed(2), decisions_per_frame: Math.round(events / frames),
    worst_step_ms: Math.round(worstFrameMs), wasm_after_mb: Math.round(wasmBytes() / 1048576) });
  host.close();
  return out;
}

const params = Object.fromEntries(new URLSearchParams(globalThis.location?.search ?? '').entries());
if (typeof window === 'undefined' && typeof WorkerGlobalScope !== 'undefined') {
  self.onmessage = async (e) => { try { self.postMessage(await bench(e.data)); } catch (error) { self.postMessage({ error: String(error?.stack ?? error) }); } };
  self.postMessage('ready');
}
