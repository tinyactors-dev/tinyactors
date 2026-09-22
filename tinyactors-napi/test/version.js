'use strict';

const assert = require('node:assert/strict');
const addon = require('..');
const packageMetadata = require('../package.json');

assert.deepEqual(Object.keys(addon), ['version']);
assert.equal(addon.version, packageMetadata.version);
