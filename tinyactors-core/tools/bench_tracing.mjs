#!/usr/bin/env node
/**
 * What tracing costs the JS bindings (docs/adr/0006 in the repository root),
 * on the workload of docs/research/trace-handoff-formats.md: 100,000 actors,
 * a `ping` with an assignment each (BEGIN, MICROSTEP, ASSIGN, END at full
 * detail), sent with sendMany and run to idle. Each variant runs in its own
 * process; the best of three rounds counts.
 *
 *   node tools/bench_tracing.mjs                 # both bindings, a markdown table
 *   node tools/bench_tracing.mjs --binding node --variant full --json
 *
 * Variants: untraced (no sink: core writes no records, contexts still flow),
 * summary and full (a sink that takes the OTLP bytes and keeps their length),
 * full+decode (and reads them back with decodeTraces).
 * Needs the addon built (mise run node:build) and the wasm (mise run web:build).
 */
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf(`--${name}`);
  return i < 0 ? def : args[i + 1];
};
const binding = opt('binding', null);
const variant = opt('variant', null);
const actors = Number(opt('actors', '100000'));
const VARIANTS = ['untraced', 'summary', 'full', 'full+decode'];

if (!binding || !variant) {
  const self = fileURLToPath(import.meta.url);
  const run = (b, v) => JSON.parse(execFileSync(process.execPath, ['--expose-gc', self, '--binding', b, '--variant', v, '--actors', String(actors), '--json']).toString());
  const fmt = (x) => (x >= 1e6 ? `${(x / 1e6).toFixed(2)}M` : `${(x / 1e3).toFixed(0)}k`);
  console.log(`${actors} actors, one ping with an assignment each; ${process.version} on ${process.platform}-${process.arch}\n`);
  console.log('| Variant | node events/s | vs untraced | web events/s | vs untraced | OTLP bytes/event | heap+external MB after |');
  console.log('|---|---:|---:|---:|---:|---:|---:|');
  const rows = {};
  for (const b of binding ? [binding] : ['node', 'web']) for (const v of VARIANTS) (rows[v] ||= {})[b] = run(b, v);
  for (const v of VARIANTS) {
    const n = rows[v].node, w = rows[v].web;
    const base = (b) => rows.untraced[b].rate;
    console.log(`| ${v} | ${n ? fmt(n.rate) : '-'} | ${n ? `${Math.round((100 * n.rate) / base('node'))}%` : '-'} | ${w ? fmt(w.rate) : '-'} | ${w ? `${Math.round((100 * w.rate) / base('web'))}%` : '-'} | ${(n || w).bytesPerEvent.toFixed(0)} | ${((n || w).memory / 1048576).toFixed(0)} |`);
  }
  process.exit(0);
}

const require = createRequire(import.meta.url);
const api = binding === 'web' ? await import('../bindings/web/node.mjs') : require('../bindings/node');
const { createMachine, createActor, createSystem, assign, decodeTraces } = api;

const machine = createMachine({
  id: 'counter',
  initial: 'idle',
  context: { n: 0 },
  states: { idle: { on: { ping: { actions: assign({ n: ({ context }) => context.n + 1 }) } } } },
});

let bytes = 0;
let decoded = 0;
const sink = (traces, logs) => {
  bytes += traces.length + (logs ? logs.length : 0);
  if (variant === 'full+decode') decoded += decodeTraces(traces).length;
};
const system = createSystem({ clock: () => 0, telemetry: false });
if (variant !== 'untraced') system.setTraceSink(sink, { detail: variant === 'summary' ? 'summary' : 'full' });
const all = Array.from({ length: actors }, () => createActor(machine, { system }).start());
system.flush(1e9);
const batch = all.map((a) => [a, { type: 'ping' }]);
let best = 0;
bytes = 0;
for (let round = 0; round < 3; round++) {
  const before = bytes;
  const t0 = performance.now();
  for (let i = 0; i < batch.length; i += 1000) system.sendMany(batch.slice(i, i + 1000));
  system.flush(1e9);
  const dt = (performance.now() - t0) / 1000;
  best = Math.max(best, actors / dt);
  if (round === 2) bytes -= before;
}
if (globalThis.gc) globalThis.gc();
const usage = process.memoryUsage();
const out = { rate: best, bytesPerEvent: bytes / actors, memory: usage.heapUsed + usage.external + usage.arrayBuffers, rss: usage.rss, decoded };
system.stop();
console.log(JSON.stringify(out));
