'use strict';

// Offloaded actors (core ADR 0074): unload() writes a stable named actor to
// bytes the system keeps, and it comes back from them; and an actor spawned
// again from stored data re-arms the delayed sends stored with it. Runs on
// Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart, literal, TinyactorsError } = require('..');

function assertCode(fn, code) {
  assert.throws(fn, (error) => error instanceof TinyactorsError && error.code === code);
}

// A timer chart: `arm` sets `tick` (id "t") 1000 ms out and `nudge` 2000 ms
// out, with a generated ID kept in `nudgeID`; `stop` cancels "t"; `tick`
// and `nudge` count in `log`.
const timerChart = () => statechart({ family: 'timer', revision: 'v1' })
  .data('log', [])
  .data('nudgeID', null)
  .data('helper', literal(() => 'a function is no value v8.serialize takes'))
  .state('idle', (s) => {
    s.on('arm', (t) => t
      .send('tick', (m) => m.to('#_actor_timer/one').after(1000).id('t'))
      .send('nudge', (m) => m.to('#_actor_timer/one').after(2000).idLocation('nudgeID')));
    s.on('stop', (t) => t.cancel('t'));
    s.on('tick', (t) => t.assign('log', function () { return [...this.log, 'tick']; }));
    s.on('nudge', (t) => t.assign('log', function () { return [...this.log, 'nudge']; }));
    s.on('hit', (t) => t.assign('log', function ({ event }) { return [...this.log, event.data]; }));
  });

const ONE = { family: 'timer', name: 'one' };
const logOf = (system) => system.findActor(ONE).inspect().data.log;

// Both value options round-trip the data, the session and the timers; a
// value v8.serialize refuses (a function) is kept by handle.
async function roundTrip() {
  for (const unloadValues of ['serialize', 'handles']) {
    const system = createSystem({ clock: 'manual', initialTime: 0, unloadValues });
    const actor = system.spawn(system.define(timerChart()), { name: 'one' });
    actor.post('arm');
    actor.post('hit', { deep: [1, 2, { three: 3 }] });
    await system.flush();
    const before = actor.inspect();
    actor.unload();
    assert.equal(system.findActor(ONE), undefined, unloadValues);
    const loaded = system.load(ONE);
    const after = loaded.inspect();
    assert.equal(after.sessionID, before.sessionID, unloadValues);
    assert.deepEqual(after.data.log, [{ deep: [1, 2, { three: 3 }] }], unloadValues);
    assert.equal(after.data.helper(), 'a function is no value v8.serialize takes', unloadValues);
    assert.deepEqual(after.delayedSends.map((send) => [send.id, send.due, send.event]),
      before.delayedSends.map((send) => [send.id, send.due, send.event]), unloadValues);
    await system.advanceTo(2500);
    await system.flush();
    assert.deepEqual(logOf(system).slice(1), ['tick', 'nudge'], unloadValues);
    assertCode(() => system.load(ONE), 'not-found');
    system.close();
  }
}

// With a loader, the loader decides: it hears that the actor is offloaded,
// can refuse ("not now", as during a shutdown), and loads it with
// "offloaded".
async function loaderDecides() {
  const dead = [];
  const system = createSystem({ deadLetter: (letter) => dead.push(letter) });
  const actor = system.spawn(system.define(timerChart()), { name: 'one' });
  await system.flush();
  actor.unload();
  const heard = [];
  system.setLoader('timer', (address, why, { offloaded }) => {
    heard.push({ why, offloaded });
    return { reject: 'shutting down' };
  });
  system.post(ONE, 'hit', 1);
  await system.flush();
  assert.deepEqual(heard, [{ why: 'mail', offloaded: true }]);
  assert.deepEqual(dead.map((letter) => [letter.reason, letter.detail]), [['rejected', 'shutting down']]);
  system.setLoader('timer', () => 'offloaded');
  system.post(ONE, 'hit', 2);
  await system.flush();
  assert.deepEqual(logOf(system), [2]);
  system.close();
}

// Across a restart: the application stored the actor's data and its
// inspect().delayedSends, and a loader spawns it again with them. The
// timers fire at their first due times, a re-armed send is cancelled by its
// ID, and a new generated ID does not reuse a re-armed one.
async function restartKeepsTimers() {
  let stored;
  {
    const system = createSystem({ clock: 'manual', initialTime: 0 });
    const actor = system.spawn(system.define(timerChart()), { name: 'one' });
    actor.post('arm');
    await system.flush();
    const view = actor.inspect();
    stored = { data: structuredClone({ log: view.data.log, nudgeID: view.data.nudgeID }), sends: view.delayedSends };
    assert.equal(stored.sends.length, 2);
    system.close(); // the process exits
  }
  const system = createSystem({ clock: 'manual', initialTime: 500 });
  const timer = system.define(timerChart());
  system.setLoader('timer', () => ({ spawn: timer, binding: stored.data, pendingSends: stored.sends }));
  system.post(ONE, 'hit', 'after restart');
  await system.flush();
  const view = system.findActor(ONE).inspect();
  assert.deepEqual(view.delayedSends.map((send) => [send.id, send.due, send.event]),
    stored.sends.map((send) => [send.id, send.due, send.event]));
  // A new <send> with idlocation gets a generated ID past the re-armed one.
  system.post(ONE, 'arm');
  await system.flush();
  const ids = system.findActor(ONE).inspect().delayedSends.map((send) => send.id);
  assert.equal(new Set(ids).size, ids.length - 1, 'only the author ID "t" repeats');
  // <cancel sendid="t"> finds the re-armed send, and the new one.
  system.post(ONE, 'stop');
  await system.flush();
  await system.advanceTo(1200);
  await system.flush();
  assert.deepEqual(logOf(system), ['after restart']);
  await system.advanceTo(3000);
  await system.flush();
  assert.deepEqual(logOf(system), ['after restart', 'nudge', 'nudge']);
  system.close();
}

(async () => {
  await roundTrip();
  await loaderDecides();
  await restartKeepsTimers();
  console.log('offload.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
