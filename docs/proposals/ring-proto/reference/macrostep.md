# The macrostep: invariants, the fence, and running out

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

This document says what a macrostep may and may not do under the ring
protocol: when one starts, how it ends, what the host may do with its
records, and what happens when something runs out. The rings are in
[ring.md](ring.md), and the envelopes it takes in [envelope.md](envelope.md).

## The invariants

1. **A macrostep never allocates in core.** Core allocates in three places
   only:
   - when a system is created (rings, pool, scratch, the continuation);
   - when a definition is registered (its compiled form and its atoms);
   - when a session is created or loaded (its fixed-size record, sized by its
     definition).

   Growing the pool up to its bound happens between macrosteps
   ([envelope.md](envelope.md#the-pool)). The data model allocates as it
   likes: that is the data model's memory, not core's
   ([data-model.md](data-model.md)).
2. **All memory is bounded by parameters.** Every queue, ring and text
   buffer has a size set when the system is created: `poolEnvelopes`,
   `nameAreaBytes`, `h2cBytes`, `c2hBytes`, `telBytes`, `maxRecordBytes`
   ([parameters.md](parameters.md)).
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
2. **Check the c2h reserve.** Unless c2h has room for a
   [`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput), a
   [`c2h.fence`](../protocol/generated/MESSAGES.md#c2hfence) and one record
   of `maxRecordBytes`, core takes nothing, and `ta_run` returns
   ([ring.md](ring.md#the-c2h-reserve)).
3. **Take** the envelope. For a recorded session, write `c2h.input`.
4. **Run** microsteps until the session is stable or final. Effects go to
   c2h, mail and timers to the pool, internal events to the internal queue.
5. **Fence.** Write `c2h.fence`. The macrostep's mail becomes runnable, and
   its taken envelope is freed.

**No session is busy when its envelope comes up**, because nothing else is
in the middle of a macrostep. That removes `TA_ERR_BUSY` (core ADR 0055),
the per-actor mailboxes, and the host's scheduler.

**Why not interleave other sessions while one is paused?** Earlier
iterations of this design considered it. Interleaving does not prevent
deadlock: that is done by refusing sends that don't fit, which works
without it. And it would need one continuation for every paused session.
Run to completion needs **one continuation per system**.

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
- **A full ring never pauses a run.** A record that doesn't fit makes its
  action fail instead (below). An earlier iteration of this design paused on
  a full c2h. That made the host hold records it couldn't refuse.

## The fence

A macrostep's records reach c2h in this order:

1. its `c2h.input`, if the session is recorded;
2. its effects: [`c2h.send`](../protocol/generated/MESSAGES.md#c2hsend),
   [`c2h.invoke`](../protocol/generated/MESSAGES.md#c2hinvoke),
   [`c2h.invoke_cancel`](../protocol/generated/MESSAGES.md#c2hinvoke_cancel),
   [`c2h.invoke_forward`](../protocol/generated/MESSAGES.md#c2hinvoke_forward),
   [`c2h.log`](../protocol/generated/MESSAGES.md#c2hlog);
3. its `c2h.fence`.

The fence is the only transaction core offers.

| | Before the fence | After the fence |
|---|---|---|
| The host may | read the records and copy them into its own memory, releasing ring space | everything: release effects to I/O processors and invokers, persist, reply |
| The host must not | act on any effect: send, start an invocation, reply over HTTP | – |
| A recorded session | – | persist its `c2h.input` first, then release its effects ([recording-and-replay.md](recording-and-replay.md)) |
| Local mail | is on the macrostep's outgoing list, on no queue | joins the run queue, or a waiting list if its target isn't resident |

Why this matters: a crash before the fence loses the macrostep completely,
input and effects alike. A crash after the input is persisted but before the
effects are released loses only effects, and the replayed session believes
it performed them. Either way, **what a session remembers includes
everything it did**. The other order would be a session that did something
it doesn't remember. The chart's own retries handle effects that were lost.

**What the host holds is its own choice.** With a budget, `ta_run` can
return in the middle of a macrostep, and the host can then:

- **copy and release.** The c2h space is freed. The host holds the records
  until the fence, in its own memory. This is cheap in Node; in an embedded
  host it may not be.
- **leave them in the ring.** c2h fills up, and the macrostep's next effect
  fails as `error.execution.capacity`. So `c2hBytes` bounds how much one
  macrostep can do, and the chart is told when it tries to do more.

Neither option is a queue that grows without anyone deciding.

## Two kinds of exhaustion

| | Who can free it | What happens | Does the chart see it? |
|---|---|---|---|
| **Rings:** c2h full when an action writes | the host, by draining | The action fails as `error.execution.capacity.c2h`. The run doesn't wait. | yes |
| **Before a macrostep:** c2h lacks the reserve | the host, by draining | No macrostep starts, and `ta_run` answers `TA_RUN_C2H_FULL`. | no |
| **h2c full** | core, by admitting | The host refuses at its edge | no; the external client does |
| **The pool or name area** when an action needs an envelope or text | the receivers, by running | The action fails as `error.execution.capacity.pool` or `.names` | yes |
| **Admission:** pool above `admissionWatermark` | the receivers, by running | Core admits nothing from h2c | no; the external client does |
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

The error can always be raised, because a slot in the fixed internal queue
is kept free for it ([envelope.md](envelope.md#the-internal-queue-is-not-in-the-pool)).

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
| a second `<raise>` or error in one microstep (the reservation held one event) | a slot in the fixed internal queue. A `<raise>` that finds none fails as `error.execution.capacity.internal`; core's own errors use reserved slots ([envelope.md](envelope.md#the-internal-queue-is-not-in-the-pool)). |
| a delayed `<send>` growing its owner's storage (ADR 0054) | an envelope with a due time ([envelope.md](envelope.md#delayed-sends-envelopes-with-a-due-time)) |
| interning an `eventexpr`, `typeexpr` or target | looked up, never interned ([names.md](names.md)) |
| scratch for `<param>`s | fixed scratch, sized at creation (ADR 0092) |
| `done.state` events and a top-level final state's completion | internal queue slots reserved before each microstep, up to the definition's static bound |
| recording an invocation | a slot in the session's record, sized by the definition's `<invoke>` count when the session is created |
| holding a send or a `<log>` for an unpublished actor | a replaying session writes no effects. Its timers are envelopes ([recording-and-replay.md](recording-and-replay.md)). |

**Starting a local child chart** (`<invoke type="scxml">`) creates a session,
which allocates. SCXML starts invocations at the end of the macrostep, so
core creates the child after the fence, outside the macrostep. Before the
fence, core takes one envelope for each invocation it will start; if the pool
has none, the `<invoke>` fails as `error.execution.capacity.pool`. After the
fence, a child that can't be created turns its envelope into
`error.communication.invoke.<id>.<reason>` for the parent; a child that
starts frees it.

So core no longer stops a run because it ran out of memory. `TA_ERR_NOMEM`
from `ta_run` can come only from growing the pool between macrosteps. It
changes nothing, and the host may call again later.

**A data model or invoker that runs out of its own memory** (a JavaScript
heap, say) answers `TA_OP_ERROR`, and the chart sees `error.execution`, as
for any other failed operation ([data-model.md](data-model.md)). `TA_OP_NOMEM`
leaves `ta_op_status`, and with it the last path that stops a run where it
stands: a shortage anywhere becomes a refusal the chart sees. The price is
that a passing memory shortage in the host becomes chart behaviour, which is
what charts are for ([migration.md](migration.md)).

The out-of-memory sweeps in the test suites (fail every allocation in turn)
become much smaller for core. A macrostep has no allocation to fail, so the
sweep covers creation, registration, spawn and load, and pool growth. The
sweep over replay in the kernel (`replay_test` with `TAH_FULL=1`) goes away
along with the kernel ([migration.md](migration.md)).
