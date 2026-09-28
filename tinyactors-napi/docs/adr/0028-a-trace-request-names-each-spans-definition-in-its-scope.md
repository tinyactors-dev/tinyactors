# ADR 0028: A trace request names each span's definition in its scope

**Status:** Accepted and implemented (2026-09-28); see the implementation notes for where the code differs from the text (point 2: the facts are captured when a definition's first span is written, not at registration). Refines ADR 0016 (the request's layout and attributes) and ADR 0017 (what the encoder keeps per definition); keeps ADR 0018 (the host's memory).
**Date:** 2026-09-28

## Context

A span says which chart it ran only as `scxml.definition`, the
definition's runtime ID (`src/otlp.c`, the macrostep attributes). That ID
is local to the process and generational (core ADR 0028, ADR 0043): it
means nothing to a backend, and it names another definition once the
first is freed. The chart's family, revision, name and source (the file
or URL its spans point into, core ADR 0085) are not in a span.

Users who want them in their traces have only bad routes:

- **Join with the definition logs.** `tinyactors.definition.registered`
  carries the family, revision and ID, but it is a DEBUG log, rate-limited
  per family (`docs/tracing.md`), and absent with
  `logs: false`. Under invoke churn the log for an ID is often suppressed,
  and a reused ID joins to the wrong chart.
- **Ask the definition.** `Definition.sourceOf()` answers only while the
  definition is alive; a sink that enriches later finds it freed.
- **Set `resource`.** It applies to the whole request and holds strings:
  no use for per-chart facts.
- **Decode and encode again.** napi ships `decodeTraces` and no encoder.
  The documented route (`docs/tracing.md` case 2,
  `test/fixtures/otel-sdk.js`) decodes, builds the OpenTelemetry SDK's
  `ReadableSpan`s and exports them. That round trip loses:

| Lost | Where |
|---|---|
| `schema_url` on the resource and the scope, and scope attributes | `lib/otlp.js` reads only the scope's name and version |
| Span `trace_state` and `flags` | not decoded; the fixture hardcodes `TraceFlags.SAMPLED` |
| Link `trace_state` and `flags`, and all `dropped_*_count` fields | not decoded; the fixture forces them to 0 |
| The bytes type | `bytes_value` decodes to a hex string |
| Integer vs. double | an integral double decodes as the same JavaScript number as an int |
| Unknown or newer fields | skipped |

The encoder knows the definition of every span it writes and can read its
identity from core at no cost that matters. The facts belong in the bytes
it writes.

## Decision

1. **One `ScopeSpans` per definition.** A trace request holds one
   `ScopeSpans` for each definition that has spans in it. Each scope is
   named `tinyactors`, with the addon's version as before, and carries
   these attributes:

   | Attribute | Type | Value |
   |---|---|---|
   | `tinyactors.definition.family` | string | The family |
   | `tinyactors.definition.revision` | string, or bytes when not UTF-8 | The revision, as `DefinitionIdentity` reads it |
   | `tinyactors.definition.id` | int | The runtime ID, as `scxml.definition` on its spans |
   | `tinyactors.definition.name` | string | The chart's name (`StatechartOptions.name`, `<scxml name>`); absent when it has none |
   | `tinyactors.definition.source` | string | The source its first state's span names: the file `statechart()` was called in, or the `name` given to `define()`; absent when it keeps no span |

   Scope attributes are part of OTLP's `InstrumentationScope`, so they
   travel as the protocol defines them, once per definition per request
   rather than once per span.
2. **The facts are captured when the definition is registered,** from the
   definition sink napi already follows (core ADR 0079), and kept until
   the definition is freed and its last span has been written. A
   definition freed in the same turn as its last span is still named.
   Nothing is read or written for a system without a trace sink, or for a
   step that is sampled out (ADR 0017).
3. **Spans keep `scxml.definition`,** so a pipeline that flattens scopes
   still has the ID to join on.
4. **The definition logs gain `tinyactors.definition.name` and
   `tinyactors.definition.source`,** so a log-only consumer sees the same
   facts. Other logs keep their per-record `tinyactors.definition.family`
   and `.revision` and are not regrouped.
5. **`decodeTraces` reads scope attributes:** `DecodedSpan.scope` gains
   `attributes`. Spans of one scope share that object, as they share
   `resource`.
6. **Enrichment belongs in a collector.** `docs/tracing.md` says to add
   data to our traces with an OpenTelemetry Collector's processors, which
   edit OTLP without decoding it into another model. For a backend that
   does not show scope attributes, it shows the collector's `transform`
   processor copying the definition attributes onto each span. The SDK
   route (case 2) stays for pipelines that do not speak OTLP. The SDK's
   instrumentation scope has no attributes, so the fixture copies the
   definition attributes onto each span's attributes.

Not decided here:

- **A per-definition attribute hook** on `setTraceSink` (a function
  answering extra attributes for a definition, called once and cached).
  It would let a host add its own facts without a collector. It waits for
  a user who needs facts beyond the chart's identity and source.
- **A lossless `decodeTraces` and an `encodeTraces`.** Point 1 removes
  the main reason to re-encode. The table above stays the record of what
  the SDK route loses.

## Consequences

- A backend, a collector, or a file of exported bytes says which chart
  each span ran, by family, revision and source, without the process that
  wrote it and after the definition is gone.
- A request with spans from many definitions is larger by one scope
  header and five short attributes per definition. A request from one
  chart grows by one scope's attributes.
- Spans are no longer in end order across the request, only within their
  definition's scope. Parents and links are by ID, so nothing depends on
  the order.
- Backends vary in how they show scope attributes; point 6 is the answer
  for those that show none.

## Implementation notes

- **Where:** `src/otlp.{c,h}`. Finished spans stay in the one `spans`
  buffer, in the order they end, so its capacity rules (ADR 0017's
  `tna_otlp_end_turn`) are unchanged. Each span gets an entry (`at`,
  `len`, next), chained per definition; a run per definition, in the
  order its first span ended, holds its chain, its byte total and its
  encoded `InstrumentationScope` (in `scopes`); an open-addressed index
  finds a run by definition ID. `scope_spans` keeps the running total of
  the `ResourceSpans.scope_spans` fields, so `tna_otlp_traces_size` still
  walks nothing. `tna_otlp_put_traces` writes the resource, then each run's
  header, scope and spans. The entries, runs, index and scopes are freed
  with the `spans` buffer when a burst's buffer is freed. `lib/otlp.js`
  reads scope attributes; `test/fixtures/otel-sdk.js` and
  `docs/tracing.md` copy them onto each span for the SDK, and the docs
  show the collector's `transform` processor.
- **Deviation, point 2: captured with the first span, not at
  registration.** A run's scope is encoded when the definition's first
  span of a request is written. A span is written while the record that
  ends it is handled, and core writes records only for actors that use
  the definition, so the definition is always alive then: a definition
  retired and freed later in the turn is still named, which is what point
  2 asks for. Capturing lazily keeps nothing per definition between
  requests (no copy of every registered definition's facts, no cleanup on
  free), and a system without a sink, or a step sampled out, encodes no
  scope at all (ADR 0017). `src/napi_system.c` and `src/definitions.{c,h}`
  did not change. A span ended after its definition was freed, which core
  never causes, would get a scope with the ID alone.
- **Source:** the source named by the span of state 0, read with
  `ta_definition_span`. A chart without spans (a spec with none, a SCXML
  import without `name`) has no `.source`.
- **Failure:** a record that cannot be encoded for lack of memory is
  dropped as before (`dropped`), and the entries, runs and scopes it added
  are taken back (`tna_runs_rollback`), so the request stays consistent.
- **Tests:** `test/native/otlp_test.c`: two definitions in one request
  make two scopes with their attributes, a non-UTF-8 revision as bytes, a
  chart without a name leaving `.name` out and one without spans leaving
  `.source` out; a definition destroyed, retired and freed before the take
  is still named; an allocation sweep (every allocation from 16 bytes to
  8 KiB refused in turn, with and without warm buffers) leaves every
  request well-formed with each span in exactly one scope, and leaks
  nothing; the golden request has its scope attributes.
  `test/trace-scopes.js` (Node and Bun): the same through `decodeTraces`,
  with a builder chart (`.source` is the test file) and an import named by
  `file:///charts/door.scxml`; spans of one scope share its object and
  carry its `scxml.definition`; the definition logs carry `.name` and
  `.source`. `test/otel-sdk.js` checks the copied attributes.
- **Benchmark** (`mise run bench-compare d281fb4`, 100,000 actors, two
  alternating runs each, medians; `test/bench/tracing.js` gained `every
  record in 100 charts`, the actors spread over 100 definitions):

  | Variant | Node (a99e44a37e2e) ns/event | Node RSS B/actor | Bun (c989bd79a51f) ns/event | Bun RSS B/actor | OTLP B/event |
  |---|---|---|---|---|---|
  | untraced | 478 → 474 (−0.7%) | 751 → 746 | 401 → 403 (+0.6%) | 1,001 → 1,044 | — |
  | sampled out | 485 → 506 (+4.5%) | 1,513 → 1,511 | 445 → 450 (+1.1%) | 1,443 → 1,446 | — |
  | every record | 1,501 → 1,576 (+5.0%) | 1,412 → 1,443 | 1,463 → 1,496 (+2.3%) | 1,574 → 1,566 | 1,110 → 1,110 |
  | every record in 100 charts | 1,588 → 1,571 (−1.1%) | 1,470 → 1,508 | 1,513 → 1,538 (+1.7%) | 1,455 → 1,488 | 1,110 → 1,136 (+2.4%) |
  | summary | 877 → 896 (+2.2%) | 1,507 → 1,508 | 818 → 849 (+3.8%) | 1,447 → 1,453 | 364 → 364 |

  Core and native bytes per actor and allocator calls per event are
  unchanged (native +0.1% to +0.3%). Nothing is outside the 5% rule;
  `every record` on Node sits at its edge. The sampled-out path runs no
  new code, so its +4.5% on Node is the run-to-run spread. With 100
  definitions a request carries 100 scopes: 2.4% more bytes per event.
