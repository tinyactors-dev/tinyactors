# `continuation.h`

A macrostep that has not reached stability is resumed from two places.

**Between microsteps** the actor holds everything (ADR 0009): its
configuration, history, internal queue, current and pending external
events. `ta_actor_resume()`'s `budget` counts microsteps and pauses only
there.

**Inside a phase** a run stops in two ways. A pause (ADR 0061) stops the
executor between two units of executable content of a microstep: an
action of a block or a `<foreach>` iteration, counted against
`action_budget`. Running out of memory (ADR 0070) stops it before the step
that failed, in any phase: the microstep's (`EXIT`, `TRANSITIONS`,
`ENTER`), the bootstrap's `DATA` and `SCRIPT`, `FINALIZE` and `SELECT`
after an external event was taken, `INVOKES` at the end of a macrostep,
and `FINISH` at a top-level final state. What the interpreter held in its
scratch then moves into one `ta_continuation`, reached from the actor's
sparse state:

- `ta_micro_position`: the phase, the state, transition or cursor it is
  at, the stage, the handler (or data declaration) being run, and whether
  the frames hold an open block;
- the live active set, the exit set, the entry set and the default-entry
  set, computed once as the microstep began;
- the data declarations the binding supplies, while bootstrapping;
- the default-history map and the enabled transitions;
- the executor's `ta_exec_frame`s: open blocks with their cursors, and each
  running `<foreach>` with its snapshot, position and count (ADR 0033).

The continuation holds no pointers, so a copy is a copy of its bytes plus
a retain of each `<foreach>` snapshot. A session image carries one; loading
or reconstructing the image gives the new actor its own copy (ADR 0061
section 4). Resuming moves it back into scratch and frees the allocation.

Saving cannot fail. Every `ta_actor_resume()` first reserves a
continuation large enough for the definition (the one it resumes from, or
the system's spare) and a sparse state for an actor without one, so a
run stops without allocating; a continuation the run no longer needs goes
back to the system as its spare (ADR 0070).

Nothing pauses in the bootstrap, in `<finalize>`, in exiting at a
top-level final state, or in exiting a cancelled invoked session; running
out of memory stops all of them but the last. An
invoked session cancelled while paused inside a microstep exits its live
active set, and the rest of its continuation is dropped.

The implementation never reports a half-completed microstep as
`TA_RUN_STABLE`, and the configuration it reports while paused is the one
after the last completed microstep.
