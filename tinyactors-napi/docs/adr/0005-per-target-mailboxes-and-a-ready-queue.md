# ADR 0005: Per-target mailboxes and a ready queue

**Status:** Accepted; scheduling table and envelope free list added with ADR 0014;
delivery results refined when step 6 was built
**Date:** 2026-09-23

## Context

The host owns external queues (tinyactors-core ADR 0011 and ADR 0029). Core
accepts one external event at a time, only when the actor is between
macrosteps, and answers `TA_ERR_BUSY` otherwise.

The first addon used a single global FIFO. It inspected each message's
target, and re-queued the message at the tail when the target was busy.
That design has several faults:
- **Quadratic cost:** each re-queue compacted the whole queue with
  `memmove`.
- **Two inspections per message:** this is one core call more than needed,
  and before tinyactors-core ADR 0054 each one scanned every delayed send.
- **Reordering:** when a turn stopped at its message budget, a re-queued
  message could end up behind a later message to the same target.
- **Head-of-line blocking:** a busy actor made the whole queue be rescanned.

## Decision

1. **A scheduling table indexed by slot.** The addon keeps one entry for
   each slot of core's actor table (ADR 0002). The table grows in fixed
   pages from the system allocator (ADR 0014) and is never copied. An
   entry is 16 bytes, about 64 MB at four million actors, and holds:
   - the generation of the actor the addon knows in that slot (0 when
     none);
   - the scheduling state: `idle`, `ready`, `runnable` or `quarantined`,
     and a back-off flag (ADR 0008);
   - the indices of the first and last envelope of the actor's mailbox.

   This is host-owned scheduling state, not a copy of a core fact
   (ADR 0001). Core does not know about the host's queues. Without this
   state the addon cannot tell whether an actor is running, and it would
   have to inspect it before every delivery (tinyactors-core ADR 0055).
2. **The table follows the actor's lifecycle.** The post-call hooks of
   ADR 0013 fill an entry for every new actor and put it on the runnable
   queue for bootstrap. The destroy and unload hooks, and the reclaiming of
   finished actors (ADR 0012), dead-letter its mailbox and clear the entry.
3. **Mailboxes are lists of reused envelopes.** Envelopes come from the
   system's envelope free list (ADR 0014). A mailbox is the list threaded
   through them from the entry's first index to its last index. An empty mailbox costs
   nothing beyond the entry. Enqueueing:
   - checks the target's generation against the entry. A mismatch is a
     dead letter with reason `not-found`, recorded at once, so mail for a
     stale ID never joins the mailbox of the slot's next actor;
   - appends the envelope;
   - moves an `idle` actor to `ready` and appends it to the ready queue.
     In any other state the envelope only waits.
4. **Ready queue.** A FIFO of the slots in state `ready`. Each actor is in
   it at most once, because the state says so.
5. **Runnable queue.** A FIFO of the slots in state `runnable`: bootstrap,
   a delivered event, or a macrostep paused on budget (`TA_RUN_PAUSED`). An
   actor stays `runnable` while its macrostep runs. Mail that arrives
   during the macrostep, including an external send to itself, only waits.
   When `ta_actor_resume()` returns `TA_RUN_STABLE`, the actor becomes
   `ready` if its mailbox is not empty and `idle` otherwise.
6. **Delivery trusts core's answer (tinyactors-core ADR 0055).** The
   addon does not inspect before delivering. It calls `ta_actor_deliver()`
   with the head envelope of a `ready` actor and acts on the result:
   - `TA_OK`: pop the envelope and make the actor `runnable`;
   - `TA_ERR_NOMEM`: keep the envelope, and back off for one turn
     (ADR 0008);
   - `TA_ERR_DONE`: dead-letter the mailbox (ADR 0008) and mark the entry
     finished. With `finished: "retain"` the actor stays resident, and
     later mail for it is a dead letter with reason `done` at once;
   - `TA_ERR_NOT_FOUND`: dead-letter the mailbox and clear the entry;
   - `TA_ERR_BUSY`: with the state above, this cannot happen. It is a host
     bug: the actor is quarantined and the envelope kept;
   - anything else (for example `TA_ERR_INVALID` for a malformed event): a
     fault (ADR 0008). The actor is quarantined and keeps its mailbox, but
     the envelope core refused becomes a dead letter with reason
     `refused`, because delivering it again after `release()` would only
     fault again.
7. **Order.** Messages to one target are delivered in the order they were
   accepted. Nothing is promised about the relative order of different
   targets beyond what the ready queue's FIFO order gives.
8. **Envelopes are small.** An envelope holds the target's generation, the
   event name ID, the send ID, the origin, the invoke ID, the index of the
   next envelope, and a value box only when data is present (ADR 0011). A
   named origin is a retained `ta_address` handle (tinyactors-core ADR
   0065, napi ADR 0006), not a copy. A post without data allocates nothing after warm-up (ADR 0014).
9. **Backpressure.** Limits on the total number of queued envelopes and on
   retained data bytes are checked when a message is accepted: at
   `post`, and in the router (ADR 0006), which answers
   `TA_EXTERNAL_EVENT_RETRY` when full. A message that has been accepted is
   never dropped because of a limit.
10. **Dead letters are bounded per turn.** Dropping a mailbox moves its
    envelopes to a dead-letter list at once, which is O(1). The pump then
    reports at most `messageBudget` of them to the `deadLetter` hook in
    each turn, so one dead actor with a deep mailbox cannot make one turn
    long.

## Consequences

- Every message costs O(1): an array index, a list append, one core
  deliver and one resume. No step hashes, scans a queue, or scans the
  population, and no table is rehashed or copied as it grows.
- A hot actor does not slow down other actors. It only takes more turns
  itself.
- Each slot costs 16 bytes, including slots of idle actors. That is a small
  part of a core actor, and it buys a scheduler without lookups. Envelope
  memory is proportional to the messages in flight.
- A quarantined actor stays quarantined while its mailbox is empty, because
  the state is in the table and not in the mailbox.
