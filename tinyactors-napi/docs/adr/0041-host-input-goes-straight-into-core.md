# ADR 0041: Host input goes straight into core

**Status:** Accepted and implemented (2026-10-05, ring-proto step 4.2e). Applies [core ADR 0099](../../../tinyactors-core/docs/adr/0099-pending-inputs-are-envelopes-in-one-pool-per-system.md) (pending inputs are envelopes in one pool per system) and its 4.2e notes to napi's API. Refines ADR 0005 (the per-target mailboxes are gone), ADR 0008 (nothing is quarantined), ADR 0020 (mail for a name waits in core), ADR 0025 (a quarantined child no longer exists), ADR 0026 (`mailbox()`, operator transitions) and ADR 0035 (`maxQueuedMessages`, `maxRetainedBytes`, `messageBudget`, `resumesPerTurn`). Kernel ADRs 0001, 0003 and 0004 carry the matching pointers.
**Date:** 2026-10-05

## Context

Until 4.2d the kernel held the application's posts in per-actor mailboxes
and handed them to core at the pace of `messageBudget`; chart mail and the
application's posts for the same actor waited in two places and were merged
by a rule (4.2d's "Ordering across sources"). Core now owns the one queue:
a pool of envelopes, a run queue, a waiting list for each address, and a
token for every host input (`ta_actor_deliver`). The kernel's scheduler
keeps only what the application needs from that queue: the `send()`
waiters, the retained-bytes estimate, the dead letters, and the limits.

## Decision

1. **`post()`, `send()`, `postNamed()` and `sendNamed()` admit into core at
   the call.** The kernel calls `ta_actor_deliver` with the actor ID, or with
   the `address` for a name that has no resident actor. The message waits in
   core's run queue or on the address's waiting list, ordered with chart mail
   for the same target: one queue, one order. Inside a macrostep (a data-model
   callback posts) the message is paid from that macrostep's claim and waits
   for its end, ordered with the macrostep's own mail. Anywhere else inside a
   core call, `post()` throws `reentrant` (ADR 0013). The event is borrowed:
   the caller keeps and releases its own references.
2. **Errors.** `queue-full` is core's `TA_ERR_FULL` (the pool is at its
   admission watermark, or a macrostep spent its claim), or one of the two
   application limits below. A `send()` rejects with the same codes instead
   of throwing. `busy` is an operator transition that cannot be taken now
   (point 8).
3. **`maxQueuedMessages` and `maxRetainedBytes` stay, and are enforced
   without a mailbox.** The kernel counts what it needs from core and from
   itself. `maxQueuedMessages` is checked against core's gauge of host inputs
   admitted and not finished (`ta_counters.host_inputs`) plus the verdicts
   waiting to be reported; `maxRetainedBytes` against the kernel's own sum of
   the estimates of the inputs it follows (a table indexed by the token's
   slot, `ta_input_slot`, so a steady state allocates nothing). Each counts
   the application's posts and sends only; mail between actors and error
   reports are core's and carry no estimate (kernel ADR 0003). `maxWaiting`
   of a loader family counts what waits on the address's list, so it now
   counts chart mail too.
4. **`send()` waiters resolve from core's notices.** A `COMPLETED` notice
   settles the waiter with `stable` or `done` (and `consumed`); a `REFUSED`
   notice settles it as a dead letter with the refusal's reason; `abandoned`
   and `runaway` settle by the input's status. Aborting, a timeout, and
   `close()` withdraw the input with `ta_input_withdraw` when core has not
   taken it; one already taken is not undone.
5. **Dead letters come from `REFUSED` notices.** The reasons are core's
   refusal reasons mapped to `not-found`, `destroyed`, `done`, `invalid` and
   the loader's outcomes; `waiting-full` and the answers core cannot give (a
   stale or unknown ID, the kernel's own limit on an address's list) are
   verdicts the kernel makes at the call and reports at the start of the next
   turn. Verdicts are reported before refusals within a turn (the order used
   to follow the mailboxes). A dead letter carries the event, its data, the
   trace parent and, for a name, the address, all from the notice.
6. **Nothing is quarantined.** A macrostep that reaches the microstep limit is
   abandoned by core and the actor stands as it was after its last microstep
   (core ADR 0099 section 7); a malformed event is refused at the call. There
   is no fault that takes an actor out of scheduling until 4.5. So:
   `scheduling()` and `inspect().scheduling` never answer `"quarantined"`;
   the metric `tinyactors.actors.quarantined` reads 0; the `FaultRecord` has
   only `runaway`; no `error.communication.invoke.<id>.quarantined` event is
   made; and `release()` stays declared, marked `// status: partial: nothing
   is quarantined until 4.5`, and throws `invalid` for every actor.
7. **`mailbox(actor)` lists core's inputs for the actor** (`ta_actor_pending`):
   host posts and mail from other actors, oldest first, with `awaited` from the
   kernel's follow table and `scope` from the origin. The `transition` field
   is gone with the queued transition. Cost: the run queue up to the
   `limit`-th match, no allocation. `scheduling()` is derived from core:
   `"none"` for an unknown ID, `"runnable"` inside a macrostep, `"ready"`
   while inputs wait, `"idle"` otherwise. The metrics `scheduler.runnable`
   and `scheduler.ready` read core's queue (`ready` is the host inputs and
   verdicts waiting; `runnable` is 0).
8. **An operator transition does not queue.** `post(…, { transition })` and
   the `send` form call `ta_actor_transition`: it throws `busy` unless the
   actor is idle with nothing waiting for it, and `invalid` when the source
   state is not active or the chart is another revision; neither is a dead
   letter and neither quarantines. (It used to queue behind the mail and be
   refused later.)
9. **`messageBudget` bounds the turn** in inputs: the inputs core finishes,
   plus the verdicts reported. It is a soft bound checked between `ta_run`
   calls; with a trace sink that polls `stop`, core is asked for one input per
   call. `resumesPerTurn` is deleted: a turn no longer resumes actors, core's
   run queue does.
10. **Smaller changes users can see.**
    - A malformed event (a name core never issued) throws `invalid` at the
      call, changes nothing, and does not quarantine the actor.
    - An actor holds at most 65,535 queued inputs; one more answers
      `queue-full`.
    - The default burst bound is core's admission watermark (about 2,048
      pending inputs at the default pool); `scale.js` posts in chunks.
    - `destroy()` settles the destroyed actor's queued sends at the next turn,
      as `destroyed` dead letters.
    - A load outcome of `done` for a host post is the dead-letter reason
      `not-found`.
    - A failed invocation start reports `error.communication.invoke.<id>.failed`
      as an ordinary input to the parent, so it never answers `busy` inside a
      macrostep.

The event block stays (4.4 removes it). The `ta_input_watch` hook stays: core
now marks every input it takes except an operator's transition.

## Consequences

- One queue, one order, for every source of input; the kernel holds no
  mailboxes, no ready or runnable queues, no quarantine and no envelope store.
- The kernel's per-actor table is gone with them: spawning costs the
  scheduler nothing, and the `scheduling` memory category holds only the
  `send()` waiters, the follow table and the verdicts.
- Back-pressure is visible at the call, as `queue-full`, not later as a
  growing mailbox. An application that posts a burst larger than the pool's
  watermark must pace itself or raise `poolEnvelopes`.
- A dead-letter hook sees verdicts before refusals within one turn.

## Superseded decisions

- **ADR 0005, points 1 to 8 and 10:** per-target mailboxes, the ready queue,
  the envelope free list; **point 9:** the limits count the mailbox.
- **ADR 0008:** quarantine and `release()`; **ADR 0025:** the quarantined
  child's `.quarantined` report.
- **ADR 0026:** a transition queued behind mail, `QueuedMessage.transition`.
- **ADR 0035:** `resumesPerTurn`, and the unit of `messageBudget`.
- **`index.d.ts`:** `maxQueuedMessages`, `messageBudget`, `release()`,
  `mailbox()`, the operator `transition`, `QueuedMessage`,
  `ActorInspection.scheduling` and `FaultRecord`.
