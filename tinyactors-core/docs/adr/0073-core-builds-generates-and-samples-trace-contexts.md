# ADR 0073: Core builds, generates and samples trace contexts

**Status:** Accepted and implemented (2026-09-25); see the implementation notes for where the code differs from the text. Refines ADR 0067 (section 6: core now creates trace IDs when a host asks, and the sink's sampling flag becomes masks). Serves napi ADR 0017 and core ADR 0072. `b9ba91c` reads a traceparent's IDs eight digits at a time (33 to 12.5 ns per parse). Refined by ADR 0098: the masks are set with `ta_system_set_trace_masks`, not with a sink, and are refused without a `tel` ring.
**Date:** 2026-09-25

## Context

ADR 0067 made core copy W3C trace contexts and never create or interpret
them, so that replay stays deterministic. Everything else about trace
contexts was left to hosts, and napi did it in JavaScript: a regular
expression parsed `traceparent`, three BigInts crossed into C, and a
cryptographic generator made a trace ID for every delivery without one.
The review in `docs/research/observability-build-vs-reuse.md` measured
that at about 494 ns per `post` with a `traceparent`, and about 120 ns
per `post` without one while a sink is installed, against 418 ns for the
whole untraced delivery.

Every host needs the same pieces: parse and write `traceparent`, make
trace IDs, decide sampling the way other OpenTelemetry processes do, and
mark the deliveries an operator is watching (napi ADR 0017 section 3).
The user asked for these to be one module in core, so hosts share one
implementation instead of each writing its own.

## Decision

### 1. `traceparent` text, parsed and written by core

```c
typedef struct { ta_str text; TA_OUT ta_trace_parent *out; } ta_traceparent_parse_params;
TA_API ta_result ta_traceparent_parse(TA_IN const ta_traceparent_parse_params *params);

typedef struct {
    ta_trace_parent parent;
    TA_BUFFER(capacity, needed) TA_OPTIONAL char *buffer;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *needed;
} ta_traceparent_format_params;
TA_API ta_result ta_traceparent_format(TA_IN const ta_traceparent_format_params *params);
```

- **Parsing follows W3C Trace Context.** The text is
  `version-traceid-parentid-flags`, lowercase hex. Version `ff` is
  invalid. Version `00` is exactly 55 characters. A higher version is
  read by its first 55 characters when the 56th is `-` or the text ends
  there. An all-zero trace ID or parent ID is invalid. Spaces and tabs
  around the text (HTTP optional whitespace) are ignored. Anything else is
  `TA_ERR_INVALID`, and the host starts a new trace, as the spec says a
  receiver does.
- **What it produces.** The trace ID and flags go into the context. The
  parent ID becomes a host cause: `session` 0 and `macrostep` holding the
  span ID, as ADR 0067 section 6 already defines.
- **Writing** produces version `00`, the context's trace ID, the span ID
  of the cause (`ta_step_key_span_id`), and the context's W3C flags. It
  follows the buffer convention: `*needed` is 56, NUL included. A parent
  with no trace ID or no cause is `TA_ERR_INVALID`.

### 2. Trace IDs and span IDs from a generator the system keeps

```c
TA_API void ta_system_seed_trace_ids(ta_system *system, uint64_t seed);
TA_API ta_trace_context ta_system_new_trace(ta_system *system);
TA_API uint64_t ta_system_new_span_id(ta_system *system);
```

- **xoshiro256\*\*, seeded through SplitMix64.** The seed is one 64-bit
  value, and a given seed gives the same sequence on every host.
- **Core never reads the OS.** It has no source of entropy and stays
  deterministic, as ADR 0067 requires. A host seeds each system once,
  from its OS's random bytes in production or a fixed value in tests.
  Until a host seeds it, the system uses seed 0, so an unseeded system is
  still deterministic, but it repeats across processes.
- **The spec asks for random IDs, not cryptographic ones.** OpenTelemetry
  requires random trace and span IDs, and W3C Trace Context Level 2 asks
  that the rightmost 56 bits be random. A new trace sets the `random`
  trace flag (`0x02`) to say so. Neither ID is ever zero.
- **A new trace is sampled by the system's sampler** (section 3). Its
  cause is empty, since nothing caused it.

### 3. A ratio sampler that agrees with other OpenTelemetry processes

```c
TA_API uint64_t ta_trace_ratio_threshold(uint32_t parts, uint32_t whole);
TA_API bool ta_trace_sampled_at(const ta_trace_context *context, uint64_t threshold);
TA_API void ta_system_set_trace_ratio(ta_system *system, uint32_t parts, uint32_t whole);
```

- **The algorithm is OpenTelemetry's consistent probability sampling,**
  the one `TraceIdRatioBased` is specified to use with W3C Level 2 random
  trace IDs. The randomness R is the trace ID's rightmost 56 bits. The
  rejection threshold is T = round((1 − ratio) × 2^56). A trace is
  sampled when R ≥ T. So ratio 1 samples everything, ratio 0 nothing, and
  every process that uses the same ratio decides the same way for the same
  trace.
- **The ratio is a fraction, `parts` / `whole`,** so every host gets the
  same threshold without floating point, computed exactly in 64-bit
  arithmetic. `parts` above `whole` counts as 1, and a `whole` of 0 as 0.
  A host with a floating-point ratio scales it, for example to parts per
  10^9.
- **The system's ratio decides only for traces it starts**
  (`ta_system_new_trace`). A parsed `traceparent` keeps its caller's
  decision (parent-based sampling). A host that wants to override that
  sets the flag itself.
- **The default ratio is 1,** so a host that sets nothing traces
  everything it starts, as today.

### 4. A detail mark on the trace context, and masks on the sink

```c
typedef struct { uint64_t trace_id_high; uint64_t trace_id_low; uint8_t flags; uint8_t detail; } ta_trace_context;
#define TA_TRACE_FLAG_SAMPLED ((uint8_t)0x01u)
#define TA_TRACE_FLAG_RANDOM  ((uint8_t)0x02u)
#define TA_TRACE_DETAIL_WATCHED ((uint8_t)0x01u)

typedef struct {
    void *self;
    const ta_trace_sink_vtable *v;
    uint32_t kinds;            /* a sampled macrostep, or one with no trace ID */
    uint32_t watched_kinds;    /* a sampled macrostep whose context is marked WATCHED */
    uint32_t unsampled_kinds;  /* a macrostep whose trace is not sampled */
} ta_trace_sink TA_ADAPTER;
```

- **`detail` is local, not W3C.** It never appears in `traceparent`
  text, so it does not cross a process boundary. Core copies it wherever
  it copies the context: internal events, sends, delayed sends,
  invocations and the bootstrap of invoked actors. So everything a
  watched delivery causes in this system is watched too.
- **The sink picks one mask per macrostep,** at `MACROSTEP_BEGIN`, as
  ADR 0067 section 2 already does. A sampled context (or one with no
  trace ID) gets `watched_kinds` when it is marked WATCHED, otherwise
  `kinds`. An unsampled context gets `unsampled_kinds`. That is how
  "summary for everyone, full detail for watched actors" works: `kinds`
  holds the summary kinds and `watched_kinds` every kind.
- **`unsampled_kinds` replaces ADR 0067's `bool unsampled`,** which ADR
  0072 asks for: a host that exports logs sets it to the failure kinds,
  so every failure is written even when traces are sampled low. The old
  `unsampled = true` is `unsampled_kinds = kinds`.
- **Lifecycle records** are outside macrosteps. They are written when
  their bit is in `kinds` or `watched_kinds`, sampled or not, as before.
- **The null sink** has all three masks 0.

### 5. Inspection

- **IDs as text.** `ta_trace_id_format` writes a context's trace ID as 32
  lowercase hex digits, and `ta_span_id_format` writes a span ID as 16,
  both following the buffer convention.
- **The step key behind a span ID is not recoverable.** Span IDs are a
  mix of the key (ADR 0067), not an encoding of it. Where a key matters,
  records carry it: every record has `key`, and every caused step has
  `cause`. A host cause is the one case that inverts:
  `ta_step_key_span_id` returns its `macrostep` unchanged.

## Alternatives

- **Keep it in each host.** That is what napi had, and it cost more than
  half of an untraced delivery. Every host would repeat the same parsing,
  generation and sampling, and could disagree on sampling decisions.
- **Put `detail` in the W3C flags.** Trace flags travel to other
  processes and vendors, and W3C reserves the bits we don't own.
- **A generator per call site or per actor.** A per-system generator is
  one state of 32 bytes, and matches the one thread that runs a system.
- **Cryptographic IDs.** The spec does not require them, and a host that
  wants them can still build a context itself and hand it to core.

## Consequences

- `ta_trace_context` gains `detail`, and `ta_trace_sink` loses
  `unsampled` and gains two masks. Every caller moves (AGENTS.md).
- `ta_system` grows by the generator's 32 bytes and the sampler's
  threshold, 8 bytes. Nothing is added per actor or per event.
- napi passes `traceparent` to core as text, seeds each system once, and
  keeps no parser or generator of its own on the hot path.
  `parseTraceparent` and `formatTraceparent` stay in `lib/` as
  conveniences for users.

## Testing

- Golden round trips of `traceparent`, including the W3C examples, and a
  table of invalid inputs: wrong length, uppercase hex, version `ff`,
  all-zero IDs, missing dashes, and a longer future version.
- The generator's first values for fixed seeds, so every host can check
  it produces the same sequence.
- The sampler at ratios 0, 1 and 0.5 on boundary trace IDs (R = T − 1
  and R = T), and the fraction sampled over many generated IDs.
- A watched delivery gets `watched_kinds` records, and what it causes
  (an internal event, a send to another actor, a delayed send) is watched
  too, while an unwatched delivery in the same system gets `kinds`.
- An unsampled macrostep writes exactly `unsampled_kinds`.

## Implementation notes

- **The ratio is an integer fraction, not a `double`.** The API extractor
  and both binding generators have no floating-point type, and adding one
  was out of scope. `ta_trace_ratio_threshold(parts, whole)` and
  `ta_system_set_trace_ratio(system, parts, whole)` compute T exactly, as
  round((whole − parts) × 2^56 / whole), in 64-bit arithmetic. napi passes
  parts per 10^9. Section 3's text was revised while this ADR was being
  implemented, before it was built.
- **Lifecycle records follow the watch mark.** A lifecycle record's kind is
  looked up in `watched_kinds` when its context is marked watched and in
  `kinds` otherwise, not in both. Otherwise an unwatched spawn would write
  its lifecycle record whenever `watched_kinds` asked for every kind.
- **Golden values** are pinned in `tests/trace_test.c`. For seed 0, the
  first trace ID is `99ec5f36cb75f2b4bf6e1f784956452a` and the next span
  ID is `1a5f849d4933e6e0`. They were checked against an independent
  implementation of xoshiro256** over SplitMix64.
- **Cost:** `ta_system` grows by 40 bytes (the generator's 32 and the
  threshold's 8). Nothing is added per actor or per event. On napi (Node 22), a
  `post` with a `traceparent` costs 19 ns more than one without, down from
  494 ns when JavaScript parsed it (napi ADR 0017 section 2).
