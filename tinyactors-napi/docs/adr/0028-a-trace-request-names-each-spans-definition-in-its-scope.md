# ADR 0028: A trace request names each span's definition in its scope

**Status:** Accepted (2026-09-28); not implemented. The target API is in `index.d.ts`, marked `// status: unimplemented`. Refines ADR 0016 (the request's layout and attributes) and ADR 0017 (what the encoder keeps per definition); keeps ADR 0018 (the host's memory).
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

- **Where:** `src/otlp.{c,h}` (per-definition runs of span bytes in a
  request, and a scope header per run in `tna_request_size` and
  `tna_put_request_header`; the definition logs' two attributes),
  `src/napi_system.c` and `src/definitions.{c,h}` (the facts captured at
  registration, dropped after the definition is freed and flushed),
  `lib/otlp.js` (scope attributes), `test/fixtures/otel-sdk.js` and
  `docs/tracing.md` (the SDK route and the collector example).
- **Tests:** `test/otlp.js` and `test/native/otlp_test.c`: a turn with
  spans from two definitions writes two scopes, each with its attributes;
  a non-UTF-8 revision is bytes; a chart with no name and no spans leaves
  those attributes out; a definition retired and freed in the turn of its
  last span is still named; `decodeTraces` returns scope attributes;
  off and sampled-out turns allocate and write nothing more than before.
- **Benchmark:** `test/bench/tracing.js` before and after, with
  `mise run bench-compare <commit>`: throughput and bytes per request for
  one definition and for many, on the same machine.
