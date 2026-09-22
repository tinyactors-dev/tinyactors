'use strict';

const assert = require('node:assert/strict');
const addon = require('..');

const system = addon.createSystem({
  clock: 'manual',
  scheduling: { resumeBudget: 4, messageBudget: 2, maxQueuedMessages: 32 },
});
const definition = system.define({
  family: 'worker',
  revision: 'v1',
  initial: { target: 'idle' },
  states: [
    { id: 'idle', transitions: [{ event: 'work', target: 'finished' }] },
    { id: 'finished', kind: 'final' },
  ],
});
const actors = Array.from({ length: 8 }, () => system.spawn(definition));

for (const actor of actors) actor.post('work');
system.post(actors[0].id, 'work');
assert.deepEqual(actors.map(actor => actor.inspect().activeStates), [
  ['idle'], ['idle'], ['idle'], ['idle'], ['idle'], ['idle'], ['idle'], ['idle'],
]);

(async () => {
  await system.flush();
  for (const actor of actors) {
    const inspection = actor.inspect();
    assert.deepEqual(inspection.activeStates, ['finished']);
    assert.equal(inspection.macrostepInProgress, false);
    assert.equal(inspection.residency, 'resident');
  }
  system.close();
  assert.throws(
    () => actors[0].post('work'),
    error => error instanceof addon.TinyactorsError && error.code === 'closed',
  );
})().catch(error => {
  system.close();
  throw error;
});
