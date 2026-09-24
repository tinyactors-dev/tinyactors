'use strict';

const assert = require('node:assert/strict');
const addon = require('..');
const packageMetadata = require('../package.json');

assert.equal(addon.version, packageMetadata.version);
assert.deepEqual(Object.keys(addon).sort(), [
  'TRACE_KINDS',
  'TinyactorsError',
  'createSystem',
  'decodeTraces',
  'formatTraceparent',
  'literal',
  'parseTraceparent',
  'statechart',
  'stepSpanId',
  'version',
]);
