'use strict';

// Metrics as plain data (napi ADR 0017 section 7, core ADR 0072).
//
// system.metrics() returns a snapshot: a list of metrics, each with a
// name, a unit, a kind ("sum" for cumulative monotonic counters, "gauge"
// for values that go up and down) and data points of attributes and a
// value. That is the shape OpenTelemetry's observable instruments read, so
// an SDK user registers one observable per metric and reports its points;
// a user without an SDK hands the snapshot to metricsToOTLP() and posts the
// bytes to /v1/metrics. No telemetry dependency either way.
//
// Names follow core ADR 0072 section 6. Attributes never name one actor:
// a metric says how many and where; which ones is inspection's job.

const { TinyactorsError } = require('./errors');

// core ADR 0072's counters: [field, metric name, unit, extra attributes].
const COUNTERS = [
  ['spawned', 'tinyactors.actors.spawned', '{actor}'],
  ['loaded', 'tinyactors.actors.loaded', '{actor}'],
  ['unloaded', 'tinyactors.actors.unloaded', '{actor}'],
  ['finished', 'tinyactors.actors.finished', '{actor}'],
  ['destroyed', 'tinyactors.actors.destroyed', '{actor}'],
  ['events_taken', 'tinyactors.events.taken', '{event}'],
  ['events_ignored', 'tinyactors.events.ignored', '{event}', { 'scxml.event.type': 'external' }],
  ['internal_ignored', 'tinyactors.events.ignored', '{event}', { 'scxml.event.type': 'internal' }],
  ['macrosteps', 'tinyactors.macrosteps', '{step}'],
  ['microsteps', 'tinyactors.microsteps', '{step}'],
  ['errors_execution', 'tinyactors.errors', '{error}', { 'scxml.error.type': 'execution' }],
  ['errors_communication', 'tinyactors.errors', '{error}', { 'scxml.error.type': 'communication' }],
  ['errors_platform', 'tinyactors.errors', '{error}', { 'scxml.error.type': 'platform' }],
  ['sends', 'tinyactors.sends', '{send}'],
  ['sends_delayed', 'tinyactors.sends.delayed', '{send}'],
  ['sends_cancelled', 'tinyactors.sends.cancelled', '{send}'],
  ['delayed_fired', 'tinyactors.timers.fired', '{send}'],
  ['invocations_started', 'tinyactors.invocations.started', '{invocation}'],
  ['invocations_ended', 'tinyactors.invocations.ended', '{invocation}'],
  ['runs_paused', 'tinyactors.runs.paused', '{run}'],
  ['runs_stopped', 'tinyactors.runs.stopped', '{run}'],
];

/**
 * The snapshot system.metrics() returns. `definitions` is a list of
 * { attributes, counters, occupancy: [{ state, actors }] } for the live
 * definitions; `system` is core's totals (freed definitions included);
 * `host` is napi's own view: messages and memory.
 */
function buildSnapshot({ startTime, time, systemName, system, definitions, host }) {
  const metrics = new Map();
  const metric = (name, unit, kind) => {
    let entry = metrics.get(name);
    if (entry === undefined) {
      entry = { name, unit, kind, points: [] };
      metrics.set(name, entry);
    }
    return entry;
  };
  const base = { 'tinyactors.system': systemName };
  for (const definition of definitions) {
    const attributes = { ...base, ...definition.attributes };
    for (const [field, name, unit, extra] of COUNTERS) {
      metric(name, unit, 'sum').points.push({ attributes: { ...attributes, ...extra }, value: definition.counters[field] });
    }
    const occupancy = metric('tinyactors.actors.occupancy', '{actor}', 'gauge');
    for (const { state, actors } of definition.occupancy) {
      occupancy.points.push({ attributes: { ...attributes, 'scxml.state': state }, value: actors });
    }
  }
  metric('tinyactors.messages.queued', '{message}', 'gauge').points.push({ attributes: base, value: host.queued });
  metric('tinyactors.messages.delivered', '{message}', 'sum').points.push({ attributes: base, value: host.delivered });
  metric('tinyactors.messages.dead_lettered', '{message}', 'sum')
    .points.push({ attributes: base, value: host.deadLetters });
  const memory = metric('tinyactors.memory.usage', 'By', 'gauge');
  for (const [category, bytes] of Object.entries(host.memory)) {
    memory.points.push({ attributes: { ...base, 'tinyactors.memory.category': category }, value: bytes });
  }
  return { startTime, time, system, metrics: [...metrics.values()] };
}

// ------------------------------------------------------------------------
// metricsToOTLP: a small protobuf writer for ExportMetricsServiceRequest
// (opentelemetry-proto, metrics/v1), no dependency.

const encoder = new TextEncoder();

class Writer {
  constructor() {
    this.parts = [];
    this.length = 0;
  }

  push(bytes) {
    this.parts.push(bytes);
    this.length += bytes.length;
  }

  varint(value) {
    const out = [];
    let v = BigInt(value);
    do {
      let byte = Number(v & 0x7fn);
      v >>= 7n;
      if (v !== 0n) byte |= 0x80;
      out.push(byte);
    } while (v !== 0n);
    this.push(Uint8Array.from(out));
  }

  tag(field, wire) {
    this.varint((field << 3) | wire);
  }

  bytes(field, bytes) {
    this.tag(field, 2);
    this.varint(bytes.length);
    this.push(bytes);
  }

  string(field, text) {
    this.bytes(field, encoder.encode(String(text)));
  }

  message(field, build) {
    const inner = new Writer();
    build(inner);
    this.bytes(field, inner.finish());
  }

  fixed64(field, value) {
    this.tag(field, 1);
    const out = new Uint8Array(8);
    new DataView(out.buffer).setBigUint64(0, BigInt.asUintN(64, BigInt(value)), true);
    this.push(out);
  }

  finish() {
    const out = new Uint8Array(this.length);
    let at = 0;
    for (const part of this.parts) {
      out.set(part, at);
      at += part.length;
    }
    return out;
  }
}

const nanos = (ms) => BigInt(Math.round(ms)) * 1000000n;

function keyValues(writer, field, attributes) {
  for (const [key, value] of Object.entries(attributes)) {
    writer.message(field, (kv) => {
      kv.string(1, key);
      kv.message(2, (any) => any.string(1, value));
    });
  }
}

function dataPoints(writer, field, metric, startTime, time) {
  for (const point of metric.points) {
    writer.message(field, (p) => {
      if (metric.kind === 'sum') p.fixed64(2, nanos(startTime));
      p.fixed64(3, nanos(time));
      p.fixed64(6, point.value); // as_int (sfixed64)
      keyValues(p, 7, point.attributes);
    });
  }
}

/**
 * The OTLP protobuf bytes of a metrics snapshot: one
 * ExportMetricsServiceRequest, to post to an OTLP/HTTP `/v1/metrics`
 * endpoint with Content-Type application/x-protobuf. `options.resource`
 * holds resource attributes (strings), as setTraceSink's does.
 */
function metricsToOTLP(snapshot, options = {}) {
  if (snapshot === null || typeof snapshot !== 'object' || !Array.isArray(snapshot.metrics)) {
    throw new TinyactorsError('metricsToOTLP(): expected a snapshot from system.metrics()', 'invalid');
  }
  const { resource = {}, version = '' } = options;
  const request = new Writer();
  request.message(1, (resourceMetrics) => {
    resourceMetrics.message(1, (r) => keyValues(r, 1, resource));
    resourceMetrics.message(2, (scopeMetrics) => {
      scopeMetrics.message(1, (scope) => {
        scope.string(1, 'tinyactors');
        if (version !== '') scope.string(2, version);
      });
      for (const metric of snapshot.metrics) {
        scopeMetrics.message(2, (m) => {
          m.string(1, metric.name);
          m.string(3, metric.unit);
          if (metric.kind === 'sum') {
            m.message(7, (sum) => {
              dataPoints(sum, 1, metric, snapshot.startTime, snapshot.time);
              sum.tag(2, 0);
              sum.varint(2); // AGGREGATION_TEMPORALITY_CUMULATIVE
              sum.tag(3, 0);
              sum.varint(1); // is_monotonic
            });
          } else {
            m.message(5, (gauge) => dataPoints(gauge, 1, metric, snapshot.startTime, snapshot.time));
          }
        });
      }
    });
  });
  return request.finish();
}

module.exports = { buildSnapshot, metricsToOTLP, COUNTERS };
