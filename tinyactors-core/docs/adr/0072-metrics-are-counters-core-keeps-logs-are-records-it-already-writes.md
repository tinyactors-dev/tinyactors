# ADR 0072: Metrics are counters core keeps; logs are records it already writes

**Status:** Proposed. It would refine ADR 0045 (a log record carries its step key and trace context) and ADR 0067 (sampling becomes a per-kind mask; two of its open points close); it builds on ADR 0070 and ADR 0071, and on napi ADR 0016's encoder. Research: [`docs/research/otel-metrics-and-logs-for-actors.md`](../../../docs/research/otel-metrics-and-logs-for-actors.md).
**Date:** 2026-09-24

## Context

tinyactors answers OpenTelemetry's traces: core writes records to a trace
sink (ADR 0067), and napi turns them into OTLP spans (napi ADR 0016).
Looking at otel-tui showed there is no answer for the other two signals,
metrics and logs.

The design question is not "what could we emit" but what an on-call
engineer needs at 3am when actors are not working in production. The
research note sets out what other actor runtimes provide and what
operators use: queue length and the age of the oldest message, failure
and drop counts, timer lateness, activation counts, and a way to find
the worst individual actors on demand. None keeps per-actor metric
streams by default.

Each signal answers a different kind of question, and none should carry
everything:

- **Metrics** say that something is wrong, and where: which definition,
  which state, which kind of failure. Their attributes must have low
  cardinality.
- **Logs** record discrete, searchable facts, including the ones that
  name one actor, and carry the trace and span IDs that lead to the
  causal story.
- **Traces** tell that story, but in production they are sampled, so
  they cannot be the only place an error shows up.

What core has today:

- the trace records, which a sink receives only when it asks for their
  kinds, and only for sampled macrosteps unless it sets `unsampled`, which
  applies to every kind at once;
- `ta_log_record` for `<log>` (ADR 0045): the actor, the label and the
  value, with no step key or trace context;
- inspection and a paged walk over actors;
- no counters of any kind.

Two of ADR 0067's open points hide exactly the failures that matter at
3am. An internal event that enables no transition leaves no record, and
events that core queues outside a run, such as the `error.communication`
of a delayed send that could not be delivered, get no `RAISED` record.

## Decision

### 1. Metrics: counters core keeps per definition, read by the host (pull)

Core keeps a small block of monotonic counters **per definition** and one
**per system**, and the host reads them when it exports. Nothing is kept
per actor.

```c
typedef struct {
    /* Actors */
    uint64_t spawned, loaded, unloaded, finished, destroyed;
    /* Work */
    uint64_t events_taken;         /* external events that began a macrostep  */
    uint64_t events_ignored;       /* external events that enabled nothing     */
    uint64_t internal_ignored;     /* internal events that enabled nothing     */
    uint64_t macrosteps, microsteps;
    /* Failures, by the error event core raised */
    uint64_t errors_execution;     /* error.execution                          */
    uint64_t errors_communication; /* error.communication, in a run or not     */
    uint64_t errors_platform;      /* error.platform                           */
    /* Effects */
    uint64_t sends, sends_delayed, sends_cancelled, delayed_fired;
    uint64_t invocations_started, invocations_ended;
    /* Execution control */
    uint64_t runs_paused;          /* a budget ran out                         */
    uint64_t runs_stopped;         /* an allocation failed (ADR 0070)          */
} ta_counters;
```

- **`ta_definition_counters()`** reads a definition's block, and
  **`ta_system_counters()`** reads the system's totals. The system block
  includes every definition that has been freed, so a cumulative sum never
  goes backwards when a definition retires (ADR 0043).
- **State occupancy.** Core also keeps, per definition, a count of how
  many resident actors have each atomic state active.
  `ta_definition_occupancy()` copies it into a caller's buffer. It is
  maintained from the exit and entry sets the interpreter already holds
  for each microstep, and on every other path that changes a resident
  configuration: bootstrap, finish, destroy, load, unload, reconstruction
  and replacement. "Resident" matches `actor_count` in inspection.
- **Always on.** The counters are plain increments on the definition,
  with no mask, no sink and no sampling, so a production system that
  installed nothing still has them when something goes wrong.
- **Deterministic and clock-free.** They count occurrences, not time.
  Durations and lateness are the host's (section 4).
- **The host maps them to OTLP.** Counters become cumulative monotonic
  Sums whose start time is when the host registered the definition, with
  the definition's family and revision as attributes. Occupancy becomes a
  Gauge with the state's author ID as an attribute. These are observable
  instruments in OpenTelemetry's terms: values the application already
  keeps, read at collection.

### 2. Logs: selected records, written whatever the sampling

Logs are not a new core sink. They are the trace records that describe
something going wrong, plus `<log>`, delivered whether or not the
macrostep they belong to is sampled.

- **Sampling becomes a mask.** ADR 0067's `bool unsampled` becomes
  `uint32_t unsampled_kinds`: the kinds core writes even for unsampled
  macrosteps. The old `unsampled = true` is `unsampled_kinds = kinds`. A
  host that exports logs sets it to the failure kinds, so traces can be
  sampled at 1% while every failure is still written. This is an
  in-place change; every caller moves.
- **Records carry what a log needs already:** the step key, the trace
  context, the actor, the definition, the position and the fault
  (ADR 0067). A record from an unsampled macrostep still carries its
  trace ID and a span ID derived from its key, with the sampled flag
  clear, so the log names the step it belongs to even though no span was
  exported.
- **`<log>` joins them.** `ta_log_record` gains the step key and the
  trace context of the step that ran the `<log>` (refines ADR 0045). A
  chart's own diagnostics then land next to the spans of the macrostep
  that wrote them in otel-tui's "logs related to this trace" view.
- **The two open points close:**
  - an internal event that enables no transition gets a record, a new
    kind `TA_TRACE_IGNORED`, used for external and internal events
    alike;
  - an event core queues outside a run gets a `RAISED` record, with the
    key of the step that caused it where there is one (a delayed send's
    parent) and none otherwise.
- **Which records become logs is the host's choice,** with a default
  catalogue (section 5). Core only has to write the records.

### 3. Exemplars and "which actors": host-side

- **Exemplars.** A host histogram point (section 4) may carry, as its
  exemplar, the trace ID and span ID of the slowest sample in the
  interval, taken from the step's record. That is the link from "p99 is
  two seconds" to one trace. It is only possible when that macrostep was
  sampled.
- **Which actors are stuck.** Metrics say how many actors sit in which
  state. Which ones, and for how long, is inspection, like
  `recon:proc_count` for Erlang. The host walks actors with
  `ta_system_visit_actors`, filtered by definition and state, and reports
  the result. An actor that is idle, not finished, has no pending delayed
  send, no active invocation and no mail is one that nothing inside the
  system will ever wake. Core adds no API for this; the walk and the
  inspection view already give every fact.

### 4. What only the host knows

The host adds, as its own metrics and logs:

- **mailboxes:** messages waiting (a Gauge per definition), and the age
  of the oldest waiting message;
- **dead letters:** a counter by reason, and a log per dead letter,
  rate-limited;
- **faults and quarantine:** a counter by fault code, a Gauge of
  quarantined actors, and an ERROR log per quarantine;
- **the pump:** turn duration (Histogram, seconds), turns, the runnable
  and ready backlog (Gauges), and allocation-failure back-offs;
- **timers:** **lateness** (Histogram) and **overdue** (a Gauge). Core
  makes both computable without a clock. Each dispatched delayed send
  carries its due time, so lateness is `now − due` at dispatch; and
  `ta_system_next_deadline()` already gives the earliest pending
  deadline, so "overdue" is `now − next deadline` when that is positive,
  which is how a starved pump or stopped timers show up.
- **memory:** bytes in use per category (napi ADR 0014).

Wall time, turn duration and memory are the host's; core has no clock
and no view of the host's queues.

### 5. The 3am questions, and what answers them

| Question | Signal | Data |
|---|---|---|
| Which actors are stuck, and in which states? | Metrics, then inspection | Occupancy Gauge per definition and state; then a filtered walk that lists the actors and flags those nothing will wake |
| Is mail piling up? Where, and is it growing? | Metrics (host) | Messages waiting per definition, and the age of the oldest message |
| Are events being dropped, dead-lettered or ignored? | Metrics, logs | `events_ignored`, `internal_ignored`; host dead letters by reason; a WARN log per dead letter (rate-limited); `IGNORED` records as DEBUG logs on request |
| Are `error.execution` and `error.communication` rising, and where? | Metrics, logs | `errors_*` per definition; an `ACTION_FAILED` or `RAISED error.*` record as a WARN log with the session ID, position and fault |
| Are timers firing late, or not at all? | Metrics (host) | Lateness Histogram; overdue Gauge from the earliest deadline; `delayed_fired` against `sends_delayed` |
| Quarantined, out of memory, retrying, hitting budgets? | Metrics, logs | Host quarantine Gauge and faults by code; `runs_stopped`, `runs_paused`; a `STOPPED` record as a WARN log; an ERROR log per quarantine |
| Is the host keeping up? | Metrics (host) | Turn duration Histogram with exemplars; runnable and ready backlog |
| What did this actor do just before it went wrong? | Logs, traces | Its failure logs, found by session ID, lead by trace and span ID to the trace when sampled; otherwise the logs alone, since failures are written unsampled |

The default log catalogue (host-side, overridable):

| Event name | Severity | From |
|---|---|---|
| `scxml.log` | INFO | `<log>` (ADR 0045) |
| `tinyactors.action.failed` | WARN | `ACTION_FAILED` |
| `tinyactors.error.raised` | WARN | `RAISED` of an `error.*` event |
| `tinyactors.run.stopped` | WARN | `STOPPED` (ADR 0070) |
| `tinyactors.event.ignored` | DEBUG, off by default | `IGNORED` |
| `tinyactors.actor.finished` | DEBUG | `FINISHED` |
| `tinyactors.actor.quarantined` | ERROR | the host's fault handling |
| `tinyactors.message.dead_lettered` | WARN | the host's dead letters |

Log attributes may name one actor (`scxml.session_id`, the actor ID),
because each record is one occurrence. Labels for transitions and actions
come from ADR 0071.

### 6. Names and cardinality

- **Metric names** are `tinyactors.*`, for example
  `tinyactors.events.ignored` (`{event}`), `tinyactors.errors`
  (`{error}`, with `scxml.error.type`), `tinyactors.actors.occupancy`
  (`{actor}`, with `scxml.state`), `tinyactors.pump.turn.duration`
  (`s`), `tinyactors.timer.lateness` (`s`). Units follow the semantic
  conventions: UCUM, curly-brace annotations for counts, seconds for
  durations. The exact table is written with the implementation.
- **Chart concepts keep ADR 0067's `scxml.*` attribute names.**
- **The Development-status messaging conventions are not used.** They
  describe clients of message brokers, not actor mailboxes.
- **Never a metric attribute:** session IDs, actor IDs, send and invoke
  IDs, event data, and event names (a host can build names from data).
- **Allowed:** definition family and revision, a state's author ID, error
  type, I/O processor type, dead-letter reason, fault code, memory
  category, and one attribute naming the system, so two systems in one
  process keep separate metric streams (OTLP's single-writer rule).
- **Histograms use explicit buckets,** which otel-tui charts; it does not
  chart exponential histograms yet.

### 7. Encoding: the ADR 0016 encoder writes all three

napi ADR 0016's portable OTLP writer gains
`ExportLogsServiceRequest` and `ExportMetricsServiceRequest`, over the
same host interface (clock, names, IDs, values). The host posts them to
`/v1/logs` and `/v1/metrics` the way it posts traces to `/v1/traces`.

- Logs leave with the trace bytes at the end of a pump turn.
- Metrics are read when the host asks, at its export interval, from
  core's counters and occupancy plus its own.

napi's part is a later napi ADR, **napi ADR 0017, "Metrics and logs leave
the addon as OTLP protobuf"**. It covers the JavaScript API, the export
interval, rate limits, the default log catalogue, and how
`setTraceSink`'s options extend to logs.

## Alternatives

- **Derive metrics from trace records (push).** A host sink would count
  records as they arrive. That makes metrics depend on tracing
  configuration and sampling, and it builds records for every macrostep
  just to add one to a counter. It is also how `:telemetry` works [research
  note §4], but there the runtime is not a freestanding C core with a
  mask that exists to avoid building records.
- **Per-actor counters or timestamps.** A per-actor "last active" time
  would answer "stuck for how long" directly, but each 8 bytes costs
  32 MB at 4M actors. Prior art does not keep per-actor streams either.
  Inspection answers "which ones" on demand. See open question 1.
- **A separate log sink in core.** The records and the `<log>` sink
  already carry everything; a third sink would duplicate ADR 0067's
  pattern and its call sites.
- **Everything as span events.** It follows ADR 0016's current mapping,
  but OpenTelemetry is deprecating the span event API in favour of logs
  correlated with spans (OTEP 4430). More importantly, span events vanish
  with their span when a trace is not sampled.
- **Occupancy by walking all actors at each export.** It needs no
  maintenance in core, but at 4M actors a walk costs tens of milliseconds
  at every collection, and it would block the host's single thread.
- **Adopt the messaging semantic conventions.** They are Development
  status and describe broker clients.

## Relation to other decisions

- **ADR 0045:** `ta_log_record` gains the step key and trace context, so
  `<log>` output correlates with traces.
- **ADR 0067:** `unsampled` becomes a per-kind mask; `IGNORED` is a new
  record kind; open points 1 and 2 close. Its records remain the only
  source of log content from core.
- **ADR 0070:** a step that runs again after a failed allocation must not
  count twice. Counters move when the unit commits, like records, and the
  fault sweep checks it.
- **ADR 0071:** labels for transitions and actions in log attributes.
- **napi ADR 0016:** its encoder writes the logs and metrics requests.
  Its mapping of non-step records to span events stays. The same records
  may also leave as logs, which is how OpenTelemetry now recommends
  events are written (OTEP 4430). Whether the span events should
  eventually give way to logs is open question 3.

## Open questions

1. **Per-actor "last active" time.** Should core keep, per actor, the
   logical time of its last macrostep? It would let inspection sort
   actors by how long they have been idle, which answers "stuck for how
   long" directly. The cost is 4 bytes as seconds since the system's
   epoch, possibly absorbed by existing padding in `ta_actor`, otherwise
   8 bytes: up to 32 MB at 4M actors. This ADR proposes not keeping it and
   relying on occupancy plus a filtered walk.
2. **Occupancy for every definition, always?** It costs 4 bytes per
   atomic state per definition and one or two increments per microstep.
   It could instead be switched on per system. This ADR proposes always,
   since it is the one metric that answers "where are they stuck" with no
   setup.
3. **Span events or logs for decisions and data.** ADR 0016 writes
   `ASSIGN`, `SENT` and similar as span events. OTEP 4430 points new
   instrumentation at logs correlated with spans. This ADR leaves ADR
   0016's mapping alone and only adds failure logs. Revisit when the
   SDKs' conversion between the two has settled.
4. **Rate limits for failure logs.** An actor in an error loop can write
   thousands of `ACTION_FAILED` records per second. The counters count all
   of them. How the host limits the logs (per definition and event name
   per interval, with a "suppressed N" count) is for napi ADR 0017.

## Consequences

- A system with no sink installed still has the counters and occupancy
  that say where things are wrong.
- Failures are written as records for every macrostep, sampled or not,
  once a host asks for those kinds. Traces can be sampled low in
  production without losing errors.
- Core's API grows by three reads (`ta_definition_counters`,
  `ta_system_counters`, `ta_definition_occupancy`), one record kind
  (`IGNORED`), fields on `ta_log_record`, and a due time on dispatched
  delayed sends. `ta_trace_sink.unsampled` changes type.
- The ADR 0016 encoder grows two request types.
- **The cost of the counters is paid by every system,** always:
  - **per event:** about five increments on the definition's block, and
    one or two occupancy updates per microstep. Estimated at 1–3% of
    the time per event at 2.2M events/s (about 450 ns). To be measured.
  - **per definition:** `ta_counters` is 21 × 8 = 168 bytes, plus 4 bytes
    per atomic state for occupancy.
  - **per actor:** nothing.
  - **per system:** 168 bytes.
- **The cost of failure logs** is only paid when a host asks for them:
  records built for failures in unsampled macrosteps, which are rare by
  definition.

## Testing

- **Core unit tests:** exact counter values for small charts, per kind:
  ignored external and internal events, each error type, a paused run,
  a stopped run, sends, delayed sends fired and cancelled, invocations.
  System totals survive retiring and freeing a definition.
- **Occupancy agrees with a walk.** In every tinyactors-test scenario,
  teardown compares each definition's occupancy with a walk over its
  actors' configurations, as the ADR 0071 check does for IDs. This
  catches any path that forgot to update it: load, unload,
  reconstruction, replacement, finishing inside a parallel state.
- **Unsampled failures.** With sampling on and the sampled bit clear, a
  failing action still writes `ACTION_FAILED` when its kind is in
  `unsampled_kinds`, and nothing else.
- **The two open points:** an internal event that enables nothing writes
  `IGNORED`; an `error.communication` queued by a delayed send that could
  not be delivered writes `RAISED` with the send's parent as cause.
- **The fault sweep** (ADR 0070): counters and occupancy after a run that
  failed each allocation in turn equal those of the run that failed
  nothing, since steps that run again must not count twice.
- **Encoder:** golden bytes for a logs request and a metrics request, and
  a strict decode of each.
- **By hand, in otel-tui:** a demo that makes actors pile up in a state,
  fail an action and dead-letter a message, where the metrics view charts
  occupancy, errors and turn duration; the logs view shows the failures;
  and "logs related to this trace" opens them from a sampled trace.
