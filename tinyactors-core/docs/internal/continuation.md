# `continuation.h`

A macrostep that has not reached stability is resumed from two places.

**Between microsteps** the actor holds everything (ADR 0009): its
configuration, history, internal queue, current and pending external
events. `ta_actor_resume()`'s `budget` counts microsteps and pauses only
there.

**Inside a microstep** (ADR 0061) a pause stops the executor between two
units of executable content: an action of a block or a `<foreach>`
iteration, counted against `action_budget`. What the interpreter held in
its scratch then moves into one `ta_continuation` allocation, reached from
the actor's sparse state:

- `ta_micro_position`: the phase (`EXIT`, `TRANSITIONS`, `ENTER`), the
  state or transition it is at, the stage within that state, the handler
  being run, whether the frames hold an open block, and where the
  microstep's event came from (for the `TA_ERR_NOMEM` path of ADR 0047);
- the live active set, the exit set, the entry set and the default-entry
  set, computed once as the microstep began;
- the default-history map and the enabled transitions;
- the executor's `ta_exec_frame`s: open blocks with their cursors, and each
  running `<foreach>` with its snapshot, position and count (ADR 0033).

The continuation holds no pointers, so a copy is a copy of its bytes plus
a retain of each `<foreach>` snapshot. A session image carries one; loading
or reconstructing the image gives the new actor its own copy (ADR 0061
section 4). Resuming moves it back into scratch and frees the allocation.

Saving can fail. A pause that cannot save does not pause: the microstep
runs one more unit and tries again, so `action_budget` never turns into
an error.

Nothing pauses in the bootstrap, in `<finalize>`, in exiting at a
top-level final state, or in exiting a cancelled invoked session. An
invoked session cancelled while paused inside a microstep exits its live
active set, and the rest of its continuation is dropped.

The implementation never reports a half-completed microstep as
`TA_RUN_STABLE`, and the configuration it reports while paused is the one
after the last completed microstep.
