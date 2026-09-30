# ADR 0095: A checkpoint restores the sends the router refused

**Status:** Accepted and implemented (2026-09-30); see the implementation notes. Amends ADR 0091 section 1 ("a send the router already refused once is not in it") and ADR 0094 section 3's restore ("a submitted one is exported but not restored"). Keeps ADR 0091 section 1's rule that an invocation carries no values, for the reason below. Builds on kernel ADR 0002, whose log writes `fired` only on the attempt that takes a send. Superseded in part by [kernel ADR 0003](../../../tinyactors-host/docs/adr/0003-a-full-mailbox-overflows-instead-of-refusing-mail-between-actors.md): a full mailbox no longer makes a submitted send; its machinery is removed there (implementation notes, 2026-09-30), since only out of memory makes one now.
**Date:** 2026-09-30

## Context

A send the router answers RETRY for, such as one to a full mailbox, has
been sent (ADR 0054): core keeps it in the deadline index, due, marked
submitted, and offers it again at every dispatch until the router takes
it. `<cancel>` no longer reaches it, and it outlives its sender as an
orphan.

ADR 0091 section 1 left such a send out of a checkpoint: "its `fired`
record is in the log, and it belongs to its receiver". ADR 0094 section 3
kept that rule when a checkpoint became structured state: a submitted send
is exported but not restored.

The reason no longer holds. Under the kernel's log (kernel ADR 0002), the
router writes `fired` before it posts and takes the record back when the
post answers RETRY. A send retried after a RETRY writes one `fired`, on
the attempt that takes it. An immediate send writes none: its receiver's
`accepted` records it. So while a send waits, submitted, nothing durable
says it exists:
- the sender's log has no `fired` for it;
- the receiver's log has no `accepted` for it;
- a checkpoint taken then leaves it out.

After a crash, a restore from that checkpoint loses the send. The owner
decided that this is a bug to fix, not an at-most-once window to accept
(`PLAN.md`, decisions of 2026-09-30).

## Decision

### 1. A restored state holds its submitted sends

`ta_actor_prepare()` with a state holds every send in it, submitted ones
included. A submitted send keeps its mark:
- **no clock move drops it.** It waits for the router, not for time: it
  was due before the state was taken, so ADR 0089 section 2's rule ("time
  reached it, so it fired") would drop it at once;
- **`<cancel>` and `ta_actor_withdraw_send()` do not reach it,** as for a
  live actor. A withdrawal of a replaying actor whose only held sends with
  that ID are submitted answers `TA_ERR_DONE`, as it does live;
- **its envelope names the actor** (`record.self`, the named address it
  was sent from) and no invocation, because a prepared actor has no
  parent;
- **at publish it is scheduled, due already,** and the next dispatch
  offers it to the router again, as a submitted send.

`ta_actor_export_state()` is unchanged: it already exported submitted
sends, and the kernel already wrote them (`TAH_WAL_SEND_SUBMITTED`).

### 2. The log says when a held submitted send left

A checkpoint may be followed by records that show the send left the actor
after the state was taken. The replay drops it there:

```c
TA_API ta_result ta_actor_drop_submitted(ta_system *system, ta_actor_id actor, ta_send_id send);
```

- **`fired` for its send ID:** the router took it, or refused it with an
  error, which consumed it too. The first held submitted send with that ID,
  in the order they were due, is dropped: the dispatch offers sends in that
  order, so one `fired` took the first one.
- **`evicted`:** `send` 0 drops every held submitted send. An eviction
  leaves them to the process that evicted the actor, which delivers them as
  orphans (ADR 0054). Restoring them too would deliver them twice.
- **`TA_ERR_NOT_FOUND`** when none is held. Most `fired` records name
  none: a delayed send taken at its first offer, or an immediate send the
  replay dropped. The kernel ignores it.
- **`TA_ERR_INVALID`** for an actor that is not replaying.

The kernel's replay calls it at each `fired` and `evicted` record. A
`fired` record must therefore carry its send ID. The kernel answers RETRY
when it cannot render the ID's text, and the send stays due.

### 3. Invocations still carry no values

ADR 0091 section 1 left an invocation's request values out of a
checkpoint. That stays. A restored invocation is never passed to its
invoker again (ADR 0089 section 3): it is active in core without an
invoker, and it is retired as lost at publish or matched by the service's
later events. Nothing reads its values.

## Consequences

- **A send that waits at a checkpoint is delivered after a crash,** once:
  at publish when the log has no `fired` for it, and not at all when the
  log shows it was taken. What a crash can still repeat is the window of
  ADR 0089 section 2: a `fired` written and not yet committed.
- **Windows that remain:**
  - **an eviction, then a crash before the orphan is delivered:** the
    replay drops the send at `evicted`, and the orphan died with the
    process. At most once, as before.
  - **an orphan delivered after its address was loaded again** writes its
    `fired` into the new actor's log, because the router finds the sender
    by its address. If that actor also holds a submitted send with the
    same send ID, restored from a checkpoint, a later replay drops that one
    instead. It needs a literal send ID reused while an earlier send with
    it still waits for the same full receiver.
  - **A replay from `spawned`, with no checkpoint, still loses such a
    send.** The replay drops an immediate send ("sent is what happened",
    ADR 0089 section 3), and drops a delayed send once the clock passes its
    due time. Neither knows that the router refused it. This ADR does not
    change that: a record of the refusal would be needed. A replay that
    starts from a checkpoint taken while the send waited does not lose it.
- **One more public function,** which only the kernel's replay calls.
  napi lists it as plumbing.
- **Costs:** nothing per actor, per event or per pending send changes. A
  restored state holds one more held send per submitted send, as it does
  for any other pending send, and the checkpoint layout is unchanged.

## Considered and rejected

- **Accept at-most-once for submitted sends,** and document it. The owner
  decided against it.
- **The kernel holds the submitted sends and posts them after publish.**
  It would repeat core's retry, deadline order and orphan handling in the
  kernel.
- **Reuse `ta_actor_withdraw_send()` for `fired`.** A withdrawal never
  reaches a submitted send, removes every send with the ID, and counts and
  traces a withdrawal that did not happen.
- **Restore a submitted send as an unsubmitted one due at once.** A clock
  move would drop it, and `<cancel>` would reach a send that live could
  not.

## Implementation notes

(2026-09-30)

- **Core (`src/actor.c`):**
  - the state check validates submitted sends like the others;
  - `ta_actor_state_restore()` holds them with their mark and the actor's
    `record.self` as origin;
  - `ta_actor_move_clock()` and publish's count of held sends keep
    submitted ones;
  - `ta_actor_cancel_held_send()` (`<cancel>`, withdrawal) skips them, and
    a withdrawal that finds only submitted ones answers `TA_ERR_DONE`;
  - `ta_actor_drop_submitted()` is new.
- **Kernel:**
  - `tah_replay_read_checkpoint()` needed no change: it already read
    `TAH_WAL_SEND_SUBMITTED` into the state it hands to prepare;
  - `tah_replay_run()` calls `ta_actor_drop_submitted()` at `fired` (by
    its send ID) and at `evicted` (every one);
  - `tah_inputs_fired()` answers out of memory, and so RETRY, when it
    cannot render the send ID.
- **napi:** `bindings.rb` lists `ta_actor_drop_submitted` as plumbing,
  and `src/gen/ta_napi.{c,h}` are regenerated for the header's comments.
  Nothing visible to napi users changes, except that a restore from a
  checkpoint now delivers a send that waited when it was taken.
- **Tests:**
  - core `tests/replay_test.c`, `test_export_restores_submitted`:
    exported and restored, the send survives a clock move past its due
    time and a withdrawal (`TA_ERR_DONE`), is published as submitted and
    routed once; `ta_actor_drop_submitted()` by ID and with 0, its result
    codes, and unsubmitted held sends left alone.
  - kernel `tests/replay_test.c`, with sink/b's mailbox full at one
    message: a checkpoint written while the send waits holds it
    (`TAH_WAL_SEND_SUBMITTED`). After a crash, a restore from it in pieces
    delivers it once, with a `fired` that continues the log's numbers. A
    `fired` committed after the checkpoint drops it: nothing is sent again.
    An eviction drops it, whether the orphan fires before or after the
    restore in the same process: delivered once.
