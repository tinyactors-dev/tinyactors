# ADR 0005: Per-target mailboxes and a ready queue

**Status:** Accepted
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

1. **Mailbox per target, only while it has messages.** A hash map from
   `ta_actor_id` to a ring of envelopes. A mailbox is created on its first
   message and freed when it is empty. Idle actors cost nothing.
2. **Ready queue.** A FIFO of actor IDs that have mail and are between
   macrosteps. An actor is added when mail arrives for an idle actor, or
   when `ta_actor_resume()` returns `TA_RUN_STABLE` and its mailbox is not
   empty. Each actor is in the ready queue at most once (a flag in its
   mailbox).
3. **Runnable queue.** A FIFO of actors whose macrostep paused on budget
   (`TA_RUN_PAUSED`). They are resumed in later turns.
4. **Delivery trusts core's answer (tinyactors-core ADR 0055).** The
   addon does not inspect before delivering. It calls `ta_actor_deliver()`
   with the head envelope and acts on the result:
   - `TA_OK`: pop the envelope and move the actor to the runnable queue;
   - `TA_ERR_BUSY`: leave the envelope in place (this means a bug in the
     ready-queue logic, and debug builds assert on it);
   - `TA_ERR_DONE` or `TA_ERR_NOT_FOUND`: drop the whole mailbox, with each
     envelope reported to the dead-letter hook (ADR 0008);
   - anything else: a fault (ADR 0008).
5. **Order.** Messages to one target are delivered in the order they were
   accepted. Nothing is promised about the relative order of different
   targets beyond what the ready queue's FIFO order gives.
6. **Envelopes are small.** An envelope holds the event name ID, the send
   ID, the origin, the invoke ID, and a data box only when data is present
   (ADR 0011). A post without data allocates no value.
7. **Backpressure.** Limits on the total number of queued envelopes and on
   retained data bytes are checked when a message is accepted: at
   `post`, and in the router (ADR 0006), which answers
   `TA_EXTERNAL_EVENT_RETRY` when full. A message that has been accepted is
   never dropped because of a limit.

## Consequences

- Every message costs O(1) amortized: one hash lookup, one ring push, one
  core deliver and one resume. No step scans a queue or the population.
- A hot actor does not slow down other actors. It only takes more turns
  itself.
- Memory for queues is proportional to the messages in flight plus the
  actors that have mail, not to the population.
