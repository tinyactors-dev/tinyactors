'use strict';

const assert = require('node:assert/strict');
const addon = require('..');

const system = addon.createSystem({ clock: 'manual' });
const definition = system.define({
  family: 'worker',
  revision: 'v1',
  initial: { target: 'idle' },
  states: [
    { id: 'idle', transitions: [{ event: 'work', target: 'finished' }] },
    { id: 'finished', kind: 'final' },
  ],
});

assert.equal(definition.system, system);
assert.equal(typeof definition.id, 'bigint');
assert.deepEqual(definition.identity, { family: 'worker', revision: 'v1' });
assert.equal(definition.retired, false);
definition.retire();
assert.equal(definition.retired, true);

const bytes = system.define({
  family: 'bytes',
  revision: new Uint8Array([1, 2, 3]),
  states: [{ id: 'only' }],
});
assert.deepEqual([...bytes.identity.revision], [1, 2, 3]);
bytes.retire();

assert.throws(
  () => system.define({
    family: 'worker',
    revision: 'v2',
    name: 'unsupported',
    states: [{ id: 'idle' }],
  }),
  error => error instanceof addon.TinyactorsError && error.code === 'unsupported',
);

system.close();
assert.throws(
  () => definition.retired,
  error => error instanceof addon.TinyactorsError && error.code === 'closed',
);
