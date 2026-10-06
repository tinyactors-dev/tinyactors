# ADR 0099: Pending inputs are envelopes in one pool per system

**Status:** Accepted (2026-10-04); sections 1 to 5, 7 and 8 implemented in ring-proto step 4.2a, in 4.2e host input straight into core (the withdrawal of host inputs, admission inside a macrostep, error reports as inputs, and the kernel's mailboxes gone), and in 4.2d the mail, waiting lists, router deletion and the event-block cache (see the implementation notes at the end); partly implemented: the start record and kept event in sparse state (4.4) and the name area (4.2b) are pending. Supersedes ADR 0011, ADR 0029, ADR 0040, ADR 0055, ADR 0064, ADR 0069, ADR 0093 and ADR 0095, each in the part section 10 names; the rest of each stays. Run-time names in the name area are ADR 0100; delayed sends join the pool in ADR 0101; slots and the chain limit are ADR 0103.
**Date:** 2026-10-04

## Context

Core has no queue for inputs it has not run (ADR 0011, ADR 0029). The host
offers one event at a time with `ta_actor_deliver()`, which answers
`TA_ERR_BUSY` when the actor is inside a macrostep (ADR 0055). Mail between
sessions leaves core through the host's router (ADR 0040), and the host
kernel keeps mailboxes, a ready queue and a scheduler for it. An actor
holds its event in an event block lent from a cache (ADR 0064, ADR 0069),
which fills on demand (ADR 0093).

That split is the root of most of the complexity the ring protocol
proposal removes ([README.md](../../../docs/proposals/ring-proto/README.md#why)):

- **A send cannot be refused in the macrostep that makes it.** The router
  can only accept, retry or fail after the fact. A full mailbox therefore
  either overflows without bound (kernel ADR 0003) or turns into a
  submitted send that a checkpoint must restore (ADR 0095).
- **Overflow has to be held somewhere,** and every binding needs the same
  router and scheduler, so they moved into a third layer.
- **The busy answer** makes every host keep a queue per actor.

Putting a bounded queue in core removes all three. Whether an input can be
accepted, and in what order a session's inputs run, is meaning, so it
belongs in core (AGENTS.md, "Layers").

This ADR covers step 4.2 of the plan: the pool, the run queue, admission,
recycling and `ta_run`, with one macrostep slot. The kernel stays, and
calls core's new API. The rings `h2c` and `c2h` come later (Phase 5.1);
until then admission and the results of a run are native calls, shaped so
that each maps onto one record later.

## Decision

### 1. An envelope is one input a session has not finished with

An envelope holds an external event admitted from the host, mail from a
local session, an invocation report, a spawned session's bootstrap, and
(ADR 0101) a delayed send that is not due. The event a session is running
stays in its envelope until the input completes. Internal events
(`<raise>`, `done.state.*`, `error.*`) never take an envelope.

An envelope has a fixed size, so the pool is a table of them. Its variable
parts are bytes in the name area (ADR 0100):

| Field | Bytes | Meaning |
|---|---|---|
| `next` | 4 | the next envelope on the list that holds this one: run queue, outgoing, waiting, deferred or free |
| `owner_next` | 4 | the next pending delayed send of the same sender (ADR 0101) |
| `heap_index` | 4 | its place in the deadline index, or none (ADR 0101) |
| `state` | 1 | free, outgoing, runnable, waiting, taken, kept, timer, invocation token, child stop |
| `type`, `source`, `flags` | 3 | `_event.type`; where it came from (`input_source`); recorded, replaying, trace-parent-present and report bits |
| `target` | 8 | the target session ID, or 0 while the target is an address in the text |
| `from` | 8 | the sending session ID, or 0 for host input |
| `due` | 8 | a timer's deadline, or a replayed input's time |
| `data` | 8 | `_event.data`: a value the envelope holds one reference to |
| `content` | 8 | a delayed I/O send's evaluated content, or 0 |
| `invoke_instance` | 8 | the invocation-instance token of a child event, completion or stop, or 0 |
| `send_id` | 8 | a local send's `ta_send_id` (ADR 0048) |
| `source_seq` | 8 | the host's token for an admitted input (its h2c seq from Phase 5.1), or 0 |
| `text` | 8 | its text in the name area: first chunk (`u32`) and total length (`u32`) |
| | **88** | on a 64-bit host, aligned to 8 |

Recomputed from the proposal's layout: 16 bytes of links, state and small
fields, then nine 8-byte fields, 88 in all, matching the catalog's figure.

**The trace parent travels in the text, not in the envelope.** ADR 0067 and
ADR 0073 give mail, timers and host input a trace parent: a trace ID (16
bytes), flags and detail, and the cause's macrostep and microstep (the
cause's session is `from`). The proposal's 88 bytes leave it out. It is
written at the start of the envelope's text, 32 bytes, when the input has
a trace ID, and a flag says it is there. Untraced envelopes pay nothing; a
traced one with a short name still fits one 64-byte chunk.

### 2. The pool is an index pool that fills on demand, up to its bound

- **Envelopes live in an A5 index pool**, in pages of 256 (22.5 KiB). An
  envelope is named by a `u32` index; 0 is none. Pages never move.
- **The pool is empty at creation.** It grows only at safe points, where
  no session is inside a unit of executable content: when an input is
  admitted, before an input is taken, and after a macrostep end. This is
  ADR 0093's shape (allocate on first use, up to the bound, keep), moved
  from event blocks to envelopes.
- **A macrostep only takes envelopes that already exist.** It never calls
  the allocator.
- **Every list is intrusive:** a `u32` head and tail of indexes, linked
  through `next` or `owner_next` (block B1). No list has memory of its own.

### 3. Two quotas, as C3 quotas

**The admission watermark.** Core admits a host input only while the
envelopes in use are below `admissionWatermark` percent of `poolEnvelopes`,
and the name-area chunks in use below the same share of the area. The rest
is headroom for what sessions send each other, so external load alone
cannot make a chart's sends fail.

**The macrostep reserve.** One input's execution, from take until it
completes, may take at most `macrostepReserve` envelopes and
`macrostepReserve` name-area chunks for the mail and delayed sends it
makes. Before an input is taken, core refills the pool and the name area so
that their free items cover the remaining reserve of every input being
executed, as far as their bounds allow. So:

- a `<send>` fails as `error.execution.capacity.pool` (or `.names`) only
  when its input has used its reserve, or when the pool (or area) holds as
  many items as its bound allows. It never fails because the pool had not
  grown yet. The failure point of a fanout depends on the parameters and
  the load, not on the system's history.
- the reserve is what a quiet system allocates at its first input:
  1,024 envelopes and 1,024 chunks by default.

Local invocation tokens (ADR 0102) and envelopes made at safe points
(admission, recycling) do not count against the reserve.

### 4. One run queue, in arrival order

Runnable envelopes wait on **one run queue per system, in arrival order**:
admitted input, mail, invocation reports, bootstraps and timers that
became due. Arrival order keeps every sender-to-receiver pair in order,
which is all SCXML asks. One queue has no policy to choose between
targets, which is the scheduler this design deletes.

- **Mail waits for its macrostep end.** A `<send>` to a local session takes
  an envelope, fills it, and appends it to the running macrostep's
  **outgoing list**, in send order. At the macrostep end core splices the
  outgoing list onto the end of the run queue in one step. A receiver never
  takes mail from a macrostep that has not ended, and an abandoned
  macrostep (ADR 0082) frees its outgoing list unseen.
- **A target that is not resident.** An envelope whose target is a named
  address with no resident session goes on that address's **waiting list**
  (at admission, or at its sender's macrostep end, never before). Waiting
  lists are found by the address bytes, in a hash table (block B3, fixed
  mode) that grows with the pool's refill so that it always has a slot for
  every envelope that exists, and never grows at routing. The first
  envelope on an address's list makes core report that a load is needed
  (section 7). `ta_actor_load()` of that address moves its waiting list,
  in order, to the end of the run queue.
- **A session ID that names no session,** or a finished one, is
  undeliverable at once (section 6).
- **The run queue drives `ta_run`.** With one slot, core takes the head of
  the run queue, runs its input to completion, and takes the next.
  ADR 0103 adds more slots and the deferred lists that keep a busy
  session's mail from blocking others.

**Fairness is a ratio, decided now and built with `h2c`.** The maintainer's
decision: a ratio of envelopes run from the run queue to records admitted
from `h2c` is enough for now. The parameter is BEHAVIORS.md's
`fairnessRatio`, default 4, minimum 1, maximum 4,096: while both have work,
core runs up to 4 envelopes for each `h2c` record it consumes, so work
already started drains first, and two sessions that mail each other
forever cannot shut out external input. Until `h2c` exists, admission is a
call the host makes between `ta_run` calls, and the host's own turn budget
sets that split; the parameter is added in the same change as `h2c`.

Round robin across targets is **not built**. One busy actor's mail delays
another's only by the share of the run queue it holds, and from ADR 0103
on, a busy actor's later mail waits on its own deferred list, outside the
run queue. **What would reopen it:** a benchmark in which one actor
receives at least half of all mail. If the 99th-percentile wait of the
other actors' mail, from joining the run queue to being taken, counted in
macrosteps, is more than twice what it is when the same mail is spread
evenly over the actors, round robin across targets gets its own ADR.

### 5. Admission is `ta_actor_deliver`, changed in place

`ta_actor_deliver()` stops being "offer, or busy". It admits the event into
the pool, or refuses it, and never runs anything.

```c
typedef struct {
    ta_actor_id actor;                          /* a resident session, or 0 */
    TA_IN TA_OPTIONAL const ta_actor_address *address;  /* when actor is 0 */
    TA_IN const ta_event *event;                /* its name is bytes (ADR 0100) */
    ta_trace_parent trace;
    ta_time time;                               /* ADR 0094, unchanged */
    TA_OUT uint64_t *input;                     /* the token ta_run reports it by */
} ta_actor_deliver_params;
```

| Result | Meaning | Nothing retained? |
|---|---|---|
| `TA_OK` | admitted: on the run queue, or on an address's waiting list | the envelope holds a reference to the data |
| `TA_ERR_FULL` (new, −13) | the pool or the name area is at its watermark, or the text cannot fit | yes |
| `TA_ERR_DONE` | the resident target has finished (ADR 0055, kept) | yes |
| `TA_ERR_NOT_FOUND` | no session has that ID, and no address was given | yes |
| `TA_ERR_INVALID` | a caller error only: a null event, a name with more than `eventNameDepth` segments, the root name | yes |
| `TA_ERR_NOMEM` | refilling the pool failed | yes |

`TA_ERR_BUSY` is no longer an answer to delivery: mail for an active
session waits in core. `ta_actor_report_invoke()` admits the same way: a
report is an envelope with source `invoke_report`. `ta_actor_spawn()`
appends a bootstrap envelope for the new session, so a bootstrap runs in
arrival order and completes like any input. Admitted input from the host
becomes `h2c.event` in Phase 5.1, `TA_ERR_FULL` becomes `c2h.bounced` with
reason `pool` or `names`, and the token becomes the h2c seq.

**An operator transition is not an input.** ADR 0082's `transition` field
leaves `ta_actor_deliver`. A separate call, `ta_actor_transition()`, takes
an envelope for the event the operator supplies and fills a slot directly,
for a session that holds no slot; it answers `TA_ERR_BUSY` when the
session or every slot is busy, as the proposal's debugger primitive does
([debugging.md](../../../docs/proposals/ring-proto/reference/debugging.md)).

### 6. Recycling: undeliverable mail becomes its own error

When an envelope cannot be delivered, the error it causes reuses it, so
reporting needs no room the pool may not have:

| The envelope was | It becomes |
|---|---|
| mail or a timer from session S, whose target is gone, finished, or failed to load, or was destroyed with mail waiting | a data-free `error.communication` for S, with `_event.sendid` the send's ID. Its data and content are released, and it joins the end of the run queue with target S. |
| an admitted host input | freed, with an "input refused" result naming its token (section 7), and reason `destroyed` or `load_failed` |
| anything whose sender is itself gone | freed |

Every local mail envelope reserves, when it is made, the name storage its
`error.communication` would need, even when its own name is shorter, so
recycling never needs a chunk. This is ADR 0040's `error.communication`
for an inaccessible target, and napi ADR 0022's, done in core.

### 7. `ta_run` replaces `ta_actor_resume`

```c
typedef enum {
    TA_RUN_IDLE,          /* nothing runnable */
    TA_RUN_MORE,          /* a budget ran out, and no input is inside a macrostep */
    TA_RUN_PAUSED,        /* a budget ran out inside a macrostep */
    TA_RUN_NOTICES_FULL   /* the notice array is full: read it and call again */
} ta_run_status;

typedef enum {
    TA_NOTICE_INPUT_COMPLETED,  /* a host input settled: token, status, ignored */
    TA_NOTICE_INPUT_REFUSED,    /* an admitted host input was discarded before take */
    TA_NOTICE_INPUT_TAKEN,      /* any input taken, with its event; only when asked */
    TA_NOTICE_FINISHED,         /* a session reached a top-level final state */
    TA_NOTICE_LOAD_NEEDED       /* envelopes wait for this address */
} ta_run_notice_kind;

typedef enum {
    TA_INPUT_STABLE, TA_INPUT_DONE, TA_INPUT_RUNAWAY, TA_INPUT_ABANDONED
} ta_input_status;

typedef struct {
    ta_run_notice_kind kind;
    ta_actor_id actor;
    uint64_t input;             /* the token from ta_actor_deliver, or 0 */
    ta_input_status status;     /* COMPLETED */
    bool ignored;               /* COMPLETED: the event enabled no transition (ADR 0089) */
    TA_OPTIONAL const ta_event *event;          /* TAKEN; borrowed until the next ta_run */
    TA_OPTIONAL const ta_actor_address *address; /* LOAD_NEEDED; borrowed likewise */
} ta_run_notice;

typedef struct {
    uint32_t budget;          /* microsteps (ADR 0009) */
    uint32_t action_budget;   /* units of executable content (ADR 0061) */
    bool report_taken;        /* write INPUT_TAKEN notices */
    TA_OUT ta_run_status *status;
    TA_OUT ta_run_notice *notices;
    uint32_t notice_capacity;
    TA_OUT uint32_t *notice_count;
} ta_run_params;

TA_API ta_result ta_run(ta_system *system, TA_IN const ta_run_params *params);
```

- One call takes inputs from the run queue and runs them, until the queue
  is empty or a budget runs out. A budget pause leaves the running input in
  the running workspace, and the next call continues it.
- **The notices are the records `c2h` will carry:** `c2h.input_completed`,
  `c2h.input_refused`, `c2h.input`, `c2h.finished` and `c2h.load_needed`
  (Phase 5.1 and 5.5). Until then they let the kernel settle its callers,
  record the inputs its recorded families take, and load addresses.
  `TA_RUN_NOTICES_FULL` plays the part `TA_RUN_C2H_FULL` will: core stops
  between phases, never inside a macrostep, and keeps what it has not
  reported.
- `ta_address_load_failed(system, address)` answers a load the host cannot
  do: core recycles the address's waiting list (section 6). It becomes
  `h2c.load_failed` in Phase 5.3.
- **With one slot, an input that reaches `microstepLimit` cannot be held:**
  it would stop every other session. So from this step on, `ta_run` applies
  ADR 0082's abandon at the limit and reports status `TA_INPUT_RUNAWAY`,
  and `TA_ERR_RUNAWAY` goes. ADR 0103 widens what the limit counts.

### 8. The taken event stays in its envelope

ADR 0069's rule that taking an event does not move it carries over: the
taken envelope stays in state `taken` while `_event` names it, through
invocation startup, until its input completes. Inspection (ADR 0083)
reports the event from there.

The internal queue leaves the event block. It becomes one live queue in the
running workspace, plus one per nested interpreter (ADR 0060): only one
input runs at a time. In this step it still grows as today; ADR 0102 makes
it fixed.

**An invoked session keeps its last event.** ADR 0069 point 4 still holds:
a cancelled child's `<onexit>` reads the event its last input bound. An
invoked session's sparse state gets one kept-event slot when the child is
created (a safe point). When its input completes, the event `_event`
names moves there instead of being freed: its taken envelope (state
`kept`), or, when `_event` names an internal event, that queue entry, with
its name chunks and data reference. Nothing is copied or allocated. The
slot is released at the session's next take and when it ends. A kept
envelope counts against the pool.

### 9. Parameters and sizes

Under ADR 0092's rules. The proposal's table is the starting point; the
maintainer asked that a system the size of the test suites' stay at a few
hundred KiB, and that the ADR say what a busy server should set.

| Parameter | Default | Minimum | Maximum | Bounds |
|---|---|---|---|---|
| `poolEnvelopes` | 4,096 | 8 | 16,777,216 | envelopes in the pool |
| `nameAreaBytes` (ADR 0100) | 262,144 | `maxRecordBytes` | 1 GiB | the name area, in 64-byte chunks |
| `admissionWatermark` | 50 (%) | 1 | 99 | the share of both above which host input is refused |
| `macrostepReserve` | 1,024 | 1 | `poolEnvelopes` | envelopes, and name chunks, one input's execution may take |
| `fairnessRatio` | 4 | 1 | 4,096 | envelopes run per `h2c` record admitted (from Phase 5.1) |

`eventBlockCache` (ADR 0069, ADR 0093) is removed.

**Bytes per envelope:** 88 for the envelope, plus 4 for its place in the
deadline index (ADR 0101), plus about 8 for its share of the waiting-list
table (4-byte slots, at most 75% full, a power of two): **about 100
bytes.** Each name-area chunk is 64 bytes.

| What it costs | Bytes |
|---|---|
| The whole pool at its default bound: 4,096 × 100 | 409,600 (400 KiB) |
| The whole name area at its default bound | 262,144 (256 KiB) |
| At creation: nothing in the pool or the name area | 0 |
| At the first input: the macrostep reserve, 1,024 × 100 + 1,024 × 64 | 167,936 (164 KiB) |
| A default system after its first input: `tel` (64 KiB, ADR 0098), interpreter scratch and render bytes (80 KiB, ADR 0092), the reserve (164 KiB) | about 310 KiB, before ordinary internal entries and mandatory storage (ADR 0102) |

Only a system that reaches its peak pays for it: the pool grows to the most
envelopes it ever held at once and keeps them, as ADR 0093's cache did.
`h2cBytes` and `c2hBytes` are sized by the ADR that introduces those rings;
this budget leaves them out.

**A busy server** sets `poolEnvelopes` to 65,536 (about 6.25 MiB at its
peak), `nameAreaBytes` to 4 MiB (one chunk per envelope), and
`macrostepReserve` to the largest fanout one input makes, for example
10,000. It keeps the default watermark until its capacity errors say
otherwise. A host that wants no allocation after start-up sets
`macrostepReserve` to `poolEnvelopes`: the first refill then fills the
pool to its bound.

### 10. What goes, and what stays of the ADRs this supersedes

| ADR | What goes | What stays |
|---|---|---|
| 0011 | The host's external queue and its choice of which actor receives which event next; busy as the answer to a delivery | The session owns its internal events during a macrostep. The host chooses the order in which its inputs enter (the order of its calls, later of `h2c`), and its policy at its edge. |
| 0029 | "Core accepts an external event only when the actor is ready"; the host's queueing topology for mail between sessions | No mailbox object per actor: an idle actor holds no envelope and no queue, only list heads (ADR 0101, ADR 0103). |
| 0040 | The external-event router (`ta_external_event_router`), `ta_external_delivery`, its clone and release helpers, and the accepted, retry and error answers | Core is the SCXML Event I/O Processor: its type URI, the target grammar (ADR 0044), the envelope mapping, data copied out (ADR 0097), and `error.communication` for a target it cannot reach. No router in this repository delivers to another system; a target in another process becomes `c2h.send` for the host's SCXML processor in Phase 5.2, and until then it is `error.communication`, as it is with the kernel's router. |
| 0055 | `TA_ERR_BUSY` from delivery; delivery's result set as the host's scheduling decision | `TA_ERR_DONE` for a finished target; `TA_ERR_INVALID` means a caller error only. |
| 0064 | A queue lent to each running actor, its cache, and its growth per microstep | Each nested interpreter has its own queue. A step makes no allocator call in the steady state (point 8), now from the first input on. |
| 0069 | Event blocks, their cache and `ta_system_set_event_block_cache()`; the external event slot in an actor | Taking an event does not move it (point 3), now in its envelope; an invoked session keeps its last event (point 4), in a slot made when the child is created (section 8). |
| 0093 | The event-block cache and `eventBlockCache` | Its shape: fill on demand up to a bound, at points where failing is clean, and keep what was made. The pool and the name area use it. |
| 0095 | Sends the router refused, the `submitted` state, and restoring them with a checkpoint | Nothing: a send now succeeds or fails in its own macrostep, so there is nothing to restore. ADR 0091's checkpoint stays. |

## Consequences

- **A send is refused where it is made.** A full pool is
  `error.execution.capacity.pool` in the sender's own block (ADR 0102),
  never a hidden overflow. The kernel loses its mailboxes, router, ready
  queue and dead-letter path; what is left of its scheduler admits input.
- **The host never sees busy,** and never orders inputs for a busy actor.
- **Order.** Each sender's mail to each receiver keeps its order. Mail from
  one macrostep becomes visible at its end, all at once, so a receiver may
  take it later than with today's router, by at most the rest of the
  sender's macrostep.
- **Memory:** about 100 bytes per pending input at peak, against 96 bytes
  per queued event (`ta_queued_event`) plus the kernel's mailbox entry
  today. Event blocks (152 bytes per actor holding an event, 728 per warm
  cached block) are gone. Per actor: nothing in this step; an invoked
  session's sparse state gains its kept-event slot (about 56 bytes).
- **Allocations:** the pool, the name area and the waiting table allocate
  only at safe points, and only while the system reaches a new peak. Zero
  allocator calls per event in the steady state holds from the first input
  on, without a cache to size.
- **API:** `ta_actor_deliver` and `ta_actor_report_invoke` admit;
  `ta_run`, `ta_actor_transition` and `ta_address_load_failed` are new;
  `ta_actor_resume`, `TA_ERR_RUNAWAY`, `ta_external_event_router` and
  `ta_system_set_event_block_cache` go. `TA_ERR_BUSY` stays only where an
  operation needs a session outside a macrostep (unload, capture,
  `ta_actor_transition`). The kernel, the test host and napi change their
  call sites in the same commit; napi's are listed in the commit.
- **Tests:** a core test of the pool against a counting allocator (empty at
  creation, the reserve at the first input, nothing after); the order of
  mail per sender and receiver; recycling for each row of section 6; the
  watermark and `TA_ERR_FULL`; a fanout that fails at exactly
  `macrostepReserve`, on a cold and on a warm system. `bench-compare`
  against the commit before this step, with bytes per actor and per
  pending input.
- **Forward references.** `h2c.withdraw`, the bounce of an input the pool
  cannot admit, and `fairnessRatio` arrive with the rings (Phase 5.1);
  waiting lists answer `c2h.load_needed` from Phase 5.3.

## Implementation notes (ring-proto step 4.2a)

Step 4.2a implements sections 1 to 5, 7 and 8 on one running slot, in
`src/envelope.{h,c}` (the pool, the quotas, the run queue and the waiting
table) and `src/run.{h,c}` (`ta_run`, admission, the started queue and the
abandoned and lent lists). Where the code differs from the text, or goes
beyond it:

- **The started inputs are a queue of sessions, not envelopes.** An input
  that has begun a macrostep (a bootstrap, a taken event, a paused run) is
  recorded in `ta_runner`'s queue of started sessions by actor ID (16-byte
  cells of a small index pool, in pages of 1,024, so that no block grows
  with the number of sessions spawned before the first `ta_run`), and the
  session holds its own event; the envelope of a taken event stays on the lent list
  until the input ends (section 8). `ta_actor` gains `started` and `queued`
  in the padding before `sparse`. A session started by `ta_actor_spawn`,
  `ta_actor_prepare` or a reconstruction needs a vector slot, reserved
  first, and no envelope, so the number of sessions does not depend on the
  pool bound.
- **The pool is bounded by whole pages.** `poolEnvelopes` is rounded down to
  pages of at most 256 envelopes. `ta_envelopes_refill`, called only by
  admission (`ta_run_fill`), builds the waiting table first and then grows
  the pool, so a refill that fails changes nothing.
- **`TAKEN` notices are for events only.** A bootstrap has no event, so it
  produces a `COMPLETED` notice and no `TAKEN` one.
- **A budget that ends inside the last microstep.** The call's budget is
  spent on the microstep, and the end of the macrostep is noticed by the next
  call: a call of budget 1 answers `PAUSED` for an input that ends in the
  following call.
- **`ta_actor_deliver` and `ta_actor_transition` gain an `input` output**
  that returns the token the notices name. `ta_actor_abandon` and
  `ta_actor_destroy` end an input with `TA_INPUT_ABANDONED`, reported by the
  next `ta_run`.
- **`TA_ERR_BUSY` remains** for `ta_actor_transition`, unload, capture
  (`ta_actor_export`), `ta_actor_publish`, `ta_actor_abandon` inside a
  microstep, and the error report of `ta_actor_report_invoke`, while the
  session is inside a macrostep or has an admitted input waiting
  (`queued != 0`). `TA_ERR_RUNAWAY` and `ta_actor_resume_params` are gone;
  the runaway abandon of ADR 0103 section 4 is implemented here (see that
  ADR's status line).
- **Not yet done, for 4.2b and 4.2c:** local mail between actors still goes
  through the external event router and arrives by `ta_actor_deliver`, so
  section 4's rule that a macrostep's mail becomes visible at its end is
  not yet in force; recycling (section 6) and `ta_address_load_failed`; the
  name area; the event blocks and their cache stay, so
  `TA_PARAM_EVENT_BLOCK_CACHE` and `ta_system_set_event_block_cache` are
  still there.
- **Callers.** The kernel's scheduler runs `ta_run` and settles the notices
  (`tah_sched_run`); a turn bounds the inputs it completes by `resumes`, and
  ends when a budget stops an input inside its macrostep, so a paused input
  holds the one slot and other actors' inputs wait behind it until slots
  take turns (ADR 0103). The test host's Ruby scheduler calls `ta.run` once
  per round. Napi's call sites are `src/gen` (regenerated), `src/pump.c` (a
  runaway settles a `send()` with code `runaway`), `src/napi_system.c` (the
  fault count) and `bindings.rb`.
- **Costs.** An envelope is 168 bytes (`sizeof(ta_envelope)`). The macrostep
  reserve at its default of 1,024 envelopes is 172,032 bytes, taken from the
  pool on the first input and not again. `scale_test` reports 329 bytes per
  actor at 50,000 and at 200,000 actors (ADR 0103 gives 332 as today's
  figure; the two were not measured in one session). The batched-delivery
  check of `scale_test` sees 0.00 allocator calls per event with an
  event-block cache of 4 as well as with the default, where it saw one per
  event with a cache of 4 before: the inputs of a batch wait as envelopes in
  the pool and run one at a time.

### Implementation notes, 4.2c: mail, waiting lists and the router's end

- **Mail is core's.** `src/mail.{h,c}` turns a send into an envelope. A
  destination is classified without allocating: a resident session takes the
  envelope on the running input's `outgoing` list, a name nobody holds or a
  gone session of a family goes on `outgoing_waiting`, and a destination that
  can never be reached (no family, a finished session, a malformed target)
  is answered inside the sender's own macrostep with `error.communication`
  (SCXML 6.2.4). At the end of the macrostep `outgoing` is spliced onto the
  run queue in O(1) and `outgoing_waiting` is walked, each mail joining its
  address's waiting list; an abandoned or runaway macrostep frees both lists
  unseen, so its sends are dropped (`ta_actor_abandon` says so now). Due
  delayed sends and activation sends are posted at a safe point after
  `ta_mail_prepare` made room.
- **Waiting and LOAD_NEEDED.** The first mail or host input for an address
  that no session holds reports one `LOAD_NEEDED` notice (address, trace,
  session); the rest wait behind it on the address's list in order.
  `ta_address_load_failed` recycles the list: mail comes back to its sender as
  a data-free `error.communication`, host input goes to the `refused` list
  with the reason in `flags`. A session that loads at the address, or
  `ta_address_loaded` for a gone session's key, moves the list to it. A key
  is `{ family, "<incarnation>.<session>.<family>" }`; a session of another
  incarnation parses as session 0 and has the key `<incarnation>.0.<family>`.
  `ta_address_waiting` counts a list.
- **Gone.** `ta_system_set_external_event_router`, `ta_external_delivery`,
  `ta_external_event_submit_params`, the retry status and the delayed send's
  `submitted` flag are deleted with every caller (kernel, test host, napi).
  `ta_due_dispatch_params.fired` replaces the submit-and-retry loop of
  `ta_system_dispatch_due`.
- **Costs.** The pool and the 168-byte envelope are unchanged; a mail that
  waits makes no allocator call, and the sweep `mail_sweep_test` fails every
  allocation of a send, a placement and a recycle in turn.
- **Ordering across sources.** Host posts to a non-resident named address
  wait in the kernel's per-load mailbox and chart mail for it waits on core's
  list; when the load ends core's list moves first and the kernel's mailbox
  is handed over after it. Each source keeps its order; chart mail precedes
  host posts to the address, whatever order they arrived in. This goes when
  the kernel's mailboxes do.
- **Not changed.** The kernel keeps the mailboxes of the host's own posts
  (`tah_system_post_named`) until the scheduler step, and the `ta_input_watch`
  hook stays an interim kernel hook that goes in Phase 6. `waiting-full`
  (a family's `maxWaiting`) cannot apply to chart mail, which the pool bounds;
  it still limits host posts.
- **The event-block cache is gone (4.2d).** A run has one slot, so at most one
  session is inside a macrostep, and the reuse the cache gave is one block.
  `system->workspace` is that block: the empty block the running session hands
  back when its macrostep ends (at most the first queue's capacity, 8 events),
  for the next input to take; a block that grew, or a second empty one, is
  freed. It is made by the first input that finds none and freed by
  `ta_system_destroy`; a warm system allocates nothing per input, whatever the
  batch (`scale_test`, `params_test`). `TA_PARAM_EVENT_BLOCK_CACHE`
  (`eventBlockCache`) and its default are deleted with their callers.
  **Not done:** the block itself stays. The pre-run state of a session that is
  not in the slot (a traced spawn's parent, the internal events of a restored
  image, a publish's retirement, an invoke error report to an idle session)
  and the kept event of an invoked session still live in it, and the taken
  external event is still copied into its slots from the envelope; moving them
  to a start record in the sparse state and reading the envelope in place
  changes the session-image format and is left for a later unit.

**Who pays for a send: an executing macrostep, whoever sends (review fixes,
4.2d).** The runner sets `executing` only while `ta_run` is inside the
interpreter for the slot's input (`ta_run_executing`, run.h); a macrostep
paused by a budget keeps the slot but is not executing. A send made while it
is true is paid from the slot's claim and waits on the slot's outgoing lists,
whichever session sends. That covers an invoked child cancelled by its
parent's macrostep (leaving the invoking state, `<cancel>`, a final state):
the child's `<onexit>` mail belongs to the parent's input, follows the
parent's earlier sends (M4), and is dropped with them if the parent's
macrostep is abandoned or runs away (D2). A send made while it is false is at
a safe point: an invoked child's `<onexit>` running because the host destroys,
frees or tears down its parent. It makes room with `ta_mail_prepare` (best
effort) and posts the envelope straight to the run queue from the pool, as a
due send is, so the mail is delivered whether the slot is empty or held by
another session's paused macrostep, and it is not lost when that other
macrostep is abandoned; if the pool has no envelope, the sender hears
`error.communication`. Invoke completions and forwarded events
(`ta_invoke_deliver`) choose the same way. Covered by `tests/mail_test.c`
(`test_exit_mail_outside_a_macrostep`,
`test_exit_mail_inside_the_parents_macrostep`).

### Implementation notes, 4.2e: host input goes straight into core

Core's part of the unit that ends the kernel's mailboxes (the kernel's part is
in the kernel's ADRs and napi's ADR 0041). Four additions, all in
`src/run.{h,c}`, `src/mail.{h,c}`, `src/invoke.{h,c}` and `src/envelope.h`:

- **A token carries its envelope.** `sequence << 25 | envelope index`: the
  index takes 25 bits because the pool is bounded by 2^24 envelopes (index
  2^24 itself is the last, so 24 bits would not hold it), the sequence has the
  other 39. Nothing but `ta_input_withdraw` reads the index.
  `ta_input_withdraw(token)` marks a waiting host input
  `TA_ENVELOPE_WITHDRAWN` (the top bit of `flags`) and returns its session's
  `queued` count at once; the envelope is released unseen where its list next
  reaches it (the take, a waiting list that moves or fails, a discarded
  macrostep), so nothing is searched. A waiting list keeps its withdrawn
  envelopes until it moves, because its first envelope holds the list's load
  cue; `ta_address_waiting` does not count them, and `ta_waiting_length` (the
  owner's own question) does. A token whose envelope was released and reused
  fails the check on `envelope->input`. A new envelope state `DECIDED` marks a
  host input that waits on the refused list for its notice, which cannot be
  withdrawn any more.
- **`ta_counters.host_inputs`** is a gauge of the host inputs (HOST and
  TRANSITION) admitted and not decided. It goes down where the end is decided
  (`ta_run_input_decided`: completion, a refusal, an abandon, a withdrawal),
  not when the notice is written, so a host that admits against it never
  waits for a notice to see room.
- **Admission inside a macrostep.** `ta_actor_deliver` and
  `ta_actor_report_invoke` may be called while `ta_run_executing`: the input
  is paid from the slot's claim and put on the slot's `outgoing` (or
  `outgoing_waiting`) list by `ta_mail_hold`, so it is ordered with the
  macrostep's own mail, invisible until the macrostep ends, and answers
  `TA_ERR_FULL` when the claim is spent. The watermark does not apply there.
  An abandoned or runaway macrostep refuses the host inputs it admitted with a
  REFUSED notice of reason `TA_REFUSED_DISCARDED` carrying their tokens. This
  is what lets a data-model callback post without a kernel queue to defer to.
- **An error report is an input.** `ta_actor_report_invoke` with
  `TA_INVOKE_REPORT_ERROR` no longer answers `TA_ERR_BUSY`: it interns the
  event name, makes an envelope of source `ERROR` flagged
  `TA_ENVELOPE_REPORT` (the invocation in `from`, `REPORT_ENDS` for `ends`)
  and queues it. `ta_run` takes it with `ta_invoke_report_take`, which queues
  the event, retires the invocation if the report ends it, moves a replaying
  session's clock and starts a macrostep; that macrostep runs from the
  started queue in the same call. The invocation is looked up again at the
  take, so a session that cancelled it meanwhile drops the report. The name's
  intern is the one allocation that can happen inside a macrostep (a failed
  start reports from `start`); it goes with the name area (4.2b).
- **Inspection.** `ta_actor_pending` writes a session's waiting inputs, in the
  order they will be taken, into the caller's array (no allocation; the cost is
  the run queue up to the capacity-th match). A TAKEN notice carries the
  input's `time`.
- **The input watch and the refusal's address.** `ta_input_watch.mark` is
  called for every input `ta_run` takes except an operator's transition (host
  inputs and error reports included), because a host input now waits in core
  and the host cannot mark it before it is taken. A host input that waited
  for an address and was refused by `ta_address_load_failed` keeps its
  reference to the address in `payload.address`, and the REFUSED notice names
  it (`ta_run_notice.address`), so a host can tell the application which
  address a dead letter was for.
- **What the kernel needs from core, and gets.** `ta_run_executing` and
  `ta_input_slot(token)` are public: the first tells a binding whether a post is
  made by a macrostep (and so waits for its end) or from outside core, the second
  is the token's envelope slot, so a host can keep one small record per input in a
  table it indexes itself, with no hashing and no allocation in a steady state
  (compare the whole token before trusting an entry: a slot is reused). A REFUSED
  notice carries the refused input's trace parent (`ta_run_notice.trace`) as well
  as its event and address, because the envelope owns it and a dead letter is
  reported with it. The carrier of an error report names its invocation in the
  event's `invoke_id`, so a TAKEN notice lets a recorder journal the report with it.
- **Lent events and the idle call.** A TAKEN or REFUSED notice lends its event
  until the next `ta_run`. A host that has nothing more to run therefore makes
  one more, idle, call after the last notices of a turn, or the data of a refused
  input would stay alive until the next turn (the kernel does this only when the
  turn saw such a notice).
- **The kernel's part.** The kernel keeps no mailbox, ready or runnable queue,
  quarantine or envelope store. It calls `ta_actor_deliver` from `post()` and
  `send()`, keeps a table indexed by `ta_input_slot` for the sends that wait and
  the retained bytes it counts, reports not-found and waiting-full answers as
  dead letters at the next turn, and settles waiters and dead letters from the
  COMPLETED and REFUSED notices. `maxQueuedMessages` is checked against
  `ta_counters.host_inputs`. The turn's bound is in inputs, checked between
  `ta_run` calls (napi ADR 0041).
- **Tests:** `tests/input_test.c` (withdrawal in every state, the gauge,
  admission and withdrawal inside a macrostep, an abandoned and a runaway
  macrostep, error reports, `ta_actor_pending`, the notice time) and
  `tests/input_sweep_test.c` (exhaustive: every allocation of a delivery, of a
  report's admission and of its take; a withdrawal with every allocation
  refused).
