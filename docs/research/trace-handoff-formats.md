# Handing trace records to a host: OTLP protobuf, OTLP JSON, OTel Arrow, a custom columnar layout

**Date:** 2026-09-24
**Question:** In what format should the napi addon hand core's trace
records (core ADR 0067) to JavaScript, so that it costs little and, ideally,
telemetry tooling understands the bytes as they are, so JavaScript only has
to move them?
**Method:** the OTLP and OTel Arrow specifications and repositories (sources
below), and a throwaway prototype in the napi addon, measured with the
tracing bench and checked against otel-tui. The prototype lived in a
discarded worktree; none of it is in the tree.

## Where things stand

napi ADR 0015 turns every record into a JavaScript object in C, one array
per pump turn, and `lib/tracing.js` (`TraceSpans`) maps the objects to
span-shaped objects. `test/bench/tracing.js`, 100,000 actors, one `ping`
with an assignment each (four records per event: BEGIN, MICROSTEP, ASSIGN,
END), Node 26 on an M-series Mac:

| Variant | Events/s |
|---|---|
| Untraced | 2.24M |
| BEGIN and END only | 0.44M |
| Every kind | 0.20M |
| Every kind, mapped to spans | 0.15M |

With the addon's conversion skipped, core writing every kind runs at 2.13M,
so nearly all of the cost is the conversion. Each record costs one
`napi_create_object` and ten to twenty `napi_set_named_property` calls, plus
strings, BigInts and nested objects (`src/napi_trace.c`, `tna_js_record`).

## 1. OTLP protobuf

**The format.** An OTLP/HTTP request is an `ExportTraceServiceRequest`
posted to `/v1/traces` with `Content-Type: application/x-protobuf`, and
optionally `Content-Encoding: gzip`. The same message is used over gRPC.
Traces are stable in the OTLP specification. [1]

The whole schema is small. The prototype needed these fields and nothing
else [2][3]:

- `ExportTraceServiceRequest.resource_spans` (1), the same shape as
  `TracesData`;
- `ResourceSpans`: `resource` (1), `scope_spans` (2);
- `ScopeSpans`: `scope` (1), `spans` (2);
- `Span`: `trace_id` (1, 16 bytes), `span_id` (2, 8 bytes),
  `parent_span_id` (4), `name` (5), `kind` (6), `start_time_unix_nano`
  (7, fixed64), `end_time_unix_nano` (8, fixed64), `attributes` (9),
  `events` (11), `links` (13), `status` (15), `flags` (16, fixed32);
- `Span.Event`: `time_unix_nano` (1), `name` (2), `attributes` (3);
- `KeyValue`: `key` (1), `value` (2); `AnyValue` is a oneof with
  `string_value` (1), `bool_value` (2), `int_value` (3), `double_value`
  (4) and `array_value` (5).

**Can C write it with no dependency?** Yes. Protobuf's wire format needs
four primitives: a varint, a field tag, fixed 64 or 32 bits, and a
length-prefixed field. The one awkward part is that a nested message is
prefixed by its length. The prototype reserved five bytes for each length,
wrote the body, and then moved the body back to fit the length's real size.
The encoder, including the record-to-span mapping below, was about 200
lines of C.

**It works as it is.** The prototype's bytes were posted by `fetch`,
untouched, to otel-tui 0.7.5 on port 4318. otel-tui answered `200` with a
protobuf body and showed the traces with their macrostep and microstep
spans, names, resource and scope. otel-tui runs the OpenTelemetry
Collector's OTLP receiver: gRPC on 4317, HTTP on 4318 [4]. Its README does
not say which HTTP encodings it takes, but protobuf worked.

**Who accepts it.** Protobuf is the encoding every OTLP receiver
implements. The specification says servers SHOULD accept both protobuf and
JSON on the same port [1], so JSON support is the less certain of the two.
The OpenTelemetry Collector, and so otel-tui, takes both.

**What mapping records to spans needs, wherever it lives.** The prototype
and `lib/tracing.js` agree on it:

- **Open spans across batches.** A macrostep span can only be written at
  `MACROSTEP_END`, when its end time and final configuration are known. Its
  microstep spans can be written as each one closes. A macrostep that pauses
  (ADR 0009) or stops (ADR 0070) outside a pump turn stays open across
  batches. So the encoder keeps, for each session with an open macrostep,
  its start, IDs and name, plus the bytes of its open microstep span with
  the span events that have arrived so far. It emits the spans when they
  end.
- **Span IDs** come from `ta_step_key_span_id()`, so every host derives the
  same ones. **Trace IDs** come from the record's context.
  - A macrostep with no context gets an ID that `lib/tracing.js` derives
    from its step, and a macrostep it causes looks that ID up in a map from
    span ID to trace ID. That map, `TraceSpans.#traces`, is never pruned, so
    it grows for the life of the object. A C encoder would need the same map
    and the same bound.
  - The alternative needs no map: when tracing is on, the host starts a
    trace, with a random trace ID, for a delivery that carries no
    `traceparent`. Core then propagates it as it does any context. Core
    itself still generates no IDs.
- **Wall time.** Core's `now` is logical. Span times need the host's
  clock, which the napi sink already reads per record (`timespec_get`).
- **Names.** Event names come from `ta_event_name_format`, and state names
  from `ta_definition_state`. Transitions, actions and expressions come from
  core ADR 0071's lookups once they exist. The prototype looked up state
  names per record. A real encoder would cache names per definition.
- **Values** (event data, assigned values) are JavaScript values behind
  napi's value boxes. C can't render them without calling back into
  JavaScript, for example through the data model's `value_text`.
  `TraceSpans` leaves them out by default, since they may hold personal
  data.

## 2. OTLP JSON

The same messages as JSON, with `Content-Type: application/json`. The
OTLP-specific rules: trace and span IDs are hex strings, not base64; enums
are integers; 64-bit integers are decimal strings [1].

- **Cost:** C would write text instead of binary. That is the same work,
  with bigger output: quoted keys, hex IDs, integers and times as decimal
  strings. We did not measure it. Expect a payload two to three times the
  protobuf one, and a somewhat slower encode.
- **What it buys:** `JSON.parse` gives an in-process consumer objects far
  more cheaply than building them through Node-API calls. It is also
  readable in a log.
- **What it costs:** receivers only SHOULD accept it [1], and the
  specification makes protobuf the primary encoding. `tools/otlp.js`
  already posts JSON to otel-tui, so the Collector's receiver accepts both.

## 3. OTel Arrow (OTAP)

OTAP is the columnar OpenTelemetry protocol, built on Apache Arrow IPC.

- **Status:** its specification says **"Status: Draft"** [5]. The project
  finished Phase 1 (transport) and is completing Phase 2 (a Rust dataflow
  engine). Phase 3, a first stable, production-usable release, is under
  discussion [6][7].
- **Transport:** gRPC bidirectional streaming only
  (`ArrowTracesService.ArrowTraces(stream BatchArrowRecords)`). The
  specification defines no HTTP or unary transport [5].
- **State:** "Servers MUST keep track of Arrow IPC Stream state per gRPC
  Stream and per Payload Type and schema_id". Dictionaries and schemas
  evolve over a stream [5]. A trace batch can hold up to eight Arrow record
  batches: spans, span attributes, events, event attributes, links, link
  attributes, resource attributes and scope attributes [5].
- **Who accepts it:** the Collector-Contrib `otelarrow` receiver and
  exporter, since v0.104.0 (July 2024) [8]. Adapter libraries exist for
  Rust and Go [8]. We found none for JavaScript.

For us this means an Arrow IPC writer and a stateful gRPC stream in C, and
a Collector configured with the contrib receiver in front of any viewer.
otel-tui uses the plain OTLP receiver. OTAP's gains are in compression and
throughput between collectors, not at the edge where our records begin.

## 4. A custom columnar layout

Fixed-size rows in one ArrayBuffer, with IDs and names in pools, read in
JavaScript through `DataView` accessors, as first sketched.

- **Cost of the hand-off:** the prototype copied the turn's
  `tna_trace_entry` structs, about 430 bytes per record, into one
  ArrayBuffer per turn. That is a pessimistic stand-in, since a packed row
  would be about 64 bytes. It ran at **1.3M events/s**, against 0.19M for
  objects on the same machine that day.
- **What it doesn't do:** nothing reads it but our own code. Every consumer
  that wants spans still maps records to spans, in JavaScript. A browser
  host or a Python host would repeat that mapping in its own language, and
  would need the layout defined twice, in C and in each language, and kept
  in step.

## 5. Users who bring their own SDK

The OpenTelemetry JS SDK has no way to take in OTLP bytes. Span processors
and exporters receive `ReadableSpan` objects, which the SDK builds from its
own tracer's spans. `@opentelemetry/otlp-transformer` serializes spans into
protobuf or JSON; we found no public deserializer [9]. So raw OTLP can reach:

- **a collector or backend** directly, by posting to the same OTLP endpoint
  the user's SDK exporter uses. That covers otel-tui, the Collector and the
  vendors with OTLP endpoints. Resource attributes such as `service.name`
  must then be set on our encoder, because the SDK's resource does not
  apply to bytes it never sees;
- **in-process pipelines** (span processors, non-OTLP vendor exporters)
  only as objects. A small protobuf reader in JavaScript can turn our bytes
  into span-shaped objects for users who need them. That is one format with
  a decoder, not a second path from C. It is paid for only by those who
  decode.

## 6. Consumers that are not OpenTelemetry

- **Verifiers** (`In()` properties, transition coverage, happens-before)
  need every record, including its decision, data and position fields.
  OTLP spans with span events can carry all of them as attributes, provided
  the mapping is **lossless**: every record kind becomes a span or a span
  event, and every field an attribute. `TraceSpans` today is not lossless.
  It drops records outside an open macrostep, for example.
- **The test host** (tinyactors-test) takes core's records directly through
  its own C sink (`trace.recording`). It is not affected by napi's format.
- **C hosts** keep core's record sink (ADR 0067) as the primary interface.
  An encoder is an adapter over it.

## 7. Where an encoder would live

- **Not core.** Core is freestanding and has no clock. Telemetry encodings
  are a host concern, and core's sink is already the extension point
  (ADR 0067). Core ships trivial implementations of its interfaces
  (ADR 0057), and an OTLP encoder is not trivial.
- **Host-neutral C, next to the sink.** The mapping (open spans, IDs,
  names) and the protobuf writer depend on core's records, core's lookups
  and a clock. They don't depend on Node-API. In napi today that is a
  portable module like `src/traces.c`, covered by native tests and
  permitted by `test-native`'s Node-API guard. When the host kernel is
  extracted (`docs/proposals/host-kernel.md`), it moves there with the
  scheduler and the router, and a browser or Python host emits the same
  bytes. The clock comes from the kernel's pump-driver interface.

## Prototype results

Same bench and machine; each row is the median of seven rounds of 100,000
events. Other test runs shared the machine, so compare rows with each other,
not with the table above:

| Hand-off (every record kind) | Events/s | Bytes per event |
|---|---|---|
| Untraced | 1.95–2.00M | none |
| Objects (today, ADR 0015) | 0.19M | none |
| Objects mapped to spans by `TraceSpans` | 0.13M | none |
| Raw copy into one ArrayBuffer | 1.29–1.32M | 1716 (unpacked structs) |
| **OTLP protobuf written in C** (spans, events, state names) | **0.83M** | **458** |

The OTLP row does all the work the `TraceSpans` row does and more: it
produces bytes ready to post. It is 6.4 times faster than objects plus
`TraceSpans`, and 4.4 times faster than objects alone. It was not
optimized. It looked up names per record, called `snprintf`, and moved
every nested body once. Constant keys encoded in advance, a name cache per
definition, and computing sizes before writing would all make it cheaper.

## Sources

1. OTLP Specification 1.11.0, OTLP/HTTP: https://opentelemetry.io/docs/specs/otlp/
2. `trace.proto`: https://github.com/open-telemetry/opentelemetry-proto/blob/main/opentelemetry/proto/trace/v1/trace.proto
3. `common.proto`: https://github.com/open-telemetry/opentelemetry-proto/blob/main/opentelemetry/proto/common/v1/common.proto
4. otel-tui README: https://github.com/ymtdzzz/otel-tui
5. OTAP specification (Status: Draft): https://github.com/open-telemetry/otel-arrow/blob/main/docs/otap-spec.md
6. OTel-Arrow Phase 2 announcement: https://opentelemetry.io/blog/2026/otel-arrow-phase-2/
7. OTel Arrow Phase 3 proposal: https://github.com/open-telemetry/otel-arrow/issues/3258
8. otel-arrow README: https://github.com/open-telemetry/otel-arrow
9. `@opentelemetry/otlp-transformer`: https://www.npmjs.com/package/@opentelemetry/otlp-transformer
