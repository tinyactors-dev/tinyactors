# ADR 0008: Faults are per actor

**Status:** Accepted
**Date:** 2026-09-23

## Context

In the first addon, any non-OK result from `ta_actor_resume()` or
`ta_actor_deliver()` aborted the whole pump. The failing envelope or
runnable entry had already been popped, so it was lost. The error then
surfaced as a thrown exception, a rejected promise, or sometimes both from
the same call. With four million actors, one bad actor must not stop the
rest.

## Decision

1. **A failure is attributed to an actor and handled there.**
   - `TA_ERR_NOMEM` from a resume or deliver: the macrostep is retryable
     (tinyactors-core ADR 0047). The actor goes back on its queue and
     backs off for a turn. The pump goes on with other actors.
   - Any other failure: the actor is **quarantined**. It is taken out of the
     scheduling queues, and its mailbox is kept but not delivered. A
     `fault` hook receives `{ actor, operation, code, message }`.
     `system.release(id)` puts the actor back into scheduling, and
     destroying it discards the mailbox.
   - Messages dropped because their target is done or unknown go to a
     `deadLetter` hook with `{ target, event, reason }`. If no hook is set,
     they are counted in system statistics and dropped.
2. **The pump never throws.** Failures inside a turn reach JavaScript only
   through hooks and, for `send()`, through that message's promise.
3. **API calls throw only for misuse**, such as wrong argument types, a
   closed system, or a definition from another system. They are thrown
   synchronously as `TinyactorsError` with a `code`. A call never both
   throws and rejects.
4. **Result names and codes are generated** from
   `tinyactors-core/include/tinyactors.api.json`, not written by hand, so
   new core results (such as `TA_ERR_DONE`) reach JavaScript without edits.

## Consequences

- A system-wide failure is only something that is actually system-wide,
  such as the system being closed or its allocator failing during setup.
- Applications that want "stop the world on first fault" can set that in
  their `fault` hook.
