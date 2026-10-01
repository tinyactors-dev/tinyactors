'use strict';

// The recorder (kernel ADR 0006 section 2): setRecorder() hands the events
// the named actors of the recorded families take to the application once
// per turn; a journal of them rebuilds the actors in another system, with
// nothing crossing the border until publish. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart } = require('..');

// A journal keyed by address, the events serialized as an application would.
function journal() {
  const byAddress = new Map();
  const wakes = new Map();
  const batches = [];
  const recorder = (batch) => {
    batches.push(batch);
    for (const { address, event, at } of batch.records) {
      const key = `${address.family}/${address.name}`;
      if (!byAddress.has(key)) byAddress.set(key, []);
      byAddress.get(key).push(JSON.parse(JSON.stringify({ event, at })));
    }
    for (const { address, at } of batch.wakes) {
      wakes.set(`${address.family}/${address.name}`, at);
    }
  };
  return { recorder, byAddress, wakes, batches, events: (key) => byAddress.get(key) ?? [] };
}

// add counts and tells peer/p; arm sets a 5s tick to itself.
const counter = () => statechart({ family: 'counter', revision: 'v1' })
  .data('count', 0)
  .state('idle', (s) => {
    s.on('add', (t) => t
      .assign('count', ({ data, event }) => data.count + event.data.by)
      .send('hello', (send) => send.to('#_actor_peer/p').data(({ data }) => ({ count: data.count }))));
    s.on('arm', (t) => t.target('waiting').send('tick', (send) => send.id('t1').after(5000)));
  })
  .state('waiting', (s) => s.on('tick', (t) => t.target('idle').assign('count', ({ data }) => data.count + 100)));

const peer = () => statechart({ family: 'peer', revision: 'v1' })
  .data('seen', [])
  .state('listening', (s) => s.on('hello', (t) => t.assign('seen', ({ data, event }) => [...data.seen, event.data.count])));

function system(clockAt = 0) {
  const made = createSystem({ clock: 'manual', initialTime: clockAt });
  return { system: made, counter: made.define(counter()), peer: made.define(peer()) };
}

// Events from the host, from another actor and from a timer are recorded
// as the charts saw them, with their times; a journal rebuilds both actors.
async function journalRebuilds() {
  const j = journal();
  const live = system();
  live.system.setRecorder(j.recorder, { families: ['counter', 'peer'] });
  const c = live.system.spawn(live.counter, { name: 'c' });
  const p = live.system.spawn(live.peer, { name: 'p' });
  await live.system.flush();
  await live.system.advanceTo(1000);
  await live.system.send(c, 'add', { by: 2 });
  await live.system.flush();
  await live.system.advanceTo(2000);
  await live.system.send(c, 'arm');
  await live.system.advanceTo(7000);
  await live.system.flush();
  await live.system.send(c, 'add', { by: 1 });
  await live.system.flush();
  assert.equal(c.inspect().data.count, 103);
  assert.deepEqual(p.inspect().data.seen, [2, 103]);

  const counterEvents = j.events('counter/c');
  assert.deepEqual(counterEvents.map(({ event, at }) => [event.name, at]),
    [['add', 1000], ['arm', 2000], ['tick', 7000], ['add', 7000]]);
  assert.deepEqual(counterEvents[0].event, { name: 'add', type: 'external', data: { by: 2 } });
  assert.equal(counterEvents[2].event.sendID, 't1');
  assert.equal(counterEvents[2].event.origin, '#_actor_counter/c');
  const peerEvents = j.events('peer/p');
  assert.deepEqual(peerEvents.map(({ event }) => [event.name, event.origin, event.data]),
    [['hello', '#_actor_counter/c', { count: 2 }], ['hello', '#_actor_counter/c', { count: 103 }]]);
  assert.equal(j.wakes.get('counter/c'), null, 'the tick fired: no wake');
  live.system.close();

  const rebuilt = system(8000);
  const rc = rebuilt.system.prepare(rebuilt.counter, { address: { family: 'counter', name: 'c' }, at: 0 });
  for (const { event, at } of counterEvents) rc.replay(event, at);
  const rp = rebuilt.system.prepare(rebuilt.peer, { address: { family: 'peer', name: 'p' }, at: 0 });
  for (const { event, at } of peerEvents) rp.replay(event, at);
  const counterActor = rebuilt.system.publish(rc);
  const peerActor = rebuilt.system.publish(rp);
  await rebuilt.system.flush();
  assert.equal(counterActor.inspect().data.count, 103);
  assert.deepEqual(peerActor.inspect().data.seen, [2, 103], 'the counter\'s replay sent the peer nothing');
  assert.deepEqual(counterActor.inspect().delayedSends, []);
  rebuilt.system.close();
}

// Wakes: an armed timer's due time, and the batch of a turn before its send() settles.
async function wakesAndOrder() {
  const j = journal();
  const live = system();
  live.system.setRecorder(j.recorder, { families: ['counter'] });
  const c = live.system.spawn(live.counter, { name: 'c' });
  await live.system.flush();
  await live.system.advanceTo(1000);
  await live.system.send(c, 'arm');
  assert.equal(j.events('counter/c').length, 1, 'recorded before send() settled');
  assert.equal(j.wakes.get('counter/c'), 6000);
  // Not recorded: another family's actor, and an unnamed one.
  live.system.spawn(live.peer, { name: 'p' });
  const unnamed = live.system.spawn(live.counter);
  await live.system.send(unnamed, 'add', { by: 1 });
  await live.system.flush();
  assert.deepEqual([...j.byAddress.keys()], ['counter/c']);
  // Eviction stores the wake; destroy removes it.
  await live.system.advanceTo(2000);
  live.system.evict(c);
  await live.system.flush();
  assert.equal(j.wakes.get('counter/c'), 6000);
  const d = live.system.spawn(live.counter, { name: 'd' });
  await live.system.send(d, 'arm');
  assert.equal(j.wakes.get('counter/d'), 7000);
  live.system.destroy(d);
  await live.system.flush();
  assert.equal(j.wakes.get('counter/d'), null);
  live.system.setRecorder(null);
  const e = live.system.spawn(live.counter, { name: 'e' });
  await live.system.send(e, 'add', { by: 1 });
  await live.system.flush();
  assert.equal(j.byAddress.has('counter/e'), false, 'recording stopped');
  live.system.close();
}

// A worker invokes a job and counts its progress; a lost job fails it.
const worker = () => statechart({ family: 'worker', revision: 'v1' })
  .data('progress', 0)
  .state('running', (s) => {
    s.invoke((i) => i.type('job').id('work'));
    s.on('progress', (t) => t.assign('progress', ({ data }) => data.progress + 1));
    s.on('error.communication', (t) => t.target('failed'));
  })
  .state('failed');

// An invocation's events and its error report are recorded, and replay rebuilds them.
async function invocations() {
  const j = journal();
  let reporter;
  const made = createSystem({
    clock: 'manual',
    invokers: { job: { start(_, report) { reporter = report; } } },
  });
  const definition = made.define(worker());
  made.setRecorder(j.recorder, { families: ['worker'] });
  const w = made.spawn(definition, { name: 'w' });
  await made.flush();
  await made.advanceTo(1000);
  reporter.event('progress');
  await made.flush();
  await made.advanceTo(2000);
  reporter.unavailable(new Error('gone'));
  await made.flush();
  assert.deepEqual(w.inspect().activeStates, ['failed']);
  const events = j.events('worker/w');
  assert.deepEqual(events.map(({ event, at }) => [event.name, event.type, event.invocationID, at]), [
    ['progress', 'external', 'work', 1000],
    ['error.communication.invoke.work.unavailable', 'platform', 'work', 2000],
  ]);
  made.close();

  let started = 0;
  const again = createSystem({ clock: 'manual', initialTime: 3000, invokers: { job: { start() { started += 1; } } } });
  const prepared = again.prepare(again.define(worker()), { address: { family: 'worker', name: 'w' }, at: 0 });
  for (const { event, at } of events) prepared.replay(event, at);
  const actor = again.publish(prepared);
  await again.flush();
  assert.equal(actor.inspect().data.progress, 1);
  assert.deepEqual(actor.inspect().activeStates, ['failed']);
  assert.equal(started, 0, 'the replay started no service');
  again.close();
}

// A loader rebuilds an evicted actor from its journal: { replay: { spawn, at, events } }.
async function loaderReplays() {
  const j = journal();
  const live = system();
  live.system.setRecorder(j.recorder, { families: ['counter'] });
  const c = live.system.spawn(live.counter, { name: 'c' });
  await live.system.flush();
  await live.system.send(c, 'add', { by: 5 });
  await live.system.flush();
  live.system.evict(c);
  live.system.setLoader('counter', (address) => ({
    replay: { spawn: live.counter, at: 0, events: j.events(`${address.family}/${address.name}`) },
  }));
  const result = await live.system.send({ family: 'counter', name: 'c' }, 'add', { by: 1 });
  assert.equal(result.status, 'stable');
  const back = live.system.findActor({ family: 'counter', name: 'c' });
  assert.equal(back.inspect().data.count, 6);
  // The journal holds both adds: the one before the eviction and the one after the load.
  assert.deepEqual(j.events('counter/c').map(({ event }) => event.data.by), [5, 1]);
  live.system.close();
}

// A delayed send to another actor (kernel ADR 0006, notes of 2026-10-01):
// its firing is not an event of the sender, but the batch updates the
// sender's wake, so a stored wake never names a send that fired. A rebuild
// before the sender takes another event repeats the send at most once; one
// after another event repeats nothing.
async function delayedSendToAnother() {
  const j = journal();
  const made = createSystem({ clock: 'manual' });
  const relay = made.define(statechart({ family: 'relay', revision: 'v1' })
    .state('idle', (s) => s.on('relay', (t) => t.target('waiting')
      .send('hello', (send) => send.to('#_actor_sink/b').after(5000))))
    .state('waiting', (s) => s.on('note', (t) => t.internal())));
  const sinkDefinition = made.define(statechart({ family: 'sink', revision: 'v1' })
    .data('hellos', 0)
    .state('h', (s) => s.on('hello', (t) => t.assign('hellos', ({ data }) => data.hellos + 1))));
  made.setRecorder(j.recorder, { families: ['relay'] });
  const sink = made.spawn(sinkDefinition, { name: 'b' });
  const r = made.spawn(relay, { name: 'p1' });
  await made.flush();
  await made.advanceTo(1000);
  await made.send(r, 'relay');
  assert.equal(j.wakes.get('relay/p1'), 6000);
  await made.advanceTo(6000);
  await made.flush();
  assert.equal(sink.inspect().data.hellos, 1);
  assert.equal(j.wakes.get('relay/p1'), null, 'the firing updated the sender\'s wake');
  await made.advanceTo(7000);
  made.evict(r);
  await made.flush();
  assert.equal(j.wakes.get('relay/p1'), null, 'the eviction stores no wake: nothing reloads it');

  // A rebuild before relay takes another event repeats the send once.
  const rebuild = async () => {
    const prepared = made.prepare(relay, { address: { family: 'relay', name: 'p1' }, at: 0 });
    for (const { event, at } of j.events('relay/p1')) prepared.replay(event, at);
    const actor = made.publish(prepared);
    await made.flush();
    return actor;
  };
  await made.advanceTo(8000);
  let rebuilt = await rebuild();
  assert.equal(sink.inspect().data.hellos, 2, 'repeated once (at least once)');
  // It takes another event: the next rebuild's clock passes the due time.
  await made.send(rebuilt, 'note');
  made.evict(rebuilt);
  await made.flush();
  rebuilt = await rebuild();
  assert.equal(sink.inspect().data.hellos, 2, 'not repeated after another event');
  assert.deepEqual(rebuilt.inspect().delayedSends, []);
  made.close();
}

// Operator actions (kernel ADR 0006 section 8, open): a jump is not an
// event the chart took by its own transitions, so it is not recorded, and a
// rebuild from the journal does not reproduce it. An application that
// operates on a recorded actor snapshots it afterwards.
async function operatorJumpNotRecorded() {
  const j = journal();
  const made = createSystem({ clock: 'manual' });
  const definition = made.define(counter());
  made.setRecorder(j.recorder, { families: ['counter'] });
  const c = made.spawn(definition, { name: 'c' });
  await made.flush();
  await made.send(c, 'add', { by: 1 }, { transition: { source: 'idle', targets: ['waiting'] } });
  await made.flush();
  assert.deepEqual(c.inspect().activeStates, ['waiting']);
  assert.deepEqual(j.events('counter/c'), [], 'the jump is not in the journal');
  made.close();
}

async function refusals() {
  const live = system();
  assert.throws(() => live.system.setRecorder('nope', { families: ['counter'] }), { code: 'invalid' });
  assert.throws(() => live.system.setRecorder(() => {}), { code: 'invalid' });
  assert.throws(() => live.system.setRecorder(() => {}, { families: [] }), { code: 'invalid' });
  live.system.close();
}

async function main() {
  await journalRebuilds();
  await wakesAndOrder();
  await invocations();
  await loaderReplays();
  await delayedSendToAnother();
  await operatorJumpNotRecorded();
  await refusals();
  console.log('recorder: ok');
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
