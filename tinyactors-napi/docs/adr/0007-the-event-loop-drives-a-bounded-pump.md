# ADR 0007: The event loop drives a bounded pump

**Status:** Accepted
**Date:** 2026-09-23

## Context

The first addon ran work only inside `flush()`. That function drained all
queues to quiescence synchronously and then returned a promise that was
already settled. Per-turn budgets did not bound the total work, so draining
four million actors blocked the event loop for the whole drain. Nothing ran
unless someone called `flush()`, and delayed sends never fired.

## Decision

1. **One pump turn does bounded work, in this order:**
   1. resume runnable actors, up to `resumeBudget` in total;
   2. deliver from the ready queue, up to `messageBudget`;
   3. call `ta_system_dispatch_due()` with `dueBudget`.
2. **Turns are scheduled by a private `MessageChannel`.** Enqueueing work
   posts one coalesced token if no turn is pending. At the end of a turn,
   another token is posted if work remains. The port is unreferenced, so an
   idle system does not keep the process alive. It is closed with the
   system. Actor data never goes through the channel (as `docs/design.md`
   explains).
3. **Time.** After each turn, the addon reads `ta_system_next_deadline()`
   and keeps exactly one JavaScript timer for it, re-armed only when the
   deadline changes.
   - With `clock: "realtime"`, the timer sets `ta_system_set_time()` from
     `performance.now()` relative to system creation, then wakes the pump.
   - With `clock: "manual"`, there is no timer. `advanceTo()` and
     `advanceBy()` set the time and return a promise that settles once
     everything due has been dispatched and the system is quiet.
4. **`flush()` resolves when the system is quiet**: no runnable actors, no
   ready actors, and nothing due at the current time. It spans as many
   turns as needed and yields to the event loop between them. Several
   `flush()` calls share one waiter list.
5. **Nothing runs synchronously in API calls.** `post`, `spawn` and the
   others only enqueue. Bootstrapping a spawned actor is runnable work like
   any other.

## Consequences

- Any single turn is bounded by the budgets, and other JavaScript runs
  between turns.
- The event loop is the only scheduler. There is no libuv or V8-specific
  API, so the same code runs on Node and Bun.
- Tests that need determinism use the manual clock and `await flush()`.
