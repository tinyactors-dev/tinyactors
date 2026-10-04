# ADR 0101: Delayed sends are envelopes

**Status:** Accepted (2026-10-04); implementation pending (ring-proto Phase 4.3). Refines ADR 0026 and ADR 0054: a delayed send is an envelope in the pool (ADR 0099), indexed by its due time and listed with its owner; the per-owner storage, the owner heap and the orphan list go.
**Date:** 2026-10-04

## Context

ADR 0026 makes a delayed send its owner's, indexed globally. ADR 0054
stores each owner's pending sends in its sparse state, in an array sorted
by `(due, timer id)`, and keeps a global heap of owners (`src/timer.c`,
`src/timer.h`, 634 lines). Sends the router refused move to a system-owned
orphan list when their owner is destroyed or unloaded.

Under ADR 0099 every pending input is an envelope in one pool. A delayed
send is a pending input that is not due yet. Keeping it elsewhere means a
second store with its own growth, its own out-of-memory path (ADR 0070
lists "a delayed `<send>` growing its owner's storage"), and its own
capacity that charts cannot see.

The proposal considered letting the host keep timers (option B) and chose
core (option A,
[envelope.md](../../../docs/proposals/ring-proto/reference/envelope.md#delayed-sends-envelopes-with-a-due-time)),
for the reasons in section 6.

## Decision

### 1. A delayed `<send>` takes an envelope in state `timer`

- It takes an envelope from the pool, whatever its target: a local
  session, or a host I/O processor. The envelope keeps the evaluated data
  and content as separate references, the I/O processor type, the target
  and every field of the event. Firing evaluates nothing again.
- It counts against the pool, against its input's macrostep reserve
  (ADR 0099), and against `maxPendingSends`, which becomes a per-session
  quota on envelopes in state `timer` (default: none). A send that finds
  any of them exhausted fails as `error.execution.capacity.pool`.
- The envelope goes into the **deadline index** and onto its sender's
  **owner list**.

### 2. The deadline index is a heap of envelope indexes

The deadline index is an indexed min-heap (block B2) of `u32` envelope
indexes, keyed by `(due, send order)`. Each envelope stores its position in
the heap in `heap_index`, so removing any one costs O(log n). Its storage
is one `u32` per envelope, grown in the same refill as the pool, so it
never grows inside a macrostep. Its head is the earliest due time in the
system.

### 3. The owner list makes `<cancel>` exact

The owner list is an intrusive list (block B1) through `owner_next`, with
its head in the sender's actor record (4 bytes).

- **`<cancel>`** walks the sender's owner list and removes and frees every
  pending envelope whose `send_id` matches, including duplicate authored
  IDs. A timer core has already turned into input cannot be cancelled,
  which is what SCXML says; core is the only place that turns it, so there
  is no race with a host.
- **ADR 0084's withdrawal** of one pending send by owner and send ID walks
  the same list.
- **Termination and destroy** walk the owner list and free its envelopes.
  There are no submitted sends any more (ADR 0099 removed the router), so
  nothing outlives its owner and the system-owned orphan list goes.
- Each costs O(k + k log n) for k sends of the owner and n pending sends in
  the system. Nothing scans the system, which keeps ADR 0054's rule 7.

### 4. Time makes timers due, and `ta_run` reports the next due time

- **Due timers leave the index in `(due, send order)`** when the system
  clock reaches them. Until `h2c.time` (Phase 5.1) the host moves the clock
  with `ta_system_set_time()`, and `ta_run` moves due timers before it
  takes an input.
- **A local target's timer** becomes runnable at the end of the run queue.
- **An I/O processor's timer** is sent when it is due, outside any
  macrostep, through the I/O processor's `send` until Phase 5.2 and as a
  `c2h.send` with `macrostep` 0 after it. Its envelope is freed once the
  send is made.
- **A due timer whose target is gone** is recycled into
  `error.communication` for its sender (ADR 0099 section 6).
- **`ta_run` reports the next due time** in two new fields of
  `ta_run_params`, so the host needs one timer:

  ```c
  TA_OUT TA_OPTIONAL ta_time *next_due;  /* the earliest due time after this call */
  TA_OUT TA_OPTIONAL bool *has_next_due; /* false: no timer is pending */
  ```

  In Phase 5.1 this becomes `c2h.wake`, written when the head of the index
  changes. `ta_system_dispatch_due` and `ta_system_next_deadline` go.

### 5. Timers travel with their owner's image

Unloading a session writes its pending timers into its image, as ADR 0026
and ADR 0054 require, and frees their envelopes. Loading takes envelopes
for them again, at a safe point, and puts them back in the deadline index
and the owner list. A load that cannot take them, because the pool is at
its bound, fails with `TA_ERR_FULL` and changes nothing (later
`c2h.bounced` with reason `pool`). A replaying session's held timers
(ADR 0089) are envelopes like the others, dropped as its clock moves.

### 6. Why core keeps the timers, not the host

- **`<cancel>` is exact.** Core fires a timer only when it reads the clock,
  with nothing queued in between, so a cancelled timer never arrives. If
  the host kept timers, one that fired just before the `<cancel>` would
  still arrive, and a chart that reuses a send ID would act on it.
- **The capacity is shared.** A timer is an envelope like any other, so a
  fanout of 10,000 timeouts is limited by the pool, and the 10,001st fails
  where the chart can see it.
- **The host needs one timer,** set from the next due time.
- **A session's state is one artifact.** Its pending timers are in its
  image.

### 7. Inspection

The owner list is not an array, so ADR 0054's borrowed `delayed_sends`
array in `ta_actor_inspection` cannot be filled in O(1). Inspection keeps
`delayed_send_count`, and the list is read by page, as the actor walk
(ADR 0062) and the definitions walk (ADR 0078) are:

```c
typedef struct {
    ta_actor_id actor;
    uint32_t start;                                   /* position in due order */
    TA_OUT ta_delayed_send_info *out;
    uint32_t capacity;
    TA_OUT uint32_t *count;
} ta_actor_delayed_sends_params;
TA_API ta_result ta_actor_delayed_sends(ta_system *system, TA_IN const ta_actor_delayed_sends_params *params);
```

The owner list is in send order, so the call walks it and sorts the page
in the caller's buffer by due time and send order. It costs O(k) for k
sends of the actor, never O(the system). `ta_delayed_send_info.submitted`
goes, since nothing is submitted any more.

## Consequences

- **`<cancel>` is exact** and costs a walk of the owner's own sends.
- **Memory:** per pending send, one envelope (88 bytes) and its 4-byte
  slot in the deadline index, plus its text, in place of ADR 0054's array
  element, private element and heap entry. Per actor: 4 bytes for the
  owner-list head; the sparse state no longer holds sends, so an actor with
  a pending send no longer keeps sparse state for it. Per system: the
  orphan list and the owner heap go.
- **Allocations:** scheduling a delayed send never allocates. ADR 0054's
  growth of an owner's array, its `TA_ERR_NOMEM` from destroy and unload,
  and its open case of a refused send lost for want of memory, all go.
- **Removed:** `timer.c` and `timer.h`, the sends in sparse state,
  `ta_system_dispatch_due`, `ta_system_next_deadline`, and
  `ta_delayed_send_info.submitted`. `ta_send_id` stays.
- **Tests:** dispatch order across owners with ties; `<cancel>` of
  duplicates and of a due timer; a fanout of timers that fails at the
  pool's bound; timers across unload and load; a load refused for want of
  envelopes. ADR 0054's scale measurement (inspect, and cancel and rearm,
  against every other actor's send) must stay flat.
