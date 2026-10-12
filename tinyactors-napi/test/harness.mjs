// A test harness that runs the same under node and bun: bun refuses
// node:test outside `bun test`. Each test file calls test() for its cases
// and run() once; the process exits 1 when a case fails.
const cases = [];

export function test(name, body) {
  cases.push({ name, body });
}

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
