#!/usr/bin/env node
/**
 * The JS bindings' throughput and memory, together: the same workloads
 * through the Node addon (N-API) and the web binding (the libraries in wasm,
 * driven by JavaScript over their C API, bindings/web/native.mjs), with each
 * "ecmascript" datamodel (native: JS values and c.expr functions; quickjs:
 * the C datamodel), each in its own process.
 *
 *   node tools/bench_js.mjs                 # every binding and datamodel, a markdown table
 *   node tools/bench_js.mjs --binding web --ecmascript quickjs --json
 *
 * Needs the addon (mise run node:build) and the wasm (mise run web:build).
 * Workloads (a deterministic clock, synchronous flushes):
 *   send, N subscribed   actor.send() round robin over max(1, N) counters, N of
 *                        them subscribed (0, 10, 1000)
 *   sendMany             the same event to 1000 counters per call
 *   spawn                createActor().start()
 *   hibernate            system.hibernate(actor), one by one
 *   wake + event         an event to each hibernated actor (sendMany, a Map store)
 * Each row: events (or actors) per second, best of three, and the process's
 * resident memory after it.
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
const ecmascript = opt('ecmascript', null);
const scale = Number(opt('scale', '1'));

if (!binding || !ecmascript) {
  const self = fileURLToPath(import.meta.url);
  const run = (b, e) => JSON.parse(execFileSync(process.execPath, ['--expose-gc', self, '--binding', b, '--ecmascript', e, '--scale', String(scale), '--json']).toString());
  const columns = [['node', 'native'], ['node', 'quickjs'], ['web', 'native'], ['web', 'quickjs']];
  const results = columns.map(([b, e]) => run(b, e));
  const fmt = (x) => (x >= 1e6 ? `${(x / 1e6).toFixed(2)} M` : x >= 1e3 ? `${(x / 1e3).toFixed(0)} k` : x.toFixed(0));
  console.log(`${process.version} on ${process.platform}-${process.arch}; per second, best of three (resident MB after)\n`);
  console.log(`| workload | ${columns.map(([b, e]) => `${b} ${e}`).join(' | ')} |`);
  console.log(`|---|${columns.map(() => '---:').join('|')}|`);
  for (const k of Object.keys(results[0])) console.log(`| ${k} | ${results.map((r) => `${fmt(r[k].rate)} (${r[k].rss})`).join(' | ')} |`);
  process.exit(0);
}

const require = createRequire(import.meta.url);
const api = binding === 'web' ? await import('../bindings/web/node.mjs') : require('../bindings/node');
const { scxml, fromSCXML, createActor, createSystem } = api;

/* a counter: c.expr for the native datamodel, text for quickjs */
const c = scxml.chart();
const counter =
  ecmascript === 'native'
    ? c.scxml('counter', { initial: 's', datamodel: 'ecmascript' },
        c.datamodel({ n: 0 }),
        c.state('s', c.transition('T', undefined, { cond: c.expr(({ n }) => n >= 0) }, c.assign('n', c.expr(({ n }) => (n + 1) % 1000000)))))
    : fromSCXML(`<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="counter" initial="s">
  <datamodel><data id="n" expr="0"/></datamodel>
  <state id="s"><transition event="T" cond="n &gt;= 0"><assign location="n" expr="(n + 1) % 1000000"/></transition></state>
</scxml>`);

const rss = () => {
  if (globalThis.gc) globalThis.gc();
  return Math.round(process.memoryUsage().rss / 2 ** 20);
};
/* best of `reps` runs of fn(n) -> { rate (ops/s), rss (MB) } */
function measure(fn, n, reps = 3) {
  let best = 0;
  for (let r = 0; r < reps; r++) {
    const t0 = performance.now();
    const ops = fn(n);
    best = Math.max(best, ops / ((performance.now() - t0) / 1000));
  }
  return { rate: best, rss: rss() };
}
const sys = (extra = {}) => createSystem({ ecmascript, clock: () => 0, capacities: { maxActors: 20000 * scale + 16 }, ...extra });

const out = {};
for (const subscribed of [0, 10, 1000]) {
  out[`send, ${subscribed} subscribed`] = measure((n) => {
    const system = sys();
    const actors = Array.from({ length: Math.max(1, subscribed) }, () => createActor(counter, { system }).start());
    let heard = 0;
    for (let i = 0; i < subscribed; i++) actors[i].subscribe(() => heard++);
    for (let i = 0; i < n; i++) actors[i % actors.length].send({ type: 'T' });
    system.stop();
    if (heard !== (subscribed ? n : 0)) throw new Error(`heard ${heard}`);
    return n;
  }, 50000 * scale);
}
out.sendMany = measure((n) => {
  const system = sys();
  const actors = Array.from({ length: 1000 }, () => createActor(counter, { system }).start());
  const batch = actors.map((a) => [a, { type: 'T' }]);
  for (let i = 0; i < n / 1000; i++) system.sendMany(batch);
  system.flush();
  system.stop();
  return n;
}, 200000 * scale);
out.spawn = measure((n) => {
  const system = sys();
  for (let i = 0; i < n; i++) createActor(counter, { system }).start();
  system.flush();
  system.stop();
  return n;
}, 10000 * scale);
{
  const n = 10000 * scale;
  let hibernate = 0, wake = 0;
  for (let r = 0; r < 3; r++) {
    const system = sys({ hibernation: { store: new Map() } });
    const actors = Array.from({ length: n }, () => createActor(counter, { system }).start());
    system.flush();
    let t0 = performance.now();
    for (const a of actors) system.hibernate(a);
    hibernate = Math.max(hibernate, n / ((performance.now() - t0) / 1000));
    t0 = performance.now();
    system.sendMany(actors.map((a) => [a, { type: 'T' }]));
    system.flush();
    wake = Math.max(wake, n / ((performance.now() - t0) / 1000));
    if (actors.some((a) => a.hibernated)) throw new Error('not all woke');
    system.stop();
  }
  out.hibernate = { rate: hibernate, rss: rss() };
  out['wake + event'] = { rate: wake, rss: rss() };
}

if (args.includes('--json')) process.stdout.write(JSON.stringify(out));
else console.log(binding, ecmascript, out);
