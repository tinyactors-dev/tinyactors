'use strict';

// Step 12: send() and backpressure (ROADMAP step 12; ADRs 0005, 0008,
// 0012, 0014). Runs on Node and Bun.

const assert = require('node:assert/strict');
const { nativeOf } = require('../lib/system');
const { createSystem, statechart, TinyactorsError } = require('..');

async function assertRejects(promise, code, pattern) {
  await assert.rejects(promise, (error) => {
    assert.ok(error instanceof TinyactorsError, `expected TinyactorsError, got ${error}`);
    assert.equal(error.code, code, error.message);
    if (pattern) assert.match(error.message, pattern);
    return true;
  });
}

function assertThrows(fn, code) {
  assert.throws(fn, (error) => error instanceof TinyactorsError && error.code === code);
}

// idle --ping--> idle, idle --work--> busy --done--> idle, idle --stop--> end
// (a top-level final with done data).
const chart = (family = 'worker') =>
  statechart({ family, revision: 'v1' })
    .data('count', 0)
    .state('idle', (s) =>
      s
        .on('ping', (t) => t.target('idle').assign('count', function () { return this.count + 1; }))
        .on('stop', (t) => t.target('end')),
    )
    .final('end', (f) => f.doneData((d) => d.param('count', function () { return this.count; })));

const plain = (family = 'plain') =>
  statechart({ family, revision: 'v1' }).state('idle', (s) => s.on('ping', (t) => t.target('idle')).on('stop', (t) => t.target('end'))).final('end');

// Watchdog: a promise that never settles fails the suite instead of hanging.
const watchdog = setTimeout(() => {
  console.error('send.js: timed out');
  process.exit(1);
}, 30000);

// Completion: stable, then done with the completion data; order is kept.
async function completion() {
  const system = createSystem();
  const actor = system.spawn(system.define(chart()));
  const first = actor.send('ping');
  const second = system.send(actor, 'ping');
  const last = actor.send('stop');
  const results = await Promise.all([first, second, last]);
  assert.equal(results[0].status, 'stable');
  assert.ok(results[0].actor.equals(actor));
  assert.equal(results[1].status, 'stable');
  assert.equal(results[2].status, 'done');
  assert.deepEqual(results[2].completion, { count: 2 });
  assert.equal(system.exists(actor), false, 'finished: "destroy" ran after the promise had its data');
  system.close();
}

// send() resolves once the macrostep its message started has ended: by
// name, with the actor the name resolved to. `until` is gone (kernel ADR
// 0006, napi ADR 0040): post() is the call that only queues.
async function byName() {
  const system = createSystem();
  const named = system.spawn(system.define(plain('named')), { address: { family: 'named', name: 'n' } });
  await system.flush();
  const completed = await system.send({ family: 'named', name: 'n' }, 'ping');
  assert.ok(completed.actor.equals(named), 'the actor the name resolved to');
  assert.equal(completed.status, 'stable');
  assertThrows(() => system.send({ family: 'named', name: 'n' }, 'ping', undefined, { until: 'accepted' }), 'invalid');
  system.close();
}

// Dead letters reject with the reason's code; faults reject with the fault's.
async function rejections() {
  const system = createSystem({ finished: 'retain' });
  const definition = system.define(plain());
  const actor = system.spawn(definition);
  await actor.send('stop');
  await assertRejects(actor.send('ping'), 'done', /dead letter \(done\)/);
  const gone = system.spawn(definition);
  await system.flush();
  gone.destroy();
  await assertRejects(gone.send('ping'), 'not-found');
  const pending = system.spawn(definition);
  const destroyed = pending.send('ping'); // queued behind the bootstrap
  pending.destroy();
  await assert.rejects(destroyed, (error) => error.code === 'not-found' && error.detail.reason === 'destroyed');
  await assertRejects(system.send({ family: 'plain', name: 'nobody' }, 'ping'), 'not-found');
  // A message core refuses quarantines its actor; its send() rejects with the fault's code.
  const bad = system.spawn(definition);
  await system.flush();
  await assertRejects(nativeOf(system).send(bad.slot, bad.generation, 0x7fffffff), 'invalid', /dead letter \(refused\)/);
  assert.equal(bad.inspect().scheduling, 'quarantined');
  system.close();
}

// Abort withdraws a queued message; a delivered one is not undone.
async function abort() {
  const system = createSystem();
  const definition = system.define(chart());
  const actor = system.spawn(definition);
  await system.flush();
  const controller = new AbortController();
  const aborted = actor.send('ping', undefined, { signal: controller.signal });
  controller.abort();
  await assertRejects(aborted, 'aborted');
  await system.flush();
  assert.equal(actor.inspect().data.count, 0, 'the withdrawn message was never delivered');
  assert.equal(system.stats().messages.queued, 0);

  // Already aborted: rejects, and nothing is queued.
  await assertRejects(actor.send('ping', undefined, { signal: controller.signal }), 'aborted');
  assert.equal(system.stats().messages.queued, 0);

  // Aborted after delivery: rejects, the macrostep still ran.
  const late = new AbortController();
  const delivered = actor.send('ping', undefined, { signal: late.signal });
  await new Promise((resolve) => setImmediate(resolve)); // one turn: delivered and run
  late.abort();
  await delivered.then(
    (result) => assert.equal(result.status, 'stable', 'settled before the abort'),
    (error) => assert.equal(error.code, 'aborted'),
  );
  await system.flush();
  assert.equal(actor.inspect().data.count, 1, 'delivery is not undone');
  system.close();
}

// timeout rejects with `timeout`; a quick macrostep beats it.
async function timeout() {
  const system = createSystem();
  const definition = system.define(chart());
  const actor = system.spawn(definition);
  await system.flush();
  const quick = await actor.send('ping', undefined, { timeout: 5000 });
  assert.equal(quick.status, 'stable');
  // A quarantined actor never runs its mail: the send times out, and the message is withdrawn.
  const bad = system.spawn(definition);
  await system.flush();
  nativeOf(system).post(bad.slot, bad.generation, 0x7fffffff);
  await system.flush();
  assert.equal(bad.inspect().scheduling, 'quarantined');
  await assertRejects(bad.send('ping', undefined, { timeout: 20 }), 'timeout');
  assert.equal(system.stats().messages.queued, 0, 'withdrawn');
  assertThrows(() => actor.send('ping', undefined, { timeout: -1 }), 'invalid');
  assertThrows(() => actor.send('ping', undefined, { until: 'later' }), 'invalid');
  system.close();
}

// close() rejects every pending send with `closed`.
async function close() {
  const system = createSystem();
  const definition = system.define(chart());
  const actor = system.spawn(definition);
  const pending = [actor.send('ping'), actor.send('ping'), system.send({ family: 'worker', name: 'x' }, 'ping')];
  system.close();
  for (const promise of pending) {
    await assertRejects(promise, 'closed');
  }
  assertThrows(() => actor.send('ping'), 'closed');
}

// A full pool of pending inputs: post() throws, send() rejects (ADR 0008;
// core ADR 0099: 8 envelopes, 2 of them for admission). The pool holds
// envelopes, not data: what a message's data retains bounds only mail
// between actors (maxRetainedBytes).
async function backpressure() {
  const system = createSystem({ parameters: { poolEnvelopes: 8, admissionWatermark: 25 } });
  const definition = system.define(chart());
  const actor = system.spawn(definition);
  await system.flush();
  actor.post('ping');
  actor.post('ping');
  assertThrows(() => actor.post('ping'), 'queue-full');
  await assertRejects(actor.send('ping'), 'queue-full');
  assert.equal(system.stats().messages.queued, 2);
  await system.flush();
  actor.post('ping', new Uint8Array(100)); // fits again once taken
  await system.flush();
  assertThrows(() => actor.post('ping', 'x', { bytes: -1 }), 'invalid');
  assertThrows(() => createSystem({ parameters: { maxRetainedBytes: -1 } }), 'invalid');
  system.close();
}

// maxRetainedBytes bounds the application's mail (kernel ADR 0003): its
// data counts against it (strings 2 bytes per code unit, other objects 64
// or `bytes`). A send between resident actors stays in core, bounded by
// core's pool (core ADR 0099): the limit never holds it back, so it is
// taken at the next flush even while the application's mail is over it.
async function retainedBytes() {
  const system = createSystem({ parameters: { maxRetainedBytes: 100 } });
  const holder = system.spawn(system.define(plain()));
  const sink = system.spawn(
    system.define(statechart({ family: 'sink', revision: 'v1' }).state('idle', (s) => s.on('pong', (t) => t.target('got'))).state('got')),
    { address: { family: 'sink', name: 'main' } },
  );
  const sender = system.spawn(
    system.define(statechart({ family: 'sender', revision: 'v1' }).state('idle', (s) => s.on('ping', (t) => t.send('pong', (m) => m.to('#_actor_sink/main'))))),
  );
  await system.flush();
  // A quarantined actor collects its mail, so its data stays retained.
  nativeOf(system).post(holder.slot, holder.generation, 0x7fffffff);
  await system.flush();
  assert.equal(holder.inspect().scheduling, 'quarantined');
  holder.post('ping', 'x'.repeat(60)); // 120 bytes: the application's mail is accepted past the limit
  sender.post('ping');
  await system.flush();
  assert.equal(system.stats().messages.overflow, 0, 'the send between actors is not held back');
  assert.equal(sink.inspect().activeStates.at(-1), 'got', 'taken at the flush, once');
  system.release(holder); // its mail is taken
  await system.flush();
  assert.equal(system.stats().messages.overflow, 0);
  system.close();
}

// After warm-up, send() and post() make no allocator call in the addon.
async function steadyState() {
  const system = createSystem();
  const definition = system.define(plain());
  const actors = Array.from({ length: 300 }, () => system.spawn(definition));
  const addonCalls = () => {
    let calls = 0;
    for (const [category, counters] of Object.entries(system.stats().memory)) {
      if (category !== 'total' && category !== 'core') calls += counters.allocations + counters.frees;
    }
    return calls;
  };
  const round = () => Promise.all(actors.map((actor) => actor.send('ping')));
  const posts = () => { for (const actor of actors) actor.post('ping'); return system.flush(); };
  for (let warm = 0; warm < 3; warm += 1) {
    await round();
    await posts();
  }
  const before = addonCalls();
  for (let index = 0; index < 5; index += 1) {
    await round();
    await posts();
  }
  assert.equal(addonCalls(), before, 'no allocator call in the addon');
  system.close();
}

(async () => {
  await completion();
  await byName();
  await rejections();
  await abort();
  await timeout();
  await close();
  await backpressure();
  await retainedBytes();
  await steadyState();
  clearTimeout(watchdog);
  console.log('send.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
