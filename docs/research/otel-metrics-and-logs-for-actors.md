# OpenTelemetry metrics and logs for an actor runtime

**Date:** 2026-09-24
**Question:** How should tinyactors answer OpenTelemetry's metrics and
logs signals, now that it has traces (core ADR 0067, napi ADR 0016)? What
should it capture, judged by what an on-call engineer needs when "it's
3am and my actors aren't working in prod"?
**Method:** the OpenTelemetry specifications and semantic conventions,
otel-tui's README, the documentation of five actor runtimes and XState,
and a reading of what core and napi already know (sources below). No
prototype.

## 1. The data models

### Logs

A `LogRecord` has a timestamp, an observed timestamp, `TraceId`,
`SpanId` and `TraceFlags` (W3C trace context), `SeverityText` and
`SeverityNumber`, a `Body`, the `Resource`, the `InstrumentationScope`,
`Attributes`, and an `EventName` [2]. The logs data model is **Stable**
[2]. Severity numbers fall in six ranges: TRACE 1–4, DEBUG 5–8, INFO
9–12, WARN 13–16, ERROR 17–20, FATAL 21–24; 0 is unspecified [2].

**An event is a LogRecord with an `EventName`.** Event names must be
static; anything that varies per occurrence goes in attributes [7].

**Span events are moving to logs.** OTEP 4430 deprecates the span event
**API** (`Span.AddEvent`, `Span.RecordException`): "new code should
write events as logs that are correlated with the current span." The
OTLP `Span.events` field stays valid, and SDKs will offer a way to turn
log-based events back into span events [8]. This matters for napi ADR
0016, which maps every non-step record kind to a span event: that is
still valid OTLP data, but the direction of the ecosystem is logs with
trace and span IDs.

### Metrics

OTLP metrics have Sums (monotonic or not, delta or cumulative
temporality), Gauges, Histograms with explicit buckets, Exponential
Histograms, and the legacy Summary [3]. Sums, Exponential Histograms and
Exemplars are marked Stable; the document as a whole is "Mixed" [3].

- **Temporality.** Cumulative streams repeat their start time;
  delta streams advance it. Cumulative needs the writer to keep state
  since the start, which is costly at high cardinality; delta moves that
  cost downstream [3][4].
- **Single writer.** Every metric stream must have one logical writer at
  a time [3]. Two tinyactors systems in one process that write the same
  metric with the same attributes would break this, so a system needs an
  identifying attribute or its own scope.
- **Restarts.** A cumulative stream whose start time is unknown resumes
  with a zero-duration point, so a receiver can tell data may be lost
  [3].
- **Exemplars** attach a trace ID, span ID, timestamp and value to a
  metric point [3]. They are how a metric links to "show me one trace
  where this happened".
- **Asynchronous (observable) instruments** fit values the application
  already keeps: they are read only at collection [4]. That is the pull
  model.
- **Cardinality.** SDKs are told to cap memory by combining or dropping
  points and reporting the loss [4]. Akka Insights has a hard limit on
  metric identifiers for the same reason [10].

### OTLP and otel-tui

OTLP 1.11.0 is Stable for traces, metrics and logs, in protobuf and
JSON, with HTTP paths `/v1/traces`, `/v1/metrics` and `/v1/logs` [1].
The ADR 0016 encoder's approach (C writes the request, JavaScript posts
the bytes) carries over to the other two services unchanged.

otel-tui displays all three signals [9]:

- **Logs:** display, filter, detail, and "show logs related to a specific
  trace or span" are all done. So logs that carry trace and span IDs can
  be reached from the trace view.
- **Metrics:** stream list, filter, detail, and charts for **Gauge, Sum
  and Histogram**. Exponential Histogram and Summary charts are still on
  its to-do list. So tinyactors should use explicit-bucket histograms if
  it wants them visible in otel-tui.
- It does not mention exemplars.

## 2. Semantic conventions

- **Naming.** Metric names are namespaced hierarchically; units are UCUM
  and not part of the name; counts use curly-brace annotations such as
  `{message}` or `{error}`; durations are in seconds (`s`) [5].
- **Messaging conventions** define `messaging.process.duration`,
  `messaging.client.sent.messages` and similar, but they are all at
  **Development** status and are about clients of a message broker
  (`messaging.system` is Kafka, RabbitMQ, SQS) [6]. An actor's mailbox is
  not a broker queue, and adopting a Development convention would tie our
  names to its churn. The note recommends not using them, apart from
  their unit and bucket guidance (seconds; buckets 5 ms to 10 s).
- **Process and runtime metrics** (CPU, heap, event loop) are the host
  language's job. Node has its own. tinyactors should not repeat them.
- **Our own names.** ADR 0067 already uses `scxml.*` for chart concepts
  (`scxml.state`, `scxml.transition`, `scxml.session_id`). Runtime
  concepts that SCXML does not have (mailboxes, pumps, quarantine, dead
  letters) need a `tinyactors.*` namespace.

## 3. Cardinality

An attribute's values multiply the number of streams. So:

- **Never a metric attribute:** session IDs, actor IDs, send IDs,
  invoke IDs, event data, and event names by default (hosts can post
  events with names built from data, such as `user.12345`).
- **Safe metric attributes:** the definition's family and revision
  (bounded by what is deployed), a state's author ID within a definition
  (bounded by the chart), an error type (`execution`, `communication`,
  `platform`), an I/O processor type, a dead-letter reason, a fault code,
  a memory category.
- **High-cardinality facts still reach the engineer,** just through other
  signals:
  - **logs** can carry `scxml.session_id` and the actor ID, because each
    log record is one occurrence, not a stream;
  - **exemplars** on a histogram point at one trace that was slow;
  - **inspection** answers "which actors" by walking them on demand,
    the way `recon:proc_count/2` answers "top N processes by message
    queue length" [15].

## 4. Prior art in actor runtimes

- **Akka Insights (Lightbend Telemetry)** records mailbox size and stash
  size (sampled), mailbox time and processing time, per actor class,
  tag or group, and warns that per-instance metrics must be selected
  carefully because of a hard limit on metric identifiers [10].
- **Orleans** has meters for activations (count, created, destroyed,
  failed to activate), messaging (dropped, failed, rejected, expired,
  rerouted), `orleans-scheduler-long-running-turns`, reminder
  **tardiness** as a histogram, and the **age of the oldest enqueued
  message** in stream queues. Logs go through the platform logger;
  tracing through ActivitySource [11].
- **Proto.Actor** exposes spawn and stop counts, failure counts, mailbox
  length and message processing duration through OpenTelemetry, by actor
  type [12].
- **Dapr actors** count activations and deactivations, pending calls
  waiting for the per-actor lock, timers and reminders fired, and
  **timers dropped** [13].
- **Erlang/OTP.** `:telemetry` has libraries emit events (a name, a map
  of measurements, a map of metadata) and consumers attach handlers and
  aggregate them into metrics; `telemetry:span/3` emits start, stop and
  exception events with a duration [14]. For live diagnosis, `recon`
  finds the top N processes by an attribute (message queue length,
  reductions, memory) at a moment or over a sliding window, and avoids
  calls that are unsafe on a process with a huge mailbox [15].
- **XState's inspection API** emits `@xstate.actor`, `@xstate.event`,
  `@xstate.snapshot` and `@xstate.microstep` to one callback [16]. It is
  the closest analogue to ADR 0067's records: a stream of facts, not
  aggregates.

**What operators lean on at 3am,** across these: queue length and the
age of the oldest queued message; processing time per message;
failure, drop and dead-letter counts; timer lateness and dropped timers;
activation counts; and a way to find the worst individual actors on
demand (recon, observer). None of them keeps per-actor metric streams by
default.

## 5. What tinyactors knows, and where

**Core knows**, deterministically and without a clock:

- every macrostep, microstep, ignored event, raised error, send,
  cancelled send, invocation, pause, and stop on allocation failure
  (ADR 0067's record kinds; ADR 0070 for `STOPPED`);
- the definition each actor runs (`ta_definition_inspection.actor_count`
  counts resident actors per definition);
- each actor's configuration, residency, pending delayed sends, active
  invocations and queued internal events (`ta_actor_inspect`), and a
  paged walk over actors (`ta_system_visit_actors`);
- the earliest pending deadline (`ta_system_next_deadline`), and the due
  time of every delayed send it dispatches.

**Core does not keep** any counters of these today, outside the trace
records a sink may receive. There is no per-definition or per-system
statistic.

**Only the host knows:** mailbox depth and message age (napi ADR 0005),
dead letters and their reasons, quarantine and faults (napi ADR 0008),
turn duration and the runnable and ready backlog (napi ADR 0007), wall
time, and memory per category (napi ADR 0014). napi's `stats()` already
reports memory per category, allocator calls, messages queued,
delivered and dead-lettered, and live values.

**Gaps that matter for 3am** (ADR 0067 open points):

- an internal event that enables no transition leaves no record;
- events core queues outside a run leave no `RAISED` record. That
  includes the `error.communication` of a delayed send that could not be
  delivered, which is a typical production failure;
- sampling suppresses every record of an unsampled macrostep, so with a
  production sampling rate most errors would never be seen. ADR 0067's
  `unsampled` flag is all or nothing.

## 6. Cost

- ADR 0067's measurements, napi on Node: untraced 2.24M events/s; core
  with every record kind and napi's copy skipped, 2.13M. So building
  records in core costs about 5%.
- A counter increment in core is a few nanoseconds against about 450 ns
  per event at 2.2M events/s. Five increments per event cost about 1–3%.
- Per-actor memory at 4M actors: every 8 bytes is 32 MB. ADR 0063 and
  ADR 0069 fought for each of those bytes.

## Sources

1. OTLP Specification 1.11.0: https://opentelemetry.io/docs/specs/otlp/
2. Logs Data Model: https://opentelemetry.io/docs/specs/otel/logs/data-model/
3. Metrics Data Model: https://opentelemetry.io/docs/specs/otel/metrics/data-model/
4. Metrics Supplementary Guidelines: https://opentelemetry.io/docs/specs/otel/metrics/supplementary-guidelines/
5. Semantic conventions, general metrics: https://opentelemetry.io/docs/specs/semconv/general/metrics/
6. Semantic conventions, messaging metrics: https://opentelemetry.io/docs/specs/semconv/messaging/messaging-metrics/
7. Semantic conventions, events: https://opentelemetry.io/docs/specs/semconv/general/events/
8. Deprecating Span Events API (blog): https://opentelemetry.io/blog/2026/deprecating-span-events/ and OTEP 4430: https://github.com/open-telemetry/opentelemetry-specification/blob/main/oteps/4430-span-event-api-deprecation-plan.md
9. otel-tui README: https://github.com/ymtdzzz/otel-tui
10. Akka Insights, actor instrumentation: https://doc.akka.io/libraries/akka-insights/current/instrumentations/akka/actors.html
11. Orleans observability: https://learn.microsoft.com/en-us/dotnet/orleans/host/monitoring/
12. Proto.Actor metrics: https://proto.actor/docs/protoactor/metrics/ (the page returned 404 when fetched on 2026-09-24; the summary is from its search listing and the Go `metrics` package: https://pkg.go.dev/github.com/asynkron/protoactor-go/metrics)
13. Dapr metrics list: https://github.com/dapr/dapr/blob/master/docs/development/dapr-metrics.md
14. Erlang `telemetry`: https://telemetry.hexdocs.pm/readme.html
15. recon: https://ferd.github.io/recon/recon.html
16. XState inspection: https://stately.ai/docs/inspection
