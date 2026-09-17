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

## Global index, actor ownership

The running system may store all delayed sends in a global heap/wheel so the next deadline is efficient. Ownership remains `(actor, send_id)`, enabling session-scoped cancellation and actor destruction.

## Session images and durability

A session image captures the actor-owned delayed-send semantics needed to reconstruct the interpreter. Host `ta_value` target/payload objects are retained as references, not serialized by core.

A portable durability layer decides how to encode/re-resolve those values and how to index deadlines for non-resident actors. Destroying an actor cancels its delayed sends; capturing/reconstructing the same session does not.

Unloading removes the actor's sends from the resident heap without cancelling
their semantic records in the image. Before discarding the in-memory image, the
host records `ta_session_image_next_deadline()` in its non-resident deadline
index. Loading restores the sends to the resident heap; sends already due become
eligible for the next bounded dispatch.

## Host dispatch

`ta_system_next_deadline()` returns the earliest resident deadline, or
`TA_ERR_NOT_FOUND`. The embedder advances logical time and calls
`ta_system_dispatch_due()` with a finite budget. Dispatch occurs in deadline
order through the recorded I/O type and reports whether more work is already
due. The calls never block, sleep, or create a scheduler inside Tinyactors.
