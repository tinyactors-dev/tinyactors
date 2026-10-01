'use strict';

const assert = require('node:assert/strict');
const addon = require('..');
const packageMetadata = require('../package.json');

assert.equal(addon.version, packageMetadata.version);
assert.deepEqual(Object.keys(addon).sort(), [
  'ChartCodecError',
  'EFFECTS',
  'TRACE_KINDS',
  'TinyactorsError',
  'actorURI',
  'content',
  'createSystem',
  'decodeTraces',
  'formatTraceparent',
  'literal',
  'metricsToOTLP',
  'named',
  'parameterInfo',
  'parseTraceparent',
  'serialize',
  'statechart',
  'stepSpanID',
  'version',
]);
