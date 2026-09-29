# ADR 0084: An operator can withdraw one pending delayed send

**Status:** Accepted and implemented (2026-09-26). Refines ADR 0012 (cancellation from outside the chart, still scoped to the owning session); applies ADR 0038, ADR 0048, ADR 0054, ADR 0067 and ADR 0070. Section 7 (withdrawal by due time) is replaced by ADR 0089: a replaying actor's held sends are dropped as its clock moves, and withdrawal also reaches them.
**Date:** 2026-09-26

## Context

A timer that should not fire is a common way for an actor to wedge: a
`<send delay="30d">` armed with the wrong data, a retry loop that keeps
re-arming itself, a reminder for an order that was refunded by hand. An
operator can see it: `ta_actor_inspection.delayed_sends` lists an actor's
pending sends with their `send_id`, due time, event, target and data
(ADR 0054). An operator cannot remove it. ADR 0012 scopes cancellation to
the session that created the send, and the only way in is a `<cancel>`
the chart itself runs. A debugger's `cancel <send ID>` console command
(`docs/proposals/debug-adapter.md`) has nothing to call.

Core has the mechanism. ADR 0054 stores an actor's pending sends in its
own sorted array and removes a cancelled or withdrawn send in O(k + log A)
for k sends of the owner and A owners with sends; `ta_timer_withdraw()`
does it for an invoked child's completion (`src/invoke.c`). What is
missing is a public entry point and the rules around it: which ID it
takes, what it does to a send the router already refused once, whether
it may run while the owner is inside a macrostep, what it answers, what
it records, and how it interacts with session images.

## Decision

### 1. One function, naming the owner and the send

```c
TA_API ta_result ta_actor_withdraw_send(ta_system *system, ta_actor_id actor, ta_send_id send);
```

It does to the actor's pending sends what a `<cancel>` of `send` run by
that actor does: every pending send of the actor with that ID that the
router has not yet been offered is removed and its values released.

**It names the owner, so ADR 0012's scope holds.** Only the named
actor's own sends are candidates. A send with the same ID owned by
another actor is not touched, and naming the wrong owner answers
`TA_ERR_NOT_FOUND`. What changes is who may cancel within that scope: the
host, as well as the chart.

**The ID is `ta_send_id`**, the number inspection reports as
`delayed_sends[i].send_id`, so an operator withdraws exactly what they
read, with no text round trip. A host that holds the text a chart sees
(`#_send.3`, or a literal `id`) turns it into the same number with
`ta_send_id_parse()` (ADR 0048), and `ta_send_id_format()` renders the
number for display. Taking text instead would put a parse, and its own
failure codes, into every call from an inspector that already has the
number.

**A repeated literal ID removes every pending send with it**, as
`<cancel>` does (SCXML 6.3 names a send by ID, and core's `<cancel>`
removes all matches). A generated ID (`#_send.<n>`) is unique within a
session, so it names one send. An operator who needs to single out one of
several sends sharing a literal ID has no way to do so yet; withdrawal by
due time (section 6) is the natural complement.

### 2. A submitted send is not withdrawn

ADR 0054 point 3: a send the router refused once is `submitted`. It has
been sent, it waits only to be offered again, it outlives its session,
and it belongs to its receiver. `<cancel>` does not remove it, and
neither does withdrawal. When the only pending sends of the actor with
that ID are submitted, the answer is `TA_ERR_DONE`: the send is done
being the sender's, and asking again will never succeed. When both kinds
share an ID, the unsubmitted ones are withdrawn and the answer is
`TA_OK`.

An invoked child's completion is kept the same way: it is stored
submitted (ADR 0054), so it is never withdrawn by this function.

### 3. It may run between macrosteps or while one is paused

Withdrawal is allowed whenever the actor is resident, including while a
macrostep is paused (ADR 0009, ADR 0061) or stopped (ADR 0070). Nothing
in a paused macrostep depends on a pending send being there:

- **The internal queue** holds events, never delayed sends. A delayed send
  lives in the owner's timer array from the unit that made it, whether or
  not the macrostep has ended, so withdrawing it touches nothing the
  macrostep will read.
- **A `<cancel>` later in the same macrostep** of a send the operator
  withdrew finds nothing, which SCXML already allows: cancelling a send
  that is gone has no effect. Its `CANCELLED` record is still written, as
  for any `<cancel>`.
- **A `<send>` the macrostep has not run yet** does not exist: withdrawing
  its ID answers `TA_ERR_NOT_FOUND`, and the send is made later as usual.
- **Running out of memory** (ADR 0070) cannot leave a half-made send: the
  unit that makes a send reserves its room first and stores it only when
  it commits.

Refusing with `TA_ERR_BUSY`, as `ta_actor_deliver()` does, would protect
nothing and would force a debugger stopped inside a macrostep to run it to
its end before removing a timer, which is the moment the operator is
looking at it. Delivery refuses because an actor holds one external event;
withdrawal adds nothing to hold.

A host must not call it for the actor that is running from inside one of
that actor's callbacks, as for every other function (ADR 0067 section 1).

### 4. Result codes

| Result | When |
|---|---|
| `TA_OK` | at least one pending, unsubmitted send of the actor with that ID was withdrawn |
| `TA_ERR_INVALID` | `send` is 0, which names no send |
| `TA_ERR_NOT_FOUND` | no resident actor has that ID (never spawned, destroyed, unloaded, or a null system), or it holds no pending send with that ID: never made, already dispatched, cancelled or withdrawn. The sends an unpublished replacement holds until activation are not pending sends yet (ADR 0037) and are not found either |
| `TA_ERR_DONE` | the actor's only pending sends with that ID are submitted (section 2) |

It allocates nothing, so it never answers `TA_ERR_NOMEM`, and a failed
call changes nothing.

### 5. A `WITHDRAWN` record, outside any macrostep

`ta_trace_kind` gains `TA_TRACE_WITHDRAWN`, carrying `send`. It is
lifecycle-like (ADR 0067 section 4, napi ADR 0016): an operator's
withdrawal is a host operation, not a step of the chart, so it has no
macrostep, no position and no trace context. Its key is `(session, 0, n)`,
the session's next lifecycle key, which moves whether or not a sink wants
the record, and the sink's mask is tested as for lifecycle records. It is
written even while a macrostep is paused, between that macrostep's
records; its key says it is not one of them.

Reusing `TA_TRACE_CANCELLED` was the alternative. It would have needed
the same lifecycle key to stay out of the paused macrostep, and every
sink that treats `CANCELLED` as an effect of the macrostep it arrives in
(napi's OTLP exporter makes it a span event of the open macrostep's span)
would have attached an operator's action to a step of the chart. A kind
of its own also lets an audit trail tell an operator's withdrawal from a
chart's `<cancel>` without reading the key. The new kind is appended
last, after ADR 0082's `ABANDONED`, which follows ADR 0080's definition
kinds, so no existing value moves.

The counter `sends_cancelled` (ADR 0072) counts withdrawals too, once per
call that withdrew something, as it counts once per `<cancel>` that did:
both remove pending sends before they fire, and a dashboard that watches
cancellations should see an operator's. The record, not the counter,
tells the two apart.

### 6. Session images

A withdrawal changes the actor, so it advances the session epoch
(`ta_actor_touch()`, ADR 0014, ADR 0038):

- An image captured **after** the withdrawal does not hold the send; it
  was removed from the array the capture copies.
- An image captured **before** it is stale. `ta_actor_unload()` refuses it
  with `TA_ERR_STALE`, so a host cannot put the actor to rest with an
  image that would bring the withdrawn send back on load. The host
  captures again.
- An image the host stored earlier and restores on its own (a checkpoint
  read back after a crash) is the host's durability, outside core's view,
  and does hold the send. A host that checkpoints must checkpoint after
  an operator's withdrawal, as after any other change.

### 7. Next to it, later: withdrawal by due time

Replay-based persistence (`docs/proposals/replay-persistence.md`) needs to
withdraw an actor's pending sends due at or before a given time when a
replay ends. That is a second entry point beside this one, with its own
rule for submitted sends; this ADR does not build it. When it is decided,
it belongs next to `ta_actor_withdraw_send()` and should reuse the same
record kind and scope.

## Consequences

- An operator or a debugger removes a wedging timer by actor and send ID,
  from inspection alone, while the actor idles or while it is paused.
- The public API gains one function and one trace kind. Both hosts bind
  the function from the generator; napi's two kind-name tables and its
  OTLP exporter learn the kind (a zero-length lifecycle span named
  `scxml.withdrawn`), as does the test host's trace renderer.
- Cost: O(k + log A) per call (ADR 0054), no allocation, no new stored
  state. Nothing grows per actor, per event, per pending send or in an
  image; the image format is unchanged.
- ADR 0012's sentence "cancellation is scoped to the actor/session that
  created the delayed send" still holds for what can be cancelled; the
  host may now do it.

## Implementation notes

- `ta_actor_withdraw_send()` is in `src/actor.c`. It removes with
  `ta_timer_cancel()`, the removal a `<cancel>` uses, and looks at the
  actor's array only to tell `TA_ERR_DONE` from `TA_ERR_NOT_FOUND`.
- **Tests.** `tests/delayed_send_test.c` withdraws the earliest of three
  sends and checks inspection, the global deadline, dispatch, the
  `WITHDRAWN` record and its key, the counter, and that nothing
  allocated; checks `TA_ERR_NOT_FOUND` for a second withdrawal, another
  owner's send, a destroyed actor and a stale ID, and `TA_ERR_INVALID`
  for ID 0; withdraws a repeated literal ID; answers `TA_ERR_DONE` for a
  submitted send that is later delivered; withdraws a send made earlier
  in a paused macrostep and runs the block's own `<cancel>` of it, with
  the records in order; and shows that an image captured before a
  withdrawal cannot unload the actor while one captured after it loads
  without the send. `delayed_sends.rb` withdraws by an ID read from
  inspection in the test host, checks another owner's send of the same
  ID stays and the trace line, and that a refused send answers
  `err_done` and is still delivered.
- No out-of-memory sweep: the function allocates nothing, which the core
  test checks with a counting allocator.
