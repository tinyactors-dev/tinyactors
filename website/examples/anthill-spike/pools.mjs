// Reports the host's pools and the wasm memory for n idle ants under a config.
import { wasmBytes } from './wasm-memory.mjs';
const { compileCharts, Host, SimulatedClock } = await import('@tinyactors/wasm');
import { antChart } from './ant.mjs';
const n = Number(process.argv[2]); const system = JSON.parse(process.argv[3] ?? '{}');
const compiled = compileCharts([{ name: 'ant.scxml', text: antChart }]);
const w0 = wasmBytes();
const host = new Host(compiled, { system: 'p', clock: new SimulatedClock(), ioProcessors: { 'urn:world': { receive() {} } }, config: { system: { tracing: false, ...system } } });
console.log('after create wasm MB', ((wasmBytes() - w0) / 1048576).toFixed(0));
for (let i = 0; i < n; i++) host.spawn('ant', { params: { stage: process.argv[4] ?? 'idle' } });
await host.idle();
const s = await host.hostStats();
const p = s.pools[0];
for (const k of Object.keys(p)) if (p[k]?.capacity !== undefined) console.log(k, p[k].capacity, 'used', p[k].used, 'hw', p[k].highWater);
console.log('wasm MB', (wasmBytes() / 1048576).toFixed(0), 'per actor B', Math.round((wasmBytes() - w0) / n));
host.close();
