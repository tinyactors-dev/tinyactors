'use strict';

const assert = require('node:assert/strict');
const addon = require('..');
const native = require('../build/tinyactors_napi.node');
const packageMetadata = require('../package.json');

// Step 1: the native module exports only its version (ROADMAP step 1).
assert.deepEqual(Object.keys(native), ['version']);
assert.deepEqual(Object.keys(addon), ['version']);
assert.equal(addon.version, packageMetadata.version);
