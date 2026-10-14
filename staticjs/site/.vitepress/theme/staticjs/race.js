// The front page's race: the same program in staticjs and in QuickJS, both
// compiled to WebAssembly, run in this browser. Each engine compiles the
// program once and runs it k times per sample; k grows until a sample takes
// long enough for the clock to resolve it.
//
// staticjs runs as the C API does: for each run, start it on the compiled
// program and push fuel until it is done, called from JavaScript each time.
// QuickJS runs the program as a function, k times in a loop inside the
// engine, called once per sample: it pays no crossing per run.

/** A runner for staticjs: `check()` gives the result as text, `run(k)` runs k times. */
export function staticjsRunner(staticjs, source) {
  const compiled = staticjs.compile(source);
  if (!compiled.ok) throw new Error(`staticjs refused the program: ${compiled.error.message}`);
  return {
    check() {
      staticjs.start();
      if (staticjs.run(1e12) !== 'done') throw new Error('staticjs faulted');
      return staticjs.write().text;
    },
    run(k) {
      for (let i = 0; i < k; i++) {
        staticjs.start();
        if (staticjs.run(1e12) !== 'done') throw new Error('staticjs faulted');
      }
    },
  };
}

/** A runner for QuickJS (quickjs-wasi): the program as a function in the engine. */
export function quickjsRunner(vm, source) {
  vm.evalCode(
    `globalThis.__program = () => (${source});\n` +
      'globalThis.__run = (k) => { let n = 0; for (let i = 0; i < k; i++) if (__program() !== undefined) n++; return n; };\n0',
  ).dispose();
  return {
    check() {
      const handle = vm.evalCode('JSON.stringify(__program())');
      const text = vm.dump(handle);
      handle.dispose();
      return text;
    },
    run(k) {
      vm.evalCode(`__run(${k})`).dispose();
    },
  };
}

const median = (xs) => [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)];

/**
 * Nanoseconds per run, the median of `samples` samples of k runs each: k
 * doubles until one sample takes at least `sampleMs`.
 */
export function measure(runner, { now, sampleMs = 40, samples = 5 }) {
  let k = 1;
  for (;;) {
    const at = now();
    runner.run(k);
    const ms = now() - at;
    if (ms >= sampleMs) break;
    k *= ms < sampleMs / 10 ? 8 : 2;
  }
  const perRun = [];
  for (let s = 0; s < samples; s++) {
    const at = now();
    runner.run(k);
    perRun.push(((now() - at) * 1e6) / k);
  }
  return { ns: median(perRun), runs: k * samples };
}
