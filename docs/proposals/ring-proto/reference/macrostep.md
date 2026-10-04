# The macrostep: invariants, end reports, and running out

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

This document says what a macrostep may and may not do under the ring
protocol: when one starts, how it ends, what the host may do with its
records, and what happens when something runs out. The rings are in
[ring.md](ring.md), and the envelopes it takes in [envelope.md](envelope.md).

## The invariants

1. **A macrostep never allocates in core.** Core allocates in three places
   only:
   - when a system is created (rings, pool, the running workspace);
   - when a definition is registered (its compiled form, atoms and required
     interpreter workspace, including mandatory internal-event storage, by
     growing the one running workspace);
   - when a session is created or loaded (its fixed-size record, sized by its
     definition).

   Growing the pool, the slot records and the park area up to their bounds
   happens between macrosteps ([envelope.md](envelope.md#the-pool),
   [parking](#parking-one-running-workspace-and-a-park-area)). The data model allocates as it
   likes: that is the data model's memory, not core's
   ([data-model.md](data-model.md)).
2. **Core storage is bounded by parameters and compiled requirements.** Every queue, ring and text
   buffer has a size set when the system is created: `poolEnvelopes`,
   `nameAreaBytes`, `internalQueueEvents`, `maxConcurrentInputs`, `h2cBytes`,
   `c2hBytes`, `telBytes`, `maxRecordBytes` ([BEHAVIORS.md](../BEHAVIORS.md#parameters)). Compiler-sized
   mandatory internal storage, one round of every phase, is additional to
   the ordinary `internalQueueEvents` quota. Beyond it, mandatory events use
   free ordinary entries, and an overflow is a runaway
   ([below](#sizing-mandatory-storage)).
3. **Every overflow becomes a refusal to someone who can retry.** Nobody
   keeps a hidden queue that grows. There are three who can retry:

   | Who can retry | How they are told |
   |---|---|
   | the external client | the host refuses at its edge when h2c is full or core is not admitting |
   | the sending chart | its action fails as `error.execution.capacity` in its own macrostep |
   | nobody (an asynchronous reply) | it is dropped; the chart's own timeout covers it |

   [BEHAVIORS.md](../BEHAVIORS.md) lists, case by case, who drops what and
   who retries.

## Macrostep slots

Core runs each session's macrosteps one at a time, to their end, but it
does not make every other session wait for them. It has up to **`maxConcurrentInputs`
slots** ([BEHAVIORS.md](../BEHAVIORS.md#maxconcurrentinputs)). A slot stands
for one session's execution of one input, from take until
`c2h.input_completed`: its taken envelope, its outgoing mail list, its
outstanding c2h reserve and its chain count. A session that holds a slot is
**active**. With one slot, this is plain run to completion.

Only one slot runs at a time, on the system's one thread, so only the
running slot needs an interpreter's workspace. The others are **parked**:
their continuation and pending internal events are copied into the park
area ([below](#parking-one-running-workspace-and-a-park-area)). A slot is
therefore cheap: a small record while it waits for its turn, and about the
size of its definition's continuation while it is parked.

Core works through its slots in turn. A slot runs until its input's chain
settles, or until it has used `slotSlice` steps in this turn; then core
moves to the next slot that can run. When a slot is free, core fills it in
the order of the run queue
([envelope.md](envelope.md#the-lists-an-envelope-can-be-on)), skipping
envelopes whose target is active ([envelope.md](envelope.md#mail-for-an-active-session)).
For one slot, the steps are:

1. **Choose.** Following the fairness rule, core takes the first runnable
   envelope whose session is not active, or first admits an h2c record
   ([envelope.md](envelope.md#fairness)).
2. **Check the mandatory c2h reserve.** It covers the recorded input
   observation, macrostep end, cancellation of
   every invocation currently active in the session, terminal session
   notification, and all autoforwards of the input. It includes actual aligned
   record sizes and one pad of up to `maxRecordBytes` − 8 per record. The
   outstanding reserves of all slots, this one included, must leave room
   for one ordinary record plus its pad: 2 × `maxRecordBytes`.
   Unless they do, core takes nothing; when no slot can run either, `ta_run`
   returns. A definition/session whose mandatory batch and that headroom
   cannot fit even an empty ring is refused before
   execution ([ring.md](ring.md#the-c2h-reserve)).
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

**The host never sees a busy session.** Mail for an active session waits
in core, on that session's deferred list, in arrival order. That removes
`TA_ERR_BUSY` as an answer to the host (core ADR 0055), the host's
mailboxes and the host's scheduler. The scheduling that remains is core's:
a fixed number of slots, taken in turn.

### Why slots, and why a fixed number

An earlier version of this design ran one macrostep at a time for the
whole system, with one continuation. It rejected interleaving because
interleaving does not prevent deadlock. That is true, and refusing local
sends that don't fit, while waiting only for c2h, which the host drains,
still prevents it ([below](#a-full-c2h-pauses-the-slot)). But interleaving
was never for deadlock. It is for isolation: with one continuation, any long macrostep stops every
other session until it ends.

- **Long but correct macrosteps exist.** A chart that walks a list with a
  guarded eventless transition, a parser driven by `<raise>` chains, a
  `<foreach>` over 10,000 items. With one slot each of them holds every
  other session, and every client waiting on one, until it finishes.
- **A loop that never ends stops the whole system.** Core cannot tell a
  loop from long work before its limit. With one slot, a loop has the
  system until the limit; with several, it has one slot.
- **Quarantine has nothing left to do.** napi quarantines a runaway so
  that it stops taking the loop's turns. With slots, a runaway only ever
  holds one slot, and the chain limit ends its input
  ([below](#runaways-the-chain-limit)). The implementation removes
  quarantine.

The number has a ceiling, so memory stays bounded and allocated outside
macrosteps, but nothing is set aside for it. Core already moves a paused
run into a continuation (ADRs 0061 and 0070); parking is that move, into
chunks of the park area instead of a block per actor. Slot records are
allocated on demand up to `maxConcurrentInputs`, as the pool is: when core
would fill a slot and every existing record is busy, it allocates one
more, between macrosteps. If that allocation fails, the envelope waits for
a slot to free.

Slots interleave only between units of executable content, where ADR 0061
already lets a run stop. They do not interleave inside a data model call:
a synchronous call that never returns still holds the thread
([data-model.md](data-model.md)).

An attached debugger takes exclusive ownership of the system and every
slot. It can inspect any active slot, finish a microstep without starting
another, abandon at a legal boundary and drive an operator transition.
Other actors remain stopped while it is attached ([debugging.md](debugging.md)).
These native debugging primitives are the exception to the production
driver's single `ta_run` interface below.

### A sketch of the call

```c
typedef enum {
    TA_RUN_IDLE,        /* nothing runnable and nothing to admit */
    TA_RUN_MORE,        /* the budget ran out and no slot is inside a macrostep */
    TA_RUN_PAUSED,      /* the budget ran out with at least one slot inside a macrostep */
    TA_RUN_C2H_FULL     /* every slot that could run waits for c2h and no input can be taken: drain c2h and call again */
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

## Pauses: the budget and the slice

A macrostep stops in the middle for three reasons only:

- **`ta_run`'s `budget` or `action_budget` runs out** (ADR 0061). The pause
  gives the host's event loop its turn back. The host may post to h2c,
  drain c2h and tel, and serve its own I/O. The next `ta_run` continues
  every slot where it stood.
- **A slot uses its `slotSlice`.** Core moves to the next slot that can
  run, within the same `ta_run`. This keeps a long macrostep from holding
  the others.
- **c2h has no room for the next unit's record**
  ([below](#a-full-c2h-pauses-the-slot)). The slot waits before that unit
  until the host drains c2h. Core moves to the next slot that can run.

A slot that reaches the chain limit does not stop in the middle: its input
ends ([below](#runaways-the-chain-limit)).

When `ta_run`'s budget runs out, the running slot stays in the running
workspace; nothing is copied. When a slot's slice ends, or it waits for
c2h, core parks it. Parking can fail without harm: a slot that cannot park
keeps the thread
([below](#parking-one-running-workspace-and-a-park-area)).
Nested interpreters for invocations have scratch of their own (ADR 0060).
While a debugger is attached, only the debugger drives core; the
production pump resumes no slot until ownership is returned.

Mandatory records never wait inside a macrostep: they use their preflight
reservation. Core can return for a drain before a macrostep starts or
between post-macrostep phases, including startup and terminal input
completion.

### A full c2h pauses the slot

An ordinary action writes at most one c2h record. For example, a `<send>`
to an I/O processor writes [`c2h.send`](../protocol/generated/MESSAGES.md#c2hsend),
and a `<log>` writes [`c2h.log`](../protocol/generated/MESSAGES.md#c2hlog).
Such a record never fails for lack of room. The slot waits for room:

- **Core checks for room before the unit starts.** A unit is an action or
  a `<foreach>` iteration (ADR 0061). A unit may write c2h when it is a
  `<log>`, or a `<send>` whose type or target is not a literal local
  session. A `<send>` with `typeexpr` or `targetexpr` is checked, because
  its destination is known only after evaluation. Before such a unit, core
  checks that every slot's outstanding reserve leaves room for the largest
  record: `maxRecordBytes` plus its pad. The check comes before any
  expression is evaluated, before a sendid is counted and before
  `idlocation` is assigned.
- **Without that room, the slot parks before the unit.** Nothing of the
  unit has run. The unit runs once, after the host drains, so an
  expression with side effects is evaluated once. After the check the
  record always fits, because no record is larger than `maxRecordBytes`.
  The price is a wait when the real record would have fit in less room.
  Why not size the record first and wait only when it does not fit: that
  evaluates the unit's expressions, and a full c2h would then make the
  unit evaluate them again.
- **Other slots go on.** Core moves to the next slot that can run, as at
  the end of a slice. A waiting slot keeps its taken envelope, its
  outgoing mail, its deferred list and its outstanding reserve.
- **`ta_run` returns `TA_RUN_C2H_FULL` when nothing else can run:** every
  slot that could run waits for c2h, and no input can be taken. The host
  drains c2h and calls again. While other work remains, `ta_run` goes on
  until its budget or the work runs out, and the host drains c2h on that
  return as on any other.
- **A slot that cannot park keeps the running workspace**
  ([below](#parking-one-running-workspace-and-a-park-area)). No other slot
  runs, and `ta_run` returns `TA_RUN_C2H_FULL` at once.

A record larger than `maxRecordBytes` never fits, so waiting cannot help.
Its action fails as `error.execution.capacity.record`. That is the only
c2h failure an action has; there is no `error.execution.capacity.c2h`.

Startup records, such as
[`c2h.invoke`](../protocol/generated/MESSAGES.md#c2hinvoke), are written
outside a macrostep. Startup returns for a drain before an operation it
cannot write.

**The wait always ends.** A waiting slot needs room for one record of at
most `maxRecordBytes`, plus a pad at the wrap. Core takes an input only
when the outstanding reserves, its own included, leave 2 ×
`maxRecordBytes` of an empty ring
([ring.md](ring.md#the-c2h-reserve)). So once the host has drained, at
least one waiting slot's record fits, and that slot's reserve comes back
when its input completes.

**Why a full c2h pauses, and a full pool fails.** The rule is: pause where
the host can free the space, fail where only sessions can.

- **The host frees c2h without any session making progress.** Draining is
  the host's own work. A slot that waits for c2h waits for the host, never
  for another slot, so no cycle of waits can form.
- **The pool and the name area are freed only by sessions.** An envelope
  is freed when its receiver takes the mail, and its text with it. Two
  slots that each wait for an envelope the other would free never go on.
  So a full pool or name area fails the action as
  `error.execution.capacity.pool` or `.names`, in the chart.
- **Nothing is half done.** The record is not written, and core checks the
  fit before the unit changes core state. Running the unit from its start
  is what ADR 0061's pause between units does.
- **The pieces exist.** Slots park at unit boundaries, and each slot holds
  its own outstanding reserve while it is paused
  ([ring.md](ring.md#the-c2h-reserve)).

The data model's memory is different, and fails the operation
([below](#what-this-replaces-the-out-of-memory-stop-adrs-0047-0070)): core
cannot free it, and a data model operation that ran out partway may have
changed something.

**What it costs:**

- **The chart does not feel the outside world's backpressure.** A slow
  I/O processor means the host either buffers what it drains, within its
  own bound and policy, or stops draining. If it stops, the slot waits. A
  chart that wants to bound its work in flight clocks it by
  acknowledgements ([windowed chart](../charts/campaign-windowed.scxml)).
- **A slot that waits holds its input longer.** A slot parked in the
  middle of a fanout keeps its taken envelope, and mail for its session
  stays on the deferred list. That is latency, not failure.
- **A ring's size is a throughput setting, not a correctness one.** A
  small c2h bounds how many slots can hold a reserve at once, and an input
  waits in the run queue until its reserve fits. It is measured with the
  ring defaults ([BEHAVIORS.md](../BEHAVIORS.md#c2hbytes)).
- **A host must not make draining c2h wait for room in h2c.** If it did,
  core would wait for the host and the host for core. A host that cannot
  post a result buffers or drops it by its own policy
  ([S09](../scenarios/S09-edge-backpressure.md)).

### Parking: one running workspace and a park area

**The running workspace** is the interpreter scratch (`interpreterBytes`),
the internal queue with its ordinary quota and its compiler-sized
mandatory storage, and the frame stack. There is one per system, as there
was under run to completion, sized for the largest registered definition.
The system allocates it at creation, and registration grows it when a
larger definition arrives. Its mandatory storage, a floor of one round
of every phase ([below](#sizing-mandatory-storage)), is paid once per
system, not once per slot.

**A parked execution** is what a slot needs to continue later:

- its continuation, at the size its own definition needs: the state sets,
  history, enabled transitions and the frames actually open
  (`ta_continuation_capacity()` in core bounds it today; about 1 KiB for a
  chart of 100 states);
- the internal events actually pending, about 48 bytes each, in order;
- its counters: chain count, slice position, trace keys.

Its taken envelope and outgoing mail are already in the pool, and its c2h
reserve is a number, so none of them move.

**The park area holds parked executions as chains of chunks.** A chunk is
256 bytes, with a 4-byte link to the next. Parking writes the state in
order into chunks from the area's free list; resuming reads it back in
order into the running workspace and returns the chunks to the free list.
A larger chart takes more chunks, not a larger block, so the area never
fragments and no size has to be chosen in advance. The copy is a few
hundred bytes to a few kilobytes, once per slice.

**Growth happens between macrosteps only.** A rotation happens in the
middle of a macrostep, which must not allocate, so parking takes only
chunks already on the free list. After each macrostep end and at
admission, core tops the free list up to a reserve, enough for one
parked execution of the largest registered definition, allocating up to
the area's ceiling. If the allocator refuses, nothing changes but how much
can be parked.

**Parking can fail, and nothing breaks when it does.** Correctness never
depends on it, only isolation. A slot that finds too few free chunks at
the end of its slice does not rotate. It keeps the thread for another
slice, and core tries again at its next slice end. That is run to
completion for this macrostep.

**The ceiling is derived, not chosen.** A parked execution is at most the
largest registered definition's continuation plus a full internal queue.
So the area can never need more than

`maxConcurrentInputs × (largest continuation + full internal queue)`

rounded up to whole chunks. Core computes this at registration, so the
storage stays bounded by parameters and compiled requirements (invariant
2) without a byte parameter anyone must set. `parkAreaBytes` is an
optional lower cap ([BEHAVIORS.md](../BEHAVIORS.md#parkareabytes)); 0
disables parking, which leaves plain run to completion.

**It looks unlimited, with two limits.** Chunks taken during a burst of
rotations come back only when those executions resume, and new chunks
arrive only at the next macrostep end, so a burst can briefly cost
isolation. And chunks return to the free list, not to the allocator, so
after a burst the area stays at its high-water mark until the host trims
it.

**The host trims; core does not.** `ta_system_trim(system, keep)` is a
native call that the host makes between `ta_run` calls, when its own
policy says so: when idle, on a timer, under memory pressure. It frees
every whole page that is free and above its structure's reserve, or above
`keep`, in the pool, the name area and the park area. It is written once,
in the index pool (A5), so all three get it. Core never trims on its own:
when to give memory back is policy, and a heuristic in core would free and
refill in a loop. A page goes back only when every item on it is free.
Items never move, because envelope indices must stay stable, so a burst
can leave pages that are partly used and cannot be freed.

**What you don't pay for.** A system whose macrosteps all finish within
one slice never parks and holds only the reserve, which is itself
allocated at the first macrostep end that needs it. The running
workspace's mandatory floor is one round of every phase. It does not grow
with the chain limit.
**Possible:** the compiler could derive a lower `microstepLimit` for a
chart whose internal transitions (eventless transitions, `<raise>`,
`done.state`) form no cycle, since such a chart takes a bounded number of
steps before it needs external input. It could also give such a chart an
exact floor for its whole chain, so that the chart never overflows.

**Parking happens only between top-level units.** A child's exit handlers,
run in a nested interpreter while the parent cancels an invocation, finish
inside the unit that started them and never park. They are finite, but a
long one keeps the thread.

## The macrostep end

A macrostep's records reach c2h in this order:

1. its `c2h.input`, if the session is recorded;
2. its effects: [`c2h.send`](../protocol/generated/MESSAGES.md#c2hsend),
   [`c2h.invoke_cancel`](../protocol/generated/MESSAGES.md#c2hinvoke_cancel),
   [`c2h.invoke_forward`](../protocol/generated/MESSAGES.md#c2hinvoke_forward),
   [`c2h.log`](../protocol/generated/MESSAGES.md#c2hlog);
3. its `c2h.macrostep_ended`.

That order holds within one macrostep, not across them. With several
slots, the records of macrosteps of different sessions interleave on c2h.
A session runs one input at a time, so the header's `session` tells the
host which macrostep each record belongs to.

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
is checked before it starts. A new startup phase may follow it. The
session's own later inputs cannot overtake this chain, because the session
stays active and its mail waits on its deferred list. Other sessions may
run in other slots meanwhile, which SCXML permits: it orders one session's
events, not different sessions'.

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

Local mail joins the run queue at the macrostep end, so a receiver never
takes mail from a macrostep that has not ended, whichever slot it runs in. Its outgoing list is scheduling state, not a host effect hold.
An abandoned macrostep frees that list, while effects the host already
executed remain executed.

## Two kinds of exhaustion

| | Who can free it | What happens | Does the chart see it? |
|---|---|---|---|
| **Rings:** c2h full when an action writes | the host, by draining | The slot parks before the action and runs it again once the record fits. Other slots go on; when none can, `ta_run` answers `TA_RUN_C2H_FULL` ([above](#a-full-c2h-pauses-the-slot)). | no |
| **Rings:** an action's record is larger than `maxRecordBytes` | nobody | The action fails as `error.execution.capacity.record`. | yes |
| **Before a macrostep:** c2h lacks the reserve | the host, by draining | No macrostep starts, and `ta_run` answers `TA_RUN_C2H_FULL` when nothing else can run. | no |
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
  `internal` or `record`, as the catalog's
  [`capacity_resource`](../protocol/generated/MESSAGES.md#capacity_resource-u8)
  enum lists them. A chart that doesn't care which one matches
  `error.execution.capacity`.
- **`_event.data` says where:** the action and the `<foreach>` iteration, as
  `ta_trace_position` gives them.
- **The chart decides what to do next:** retry later with a delayed send to
  itself, send less (ack-clocked sending), or give up and report.

Errors use compiler-sized mandatory internal storage, separate from the quota
for ordinary raises. Beyond that floor they use free ordinary entries; an
error that fits in neither is a runaway (below).

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

Internal events (`<raise>`, `done.state.*`, `error.*`) share one ordered
queue. It is not in the envelope pool. Its capacity has two parts:

- **the ordinary quota,** `internalQueueEvents` entries (default 256), for
  the chart's `<raise>`;
- **the mandatory floor,** sized by the compiler, for the events the
  interpreter must deliver: errors and `done.state` events.

A `<raise>` that finds the ordinary quota full fails as
`error.execution.capacity.internal`. That error is itself a mandatory
event. Mandatory events beyond the floor take free ordinary entries, and
one that fits in neither is a runaway
([below](#sizing-mandatory-storage)). The mechanics are in
[envelope.md](envelope.md#the-internal-queue-is-not-in-the-pool).

**Why not envelopes:**

- **Internal events never outlive their macrostep.** They need no admission,
  no deadline, no waiting list and no recycling, which is everything the
  pool provides.
- **They shouldn't compete with mail.** If they came from the pool, a full
  pool would stop a chart from raising its own events, and a chart raising
  in a loop would starve every other session's mail. Separate memory keeps a
  fault inside the chart that causes it.
- **One live queue.** One top-level interpreter runs at a time, so the
  system needs one live queue, in the running workspace, plus one per
  nested interpreter, sized once. A parked slot's pending entries wait in
  the park area. That replaces ADR 0064's queue lent to each actor and its
  cache.

### Sizing mandatory storage

**The compiler counts independent failure sites.** SCXML 4.9 stops only
the failed block. Two entry blocks can each fail in one microstep, and both
errors keep their queue order. A loop in one block stops at its first
error, so it needs one entry, not one per unexecuted iteration.

**The floor is one round of every phase.** The compiler records a
conservative maximum for each phase:

| Bound | Phase |
|---|---|
| `B` | bootstrap |
| `E` | one microstep's independently executed blocks and data initialization |
| `C` | one transition-selection pass |
| `V` | one invocation-startup pass |
| `F` | final-state completion data and stop or cancellation cleanup |

Mandatory storage has `B + E + C + V + F` entries, computed with checked
arithmetic. Generated error names and diagnostic positions also have
pre-sized storage, so reporting a failure never needs another fallible
allocation.

**It is allocated outside macrosteps.** Registration, spawn and load size
the storage, once per system in the running workspace and once per nested
interpreter workspace, before the system or workspace becomes usable. If
sizing or allocation fails, the operation is refused as a whole. Neither
the floor nor the ordinary quota grows during execution. A chart that reads
its queue as it goes never exceeds the floor.

**Beyond the floor, mandatory events use the ordinary quota.** They take
free entries of `internalQueueEvents`. Mandatory and ordinary entries keep
one FIFO order: the floor is reserved capacity, not priority. A chart that
raises a lot leaves less room for its own mandatory events, and that is the
same chart's fault.

**Overflow is a runaway.** A mandatory event that fits neither the floor
nor a free ordinary entry abandons the input with the chain limit's rules
([below](#runaways-the-chain-limit)). The completion says `runaway`, and
the `TA_TRACE_RUNAWAY` record's `runaway_cause` says `internal_queue`
instead of `chain_limit`.

**Why a runaway, and not a pause.** SCXML runs eventless transitions before
it takes the next internal event. So mandatory events accumulate only while
a chart keeps stepping without reading its queue. That is a runaway in all
but name, and it gets the runaway outcome. A pause cannot make room, because
only the session drains its own queue. c2h differs: the host drains it. The
rule is the same for both: pause where the host can free space, fail where
only the session can.

**Cost and price.** The cost is the floor plus 256 entries of about 48
bytes: about 12 KiB per system. A bound for the whole chain instead would
be `B + L·E + S·C + I·V + F` entries, linear in the chain limit; at the
default limit, a chart with three failure sites per microstep would need
about 14 MB (an illustration, not a measurement). The price: a chart whose
error storm would have ended with more pending mandatory events than the
free ordinary entries hold is abandoned instead of finishing.

**Why not a coalesced marker.** Replacing overflowing events with one
`error.execution.capacity.internal` would lose a `done.state` event, and
that changes control flow.

**Later optimisations.** For a chart whose internal transitions form no
cycle, the compiler can derive a larger exact floor, and a lower
`microstepLimit`. Neither changes the protocol.

## Runaways: the chain limit

A macrostep that never ends is the case core ADR 0075 exists for. Under
slots it holds one slot, not the system, but it still needs a limit:

- **memory:** without it, a guard that keeps failing would fill the
  internal queue; an overflow of mandatory events is itself a runaway
  ([above](#sizing-mandatory-storage));
- **the thread:** a loop takes its turn in every rotation, for ever;
- **visibility:** something has to say that the actor is broken, and its
  mail must not pile up on its deferred list for ever.

**The limit counts the whole chain.** `microstepLimit` counts, for one
input, from take until `c2h.input_completed`:

- every microstep;
- every selection pass, including one that enables nothing and only
  consumes an internal event;
- every invocation-startup pass.

Units of executable content do not count. A `<foreach>` over 10,000 items
is one microstep, however long it runs, and its array is a snapshot, so it
always ends; it holds one slot and rotates with the others.

Today's limit counts only committed microsteps in one macrostep. It misses
an eventless transition whose guard keeps throwing: each pass raises an
error that is consumed and ignored, so no microstep commits. It also misses
an `<invoke>` that fails and whose error re-enters its state: each
macrostep of that chain is short. Both are caught by the chain count. The
limit must be finite: 0 and `TA_MICROSTEP_LIMIT_NONE` are refused, for the
system and for a definition.

**At the limit, core ends the input, not the actor.** The check sits
between steps, where nothing is open (ADR 0075). Core applies ADR 0082's
abandon at that boundary:

- the configuration and data after the last completed microstep stay;
- the internal queue and the local mail the macrostep sent are freed and
  never delivered;
- effects already written to c2h (external sends, logs, invocations) have
  gone out and stay done;
- `c2h.macrostep_ended` and `c2h.input_completed` say `runaway`, and core
  writes the `TA_TRACE_RUNAWAY` telemetry record ADR 0075 defines, with the
  configuration and the transitions of the looping microstep, and its
  `runaway_cause` (`chain_limit`, or `internal_queue` for a mandatory
  overflow);
- the slot is free, and the first envelope of the session's deferred list
  returns to the run queue at the position its arrival gives it
  ([envelope.md](envelope.md#mail-for-an-active-session)).

It is like a `RecursionError` that only the event loop can catch: the
handler stops, the process lives on, and nothing it already changed or
sent is undone. The chart does not see it, as ADR 0075 decided: no
`error.*` event is raised, because the macrostep it would belong to has
just been stopped. The host sees it, and decides what to do about an actor
that runs away on every input, for example destroying it after a number of
runaways with `h2c.destroy`. That is application policy.

A chart whose legitimate work takes more steps than the system's limit
gets a higher `microstepLimit` for its definition, set ahead of time. With
slots, its long chains cost the rest of the system nothing.

**Quarantine is removed.** napi quarantines a runaway today (napi ADRs
0008 and 0025): it holds the actor where it stopped until an operator
calls `release()`. That kept one bad actor from taking the loop's turns,
which slots now do. Holding it would also let its mail fill the pool while
nobody looks, leave the host's promise for its input pending, and stop a
replay whose seed loops from ever being published. The implementation
therefore removes quarantine, `release()`, and the parent's
`error.communication.invoke.<id>.quarantined` event: a child whose input
ran away is not stuck, and a parent waiting for its `done.invoke` relies
on its own timeout, as for any child that does not finish. An operator who
wants to look at a live actor attaches the debugger
([debugging.md](debugging.md)).

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

**Why an error and not a pause:** this is about the data model's memory. A
full c2h ring pauses the slot instead, because the host frees it and
nothing has been done yet ([above](#a-full-c2h-pauses-the-slot)).

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
