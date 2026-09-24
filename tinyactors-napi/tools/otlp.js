'use strict';

// A small OTLP/HTTP JSON trace exporter, for tests and the otel-tui demo
// only (ADR 0015). It is not part of the package: tinyactors depends on no
// telemetry package, and a user exports with the tracer they already have
// (docs/tracing.md). It uses only fetch, which Node and Bun both have.
//
// Spans are the span-shaped objects TraceSpans (lib/tracing.js) makes.
// The encoding follows the OTLP JSON mapping: IDs are hex strings, 64-bit
// integers and times are decimal strings, enums are numbers.

const SPAN_KIND_INTERNAL = 1;
const STATUS = { unset: 0, ok: 1, error: 2 };

function anyValue(value) {
  if (Array.isArray(value)) {
    return { arrayValue: { values: value.map(anyValue) } };
  }
  switch (typeof value) {
    case 'boolean':
      return { boolValue: value };
    case 'number':
      return Number.isInteger(value) ? { intValue: String(value) } : { doubleValue: value };
    case 'bigint':
      return { intValue: value.toString() };
    default:
      return { stringValue: String(value) };
  }
}

function attributes(object) {
  return Object.entries(object ?? {})
    .filter(([, value]) => value !== undefined)
    .map(([key, value]) => ({ key, value: anyValue(value) }));
}

// Milliseconds since the epoch, as OTLP's nanoseconds, in decimal text.
function nanos(milliseconds) {
  return (BigInt(Math.round(milliseconds * 1000)) * 1000n).toString();
}

function otlpSpan(span) {
  const out = {
    traceId: span.traceId,
    spanId: span.spanId,
    name: span.name,
    kind: SPAN_KIND_INTERNAL,
    startTimeUnixNano: nanos(span.startTime),
    endTimeUnixNano: nanos(span.endTime),
    attributes: attributes(span.attributes),
    events: span.events.map((event) => ({
      timeUnixNano: nanos(event.time),
      name: event.name,
      attributes: attributes(event.attributes),
    })),
    links: span.links.map((link) => ({ traceId: link.traceId, spanId: link.spanId })),
    status: { code: STATUS[span.status?.code] ?? STATUS.unset },
  };
  if (span.parentSpanId !== undefined) {
    out.parentSpanId = span.parentSpanId;
  }
  return out;
}

/** The OTLP ExportTraceServiceRequest, as JSON, for `spans`. */
function exportRequest(spans, { serviceName = 'tinyactors', scope = 'tinyactors' } = {}) {
  return {
    resourceSpans: [
      {
        resource: { attributes: attributes({ 'service.name': serviceName }) },
        scopeSpans: [{ scope: { name: scope }, spans: spans.map(otlpSpan) }],
      },
    ],
  };
}

/**
 * Collects spans (pass `exporter.onSpan` to TraceSpans) and posts them to
 * an OTLP/HTTP collector on flush(). `endpoint` is the collector's base URL;
 * OTEL_EXPORTER_OTLP_ENDPOINT, then http://localhost:4318, by default.
 */
class OtlpExporter {
  #endpoint;
  #serviceName;
  #pending = [];

  constructor({ endpoint = process.env.OTEL_EXPORTER_OTLP_ENDPOINT ?? 'http://localhost:4318',
                serviceName = 'tinyactors' } = {}) {
    this.#endpoint = endpoint.replace(/\/$/, '');
    this.#serviceName = serviceName;
    this.onSpan = (span) => this.#pending.push(span);
  }

  get pending() {
    return this.#pending.length;
  }

  /** Posts every pending span; resolves with how many were sent. */
  async flush() {
    const spans = this.#pending.splice(0);
    if (spans.length === 0) {
      return 0;
    }
    const response = await fetch(`${this.#endpoint}/v1/traces`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(exportRequest(spans, { serviceName: this.#serviceName })),
    });
    if (!response.ok) {
      throw new Error(`OTLP export failed: ${response.status} ${await response.text()}`);
    }
    return spans.length;
  }
}

module.exports = { OtlpExporter, exportRequest };
