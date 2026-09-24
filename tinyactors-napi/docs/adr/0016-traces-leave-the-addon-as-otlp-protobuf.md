# ADR 0016: Traces leave the addon as OTLP protobuf

**Status:** Proposed. It would supersede ADR 0015 points 1 and 3, and relax one constraint the user set for ADR 0015 (see "Constraints"). Research: [`docs/research/trace-handoff-formats.md`](../../../docs/research/trace-handoff-formats.md).
**Date:** 2026-09-24

## Context

ADR 0015 hands JavaScript one array of plain record objects per pump turn,
and `lib/tracing.js` maps them to span-shaped objects. Building those
objects costs nearly everything tracing costs. On the tracing bench, core
writing every record kind runs at 2.13M events/s. Handing the records over
as objects drops that to 0.20M, and mapping them to spans to 0.15M. Each
record costs one `napi_create_object` and ten to twenty property writes.

The user's goal is a format that telemetry tooling reads as it is, so the
JavaScript host only has to move bytes. Their constraints so far:

1. **lib/ depends on no telemetry package,** not even as an optional or
   peer dependency.
2. **tinyactors is a library, and users bring their own exporter.**
3. **The C code does not do the export** (ADR 0015 point 1: "C only
   hooks").

The research note compares four formats and measures a prototype on the
same bench and machine (medians, every record kind):

| Hand-off | Events/s |
|---|---|
| Objects (ADR 0015) | 0.19M |
| Objects mapped to spans | 0.13M |
| A raw copy of the records into one ArrayBuffer | 1.3M |
| **OTLP protobuf written in C** | **0.83M** (458 bytes per event) |

The prototype's bytes, posted by `fetch` and untouched, were accepted by
otel-tui (HTTP 200), which showed the traces with their spans.

## Decision

1. **One format out: OTLP protobuf.** The trace hook receives, per pump
   turn, one `Uint8Array` holding one `ExportTraceServiceRequest`: the spans
   that ended in that turn. It can be posted to any OTLP/HTTP `/v1/traces`
   endpoint with `Content-Type: application/x-protobuf` as it is, written to
   a file, or handed to a user. The hook is
   `setTraceSink(sink, { kinds, unsampled, resource })`. `resource` holds
   the resource attributes (`service.name`, ...), encoded once when the sink
   is installed.

2. **The mapping is lossless and specified once.**
   - A macrostep is a span.
   - A microstep is its child span.
   - Every other record kind is a span event on the step it happened in.
   - Lifecycle records are spans with no duration.

   Every field of every record is an attribute, named with ADR 0067's
   `scxml.*` names, and a table in this ADR's implementation notes lists
   them. Span IDs are `ta_step_key_span_id()`. Parents follow ADR 0067
   section 6: the cause, or a link when `parent: "link"`. The mapping
   `lib/tracing.js` does today is the reference, and its gaps become
   requirements. For example, it drops records that fall outside an open
   macrostep.

3. **C encodes; it does not export.** A portable C module with no
   Node-API, next to `src/traces.c`, does three things: maps records to
   spans, keeps the spans that are still open across turns, and writes
   protobuf with a small hand-written writer. It depends on nothing outside
   the repository. HTTP, retries, batching across turns, gzip and where the
   bytes go all stay in JavaScript and belong to the user.

4. **A trace always has an ID.** When a sink is installed and a delivery or
   spawn carries no `traceparent`, napi starts a trace with a random trace
   ID. Core never generates IDs; the host does. Core then propagates the
   context as usual. This replaces the step-derived trace IDs and the
   unbounded span-to-trace map in `TraceSpans`.

5. **Names come from core, cached per definition.** Event names, state
   IDs, and the transition, action and expression facts of core ADR 0071
   are looked up the first time a definition is seen and kept until it is
   freed. The labels follow ADR 0071 section 3: the host's format.

6. **Values stay out by default.** Event data and assigned values are
   JavaScript values. With `values: true`, the encoder renders them through
   the data model's `value_text` at the end of the turn, cut to a limit.
   That costs one call into JavaScript per value, which is why it is off by
   default, as it is today.

7. **Objects for those who need them: a decoder in lib/.**
   `decodeTraces(bytes)` is a small protobuf reader in `lib/` with no
   dependency. It returns span-shaped objects, with the same shape
   `TraceSpans` produces today, for users who feed an in-process pipeline
   (span processors, exporters that don't speak OTLP) or who check
   properties in JavaScript. The OpenTelemetry JS SDK has no way to take in
   OTLP bytes, so these users need objects. Only those who decode pay for
   them.

8. **`tools/otlp.js` shrinks to a POST.** The test exporter and the
   otel-tui demo post the hook's bytes. The in-process receiver in
   `test/otlp.js` decodes them with `decodeTraces`.

## Alternatives

- **Keep objects (ADR 0015).** It is the slowest by six times, and
  JavaScript still has to encode the spans afterwards.
- **OTLP JSON.** It is the same work in C with a payload two to three times
  larger, and receivers only SHOULD accept it. `JSON.parse` would give
  cheap objects, but so does the decoder in point 7, for those who want
  them. Not measured.
- **OTel Arrow (OTAP).** Its specification is a Draft. It runs only over
  gRPC bidirectional streaming, with dictionary state kept per stream. It
  needs an Arrow IPC writer, and a collector with the contrib receiver in
  front of any viewer; otel-tui doesn't take it. It fits links between
  collectors, not our edge.
- **A custom columnar layout with accessors in JavaScript.** It has the
  cheapest hand-off (1.3M, measured with unpacked structs). But no tool
  reads it, every consumer that wants spans still has to map and encode in
  JavaScript, and every language host has to repeat the layout and the
  mapping. It is a second format to keep in step with core, where OTLP is a
  format others already maintain.
- **Put the encoder in core.** Core is freestanding and has no clock, and
  its sink is already the extension point (ADR 0067). Core ships only
  trivial implementations of its interfaces (ADR 0057).

## Constraints

- **Kept: no telemetry dependency.** The writer and the decoder are
  hand-written, and `package.json` gains nothing.
- **Kept, in a narrower sense: users bring their own exporter.** Users
  choose where the bytes go and how, and can decode them into objects.
  What they can no longer do is receive raw records in JavaScript with no
  span mapping. The OTLP output is the only format. The mapping has to be
  lossless so that nothing is lost by that.
- **Relaxed: "C only hooks".** C now knows OTLP's span model and wire
  format. It still does no export: no transport, no retries, no policy. The
  user should confirm this relaxation, since the proposal rests on it. If
  it is not accepted, the next best design is the custom columnar layout
  (alternative 4), with the span mapping and a protobuf writer in
  JavaScript.

## Relation to other decisions

- **ADR 0015:** points 1 (plain records) and 3 (`TraceSpans` as the output)
  are superseded. Points 2 (the mask decides what is built), 4
  (`traceparent` in and out) and 5 (our own export is for testing) stand.
  Its open point 1 (the cost of objects) is what this ADR answers.
- **Core ADR 0067:** unchanged. The encoder is a sink adapter over its
  records. Its attribute names and span-ID derivation are what the encoder
  writes.
- **Core ADR 0071:** provides the names in point 5. Until it lands,
  transitions, actions and expressions cross as numbers.
- **The host kernel** (`docs/proposals/host-kernel.md`): the encoder has
  no Node-API in it and moves into the kernel when the kernel is extracted,
  so a browser or Python host emits the same bytes. Its clock then comes
  from the kernel's pump-driver interface.

## Consequences

- Tracing costs about four to six times less in napi. Once the encoder is
  optimized (constant keys encoded in advance, names cached, sizes computed
  before writing), the prototype's 0.83M events/s should rise.
- `setTraceSink`'s callback receives bytes, not records. Every current
  caller changes (AGENTS.md: change APIs in place). `TraceSpans` is
  removed. `decodeTraces` takes its place for users who want objects.
- The addon grows by the encoder: a few hundred lines of portable C with
  native tests, which check golden bytes for small charts and round trips
  through `decodeTraces`.
- A verifier written in JavaScript reads decoded spans. The mapping table
  in the implementation notes is its contract.

## Testing (when implemented)

- **Native tests:** golden bytes for small charts, and the encoder's open
  spans across turns (a macrostep that pauses between turns).
- **JavaScript tests, on Node and Bun:**
  - `decodeTraces` of the hook's bytes gives the spans the mapping table
    says, one for one with the records core wrote, with nothing lost;
  - the bytes pass a strict protobuf decode;
  - posting them to the in-process receiver works.
- **Bench:** `test/bench/tracing.js` reports the OTLP hand-off next to
  untraced.
- **By hand:** `mise run otel-demo` shows the demo in otel-tui.
