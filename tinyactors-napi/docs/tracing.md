# Tracing

Core writes trace records (core ADR 0067); the addon hands them to a
function you register, one array per pump turn (ADR 0015). tinyactors
depends on no telemetry package: it gives you plain data, and you connect
it to the tracer you already use.

```js
const { createSystem, TraceSpans } = require('tinyactors-napi');

const system = createSystem();
const spans = new TraceSpans({ onSpan: (span) => exportSomewhere(span) });
system.setTraceSink((records) => spans.write(records));

// A request that came in with a W3C traceparent: every macrostep it
// causes, in any actor and after any timer, lands in its trace.
system.post(actor, 'order', body, { traceparent: request.headers.traceparent });
```

- `setTraceSink(sink, { kinds, unsampled })`: `kinds` lists record kinds
  (`TRACE_KINDS`), every kind by default; core builds no other. Without a
  sink core builds nothing.
- `TraceSpans` makes one span per macrostep, a child span per microstep,
  and span events for conditions, assignments, raises, sends and the rest.
  `parent: "link"` makes each macrostep a trace root linked to its cause
  instead. `values: true` adds values as JSON text (off by default: they
  may hold personal data).
- I/O processors and invokers get `request.traceparent`, naming the step
  that sent or invoked, to pass on to what they call.

## Connecting the OpenTelemetry JS SDK

An example, not a dependency. With `@opentelemetry/api` and an SDK
installed in your application, a span object becomes an OpenTelemetry span
through a span processor's exporter, or directly, for example:

```js
const { trace, context, TraceFlags } = require('@opentelemetry/api');
const tracer = trace.getTracer('tinyactors');

function exportSomewhere(span) {
  const parent = span.parentSpanId === undefined
    ? context.active()
    : trace.setSpanContext(context.active(), {
        traceId: span.traceId, spanId: span.parentSpanId, traceFlags: TraceFlags.SAMPLED, isRemote: true,
      });
  const otel = tracer.startSpan(span.name, { startTime: span.startTime, attributes: span.attributes }, parent);
  for (const event of span.events) otel.addEvent(event.name, event.attributes, event.time);
  otel.end(span.endTime);
}
```

The SDK assigns its own span IDs this way; an exporter that writes spans
as they are (the IDs core derives) keeps parents across processes exact.
`tools/otlp.js` is such an exporter for OTLP/HTTP JSON, used by the tests.

## Looking at a trace in otel-tui

```text
mise exec aqua:ymtdzzz/otel-tui -- otel-tui    # in another terminal
mise run otel-demo
```

The demo sends one order through three actors; its trace shows up under
`POST /orders`. If otel-tui cannot bind its internal metrics port
(8888), start it with `--disable-internal-metrics`.
