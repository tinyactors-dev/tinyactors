# ADR 0007: Metrics are per-chart counts; tracing is a policy of classes and rules

**Status:** Accepted (2026-10-10). Carries out ROADMAP "Next" item 2
(metrics, watching). Proposal:
[metrics and trace policy](../proposals/metrics-and-trace-policy.md).
**Date:** 2026-10-10

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md) and section 1.1 of the proposal:

- **First: the operator and the capacity planner.** In production, traces
  are sampled, so rates and failure counts cannot come from spans. These
  roles need a few numbers that are on even when nobody installed anything:
  which chart is busy, failing, or holding the actors, and whether a pool is
  about to fill.
- **Under the C embedder's rule:** nothing per event, nothing per actor, no
  clock, no OpenTelemetry needed. The PostgreSQL extension, a game and a
  microcontroller read plain structs.
- **The application developer** is served by traces: watching one actor or
  chart in full, and a policy that keeps or drops what the developer names.
  Developers are not served by metrics.
- **The observability backend:** about 8 series per chart and 25 per
  system, no histogram buckets from core, cumulative sums.
- **Out of scope:** metrics per actor, per actor name or per event name;
  histograms in core; state occupancy; mailbox wait per event; delta
  temporality; metrics of the PostgreSQL extension.
- **Who pays:** 104 bytes per chart per system (on 64-bit hosts); one
  branch per decision point with no rules; one test more per record (the
  step's class); a context per event of an unsampled trace. Measured below:
  no change in throughput that the torture chamber can see.

## Decisions

### 1. Per-chart counts, once per turn (`tinyactors/stats.h`)

- **Where they live.** A system keeps one `ta_chart_stats` per chart of
  its image, in the region:
  - gauges: actors (resident and hibernated);
  - counts: spawned, ended, turns, macrosteps begun, fuel, errors
    (`error.execution`, `error.communication`, `error.platform*`) and
    faults.
- **When they move:**
  - turns, macrosteps and fuel once per scheduling turn, in `run_actor`
    (macrosteps are the difference of the instance's own step counter);
  - errors where the kernel raises them;
  - faults on the `TA_EFFECT_ERROR` path;
  - the gauges on lifecycle changes.
- **Reading them:**
  - `ta_system_chart_stats_get` reads one chart's counts.
  - `ta_host_chart_stats` sums them by chart name over every segment. When
    a version retires, the host folds its counts into its totals, so a
    count never goes back.
- **What they survive.** Relocation and system snapshots keep them;
  host snapshots keep the retired totals.
- **Snapshot formats.** System snapshots go to version 3, host snapshots
  to version 2.
- **The structs move.** `ta_system_stats` and `ta_host_stats` move to
  `tinyactors/stats.h`, so the otlp codec reads stats without depending
  on `system`.
- **New system counts.** `ta_system_stats` gains `traces_started`,
  `traces_sampled` and `next_deadline`.

### 2. Metrics through the otlp codec; latency from spans

- **One protobuf writer.** `ta_otlp_add_metric` takes sums, gauges and
  histograms with exemplars as plain data. `ta_otlp_add_stats` turns
  stats into the metric set, and `ta_otlp_metrics_size` /
  `ta_otlp_put_metrics` write an ExportMetricsServiceRequest with one
  `tinyactors` scope.
- **Cumulative only.**
- **Reading it back.** `ta_otlp_next_metric`, `ta_otlp_next_point` and
  `ta_otlp_next_exemplar` read requests back.
- **The metric set:**
  - per chart: `tinyactors.actors`, `.actors.spawned`, `.actors.ended`,
    `.macrosteps`, `.fuel`, `.errors` (by `scxml.error.type`), `.faults`;
  - per system: `.pool.usage` / `.pool.limit` by pool, `.memory.usage` /
    `.memory.limit`, `.runnable`, `.timers.overdue`, `.events.delivered`,
    `.trace.records.lost`, `.trace.contexts.dropped`, `.traces` (started,
    sampled);
  - per host: `.messages.routed`, `.actors.migrated`;
  - in the bindings: `.run.duration` and `.telemetry.bytes`.
- **Durations come from spans,** never from core:
  - a collector's spanmetrics, or
  - opt-in, `TA_OTLP_SPAN_METRICS` with `ta_otlp_add_span_metrics`: the
    encoder keeps, per (system, chart), a histogram of the durations of
    the macrostep spans it writes, as `tinyactors.macrostep.duration`.
    Each bucket's exemplar is the latest of those spans, so an exemplar
    always points at an exported span.

### 3. JavaScript

- **`system.metrics()`** returns 0.1's snapshot shape: `{ startTime,
  time, metrics: [{ name, unit, kind, points }] }`.
- **`metricsToOTLP(snapshot, { resource })`** encodes any snapshot of
  that shape with the C writer.
- **The exporter** posts `/v1/metrics`:
  - every `telemetry.metricsInterval` ms (60000) and at `stop()`;
  - with what `telemetry.metrics()` returns;
  - with the encoder's durations when `telemetry.spanMetrics` is set.
- **The Node.js entry** reads `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT`,
  `OTEL_METRIC_EXPORT_INTERVAL` and `OTEL_METRICS_EXPORTER=none`.

### 4. Detail classes

- **Where the class lives.** A trace context carries a detail class in
  bits 8 to 10 of its flags. W3C's bits 2 to 7 are reserved, so a vendor
  bit there would leak into every traceparent; bits 8 and up never leave
  the process, because `ta_traceparent_format` writes the low byte.
- **What a class means.** `ta_trace_policy` gives each of the 8 classes
  its kinds, `TA_TRACE_VALUES` included.
- **How it travels.** A step's events copy the macrostep's flags, so the
  class follows everything the step causes.
- **The kernel's check.** The kernel tests the union of the classes'
  kinds first (one branch per boundary when tracing is off, as before),
  then the step's class.
- **The new config fields:**
  - `ta_system_config.trace_policy` replaces `trace_kinds` and the
    sampling fields;
  - `ta_system_trace_set` and `ta_host_trace_set` take a `ta_trace_policy`;
  - `ta_trace_policy_default` fills one.

### 5. Rules, asked where something new starts

- **What a rule is.** `ta_trace_rule` matches:
  - a chart;
  - an actor's name, or a prefix ending in `*`;
  - a session;
  - an event descriptor (SCXML prefix match: the trie's interval, or text
    when the descriptor is not in the trie);
  - where the event came from.

  It decides keep, sample, drop or a ratio of the trace ID; a class; and
  at most so many matches a second of logical time.
- **Where rules live.** Rules are compiled into the system block
  (`trace_rules`, 16 by default, 64 in the bindings).
- **Where rules are asked.** Only at two decision points:
  - `ta_system_start_trace`: host input, spawn, a timer of an untraced
    send;
  - `run_actor`, just before the kernel takes an external event.

  With no rules, each point costs one branch.
- **Caching.** An actor's name matches are cached in spare bits of its
  record (`trace_names`, at most 16 name rules). Setting a policy clears
  the cache with one walk over the actors.
- **Across segments.** The class crosses the segments of a host:
  `ta_event_in.trace_flags` and `ta_envelope.trace_flags`, which is never
  on the wire to peers.
- **In JavaScript:**
  - `configureTraces({ rules })`;
  - `system.watchTraces({ actors, sessions, names, charts, events, detail,
    values, for, perSecond })` puts rules in front of the application's.
    It ends after `for` ms (15 minutes by default) so a forgotten watch
    ends.
  - Classes 2 to 7 go to the distinct details in use.

### 6. Unsampled traces keep their context

- **The context stays.** An unsampled trace keeps a context in class 0.
  Its ID flows out as `traceparent ...-00`, so a parent-based sampler
  downstream continues the caller's decision.
- **What it writes.** Class 0 writes lifecycle records only by default.
- **A change for delayed sends.** A delayed send of an unsampled step stays
  unsampled when it fires. Before, it started a new trace under the ratio
  of that time.

### 7. A step sampled late begins a new trace; a failing step reports nothing

This is the maintainer's decision 4.

- **A take rule.** When a take rule samples a step of an unsampled trace,
  that step begins a new trace and is its root. No reported span names a
  parent that was never reported.
- **A failure.** A failing step of an unsampled trace reports nothing.
  Promotion at the failure point was not built:
  - a late span would need a `MACROSTEP_BEGIN` written after the fact and
    a link;
  - the encoder would open partial spans for unsampled steps;
  - reporting nothing is what the step's class already does.
- **Where failures show.** They are counted, always, in the chart's
  `tinyactors.errors`. A trace of one needs sampling, a rule, or a watch.

## Measured

- **Machine and builds.** Apple M4 Max (16 cores, 48 GB), macOS 26.6.2,
  release builds.
- **Workload.** The torture chamber's `throughput` scenario (30 s), three
  runs of each build interleaved, medians in events per second; region is
  the peak of both systems' blocks.
- **The baseline.** Each row compares an increment with the commit before
  it, built in a temporary worktree.
- **Comparing rows.** Rows of different days differ in absolute terms (the
  machine ran other work on the 9th: load average 3 to 10); compare within
  a row. Runs of one build vary by about ±1.5%.

| Increment | Mode | Before | After | Region |
|---|---|---:|---:|---|
| 1 per-chart counts (`e366bd6d`) | tracing disabled | 1.104M | 1.107M | 4.6 MB → 4.6 MB |
| 1 | records, full detail | 0.988M | 0.969M (load 10, ranges overlap) | 18.3 MB → 18.3 MB |
| 3 classes (`360dfea3`) | tracing disabled | 1.103M | 1.111M | 4.6 MB → 4.6 MB |
| 3 | records, full detail | 0.969M | 0.963M | 18.3 MB → 18.3 MB |
| 4 rules (`59d54bcd`) | tracing disabled | 1.110M | 1.114M | 4.6 MB → 4.6 MB |
| 4 | records, full detail | 1.002M | 0.999M | 18.3 MB → 18.4 MB |
| 5 unsampled contexts (`a6aa8836`) | sampled out (`traceSample=0`) | 1.489M | 1.489M | 18.4 MB → 18.4 MB |
| 5 | tracing disabled | 1.504M | 1.518M | 4.6 MB → 4.6 MB |
| 5 | records, full detail | 1.351M | 1.358M | 18.4 MB → 18.4 MB |
| 6 span metrics (`22b64598`) | OTLP encoding, span metrics off | 0.770M | 0.778M | 18.4 MB → 18.4 MB |

- **Increment 2** (`b1eeb700`, the metrics writer and JavaScript) changes
  no per-event path and was not measured.
- **Sampled-out baseline.** For increment 5 the baseline ran with this
  increment's bench tool, which adds `--param traceSample`; the library
  was its own. The 2% the proposal expected for unsampled contexts did not
  show in this scenario.

## Consequences

- **What every system gets.** Per-chart counts that say which chart is
  busy, failing or holding the actors, with no setup and no cost the
  torture chamber can see.
- **Granary:**
  - it can drop its walk over every actor to count faulted ones
    (`tinyactors.faults`);
  - its on-the-wire filter of `telemetry-sink` spans becomes a rule;
  - a trace explorer watches one issue with `watchTraces`.
- **Breaking:**
  - system snapshots before version 3 and host snapshots before version 2
    are refused;
  - `ta_system_config`'s trace fields, `ta_system_trace_set` and
    `ta_host_trace_set` changed;
  - `ta_event_in` and `ta_envelope` gained `trace_flags`;
  - a delayed send of an unsampled step stays unsampled.
- **Smaller than proposed:**
  - `tinyactors.run.duration` has no exemplars;
  - `metricsToOTLP` drops 0.1's `version` option: the scope is
    `tinyactors` and the library's version;
  - there is no `traces_promoted`, since nothing is promoted.

## Open questions and follow-ups

- **`tracestate`** could carry the class across processes; within a
  process it rides the envelope.
- **Waits and host work** (from the agent-workspace example, proposal
  section 9):
  - F2: host work cannot continue a trace. Actions have no traceparent,
    and a reply from host code starts a new trace. Core already puts the
    step's context on SEND, INVOKE, LOG and DONE effects. A host-carried
    invocation in the JavaScript API (F1) would give async work the
    invoking step's context.
  - F3: a wait (a model call, a child's life, days in a state) is not a
    span, so no span metric can give its latency. Span-derived histograms
    cover macrosteps only.
- **The PostgreSQL extension** still traces nothing and exports no
  metrics.
