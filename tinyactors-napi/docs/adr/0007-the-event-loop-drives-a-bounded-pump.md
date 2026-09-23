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
2. **The pump is native. Turns are scheduled by a threadsafe function.**
   The system creates one `napi_threadsafe_function` and calls it only
   from its own thread.
   - Enqueueing work calls the function once if no turn is pending, and a
     flag in the system coalesces further calls. Node and Bun both queue the
     call on their event loop and run the turn from there.
   - At the end of a turn, the function is called again if work remains.
   - The function is referenced (`napi_ref_threadsafe_function`) only while
     a turn is pending, and unreferenced (`napi_unref_threadsafe_function`)
     when the system is quiet. Queued work, and a script ending in `await
     system.flush()`, therefore run to completion, while an idle system does
     not keep the process alive. The function is released with the system.
   - No JavaScript file takes part in scheduling, and actor data never goes
     through the function.
3. **Time.** After each turn, the addon reads `ta_system_next_deadline()`
   and keeps exactly one timer for it, re-armed only when the deadline
   changes.
   - The timer is created from native code with the global `setTimeout`
     and cancelled with `clearTimeout`. Both runtimes provide them. The
     handle is unreferenced by calling its `unref` method.
   - Node-API has no timer of its own, and `napi_get_uv_event_loop` would
     tie the addon to libuv.
   - With `clock: "realtime"`, the timer callback sets
     `ta_system_set_time()` from a monotonic clock read natively (for
     example `clock_gettime(CLOCK_MONOTONIC)`), relative to system
     creation, and then wakes the pump.
   - With `clock: "manual"`, there is no timer. `advanceTo()` and
     `advanceBy()` set the time and return a promise that settles once
     everything due has been dispatched and the system is quiet.
4. **`flush()` resolves when the system is quiet**: no runnable actors, no
   ready actors, and nothing due at the current time. It spans as many
   turns as needed and yields to the event loop between them. Several
   `flush()` calls share one waiter list. The waiters are native promises
   (`napi_create_promise`), settled at the end of the turn that reaches
   quiescence.
5. **Hooks are called from native code.** The `done`, `fault` and
   `deadLetter` functions (ADRs 0008 and 0012) are passed in the system
   options and held as references by the system. The pump calls them at
   the end of a turn, after the last core call, so no hook runs while core
   is running.
6. **Nothing runs synchronously in API calls.** `post`, `spawn` and the
   others only enqueue. Bootstrapping a spawned actor is runnable work like
   any other.

## Consequences

- Any single turn is bounded by the budgets, and other JavaScript runs
  between turns.
- The event loop is the only scheduler. There is no libuv or V8-specific
  API, so the same code runs on Node and Bun.
- Tests that need determinism use the manual clock and `await flush()`.
- All scheduling state (queues, the pending-turn flag, the timer handle,
  waiters and hooks) lives in the native `System`. `close()` releases it in
  one place (ADR 0012).
- Calling a threadsafe function from its own thread is allowed by Node-API,
  but the function was designed for cross-thread use. The pump depends on
  both runtimes queueing that call on the event loop rather than running it
  inline. A test on each runtime checks that no turn runs inside the API
  call that enqueued the work.
