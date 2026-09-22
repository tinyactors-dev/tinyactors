'use strict';

const assert = require('node:assert/strict');
const addon = require('..');

const populationText = process.env.TINYACTORS_NAPI_E2E_POPULATION ?? '1000';
const population = Number(populationText);
assert(
  Number.isSafeInteger(population) && population > 0,
  'TINYACTORS_NAPI_E2E_POPULATION must be a positive safe integer',
);

const system = addon.createSystem({ clock: 'manual' });
try {
  const definition = system.define({
    family: 'worker',
    revision: 'v1',
    initial: { target: 'idle' },
    states: [
      {
        id: 'idle',
        transitions: [{ event: 'work', target: 'finished' }],
      },
      { id: 'finished', kind: 'final' },
    ],
  });

  const actors = Array.from({ length: population }, () => system.spawn(definition));
  assert.equal(new Set(actors.map(actor => actor.id)).size, actors.length);

  (async () => {
    await system.flush();
    for (const actor of actors) {
      assert.deepEqual(actor.inspect().activeStates, ['idle']);
    }

    for (const actor of actors) actor.post('work');
    await system.flush();

    for (const actor of actors) {
      const inspection = actor.inspect();
      assert.equal(inspection.residency, 'done');
      assert.equal(Object.hasOwn(inspection, 'completion'), true);
      assert.equal(inspection.macrostepInProgress, false);
    }

    system.close();
    assert.equal(system.closed, true);
    assert.throws(
      () => actors[0].inspect(),
      error => error instanceof addon.TinyactorsError && error.code === 'closed',
    );
    assert.throws(
      () => actors[0].post('work'),
      error => error instanceof addon.TinyactorsError && error.code === 'closed',
    );
  })().catch(error => {
    if (!system.closed) system.close();
    throw error;
  });
} catch (error) {
  if (!system.closed) system.close();
  throw error;
}
