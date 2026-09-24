# Observability: build or reuse? Where tracing's time goes, and what the OpenTelemetry SDK would cost instead

**Date:** 2026-09-24
**Question:** Tracing in napi costs more than half of throughput even with
only macrostep records, and we have built our own OTLP encoder, decoder,
name cache, open-span tracking and trace-ID generation. Is that cost
inherent, or ours? Would the OpenTelemetry ecosystem (the JS SDK, or only
`@opentelemetry/api`) be cheaper, and what would we give up?
**Method:** the tracing bench and a single-variant bench of the same
chart, profiled with macOS `sample`; a throwaway worktree with two
fixes applied; and a JS bench of `@opentelemetry/api` and the official SDK
creating equivalent spans. Everything ran on one machine (Node v26.10.0,
M-series Mac, macOS). The worktree and the benches lived in the
scratchpad and are not in the tree.

## 1. What tracing costs today

The chart: one state, one transition on `ping` that assigns. Every
delivered event writes four records (BEGIN, MICROSTEP, ASSIGN, END).
100,000 actors, 20 rounds of one `ping` each, ns per delivered event.

| Mode | ns/event | Events/s | OTLP bytes/event |
|---|---|---|---|
| Untraced (no sink, no `traceparent`) | 418 | 2.39M | – |
| **No sink, but `post` gets a `traceparent`** | **912** | **1.10M** | – |
| Sink wants BEGIN and END, no `traceparent` | 941 | 1.06M | 338 |
| Sink wants BEGIN and END, sampled `traceparent` | 1361 | 0.73M | 441 |
| Sink wants every kind, **sampled-out** `traceparent` | 924 | 1.08M | 50 (spawn records only) |
| Sink wants every kind, no `traceparent` | 1573 | 0.64M | 1118 |

Three findings stand out before any profile:

- **Passing a `traceparent` costs 494 ns per post, with no sink at all.**
  That is more than the whole untraced event. `lib/tracing.js` parses the
  string with a regular expression and builds three BigInts
  (`nativeTrace`: 317 ns measured alone, 169 of them in
  `parseTraceparent`), and the addon reads the BigInts back through
  Node-API. A production host that propagates its callers' context on
  every post pays this whether or not anything is traced.
- **Sampling out is already nearly free in core.** With a sink installed
  and a sampled-out `traceparent`, an event costs 924 ns against 912 ns
  for the same `traceparent` with no sink: 12 ns. Core's gate (ADR 0067
  section 6) works. What is not free is everything around it.
- **The encoder's output is large because the mapping is lossless.** A
  macrostep span carries session, macrostep, actor, definition, time,
  event name and type, flags, and the configuration as names and IDs:
  about 300 bytes. OTLP has no key dictionary, so every span repeats every
  attribute name.

## 2. Where the time goes

`sample` for six seconds of the steady state in each mode, self time
grouped by category (every non-waiting thread; GC threads included).
Nanoseconds are the share times the measured ns/event.

| Category | Untraced | Macrosteps only | Every kind |
|---|---|---|---|
| V8, Node, JavaScript | 36% | 24% (~230 ns) | 13% (~210 ns) |
| Core interpreter | 32% | 18% (~170 ns) | 12% (~185 ns) |
| **OTLP encoder** (attribute and span writing) | – | 13% (~125 ns) | 29% (~450 ns) |
| **Trace-ID random bytes** (`arc4random_buf` → corecrypto AES-CTR DRBG) | – | 13% (~120 ns) | 7% (~105 ns) |
| **memmove/memset** (mostly `tna_span_end` copying a finished span into the request) | 2% | 10% (~100 ns) | 17% (~260 ns) |
| napi, other | 13% | 8% (~75 ns) | 3% |
| **Wall clock** (`gettimeofday` per record) | 4% | 5% (~45 ns) | 7% (~105 ns) |
| malloc | 11% | 3% | 3% |
| GC | – | 3% (~24 ns) | 5% (~72 ns) |
| Names (`ta_event_name_format` per record) | – | 2% (~21 ns) | 3% (~42 ns) |
| **Core record building** (ADR 0067) | 2% | 2% (~17 ns) | 3% (~41 ns) |
| Open-span bookkeeping | – | 0.3% | <0.5% |

**Why macrostep records alone halve throughput (+523 ns):** a random trace
ID from the OS's cryptographic generator on every post (~120 ns), the
encoder (~125 ns), copying each span twice (~100 ns), JavaScript and GC for
a fresh `Uint8Array` per turn (~80 ns), a wall-clock read per record (~30
ns), formatting the event name per record (~20 ns). Core itself adds 17 ns.
**Nearly all of the cost is in napi's encoder and in host choices, not in
core, and not in OTLP as such.**

**Two of those choices are easy to change, and a throwaway worktree
measured them:** trace IDs from SplitMix64 seeded once from the OS, and
the wall clock read once per 64 records.

| Mode | Before | After both fixes |
|---|---|---|
| Untraced | 418 ns | 416 ns |
| Macrosteps only | 941 ns (1.06M/s) | **789 ns (1.27M/s)** |
| Every kind | 1573 ns (0.64M/s) | **1416 ns (0.71M/s)** |

In the fixed build the remaining macrostep overhead (~375 ns) is the
encoder, the span copy, the per-turn buffer on the JavaScript side, and
event-name formatting, in that order.

## 3. What the OpenTelemetry ecosystem would cost

A JS bench creating, per delivered event, the spans our encoder writes,
through `@opentelemetry/api` 1.9.1, with and without the SDK
(`@opentelemetry/sdk-trace-base` 2.11.0, `BasicTracerProvider`,
`BatchSpanProcessor`, an exporter that discards). This is the SDK alone:
no tinyactors, no crossing from C.

| Through the OpenTelemetry API | ns/event | Events/s |
|---|---|---|
| API only, no SDK installed (no-op spans) | 22 | 44.7M |
| SDK, sampler `TraceIdRatioBased(0)` (sampled out) | 388 | 2.58M |
| SDK, one macrostep span | 652 | 1.53M |
| SDK, one macrostep span with four attributes | 780 | 1.28M |
| SDK, macrostep and microstep spans | 1284 | 0.78M |
| SDK, both spans, attributes and one span event (our "every kind") | 2024 | 0.49M |
| **SDK, one macrostep span, serialized to OTLP protobuf** (`ProtobufTraceSerializer`) | **1331** | 0.75M (88 bytes/event) |
| **SDK, "every kind", serialized to OTLP protobuf** | **5415** | 0.18M (598 bytes/event) |

Delegating to the SDK would add these numbers **on top of** tinyactors'
own ~418 ns and of getting each record into JavaScript. The one
measurement we have of that crossing is napi ADR 0015's record objects
(0.19M/s for every kind, about 4.8 µs per event above untraced). So:

| For every kind, as OTLP bytes | ns/event, whole system |
|---|---|
| Ours today | 1573 |
| Ours with the two fixes | 1416 |
| SDK, excluding the crossing | 418 + 5415 ≈ 5800 |
| SDK, including an ADR 0015-style crossing | ≈ 10,600 |

- **The SDK is 4 to 7 times more expensive than what we built,** for the
  same spans. It is a general-purpose tracer: per span it allocates a
  `Span` object, attribute maps, a `SpanContext`, and runs the processor
  and serializer in JavaScript.
- **Its sampled-out path costs 388 ns per span** (IDs, a non-recording span
  and a sampler call per `startSpan`). Ours costs 12 ns per event, because
  core decides once per macrostep and builds nothing.
- **The API with no SDK is cheap (22 ns),** but we would still pay the
  crossing to call it per record, so "untraced" would only be cheap if the
  crossing were skipped, which is what our null sink already does.

## 4. What OpenTelemetry advises libraries to do

- "Libraries should only use the OpenTelemetry API," and "Unless your
  application imports the OpenTelemetry SDK, your instrumentation does
  nothing and does not impact application performance." [1]
- "Real-life applications, especially on the high scale, would frequently
  have head-based sampling configured. Sampled-out spans are affordable
  and you can check if the span is recording to avoid extra allocations."
  [1]
- The SDK "MUST by default randomly generate both the TraceId and the
  SpanId", and SHOULD meet W3C Trace Context Level 2's randomness for the
  rightmost 56 bits. The spec does not require cryptographic randomness.
  `TraceIdRatioBased` MUST decide from a deterministic hash of the trace
  ID, so every process agrees. [2]

**Weighing that against "lib/ has no telemetry dependency":**

- **For the guidance:** a library that uses `@opentelemetry/api` joins
  the application's traces with no glue. It can read the active context at
  `post`/`send`/`spawn` time (propagation with no `traceparent` option),
  and it inherits the application's sampler. The API package is small,
  stable at 1.x, and costs 22 ns per call as a no-op.
- **Against it, for tinyactors specifically:**
  - The guidance assumes spans are created in the language the SDK runs
    in. Ours are created in C, by core, at a rate (millions per second)
    where every JS call per record costs more than the event. Using the
    API for spans means the crossing per record, which is the cost we
    removed.
  - It serves only the JS host. The browser build could share it, but
    Python, CRuby, and the database hosts would each need their own
    adapter, while the OTLP bytes are the same everywhere.
  - Measured, the SDK path is 4 to 7 times more expensive than ours.
- **Where the guidance would help us at low cost:** reading the active
  context for propagation only (one `context.active()` per post, tens of
  ns), not creating spans. That can live in a separate, optional adapter
  package, leaving `lib/` free of the dependency. That is a question for
  the user, not something to adopt quietly.

## 5. The user's three ideas, measured against this

1. **Correlation IDs that index a table.** Trace parents are 48 bytes on
   delayed sends and 8 extra bytes per napi envelope; replacing them with a
   4-byte index into a shared table saves memory for pending sends. It
   does not save CPU: core's record building is 17–41 ns per event. The
   table needs reference counts, a free list, and entries in session
   images. The place an ID table *would* pay is the JavaScript boundary,
   where parsing a `traceparent` costs 494 ns per post; but parsing the
   55 characters in C costs a few tens of ns, with no lifetime to manage.
   Verdict: not now; parse in C instead.
2. **A new arena with a new lifetime for tracing.** The encoder already
   reuses its buffers (malloc is 3% of traced time). A turn arena cannot
   hold open spans: a macrostep that pauses stays open across turns. What
   is right in the idea is tying state to the lifetime it belongs to:
   the name cache to the definition (resolves "the name cache never
   evicts"), open spans to the macrostep and the actor (resolves "linear
   array"). Verdict: yes to lifetimes, no to a new arena.
3. **Swallow the cost, but make toggling on, off and sampling really
   easy.** Largely what the data supports. The traced path costs what the
   bytes cost; the untraced and sampled-out paths must be near-free, and
   they nearly are in core (12 ns). Two caveats: about 40% of today's
   traced overhead is avoidable (ID generation, clock, double copy, name
   formatting), and the untraced path is not free today when a
   `traceparent` is passed (494 ns).

## 6. Turning tracing on at 3am

What exists today: a sink can be installed and removed at runtime
(`setTraceSink`), and a `traceparent` with the sampled flag set traces one
request end to end, because core propagates the context to everything the
request causes (ADR 0067 section 6).

What is missing:
- **A sampler for the traces napi starts.** Today every delivery without a
  `traceparent` starts a sampled trace, so installing a sink traces
  everything. A `TraceIdRatioBased`-compatible ratio on the new trace ID
  costs a hash and a compare.
- **Targeting one actor or one definition.** The host knows the target of
  every `post` and `send` before core does. A host-side watch set (by
  session ID, by definition family, by address) consulted only when it is
  non-empty can mark the context sampled for matching deliveries. That
  needs no core change and no per-actor memory. Everything the watched
  actor causes then follows, because core propagates the flag.
- **Changing the configuration without reinstalling the sink,** so open
  spans are not ended as incomplete.

## Sources

1. OpenTelemetry, "Libraries" (instrumentation concepts):
   https://opentelemetry.io/docs/concepts/instrumentation/libraries/
2. OpenTelemetry Specification, Tracing SDK (samplers, `IdGenerator`,
   randomness): https://opentelemetry.io/docs/specs/otel/trace/sdk/
3. Package versions measured: `@opentelemetry/api` 1.9.1,
   `@opentelemetry/sdk-trace-base` 2.11.0,
   `@opentelemetry/otlp-transformer` 0.222.0 (as pinned in
   `tinyactors-napi`'s dev dependencies).
