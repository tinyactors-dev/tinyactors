'use strict';

const native = require('./lib/native');
const { TinyactorsError } = require('./lib/errors');
const { createSystem, actorURI } = require('./lib/system');
const { statechart, literal, content, named, EFFECTS } = require('./lib/statechart');
const { TRACE_KINDS, parseTraceparent, formatTraceparent, stepSpanID } = require('./lib/tracing');
const { decodeTraces } = require('./lib/otlp');
const { ChartCodecError } = require('./lib/codecs');
const { serialize } = require('./lib/values');
const { metricsToOTLP } = require('./lib/metrics');
const { parameterInfo } = require('./lib/params');

module.exports = {
  version: native.version,
  createSystem,
  // A named actor's target URI, percent-encoded as core writes it (core ADR 0096).
  actorURI,
  // System parameters (napi ADR 0035): name → { default, min, max, unit, owner }.
  parameterInfo,
  statechart,
  literal,
  content,
  // A call of a function the system registered (napi ADR 0029).
  named,
  // The I/O processor type effect() sends through (napi ADR 0038).
  EFFECTS,
  // What a value implements to describe itself in text (napi ADR 0031).
  serialize,
  TinyactorsError,
  // Chart codecs (napi ADR 0027): a codec's refusal.
  ChartCodecError,
  // Tracing (core ADR 0067, napi ADR 0016): OTLP bytes, and a decoder; no
  // telemetry dependency.
  TRACE_KINDS,
  decodeTraces,
  parseTraceparent,
  formatTraceparent,
  stepSpanID,
  // Metrics (core ADR 0072, napi ADR 0017): system.metrics() and its OTLP bytes.
  metricsToOTLP,
  // An input log decoded for an audit trail (napi ADR 0034).
};
