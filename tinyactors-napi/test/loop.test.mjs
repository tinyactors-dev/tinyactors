// The loop on the event loop (docs/plan.xml, p1-loop): idle() as a
// promise of the next idle turn, nextDeadline, close() with a waiting
// idle(), SimulatedClock in milliseconds and the hosts it holds weakly.
// The run statuses other than idle, the bound that yields to I/O and the
// one timer at a deadline need actors, which come with images
// (p1-compile): log entry l-p1-loop-statuses-untested.
import { Host, SimulatedClock, TinyactorsError } from '@tinyactors/node';
import { collect, run, test } from './harness.mjs';

const QUICKJS = { datamodel: 'quickjs' };
const CONFIG = { systemID: 't' };

function equal(actual, expected, what) {
  if (actual !== expected) throw new Error(`${what}: ${actual}, expected ${expected}`);
}

function throws(body, check, what) {
  try {
    body();
  } catch (error) {
    check(error);
    return;
  }
  throw new Error(`${what}: not refused`);
}

const typeError = (error) => {
  if (!(error instanceof TypeError)) throw error;
};
const rangeError = (error) => {
  if (!(error instanceof RangeError)) throw error;
};
const stateError = (error) => {
  if (!(error instanceof TinyactorsError)) throw error;
  equal(error.status, 'TA_ERROR_STATE', 'status');
};

const tick = () => new Promise((resolve) => setImmediate(resolve));

test('idle() on a fresh host settles at the next turn, with no driver called', async () => {
  const host = new Host(CONFIG, QUICKJS);
  const first = host.idle();
  if (!(first instanceof Promise)) throw new Error('idle() gave no promise');
  equal(host.idle(), first, 'the same promise until it settles');
  equal(await first, undefined, 'its value');
  const second = host.idle();
  if (second === first) throw new Error('a settled promise was given again');
  await second;
  host.close();
});

test('nextDeadline is undefined with nothing due, and on a closed host', async () => {
  const host = new Host(CONFIG, QUICKJS);
  equal(host.nextDeadline, undefined, 'before a turn');
  await host.idle();
  equal(host.nextDeadline, undefined, 'after an idle turn with no deadline');
  host.close();
  equal(host.nextDeadline, undefined, 'closed');
});

test('idle() on a closed host is refused', () => {
  const host = new Host(CONFIG, QUICKJS);
  host.close();
  throws(() => host.idle(), stateError, 'idle() after close');
});

test('close() rejects a waiting idle() and clears its turn', async () => {
  const host = new Host(CONFIG, QUICKJS);
  const waiting = host.idle();
  host.close();
  let rejected;
  try {
    await waiting;
  } catch (error) {
    rejected = error;
  }
  if (rejected === undefined) throw new Error('idle() settled after close');
  stateError(rejected);
  await tick(); // the cleared turn does not run on a freed host
  await tick();
  equal(host.closed, true, 'closed');
});

test('idle() on another object is refused by the type tag', () => {
  for (const self of [{}, Object.create(Host.prototype), new SimulatedClock()]) {
    throws(() => Host.prototype.idle.call(self), typeError, 'idle on another object');
  }
});

test('SimulatedClock: milliseconds, moved only by increment', () => {
  const clock = new SimulatedClock();
  equal(clock.now, 0, 'a new clock');
  clock.increment(1000);
  equal(clock.now, 1000, 'after 1000 ms');
  clock.increment(0.5);
  equal(clock.now, 1000.5, 'fractions of a millisecond');
  clock.increment(0);
  equal(clock.now, 1000.5, 'after 0 ms');
  throws(() => clock.increment(-1), rangeError, 'a negative increment');
  throws(() => clock.increment(Number.NaN), rangeError, 'NaN');
  throws(() => clock.increment(Number.POSITIVE_INFINITY), rangeError, 'infinity');
  throws(() => clock.increment('1'), typeError, 'a string');
  throws(() => clock.increment(), typeError, 'no argument');
  throws(() => clock.increment(1n), typeError, 'a bigint');
  equal(clock.now, 1000.5, 'unchanged by the refusals');
  throws(() => SimulatedClock(), typeError, 'a call without new');
  throws(() => SimulatedClock.prototype.increment.call({}, 1), typeError, 'increment on another object');
  throws(() => new Host(CONFIG, { ...QUICKJS, clock: {} }), typeError, 'a clock that is not a SimulatedClock');
});

test('increment schedules a turn on each host made with the clock', async () => {
  const clock = new SimulatedClock();
  const hosts = [new Host(CONFIG, { ...QUICKJS, clock }), new Host(CONFIG, { ...QUICKJS, clock })];
  clock.increment(250);
  for (const host of hosts) {
    await host.idle();
    equal(host.nextDeadline, undefined, 'no deadline');
  }
  hosts[0].close();
  clock.increment(250); // a closed host is forgotten, the open one turns
  await hosts[1].idle();
  hosts[1].close();
  clock.increment(1);
});

test('a clock holds its hosts weakly', async () => {
  const clock = new SimulatedClock();
  let collected = 0;
  const registry = new FinalizationRegistry(() => collected++);
  (() => {
    for (let i = 0; i < 10; i++) registry.register(new Host(CONFIG, { ...QUICKJS, clock }), i);
  })();
  await collect();
  equal(collected, 10, 'hosts collected while their clock lives');
  clock.increment(1000); // their WeakRefs are empty: nothing to schedule
  equal(clock.now, 1000, 'the clock goes on');
});

await run();
