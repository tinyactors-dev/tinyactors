# Tracing

Core writes trace records (core ADR 0067). The addon turns them into
OpenTelemetry spans and hands you, once per pump turn, the spans that
ended as **OTLP protobuf**: one `Uint8Array` holding one
`ExportTraceServiceRequest` (napi ADR 0016). Any OTLP/HTTP collector takes
those bytes as they are. tinyactors depends on no telemetry package: where
the bytes go, and how, is yours.

```js
const { createSystem } = require('tinyactors-napi');

const system = createSystem();
system.setTraceSink(
  (bytes) => fetch('http://localhost:4318/v1/traces', {
    method: 'POST', headers: { 'content-type': 'application/x-protobuf' }, body: bytes,
  }),
  { resource: { 'service.name': 'orders' } },
);

// A request that came in with a W3C traceparent: every macrostep it
// causes, in any actor and after any timer, lands in its trace.
system.post(actor, 'order', body, { traceparent: request.headers.traceparent });
```

Batching across turns, retries and compression are up to you; the bytes
of several turns are separate requests.

## Options

`setTraceSink(sink, options)`:

- `kinds`: the record kinds wanted (`TRACE_KINDS`), every kind by default.
  Core builds no other record. Without a sink core builds nothing.
- `unsampled`: also trace macrosteps whose traceparent is not sampled.
- `resource`: resource attributes every request carries, strings only.
- `values`: event data and assigned values as JSON attributes, cut at
  `valueLimit` characters (default 256). Off by default: values may hold
  personal data.
- `parent`: `"cause"` (default) puts a macrostep under the step that
  caused it: the caller's span for a delivery with a traceparent, else the
  sending step. `"link"` makes each macrostep a trace root linked to its
  cause instead.
- `seed`: a bigint. A delivery or spawn without a traceparent starts a new
  trace with a random ID; with a seed the IDs are a fixed sequence, for
  tests.

Replacing or removing a sink ends the spans still open (a macrostep that
paused between turns, say), marked `scxml.incomplete`, and hands them to
the previous sink first.

## What the spans say

- A **macrostep** is a span, `scxml.macrostep <event>`; its **microsteps**
  are child spans, `scxml.microstep <event>` or `(eventless)`.
- Every other record is a **span event** on the step it happened in:
  `scxml.condition`, `scxml.assign`, `scxml.raise`, `scxml.send`,
  `scxml.data`, `scxml.error`, and the rest.
- **Lifecycle** records (`scxml.spawned`, `scxml.finished`, ...) are spans
  of no duration.
- Span IDs are derived from step keys, the same way in every host
  (`stepSpanId`). Transitions and actions are named through the
  definitions: `scxml.transition` reads like
  `idle → busy on order.placed [guarded]`, `scxml.action` like
  `<assign> in onentry of busy`; `scxml.transition.id` and
  `scxml.action.id` keep the compiled IDs.
- I/O processors and invokers get `request.traceparent`, naming the step
  that sent or invoked, to pass on to what they call.

The full attribute table is in napi ADR 0016's implementation notes.

## Objects instead of bytes

`decodeTraces(bytes)` reads the bytes back into span objects (`traceId`,
`spanId`, `parentSpanId`, `name`, times, `attributes`, `events`, `links`,
`status`, `resource`, `scope`), with no dependency. Use it to check
properties in JavaScript, or to feed a pipeline that wants objects.

## With the official OpenTelemetry JS SDK

tinyactors does not depend on the SDK. If your application runs it, you
are in one of two cases.

**1. Both send to the same collector.** This needs no glue. Your SDK
exports your spans; you post the trace sink's bytes to the same OTLP
endpoint. To put both in one trace, pass the active span as the
traceparent:

```js
const { context, trace } = require('@opentelemetry/api');
const { formatTraceparent } = require('tinyactors-napi');

const active = trace.getSpanContext(context.active());
system.post(actor, 'order', body, {
  traceparent: formatTraceparent({ traceId: active.traceId, spanId: active.spanId,
                                   flags: active.traceFlags }),
});
```

Our macrosteps then carry your trace ID and sit under your span, and your
sampling decision carries over through the sampled flag. Your SDK's
resource does not apply to bytes it never sees, so set `resource` on
`setTraceSink`.

**2. Our spans go through the SDK's pipeline** (its processors, or an
exporter that does not speak OTLP). The SDK cannot take in OTLP bytes, so
decode them and hand the SDK `ReadableSpan` objects. This keeps our span
IDs, so parents and links stay exact (recreating the spans with the SDK's
tracer would give them new IDs). The example below is tested in
`test/otel-sdk.js` against pinned SDK versions (`@opentelemetry/api`
1.9.1, `@opentelemetry/sdk-trace-base` 2.11.0,
`@opentelemetry/resources` 2.11.0,
`@opentelemetry/exporter-trace-otlp-http` 0.222.0):

```js
const { decodeTraces } = require('tinyactors-napi');
const { SpanKind, SpanStatusCode, TraceFlags } = require('@opentelemetry/api');
const { resourceFromAttributes } = require('@opentelemetry/resources');

const KIND = { internal: SpanKind.INTERNAL, server: SpanKind.SERVER, client: SpanKind.CLIENT,
               producer: SpanKind.PRODUCER, consumer: SpanKind.CONSUMER };
const STATUS = { unset: SpanStatusCode.UNSET, ok: SpanStatusCode.OK, error: SpanStatusCode.ERROR };
const hrTime = (nanos) => [Number(nanos / 1000000000n), Number(nanos % 1000000000n)];
const context = (traceId, spanId) => ({ traceId, spanId, traceFlags: TraceFlags.SAMPLED, isRemote: false });

function toReadableSpans(bytes) {
  const resources = new Map();
  return decodeTraces(bytes).map((span) => {
    let resource = resources.get(span.resource);
    if (resource === undefined) {
      resource = resourceFromAttributes(span.resource.attributes);
      resources.set(span.resource, resource);
    }
    const own = context(span.traceId, span.spanId);
    return {
      name: span.name,
      kind: KIND[span.kind] ?? SpanKind.INTERNAL,
      spanContext: () => own,
      parentSpanContext: span.parentSpanId === undefined ? undefined : context(span.traceId, span.parentSpanId),
      startTime: hrTime(span.startTimeUnixNano),
      endTime: hrTime(span.endTimeUnixNano),
      duration: hrTime(span.endTimeUnixNano - span.startTimeUnixNano),
      status: { code: STATUS[span.status.code], message: span.status.message },
      attributes: span.attributes,
      links: span.links.map((link) => ({ context: context(link.traceId, link.spanId), attributes: link.attributes })),
      events: span.events.map((event) => ({ name: event.name, time: hrTime(event.timeUnixNano),
                                            attributes: event.attributes })),
      ended: true,
      resource,
      instrumentationScope: { name: span.scope.name, version: span.scope.version },
      droppedAttributesCount: 0, droppedEventsCount: 0, droppedLinksCount: 0,
    };
  });
}

// For example, with the SDK's exporter:
system.setTraceSink((bytes) => exporter.export(toReadableSpans(bytes), () => {}));
```

## Looking at a trace in otel-tui

```text
mise exec aqua:ymtdzzz/otel-tui -- otel-tui    # in another terminal
mise run otel-demo
```

The demo sends one order through three actors; its trace shows up under
`POST /orders`. `OTEL_EXPORTER_OTLP_ENDPOINT` picks another collector. If
otel-tui cannot bind its internal metrics port (8888), start it with
`--disable-internal-metrics`. `tools/otlp.js`, the exporter the demo and
the tests use, is not part of the package.
