'use strict';

const assert = require('node:assert/strict');
const addon = require('..');

const system = addon.createSystem({ clock: 'manual' });
const definition = system.define({
  family: 'worker',
  revision: 'v1',
  initial: { target: 'idle' },
  states: [{ id: 'idle' }],
});

const actors = Array.from({ length: 256 }, () => system.spawn(definition));
assert.equal(new Set(actors.map(actor => actor.id)).size, actors.length);
for (const actor of actors) {
  assert.equal(actor.system, system);
  assert.equal(actor.definition, definition);
  assert.equal(actor.destroyed, false);
  const inspection = actor.inspect();
  assert.deepEqual(inspection.activeStates, ['idle']);
  assert.equal(inspection.macrostepInProgress, false);
}

actors[0].destroy();
assert.equal(actors[0].destroyed, true);
actors[0].destroy();
for (let index = 1; index < actors.length; index += 1) {
  actors[index].destroy();
}

system.close();
assert.equal(actors[0].destroyed, true);
assert.throws(
  () => actors[0].inspect(),
  error => error instanceof addon.TinyactorsError && error.code === 'closed',
);
