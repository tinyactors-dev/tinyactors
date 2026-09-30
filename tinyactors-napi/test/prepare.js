'use strict';

// Prepared actors (core ADR 0089, napi ADR 0032 section 5): prepare() with
// an address or an incumbent, a replaying actor on its own clock that
// crosses no border, and publish(). The input log and system.replay() come
// later (ADR 0032 sections 6 and 7); here the test replays by hand. Runs on
// Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart } = require('..');

function assertCode(run, code) {
  assert.throws(run, (error) => error.code === code);
}

// e1 arms a 5s timeout to itself and logs; `timeout` moves to `timedout`.
const timer = () => statechart({ family: 'timer', revision: 'v1' })
  .data('seen', 0)
  .state('idle', (s) => s.on('e1', (t) => t.target('waiting')
    .assign('seen', ({ data }) => data.seen + 1)
    .log('armed')
    .send('timeout', (send) => send.after(5000))))
  .state('waiting', (s) => s.on('timeout', (t) => t.target('timedout')))
  .state('timedout');

// The live run's inputs, replayed by hand into a new actor at the same
// address. A message sent to a replaying actor is taken at the system's
// time (core ADR 0094), so the manual clock is stepped to each input's
// time: the timeout that fired is dropped as the clock passes it, the chart
// takes the logged timeout, and nothing leaves the replaying actor.
async function replayByHand() {
  const system = createSystem({ clock: 'manual' });
  const logs = [];
  system.setLogSink((record) => logs.push(record.label));
  const definition = system.define(timer());
  const address = { family: 'timer', name: 't' };

  const replay = system.prepare(definition, { address, at: 0 });
  assert.equal(replay.replaying, true);
  assert.equal(replay.published, false);
  await system.flush(); // the bootstrap
  await system.advanceTo(1000);
  await system.send(replay, 'e1');
  assert.equal(replay.inspect().delayedSends.length, 0, 'the timeout is held, not pending');
  await system.advanceTo(6000); // time reaches it: it fired in the live run, and is dropped
  await system.send(replay, 'timeout');
  assert.deepEqual(replay.inspect().activeStates, ['timedout']);
  assert.deepEqual(logs, [], 'the replay wrote no log');

  const published = system.publish(replay, { at: 6000 });
  assert.equal(published.inspect().published, true);
  assert.ok(system.findActor(address).equals(published));
  assert.equal(published.inspect().data.seen, 1);
  assert.equal(published.inspect().delayedSends.length, 0, 'nothing fires twice');
  system.close();
}

// A send still pending when the actor stopped fires once, late, at publish.
async function pendingAtStop() {
  const system = createSystem({ clock: 'manual' });
  const definition = system.define(timer());
  const replay = system.prepare(definition, { address: { family: 'timer', name: 'p' }, at: 0 });
  await system.flush();
  await system.advanceTo(1000);
  await system.send(replay, 'e1');
  await system.advanceTo(10000);
  const published = system.publish(replay, { at: 3000 }); // stopped at 3000, the timeout due at 6000
  await system.flush();
  assert.deepEqual(published.inspect().activeStates, ['timedout'], 'the late timeout was delivered');
  system.close();
}

// prepare() and publish() refuse what they cannot do.
async function refusals() {
  const system = createSystem();
  const definition = system.define(timer());
  const holder = system.spawn(definition, { name: 'h' });
  await system.flush();
  assertCode(() => system.prepare(definition, {}), 'invalid');
  assertCode(() => system.prepare(definition, { incumbent: holder, address: { family: 'timer', name: 'x' } }),
    'invalid');
  assertCode(() => system.prepare(definition, { address: { family: 'other', name: 'x' } }), 'invalid');
  const plain = system.prepare(definition, { address: { family: 'timer', name: 'h' } });
  assert.equal(plain.replaying, false);
  await system.flush();
  assertCode(() => system.publish(plain, { at: 0 }), 'invalid'); // `at` is for a replaying actor
  assertCode(() => system.publish(plain), 'busy'); // h is resident
  holder.destroy();
  const published = system.publish(plain);
  assert.ok(system.findActor({ family: 'timer', name: 'h' }).equals(published));
  system.close();
}

(async () => {
  await replayByHand();
  await pendingAtStop();
  await refusals();
  console.log('prepare.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
