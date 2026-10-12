// The driver of `mise run bench-engines` (staticjs/mise/tasks/bench-engines
// builds what it needs and runs it twice: `verify`, then `measure`):
//
//   node compare.mjs PHASE --cases=FILE --dump=PROGRAM --bench=PROGRAM
//                    --records=FILE [--filter=PREFIX] [--rounds=N] [--quick]
//
// Every engine runs in a process of its own, one at a time (never two at
// once), each under a timeout. Records are appended to --records as JSON
// lines, each with a `kind`; report.mjs turns them into the report.
//
// verify: for every selected case, at its smoke size and at its measured
//   size, bench.c's own fixtures and result (dump.c `values`, written by
//   staticjs's writer) must equal what guest.js builds and what each engine
//   computes (length and hash), and at the smoke size each engine's result
//   must print as bench.c's .expect. Any difference exits 1, naming the
//   case, before anything is timed.
// measure: for each case, ROUNDS rounds; a round runs the four engines one
//   after another, in the order staticjs, Node, Node --jitless,
//   quickjs-wasi, and the next round in the reverse order, so that drift
//   does not favour one engine. Then the start-up rows, in rounds of their
//   own, and the memory of each eval case in quickjs-wasi and Node.
import { spawnSync } from 'node:child_process';
import { appendFileSync } from 'node:fs';
import { loadavg } from 'node:os';
import { fileURLToPath } from 'node:url';
import { CASES } from './cases.mjs';
import { loadCases, unsupported } from './guest.mjs';

const STATICJS = fileURLToPath(new URL('../..', import.meta.url));
const ENGINE = fileURLToPath(new URL('engine.mjs', import.meta.url));
const STARTUP = fileURLToPath(new URL('startup.mjs', import.meta.url));
const CHILD_TIMEOUT_MS = 300_000;

const [phase, ...rest] = process.argv.slice(2);
const option = name => rest.find(a => a.startsWith(`--${name}=`))?.slice(name.length + 3);
const quick = rest.includes('--quick');
const casesFile = option('cases'), dump = option('dump'), bench = option('bench'), recordsFile = option('records');
const filter = option('filter') ?? '';
if (!['verify', 'measure'].includes(phase) || !casesFile || !dump || !bench || !recordsFile) {
  console.error('usage: node compare.mjs verify|measure --cases=FILE --dump=PROGRAM --bench=PROGRAM --records=FILE ' +
    '[--filter=PREFIX] [--rounds=N] [--quick]');
  process.exit(2);
}

// How long each run samples. --quick is for a look, not for a report.
const SETTINGS = quick
  ? { rounds: 3, samples: 3, sampleMs: 10, warmupNodeMs: 200, warmupStaticjsMs: 100, startupSampleMs: 10,
      startupWarmupMs: 100 }
  : { rounds: 5, samples: 3, sampleMs: 30, warmupNodeMs: 1000, warmupStaticjsMs: 300, startupSampleMs: 30,
      startupWarmupMs: 500 };
if (option('rounds')) SETTINGS.rounds = Math.max(1, Number(option('rounds')));

const info = loadCases(casesFile);
const selected = CASES.filter(c => c.name.startsWith(filter));
if (selected.length === 0) {
  console.error(`bench-engines: no case in bench/engines/cases.mjs starts with ${JSON.stringify(filter)}`);
  process.exit(1);
}
const record = r => appendFileSync(recordsFile, `${JSON.stringify(r)}\n`);
const median = xs => { const s = [...xs].sort((a, b) => a - b); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };

// Runs a program to completion under the timeout; its stdout's JSON lines.
function run(command, args) {
  const p = spawnSync(command, args, { cwd: STATICJS, encoding: 'utf8', timeout: CHILD_TIMEOUT_MS, killSignal: 'SIGKILL',
    maxBuffer: 64 << 20 });
  const what = `${command === process.execPath ? 'node' : command} ${args.join(' ')}`;
  if (p.error) throw new Error(`${what}: ${p.error.code === 'ETIMEDOUT' ? `timed out after ${CHILD_TIMEOUT_MS / 1000} s` : p.error.message}`);
  if (p.status !== 0) throw new Error(`${what}: exit ${p.status ?? p.signal}: ${(p.stderr || '').trim().slice(-600)}`);
  return p.stdout.trim().split('\n').filter(Boolean).map(line => JSON.parse(line));
}

// The JavaScript engines: how to start each, by name.
const NODE = process.execPath;
const JS_ENGINES = {
  node: (...args) => [NODE, [ENGINE, casesFile, 'node', ...args]],
  'node-jitless': (...args) => [NODE, ['--jitless', ENGINE, casesFile, 'node', ...args]],
  'quickjs-wasi': (...args) => [NODE, [ENGINE, casesFile, 'quickjs-wasi', ...args]],
};

const isCompile = c => c.op === 'compile';
const smokeSize = c => (isCompile(c) ? 1 : c.smoke);

function verify() {
  const failures = [];
  for (const { name, n } of selected) {
    const c = info.get(name);
    const problems = [];
    if (!c) {
      failures.push(`${name}: not a case of bench/bench.c`);
      console.log(`${name}: not a case of bench/bench.c`);
      continue;
    }
    const why = unsupported(c);
    if (why) problems.push(why);
    else if (isCompile(c) && n !== null) problems.push('a compile case takes no size in cases.mjs');
    else if (!isCompile(c) && !c.sizes.includes(n)) problems.push(`n = ${n} is not one of bench.c's sizes (${c.sizes.join(', ')})`);
    const sizes = [...new Set([smokeSize(c), n ?? 1])];
    const checked = {};
    for (const size of problems.length ? [] : sizes) {
      let staticjs;
      try {
        [staticjs] = run(dump, ['values', name, String(size)]);
      } catch (error) {
        problems.push(`staticjs at n = ${size}: ${error.message}`);
        continue;
      }
      for (const [engine, start] of Object.entries(JS_ENGINES)) {
        let got;
        try {
          [got] = run(...start('check', name, String(size)));
        } catch (error) {
          problems.push(`${engine} at n = ${size}: ${error.message}`);
          continue;
        }
        for (const g of c.globals) {
          const want = staticjs.globals[g.name], have = got.fixtures[g.name];
          if (!want || !have || want.hash !== have.hash || want.len !== have.len) {
            problems.push(`fixture ${g.name} at n = ${size} differs: bench.c ${want?.hash}/${want?.len} chars, ` +
              `guest.js in ${engine} ${have?.hash}/${have?.len} chars`);
          }
        }
        if (staticjs.result && (staticjs.result.hash !== got.hash || staticjs.result.len !== got.len)) {
          problems.push(`result at n = ${size} differs: staticjs ${staticjs.result.hash}/${staticjs.result.len} chars, ` +
            `${engine} ${got.hash}/${got.len} chars`);
        }
        if (size === smokeSize(c) && !isCompile(c) && got.show !== c.expect) {
          problems.push(`${engine} at the smoke size n = ${size} gives ${got.show}, bench.c expects ${c.expect}`);
        }
        checked[`${engine}@${size}`] = { hash: got.hash, len: got.len, fold: got.fold };
      }
      if (isCompile(c)) {
        const results = new Set(Object.keys(JS_ENGINES).map(e => checked[`${e}@${size}`]?.hash));
        if (results.size !== 1) problems.push(`the engines disagree at n = ${size}`);
      }
    }
    const at = checked[`node@${n ?? 1}`];
    record({ kind: 'verify', case: name, n, sizes, result: at ?? null, problems });
    console.log(`${name}: ${problems.length ? `FAILED\n  ${problems.join('\n  ')}` :
      `fixtures and results agree at n = ${sizes.join(' and ')}` + (at ? ` (${at.len} chars, hash ${at.hash})` : '')}`);
    if (problems.length) failures.push(name);
  }
  if (failures.length) {
    console.error(`bench-engines: verification failed for ${failures.join(', ')}; nothing was timed`);
    process.exit(1);
  }
}

function staticjsTime(name, n) {
  const c = info.get(name);
  const lines = run(bench, ['--jsonl', `--only=${name}`, ...(isCompile(c) ? [] : [`--size=${n}`]),
    `--samples=${SETTINGS.samples}`, `--sample-ms=${SETTINGS.sampleMs}`, `--warmup-ms=${SETTINGS.warmupStaticjsMs}`]);
  const meta = lines.find(l => l.meta)?.meta;
  const r = lines.find(l => l.bench === name);
  if (!r || r.error) throw new Error(`bench: ${r?.error ?? 'no record'}`);
  return { nsPerOp: r.nsPerOp.median, p25: r.nsPerOp.p25, p75: r.nsPerOp.p75, unit: r.unit, unitsPerOp: r.unitsPerOp,
    memory: r.memory, fuelPerOp: r.fuelPerOp, itersPerSample: r.itersPerSample, staticjsVersion: meta?.version };
}

function jsTime(engine, name, n) {
  const [r] = run(...JS_ENGINES[engine]('time', name, String(n ?? 1), String(SETTINGS.samples), String(SETTINGS.sampleMs),
    String(SETTINGS.warmupNodeMs)));
  return { nsPerOp: median(r.nsPerOp), samples: r.nsPerOp, k: r.k, ...(r.versions ? { versions: r.versions } : {}) };
}

// The start-up rows: what each includes is printed in the report.
const STARTUP_ROWS = [
  ['guard: eval of a compiled program', 'staticjs', 'bench:eval.guard.event',
    'sjs_vm_start + run + take result + release (program compiled once, globals bound once)'],
  ['guard: compile', 'staticjs', 'bench:compile.guard.event.whole',
    'parser + compiler to a program, released (no evaluation)'],
  ['`1`: fresh evaluator', 'staticjs', 'bench:vm.fresh', 'sjs_vm_init + start + run `1` + take result + sjs_vm_destroy'],
  ['`1`: kept evaluator', 'staticjs', 'bench:vm.reuse', 'start + run `1` + take + release'],
  ['guard: vm.Script.runInContext', 'node', 'node.run-compiled',
    'compiled vm.Script run in an existing context (globals there), result to host'],
  ['guard: new vm.Script + runInContext', 'node', 'node.compile-run',
    'compile (unique source, no cache hit; V8 compiles lazily) + run'],
  ['`1 + 1`: vm.createContext + run', 'node', 'node.fresh-context', 'a new V8 context + run of a compiled script'],
  ['guard: vm.Script.runInContext', 'node-jitless', 'node.run-compiled', 'as above, --jitless'],
  ['guard: new vm.Script + runInContext', 'node-jitless', 'node.compile-run', 'as above, --jitless'],
  ['guard: evalCode (host)', 'quickjs-wasi', 'quickjs.host-eval',
    'evalCode(guard source): compile + run + handle back + dump + dispose'],
  ['`1 + 1`: evalCode (host)', 'quickjs-wasi', 'quickjs.host-eval-1+1', 'evalCode("1 + 1") + toNumber + dispose'],
  ['guard: eval inside the guest', 'quickjs-wasi', 'quickjs.guest-eval',
    'indirect eval (compile + run) in a guest loop; no host crossing per op'],
  ['`1 + 1`: fresh QuickJS.create', 'quickjs-wasi', 'quickjs.fresh',
    'new WebAssembly instance (module precompiled) + runtime + context + evalCode + dispose'],
];

function startupTime(engine, mode) {
  if (mode.startsWith('bench:')) {
    const name = mode.slice(6);
    const lines = run(bench, ['--jsonl', `--only=${name}`, `--samples=${SETTINGS.samples}`,
      `--sample-ms=${SETTINGS.startupSampleMs}`, `--warmup-ms=${SETTINGS.warmupStaticjsMs}`]);
    const r = lines.find(l => l.bench === name);
    if (!r || r.error) throw new Error(`bench: ${r?.error ?? 'no record'}`);
    return { nsPerOp: r.nsPerOp.median };
  }
  const [r] = run(NODE, [...(engine === 'node-jitless' ? ['--jitless'] : []), STARTUP, casesFile, mode,
    String(SETTINGS.samples), String(SETTINGS.startupSampleMs), String(SETTINGS.startupWarmupMs)]);
  return { nsPerOp: median(r.nsPerOp), samples: r.nsPerOp };
}

function measure() {
  record({ kind: 'settings', ...SETTINGS, quick, filter, cases: selected.map(c => c.name), node: process.version,
    v8: process.versions.v8 });
  const engines = ['staticjs', ...Object.keys(JS_ENGINES)];
  const start = Date.now();
  const elapsed = () => `${((Date.now() - start) / 1000).toFixed(0)} s`;
  for (const { name, n } of selected) {
    for (let round = 0; round < SETTINGS.rounds; round++) {
      for (const engine of round % 2 === 0 ? engines : [...engines].reverse()) {
        const r = { kind: 'time', case: name, n, round, engine, load1: loadavg()[0] };
        try {
          Object.assign(r, engine === 'staticjs' ? staticjsTime(name, n) : jsTime(engine, name, n));
        } catch (error) {
          r.error = error.message.slice(0, 600);
        }
        record(r);
      }
    }
    console.log(`${name}: timed, ${elapsed()}, load ${loadavg()[0].toFixed(2)}`);
  }
  for (let round = 0; round < SETTINGS.rounds; round++) {
    for (const [label, engine, mode, includes] of round % 2 === 0 ? STARTUP_ROWS : [...STARTUP_ROWS].reverse()) {
      const r = { kind: 'startup', label, engine, mode, includes, round };
      try {
        Object.assign(r, startupTime(engine, mode));
      } catch (error) {
        r.error = error.message.slice(0, 600);
      }
      record(r);
    }
  }
  console.log(`start-up: timed, ${elapsed()}`);
  for (const { name, n } of selected) {
    if (isCompile(info.get(name))) continue;
    for (const [engine, flags, start] of [['quickjs-wasi', [], 'quickjs-wasi'], ['node', ['--expose-gc'], 'node']]) {
      for (const size of [...new Set([1, n])]) {
        const r = { kind: 'memory', case: name, n: size, engine };
        try {
          Object.assign(r, run(NODE, [...flags, ENGINE, casesFile, start, 'mem', name, String(size)])[0], { kind: 'memory' });
        } catch (error) {
          r.error = error.message.slice(0, 600);
        }
        record(r);
      }
    }
  }
  console.log(`memory: measured, ${elapsed()}`);
}

if (phase === 'verify') verify();
else measure();
