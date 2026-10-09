# ADR 0006: Tracing is a core feature

**Status:** Accepted (2026-10-07). Carries out ROADMAP "Next" items 1 and 2.
**Date:** 2026-10-07

## Context

Every application built on tinyactors needs to see what its actors did:
which event a macrostep took, which transitions fired, what was raised,
sent and assigned, and which actor's step caused which. The reference
application is granary (`~/projects/granary`): it ran on
`@tinyactors/node` 0.1, its integration tests assert only on spans
(granary ADR 0007), one HTTP request is one trace across several actors
(ADR 0234), and its ADRs 0042 and 0062 record the span vocabulary with
real captured spans. Granary also had to add, by decoding, enriching and
encoding the spans again, things tinyactors should give it: an actor's
name, the chart's name and the kind of a span (ADR 0155).

The standalone core (ADR 0004) has no telemetry. What 0.1 learned is
recorded in `docs/research/trace-handoff-formats.md`: core writing every
record kind cost about 5% of throughput, and turning records into
JavaScript objects cost the rest (2.24M events/s untraced, 0.20M with
every kind).

Only core sees sends between actors inside a system: `ta_system_deliver`
pushes self, parent, child and session routes straight into a mailbox, and
`<raise>` and `#_internal` never leave the kernel. A host cannot propagate
a trace it cannot see, so propagation and the records belong in core.

## Decisions

### 1. Building blocks, one job each

```
            base ──────────────── + journal: lossy ring of variable-size records
           ▲    ▲                   (knows nothing about traces)
       image    trace  (new, freestanding)
        ▲  ▲     ▲  ▲   values: ta_trace_id, ta_span_id, ta_step_key,
        │  │     │  │   ta_trace_context, traceparent parse/format,
        │  │     │  │   ta_step_key_span_id, the seeded trace ID generator,
        │  │     │  │   ratio sampling; the record schema (kinds and layouts)
        │  core ─┘  │   kernel: stamps the cause on events it creates and
        │   ▲       │   appends facts at step boundaries
        │ system    │   context pool, start/inherit/sample, lifecycle records,
        │   ▲       │   context on effects and through timers/hibernation
        │  host     │   context across systems (router envelopes)
        │           │
        └── otlp ───┘  (new, freestanding, moved from tinyactors-otlp)
                        records + image → OTLP protobuf; OTLP → spans (decode)
                        uses trace + image only, never system
```

The edges are `uses` in `tinyactors-core/mise/core.rb`. `trace` and `otlp`
ship in `libtinyactors` with their headers `tinyactors/trace.h` and
`tinyactors/otlp.h`. `tinyactors-otlp/` is deleted; its protobuf writer,
span staging and size-then-write output move into `src/otlp`.

- **`trace` holds values and the schema, not policy.** It keeps no state
  of its own: the generator's state is the caller's, as `address.c`'s
  functions take theirs. Every layer and binding uses its one traceparent
  parser and its one span-ID function; conformance vectors are in
  `tests/conformance/traceparent.json` and `span_ids.json`, read by the C
  and the JavaScript tests.
- **`journal` is a base block**: a lossy ring of variable-size records
  (reserve, commit, peek, release), counting what it refused, with
  self-relative offsets so it moves with `memcpy`. It knows nothing about
  traces.
- **The kernel records facts, not policy.** It writes the kinds a mask on
  `ta_core` asks for, one branch per boundary when a kind is off, and it
  gives every event it creates the context of the step that created it.
  It knows nothing about sampling, trace IDs or OTLP.
- **`system` holds the policy**: the context pool, starting, inheriting
  and sampling traces, lifecycle records, and the drain API.
- **`otlp` is a codec**, like `basic_http`. A host or a binding drains the
  records and feeds them to it; it never runs inside `ta_system_run`, and
  core never handles protobuf.

### 2. Values (`tinyactors/trace.h`)

Only fixed-width integers, pointers and views cross the ABI, so IDs are
integers:

```c
typedef struct ta_trace_id { uint64_t high, low; } ta_trace_id;          /* 0:0 = none */
typedef uint64_t ta_span_id;                                            /* 0 = none */
typedef struct ta_step_key { uint64_t session, macrostep; uint32_t microstep, pad; } ta_step_key;
typedef struct ta_trace_context { ta_trace_id trace; ta_span_id parent; uint32_t flags, pad; } ta_trace_context;
```

- `ta_step_key_span_id` is 0.1's (`943b1f31^:tinyactors-core/src/trace.c`)
  bit for bit: three rounds of the MurmurHash3 finalizer over session,
  macrostep and `microstep | 1 << 32`, never 0; a key with session 0 is a
  host's cause and returns its macrostep (the host's span ID) unchanged.
  Microstep 0 is the macrostep itself.
- `ta_traceparent_parse` and `ta_traceparent_format` follow 0.1's
  `parseTraceparent`: spaces and tabs around the text are ignored, version
  `ff`, an all-zero trace ID or parent ID and version 00 with anything after
  the flags are refused, and a later version is read by its first four
  fields. `TA_TRACEPARENT_LENGTH` is 55.
- The generator is xoshiro256** seeded through splitmix64, as in 0.1; its
  state is a 32-byte value the caller keeps. Sampling is OpenTelemetry's
  `TraceIDRatioBased` on the trace ID's low 56 bits, with the threshold
  computed from a ratio `parts / whole` in integers, so every host decides
  alike without floating point.

### 3. The record schema

The writer (core, system) and the readers (otlp, bindings) share one
definition in `tinyactors/trace.h`. A record is a fixed 88-byte header and
a body; records are 8-aligned.

| Offset | Field | |
|---|---|---|
| 0 | `uint16_t kind` | `TA_TRACE_*` |
| 2 | `uint16_t flags` | the trace's W3C flags |
| 4 | `uint32_t size` | the whole record, header included |
| 8 | `uint32_t actor` | the actor's handle in its system |
| 12 | `uint32_t chart` | its chart, an index into the image |
| 16 | `ta_step_key key` | session, macrostep, microstep |
| 40 | `ta_trace_id trace` | |
| 56 | `ta_span_id span` | `ta_step_key_span_id(key)` |
| 64 | `ta_span_id parent` | the span's parent: the cause for a macrostep, the macrostep for a microstep |
| 72 | `uint64_t fuel` | the system's fuel clock (decision 9) |
| 80 | `uint64_t now` | the system's logical time |

Kinds: `spawned` (actor name, parent session), `finished` (final state,
actor name), `destroyed`, `unloaded` and `loaded` (hibernate and wake; the
actor name); `macrostep_begin` (event name and its trie node, type, sendid,
origin, the cause's step key, payload handle, datamodel index and the
actor's name), `macrostep_end` (the configuration after it); `microstep`
(its first fuel, the name of the event it took, the transitions, the
states exited in exit order and entered in entry order); `condition`
(transition, result),
`raised`, `sent` (target, event, the new event's context), `invoked`,
`cancelled`, `assign` (location, payload handle), `log`, `action_failed`;
and `dropped` (the records lost since the last one). A reader
(`ta_trace_next`) walks the records in a buffer and checks each size.

Names are not copied. States, transitions, charts and static event names
are indices into the image. Dynamic strings (event names, actor names,
targets, sendids) are references into the system's string slab, retained
while their record is in the journal and released when the host
acknowledges it, as effect strings are. The drain view carries the slab's
base, and `ta_trace_dynamic_string` resolves a dynamic reference against
it; the others are the image's (`ta_image_string`).
Payloads are handles, retained the same way, as `ta_effect.payload` is.
Core never reads payload bytes.

The lifecycle records' keys are fixed: `spawned` is (session, 0, 1),
`finished` (session, 0, 2), `destroyed` (session, 0, 3), and `unloaded` and
`loaded` (session, macrostep, `0xFFFFFFFE` and `0xFFFFFFFF`). They are
written whatever the sampling decision; one with no trace gets a fresh
trace ID from the generator.

### 4. The kernel: cause and facts

- **The event record keeps its size.** `ta_event` gains a `uint32_t
  trace`, a reference into the context pool (`TA_NIL` when untraced). To
  pay for it, `invoke_slot` and `flags` become 16 bits each, so the record
  stays at 56 bytes and tracing adds nothing per event. A chart may then
  have at most 65,534 invocations (`0xFFFF` means none); the compiler and
  the image verifier refuse more.
- **The step counters live in the instance header,** which grows from 48 to
  64 bytes: `uint64_t macrostep`, `uint32_t microstep` and `uint32_t trace`
  (the current macrostep's context). The kernel steps with an instance and
  never sees the actor record, and the header is already copied verbatim by
  hibernation and export, so the counters survive hibernation, snapshots
  and migration without new code. That is 16 bytes per actor whether
  tracing is on or not; keys are then the same with tracing on and off. The
  `trace` reference is a region reference: it is `TA_NIL` at every stable
  point, which is the only point an instance is written to a blob.
- **Macrosteps count from 1 per session** (the bootstrap is 1); microsteps
  count from 1 within their macrostep.
- **Each event the kernel creates** (send, raise, a platform error, a done
  event) gets the context of the step that created it, where `origin` is
  set: the macrostep's trace, its flags, and the step's key as the cause.
  One context per step is created when the first event needs it and shared
  by every event of that step (the workspace holds it); an untraced
  macrostep creates none.
- **Facts at phase boundaries:** begin and end of a macrostep, each
  microstep with its transitions, exit and entry sets, conditions, raises,
  sends, assigns and action failures, written when the mask on `ta_core`
  asks for the kind and the macrostep has a context. An assignment's value
  is captured as a payload (the datamodel's `capture` of the location) only
  when the mask has `TA_TRACE_VALUES`.

### 5. The system: contexts and policy

- **The context pool** is a `ta_pool` of 48-byte entries in the system
  block: the trace ID, the cause's step key (session, macrostep,
  microstep), the W3C flags and a reference count. The parent span ID is computed from the cause when needed. Its
  default capacity is `max_events`, since every event can hold a distinct
  context (host inputs each start a trace); contexts of one step are
  shared. When the pool is full the new event is untraced and
  `ta_system_stats.trace_contexts_dropped` goes up. Tracing never stops a
  run: no `TA_RUN_EXHAUSTED` because of tracing.
- **Starting traces.** `ta_event_in` gains `traceparent`. An external event
  without one (a bootstrap, a spawn, a host input, a timer of an untraced
  send) starts a new trace from the system's generator, seeded by
  `ta_system_config.trace_seed`: the same seed gives the same IDs. A host
  that runs more than one process must give each a different seed; the
  JavaScript bindings draw one from `crypto.getRandomValues` unless told
  otherwise.
- **Sampling** is `trace_sample_parts / trace_sample_whole` (`whole` 0 is
  the default, 1). A traceparent keeps its caller's sampled flag. An
  unsampled event has no context, so nothing downstream of it is traced.
- **Changing it while running.** `ta_system_trace_set` (and
  `ta_host_trace_set` for every segment of a host, and the ones added
  later) changes the kinds and the sampling between runs; open spans are
  not affected.
- **Propagation.** The context goes through `ta_system_route_send`, timers,
  autoforward (the copy is a child of the parent's macrostep) and invoke
  (a child's bootstrap is a child of the invoking step: the macrostep's
  last microstep, since invocations start at the macrostep's end).
  `done.invoke`, `error.communication` and the other events the system
  makes carry the context of the step they report on.
- **Effects.** `ta_effect` gains `ta_trace_context trace`, filled for
  `SEND`, `INVOKE`, `LOG` and `DONE`: an I/O processor writes `traceparent`
  without calling core.
- **Draining** mirrors the effects: `ta_system_trace(system, &view)` and
  `ta_system_trace_ack(system, bytes)`, with the lifetime rules of
  architecture section 11.3. Acknowledging releases the strings and
  payloads the acknowledged records hold.

### 6. Durability

Mailboxes, timers and the context pool live in the region, so hibernation,
snapshots, restore and relocation keep every waiting event's context. The
export blob writes each delayed send's context as a value (trace ID, cause,
flags), and adoption creates it again. The snapshot format's version goes
to 2 (the instance header and the event record changed); a version 1
snapshot is refused. `docs/durability.md` lists what each path keeps.

### 7. The host: contexts across systems

Every router envelope carries the sender's traceparent text into the
receiving system's `ta_event_in.traceparent`, queued envelopes and relays
included. The host is freestanding (ADR 0005), so this adds no use of the
C library. Each segment's seed is the host's seed mixed with the
segment's index, so segments never draw the same IDs.

### 8. The `otlp` codec

- **Memory as in the system:** one block sized at creation by
  `ta_otlp_layout` with `ta_arena`'s measure mode, `ta_otlp_init` places it,
  `ta_otlp_relocate` moves it into a bigger one. No allocator, no
  allocation after init. Open spans are a table, their span events chains
  of fixed chunks from a pool, finished spans the staging bytes. When the
  staging is full the encoder stops and says so, and the host flushes and
  calls again; when the open table or the chunks are full it says so, and
  the host relocates to grow. No record is lost either way.
- **Input:** the records of a system (decision 3), its image for names, the
  run time pairs (decision 9), and a payload renderer the binding passes
  (payload handle and datamodel index → JSON text, cut at the value limit).
  Rendering an ECMAScript value as JSON is free of any language's API, so it
  is in `src/datamodel_ecmascript`; the binding connects the two.
- **Output:** size, then write into the caller's memory
  (`ta_otlp_traces_size`, `ta_otlp_put_traces`), and the same for logs.
- **Decoding:** OTLP protobuf traces to plain span views, for tests, for
  the bindings' `decodeTraces`, and for applications that read their own
  spans.
- **Vocabulary:** granary's ADRs 0042 and 0062, exactly: `scxml.spawned`,
  `scxml.macrostep bootstrap` / `scxml.macrostep <event>`,
  `scxml.microstep <event>` / `scxml.microstep (eventless)`,
  `scxml.finished`, `scxml.destroyed`, and the span events
  `scxml.condition`, `scxml.raised`, `scxml.sent`, `scxml.assign`. Every
  span of an actor also gets `tinyactors.actor.name`,
  `tinyactors.chart.name` and `tinyactors.span.kind` (granary ADR 0155),
  so an application need not add them. Each scope names its chart (napi
  ADR 0028). `parent: cause | link` makes a macrostep a root linked to its
  cause.

### 9. Time: fuel, interpolated

Core has no clock, and a clock callback is not allowed. Every record
carries a **fuel clock**: a monotonic counter of the fuel the system has
used, read at the exact point inside a step where the record is written
(the system's clock at the start of the step plus what the step has used
so far, overrun included). A host notes its wall clock and the fuel clock
before and after each run; the encoder maps a record's fuel to wall time
by linear interpolation within the run that contains it. Spans stay open
across runs.

- **Order is correct:** the fuel clock never goes back, and the mapping is
  monotonic within a run and between runs (a run's pair comes after the
  previous one's). Each run moves the clock up by one before it does any
  work, so a fuel value belongs to one run only, and the encoder maps a
  record to the newest mark strictly below its fuel. A record written
  between runs (a spawn by the host) has the fuel of the previous run's end
  and takes its time.
- **Durations are positive:** a step charges fuel, but a macrostep whose
  event matches nothing can begin and end on the same fuel, and a coarse
  wall clock can give a run zero length. The encoder ends a span at least
  one nanosecond after its start, and keeps every time after the time of
  the record before it.
- **Many systems, one clock.** A host runs several segments in one call,
  and each segment would only count its own fuel. The host keeps one fuel
  clock and moves each segment's clock up to it before running it
  (`ta_system_trace_clock`), so the records of all segments share one
  clock and one pair per `ta_host_run` maps them all, in an order that
  agrees with causality across segments.

### 10. Defaults: on, at full detail

`ta_system_config`'s new fields follow its rule that 0 is the default:

| Field | Default |
|---|---|
| `tracing` | `TA_TRACING_ENABLED`; `TA_TRACING_DISABLED` turns it off |
| `trace_kinds` | `TA_TRACE_FULL`: every kind, but not `TA_TRACE_VALUES` |
| `trace_bytes` | the journal: 512 bytes per event of `max_events`, at least 64 KiB, enough for a burst |
| `trace_contexts` | `max_events` |
| `trace_sample_parts`, `trace_sample_whole` | 1 / 1 |
| `trace_seed` | 0 |

With tracing off the journal and the context pool are not carved: the
layout carries nothing for them, the kernel's mask is empty, and the only
cost is a branch per boundary.

Values (event data and assigned values) are asked for apart, with
`TA_TRACE_VALUES`, for two reasons found while building this. They can
hold personal data, so an operator should turn them on knowingly. And a
record holds its payload until the host acknowledges it: in the JavaScript
bindings a payload is a reference to a JavaScript value, so values on by
default kept every traced event's data alive until the next drain, which
the binding tests caught as memory that was not let go.

The PostgreSQL extension sets `TA_TRACING_DISABLED`: it runs one system per
statement and has no host loop that would drain records. Tracing there is
left for later (ROADMAP).

### 11. The JavaScript bindings

Written once, in `bindings/shared/binding.c` and `bindings/shared/core.mjs`,
for Node.js and the browser. Every system traces at full detail with no
code. `bindings/shared` takes the exporter's settings as one options object
(`createSystem({ telemetry })`) and never reads the environment. The
Node.js entry (`bindings/node/telemetry.js`) reads
`OTEL_EXPORTER_OTLP_ENDPOINT` (or `..._TRACES_ENDPOINT`), `..._HEADERS`,
`OTEL_SERVICE_NAME`, `OTEL_RESOURCE_ATTRIBUTES`, `OTEL_TRACES_SAMPLER_ARG`
and `OTEL_SDK_DISABLED` once, and an application's options win. The bytes
go from C to the sink or the transport as one `Uint8Array` per run: no
JavaScript object per record or span.

- **The encoder is the binding's, in C.** After every `ta_host_run` the
  binding marks the wall clock (the runtime's `performance.timeOrigin +
  performance.now()`, read through one call) and the host's fuel clock,
  reads every segment's records into one `ta_otlp` block, and concatenates
  the staged requests into one buffer per run (protobuf messages of one
  type merge when concatenated). The block grows by relocation when it
  says so.
- **With nowhere for records to go, core writes none.** A system with
  neither a sink (`setTraceSink`) nor an exporter (an endpoint) sets the
  kind mask to `dropped` alone: traceparents still flow through actors,
  links and effects, so a sink installed later continues the traces
  already running, but no record is written and none is encoded. "On by
  default" means the contexts; the records follow the first destination.
- **The exporter** posts `/v1/traces` and `/v1/logs` with the platform's
  HTTP client (Node.js: `bindings/node/transports.js`; browsers: `fetch`),
  gathering a run's bytes for 250 ms or 1 MiB, one request in flight per
  signal, at most 16 MiB waiting. Failures are quiet, logged at most once
  every ten seconds. `system.flushTraces()` waits for what is in flight;
  `stop()` ends the spans still open (marked incomplete) and posts them.
- `stepSpanID`, `parseTraceparent`, `formatTraceparent` and `decodeTraces`
  call the C code; `tests/conformance/run_js.mjs` holds them to the same
  vectors as `tests/unit/test_trace.c`.

## Performance budget

Measured with the torture chamber, throughput and memory together, on one
machine, side by side against the commit before this work:

- tracing off: at most 4 bytes per event, one branch per boundary, no
  measurable change in throughput;
- tracing on at full detail, records only: at least 90% of untraced
  throughput;
- tracing on with OTLP encoding through the Node.js binding: measured and
  reported next to 0.1's numbers in `docs/research/trace-handoff-formats.md`.

### Measured (2026-10-09)

Apple M4 Max (16 cores, 48 GB), macOS 26.6.2, release builds, one bench
thread. The torture chamber's `throughput` scenario (30 s: null-datamodel
ping-pong storms next to ECMAScript pingers, storms added at 10 s and
halved at 20 s), three runs of each build interleaved, medians. "Before"
is `4e64c647`, the commit before this work; the others are this tree with
`--param tracing=...`. Region is the peak of both systems' blocks.

| Build | Events/s | vs before | Region (peak) | Records/event | Lost | OTLP bytes/event |
|---|---:|---:|---:|---:|---:|---:|
| before (`4e64c647`) | 1.109M | 100% | 3.7 MB | – | – | – |
| tracing disabled | 1.104M | 99.6% | 4.3 MB | – | – | – |
| full detail, records written and acknowledged (not read) | 0.994M | 89.6% (90.0% of disabled) | 17.5 MB | 4.27 | 0 | – |
| full detail, encoded as OTLP in C (`tracing` scenario) | 0.561M | 50.6% | 17.5 MB | 4.27 | 0 | 1,197 |

Runs of one build vary by about ±1.5%.

- **Tracing disabled:** no measurable change in throughput; the event
  record is unchanged (0 bytes per event). The region grows by 0.6 MB
  because the instance slab is now sized in whole pages for `max_actors`
  (190–290 bytes of capacity per actor here, 16 of them the header's step
  counters), a sizing fix the bigger header made necessary; it is
  capacity, not use.
- **Records only: at the budget line, not clearly above it.** 89.6% of
  before and 90.0% of disabled, within the noise. This scenario is the
  worst case: an event costs about 1 µs, and full detail writes 4.27
  records (about 500 bytes) for it. What it costs, measured by detail:
  carrying contexts with no records 2%, `summary` (macrostep begin and end)
  7%, `decisions` 9.5%, `full` 10%. The rest of the time is in writing the
  record header and body (`ta_core_trace_step`, about 4%) and releasing
  what records hold when they are acknowledged (about 3%). Getting there
  took three changes found by profiling: the kernel keeps the span-ID mix
  of the macrostep it writes about and its session (one round of the mix
  per record instead of six, no call per record to look up the session),
  the journal's reserve and commit are inline, and acknowledging walks
  records by their sizes. Region: the journal (512 bytes per event of
  `max_events`: 8 MiB for the storm system's 16,384 events) and the
  context pool (48 bytes per event).
- **OTLP in C** halves the throughput of this scenario: encoding costs
  about as much as running a null-datamodel event.
- **OTLP through the Node.js binding** (`tools/bench_tracing.mjs`, the
  research note's workload: 100,000 actors, one `ping` with an assignment
  each, Node 22.23 on the same machine; best of three rounds):

  | Variant | Node addon | vs untraced | wasm | vs untraced | OTLP bytes/event |
  |---|---:|---:|---:|---:|---:|
  | untraced (no sink) | 327k/s | 100% | 274k/s | 100% | 0 |
  | `summary`, to a sink | 295k/s | 90% | 221k/s | 81% | 418 |
  | `full`, to a sink | 261k/s | 80% | 196k/s | 72% | 1,015 |
  | `full`, and `decodeTraces` in JavaScript | 36k/s | 11% | 20k/s | 7% | 1,015 |

  0.1 ran this workload at 2.24M/s untraced and 0.20M/s with every kind
  (9%), all of the loss in turning records into JavaScript objects. Here
  full detail keeps 80%: the bytes reach JavaScript as one `Uint8Array` per
  run. Untraced is slower than 0.1's because every event here calls the
  JavaScript datamodel (the assignment); that is the same in every row.
  Decoding is for tests and tools, not for the hot path.

## Consequences

- Every application gets traces with no setup; granary's trace tests read
  `tinyactors.actor.name` and `tinyactors.span.kind` instead of adding them.
- Every actor costs 16 bytes more (the instance header); an event costs
  nothing more.
- Version 1 snapshots are refused.
- A chart may have at most 65,534 invocations.
- A C host that runs more than one process must seed each system's trace
  IDs differently.
