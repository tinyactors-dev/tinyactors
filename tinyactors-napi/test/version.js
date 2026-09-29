'use strict';

const assert = require('node:assert/strict');
const addon = require('..');
const packageMetadata = require('../package.json');

assert.equal(addon.version, packageMetadata.version);
assert.deepEqual(Object.keys(addon).sort(), [
  'ChartCodecError',
  'DAPReader',
  'TRACE_KINDS',
  'TinyactorsError',
  'content',
  'createDebugSession',
  'createSystem',
  'decodeTraces',
  'encodeDAPMessage',
  'formatTraceparent',
  'literal',
  'metricsToOTLP',
  'named',
  'parameterInfo',
  'parseTraceparent',
  'readInputLog',
  'serialize',
  'statechart',
  'stepSpanID',
  'version',
]);
