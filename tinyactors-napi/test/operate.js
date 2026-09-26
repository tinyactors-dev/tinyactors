'use strict';

// Operating one actor (ADR 0027; core ADRs 0082, 0083, 0084): hold, step
// and unhold, budgets for one actor, the mailbox, operator transitions in
// post() and send(), abandoning a macrostep, withdrawing a delayed send,
// and what inspect() and definitions() report. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart, TinyactorsError } = require('..');

const watchdog = setTimeout(() => {
  console.error('operate.js: timed out');
  process.exit(1);
}, 30000);

async function assertRejects(promise, code) {
  await assert.rejects(promise, (error) => {
    assert.ok(error instanceof TinyactorsError, `expected TinyactorsError, got ${error}`);
    assert.equal(error.code, code, error.message);
    return true;
  });
}

function assertThrows(fn, code) {
  assert.throws(fn, (error) => error instanceof TinyactorsError && error.code === code);
}

const turn = () => new Promise((resolve) => setImmediate(resolve));

// a --hook--> b --hook--> a, counting; `spin` starts a macrostep that never
// ends; `arm` sets a delayed send; c and d are only reached by jumps.
const chart = () =>
  statechart({ family: 'account', revision: 'v1' })
    .data('hooks', 0)
    .data('entered', [])
    .state('a', (s) =>
      s
        .on('hook', (t) => t.target('b').assign('hooks', function () { return this.hooks + 1; }))
        .on('spin', (t) => t.target('loop1'))
        .on('arm', (t) => t.send('remind', (send) => send.id('reminder').after('10s'))))
    .state('b', (s) => s.on('hook', (t) => t.target('a').assign('hooks', function () { return this.hooks + 1; })))
    .state('c', (s) => s.entry((e) => e.script(function ({ event }) { this.entered.push(event.name); })))
    .state('d')
    .state('loop1', (s) =>
      s.entry((e) => e.script(function () { this.spins = (this.spins ?? 0) + 1; }).script(function () {}))
        .always((t) => t.target('loop2')))
    .state('loop2', (s) => s.always((t) => t.target('loop1')));

// Hold, the webhook case, step one message, and unhold.
async function holding() {
  const system = createSystem({ clock: 'manual' });
  const actor = system.spawn(system.define(chart()));
  await system.flush();
  assert.equal(system.hold(actor), true, 'an idle actor is held at once');
  assert.equal(actor.inspect().scheduling, 'held');

  // A webhook handler that waits with a timeout: its message is withdrawn,
  // never taken later, so a retry is not a duplicate.
  await assertRejects(actor.send('hook', { attempt: 1 }, { timeout: 20 }), 'timeout');
  assert.equal(system.mailbox(actor).length, 0);
  // One that acknowledged on acceptance waits for the hold to end.
  const accepted = await actor.send('hook', { attempt: 2 }, { until: 'accepted' });
  assert.equal(accepted.status, 'accepted');
  actor.post('hook', { attempt: 3 });
  await system.flush();
  assert.equal(actor.inspect().data.hooks, 0, 'held mail waits');
  assert.deepEqual(system.mailbox(actor).map((message) => message.data), [{ attempt: 2 }, { attempt: 3 }]);
  assert.deepEqual(system.mailbox(actor, { limit: 1 }).length, 1);

  assert.equal(system.step(actor), 'message');
  await system.flush();
  assert.equal(actor.inspect().data.hooks, 1, 'exactly one message');
  assert.equal(actor.inspect().scheduling, 'held');
  assertThrows(() => system.step(system.spawn(system.define(chart()))), 'invalid');

  system.unhold(actor);
  await system.flush();
  assert.equal(actor.inspect().data.hooks, 2);
  assert.equal(actor.inspect().scheduling, 'idle');

  // A hold on an actor inside a macrostep lets the macrostep end.
  const slow = createSystem({ clock: 'manual', scheduling: { resumeBudget: 1 } });
  const other = slow.spawn(slow.define(chart()));
  await slow.flush();
  const completed = other.send('hook');
  await turn();
  assert.equal(other.inspect().scheduling, 'runnable');
  assert.equal(slow.hold(other), false);
  assert.equal((await completed).status, 'stable', 'the send() of the macrostep in progress completes');
  assert.equal(other.inspect().scheduling, 'held');
  slow.close();

  // Held anywhere: an actor no caller waits on stops at every pause, inside
  // a macrostep too, and a step runs it to the next one.
  const fork = createSystem({ clock: 'manual' });
  const copy = fork.spawn(fork.define(chart()));
  await fork.flush();
  fork.setBudgets(copy, { resumeBudget: 64, actionBudget: 1 });
  assert.equal(fork.hold(copy, { anywhere: true }), true);
  copy.post('spin');
  assert.equal(fork.step(copy), 'message');
  await fork.flush();
  assert.equal(copy.inspect().scheduling, 'held');
  assert.equal(copy.inspect().currentEvent.name, 'spin', 'held with the event taken in, before its macrostep');
  assert.equal(copy.inspect().step.macrostep, 0);
  assert.equal(fork.step(copy), 'macrostep');
  await fork.flush();
  assert.equal(copy.inspect().microstepInProgress, true, 'held inside the microstep');
  const before = copy.inspect().position;
  assert.equal(fork.step(copy), 'macrostep');
  await fork.flush();
  assert.notDeepEqual(copy.inspect().position, before, 'one unit further');
  fork.close();

  const done = createSystem({ clock: 'manual', finished: 'retain' });
  const finished = done.spawn(done.define(statechart({ family: 'f', revision: 'v1' }).final('end')));
  await done.flush();
  assertThrows(() => done.hold(finished), 'done');
  done.close();
  system.close();
}

// Budgets for one actor: it pauses where the others do not.
async function budgets() {
  const system = createSystem({ clock: 'manual' });
  const definition = system.define(chart());
  const debugged = system.spawn(definition);
  const other = system.spawn(definition);
  await system.flush();
  system.setBudgets(debugged, { resumeBudget: 1, actionBudget: 1 });
  debugged.post('spin');
  other.post('hook');
  await turn();
  await turn();
  assert.equal(other.inspect().scheduling, 'idle');
  assert.equal(debugged.inspect().scheduling, 'runnable');
  assertThrows(() => system.setBudgets(debugged, { resumeBudget: 0 }), 'invalid');
  assertThrows(() => system.setBudgets(debugged, { resumeBudget: 1, actionBudget: -1 }), 'invalid');
  assertThrows(() => system.setBudgets(debugged, { resumeBudget: 1, microstepStart: 'yes' }), 'invalid');
  system.setBudgets(debugged, undefined);
  // With microstepStart, a microstep the resume begins stops at its start
  // (core ADR 0087): selected, in progress, and nothing of it ran.
  // (debugged spins on, so the system never flushes: count turns.)
  const started = system.spawn(definition);
  for (let turns = 0; turns < 10 && started.inspect().activeStates[0] !== 'a'; turns += 1) await turn();
  system.setBudgets(started, { resumeBudget: 1, actionBudget: 1, microstepStart: true });
  started.post('hook');
  for (let turns = 0; turns < 10 && !started.inspect().microstepInProgress; turns += 1) await turn();
  const start = started.inspect();
  assert.equal(start.microstepInProgress, true);
  assert.equal(start.position.at, 'transition');
  assert.equal(start.data.hooks, 0);
  system.setBudgets(started, undefined);
  for (let turns = 0; turns < 10 && started.inspect().macrostepInProgress; turns += 1) await turn();
  assert.equal(started.inspect().data.hooks, 1);
  system.close();
}

// Operator transitions queue behind real mail; a refused one is a dead
// letter and the actor is not quarantined.
async function transitions() {
  const deadLetters = [];
  const system = createSystem({ clock: 'manual', deadLetter: (record) => deadLetters.push(record) });
  const actor = system.spawn(system.define(chart()));
  await system.flush();
  actor.post('hook');
  const jumped = actor.send('operator.goto', { by: 'test' }, { transition: { targets: ['c'] } });
  const [queued] = system.mailbox(actor).slice(-1);
  assert.deepEqual(queued.transition, { source: null, targets: ['c'] });
  assert.equal(queued.awaited, true);
  assert.equal((await jumped).status, 'stable');
  assert.deepEqual(actor.inspect().activeStates, ['c']);
  assert.deepEqual(actor.inspect().data.entered, ['operator.goto'], 'onentry sees the operator event');

  // From b, but the actor is in c by the time it arrives: refused.
  await assertRejects(actor.send('operator.goto', undefined, { transition: { targets: ['d'], source: 'b' } }), 'invalid');
  assert.equal(actor.inspect().scheduling, 'idle', 'not quarantined');
  assert.deepEqual(actor.inspect().activeStates, ['c']);
  assert.equal(deadLetters.at(-1).reason, 'refused');

  assertThrows(() => actor.post('operator.goto', undefined, { transition: { targets: ['nowhere'] } }), 'invalid');
  assertThrows(() => actor.post('operator.goto', undefined, { transition: { targets: [] } }), 'invalid');
  assertThrows(() => system.post({ family: 'account', name: 'x' }, 'operator.goto', undefined,
                                 { transition: { targets: ['c'] } }), 'invalid');
  system.close();
}

// Abandoning a macrostep that never ends: the send() rejects with
// `abandoned`, and the actor takes its next message.
async function abandoning() {
  const system = createSystem({ clock: 'manual', scheduling: { actionBudget: 1 } });
  const actor = system.spawn(system.define(chart()));
  await system.flush();
  const runaway = actor.send('spin');
  actor.post('operator.goto', undefined, { transition: { targets: ['d'] } });
  let view;
  for (let index = 0; index < 20; index += 1) {
    await turn();
    view = actor.inspect();
    if (view.microstepInProgress) break;
  }
  assert.equal(view.macrostepInProgress, true);
  assert.equal(view.microstepInProgress, true);
  assert.ok(view.step.macrostep > 0 && view.step.microstep > 0, 'inspection names the step');
  assert.equal(view.position.at, 'enter');
  assert.equal(view.position.state, 'loop1');
  // Inside a microstep core refuses; budgets of its own let it finish the
  // microstep and pause before the next one (core ADR 0082).
  assertThrows(() => system.abandon(actor), 'busy');
  system.setBudgets(actor, { resumeBudget: 1, actionBudget: 0 });
  await turn();
  assert.equal(actor.inspect().microstepInProgress, false);
  system.abandon(actor);
  system.setBudgets(actor, undefined);
  await assertRejects(runaway, 'abandoned');
  await system.flush();
  assert.deepEqual(actor.inspect().activeStates, ['d'], 'the next message, a jump, was taken');
  assertThrows(() => system.abandon(actor), 'invalid');
  system.close();
}

// Withdrawing one delayed send by the ID inspection shows.
async function withdrawing() {
  const system = createSystem({ clock: 'manual' });
  const actor = system.spawn(system.define(chart()));
  await system.flush();
  actor.post('arm');
  await system.flush();
  const [send] = actor.inspect().delayedSends;
  assert.equal(send.id, 'reminder');
  system.withdrawSend(actor, send.id);
  assert.equal(actor.inspect().delayedSendCount, 0);
  assertThrows(() => system.withdrawSend(actor, 'reminder'), 'not-found');
  assertThrows(() => system.withdrawSend(actor, ''), 'invalid');
  system.close();
}

// What inspect() and definitions() report for a debugger.
async function inspection() {
  const system = createSystem({ clock: 'manual', scheduling: { resumeBudget: 1 } });
  const definition = system.define(chart());
  const actor = system.spawn(definition, { name: 'acct' });
  await system.flush();
  let view = actor.inspect();
  assert.equal(view.step.macrostep, 0);
  assert.equal(view.position.at, 'none');
  assert.equal(view.currentEvent, null);
  assert.deepEqual(view.invocations, []);
  const scope = (system.registerIO('debug', { send() {} }), system.openScope('debug', 'debug://test'));
  actor.post('hook', { n: 1 }, { origin: { type: 'debug', scope } });
  await turn();
  view = actor.inspect();
  assert.equal(view.currentEvent.name, 'hook');
  assert.equal(view.currentEvent.origin, 'debug://test');
  assert.deepEqual(view.currentEvent.data, { n: 1 });
  await system.flush();
  const [inspected] = system.definitions();
  assert.deepEqual(inspected.identity, { family: 'account', revision: 'v1' });
  assert.equal(inspected.stateCount, 6);
  system.close();
}

(async () => {
  await holding();
  await budgets();
  await transitions();
  await abandoning();
  await withdrawing();
  await inspection();
  clearTimeout(watchdog);
  console.log('operate.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
