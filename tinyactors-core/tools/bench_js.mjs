#!/usr/bin/env node
/**
 * The JS bindings' call cost: the same workloads through the Node addon
 * (N-API) and the web binding (wasm behind the synchronous N-API-subset
 * adapter, bindings/web/native.mjs), each in its own process.
 *
 *   node tools/bench_js.mjs            # both bindings, a markdown table
 *   node tools/bench_js.mjs --binding web --json
 *
 * Needs the addon built (mise run node:test) and the wasm (mise run web:build).
 * Workloads (deterministic clock, synchronous flushes):
 *   machine send      one actor.send() per event on a two-state machine
 *   machine sendMany  the same events in batches of 1000 (one native call each)
 *   scxml send        actor.send() on an SCXML document (cond + assign + log-free script
 *                     in the ECMAScript datamodel: JS called back from C per expression)
 *   spawn             createActor().start() of the machine
 *   hibernate         system.hibernate(actor), one by one
 *   wake              an event to each hibernated actor (sendMany, store in memory)
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
const scale = Number(opt('scale', '1'));

if (!binding) {
  const self = fileURLToPath(import.meta.url);
  const run = (b) => JSON.parse(execFileSync(process.execPath, [self, '--binding', b, '--scale', String(scale), '--json']).toString());
  const node = run('node'), web = run('web');
  const rows = Object.keys(node);
  const fmt = (x) => (x >= 1e6 ? `${(x / 1e6).toFixed(2)} M` : x >= 1e3 ? `${(x / 1e3).toFixed(0)} k` : x.toFixed(0));
  console.log('| workload | node (ops/s) | web (ops/s) | web / node |');
  console.log('|---|---:|---:|---:|');
  for (const k of rows) console.log(`| ${k} | ${fmt(node[k])} | ${fmt(web[k])} | ${(web[k] / node[k]).toFixed(2)} |`);
  process.exit(0);
}

const require = createRequire(import.meta.url);
const api = binding === 'web' ? await import('../bindings/web/node.mjs') : require('../bindings/node');
const { createMachine, createActor, createSystem, createMemoryStore, fromSCXML } = api;

const toggle = createMachine({ id: 'toggle', initial: 'off', states: { off: { on: { T: 'on' } }, on: { on: { T: 'off' } } } });
const counter = fromSCXML(`<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" initial="s">
  <datamodel><data id="n" expr="0"/></datamodel>
  <state id="s"><transition event="T" cond="n &gt;= 0"><assign location="n" expr="n + 1"/><script>n = n % 1000000;</script></transition></state>
</scxml>`);

/* best of `reps` runs of fn(n) -> ops/s */
function measure(fn, n, reps = 3) {
  let best = 0;
  for (let r = 0; r < reps; r++) {
    const t0 = performance.now();
    const ops = fn(n);
    const dt = (performance.now() - t0) / 1000;
    best = Math.max(best, ops / dt);
  }
  return best;
}
const sys = (extra = {}) => createSystem({ clock: () => 0, ...extra });

const out = {};
out['machine send'] = measure((n) => {
  const system = sys();
  const a = createActor(toggle, { system }).start();
  for (let i = 0; i < n; i++) a.send({ type: 'T' });
  system.stop();
  return n;
}, 100000 * scale);
out['machine sendMany'] = measure((n) => {
  const system = sys();
  const actors = Array.from({ length: 1000 }, () => createActor(toggle, { system }).start());
  const batch = actors.map((a) => [a, { type: 'T' }]);
  for (let i = 0; i < n / 1000; i++) system.sendMany(batch);
  system.flush();
  system.stop();
  return n;
}, 500000 * scale);
out['scxml send'] = measure((n) => {
  const system = sys();
  const a = createActor(counter, { system }).start();
  for (let i = 0; i < n; i++) a.send({ type: 'T' });
  system.stop();
  return n;
}, 50000 * scale);
out['spawn'] = measure((n) => {
  const system = sys();
  for (let i = 0; i < n; i++) createActor(toggle, { system }).start();
  system.flush();
  system.stop();
  return n;
}, 20000 * scale);
{
  const n = 10000 * scale;
  let hib = 0, wake = 0;
  for (let r = 0; r < 3; r++) {
    const system = sys({ hibernation: { store: createMemoryStore(), sweepMs: 0 } });
    const actors = Array.from({ length: n }, () => createActor(counter, { system }).start());
    system.flush();
    let t0 = performance.now();
    for (const a of actors) system.hibernate(a);
    hib = Math.max(hib, n / ((performance.now() - t0) / 1000));
    t0 = performance.now();
    system.sendMany(actors.map((a) => [a, { type: 'T' }]));
    system.flush();
    wake = Math.max(wake, n / ((performance.now() - t0) / 1000));
    if (actors.some((a) => a.hibernated)) throw new Error('not all woke');
    system.stop();
  }
  out['scxml hibernate'] = hib;
  out['scxml wake + event'] = wake;
}

if (args.includes('--json')) process.stdout.write(JSON.stringify(out));
else console.log(binding, out);
