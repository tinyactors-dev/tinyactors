'use strict';

const assert = require('node:assert/strict');
const addon = require('..');

const system = addon.createSystem({ clock: 'manual', initialTime: 42 });
assert.equal(system.closed, false);
assert.equal(system.clock, 'manual');
assert.equal(system.time, 42);

system.close();
assert.equal(system.closed, true);
assert.doesNotThrow(() => system.close());
assert.throws(
  () => system.time,
  error => error instanceof addon.TinyactorsError && error.code === 'closed',
);

const independent = addon.createSystem();
assert.equal(independent.closed, false);
independent.close();

assert.throws(
  () => addon.createSystem({ clock: 'realtime' }),
  error => error instanceof addon.TinyactorsError && error.code === 'unsupported',
);
