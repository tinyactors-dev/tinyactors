# Metrics, and trace policy as data

**Status:** Adopted → [ADR 0007](../adr/0007-metrics-and-trace-policy.md) (2026-10-10; built in commits `e366bd6d`, `b1eeb700`, `360dfea3`, `59d54bcd`, `a6aa8836`, `22b64598`). Where this text and the ADR differ, the ADR says what was built.
**Touches:** tinyactors-core: `src/system` (per-chart counters, the policy table and its decision points), `src/core` (the step's kind mask, error counts), `src/host` (merging segments, carrying the detail across segments), `src/otlp` (an ExportMetricsServiceRequest writer), `bindings/shared` and `bindings/node` (`metrics()`, `metricsToOTLP`, the `/v1/metrics` exporter, `configureTraces({ rules })`, `watchTraces`). Granary is read, not changed.
**Builds on:** [ADR 0006](../adr/0006-tracing-is-a-core-feature.md). Carries out ROADMAP "Next" item 2 (metrics, watching).
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Summary

- **Metrics: whom first.** The operator running a system in production,
  and the capacity planner who sizes it, under one hard rule from the C
  embedder: nothing per event, nothing per actor, no clock, no
  OpenTelemetry needed. The application developer is served by traces and
  by Part 2, not by metrics.
- **Metrics: what is built.** Most questions are already answered by
  `ta_system_stats`, `ta_host_stats` and spans. Core gains one small block
  of counters per chart, written once per *turn* (not per event) and on
  failure paths only. Distributions (latency, duration) come from spans: a
  collector derives them (spanmetrics), and the encoder can, as an opt-in,
  with exemplars that are always real spans. The `otlp` codec writes an
  ExportMetricsServiceRequest; JavaScript gets 0.1's `metrics()` and
  `metricsToOTLP()` and a `/v1/metrics` exporter.
- **Trace policy.** A trace context gets a 3-bit *detail class* in flag
  bits W3C does not own. The system holds one kind mask per class and a
  small table of rules, evaluated only at decision points: when a trace
  starts, when an actor takes an event, when an action fails. The decision
  travels with the context to everything the step causes. The kernel reads
  one mask per step instead of the system-wide one. No callback, no byte
  per event, no byte per actor.

## Where things stand

| What | Where | Today |
|---|---|---|
| Pool occupancy and capacity (actors, events, timers, workspaces, requests, slab, outbox), runnable, delivered, fuel, turns, timers fired, sessions, hibernated, hibernations, wakes, region bytes, logical time | `ta_system_stats_get` (`src/system/system.c`) | O(1), per system |
| Journal bytes, records written and lost, contexts in use and dropped, fuel clock | `ta_system_stats` | per system |
| Segments, actors, hibernated, runnable, jobs, delivered, routed, migrated, retired | `ta_host_stats_get` | per host |
| Spans with exact durations, per macrostep and microstep, with `tinyactors.chart.name` and `tinyactors.span.kind` | `src/otlp` | when sampled |
| Anything per chart; errors; faults | none | |
| Sampling | `ta_system_start_trace` (`src/system/tracing.c`) | once per trace start: the caller's sampled flag, else TraceIDRatioBased on the new trace ID |
| What is written | `ta_core.trace_kinds` | one mask per system; a record is written when its bit is set *and* the macrostep has a context |
| An unsampled trace | `ta_system_start_trace` | gets no context: its trace ID is dropped, effects carry no `traceparent` |
| Context entry | `ta_trace_entry` (`src/core/core_internal.h`) | 48 B; `uint32_t flags` of which W3C uses the low 8 bits |
| Actor record | `ta_actor` (`src/system/system_internal.h`) | 48 B; `flags` bits 6 and 7 and a `uint16_t pad` are free |

0.1 had metrics and watching (`943b1f31^`): core ADR 0072 (per-definition
counters, state occupancy, failure kinds written unsampled), napi ADR 0017
(`watchTraces`, `configureTraces`, `metrics()`, `metricsToOTLP` in
JavaScript, host histograms with exemplars), and three kind masks chosen
by a local `detail` byte on the context. This proposal keeps what 0.1
learned and fits it to the standalone core: core routes sends between
actors itself now, so a watch can no longer be the host's check at its one
delivery point.

---

## Part 1: metrics

### 1.1 Frame of reference: who asks what, and what they decide

| Perspective | Their questions | What they decide with the answer |
|---|---|---|
| **Application developer** debugging or tuning charts (granary) | Why did this issue end up `closed`? Which transition fired, with what data? Which step is slow? Did my change make the bootstrap cheaper? | Fix a chart, a guard, an action; accept or reject a change. |
| **Operator / on-call** | Is it healthy (errors, faults, lost records)? Is it overloaded (backlog, late timers, a run blocking the loop)? Which chart is busy or failing? Is a pool about to fill? | Page or not; scale out, raise capacities, roll back a version, turn tracing up for one chart. |
| **Capacity planner** | How many actors and events per second per machine? Memory per actor? Which chart costs the CPU? How fast does it grow? | Machine size, capacities in `ta_system_config`, how many systems per host. |
| **C embedder without OpenTelemetry** (a game, the PostgreSQL extension, a microcontroller) | Are my pools big enough? Did anything fail? How much fuel per frame or per statement? | Size the block; budget fuel per frame. And: what must I *never* pay for. |
| **tinyactors maintainers** | Did a change cost throughput or memory? Where does the fuel go? | Accept a commit; find a regression. |
| **The observability backend** (Prometheus / Mimir through OTLP, Grafana Cloud) | How many series does this add? Are names, units and temporality what the translator expects? | Cost per month; whether a metric is kept or dropped. |

What the evidence says about each:

- **Developer.** Granary's tests assert on spans only (ADR 0007, `tests/traces.ts`
  with its predicates such as `actorFinished`); its load test checks the
  oracle's own metrics and the actors' traces. It never called 0.1's
  `system.metrics()`. Its developer questions are about *one* actor or
  request, and traces answer them, as long as the trace is recorded when it
  matters: that is Part 2 (watching, keeping errors).
- **Operator.** Granary's health (`src/lib/server/ops-health.ts`, ADRs 0088,
  0100, 0123) reads its *own* queues (outbox and inbox ages in SQLite),
  dead letters counted by a hook, quarantined actors by walking every actor
  every 10 s, event-loop lag p99 and RSS. From tinyactors it wants a few
  cheap numbers: failures, faulted actors, backlog, memory. The walk to
  count faulted actors is a sign of a missing counter.
- **Capacity planner.** Region bytes, actors, events per second and
  pool use are in `ta_system_stats` already. What is missing is *which chart*
  holds the actors and burns the fuel.
- **C embedder.** Wants plain structs to read (`ta_system_stats` is that),
  and must never pay per event, per actor, for a clock, for OTLP, or for an
  allocation. The PostgreSQL extension runs one system per statement and
  disables tracing; a game reads fuel per frame from `ta_run.used`.
- **Maintainers.** The torture chamber already reports throughput and
  region from `ta_system_stats`. Per-chart fuel would let it say which
  chart (null-datamodel storms or ECMAScript pingers) costs what. Nothing
  else is needed.
- **Backend.** Every attribute value multiplies series. A classic
  Prometheus histogram is one series per bucket: 32 log2 buckets × charts ×
  systems is expensive, and most backends get latency histograms from
  spans already (Tempo's metrics-generator, the Collector's `spanmetrics`
  connector). Prometheus wants cumulative temporality; OTLP-to-Prometheus
  translation adds `_total` and unit suffixes and drops curly-brace units.

**Served first: the operator and the capacity planner, within the C
embedder's "never pay" rule.** They are the ones with no other source:
traces are sampled in production, so rates and failure counts cannot come
from spans; and their numbers must be on even when nobody installed
anything. The developer is served by traces and Part 2; the maintainers by
what the operator gets; the backend's limits shape every choice below.

**Out of scope:** metrics per actor, per actor name, per session or per
event name (unbounded); histograms inside core; state occupancy maintained
per microstep (inspection answers "which actors are in which state" on
demand); mailbox wait per event; delta temporality; SLO computation (the
application's request span and the backend do that); metrics from the
PostgreSQL extension (later, with its tracing).

### 1.2 Each question, and what answers it

Preference order: spans already answer it; existing stats answer it; a
new counter in core, off the event path; a new instrument on the event
path (none survive).

| Question | Perspective | Answered by | New in core? |
|---|---|---|---|
| Which step is slow, p50/p99 per chart and event | developer, operator | spans: `scxml.macrostep <event>` with `tinyactors.chart.name`; the backend's spanmetrics, or the encoder's opt-in histogram (1.5) | no |
| Why did this actor do that | developer | traces, with Part 2's watch and error retention | no (Part 2) |
| Events per second, per system | operator, planner | `ta_system_stats.delivered`, `turns` | no |
| Events and fuel per chart: which chart is busy, which costs CPU | operator, planner, maintainers | — | **yes: per-chart `macrosteps`, `fuel`, `turns`, counted once per turn** |
| Errors and faults, per chart, whatever the sampling | operator, embedder | — (spans only show sampled ones) | **yes: per-chart error and fault counters, on failure paths only** |
| Actors per chart, resident and hibernated | planner, operator | totals in `ta_system_stats`; per chart — | **yes: per-chart gauges, at spawn, end, hibernate, wake** |
| Is a pool about to fill | operator, embedder | `ta_system_stats` (used and capacity of each pool, journal, contexts) | no |
| Backlog: is it overloaded | operator | `runnable`, `events` used vs capacity, `outbox` in `ta_system_stats` | no |
| Are timers late | operator | overdue = `now − ta_system_next_deadline()` when positive, read at pull | no (computed at pull) |
| Does a run block the host loop | operator | the binding's clock around `ta_host_run` | no (binding) |
| Memory per actor | planner | `region_bytes`, `slab_pages` / `actors` | no |
| Are records lost, is the exporter failing | operator | `trace_records_lost`, `trace_contexts_dropped`; the exporter's counts | no (binding counts its own) |
| How much of the traffic is traced | operator | — | **yes, in Part 2: traces started, sampled, promoted (at trace start only)** |
| Did a commit cost throughput | maintainers | the torture chamber over `ta_system_stats` | no |

### 1.3 What core gains: one block per chart

```c
/* tinyactors/stats.h: the stats structs, no functions. system.h and otlp.h
 * both include it, so otlp reads stats without depending on system
 * (ADR 0006, 1). ta_system_stats and ta_host_stats move here unchanged. */
typedef struct ta_chart_stats {      /* 96 B */
    uint32_t size, chart;
    uint32_t actors, hibernated;     /* gauges: resident, hibernated */
    uint64_t spawned, ended;         /* sessions started; finished, destroyed or faulted */
    uint64_t turns, macrosteps, fuel;
    uint64_t errors_execution, errors_communication, errors_platform;
    uint64_t faults;                 /* actors reclaimed after a fault (TA_EFFECT_ERROR) */
} ta_chart_stats;

/* tinyactors/system.h */
TA_API void ta_system_chart_stats_get(const ta_system *system, uint32_t chart, ta_chart_stats *out);
/* tinyactors/host.h: by chart name, every segment and version summed, with
 * the totals of retired segments kept, so a cumulative sum never goes back
 * when a version retires. */
TA_API ta_status ta_host_chart_stats_get(const ta_host *host, ta_string_view chart,
                                         ta_chart_stats *out);
```

It is the same pull as `ta_system_stats_get`: O(1), a copy, no view to
keep, between runs or not. The block is carved by the layout function per
chart in the image, lives in the region, relocates by `memcpy`, and is in
snapshots, so cumulative counts survive a restart through restore.

Where each field moves, and what it costs:

| Field | Moved at | Cost |
|---|---|---|
| `turns`, `macrosteps`, `fuel` | `ta_system_run`, next to `stats_turns++`: the turn's `used` is known, and `run_actor` already has the instance; `macrosteps` is the instance's own step counter at the end of the turn minus at its start (ADR 0006 keeps it in the header) | per turn: two loads, a subtraction, three adds; nothing per event |
| `errors_*` | the kernel's `error_exec` and `ta_core_raise_platform`, and where the system raises `error.communication` | only when something fails |
| `faults` | the `TA_EFFECT_ERROR` branch of `run_actor` | only on a fault |
| `actors`, `hibernated`, `spawned`, `ended` | spawn, done, terminate, reclaim, hibernate, wake, export, adopt | per lifecycle change, not per event |

Memory: 96 B per chart per system (64 charts: 6 KB). Per actor 0, per
event 0. A turn runs one actor for up to `quantum` fuel (1000 by default)
and usually several events, so the per-turn adds are under one instruction
per event. ADR 0006's rule stands: **tracing off, metrics on: no
measurable change in throughput**, measured with the torture chamber's
`throughput` scenario side by side with the commit before. If it is
measurable, the counters move behind a config switch.

`faults` replaces granary's walk over every actor to count quarantined
ones; `errors_communication` counts what 0.1 called dead letters (an
event for a session that is gone).

### 1.4 Instruments on the wire

All cumulative. Per chart, attribute `tinyactors.chart.name`; per system,
the resource's `tinyactors.system.id` (OTLP's single-writer rule). Names
follow 0.1 where the meaning is the same.

| Metric | Kind | Unit | From | Serves |
|---|---|---|---|---|
| `tinyactors.actors` (`tinyactors.actor.state`: `resident`, `hibernated`) | gauge | `{actor}` | `ta_chart_stats` | planner, operator |
| `tinyactors.actors.spawned`, `tinyactors.actors.ended` | sum | `{actor}` | `ta_chart_stats` | planner |
| `tinyactors.macrosteps` | sum | `{step}` | `ta_chart_stats` | operator (rate per chart), planner |
| `tinyactors.fuel` | sum | `{fuel}` | `ta_chart_stats` | planner, maintainers (CPU share per chart) |
| `tinyactors.errors` (`scxml.error.type`) | sum | `{error}` | `ta_chart_stats` | operator, embedder |
| `tinyactors.faults` | sum | `{actor}` | `ta_chart_stats` | operator |
| `tinyactors.pool.usage`, `tinyactors.pool.limit` (`tinyactors.pool`: `actors`, `events`, `timers`, `workspaces`, `requests`, `slab`, `outbox`, `trace.journal`, `trace.contexts`) | gauge | `{item}`, `By` for slab and journal | `ta_system_stats` | operator, embedder |
| `tinyactors.runnable` | gauge | `{actor}` | `ta_system_stats` | operator |
| `tinyactors.memory.usage` | gauge | `By` | `region_bytes` | planner |
| `tinyactors.timers.overdue` | gauge | `s` | `now − next_deadline` at pull | operator |
| `tinyactors.trace.records.lost`, `tinyactors.trace.contexts.dropped` | sum | `{record}`, `{context}` | `ta_system_stats` | operator |
| `tinyactors.traces` (`tinyactors.trace.decision`: `started`, `sampled`, `promoted`) | sum | `{trace}` | `ta_system_stats` (Part 2) | operator |
| `tinyactors.messages.routed`, `tinyactors.actors.migrated` | sum | | `ta_host_stats` | operator |
| `tinyactors.run.duration` | histogram, 8 buckets | `s` | the binding, around each `ta_host_run` | operator |
| `tinyactors.telemetry.bytes` (`tinyactors.telemetry.outcome`) | sum | `By` | the binding's exporter | operator |

Series: about 8 per chart plus about 25 per system. 20 charts: about 185
series per system, no histogram buckets from core. The one histogram is
per system, 8 buckets.

### 1.5 Distributions come from spans

Durations are exact in spans: the encoder maps each record's fuel stamp to
wall time (ADR 0006, 9). Two ways to turn them into histograms, neither in
core:

- **In the backend** (default): Tempo's metrics-generator or the
  Collector's `spanmetrics` connector, keyed by span name and
  `tinyactors.chart.name`. Nothing to build.
- **In the encoder, opt-in** (`TA_OTLP_SPAN_METRICS`, `telemetry: {
  spanMetrics: true }`): `ta_otlp` keeps, per chart, a histogram of
  macrostep durations, added to when it ends a macrostep span it is
  writing anyway. A bucket's exemplar is that span's trace and span ID, so
  every exemplar points at a span that was exported. For users with no
  collector. Memory is the encoder's (about 300 B per chart in its block);
  core pays nothing.

Under head sampling at ratio *p*, span-derived *counts* are those of the
sampled traces; latency *percentiles* stay unbiased. Rates therefore come
from the core counters (1.4), latency from spans, and `tinyactors.traces`
says what share was sampled.

### 1.6 Encoding: an ExportMetricsServiceRequest in `src/otlp`

One protobuf writer for every signal, fed with plain metric descriptions.
`otlp` also maps the stats structs (it reads `tinyactors/stats.h`), so the
binding never builds the names itself.

```c
/* tinyactors/otlp.h */
typedef struct ta_otlp_metric_point {
    const ta_otlp_attribute *attributes;
    uint32_t attribute_count, flags;         /* TA_OTLP_POINT_INTEGER, ... */
    double value;
    int64_t integer;
    uint64_t count;                          /* histograms */
    double sum, min, max;
    const double *bounds;                    /* bucket_count - 1 of them */
    const uint64_t *buckets;
    uint32_t bucket_count, exemplar_count;
    const ta_otlp_exemplar *exemplars;       /* trace ID, span ID, time, value */
} ta_otlp_metric_point;
typedef struct ta_otlp_metric {
    ta_string_view name, unit, description;
    uint32_t kind;                           /* TA_OTLP_METRIC_SUM, _GAUGE, _HISTOGRAM */
    uint32_t point_count;
    const ta_otlp_metric_point *points;
    uint64_t start_ns, time_ns;
} ta_otlp_metric;
TA_API ta_status ta_otlp_add_metric(ta_otlp *otlp, const ta_otlp_metric *metric);

/* The stats of one system as the metrics of 1.4. */
typedef struct ta_otlp_stats_input {
    uint32_t size, source;
    const ta_image *image;                   /* chart names */
    const ta_system_stats *system;
    const ta_chart_stats *charts;
    uint32_t chart_count, pad;
    const ta_host_stats *host;               /* or NULL */
    uint64_t start_ns, time_ns, ticks_per_second;
} ta_otlp_stats_input;
TA_API ta_status ta_otlp_add_stats(ta_otlp *otlp, const ta_otlp_stats_input *input);

TA_API size_t ta_otlp_metrics_size(const ta_otlp *otlp);
TA_API size_t ta_otlp_put_metrics(ta_otlp *otlp, void *out, size_t size);
```

- Memory as for traces: `ta_otlp_config.metric_bytes` (default 64 KiB);
  `TA_ERROR_NO_MEMORY` means relocate; size, then write.
- The message is 0.1's: `resource_metrics` (1) → `resource` (1),
  `scope_metrics` (2) → `scope` (1), `metrics` (2); `Metric` with `name`
  (1), `unit` (3), and `gauge` (5), `sum` (7: `aggregation_temporality` 2 =
  cumulative, `is_monotonic` 3) or `histogram` (9);
  `NumberDataPoint` (`start_time_unix_nano` 2, `time_unix_nano` 3, `as_int`
  6, `attributes` 7); `HistogramDataPoint` (`count` 4, `sum` 5,
  `bucket_counts` 6, `explicit_bounds` 7, `exemplars` 8, `attributes` 9,
  `min` 11, `max` 12); `Exemplar` (`time_unix_nano` 2, `span_id` 4,
  `trace_id` 5, `as_double` 3).
- Cumulative only: Prometheus and Mimir want it, and a collector converts
  for a backend that wants delta. A counter that went down (a restore from
  an older snapshot) starts a new stream with a new start time.
- `ta_otlp_next_metric` decodes, for tests in C and JavaScript.

### 1.7 The JavaScript API

```js
const system = createSystem({
  telemetry: {
    endpoint: 'http://localhost:4318',   // /v1/traces, /v1/logs and /v1/metrics
    metricsInterval: 60000,              // OTEL_METRIC_EXPORT_INTERVAL; 0: none
    spanMetrics: false,                  // the encoder's duration histograms (1.5)
    metrics: () => [/* the application's own Metric objects, exported with ours */],
  },
});
const snapshot = system.metrics();       // 0.1's shape: { startTime, time, metrics: Metric[] }
const bytes = metricsToOTLP(snapshot, { resource: { 'service.name': 'granary' } });
```

- `system.metrics()` returns 0.1's `MetricsSnapshot` (`index.d.ts` at
  `37c6514f`), built from `ta_host_stats_get`, `ta_host_chart_stats_get`
  and the binding's own. An SDK user reads it through observable
  instruments.
- `metricsToOTLP` keeps 0.1's signature; its writer moves to C
  (`ta_otlp_add_metric`). Granary's ops metrics (`signals-out.ts`) keep
  working unchanged.
- The exporter adds `/v1/metrics`, with the same gathering, one request in
  flight and quiet failures. The Node.js entry reads
  `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT`, `OTEL_METRIC_EXPORT_INTERVAL` and
  `OTEL_METRICS_EXPORTER=none`. `stop()` exports a last snapshot, and
  `flushTraces()` waits for it.

### 1.8 Rejected, and why

| Idea | Who it was for | Why not |
|---|---|---|
| Counters per event (macrosteps, microsteps, sends) in the kernel | operator | the same rates come once per turn; per event is 4 increments × every event |
| Log2 histograms in core (macrostep fuel, mailbox depth, mailbox wait, timer lateness) with exemplar slots | operator | about 20 instructions per event and 2.4 KB per chart, and 32 series per chart in Prometheus; durations are exact in spans already |
| Mailbox wait stamped on the trace context | operator | a store and a load per event; backlog shows in `runnable` and the event pool, and latency in spans |
| State occupancy maintained per microstep (0.1) | developer, operator | cost grows with the chart; a walk with `ta_system_actors` and `ta_actor_config` answers "which state" when asked |
| Metrics per actor or per actor name | developer | unbounded series; inspection answers "which ones" |
| Counting from trace records | operator | depends on sampling, and records cost far more than increments |
| Delta temporality in core or in the encoder | backend | Prometheus wants cumulative; a collector converts |

---

## Part 2: watching and sampling as policy

### 2.1 What is wanted

- 0.1's `watchTraces({ sessions, families, addresses })`: matching
  deliveries traced in full whatever the ratio, and what they cause, until
  `stop()`.
- Per-chart and per-actor-name ratios and detail; "never trace this chart"
  (granary 0093).
- Always keep errors (granary 0121), without tracing everything.
- Rate limits: at most N traces per second.
- Head and tail decisions, and room for policies not thought of yet.

What the perspectives of 1.1 add:

- **The on-call operator** turns detail up for one chart or actor while
  an incident lasts, and forgets to turn it down. A watch must end by
  itself (`for`) and be bounded (`perSecond`), so a forgotten watch on a hot
  chart cannot flood the journal or the bill.
- **The operator** sees errors in `tinyactors.errors` (1.4) whatever the
  sampling, and needs the trace of at least some of them: keeping errors
  is a decision at the failure point (2.5), rate-limited like any rule.
- **The operator and the backend** need to know what share was traced, to
  read span-derived numbers (1.5): `ta_system_stats` gains
  `traces_started`, `traces_sampled` and `traces_promoted`, counted at trace
  start and at promotion only.
- **The developer** wants full detail and values for one actor in tests and
  in granary's trace explorer: a watch with `values: true`, values for that
  class only.

Under the rules: no callback from core; tracing off stays one branch per
boundary and 0 bytes per event; records-only at full detail stays at about
90% of untraced; one way to do each thing.

### 2.2 The two building blocks every option needs

**A. A detail class on the context.** Today the kind mask is one per
system. To give one trace full detail and another none, the decision must
travel with the trace. `ta_trace_context.flags` and `ta_trace_entry.flags`
are 32 bits; W3C owns the low 8. Bits 8 to 10 hold a class, 0 to 7, and
the system keeps a kind mask per class. A step's events copy the
macrostep's flags (`ta_core_step_context`), so the class follows
everything a watched step causes, through sends, timers, invocations and
autoforward, at no cost: this is how "what a watched delivery causes is
watched too" works without a check per event.

**B. Decision points.** A decision is made only where something new
starts. Everywhere else it is read from the context.

| Point | Where | What is known there |
|---|---|---|
| **start** | `ta_system_start_trace` (host input, spawn, bootstrap, timer of an untraced send) | the target actor and chart, the event name, the origin, the caller's `traceparent` and its flag |
| **take** | `run_actor`, `TA_STEP_STABLE`, just before `ta_core_deliver` | the actor, its chart, its session and name, the event and its context |
| **failure** | the kernel's `error_exec`, `ta_core_raise_platform` | the actor, chart, macrostep and its context |
| **spawn / adopt** | `ta_system_spawn`, invoke, `ta_host_adopt` | the new actor's chart and name (for name rules, 2.5) |

Internal events (`<raise>`) stay inside their macrostep, so take covers
every event that starts one.

### 2.3 Is a W3C flag bit safe?

No. W3C Trace Context reserves trace-flags bits 2 to 7 for future flags
(Level 2 took bit 1 for `random`). A vendor bit there would go into every
`traceparent` we write and could clash with the next W3C flag. Bits 8 and
up of our 32-bit flags never leave the process: `ta_traceparent_format`
writes the low byte only, and the encoder writes `Span.flags` from `flags &
0xFF` plus OTLP's own 0x100 (`otlp.c`; keep it so: OTLP gives bits 8 and 9
of `Span.flags` their own meaning). The class is local by construction.
Carrying it to another process is W3C's `tracestate`'s job (a vendor key
such as `tinyactors=d:2`), not trace-flags. Left for later.

### 2.4 The options

| Option | Lives in | Hot path | Fits no-callback | Covers |
|---|---|---|---|---|
| **1. A host callback per event or per trace** | host | a call per event | no | anything |
| **2. The host marks its inputs** (0.1's watch: sampled flag or a class on `ta_event_in`) | host | 0 | yes | host inputs only: not a send from actor A to a watched actor B in the same system, which core routes |
| **3. A hard-coded watch set** (sessions, charts) in the system | system | a set lookup per delivery while a watch is active | yes | watches only; every new policy is a new special case |
| **4. A rule table evaluated at decision points**, compiled into the system block, decisions cached in the context and the actor record | system; class in trace; kernel reads one mask per step | one branch per decision point with no rules; a short scan with rules | yes | watches, per-chart and per-name ratios and detail, rate limits, errors (at failure), "never this chart" |
| **5. A policy bytecode** (a small verified VM run at the same points) | system | about the same as 4 per evaluation | yes | anything 4 does, and combinations a table cannot say |
| **6. Tail decisions in the encoder** (records kept until a trace ends, then kept or dropped) | otlp / binding | every record written for every trace: full tracing's cost (10% at full detail) for all traffic | yes | errors, slow traces; but an actor trace has no end, so memory is unbounded or the "trace" is one macrostep |
| **7. Tail decisions in a collector** (OpenTelemetry Collector `tail_sampling`) | outside | what is exported | yes | whatever the collector does, for those who run one |
| **8. A mask per actor** (4 B per actor) or **a full mask per context** | system / context | a load per record | yes | per-actor detail; costs 4 MB per million actors, or 4 B more per context entry |

- **1** is the "something dumb" the maintainer warned of, and core may not
  call back.
- **2** stays, as a building block: router envelopes carry the class
  between segments (2.5, "Where decisions live"). It cannot be the whole answer.
- **3** is 0.1's shape, and the next policy is another list.
- **5** buys generality nothing asks for yet. It needs a verifier, an
  assembler in every binding, and fuel bounds. The rules of 4 are data, so
  they can be compiled to bytecode later behind the same public struct.
- **6** in general is unbounded for actors. Its useful core, "errors
  always", is a decision at the failure point (2.5), which costs nothing
  until something fails.
- **7** needs no code: everyone who has a collector can already do it.
- **8** classes do the same with 3 bits and a table of 8 masks.

**Recommendation: option 4**, with classes (A), the four decision points
(B), errors as a decision at the failure point, and tail decisions left to
a collector.

### 2.5 The recommended design

#### Classes and the step's mask

```
context flags (u32):  31 ........ 12 | 11       | 10..8  | 7..0
                       unused          promoted   class    W3C trace-flags (sampled, random)
```

| Class | Default kinds | Used for |
|---|---|---|
| 0 | `TA_TRACE_LIFECYCLE` | unsampled: carried, only lifecycle written (today's behaviour) |
| 1 | `TA_TRACE_FULL` (the old `trace_kinds`) | sampled |
| 2 | `TA_TRACE_FULL` | watched (the binding asks for values when the watch does) |
| 3–7 | 0 | what rules and watches define (`detail: 'off'` is a class with kinds 0) |

The kernel stops testing the system mask and the context separately. It
keeps one mask for the step it is running:

```c
/* init_k (once per ta_core_step), begin_macrostep, and promotion: */
core->step_kinds = instance->trace == TA_NIL
    ? 0 : core->class_kinds[(ta_trace_entry_at(core, instance->trace)->flags >> 8) & 7];
/* every record site, and the two TA_TRACE_VALUES tests: */
if (core->step_kinds & TA_TRACE_BIT(kind)) ...
```

Today a record site tests `trace_kinds & bit`, then `instance->trace !=
TA_NIL`. Afterwards it tests one word. The new cost is one branch and one
load per `ta_core_step` call (two or three per event), where the entry's
line is about to be used anyway. Tracing off: no contexts, so `step_kinds`
is 0 and every boundary is the one branch it is today. Records-only at full
detail: one test per record instead of two. `TA_TRACE_VALUES` per class
gives "values for watched traces only", which is what ADR 0006's privacy
concern asks.

Lifecycle records (written by the system) test the class of the actor's
context, or class 0 without one, so "lifecycle whatever the sampling"
stays the default and a chart in a `detail: 'off'` class writes none.

#### Rules

```c
/* tinyactors/system.h */
enum { TA_TRACE_AT_START = 1u, TA_TRACE_AT_TAKE = 2u, TA_TRACE_AT_FAILURE = 4u };
enum { TA_TRACE_KEEP = 0, TA_TRACE_SAMPLE = 1, TA_TRACE_DROP = 2, TA_TRACE_RATIO = 3 };
enum { TA_TRACE_FROM_HOST = 1u, TA_TRACE_FROM_ACTOR = 2u, TA_TRACE_FROM_TIMER = 4u,
       TA_TRACE_FROM_SPAWN = 8u, TA_TRACE_FROM_SAMPLED = 16u, TA_TRACE_FROM_UNSAMPLED = 32u };

typedef struct ta_trace_rule {
    uint32_t size;
    uint32_t at;                 /* TA_TRACE_AT_*: the points it is asked at */
    /* what it matches; an empty view or 0 matches anything */
    ta_string_view chart;        /* a chart's name; a system without it never matches */
    ta_string_view actor;        /* an actor's name; a trailing '*' makes it a prefix */
    ta_string_view event;        /* an event descriptor, SCXML's prefix rule ("error", "webhook.*") */
    uint64_t session;
    uint32_t from;               /* TA_TRACE_FROM_*, 0: any origin */
    /* what it decides */
    uint32_t sample;             /* TA_TRACE_KEEP, _SAMPLE, _DROP, _RATIO */
    uint32_t sample_parts, sample_whole;
    uint32_t detail;             /* the class to give, TA_NIL: keep the one it has */
    uint32_t per_second, burst;  /* matches at most this often (logical time); 0: no limit */
} ta_trace_rule;

typedef struct ta_trace_policy {
    uint32_t size;
    uint32_t sample_parts, sample_whole; /* traces the system starts; whole 0: 1 / 1 */
    uint32_t kinds[8];                   /* each class's kinds (the defaults above when all 0) */
    uint32_t rule_count;                 /* at most ta_system_config.trace_rules */
    const ta_trace_rule *rules;          /* asked in order; at each point the first match decides */
} ta_trace_policy;

/* Replaces the kinds, the ratio and the rules between runs (in place of
 * today's (kinds, parts, whole)); open spans are kept. TA_ERROR_NO_MEMORY:
 * more rules than trace_rules (relocate with more). */
TA_API ta_status ta_system_trace_set(ta_system *system, const ta_trace_policy *policy);
TA_API ta_status ta_host_trace_set(ta_host *host, const ta_trace_policy *policy);
```

`ta_system_config` changes in place: `trace_kinds`, `trace_sample_parts`
and `trace_sample_whole` go; `trace_rules` (capacity, default 16, at most
255) and `const ta_trace_policy *trace_policy` (the initial one, NULL for
the defaults; a host pointer copied at init, like `io_processors`) come.
Every caller moves (`git grep trace_kinds`: system, host, bindings,
torture chamber, tests).

**Compiled form, in the system block** (no pointers; chart names become
indices with `ta_image_chart_by_name`, actor names slab strings, event
descriptors the event trie's preorder interval `[node, end)` when the name
is in the trie, else a slab string compared by prefix):

```c
typedef struct ta_trace_rule_compiled { /* 72 B */
    uint8_t at, sample, detail, from;
    uint8_t actor_prefix, pad[3];
    uint32_t chart;                      /* TA_NIL: any */
    uint32_t event_first, event_end;     /* trie interval; TA_NIL: any */
    uint32_t actor, event_text;          /* slab strings, TA_NIL: none */
    uint64_t session;
    uint64_t threshold;                  /* ta_trace_ratio_threshold(parts, whole) */
    uint64_t tokens, refilled;           /* the rate limit's bucket (logical time) */
    uint32_t per_second, burst;
} ta_trace_rule_compiled;
/* and: uint8_t points (any rule at each point), uint8_t chart_points[chart_count]
 * (a rule at each point can match this chart), uint32_t class_kinds[8] in ta_core */
```

**Evaluation.** At each point: `if (!(system->points & AT)) skip` (one
branch with no rules); else `if (!(chart_points[chart] & AT)) skip` (one
byte); else scan the point's rules in order. A match decides:

- `SAMPLE`, `DROP`: set or clear the sampled flag.
- `RATIO`: `ta_trace_sampled(trace, threshold)`. The ratio is
  TraceIDRatioBased on the trace ID, the same in every host. It is also
  *consistent*: a trace sampled at 1% is sampled at every ratio above 1%.
  So a chart at 10% under a 1% default records every trace the root
  recorded, plus its own share.
- `detail`: the class.
- A rate limit: the bucket refills `(now − refilled) × per_second` tokens
  (capped at `burst × ticks_per_second`), and a match costs
  `ticks_per_second` of them; an empty bucket makes the rule not match, so
  the next rule decides. "Sample up to 5 traces per second, else 1%" is two
  rules. Integers only, no division, deterministic in logical time.

At *start* the decision shapes the new context. At *take*, if it differs
from the event's context, the event gets a copy with the new flags (one
pool entry; the shared entry of the sending step is not changed, so its
other events keep theirs); when the event has no context, a new one is
made. At *failure*, see below.

**Per-actor caching.** A rule that names a session or an actor name is
about one actor. At take, comparing a session is cheap, but an actor name
is a string. The answer is cached in the actor record, in bits that are
free today: `flags` bit 6 (`ACTOR_FLAG_TRACE_RULE`: "an actor rule matched")
and the `uint16_t pad` (the rule's index, and the low 8 bits of the policy
generation it was decided under). Spawn, adopt and the first take after a
policy change decide it; later takes read it. `ta_system_trace_set`
increments the generation, so stale caches are noticed at their next take
and no walk over the actors is needed (on the 256th change the system
clears every actor's cache once, O(actors), between runs). Hibernation
keeps the actor record, so a watch survives it.

#### Errors: a late head decision at the failure point

"Always keep errors" without recording everything: when an action fails
(or core raises `error.communication` or `error.platform`) in a macrostep
whose class does not write `ACTION_FAILED`, and an `AT_FAILURE` rule
matches, the macrostep is *promoted*:

1. The instance's context is replaced by a copy with the sampled flag, the
   rule's class and `TA_TRACE_FLAG_PROMOTED`; without a context, the
   system starts a trace (through the kernel's existing internal hook to
   its system, as `serial_of` and `name_of` are; not a public callback).
2. The kernel writes `MACROSTEP_BEGIN` now, late, then `ACTION_FAILED`, and
   the rest of the macrostep as its class says. The encoder opens the
   macrostep span at the failure and marks it
   `tinyactors.trace.promoted = "failure"`.
3. What the failing step causes from then on is recorded, because its
   events copy the promoted flags: the `error.execution` handling, the
   replies, the downstream actors.

The span's parent (the cause) was not recorded, so the backend shows it as
a root whose parent is missing. A new trace linked to the old one is the
alternative (decision 4). Cost: nothing until something fails; then one
context and one record.

#### Unsampled traces keep their context

Today an unsampled trace gets no context and its trace ID is lost. So an
effect carries no `traceparent`, and a downstream service with a
parent-based sampler starts its own trace and may sample a fragment the
root did not. W3C and OpenTelemetry expect `traceparent` with flag `00` to
be propagated. With classes, an unsampled trace keeps a context in class 0:
the trace ID flows, effects carry `-00`, a later rule at take or failure
can sample it inside the same trace, and lifecycle records name the real
trace instead of a fresh one. The cost is ADR 0006's "carrying contexts
with no records": 2% of the sampled-out path, and a pool entry per event in
flight (capacity that exists already). Decision 3.

#### Where decisions live, and what survives

| State | Where | Hibernation | Relocation, snapshot | Export / adopt | Across segments | Across processes |
|---|---|---|---|---|---|---|
| class, promoted | context flags bits 8–11 | in the region | `memcpy` | the blob's `trace_flags` is already u32 | `ta_event_in.trace_detail` (new) in router envelopes | `tracestate` (later); else the receiver's rules decide |
| actor cache | actor record bit 6 and pad | kept (the record stays) | `memcpy` | not carried: adopt decides again | – | – |
| rules, buckets, class masks | system block | – | `memcpy`; in snapshots | – | `ta_host_trace_set` sets every segment | – |

A rule names charts by name, so one policy fits every segment of a host
and every version of a chart.

#### Cost on the hot path

| Point | How often | No rules | With rules |
|---|---|---|---|
| record site | each boundary | one branch (as today) | the same |
| step mask | each `ta_core_step` call | one branch, one load | the same |
| start | each new trace | one branch | a scan of the start rules (integer compares; a string prefix only for an event not in the trie) |
| take | each macrostep of an external event | one branch | one byte (`chart_points`), the actor's cache bit, a short scan; one pool entry when the decision changes |
| failure | each failure | one branch | one context and one record when promoted |
| spawn, adopt | each | one branch | a string compare per name rule |

Bytes per event: 0. Bytes per actor: 0. Per system: 72 B per rule (16
rules: 1,152 B), one byte per chart, 32 B of class masks in `ta_core`.
Records-only at full detail: one test per record fewer, one load per step
more; expected unchanged, measured against ADR 0006's table.

### 2.6 The JavaScript API

```js
system.configureTraces({
  detail: 'decisions', sample: 0.01,              // as today: class 1 and the default ratio
  rules: [
    { chart: 'telemetry-sink', detail: 'off' },   // never this chart (granary 0093)
    { at: 'failure', detail: 'summary' },         // every failure, with what follows it
    { at: 'start', from: 'host', sample: true, perSecond: 5 },
    { chart: 'issue', sample: 0.1 },              // consistent with the 1% default
  ],
});

const watch = system.watchTraces({
  actors: [actor], sessions: ['42'], names: ['issue/owner/repo#7', 'relay/*'],
  charts: ['issue'], events: ['webhook.*'],
  detail: 'full', values: true,                   // the watch's class (default: full, no values)
  for: 15 * 60 * 1000, perSecond: 50,             // ends by itself; at most 50 new traces a second
});
watch.stop();
```

- `rules` map one to one onto `ta_trace_rule` (`at`: `'start' | 'take' |
  'failure'`, default `['start', 'take']`; `sample`: `true`, `false` or a
  ratio; `detail`: a preset, `'off'`, or `{ kinds, values }`; `perSecond`,
  `burst`).
- `watchTraces` puts rules in front of the application's: `sample: true`
  at start and take, with the watch's class. `stop()` takes them out with
  `ta_host_trace_set`; `for` (default 15 minutes, `Infinity` to keep it)
  stops it by itself on the system's clock; `perSecond` is the rule's rate
  limit. Contexts already marked keep their class until their traces end,
  as 0.1's did.
- The binding hands out classes 2 to 7 to the distinct `detail` and
  `values` combinations in use and refuses a seventh (`RangeError`).
- `setTraceSink` and `configureTraces` keep their options; `sample`,
  `detail`, `kinds` and `values` become class 1's mask and the policy's
  ratio. 0.1's `unsampled: true` is `{ at: 'start', from: 'unsampled',
  sample: true }`.

---


## Increments

Each builds and passes `mise run test`. While building, the narrowest task
covers each step (a core test program, `node:test`, `conformance:js`).
Increments that touch the event path (3 and 4) are measured with the
torture chamber's `throughput` scenario (`mise run torture`, one scenario),
side by side with the commit before, throughput and region together, and
the commit message gives the numbers.

1. **Per-chart counters.** `tinyactors/stats.h` (the stats structs move
   there), `ta_chart_stats` carved per chart, the per-turn adds, error and
   fault counts on their failure paths, lifecycle gauges,
   `ta_system_chart_stats_get`, `ta_host_chart_stats_get` with retired
   totals, `traces_started`/`_sampled` in `ta_system_stats`. Unit tests
   with exact counts for small charts, across hibernation, export and
   adopt, retirement, snapshot and restore. Measured: must be no change.
2. **The OTLP metrics writer and JavaScript.** `ta_otlp_add_metric`,
   `ta_otlp_add_stats`, size and put, `ta_otlp_next_metric`; golden bytes;
   `system.metrics()`, `metricsToOTLP` on the C writer (0.1's shape tests
   ported), `/v1/metrics` in the exporter, the `OTEL_*` variables,
   `telemetry.metrics`, `run.duration`. By hand: otel-tui shows them.
3. **Classes.** Flag bits 8–11, `class_kinds`, `core->step_kinds` at every
   record site and the values tests, `ta_system_trace_set(policy)` with no
   rules yet, the config fields moved and every caller migrated, the
   encoder masking local bits. ADR 0006's records-only row measured again.
4. **Rules at start and take, and watches.** The compiled table, chart,
   actor and event matching, ratios, rate limits, the actor cache, copy on
   change, `ta_event_in.trace_detail` through router envelopes;
   `configureTraces({ rules })`, `watchTraces` with `for` and `perSecond`.
   Tests: consistent ratios over fixed seeds; a bucket in logical time; a
   watched actor's macrosteps and what they cause are recorded in full,
   nothing else, across segments and hibernation; after `stop()` or `for`,
   nothing new is.
5. **Unsampled contexts and promotion.** Class 0 contexts, the failure
   point, the late `MACROSTEP_BEGIN`, promoted spans in the encoder,
   `traces_promoted`.
6. **Span metrics in the encoder** (opt-in, if decision 2 says so):
   per-chart macrostep duration histograms with exemplars in `ta_otlp`,
   `telemetry.spanMetrics`.

Each increment that settles a decision gets its ADR when it lands (one for
metrics, one for trace policy), and the ROADMAP item 2 lines move there.

## What granary would use

| Granary today | With this |
|---|---|
| counts quarantined actors by walking every actor every 10 s (`ops-health.ts`) | `tinyactors.faults` and `ta_host_chart_stats_get`: O(1) |
| dead letters counted by a hook | `tinyactors.errors{communication}` per chart |
| `system.stats()` for queued, delivered, memory | the same numbers in `system.metrics()`, and exported |
| ops decodes every batch to drop `telemetry-sink` spans (0093, `ops/system.ts` `filterSpans`) | `rules: [{ chart: 'telemetry-sink', detail: 'off' }]`; no decode |
| keeps error and lifecycle spans, the rest by trace-ID ratio, filtered on the wire (0121) | `sample: r` and `{ at: 'failure', detail: 'summary' }`; lifecycle is class 0's default; no re-encode |
| lowers the global ratio when a sink's volume budget is exceeded (0121) | `configureTraces({ sample })` as today, or a `perSecond` rule; `tinyactors.traces` shows the effect |
| looking at one issue in the trace explorer (0054) | `watchTraces({ names: ['issue/…'], detail: 'full', values: true, for: … })` |
| `metricsToOTLP` for ops' own gauges every 60 s (0121) | unchanged signature; `metricsToOTLP(system.metrics())` adds `tinyactors.*` to the same push |
| latency per step | spans, through Tempo's metrics-generator (granary already runs Tempo, 0099), or `spanMetrics: true` |

## Decisions for the maintainer

1. **Per-chart counters always on, with no switch.** They are counted once
   per turn and on failure paths, 96 B per chart. *Recommended: yes*; if
   the side-by-side measurement shows any change, they go behind a config
   switch instead.
2. **Span-derived duration histograms in the encoder**, opt-in, with
   exemplars that are always exported spans (increment 6), or leave
   latency histograms to the backend's spanmetrics entirely.
   *Recommended: build it, opt-in, last*: ROADMAP item 2 asks for
   exemplars, and users without a collector have no other way to get them.
3. **Unsampled traces keep a context** (class 0): `-00` propagates as W3C
   expects, a rule at take or failure can sample inside the same trace,
   lifecycle records name the real trace. About 2% on the sampled-out path
   (ADR 0006's "contexts with no records"). *Recommended: yes.*
4. **A promoted or upgraded macrostep keeps its trace ID**, so its parent
   span is missing in the backend, or it starts a new trace linked to the
   old one. *Recommended: keep the trace ID*: the trace stays one trace,
   and the span says `tinyactors.trace.promoted`.
5. **The in-place API changes**: `trace_kinds`, `trace_sample_parts` and
   `trace_sample_whole` leave `ta_system_config` for `trace_policy` and
   `trace_rules`; `ta_system_trace_set` and `ta_host_trace_set` take a
   `ta_trace_policy`; the stats structs move to `tinyactors/stats.h`.
   *Recommended: yes*, every caller migrated in the same change.

Settled without asking, unless you say otherwise: cumulative temporality
only; explicit histogram buckets; rules live in the region and so in
snapshots; `tracestate` for the class across processes is left for later;
8 classes and 16 rules by default; `flushTraces()` also waits for metrics.
