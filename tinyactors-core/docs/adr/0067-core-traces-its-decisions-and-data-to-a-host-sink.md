# ADR 0067: Core traces its decisions and data to a host sink

**Status:** Accepted; not yet implemented. Applies ADR 0045, ADR 0047, ADR 0057, ADR 0061 and ADR 0066; refines ADR 0038 and ADR 0054; serves ADR 0020 and ADR 0021.
**Date:** 2026-09-23

## Context

ADR 0020 and ADR 0021 make verification a host that drives the production
interpreter: it owns scheduling, the clock and every I/O answer, and it
checks invariants over what happens. Core already gives such a host most
of what it needs as adapters it supplies: the router, the clock and
`ta_system_dispatch_due()`, invokers, the log sink (ADR 0045), an
allocator that can fail on demand (ADR 0047), inspection, session images
for forking, and `budget`/`action_budget` for single-stepping (ADR 0061).

What no adapter can see is what the interpreter decides in between:

- which event a macrostep or microstep took, and whether it was internal
  or external;
- which transitions were selected, and the exit and entry sets in order;
- the events core raises itself (`done.state.*`, `done.invoke.*`,
  `error.execution`, `error.communication`);
- guard outcomes, and the values `<assign>`, `<data>` and `<foreach>`
  write;
- where a macrostep began and ended, paused, or will repeat after an
  allocation failure.

Comparing configurations between steps loses targetless transitions,
exits and re-entries of the same state, and entry order. Transition
coverage, the signal a coverage-guided fuzzer needs, and properties such
as "the server never enters `open` without a `hello` it received earlier"
depend on exactly this.

The same information is what production observability wants. A Node HTTP
handler that drives several actors until they are quiet should be able
to show every macrostep its request caused, under the request's span, in
an OpenTelemetry (OTEL) trace. That requires causality to travel with
events: the handler's pump is not a reliable owner, because work one
request leaves queued, and delayed sends that come due later, run while
another request is driving.

Options considered and rejected:

- **A network inspector in core**, as `@statelyai/inspect` offers over a
  websocket. Core runs where there is no network (ADR 0023, ADR 0024),
  and a remote observer cannot choose the next event or roll back an
  experiment. XState's own transports sit on an in-process `inspect`
  callback; that callback is the part core should provide. Transports are
  host code.
- **Emitting OTEL from core.** OTEL wants wall-clock timestamps, random
  IDs, string attributes, batching and sampling at export: allocation,
  formatting, and nondeterminism that ADR 0023, ADR 0047 and replay
  forbid. Every host language already has an OTEL SDK.
- **A data model proxy beside a trace of decisions.** A proxy sees every
  data model call, but its journal and the trace are two streams, joined
  by a key both sides must carry. Every data model call already passes
  through core's executor, so core can report the data operations in the
  same stream, and no join is needed.
- **An opaque 8-byte host tag instead of a W3C trace context.** Every host
  would keep a tag-to-context table, and a tag means nothing in another
  process after an image moves there. A W3C `traceparent` costs 8 more
  bytes per actor and per pending send and needs no table.
- **Begin and end callbacks for batching.** Every record arrives inside a
  host call (`deliver`, `resume`, `dispatch_due`, lifecycle calls), and
  `TA_TRACE_MACROSTEP_BEGIN`/`END` mark macrosteps. A sink batches until
  one of those boundaries without help. What reduces volume is not
  building records nobody wants (section 5).
- **Records serialized into a host-lent buffer.** It saves one indirect
  call per record, but core would copy borrowed arrays and values into a
  format of its own. The cost of tracing is in the sink; this can be
  revisited if a measurement says otherwise.

## Decision

### 1. A trace sink with one record type

A system has exactly one trace sink, an ordinary `{self, vtable}` adapter
like the log sink, plus a mask of the record kinds it wants:

```c
typedef struct ta_trace_sink_vtable {
    void (*write)(void *self, TA_IN const ta_trace_record *record);
} ta_trace_sink_vtable TA_VTABLE;

typedef struct {
    void *self;
    const ta_trace_sink_vtable *v;     /* never null */
    uint32_t kinds;                     /* bit TA_TRACE_BIT(kind): emit it */
    bool unsampled;                     /* also trace unsampled macrosteps */
} ta_trace_sink TA_ADAPTER;

#define TA_TRACE_BIT(kind) ((uint32_t)1 << (uint32_t)(kind))

TA_API ta_trace_sink ta_null_trace_sink(void);
TA_API ta_result ta_system_set_trace_sink(ta_system *system, ta_trace_sink sink);
```

**Writing a record cannot fail and cannot change the run.** `write`
returns `void`, as the log sink's does (ADR 0045). A sink must not call
back into the system for the actor that is running; it may read the
record, retain values it keeps, and inspect other actors.

**One flat record, not a callback per kind.** It matches the log sink,
forwards unchanged through a journal or a binding, and a new kind does
not change the vtable. Only the fields named for a kind are meaningful;
the rest are zero. Everything in a record is borrowed for the call.

### 2. The null sink: one test per call site

`ta_null_trace_sink()` returns a stateless adapter: `.self = NULL`, a
static vtable whose `write` does nothing, `kinds = 0`, `unsampled =
false`. A system starts with it installed, and installing it is how a
host stops tracing. `ta_system_set_trace_sink()` rejects a sink whose `v`
or `v->write` is null with `TA_ERR_INVALID`.

`v` is therefore never null, and a call site tests one thing, the mask:

```c
if (system->trace.kinds & TA_TRACE_BIT(TA_TRACE_ASSIGN)) {
    /* build the record, then: */
    system->trace.v->write(system->trace.self, &record);
}
```

The mask test stays even though the null sink would accept any record:
it is what keeps core from building records in the untraced case, which
costs far more than a branch. The null sink makes it the only test.
Sampling (section 6) adds one flag test to call sites inside a
macrostep, folded into a per-macrostep mask the interpreter computes
once at `MACROSTEP_BEGIN`: `sink.kinds` when the macrostep is sampled or
`sink.unsampled` is set, otherwise only the kinds outside macrosteps.

This follows ADR 0057: core ships the trivial implementation of an
interface, so no host writes one and no call site special-cases absence.

### 3. Step keys and positions

```c
typedef struct {
    uint64_t session;     /* the session ID (ADR 0066); 0: see section 6 */
    uint64_t macrostep;   /* from 1 per session; 0 outside a macrostep  */
    uint32_t microstep;   /* from 1 per macrostep; 0 for the macrostep  */
} ta_step_key;

typedef enum {
    TA_TRACE_AT_NONE = 0,       /* not inside a microstep                 */
    TA_TRACE_AT_SELECT,         /* selecting transitions (guards)         */
    TA_TRACE_AT_EXIT,           /* onexit of `state`                      */
    TA_TRACE_AT_TRANSITION,     /* content of `transition`                */
    TA_TRACE_AT_ENTER,          /* onentry, initial or history of `state` */
    TA_TRACE_AT_BOOTSTRAP       /* early-bound <data> at bootstrap        */
} ta_trace_at;

typedef struct {
    ta_trace_at at;
    ta_state_id state;
    ta_transition_id transition;
    ta_action_id action;        /* the unit of executable content          */
    uint32_t iteration;         /* the <foreach> iteration, from 0         */
} ta_trace_position;
```

- **Counters always run.** Every actor counts macrosteps (`uint64_t`) and
  microsteps within the current macrostep (`uint32_t`), whether or not a
  sink is installed, so keys are the same with tracing on or off and
  every event gets a cause (section 6). Both live on the actor, because a
  pause between microsteps (ADR 0009) must keep them, and both are in the
  session image (section 8), so a key is never reused within a session.
- **The position already exists.** `ta_micro_position` and the executor
  frames (ADR 0060, ADR 0061) say where the interpreter is. Records
  report it as `ta_trace_position`, the public view of the same data.
  A position orders data operations inside a microstep by their place in
  the document, and names the element that performed them without a
  lookup by expression ID.

### 4. What is recorded

```c
typedef enum {
    /* Lifecycle; outside macrosteps. */
    TA_TRACE_SPAWNED,           /* bootstrap follows as macrostep 1      */
    TA_TRACE_LOADED,
    TA_TRACE_UNLOADED,
    TA_TRACE_FINISHED,          /* `state`: the final state (ADR 0056)   */
    TA_TRACE_DESTROYED,

    /* Macrosteps. */
    TA_TRACE_MACROSTEP_BEGIN,   /* `event`: the external event, or none
                                   for bootstrap                         */
    TA_TRACE_MACROSTEP_END,     /* stable; `states`: the configuration   */

    /* Decisions. */
    TA_TRACE_CONDITION,         /* `transition`, `expr`, `result`/fault  */
    TA_TRACE_MICROSTEP,         /* `event` (or none: eventless),
                                   `transitions`, `exited`, `entered`,
                                   `history`                             */
    TA_TRACE_RAISED,            /* an event core queued internally       */

    /* Data. */
    TA_TRACE_DATA_INIT,         /* `expr` or none, `value`: <data>       */
    TA_TRACE_ASSIGN,            /* `expr` (the location), `value`        */
    TA_TRACE_SCRIPT,            /* `expr` (the script), success or fault */
    TA_TRACE_FOREACH,           /* `value` (the snapshot), `iteration`   */
    TA_TRACE_ACTION_FAILED,     /* `action`, fault; the error.execution
                                   is the RAISED record that follows     */

    /* Effects that leave the actor. */
    TA_TRACE_SENT,              /* `send`, `event`; its cause is this key */
    TA_TRACE_CANCELLED,         /* `send`                                 */
    TA_TRACE_INVOKED,           /* `invoke`                               */
    TA_TRACE_INVOKE_CANCELLED,  /* `invoke`                               */

    /* Execution control. */
    TA_TRACE_PAUSED,            /* budget or action budget ran out        */
    TA_TRACE_RESUMED,
    TA_TRACE_RETRIED            /* stopped on allocation failure; section 7 */
} ta_trace_kind;

typedef struct {
    ta_trace_kind kind;
    ta_step_key key;
    ta_trace_position position;
    ta_trace_context context;        /* the macrostep's (section 6)       */
    ta_actor_id actor;               /* runtime ID at this moment         */
    ta_definition_id definition;
    ta_time now;                     /* logical time                      */

    TA_OPTIONAL TA_LIFETIME(call) const ta_event *event;
    ta_state_id state;
    ta_expr_id expr;
    ta_action_id action;
    uint32_t iteration;
    ta_send_id send;
    ta_invoke_id invoke;
    bool result;                     /* CONDITION                          */
    bool has_value;
    ta_value value;                  /* borrowed; retain to keep (ADR 0006) */
    bool has_fault;
    ta_fault fault;

    TA_COUNTED_BY(transition_count) TA_LIFETIME(call) const ta_transition_id *transitions;
    uint32_t transition_count;
    TA_COUNTED_BY(exited_count) TA_LIFETIME(call) const ta_state_id *exited;
    uint32_t exited_count;
    TA_COUNTED_BY(entered_count) TA_LIFETIME(call) const ta_state_id *entered;
    uint32_t entered_count;
    TA_COUNTED_BY(history_count) TA_LIFETIME(call) const ta_state_id *history;
    uint32_t history_count;
    TA_COUNTED_BY(state_count) TA_LIFETIME(call) const ta_state_id *states;
    uint32_t state_count;
} ta_trace_record;
```

- **IDs are compiled IDs.** The host names states, transitions,
  expressions and actions through definition inspection (ADR 0042).
- **A microstep is one record, written before its actions run.** It
  carries the transitions in selection order, the exit set in exit
  order, the entry set in entry order (SCXML 3.13), and the history
  states whose record the exit updated. Core holds all of these in
  interpreter scratch already, so writing them allocates nothing. The
  data records that follow carry the same key and a position naming the
  `onexit`, transition or `onentry` action that produced them.
- **Data records come from core, not the data model.** Core makes every
  data model call from its executor and holds what the record needs:
  the value it evaluated and passes to `assign` and `set_named`, the
  boolean or fault `eval_condition` returned, the snapshot and index it
  passes to `foreach_bind`. Values are borrowed; the sink renders them
  with the data model it installed, as a log sink does. What a
  `<script>` changes stays opaque: core reports that script `expr` ran
  and whether it failed, and a sink that wants the effect inspects the
  data at `SCRIPT` or `MACROSTEP_END`.
- **A macrostep with no microstep** is recorded: `MACROSTEP_BEGIN` then
  `MACROSTEP_END` means the event enabled no transition. That is the
  "message silently ignored" case a verifier must see.
- **The configuration at `MACROSTEP_END`** lets properties such as
  `In(client, connected)` be checked from the trace alone. A sink that
  does not want it clears the kind; one that keeps the configuration
  itself from exit and entry sets can do so.
- **`<log>` stays with the log sink** (ADR 0045). `<send>` is `SENT`.

### 5. Volume is controlled at the source

The kind mask decides which records are built at all. A verifier that
checks `In()` properties over data wants `MICROSTEP`, `ASSIGN` and
`MACROSTEP_END`; an OTEL exporter may want everything except
`CONDITION`. Sampling (section 6) removes whole macrosteps. Batching is
the sink's: it buffers and flushes at `MACROSTEP_END` or when the host's
call returns, since no record arrives between host calls.

### 6. Trace context and cause travel with events

```c
typedef struct {
    uint8_t trace_id[16];       /* W3C trace-id; all zero: none           */
    uint8_t flags;              /* W3C trace-flags; bit 0: sampled        */
} ta_trace_context;

typedef struct {
    ta_trace_context context;
    ta_step_key cause;
} ta_trace_parent;
```

`ta_event`, `ta_send` and `ta_actor_spawn_params` gain a
`ta_trace_parent trace`.

- **The context is the host's, copied unchanged.** Core never creates,
  parses or interprets a trace ID. It copies `trace_id` and `flags` and
  tests the sampled bit. It generates no random IDs, so replay stays
  deterministic.
- **The cause is a step key.** A host-delivered event's cause has
  `session = 0`, and its `macrostep` holds the host's parent span ID
  (the W3C `parent-id`), or 0 for none. Core writes the cause of what it
  produces: the key of the step that produced it.
- **Propagation.** A macrostep runs under the parent of the external
  event it took, or of its spawn for bootstrap. Everything it produces
  inherits that context: its internal events, sends, delayed sends,
  invocations, and the bootstrap of actors it invokes. Each carries as
  its cause the key of the step that produced it. A delayed send keeps
  the parent it had when it was scheduled, so a timer that fires later
  still names the request that set it.
- **Internal events store nothing extra.** They inherit the macrostep's
  context, and a `RAISED` record names the step that raised each one, so
  internal queue entries (ADR 0064) do not grow.
- **Span IDs are derived, the same way in every host.** OTEL needs an
  8-byte span ID per step. `ta_step_key_span_id(key)` computes one from
  the key with a fixed 64-bit mix, never 0, and returns `macrostep`
  unchanged when `session` is 0. Hosts need no table from steps to
  spans, and a host's parent span ID passes through as itself.
- **Sampling at the source.** When a macrostep's context has a trace ID
  and its sampled bit is clear, core builds no records for it unless the
  sink set `unsampled`. A verifier sets `unsampled`; a production
  exporter usually does not. Lifecycle records are not sampled.
- **Host operations that are not events** (load, unload, destroy) carry
  no parent. The sink attributes them to whatever the host is doing.
- **After an image moves to another process** a trace ID is still a valid
  W3C trace ID, so a timer that fires after a restore lands in the
  original request's trace.

With this, the Node handler in the Context starts a request span, puts
its trace ID, flags and span ID on the events it delivers and the actors
it spawns, and drives. Every macrostep the request caused, directly or
through sends and timers, arrives with the request's trace ID, and a
sink parents it either under the request span (by context) or under the
step that caused it (by cause), whichever tree the host wants to show.

### 7. Retries

ADR 0047 resumes an actor interrupted by `TA_ERR_NOMEM` from the last
interpreter boundary before the failed allocation. Records are written
only for work that has taken effect, with one exception: guards run
during transition selection, before the microstep's reservations, so
`CONDITION` records can precede a failure.

`TA_TRACE_RETRIED` says: the `CONDITION` records this actor wrote since
its last `MICROSTEP` or `MACROSTEP_BEGIN` record did not lead to a
microstep, and selection will run again with the same key. A sink that
buffers to a boundary drops them. No other record is ever written
twice, and the implementation must keep it so.

### 8. Session images and memory

- **Per actor:** the two counters (12 bytes) and the current macrostep's
  parent (48 bytes), stored once. The actor holds three `ta_event`
  copies (`external_event`, `macrostep_event`, `current_event`); they
  must not each carry a `ta_trace_parent`. Growth is at most 64 bytes
  per actor, from about 532 to about 596, within the budget of 768.
- **Per pending delayed send:** its parent, 48 bytes.
- **Internal queue entries:** unchanged (section 6).
- **Images** carry the counters, the current macrostep's parent, and each
  pending send's parent. The image format version changes.
- **`ta_event` grows** from 64 to 112 bytes as a public type. It lives in
  the host's envelopes and in core's three per-actor copies, which the
  point above handles.

## Consequences

- A verifier sees every decision and data change of every actor as one
  ordered stream per host call, with no join, and records carry enough
  to check configuration, data and happens-before properties.
- Transition coverage is the set of transition IDs in `MICROSTEP`
  records, per definition.
- OTEL export is a host adapter: it maps records to spans and span
  events, names IDs through inspection, adds wall time, and hands spans
  to its language's SDK. Hosts share attribute names (`scxml.state`,
  `scxml.transition`, `scxml.event.name`, `scxml.session_id`, ...), which
  are ours: OTEL has no statechart convention.
- Hosts carry `traceparent` across their own transports: they write
  `trace` from `ta_send` into it, and read it back into the event they
  deliver.
- The untraced cost is the counters, one mask test per call site, 64
  bytes per actor, 48 per pending send, and the larger public
  `ta_event`.
- A `datamodel.faulty` wrapper in the test host still injects data model
  failures; it needs no journal, since the trace shows the failure.
- The log sink uninstalls with a null vtable today. Whether it moves to
  the null-sink pattern is a separate decision.

## Testing

- **Core unit tests:** exact record sequences for small charts (eventless
  chains, parallel regions, history, targetless and internal
  transitions, `<foreach>`, errors); the null sink is installed when a
  system is created; `TA_ERR_INVALID` for a null `v` or `write`; the mask
  suppresses construction (the counting allocator and a stub sink see
  nothing); sampling on and off; `ta_step_key_span_id()` golden values;
  counters and parents survive capture and load.
- **Retry equivalence:** with the counting allocator failing each
  allocation of a scenario in turn, the trace, after dropping what
  `RETRIED` voids, equals the trace of the run without failures.
- **Test host:** a `trace.recording` instrument that journals records on
  the shared `seq` counter, and scenario assertions over it.
- **Causality:** a scenario where one delivery causes sends across three
  actors and a delayed send, driven by two different pumps; every
  resulting macrostep carries the delivery's trace ID and a cause chain
  back to it.
- **Scale:** the scale test runs with the null sink and asserts that
  per-actor memory grows by no more than section 8 allows.
