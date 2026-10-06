'use strict';

// Operating one actor (ADR 0026; core ADRs 0082, 0083, 0084): the inputs
// core holds for it, operator transitions in post() and send() (napi ADR
// 0041: an idle actor with nothing waiting), abandoning a macrostep,
// withdrawing a delayed send, and what inspect() and definitions() report.
// Runs on Node and Bun.

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

// An operator transition needs an idle actor with nothing waiting for it: it
// is `busy` while mail waits, and a refused one is `invalid` with the actor
// not quarantined (napi ADR 0041).
async function transitions() {
  const deadLetters = [];
  const system = createSystem({ clock: 'manual', deadLetter: (record) => deadLetters.push(record) });
  const actor = system.spawn(system.define(chart()));
  await system.flush();
  actor.post('hook', { attempt: 1 });
  actor.post('hook', { attempt: 2 }, { origin: undefined });
  // The inputs core holds, before the pump takes them: oldest first, at most `limit`.
  assert.deepEqual(system.mailbox(actor).map((message) => message.event), ['hook', 'hook']);
  assert.deepEqual(system.mailbox(actor, { limit: 1 }).map((message) => message.data), [{ attempt: 1 }]);
  assertThrows(() => system.mailbox(actor, { limit: -1 }), 'invalid');
  assert.equal(system.mailbox(actor)[0].awaited, false);
  assert.equal(actor.inspect().scheduling, 'ready');
  assertThrows(() => actor.post('operator.goto', { by: 'test' }, { transition: { targets: ['c'] } }), 'busy');
  await assertRejects(actor.send('operator.goto', { by: 'test' }, { transition: { targets: ['c'] } }), 'busy');
  await system.flush();
  assert.deepEqual(actor.inspect().activeStates, ['a']);
  assert.equal(actor.inspect().data.hooks, 2);
  assert.deepEqual(system.mailbox(actor), []);

  const awaited = actor.send('hook');
  assert.equal(system.mailbox(actor)[0].awaited, true, 'a send() waits for it');
  await awaited;

  const jumped = actor.send('operator.goto', { by: 'test' }, { transition: { targets: ['c'] } });
  assert.equal((await jumped).status, 'stable');
  assert.deepEqual(actor.inspect().activeStates, ['c']);
  assert.deepEqual(actor.inspect().data.entered, ['operator.goto'], 'onentry sees the operator event');

  // From b, but the actor is in c: refused at the call.
  await assertRejects(actor.send('operator.goto', undefined, { transition: { targets: ['d'], source: 'b' } }), 'invalid');
  assert.equal(actor.inspect().scheduling, 'idle', 'not quarantined');
  assert.deepEqual(actor.inspect().activeStates, ['c']);
  assert.equal(deadLetters.length, 0, 'a refused transition is not a dead letter');

  assertThrows(() => actor.post('operator.goto', undefined, { transition: { targets: ['nowhere'] } }), 'invalid');
  assertThrows(() => actor.post('operator.goto', undefined, { transition: { targets: [] } }), 'invalid');
  assertThrows(() => system.post({ family: 'account', name: 'x' }, 'operator.goto', undefined,
                                 { transition: { targets: ['c'] } }), 'invalid');
  system.close();
}

// Abandoning a macrostep that never ends: the send() rejects with
// `abandoned`, and the actor takes its next message.
async function abandoning() {
  const system = createSystem({ clock: 'manual' });
  const actor = system.spawn(system.define(chart()));
  await system.flush();
  const runaway = actor.send('spin');
  let view;
  for (let index = 0; index < 20; index += 1) {
    await turn();
    view = actor.inspect();
    if (view.step.microstep > 0) break;
  }
  assert.equal(view.macrostepInProgress, true);
  assert.ok(view.step.macrostep > 0 && view.step.microstep > 0, 'inspection names the step');
  assert.equal(view.position.at, 'none', 'the system\'s budgets pause it between microsteps');
  assertThrows(() => actor.post('operator.goto', undefined, { transition: { targets: ['d'] } }), 'busy');
  system.abandon(actor);
  await assertRejects(runaway, 'abandoned');
  await system.flush();
  actor.post('operator.goto', undefined, { transition: { targets: ['d'] } });
  await system.flush();
  assert.deepEqual(actor.inspect().activeStates, ['d'], 'the next message, a jump, was taken');
  assertThrows(() => system.abandon(actor), 'invalid');
  system.close();

  // Inside a microstep core refuses (core ADR 0082): an action budget of 1
  // pauses this loop inside its microsteps.
  const tight = createSystem({ clock: 'manual', parameters: { actionBudget: 1 } });
  const spinner = tight.spawn(tight.define(chart()));
  await tight.flush();
  spinner.post('spin');
  for (let index = 0; index < 20 && !spinner.inspect().microstepInProgress; index += 1) await turn();
  view = spinner.inspect();
  assert.equal(view.microstepInProgress, true);
  assert.equal(view.position.at, 'enter');
  assert.equal(view.position.state, 'loop1');
  assertThrows(() => tight.abandon(spinner), 'busy');
  tight.close();
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
  // `spin` does not end by itself, so a budget of one microstep leaves its macrostep paused in the first turn;
  // the limit of three ends it (core ADR 0099 section 7) once the test has looked.
  const system = createSystem({ clock: 'manual', parameters: { resumeBudget: 1, microstepLimit: 3 } });
  const definition = system.define(chart());
  const actor = system.spawn(definition, { name: 'acct' });
  await system.flush();
  let view = actor.inspect();
  assert.equal(view.step.macrostep, 0);
  assert.equal(view.position.at, 'none');
  assert.equal(view.currentEvent, null);
  assert.deepEqual(view.invocations, []);
  const scope = (system.registerIO('debug', { send() {} }), system.openScope('debug', 'debug://test'));
  actor.post('spin', { n: 1 }, { origin: { type: 'debug', scope } });
  await turn(); // core admits the message
  await turn(); // and the next turn runs it
  view = actor.inspect();
  assert.equal(view.currentEvent.name, 'spin');
  assert.deepEqual(view.currentEvent.origin,
                   { kind: 'io', scope: { index: scope.index, generation: scope.generation }, type: 'debug', uri: 'debug://test' });
  assert.deepEqual(view.currentEvent.data, { n: 1 });
  await system.flush();
  const [inspected] = system.definitions();
  assert.deepEqual(inspected.identity, { family: 'account', revision: 'v1' });
  assert.equal(inspected.stateCount, 6);
  system.close();
}

(async () => {
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
