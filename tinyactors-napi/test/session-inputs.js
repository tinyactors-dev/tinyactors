'use strict';

// What core ADR 0089 changes for live actors, as napi ADR 0032 sections 3
// and 4 expose it: SendResult.consumed, EvaluationContext.time, and an
// invocation's error report that waits in its parent's mailbox, behind the
// mail before it, instead of reaching core at once. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart, named } = require('..');

const door = statechart({ family: 'door', revision: 'v1' })
  .state('closed', (s) => s.on('open', (t) => t.target('opened')))
  .state('opened', (s) => s.on('close', (t) => t.target('closed')));

// A message the chart takes is consumed; one it ignores is not.
async function consumed() {
  const system = createSystem();
  const front = system.spawn(system.define(door));
  const ignored = await front.send('close');
  assert.equal(ignored.status, 'stable');
  assert.equal(ignored.consumed, false, 'closed takes no close');
  const taken = await front.send('open');
  assert.equal(taken.consumed, true);
  assert.deepEqual(front.inspect().activeStates, ['opened']);
  assert.equal((await front.send('open')).consumed, false, 'opened takes no open');
  system.close();
}

// context.time is the macrostep's logical time: the manual clock's, when the
// macrostep began.
async function time() {
  const system = createSystem({ clock: 'manual', initialTime: 1000 });
  system.registerFunction('now', function ({ time: now }) { return now; });
  const stamper = statechart({ family: 'stamper', revision: 'v1' })
    .data('at', null)
    .state('idle', (s) => s.on('stamp', (t) => t.assign('at', named('now'))));
  const actor = system.spawn(system.define(stamper));
  await actor.send('stamp');
  assert.equal(actor.data().at, 1000);
  await system.advanceTo(2500);
  await actor.send('stamp');
  assert.equal(actor.data().at, 2500);
  system.close();
}

// An unavailable() report queues behind the mail before it: the parent takes
// `work` first, then the report, never the report inside work's macrostep or
// ahead of it.
async function reportsQueue() {
  const system = createSystem();
  let reporter;
  system.registerInvoker('svc', { start(request, report) { reporter = report; } });
  system.registerFunction('seen', function ({ event }) { return this.seen.concat([event.name]); });
  const parent = statechart({ family: 'parent', revision: 'v1' })
    .data('seen', [])
    .state('waiting', (s) => s
      .invoke((i) => i.type('svc').id('svc'))
      .on('work', (t) => t.assign('seen', named('seen')))
      .on('error.communication.invoke.svc.unavailable', (t) => t.target('lost').assign('seen', named('seen'))))
    .state('lost');
  const actor = system.spawn(system.define(parent));
  await system.flush();
  assert.ok(reporter, 'the service started');
  const work = actor.send('work');
  assert.equal(reporter.unavailable(new Error('gone')), true, 'the report is queued');
  await work;
  await system.flush();
  assert.deepEqual(actor.data().seen, ['work', 'error.communication.invoke.svc.unavailable']);
  assert.deepEqual(actor.inspect().activeStates, ['lost']);
  assert.equal(reporter.unavailable(), false, 'the invocation has ended');
  system.close();
}

(async () => {
  await consumed();
  await time();
  await reportsQueue();
  console.log('session-inputs.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
