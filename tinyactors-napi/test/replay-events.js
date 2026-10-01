'use strict';

// Replaying events (kernel ADR 0006 section 3): PreparedActor.replay(event,
// at) takes an ActorEvent, as the chart saw it, at the time it was taken.
// Nothing the actor does crosses the border until publish(): no mail to
// other actors, no I/O processor, no invoker, no log record. Runs on Node
// and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart } = require('..');

function event(name, data, extra = {}) {
  return { name, type: 'external', data, ...extra };
}

// add counts, tells peer/p, writes an audit record through an I/O
// processor, and logs; arm sets a 5s tick to itself.
const counter = () => statechart({ family: 'counter', revision: 'v1' })
  .data('count', 0)
  .state('idle', (s) => {
    s.on('add', (t) => t
      .assign('count', ({ data, event: e }) => data.count + e.data.by)
      .send('hello', (send) => send.to('#_actor_peer/p'))
      .send('audit', (send) => send.via('audit').data(({ data }) => data.count))
      .log('added'));
    s.on('arm', (t) => t.target('waiting').send('tick', (send) => send.after(5000)));
  })
  .state('waiting', (s) => s.on('tick', (t) => t.target('ticked')))
  .state('ticked');

const peer = () => statechart({ family: 'peer', revision: 'v1' })
  .data('hellos', 0)
  .state('listening', (s) => s.on('hello', (t) => t.assign('hellos', ({ data }) => data.hellos + 1)));

function world() {
  const audits = [];
  const logs = [];
  const system = createSystem({ clock: 'manual' });
  system.registerIO('audit', { send(request) { audits.push(request.data); } });
  system.setLogSink((record) => logs.push(record.label));
  const counterDefinition = system.define(counter());
  const peerActor = system.spawn(system.define(peer()), { name: 'p' });
  return { system, audits, logs, counterDefinition, peerActor };
}

// Replayed events rebuild the state; nothing leaves the actor, before or after publish.
async function crossesNothing() {
  const { system, audits, logs, counterDefinition, peerActor } = world();
  await system.flush();
  await system.advanceTo(3000);
  const address = { family: 'counter', name: 'c' };
  const prepared = system.prepare(counterDefinition, { address, at: 0 });
  const first = prepared.replay(event('add', { by: 2 }), 1000);
  assert.deepEqual(first, { actor: prepared, status: 'stable', consumed: true });
  prepared.replay(event('add', { by: 3 }), 2000);
  assert.equal(prepared.inspect().data.count, 5);
  await system.flush();
  const actor = system.publish(prepared);
  await system.flush();
  assert.ok(system.findActor(address).equals(actor));
  assert.equal(actor.inspect().data.count, 5);
  assert.equal(peerActor.inspect().data.hellos, 0, 'no mail left the replaying actor');
  assert.deepEqual(audits, [], 'no I/O processor was called');
  assert.deepEqual(logs, [], 'no log record was written');

  // Live from here: the next add crosses.
  await system.send(actor, 'add', { by: 1 });
  await system.flush();
  assert.equal(actor.inspect().data.count, 6);
  assert.equal(peerActor.inspect().data.hellos, 1);
  assert.deepEqual(audits, [6]);
  assert.deepEqual(logs, ['added']);
  system.close();
}

// An event the chart has no transition for is not consumed.
async function unconsumed() {
  const { system, counterDefinition } = world();
  const prepared = system.prepare(counterDefinition, { address: { family: 'counter', name: 'c' }, at: 0 });
  assert.equal(prepared.replay(event('nothing'), 0).consumed, false);
  system.close();
}

// A timer due by a later event's time fired: its event is in the journal, and nothing fires twice.
async function timerInJournal() {
  const { system, counterDefinition } = world();
  await system.advanceTo(9000);
  const prepared = system.prepare(counterDefinition, { address: { family: 'counter', name: 'c' }, at: 0 });
  prepared.replay(event('arm'), 1000);
  prepared.replay(event('tick'), 6000);
  const actor = system.publish(prepared);
  await system.flush();
  assert.deepEqual(actor.inspect().activeStates, ['ticked']);
  assert.equal(actor.inspect().delayedSends.length, 0);
  system.close();
}

// A timer due after the last event keeps its due time and fires after publish.
async function timerAfterPublish() {
  const { system, counterDefinition } = world();
  await system.advanceTo(2000);
  const prepared = system.prepare(counterDefinition, { address: { family: 'counter', name: 'c' }, at: 0 });
  prepared.replay(event('arm'), 1000);
  const actor = system.publish(prepared);
  await system.flush();
  assert.deepEqual(actor.inspect().delayedSends.map((send) => send.due), [6000]);
  await system.advanceTo(6000);
  await system.flush();
  assert.deepEqual(actor.inspect().activeStates, ['ticked']);
  system.close();
}

// An invocation's events, its done, and its error report are replayed; the
// invoker is never called while the actor replays.
async function invocations() {
  const started = [];
  const system = createSystem({
    clock: 'manual',
    invokers: { job: { start(request) { started.push(request.invokeID); } } },
  });
  const definition = system.define(
    statechart({ family: 'worker', revision: 'v1' })
      .data('progress', 0)
      .data('result', null)
      .state('running', (s) => {
        s.invoke((i) => i.type('job').id('work'));
        s.on('progress', (t) => t.assign('progress', ({ data }) => data.progress + 1));
        s.on('done.invoke.work', (t) => t.target('finished').assign('result', ({ event: e }) => e.data));
        s.on('error.communication', (t) => t.target('failed'));
      })
      .state('finished')
      .state('failed'),
  );
  await system.advanceTo(5000);
  const done = system.prepare(definition, { address: { family: 'worker', name: 'w1' }, at: 0 });
  done.replay(event('progress', undefined, { invocationID: 'work' }), 1000);
  done.replay(event('done.invoke.work', { total: 1 }, { invocationID: 'work' }), 2000);
  assert.equal(done.inspect().data.progress, 1);
  assert.deepEqual(done.inspect().activeStates, ['finished']);
  const failed = system.prepare(definition, { address: { family: 'worker', name: 'w2' }, at: 0 });
  const report = failed.replay(
    { name: 'error.communication.invoke.work.unavailable', type: 'platform', invocationID: 'work' }, 1000);
  assert.equal(report.consumed, true);
  assert.deepEqual(failed.inspect().activeStates, ['failed']);
  // Still running at publish: the invocation did not survive, and the chart hears it.
  const open = system.prepare(definition, { address: { family: 'worker', name: 'w3' }, at: 0 });
  await system.flush();
  assert.deepEqual(started, [], 'no invoker was called during replay');
  system.publish(done);
  system.publish(failed);
  const published = system.publish(open, { at: 0 });
  await system.flush();
  assert.deepEqual(published.inspect().activeStates, ['failed'], 'an open invocation is retired at publish');
  assert.deepEqual(started, []);
  system.close();
}

// An empty journal: publish() runs the bootstrap first.
async function emptyJournal() {
  const { system, counterDefinition } = world();
  const prepared = system.prepare(counterDefinition, { address: { family: 'counter', name: 'c' }, at: 0 });
  const actor = system.publish(prepared);
  assert.deepEqual(actor.inspect().activeStates, ['idle']);
  system.close();
}

async function refusals() {
  const { system, counterDefinition } = world();
  await system.advanceTo(5000);
  const plain = system.prepare(counterDefinition, { address: { family: 'counter', name: 'p' } });
  assert.throws(() => plain.replay(event('add', { by: 1 }), 0), { code: 'invalid' });
  const prepared = system.prepare(counterDefinition, { address: { family: 'counter', name: 'c' }, at: 0 });
  assert.throws(() => prepared.replay({ type: 'external' }, 0), { code: 'invalid' });
  assert.throws(() => prepared.replay(event('add', { by: 1 }), -1), { code: 'invalid' });
  prepared.replay(event('add', { by: 1 }), 2000);
  assert.throws(() => prepared.replay(event('add', { by: 1 }), 1000), { code: 'invalid' }, 'earlier than the last');
  assert.throws(() => prepared.replay(event('add', { by: 1 }), 6000), { code: 'invalid' }, 'later than now');
  assert.equal(prepared.inspect().data.count, 1);
  system.close();
}

async function main() {
  await crossesNothing();
  await unconsumed();
  await timerInJournal();
  await timerAfterPublish();
  await invocations();
  await emptyJournal();
  await refusals();
  console.log('replay-events: ok');
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
