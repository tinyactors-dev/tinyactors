// One engine, one case, one size, in a process of its own, for
// `mise run bench-engines`:
//
//   node [--jitless] [--expose-gc] engine.mjs CASES ENGINE MODE CASE N [SAMPLES SAMPLE_MS WARMUP_MS]
//
// CASES is dump.c's `cases` output; ENGINE is `node` (the guest runs in this
// process's V8: JIT, or not with --jitless) or `quickjs-wasi` (the guest
// runs in QuickJS-NG compiled to WebAssembly, in a fresh instance). It
// prints one JSON line.
//
//   time   calibrates k so that one sample (k ops) takes at least SAMPLE_MS,
//          warms up for WARMUP_MS, then takes SAMPLES samples: each is the
//          host's clock around one call into the guest that runs the case
//          k times there (__bench.run(k)); prints ns per op of each sample.
//   check  the case's fixtures and one result as __show prints them.
//   mem    bytes live after the fixtures are built, and what one held
//          result adds, each after a full collection (Node needs
//          --expose-gc): QuickJS's own malloc accounting, or Node's
//          heapUsed + external, which the runner's own objects share.
import { readFile } from 'node:fs/promises';
import { guestSource, loadCases } from './guest.mjs';

const [casesFile, engine, mode, name, nText, samplesText = '3', sampleMsText = '30', warmupMsText = '300'] =
  process.argv.slice(2);
const n = Number(nText);
const samples = Number(samplesText), sampleMs = Number(sampleMsText), warmupMs = Number(warmupMsText);
const info = loadCases(casesFile).get(name);
if (!info) throw new Error(`no case ${name} in ${casesFile}`);
const source = guestSource(info, n);
const now = () => Number(process.hrtime.bigint());

// evaluate(code) -> the value (dumped to the host), runK(k) -> a number,
// collect() a full GC, used() live bytes, linearMemory() WebAssembly bytes.
let evaluate, runK, collect, used, linearMemory = () => null, dispose = () => {}, versions = null;
if (engine === 'node') {
  evaluate = code => (0, eval)(code);
  runK = k => globalThis.__bench.run(k);
  collect = () => {
    if (!globalThis.gc) throw new Error('mem needs node --expose-gc');
    globalThis.gc();
    globalThis.gc();
  };
  used = () => { const m = process.memoryUsage(); return m.heapUsed + m.external; };
} else if (engine === 'quickjs-wasi') {
  const { QuickJS } = await import('quickjs-wasi');
  const bytes = await readFile(new URL(import.meta.resolve('quickjs-wasi/quickjs.wasm')));
  const vm = await QuickJS.create({ wasm: await WebAssembly.compile(bytes) });
  evaluate = code => { const handle = vm.evalCode(code); const value = vm.dump(handle); handle.dispose(); return value; };
  runK = k => { const handle = vm.evalCode(`__bench.run(${k})`); const value = handle.toNumber(); handle.dispose(); return value; };
  collect = () => vm.runGC();
  used = () => vm.getMemoryUsage().mallocSize;
  linearMemory = () => vm._getMemory().buffer.byteLength;
  dispose = () => vm.dispose();
  versions = vm.versions;
} else {
  throw new Error(`engine ${engine}`);
}

const out = { engine: engine + (process.execArgv.includes('--jitless') ? '-jitless' : ''), mode, case: name, n };
if (mode === 'time') {
  evaluate(source);
  let k = 1, sink = 0;
  const start = now();
  for (;;) {
    const at = now();
    sink += runK(k);
    const ns = now() - at;
    if (ns >= sampleMs * 1e6) break;
    k *= ns < sampleMs * 1e5 ? 8 : 2;
  }
  while (now() - start < warmupMs * 1e6) sink += runK(k);
  const perOp = [];
  for (let s = 0; s < samples; s++) {
    const at = now();
    sink += runK(k);
    perOp.push((now() - at) / k);
  }
  Object.assign(out, { k, nsPerOp: perOp, sink });
} else if (mode === 'check') {
  evaluate(source);
  Object.assign(out, JSON.parse(evaluate('__bench.checkOnce()')), { fixtures: JSON.parse(evaluate('__bench.fixtures()')) });
} else if (mode === 'mem') {
  collect();
  const before = used();
  evaluate(source);
  evaluate('0');
  collect();
  const afterFixtures = used();
  evaluate('globalThis.__held = __bench.once(); 0');
  collect();
  const withResult = used();
  const linear = linearMemory();
  evaluate('globalThis.__held = undefined; 0');
  collect();
  Object.assign(out, { before, afterFixtures, heldBytes: withResult - afterFixtures, afterRelease: used(),
    linearMemoryBytes: linear });
} else {
  throw new Error(`mode ${mode}`);
}
if (versions) out.versions = versions;
dispose();
process.stdout.write(`${JSON.stringify(out)}\n`);
