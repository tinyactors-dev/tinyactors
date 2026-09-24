'use strict';

const native = require('./lib/native');
const { TinyactorsError } = require('./lib/errors');
const { createSystem } = require('./lib/system');
const { statechart, literal } = require('./lib/statechart');
const { TRACE_KINDS, TraceSpans, parseTraceparent, formatTraceparent, stepSpanId } = require('./lib/tracing');

module.exports = {
  version: native.version,
  createSystem,
  statechart,
  literal,
  TinyactorsError,
  // Tracing (core ADR 0067, ADR 0015): plain data, no telemetry dependency.
  TRACE_KINDS,
  TraceSpans,
  parseTraceparent,
  formatTraceparent,
  stepSpanId,
};
