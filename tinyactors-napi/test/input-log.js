'use strict';

// The input log (napi ADR 0032 section 6): the inputs core accepted for the
// named actors of the logged families, handed to the sink once per turn;
// the turn's services held until its batch commits; a failed commit that
// quarantines the batch's actors; the dispatch horizon; the durable
// mailbox's queued records and requeue() after a crash. Runs on Node and
// Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart } = require('..');

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

const kinds = (batches) => batches.flatMap((batch) => batch.inputs.map((input) => `${input.kind}:${input.address.name}`));

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

  const records = batches.flatMap((batch) => batch.inputs);
  assert.ok(records.every((input) => input.address.family !== 'unlogged'), 'no record for an unlogged family');
  assert.deepEqual(kinds(batches), [
    'spawned:c1', 'spawned:p1',
    'queued:c1', 'queued:p1', 'event:c1', 'event:p1',
    'queued:c1', 'event:c1', // ping
    'queued:p1', 'event:p1', 'finished:p1', // pong
    'queued:c1', 'event:c1', // arm
    'withdrawn:c1',
    'queued:c1', 'event:c1', 'finished:c1',
  ]);
  const [spawned] = records;
  assert.equal(spawned.revision, 'v1');
  assert.deepEqual(spawned.binding, { n: 5 }, 'the binding as it was at the spawn');
  assert.equal(spawned.slot, undefined, 'no native fields');
  const withdrawn = records.find((input) => input.kind === 'withdrawn');
  assert.equal(withdrawn.sendID, 'tick1');

  // A queued record and its event share a seq; mail between actors keeps its origin.
  const queued = records.filter((input) => input.kind === 'queued');
  const events = records.filter((input) => input.kind === 'event');
  assert.equal(queued.length, events.length);
  for (const input of queued) {
    const event = events.find((candidate) => candidate.seq === input.seq);
    assert.ok(event, `seq ${input.seq} is delivered`);
    assert.equal(event.name, input.name);
    assert.deepEqual(event.data, input.data);
  }
  assert.equal(new Set(queued.map((input) => input.seq)).size, queued.length, 'seqs are unique');
  const ping = events.find((input) => input.name === 'ping');
  assert.equal(ping.origin, '#_actor_pinger/p1');
  assert.equal(ping.originType, 'http://www.w3.org/TR/scxml/#SCXMLEventProcessor');
  assert.deepEqual(events.find((input) => input.name === 'inc').data, { by: 1 });
  assert.ok(batches.every((batch, index) => index === 0 || batch.turn > batches[index - 1].turn), 'batches in turn order');
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
  const report = batches.flatMap((batch) => batch.inputs).find((input) => input.kind === 'report');
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
  const operations = batches.flatMap((batch) => batch.inputs)
    .filter((input) => input.address.name === 's1' && (input.kind === 'abandoned' || input.kind === 'released'))
    .map((input) => input.kind);
  assert.deepEqual(operations, ['abandoned', 'released']);
  system.close();
}

// A turn's services wait for its batch's commit, and run once it resolves.
async function servicesWaitForTheCommit() {
  const sent = [];
  const logs = [];
  let resolve;
  const commits = [];
  const system = createSystem({ io: { out: { send(request) { sent.push(request.event); } } } });
  system.setLogSink((record) => logs.push(record.label));
  system.setInputLog({
    commit(batch) {
      commits.push(batch.turn);
      return new Promise((done) => { resolve = done; });
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
  assert.equal(commits.length, 1, 'one batch commits at a time');
  resolve();
  await new Promise((done) => setImmediate(done));
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

// The horizon: every send due by it has been dispatched, even when a turn's
// dispatch budget leaves some due for the next turn.
async function horizonUnderADispatchBudget() {
  const batches = [];
  const system = createSystem({ clock: 'manual', scheduling: { dueBudget: 1 } });
  system.setInputLog({ commit(batch) { batches.push(batch); } }, { families: ['burst'] });
  const burst = system.spawn(system.define(statechart({ family: 'burst', revision: 'v1' })
    .data('ticks', 0)
    .state('s', (s) => s
      .on('arm', (t) => t
        .send('tick', (send) => send.after(1000))
        .send('tick', (send) => send.after(1000))
        .send('tick', (send) => send.after(1000)))
      .on('tick', (t) => t.assign('ticks', function () { return this.ticks + 1; })))), { name: 'b1' });
  burst.post('arm');
  await system.flush();
  batches.length = 0;
  await system.advanceTo(1000);
  await system.flush();
  const horizons = batches.map((batch) => batch.horizon);
  assert.ok(horizons.includes(999), `a turn that left sends due holds the horizon before them: ${horizons}`);
  assert.equal(horizons.at(-1), 1000, 'once all are dispatched, the horizon is now');
  assert.ok(horizons.every((horizon, index) => index === 0 || horizon >= horizons[index - 1]), 'the horizon never goes back');
  assert.equal(burst.inspect().data.ticks, 3);
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
  const queued = batches.flatMap((batch) => batch.inputs).filter((input) => input.kind === 'queued');
  assert.equal(queued.length, 1);
  assert.deepEqual(queued[0].address, { family: 'counter', name: 'lazy' });
  release();
  await new Promise((done) => setTimeout(done, 5));
  await system.flush();
  const event = batches.flatMap((batch) => batch.inputs).find((input) => input.kind === 'event');
  assert.equal(event.seq, queued[0].seq);
  system.close();
}

// A crash: the first system closes with a message queued and not delivered.
// A new system requeues what has no event, and each message is delivered
// once.
async function requeueAfterACrash() {
  const log = [];
  const first = createSystem({ incarnation: 1 });
  const firstDefinition = first.define(counter());
  first.setInputLog({ commit(batch) { log.push(...batch.inputs); } }, { families: ['counter'] });
  first.setLoader('counter', () => new Promise(() => {})); // never loads: the mail waits
  first.post({ family: 'counter', name: 'c9' }, 'inc', { n: 1 });
  first.post({ family: 'counter', name: 'c9' }, 'inc', { n: 2 });
  await new Promise((done) => setTimeout(done, 5));
  assert.equal(log.filter((input) => input.kind === 'queued').length, 2);
  void firstDefinition;
  first.close(); // the crash

  const delivered = new Set(log.filter((input) => input.kind === 'event').map((input) => input.seq));
  const undelivered = log.filter((input) => input.kind === 'queued' && !delivered.has(input.seq));
  assert.equal(undelivered.length, 2);

  const after = [];
  const second = createSystem({ incarnation: 2 });
  second.setInputLog({ commit(batch) { after.push(...batch.inputs); } }, { families: ['counter'] });
  const actor = second.spawn(second.define(counter()), { name: 'c9' });
  second.requeue(undelivered);
  await second.flush();
  assert.equal(actor.inspect().data.n, 2, 'both messages, once each');
  const requeued = after.filter((input) => input.kind === 'queued');
  assert.equal(requeued.length, 2, 'queued again');
  assert.deepEqual(requeued.map((input) => input.data), [{ n: 1 }, { n: 2 }]);
  assert.equal(after.filter((input) => input.kind === 'event').length, 2);
  second.close();
}

// requeue() keeps an actor's origin, so a reply still reaches its sender.
async function requeueKeepsTheOrigin() {
  const system = createSystem({ incarnation: 0, finished: 'retain' });
  system.setInputLog({ commit() {} }, { families: ['counter', 'pinger'] });
  system.spawn(system.define(counter()), { name: 'c1' });
  const p1 = system.spawn(system.define(pinger()), { name: 'p1' });
  await system.flush();
  system.requeue([{
    kind: 'queued', time: 0, seq: 1, address: { family: 'counter', name: 'c1' }, name: 'ping', type: 'external',
    origin: '#_actor_pinger/p1', originType: 'http://www.w3.org/TR/scxml/#SCXMLEventProcessor',
  }]);
  await system.flush();
  assert.equal(p1.inspect().finalState, 'done', 'the pong reached the pinger');
  assert.throws(() => system.requeue([{ kind: 'event' }]), { code: 'invalid' });
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

  let resolve;
  system.setInputLog({ commit() { return new Promise((done) => { resolve = done; }); } }, { families: ['emitter'] });
  emitter.post('emit');
  await system.flush();
  assert.deepEqual(sent, ['out-event']);
  system.setInputLog(null);
  resolve();
  await new Promise((done) => setImmediate(done));
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
  await horizonUnderADispatchBudget();
  await queuedWhileALoaderRuns();
  await requeueAfterACrash();
  await requeueKeepsTheOrigin();
  await withoutASink();
  console.log('input-log: ok');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
