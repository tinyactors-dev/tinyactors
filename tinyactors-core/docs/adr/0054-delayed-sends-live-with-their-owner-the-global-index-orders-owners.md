# ADR 0054: Per-actor queries do not scale with the population

**Status:** Proposed; applies ADR 0018 and ADR 0026
**Date:** 2026-09-23

## Context

ADR 0018 sets the design target at about four million actors in one
process. ADR 0026 keeps delayed sends in one global heap, indexed by due
time, not by owner.

`ta_actor_inspect()` fills `delayed_send_count` with
`ta_timer_count_owner()` (`src/timer.c:191`). That function scans the whole
global heap. So the cost of inspecting one actor grows with the number of
delayed sends in the whole system, not with the size of that actor.

Hosts call `ta_actor_inspect()` often. The Node-API binding calls it twice
for each posted event: once to validate the target, and once to check
readiness before `ta_actor_deliver()`. Suppose each of four million actors
holds a single timeout. Then every message costs four million heap reads,
and a pump that delivers N messages costs N × 4M. The heap is "physically
globally indexed", which is correct for dispatch, but it was never meant to
make a per-actor question global.

## Decision

1. **A query about one actor costs O(size of that actor), not O(size of the
   system).** "Size of that actor" means its definition-sized session
   storage, its sparse state, and its own lists. This rule covers
   `ta_actor_inspect()` and every other function that takes one
   `ta_actor_id` and reads state.
2. **The per-owner delayed-send count is kept, not computed.** The actor's
   sparse state (ADR 0018) holds a counter. `ta_timer_schedule()` increments
   it. Every path that removes a send owned by a resident actor decrements
   it: pop on dispatch, cancel, withdraw, and termination. Actors that never
   schedule a delayed send do not pay for it, because the counter lives in
   the sparse allocation that such an actor does not have.
3. **Submitted sends that outlive their session (ADR 0026) are not counted.**
   Once the owner is destroyed, no actor is left to report the count.
4. **`ta_actor_delayed_sends()` may still scan.** That function copies a
   list and the host calls it deliberately. It is not on a delivery path.
   Its count parameter is sized from the kept counter.
5. A test inspects one actor while the system holds a large number of delayed
   sends from other actors, and asserts that the work done does not depend
   on that number (for example, by counting heap reads).

## Consequences

- Hosts can inspect an actor on a hot path without a hidden quadratic cost.
  Bindings still should not inspect before each delivery; ADR 0055 makes
  `ta_actor_deliver()` the admission test.
- The timer module needs a way to find the owner's sparse state when a send
  leaves the heap. `ta_actor_lookup()` is O(1) (a slot and a generation), so
  this adds no scan.
- An actor that schedules its first delayed send allocates sparse state if
  it did not already have it. Under ADR 0047 that allocation is already on
  a retryable path.
