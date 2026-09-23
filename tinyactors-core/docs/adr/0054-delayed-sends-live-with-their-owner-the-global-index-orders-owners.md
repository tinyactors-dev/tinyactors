# ADR 0054: Delayed sends live with their owner; the global index orders owners

**Status:** Proposed; refines ADR 0026, applies ADR 0018
**Date:** 2026-09-23

## Context

ADR 0018 sets the design target at about four million actors in one
process. ADR 0026 says a delayed send belongs to its actor but "may be
indexed globally" so that due work is found without scanning actors. Core
does this by storing the send records *in* the global heap
(`ta_timer_heap.items`) and nowhere else.

Because of that, every question about one actor's sends is a scan of every
send in the system:

- `ta_actor_inspect()` fills `delayed_send_count` with
  `ta_timer_count_owner()`, a full heap scan (`src/timer.c:191`).
- `ta_actor_delayed_sends()` repeats a selection pass over the whole heap
  once per returned send (`src/actor.c:835`). That is O(k × S) for k sends
  of the actor and S sends in the system.
- Cancel, withdraw, termination, destroy and capture also scan by owner.

The count is also the wrong thing to show. "3 delayed sends" does not tell an
operator what an actor is waiting for. Inspection already returns the
internal event queue and the active invocations as borrowed arrays. Delayed
sends should appear the same way: the whole list, as a borrowed array,
available in O(1).

## Decision

1. **A delayed send is stored with its owner.** The actor's sparse state
   (ADR 0018) holds its pending sends in one array sorted by
   `(due, timer id)`. The array element is the public
   `ta_delayed_send_info`. Private fields (the timer ID, the resolved
   destination for retry, and the envelope of a submitted send) go in a
   parallel array. This is the same arrangement as `invokes` and
   `invoke_meta`. Actors with no delayed sends pay nothing.
2. **The global index orders owners, not sends.** The system heap holds one
   entry per actor that has at least one pending send. The entry's key is
   the `(due, id)` of that actor's first send, and it records the actor's
   position. The sparse state stores the entry's heap position.
   - The global minimum is the front send of the owner at the top of the
     heap. Dispatch order, `ta_system_next_deadline()` and
     `ta_system_dispatch_due()` behave exactly as before.
   - *Schedule:* binary search plus insertion into the owner's array. If the
     send is the owner's new front, fix up the owner's heap entry.
   - *Dispatch:* take the owner's front send, then re-key or remove the
     owner's heap entry.
   - *Cancel or withdraw:* remove from the owner's array. If the removed send
     was the front, re-key the heap entry.
   - *Termination, destroy, unload and capture:* work on the owner's array
     only.

   Each operation costs O(k + log A), where k is the owner's sends and A is
   the number of actors with sends. Nothing scans the system. The heap
   holds A entries instead of S, so it also shrinks.
3. **Submitted sends that outlive their session (ADR 0026) move to a
   system-owned list.** Destroying an actor already discards unsubmitted
   sends and keeps submitted ones queued for their receivers. Core moves
   those sends into one system-owned sorted list, which takes part in the
   heap like an owner. They are no longer any actor's state, so they are
   not reported by actor inspection. Unloading an actor does the same. The
   move may need room in that list; core reserves it before anything
   changes, so `ta_actor_destroy()` and `ta_actor_unload()` can fail with
   `TA_ERR_NOMEM` and leave the actor as it was. A send the router refuses
   at dispatch goes back to its owner's list, or to the system-owned list
   when the owner is gone. System teardown releases the list.
4. **Inspection returns the list.** `ta_actor_inspection` replaces
   `uint32_t delayed_send_count` with a borrowed array:

   ```c
   TA_COUNTED_BY(delayed_send_count) TA_OPTIONAL
   const ta_delayed_send_info *delayed_sends;
   uint32_t delayed_send_count;
   ```

   Its order is by due time, then send order. It follows the same borrowing
   rule as `internal_events` and `invokes`: valid only while the actor is
   externally synchronized against mutation.
5. **`ta_delayed_send_info` carries what an operator needs.** It already has
   `send_id`, `due`, `event`, `io_type`, `target` and `data`. It gains:
   - `bool submitted`: the router refused the send once and it is waiting to
     be offered again (ADR 0026, ADR 0040);
   - `ta_actor_ref destination`: the resolved target for the built-in SCXML
     processor, or none for custom processors, which use `target`.
6. **`ta_actor_delayed_sends()` is removed.** Inspection is now the one read
   path, as it already is for internal events. A host that needs the list
   beyond the synchronization window copies it and retains `target` and
   `data`.
7. **Rule:** a function that takes one `ta_actor_id` and reads state costs
   O(size of that actor) — its definition-sized session storage, its sparse
   state and its own lists — never O(size of the system). A core test
   (`tests/delayed_send_test.c`) checks one actor's list while thousands of
   other actors hold sends. The population scale test
   (`tinyactors-test/tests/scale_test.c`) gives every actor a send, measures
   inspecting an actor and arming then cancelling one more send at two
   populations, and requires the cost per operation to stay flat.

## Consequences

- An operator sees exactly what each actor is waiting for, including sends
  held back by the router, with no extra call and no scan.
- Hosts can inspect on hot paths. They still should not inspect before each
  delivery: ADR 0055 makes `ta_actor_deliver()` the admission test.
- The timer module stops being a flat heap of records. It becomes
  per-owner sorted arrays plus an owner heap. `ta_timer_*` functions take
  the owner's sparse state instead of scanning by owner ID. Session capture
  (`ta_timer_copy_unsubmitted`) becomes a copy of the owner's array.
- Scheduling an actor's first delayed send may allocate sparse state and
  grow its array. Under ADR 0047 both happen on the existing retryable path,
  as `ta_timer_reserve()` does today.
- An actor with a pending send keeps its sparse state. The other storage in
  it is released when empty at the end of a macrostep, so an actor holds the
  same bytes however long it has run. `allocated_bytes` now counts the
  storage of the actor's sends.
- An actor with a very large number of pending sends pays O(k) per insert or
  cancel for array moves. This is bounded by that actor's own state, which
  is the stated rule.
- The public ABI changes: an inspection field changes type, two fields are
  added to `ta_delayed_send_info`, and `ta_actor_delayed_sends()` and its
  params type are removed.
- Callers updated with the change: the JSON-RPC test host (its bindings are
  generated from the header) and its Ruby scenario, trace and workbench
  code. `tinyactors-napi/index.d.ts`, which declares the count, still needs
  the same change.
