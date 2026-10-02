# The macrostep: invariants, end reports, and running out

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

This document says what a macrostep may and may not do under the ring
protocol: when one starts, how it ends, what the host may do with its
records, and what happens when something runs out. The rings are in
[ring.md](ring.md), and the envelopes it takes in [envelope.md](envelope.md).

## The invariants

1. **A macrostep never allocates in core.** Core allocates in three places
   only:
   - when a system is created (rings, pool, scratch, the continuation);
   - when a definition is registered (its compiled form, atoms and required
     interpreter workspace, including mandatory internal-event storage);
   - when a session is created or loaded (its fixed-size record, sized by its
     definition).

   Growing the pool up to its bound happens between macrosteps
   ([envelope.md](envelope.md#the-pool)). The data model allocates as it
   likes: that is the data model's memory, not core's
   ([data-model.md](data-model.md)).
2. **Core storage is bounded by parameters and compiled requirements.** Every queue, ring and text
   buffer has a size set when the system is created: `poolEnvelopes`,
   `nameAreaBytes`, `internalQueueEvents`, `h2cBytes`, `c2hBytes`, `telBytes`,
   `maxRecordBytes` ([BEHAVIORS.md](../BEHAVIORS.md#parameters)). Compiler-sized
   mandatory internal storage is additional to the ordinary
   `internalQueueEvents` quota; its whole-execution bound is discussed below.
3. **Every overflow becomes a refusal to someone who can retry.** Nobody
   keeps a hidden queue that grows. There are three who can retry:

   | Who can retry | How they are told |
   |---|---|
   | the external client | the host refuses at its edge when h2c is full or core is not admitting |
   | the sending chart | its action fails as `error.execution.capacity` in its own macrostep |
   | nobody (an asynchronous reply) | it is dropped; the chart's own timeout covers it |

   [BEHAVIORS.md](../BEHAVIORS.md) lists, case by case, who drops what and
   who retries.

## Run to completion

Core runs one macrostep at a time, to its end, in the order of the run queue
([envelope.md](envelope.md#the-lists-an-envelope-can-be-on)):

1. **Choose.** Following the fairness rule, core takes the head of the run
   queue, or first admits an h2c record ([envelope.md](envelope.md#fairness)).
2. **Check the mandatory c2h reserve.** It covers the recorded input
   observation, macrostep end, cancellation of
   every invocation currently active in the session, terminal session
   notification, and all autoforwards of the input. It includes actual aligned
   record sizes and wrap padding. Unless the reserve fits, core takes nothing
   and `ta_run` returns. A definition/session whose mandatory batch cannot fit
   even an empty ring is refused before execution
   ([ring.md](ring.md#the-c2h-reserve)).
3. **Take** the envelope after its recording snapshot has been prepared
   without changing the session. For a recorded session, write `c2h.input`.
   Bind the event, apply the matching invocation's finalize handler, then
   copy and autoforward at the SCXML algorithm's forwarding point before
   transitions run. Each forward preserves the event's fields separately
   from its destination invocation identity. A forwarding copy failure
   raises an execution error and skips that recipient, as ADR 0097 specifies.
4. **Run** microsteps until the session is stable or final. Effects go to
   c2h, mail and timers to the pool, internal events to the internal queue.
5. **Macrostep end and routing.** Write `c2h.macrostep_ended`. A bounded drain
   phase routes outgoing mail and commits required load and wake notifications.
   Its cursor remains in the existing lists if c2h must be drained; the
   completed macrostep is not repeated. Its taken envelope remains owned
   through this phase and any startup follow-up.
6. **Start pending invocations.** This is a separate phase after stabilization,
   outside the macrostep. Process any resulting internal errors before taking
   another external input or running another session. Repeat stabilization and
   startup until settled, then write `c2h.input_completed` and release the
   taken envelope. Completion is emitted for unrecorded inputs too.

Ordinary c2h actions preserve the outstanding mandatory reserve. Invocation
cancellation follows a state's exit content even when that content fails.
No new service starts inside a stabilization macrostep, so each invocation
active at its start needs at most one cancellation record. Repeated internal
exit/re-entry does not increase this bound. Local child stops run separately,
with their own reserves, rather than recursively expanding the parent's
reserve to its entire invocation subtree.

**No session is busy when its envelope comes up**, because nothing else is
in the middle of a macrostep. That removes `TA_ERR_BUSY` (core ADR 0055),
the per-actor mailboxes, and the host's scheduler.

**Why not interleave other sessions while one is paused?** Earlier
iterations of this design considered it. Interleaving does not prevent
deadlock: that is done by refusing sends that don't fit, which works
without it. And it would need one continuation for every paused session.
Run to completion needs **one continuation per system**.

An attached debugger takes exclusive ownership of that same system instead
of interleaving actors. It can inspect a budget pause, finish the current
microstep without starting another, abandon at a legal boundary and drive
an operator transition. Other actors remain stopped while it is attached
([debugging.md](debugging.md)). These native debugging primitives are the
exception to the production driver's single `ta_run` interface below.

### A sketch of the call

```c
typedef enum {
    TA_RUN_IDLE,        /* nothing runnable and nothing to admit */
    TA_RUN_MORE,        /* the budget ran out between macrosteps */
    TA_RUN_PAUSED,      /* the budget ran out inside a macrostep */
    TA_RUN_C2H_FULL     /* the reserve is not there: drain c2h and call again */
} ta_run_status;

typedef struct {
    uint32_t budget;          /* microsteps (ADR 0009) */
    uint32_t action_budget;   /* units of executable content (ADR 0061) */
    TA_OUT ta_run_status *status;
} ta_run_params;

TA_API ta_result ta_run(ta_system *system, TA_IN const ta_run_params *params);
```

This one call replaces `ta_actor_deliver`, `ta_actor_resume` and
`ta_system_dispatch_due` for a running system. The host's loop is:

1. post to h2c;
2. call `ta_run`;
3. drain c2h and tel;
4. arm the timer from the last [`c2h.wake`](../protocol/generated/MESSAGES.md#c2hwake);
5. repeat.

## The budget pause is the only pause

A macrostep pauses only when `budget` or `action_budget` runs out (ADR 0061).
The pause gives a host's event loop its turn back during a long macrostep,
such as a `<foreach>` over 10,000 items.

- The paused macrostep's position, sets and frames move into **the system's
  continuation**. It is reserved when the system is created, so saving a
  pause never fails (ADR 0070 section 2 already requires this). Nested
  interpreters for invocations still have scratch of their own (ADR 0060).
- **While it is paused, nothing else runs.** The host may post to h2c, drain
  c2h and tel, and serve its own I/O. The next `ta_run` continues the same
  macrostep.
  During debugger attachment only the debugger drives core; the production
  pump cannot resume the macrostep until ownership is returned.
- **A full ring never pauses executable content inside a macrostep.** An
  ordinary action whose record does not fit fails instead (below), while
  mandatory records use their preflight reservation. Core can return for a
  drain before a macrostep starts or between post-macrostep phases, including
  startup and terminal input completion.

## The macrostep end

A macrostep's records reach c2h in this order:

1. its `c2h.input`, if the session is recorded;
2. its effects: [`c2h.send`](../protocol/generated/MESSAGES.md#c2hsend),
   [`c2h.invoke_cancel`](../protocol/generated/MESSAGES.md#c2hinvoke_cancel),
   [`c2h.invoke_forward`](../protocol/generated/MESSAGES.md#c2hinvoke_forward),
   [`c2h.log`](../protocol/generated/MESSAGES.md#c2hlog);
3. its `c2h.macrostep_ended`.

**`c2h.macrostep_ended` reports that execution ended and how it ended,
including abandonment or runaway termination. It does not gate effects or
commit storage.** The host may execute each effect as soon as it reads the
record, for recorded and unrecorded sessions alike. It need not wait for
the macrostep end or for a journal write.

When `ta_run` returns on a budget pause, the host can drain c2h, execute
sends, invocations and logs, release ring space, and post answers on h2c.
The next `ta_run` resumes the macrostep. No collection of effects has to
be held until the macrostep end. An application may choose its own buffering or
durability policy, with its own memory bounds.

A crash or operator abandonment can occur after some effects have already
run. Neither the macrostep end nor abandonment rolls them back. Charts and I/O
processors use timeouts, retries and business-key idempotency for work that
matters. Recording observes inputs independently of effect execution;
[replay seeds a new execution](recording-and-replay.md).

### Invocation startup and input completion

Entering a state marks its invokes as pending. Exiting that state before
stabilization removes them. Only surviving pending invokes are evaluated and
started, in state entry order and invocation document order, after the
macrostep end (SCXML 3.13 and appendix D). Argument evaluation and local child
creation do not happen in the state's entry block.

Startup can allocate core memory and can return for the host to drain c2h
before its next operation. This boundary is outside a macrostep. Starting a
local child first acquires its lifetime envelope token; starting an external
service writes `c2h.invoke`. An operation either starts the service completely
or leaves it unstarted and queues its error using mandatory internal storage.

SCXML invocation errors are processed before the next external dequeue.
Startup-generated errors therefore start an internal-only stabilization
macrostep of the same session. Each such macrostep has its own ID and end
record; all retain the original input's source correlation. Its c2h reserve
is checked before it starts. A new startup phase may follow it. Other sessions
and ordinary h2c commands cannot overtake this chain.

Core reserves `c2h.input_completed` separately before consuming the terminal
completion phase; if c2h is full it retains the source envelope and returns
for a drain. The record is written once after routing and mandatory load/wake
notifications have been flushed and that chain settles, or ends through
runaway, abandonment or another defined terminal result. It carries the
source correlation even when recording is disabled. An individual
`c2h.macrostep_ended` does not resolve the host's input promise while startup
or internal follow-up remains pending. Completion is an execution result,
not an acknowledgment that remote effects finished or were persisted.

A normal local child completion is an external `done.invoke` input to its
parent. Cancellation logically retires the instance after the parent's exit
handlers and schedules child exit cleanup ahead of ordinary child events.
Canceled children emit no done event. The lifetime token guarantees both
paths without allocating an envelope during a macrostep
([envelope.md](envelope.md#local-invocation-tokens)).

Local mail joins the run queue at the macrostep end because core runs one macrostep
at a time. Its outgoing list is scheduling state, not a host effect hold.
An abandoned macrostep frees that list, while effects the host already
executed remain executed.

## Two kinds of exhaustion

| | Who can free it | What happens | Does the chart see it? |
|---|---|---|---|
| **Rings:** c2h full when an action writes | the host, by draining | The action fails as `error.execution.capacity.c2h`. The run doesn't wait. | yes |
| **Before a macrostep:** c2h lacks the reserve | the host, by draining | No macrostep starts, and `ta_run` answers `TA_RUN_C2H_FULL`. | no |
| **h2c full** | core, by admitting | The host refuses at its edge | no; the external client does |
| **The pool or name area** when an action needs an envelope or text | the receivers, by running | The action fails as `error.execution.capacity.pool` or `.names` | yes |
| **Admission:** pool above `admissionWatermark` | the receivers, by running | Core consumes the event and writes a reserved capacity bounce; following controls remain reachable | no; the external client does |
| **tel full** | the host, by draining | The record is dropped and counted | no |

So the chart sees exactly one thing: **`error.execution.capacity`, raised by
the action that couldn't be done.** It works like any other error in
executable content:

- **The rest of the block is abandoned.** A fanout fails once, at the first
  send that doesn't fit, not once per remaining item.
- **The last segment of its name says what ran out:** `pool`, `names`,
  `c2h`, `internal` or `record`, as the catalog's
  [`capacity_resource`](../protocol/generated/MESSAGES.md#capacity_resource-u8)
  enum lists them. A chart that doesn't care which one matches
  `error.execution.capacity`.
- **`_event.data` says where:** the action and the `<foreach>` iteration, as
  `ta_trace_position` gives them.
- **The chart decides what to do next:** retry later with a delayed send to
  itself, send less (ack-clocked sending), or give up and report.

Errors use compiler-sized mandatory internal storage, separate from the quota
for ordinary raises. The remaining whole-execution sizing obligation is
explicit below; a single reserved slot does not establish this guarantee.

**Why the resource is the last segment of the name,** and not only a field
of `_event.data`:

- **A chart chooses by matching, which is how SCXML chooses.** A chart that
  only cares that something ran out matches `error.execution.capacity`. One
  that handles a full pool differently from a name that is too long matches
  `error.execution.capacity.pool`. Neither needs a condition that reads the
  data.
- **It stays SCXML-compatible.** Every name is still an `error.execution`
  prefix match. A chart written without capacity errors in mind catches
  them in its existing `error.execution` handler.
- **The set is closed.** The catalog's enum lists the resources, so a chart
  author can see every name that can occur.

## The internal queue: compiler-sized mandatory storage

Internal events (`<raise>`, `done.state.*`, `error.*`) share an ordered queue,
not the envelope pool. `internalQueueEvents` is the ordinary raise quota.
Additional compiler-sized storage holds mandatory errors and completion
events. A `<raise>` that exhausts the ordinary quota fails as
`error.execution.capacity.internal` in mandatory storage. The mechanics are in
[envelope.md](envelope.md#the-internal-queue-is-not-in-the-pool).

**Why not envelopes:**

- **Internal events never outlive their macrostep.** They need no admission,
  no deadline, no waiting list and no recycling, which is everything the
  pool provides.
- **They shouldn't compete with mail.** If they came from the pool, a full
  pool would stop a chart from raising its own events, and a chart raising
  in a loop would starve every other session's mail. Separate memory keeps a
  fault inside the chart that causes it.
- **Only one queue is live at a time.** Under run to completion, one
  interpreter runs at the top level, so the system needs one queue, plus one
  per nested interpreter, sized once. That replaces ADR 0064's queue lent to
  each actor and its cache.
- **The compiler counts independent failure sites.** SCXML 4.9 stops only
  the failed block. Two entry blocks can each fail in one microstep, and
  both errors must retain their queue order. A loop in one block stops at
  its first error; it does not need one error slot per unexecuted iteration.

### Sizing mandatory storage

The compiler records conservative per-phase maxima: errors from independently
executed blocks and data initialization, errors from a transition-selection
pass, errors from an invocation-startup pass, and `done.state` events. Include
bootstrap, final-state completion data evaluation and cancellation cleanup.
Generated error names and diagnostic positions must also use pre-sized
storage; reporting a failure must not require another fallible allocation.
Registration, spawn and load size the system and any nested interpreter
workspace before it becomes usable, and refuse atomically if sizing or
allocation fails. Neither quota grows during execution.

**A per-phase maximum is not yet a whole-macrostep maximum.** Earlier
mandatory events can remain queued while later microsteps append more. If a
definition has at most `L` executed microsteps, at most `S` selection passes
and at most `I` startup passes in one input's stabilization/startup chain,
and the compiler bounds these phases by `E`, `C` and `V` mandatory events
respectively, a conservative extra arena has
`B + L*E + S*C + I*V + F` entries. `B` and `F` bound bootstrap and final or
stop cleanup. Reserving all generated mandatory events is sufficient even
if none have been dequeued; the ordinary quota is additional. Checked
arithmetic and available-memory checks precede publication. Tighter
occupancy bounds may reduce this conservative allocation.

**Remaining proof obligation:** the existing `microstepLimit` bounds executed
microsteps in one stabilization macrostep, not total `L` across an unrestricted
startup/follow-up chain, nor `S`. Selection can raise errors without
taking a transition. Two failing guards can repeatedly append more errors
than an internal dequeue removes, without executing another microstep.
Budget pauses do not bound the accumulated queue across resumes. Disabling
the microstep limit also removes the bound on `L`.

No finite compiler-sized arena can cover an unrestricted error-generating
loop solely from its definition's per-phase counts. The implementation needs
either a proven finite occupancy bound for the admitted definition or a
separately decided execution guard that also bounds selection and startup
passes. For example, a finite guard that counts every selection pass, every
microstep and every startup pass would make the arithmetic above finite.
This proposal does not introduce that guard, alter `microstepLimit`, or
terminate on internal capacity exhaustion by implication. Closing this
whole-execution bound is required before claiming lossless mandatory events
for unrestricted definitions.

### Consequence for chart authors: order inside a block matters

```xml
<onentry>
  <send id="t" event="sending.timeout" delay="30s"/>  <!-- 1. before the fanout -->
  <foreach array="batch" item="n">                   <!-- 2. may fail partway -->
    <send type="x-notify" target="notify:" event="notification" namelist="n"/>
  </foreach>
</onentry>
<transition event="error.execution.capacity" target="throttled"/>
```

If the timeout came after the `<foreach>`, a fanout that ran out would also
lose its timeout, and nothing would ever wake the chart.
[scenarios/S02-full-pool.md](../scenarios/S02-full-pool.md) runs this case
both ways.

## What this replaces: the out-of-memory stop (ADRs 0047, 0070)

ADR 0047 says a macrostep interrupted by `TA_ERR_NOMEM` can be retried.
ADR 0070 makes that hold everywhere: a run that runs out of memory stops
"where it stands", at the last boundary before the allocation that failed,
and a later resume finishes it. Each allocation that could fail mid-run
needed its own care. ADR 0070 lists them. Here is where each one goes:

| Allocation that could fail mid-run (ADR 0070) | Under the ring protocol |
|---|---|
| a second `<raise>` or error in one microstep (the reservation held one event) | an ordinary internal entry for a raise, or compiler-sized mandatory storage for an error; distinct failing blocks keep distinct errors ([envelope.md](envelope.md#the-internal-queue-is-not-in-the-pool)). |
| a delayed `<send>` growing its owner's storage (ADR 0054) | an envelope with a due time ([envelope.md](envelope.md#delayed-sends-envelopes-with-a-due-time)) |
| interning an `eventexpr`, `typeexpr` or target | looked up, never interned ([names.md](names.md)) |
| scratch for `<param>`s | fixed scratch, sized at creation (ADR 0092) |
| `done.state` events and a top-level final state's completion | compiler-sized mandatory storage; local parent completion uses the invocation's lifetime envelope token |
| recording an invocation | a slot in the session's record, sized by the definition's `<invoke>` count when the session is created |
| holding a send or a `<log>` for an unpublished actor | a replaying session writes no effects. Its timers are envelopes ([recording-and-replay.md](recording-and-replay.md)). |

**Starting a local child chart** (`<invoke type="scxml">`) creates a session
in the post-macrostep startup phase. Core acquires the lifetime token there,
not while entering the invoking state. A successful child keeps it for
completion or cancellation. Failure starts no child, releases any partially
prepared resources and queues the corresponding invocation error before the
next external input. Storage for all errors in that startup pass is sized
before any invoke executes.

Core no longer stops an executable block because core ran out of memory.
`TA_ERR_NOMEM` can arise only outside macrosteps, while growing the pool or
preparing a session and its workspace. A failed preparation changes nothing;
an invocation-start failure follows the internal-error rules above.

**A data model or invoker that runs out of its own memory** (a JavaScript
heap, say) answers `TA_OP_ERROR`, and the chart sees `error.execution`, as
for any other failed operation ([data-model.md](data-model.md)). `TA_OP_NOMEM`
leaves `ta_op_status`, and with it the last path that stops a run where it
stands: a shortage anywhere becomes a refusal the chart sees. The price is
that a passing memory shortage in the host becomes chart behaviour, which is
what charts are for ([migration.md](migration.md)).

**Why an error and not a pause:**

- **The memory is the data model's, not core's.** Core can't free it and
  can't know when it will be free. A pause that waits for it is a guess.
- **A pause could not be resumed safely.** An operation that ran out of
  memory partway, such as an `<assign>` that built half an object in the
  JavaScript heap, may already have changed something. ADR 0047's retry is
  safe only for operations that changed nothing, and core can't check that
  for a foreign data model.
- **Charts are built to handle errors.** `error.execution` is the SCXML
  answer to an operation that failed. The chart's own structure (retry,
  give up, report) decides, as it does for every other refusal.
- **It deletes the last of the machinery.** With no `TA_OP_NOMEM`, nothing
  in a macrostep can stop it where it stands. ADR 0070's stop and resume,
  and ADR 0047's retry guarantee, have nothing left to protect.

The out-of-memory sweeps in the test suites (fail every allocation in turn)
become much smaller for core. A macrostep has no allocation to fail, so the
sweep covers creation, registration, spawn and load, and pool growth. The
sweep over replay in the kernel (`replay_test` with `TAH_FULL=1`) goes away
along with the kernel ([migration.md](migration.md)).
