// A test harness that runs the same under node and bun: bun refuses
// node:test outside `bun test`. Each test file calls test() for its cases
// and run() once; the process exits 1 when a case fails.
const cases = [];

export function test(name, body) {
  cases.push({ name, body });
}

// Runs every case; again on each call (a file that runs its cases once per
// datamodel calls it once per datamodel).
export async function run() {
  let failed = 0;
  for (const { name, body } of cases) {
    try {
      await body();
      console.log(`ok   ${name}`);
    } catch (error) {
      failed++;
      console.log(`FAIL ${name}`);
      console.log(String(error?.stack ?? error).replace(/^/gm, '     '));
    }
  }
  if (failed) process.exitCode = 1;
}

// A full collection, then the turns the runtime runs finalizers in: Bun's
// collector is Bun.gc(true); node's gc() is exposed at run time
// (v8.setFlagsFromString, then a new context's global), so the test task
// passes no flag. Node runs Node-API finalizers after the collection, from
// the event loop, so each collection is followed by a turn.
let gc;
export async function collect(rounds = 10) {
  if (gc === undefined) {
    if (globalThis.Bun) gc = () => globalThis.Bun.gc(true);
    else {
      const v8 = await import('node:v8');
      const vm = await import('node:vm');
      v8.setFlagsFromString('--expose_gc');
      gc = vm.runInNewContext('gc');
    }
  }
  for (let i = 0; i < rounds; i++) {
    gc();
    await new Promise((resolve) => setImmediate(resolve));
  }
}
