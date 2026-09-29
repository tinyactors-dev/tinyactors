'use strict';

// The input log (napi ADRs 0032 to 0034): the records of what core accepted
// for the named actors of the logged families, in core's portable layout,
// handed to the sink once per turn as { entries: [{ address, bytes }],
// wakes } and read back with readInputLog(); the turn's services held until
// its batch commits; a failed commit that quarantines the batch's actors;
// `fired`, `dropped` and `jump` records; the wakes. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart, readInputLog } = require('..');

// Counts `inc`, answers `ping` with `pong` to its origin, and arms a 5s
// `tick` to itself on `arm`.
const counter = (family = 'counter') => statechart({ family, revision: 'v1' })
  .data('n', 0)
  .state('s', (s) => s
    .on('inc', (t) => t.assign('n', function () { return this.n + 1; }))
    .on('ping', (t) => t.send('pong', (send) => send.to(({ event }) => event.origin)))
    .on('arm', (t) => t.send('tick', (send) => send.id('tick1').after(5000)))
    .on('tick', (t) => t.assign('n', () => -1))
    .on('stop', (t) => t.target('over')))
  .final('over');

// Pings the counter c1 on `go`, and finishes on `pong`.
const pinger = () => statechart({ family: 'pinger', revision: 'v1' })
  .state('p', (s) => s
    .on('go', (t) => t.send('ping', (send) => send.to('#_actor_counter/c1')))
    .on('pong', (t) => t.target('done')))
  .final('done');

// Each batch's entries decoded, one { address, record } per record.
async function decoded(batches) {
  const out = [];
  for (const batch of batches) {
    for (const { address, bytes } of batch.entries) {
      assert.ok(bytes instanceof Uint8Array, 'an entry is bytes');
      for await (const record of readInputLog([bytes])) {
        out.push({ address, record });
      }
    }
  }
  return out;
}
const kinds = async (batches) => (await decoded(batches)).map(({ address, record }) => `${record.kind}:${address.name}`);
const entriesOf = decoded;
const recordsOf = async (batches) => (await decoded(batches)).map((entry) => entry.record);
const wakesOf = (batches, name) => batches.flatMap((batch) => batch.wakes).filter((wake) => wake.address.name === name)
  .map((wake) => wake.at);

// Every kind of record a live run makes, in the order core accepted them,
// for the logged families only.
async function recordsEachKind() {
  const batches = [];
  const system = createSystem({ clock: 'manual', incarnation: 0 });
  system.setInputLog({ commit(batch) { batches.push(batch); } }, { families: ['counter', 'pinger'] });
  const counterDefinition = system.define(counter());
  const other = system.define(counter('unlogged'));
  const c1 = system.spawn(counterDefinition, { name: 'c1', binding: { n: 5 } });
  const p1 = system.spawn(system.define(pinger()), { name: 'p1' });
  const quiet = system.spawn(other, { name: 'q1' });
  system.post(c1, 'inc', { by: 1 });
  system.post(p1, 'go');
  system.post(quiet, 'inc');
  await system.flush();
  system.post(c1, 'arm');
  await system.flush();
  system.withdrawSend(c1, 'tick1');
  system.post(c1, 'stop');
  await system.flush();

  const records = (await recordsOf(batches));
  assert.ok((await entriesOf(batches)).every(({ address }) => address.family !== 'unlogged'), 'no record for an unlogged family');
  for (const { address, record } of await entriesOf(batches)) {
    if (record.kind === 'spawned') {
      assert.deepEqual(record.address, address, 'a spawned record names the entry\'s address');
    }
  }
  // An entry holds one address's records of a turn: each address's own order is core's.
  const of = async (name) => (await kinds(batches)).filter((kind) => kind.endsWith(`:${name}`)).map((kind) => kind.split(':')[0]);
  assert.deepEqual(await of('c1'), [
    'spawned',
    'queued', 'event', // inc
    'queued', 'event', // ping
    'queued', 'event', // arm
    'withdrawn',
    'queued', 'event', 'finished', // stop
  ]);
  assert.deepEqual(await of('p1'), [
    'spawned',
    'queued', 'event', // go
    'queued', 'event', 'finished', // pong
  ]);
  const [spawned] = records;
  assert.equal(spawned.revision, 'v1');
  assert.deepEqual(spawned.binding, { n: 5 }, 'the binding as it was at the spawn');
  assert.equal(spawned.slot, undefined, 'no native fields');
  const withdrawn = records.find((input) => input.kind === 'withdrawn');
  assert.equal(withdrawn.sendID, 'tick1');

  // A queued record and its event share a host number; mail between actors keeps its origin.
  const queued = records.filter((input) => input.kind === 'queued');
  const events = records.filter((input) => input.kind === 'event');
  assert.equal(queued.length, events.length);
  for (const input of queued) {
    const event = events.find((candidate) => candidate.hostSeq === input.hostSeq);
    assert.ok(event, `host number ${input.hostSeq} is delivered`);
    assert.equal(event.name, input.name);
    assert.deepEqual(event.data, input.data);
  }
  assert.equal(new Set(queued.map((input) => input.hostSeq)).size, queued.length, 'host numbers are unique');
  // Each actor's records are numbered from 1, in order.
  for (const name of ['c1', 'p1']) {
    const own = (await entriesOf(batches)).filter(({ address }) => address.name === name).map(({ record }) => Number(record.seq));
    assert.deepEqual(own, own.map((_, index) => index + 1), `${name}'s records are numbered in order`);
  }
  const ping = events.find((input) => input.name === 'ping');
  assert.equal(ping.origin, '#_actor_pinger/p1');
  assert.equal(ping.originType, 'http://www.w3.org/TR/scxml/#SCXMLEventProcessor');
  assert.deepEqual(events.find((input) => input.name === 'inc').data, { by: 1 });
  assert.deepEqual(Object.keys(batches[0]).sort(), ['entries', 'wakes'], 'a batch is entries and wakes');
  assert.deepEqual(wakesOf(batches, 'p1').at(-1), null, 'a finished actor needs no wake');
  assert.deepEqual(wakesOf(batches, 'c1').at(-1), null);
  system.close();
}

// An invocation's error report, a release and an abandon are inputs too.
async function reportsAndOperations() {
  const batches = [];
  let reporter;
  const system = createSystem({
    scheduling: { microstepLimit: 50 },
    invokers: { job: { start(request, report) { reporter = report; } } },
  });
  system.setInputLog({ commit(batch) { batches.push(batch); } }, { families: ['worker', 'spinner'] });
  const worker = system.spawn(system.define(statechart({ family: 'worker', revision: 'v1' })
    .state('running', (s) => {
      s.invoke((i) => i.type('job').id('work'));
      s.on('error.communication.invoke.work.unavailable', (t) => t.target('lost'));
    })
    .state('lost')), { name: 'w1' });
  await system.flush();
  reporter.unavailable({ why: 'gone' });
  await system.flush();
  assert.deepEqual(worker.inspect().activeStates, ['lost']);
  const report = (await recordsOf(batches)).find((input) => input.kind === 'report');
  assert.ok(report, 'the report is recorded');
  assert.equal(report.invokeID, 'work');
  assert.equal(report.reason, 'unavailable');
  assert.equal(report.ends, true);
  assert.ok(report.data !== undefined, 'with the data of the report');

  const spinner = system.spawn(system.define(statechart({ family: 'spinner', revision: 'v1' })
    .data('n', 0)
    .state('idle', (s) => s.on('go', (t) => t.target('spin')))
    .state('spin', (s) => s.always((t) => t.assign('n', function () { return this.n + 1; }).target('spin')))),
  { name: 's1' });
  await system.flush();
  spinner.post('go');
  await system.flush();
  assert.equal(system.inspect(spinner).scheduling, 'quarantined');
  system.abandon(spinner);
  system.release(spinner);
  await system.flush();
  const operations = (await entriesOf(batches))
    .filter(({ address, record }) => address.name === 's1' && (record.kind === 'abandoned' || record.kind === 'released'))
    .map(({ record }) => record.kind);
  // The abandon ended the runaway's macrostep, so the release lifted nothing: no record.
  assert.deepEqual(operations, ['abandoned']);

  // A runaway released without an abandon finishes its macrostep with no limit: `released`.
  const bounded = system.spawn(system.define(statechart({ family: 'spinner', revision: 'v2' })
    .data('n', 0)
    .state('idle', (s) => s.on('go', (t) => t.target('spin')))
    .state('spin', (s) => s.always((t) => t.when(function () { return this.n < 200; })
      .assign('n', function () { return this.n + 1; }).target('spin')))), { name: 's2' });
  await system.flush();
  bounded.post('go');
  await system.flush();
  assert.equal(system.inspect(bounded).scheduling, 'quarantined');
  system.release(bounded);
  await system.flush();
  assert.equal(bounded.inspect().data.n, 200);
  const released = (await entriesOf(batches))
    .filter(({ address, record }) => address.name === 's2' && record.kind === 'released');
  assert.equal(released.length, 1);
  system.close();
}

// A turn's services wait for its batch's commit, and run once it resolves;
// batches commit one at a time.
async function servicesWaitForTheCommit() {
  const sent = [];
  const logs = [];
  const resolvers = [];
  let commits = 0;
  const system = createSystem({ io: { out: { send(request) { sent.push(request.event); } } } });
  system.setLogSink((record) => logs.push(record.label));
  system.setInputLog({
    commit() {
      commits += 1;
      return new Promise((done) => { resolvers.push(done); });
    },
  }, { families: ['emitter'] });
  const emitter = system.spawn(system.define(statechart({ family: 'emitter', revision: 'v1' })
    .state('s', (s) => s.on('emit', (t) => t.log('emitting').send('out-event', (send) => send.via('out').to('x'))))),
  { name: 'e1' });
  emitter.post('emit');
  await system.flush();
  await new Promise((done) => setImmediate(done));
  assert.deepEqual(sent, [], 'the send waits for the commit');
  assert.deepEqual(logs, [], 'the log record waits too');
  assert.equal(commits, 1, 'one batch commits at a time');
  while (sent.length === 0 && resolvers.length > 0) {
    resolvers.shift()();
    await new Promise((done) => setImmediate(done));
  }
  assert.deepEqual(sent, ['out-event']);
  assert.deepEqual(logs, ['emitting']);
  system.close();
}

// A commit that fails stops the system's services and quarantines the
// batch's actors with an "input-log" fault.
async function aFailedCommitFaults() {
  const faults = [];
  const sent = [];
  const system = createSystem({
    fault: (fault) => faults.push(fault),
    io: { out: { send(request) { sent.push(request.event); } } },
  });
  system.setInputLog({ commit() { return Promise.reject(new Error('disk full')); } }, { families: ['emitter'] });
  const emitter = system.spawn(system.define(statechart({ family: 'emitter', revision: 'v1' })
    .state('s', (s) => s.on('emit', (t) => t.send('out-event', (send) => send.via('out').to('x'))))),
  { name: 'e1' });
  emitter.post('emit');
  await system.flush();
  await new Promise((done) => setImmediate(done));
  assert.equal(faults.length, 1);
  assert.equal(faults[0].operation, 'input-log');
  assert.equal(faults[0].code, 'input-log');
  assert.match(faults[0].message, /disk full/);
  assert.equal(faults[0].actor.slot, emitter.slot);
  assert.equal(system.inspect(emitter).scheduling, 'quarantined');
  assert.deepEqual(sent, [], 'nothing the failed batch held runs');
  system.close();
}

// A delayed send the actor dispatches is `fired`, whether or not a
// dispatch budget postpones it, and wakes follow the actor's first pending
// send: its due time while one is pending, null once none is.
async function firedAndWakes() {
  const batches = [];
  const system = createSystem({ clock: 'manual', scheduling: { dueBudget: 1 } });
  system.setInputLog({ commit(batch) { batches.push(batch); } }, { families: ['burst'] });
  const burst = system.spawn(system.define(statechart({ family: 'burst', revision: 'v1' })
    .data('ticks', 0)
    .state('s', (s) => s
      .on('arm', (t) => t
        .send('tick', (send) => send.id('a').after(1000))
        .send('tick', (send) => send.id('b').after(1000))
        .send('tock', (send) => send.to('#_actor_burst/other').id('c').after(3000)))
      .on('tick', (t) => t.assign('ticks', function () { return this.ticks + 1; })))), { name: 'b1' });
  burst.post('arm');
  await system.flush();
  assert.deepEqual(wakesOf(batches, 'b1').at(-1), 1000, 'the first pending send');
  await system.advanceTo(1000);
  await system.flush();
  const fired = (await recordsOf(batches)).filter((record) => record.kind === 'fired').map((record) => record.sendID);
  assert.deepEqual(fired, ['a', 'b'], 'each dispatched send, one turn apart under the budget');
  assert.deepEqual(wakesOf(batches, 'b1').at(-1), 3000, 'then the send to another actor');
  await system.advanceTo(3000);
  await system.flush();
  assert.deepEqual((await recordsOf(batches)).filter((record) => record.kind === 'fired').map((record) => record.sendID),
    ['a', 'b', 'c'], 'a send to another actor is fired too');
  assert.equal(wakesOf(batches, 'b1').at(-1), null, 'nothing pending: no wake');
  assert.equal(burst.inspect().data.ticks, 2);
  system.close();
}

// A message that leaves undelivered is `dropped`: one withdrawn by a send()
// timeout while it waited for a loader, and one buried when its actor
// finished. An operator transition is a `jump`, by state names.
async function droppedAndJump() {
  const batches = [];
  const system = createSystem({ incarnation: 0 });
  const definition = system.define(counter());
  system.setInputLog({ commit(batch) { batches.push(batch); } }, { families: ['counter'] });
  let release;
  system.setLoader('counter', async () => {
    await new Promise((done) => { release = done; });
    return { spawn: definition };
  });
  // A send()'s timeout does not keep the process alive, and nothing else would.
  const alive = setInterval(() => {}, 1000);
  await assert.rejects(system.send({ family: 'counter', name: 'slow' }, 'inc', undefined, { timeout: 5 }),
    (error) => error.code === 'timeout');
  clearInterval(alive);
  await new Promise((done) => setTimeout(done, 5));
  release();
  await new Promise((done) => setTimeout(done, 5));
  await system.flush();
  const slow = (await entriesOf(batches)).filter(({ address }) => address.name === 'slow').map(({ record }) => record);
  const queued = slow.find((record) => record.kind === 'queued');
  const dropped = slow.find((record) => record.kind === 'dropped');
  assert.ok(dropped, 'the withdrawn message is dropped');
  assert.equal(dropped.hostSeq, queued.hostSeq);
  assert.ok(!slow.some((record) => record.kind === 'event'), 'and never delivered');

  const c = system.spawn(definition, { name: 'ender' });
  await system.flush();
  system.post(c, 'stop');
  system.post(c, 'inc');
  await system.flush();
  const ender = (await entriesOf(batches)).filter(({ address }) => address.name === 'ender').map(({ record }) => record);
  const late = ender.filter((record) => record.kind === 'queued')[1];
  assert.ok(ender.some((record) => record.kind === 'dropped' && record.hostSeq === late.hostSeq), 'buried with its actor');

  const j = system.spawn(system.define(statechart({ family: 'counter', revision: 'v2' })
    .state('a', (s) => s.on('x', (t) => t.target('a')))
    .state('b')), { name: 'jumper' });
  await system.flush();
  system.post(j, 'operator.goto', { why: 'test' }, { transition: { targets: ['b'] } });
  await system.flush();
  const jump = (await entriesOf(batches)).find(({ address, record }) => address.name === 'jumper' && record.kind === 'jump');
  assert.ok(jump, 'the operator transition is a jump');
  assert.equal(jump.record.source, undefined, 'from the root');
  assert.deepEqual(jump.record.targets, ['b'], 'by state names');
  system.close();
}

// Mail that waits for a loader is queued under its address, and its event
// carries the same seq once the loader brought the actor.
async function queuedWhileALoaderRuns() {
  const batches = [];
  let release;
  const system = createSystem({ incarnation: 0 });
  const definition = system.define(counter());
  system.setInputLog({ commit(batch) { batches.push(batch); } }, { families: ['counter'] });
  system.setLoader('counter', async () => {
    await new Promise((done) => { release = done; });
    return { spawn: definition };
  });
  system.post({ family: 'counter', name: 'lazy' }, 'inc', { n: 1 });
  await new Promise((done) => setTimeout(done, 5));
  const queued = (await entriesOf(batches)).filter(({ record }) => record.kind === 'queued');
  assert.equal(queued.length, 1);
  assert.deepEqual(queued[0].address, { family: 'counter', name: 'lazy' });
  assert.equal(typeof wakesOf(batches, 'lazy').at(-1), 'number', 'mail it owes needs it back');
  release();
  await new Promise((done) => setTimeout(done, 5));
  await system.flush();
  const event = (await recordsOf(batches)).find((input) => input.kind === 'event');
  assert.equal(event.hostSeq, queued[0].record.hostSeq);
  assert.equal(wakesOf(batches, 'lazy').at(-1), null, 'delivered, and nothing pending');
  system.close();
}

// Without a sink nothing is recorded and nothing waits; removing the sink
// releases what it held.
async function withoutASink() {
  const sent = [];
  const system = createSystem({ io: { out: { send(request) { sent.push(request.event); } } } });
  const emitter = system.spawn(system.define(statechart({ family: 'emitter', revision: 'v1' })
    .state('s', (s) => s.on('emit', (t) => t.send('out-event', (send) => send.via('out').to('x'))))),
  { name: 'e1' });
  emitter.post('emit');
  await system.flush();
  assert.deepEqual(sent, ['out-event'], 'no hold without a sink');

  const resolvers = [];
  system.setInputLog({ commit() { return new Promise((done) => { resolvers.push(done); }); } }, { families: ['emitter'] });
  emitter.post('emit');
  await system.flush();
  assert.deepEqual(sent, ['out-event']);
  system.setInputLog(null);
  while (resolvers.length > 0) {
    resolvers.shift()();
    await new Promise((done) => setImmediate(done));
  }
  assert.deepEqual(sent, ['out-event', 'out-event'], 'the held send runs after its commit');
  emitter.post('emit');
  await system.flush();
  assert.deepEqual(sent, ['out-event', 'out-event', 'out-event'], 'and later ones run at once');

  assert.throws(() => system.setInputLog({}, { families: ['x'] }), { code: 'invalid' });
  assert.throws(() => system.setInputLog({ commit() {} }, { families: [] }), { code: 'invalid' });
  assert.throws(() => system.setInputLog({ commit() {} }), { code: 'invalid' });
  system.close();
}

(async () => {
  await recordsEachKind();
  await reportsAndOperations();
  await servicesWaitForTheCommit();
  await aFailedCommitFaults();
  await firedAndWakes();
  await droppedAndJump();
  await queuedWhileALoaderRuns();
  await withoutASink();
  console.log('input-log: ok');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
