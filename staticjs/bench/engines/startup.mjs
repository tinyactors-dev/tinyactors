// The cost of getting to run one expression, for the start-up table of
// `mise run bench-engines`, in a process of its own:
//
//   node [--jitless] startup.mjs CASES MODE [SAMPLES SAMPLE_MS WARMUP_MS]
//
// "The guard" is bench.c's eval.guard.event: its source, with its globals
// bound once from their literals (read from CASES, dump.c's `cases`). Each
// mode times one op k times per sample (k calibrated so that a sample takes
// at least SAMPLE_MS), after WARMUP_MS of warm-up, and prints the ns per
// op of each sample. What each op includes is in MODES; compare.mjs gives
// the staticjs rows from bench.c's own cases.
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import { loadCases } from './guest.mjs';

const [casesFile, mode, samplesText = '3', sampleMsText = '30', warmupMsText = '500'] = process.argv.slice(2);
const samples = Number(samplesText), sampleMs = Number(sampleMsText), warmupMs = Number(warmupMsText);
const guardCase = loadCases(casesFile).get('eval.guard.event');
if (!guardCase || guardCase.globals.some(g => g.fixture !== 'literal')) {
  throw new Error('eval.guard.event in bench.c is no longer a guard over literal globals');
}
const GUARD = guardCase.source;
const GLOBALS = guardCase.globals.map(g => `globalThis.${g.name} = (${g.literal});`).join(' ');

const quickjs = async () => {
  const { QuickJS } = await import('quickjs-wasi');
  const bytes = await readFile(new URL(import.meta.resolve('quickjs-wasi/quickjs.wasm')));
  return { QuickJS, module: await WebAssembly.compile(bytes) };
};
const counted = step => k => { let c = 0; for (let i = 0; i < k; i++) c += step(i); return c; };

// mode -> async () => op, where op(k) runs k ops and returns a checksum.
const MODES = {
  // A compiled vm.Script run in an existing context holding the globals.
  'node.run-compiled': async () => {
    const context = vm.createContext({});
    vm.runInContext(GLOBALS, context);
    const script = new vm.Script(GUARD);
    return counted(() => (script.runInContext(context) ? 1 : 0));
  },
  // A new vm.Script of a unique source (no cache hit), then run.
  'node.compile-run': async () => {
    const context = vm.createContext({});
    vm.runInContext(GLOBALS, context);
    return counted(i => (new vm.Script(`${GUARD}//${i}`).runInContext(context) ? 1 : 0));
  },
  // A new context, then a compiled `1 + 1` run in it.
  'node.fresh-context': async () => {
    const script = new vm.Script('1 + 1');
    return counted(() => script.runInContext(vm.createContext({})));
  },
  // evalCode(guard): compile + run + a handle back to the host + dump.
  'quickjs.host-eval': async () => {
    const { QuickJS, module } = await quickjs();
    const q = await QuickJS.create({ wasm: module });
    q.evalCode(GLOBALS).dispose();
    return counted(() => { const h = q.evalCode(GUARD); const v = q.dump(h) ? 1 : 0; h.dispose(); return v; });
  },
  // evalCode('1 + 1') + toNumber.
  'quickjs.host-eval-1+1': async () => {
    const { QuickJS, module } = await quickjs();
    const q = await QuickJS.create({ wasm: module });
    return counted(() => { const h = q.evalCode('1 + 1'); const v = h.toNumber(); h.dispose(); return v; });
  },
  // Inside the guest: an indirect eval of the guard (compile + run) in a
  // guest loop, no host crossing per op.
  'quickjs.guest-eval': async () => {
    const { QuickJS, module } = await quickjs();
    const q = await QuickJS.create({ wasm: module });
    q.evalCode(GLOBALS).dispose();
    q.evalCode(`globalThis.__guestEval = k => { let c = 0; for (let i = 0; i < k; i++) ` +
      `c += (0, eval)(${JSON.stringify(GUARD)} + "//" + i) ? 1 : 0; return c; }`).dispose();
    return k => { const h = q.evalCode(`__guestEval(${k})`); const v = h.toNumber(); h.dispose(); return v; };
  },
  // QuickJS.create (a new WebAssembly instance of the precompiled module, a
  // runtime and a context) + evalCode('1 + 1') + dispose.
  'quickjs.fresh': async () => {
    const { QuickJS, module } = await quickjs();
    return async k => {
      let c = 0;
      for (let i = 0; i < k; i++) {
        const q = await QuickJS.create({ wasm: module });
        const h = q.evalCode('1 + 1');
        c += h.toNumber();
        h.dispose();
        q.dispose();
      }
      return c;
    };
  },
};

if (!MODES[mode]) throw new Error(`mode ${mode}`);
const op = await MODES[mode]();
const now = () => Number(process.hrtime.bigint());
let k = 1, sink = 0;
const start = now();
for (;;) {
  const at = now();
  sink += await op(k);
  const ns = now() - at;
  if (ns >= sampleMs * 1e6) break;
  k *= ns < sampleMs * 1e5 ? 8 : 2;
}
while (now() - start < warmupMs * 1e6) sink += await op(k);
const perOp = [];
for (let s = 0; s < samples; s++) {
  const at = now();
  sink += await op(k);
  perOp.push((now() - at) / k);
}
process.stdout.write(`${JSON.stringify({ mode, k, nsPerOp: perOp, sink })}\n`);
