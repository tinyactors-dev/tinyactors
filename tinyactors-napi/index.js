'use strict';

const native = require('./lib/native');
const { TinyactorsError } = require('./lib/errors');
const { createSystem } = require('./lib/system');
const { statechart, literal } = require('./lib/statechart');
const { TRACE_KINDS, parseTraceparent, formatTraceparent, stepSpanId } = require('./lib/tracing');
const { decodeTraces } = require('./lib/otlp');
const { metricsToOTLP } = require('./lib/metrics');

module.exports = {
  version: native.version,
  createSystem,
  statechart,
  literal,
  TinyactorsError,
  // Tracing (core ADR 0067, napi ADR 0016): OTLP bytes, and a decoder; no
  // telemetry dependency.
  TRACE_KINDS,
  decodeTraces,
  parseTraceparent,
  formatTraceparent,
  stepSpanId,
  // Metrics (core ADR 0072, napi ADR 0017): system.metrics() and its OTLP bytes.
  metricsToOTLP,
};
