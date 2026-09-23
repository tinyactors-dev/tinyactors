# `timer.h`

A delayed send contains the **already evaluated** semantics of a future dispatch:

- timer ID;
- owning actor/session;
- send ID;
- absolute logical due time;
- structured event name;
- I/O processor type atom;
- target value;
- payload value.

`ta_delayed_send.io_type` is the resolved SCXML `<send>` type. It is retained with the other evaluated send arguments so that due-time dispatch selects the same I/O processor without reconstructing information from chart source.

## Stored with the owner, indexed by owner (ADR 0054)

Each actor keeps its pending sends in its sparse state, in one list sorted
by `(due, timer id)` (`ta_timer_list`): the public `ta_delayed_send_info`
array that inspection lends out, and a parallel array of private fields
(timer ID, owner, and the envelope of a submitted send). An actor with no
pending send pays nothing.

The system keeps a min-heap of the lists that hold at least one send
(`ta_timer_heap.owners`), keyed by each list's front send. The top list's
front is the globally earliest send, so dispatch order is exactly
`(due, timer id)` across all actors. Each list records its heap position.

- Schedule: binary search and insert into the owner's list, then re-key
  its heap entry if the send became the front.
- Dispatch: take the top list's front send, then re-key or remove the
  list's heap entry.
- Cancel and withdraw: remove from the owner's list, re-keying if the front
  changed.
- Termination, destroy, unload and capture touch the owner's list only.

Every one of these costs O(k + log A): k sends of that owner, A owners with
sends. Nothing scans the system.

A send the router refuses when it falls due (`TA_EXTERNAL_EVENT_RETRY`,
ADR 0040) is **submitted**: it is already sent, keeps its envelope, can no
longer be cancelled, and waits to be offered again at its own place in the
order. It goes back into its owner's list. Submitted sends of an actor that
is destroyed or unloaded move to the system-owned `orphans` list, which
takes part in the heap like any owner. Destroy and unload reserve that room
before changing anything, so they can fail with `TA_ERR_NOMEM` and leave the
actor as it was.

## Session images and durability

A session image captures the actor-owned delayed-send semantics needed to reconstruct the interpreter. Host `ta_value` target/payload objects are retained as references, not serialized by core.

A portable durability layer decides how to encode/re-resolve those values and how to index deadlines for non-resident actors. Destroying an actor cancels its delayed sends; capturing/reconstructing the same session does not.

Unloading removes the actor's unsubmitted sends from the resident index
without cancelling their semantic records in the image; its submitted sends
stay resident, owned by the system. Before discarding the in-memory image, the
host records `ta_session_image_next_deadline()` in its non-resident deadline
index. Loading restores the sends to the actor's list; sends already due become
eligible for the next bounded dispatch.

## Host dispatch

`ta_system_next_deadline()` returns the earliest resident deadline, or
`TA_ERR_NOT_FOUND`. The embedder advances logical time and calls
`ta_system_dispatch_due()` with a finite budget. Dispatch occurs in deadline
order through the recorded I/O type and reports whether more work is already
due. The calls never block, sleep, or create a scheduler inside Tinyactors.
