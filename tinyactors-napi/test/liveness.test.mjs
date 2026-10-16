// What keeps the process alive (docs/plan.xml, p1-loop; AGENTS.md, "The
// loop"): a host keeps it alive only while a turn or a timer at a deadline
// is pending, so a process whose hosts have nothing to do exits, open or
// not. Each case runs a child process of this runtime. That an open host
// with a pending deadline keeps it alive until the deadline or close()
// needs an actor with a delayed send, which comes with images
// (p1-compile): log entry l-p1-loop-liveness-deadline.
import { spawnSync } from 'node:child_process';
import { run, test } from './harness.mjs';

const index = new URL('../index.mjs', import.meta.url).href;

// Runs `body` (the text of an async function's body, given the module as
// `m`) in a child; its exit status, output and whether it ran out of time.
function child(body) {
  const code = `import(${JSON.stringify(index)}).then(async (m) => { ${body} });`;
  const result = spawnSync(process.execPath, ['-e', code], { encoding: 'utf8', timeout: 20000 });
  return { status: result.status, output: `${result.stdout}${result.stderr}`, timedOut: result.error?.code === 'ETIMEDOUT' };
}

function exits(body, what) {
  const result = child(body);
  if (result.timedOut) throw new Error(`${what}: the process did not exit`);
  if (result.status !== 0) throw new Error(`${what}: exit status ${result.status}\n${result.output}`);
  return result.output;
}

test('a process with only an open host exits', () => {
  exits(`globalThis.host = new m.Host({ systemID: 't' }, { datamodel: 'quickjs' });`, 'a host never turned');
});

test('a process exits after an idle turn, the host still open', () => {
  const output = exits(`
    globalThis.host = new m.Host({ systemID: 't' }, { datamodel: 'quickjs' });
    await host.idle();
    console.log('idle', host.closed, host.nextDeadline);`, 'a host idle');
  if (!output.includes('idle false undefined')) throw new Error(`the child said: ${output}`);
});

test('a process exits with a simulated clock moved', () => {
  exits(`
    const clock = new m.SimulatedClock();
    globalThis.host = new m.Host({ systemID: 't' }, { datamodel: 'quickjs', clock });
    clock.increment(3600 * 1000);
    await host.idle();`, 'a host on a simulated clock');
});

test('a process exits when a waiting idle() is rejected by close()', () => {
  const output = exits(`
    const host = new m.Host({ systemID: 't' }, { datamodel: 'quickjs' });
    const waiting = host.idle();
    host.close();
    await waiting.catch((error) => console.log('rejected', error.status));`, 'a host closed while idle() waits');
  if (!output.includes('rejected TA_ERROR_STATE')) throw new Error(`the child said: ${output}`);
});

await run();
