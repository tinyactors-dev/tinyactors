# `continuation.h`

A macrostep that has not reached stability is resumed from two places.

**Between microsteps** the actor holds everything (ADR 0009): its
configuration, history, internal queue, current and pending external
events. `ta_actor_resume()`'s `budget` counts microsteps and pauses only
there.

**Inside a phase** a run stops in two ways. A pause (ADR 0061) stops the
executor between two units of executable content of a microstep: an
action of a block or a `<foreach>` iteration, counted against
`action_budget`. With `pause_at_microstep_start` (ADR 0086) it also stops
a microstep at its `START`, after its transitions are selected and its
sets computed and before anything of it runs; a run that resumes from
there passes it. Running out of memory (ADR 0070) stops it before the step
that failed, in any phase: the microstep's (`EXIT`, `TRANSITIONS`,
`ENTER`; nothing between selection and `START` allocates), the bootstrap's `DATA` and `SCRIPT`, `FINALIZE` and `SELECT`
after an external event was taken, `INVOKES` at the end of a macrostep,
and `FINISH` at a top-level final state. What the interpreter held in its
scratch then moves into one `ta_continuation`, reached from the actor's
sparse state. What it holds and how its block is laid out is in the file
comment of `src/continuation.h`.

The continuation holds no pointers, so a copy is a copy of its bytes plus
a retain of each `<foreach>` snapshot. A session image carries one; loading
or reconstructing the image gives the new actor its own copy (ADR 0061
section 4). Resuming moves it back into scratch and gives the allocation back to the system as its spare continuation.

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

`ta_actor_abandon()` refuses an actor that holds a continuation
(`TA_ERR_BUSY`): the states a stopped step entered or exited have already
started or cancelled invocations and recorded history, which the stored
configuration does not reflect. Resuming with a budget of 1 and no action
budget finishes the step and pauses between microsteps, where abandoning
is allowed (ADR 0082).

The implementation never reports a half-completed microstep as
`TA_RUN_STABLE`, and the configuration it reports while paused is the one
after the last completed microstep.

Where a stopped run stands is read from the continuation (ADR 0083).
`ta_run_position()` maps a `ta_run_view` (the position, the enabled
transitions and the frames) to a `ta_trace_position`: the phase and state,
and the unit the run does next, from the top frame. The interpreter builds
the view from scratch for `PAUSED`, `RESUMED` and `STOPPED` records, and
`ta_actor_position()` builds it from the continuation for
`ta_actor_inspect()`, so both report the same place. At a microstep's
`START` the view's sets give the position: `ta_start_position()` finds the
first unit the microstep will run, walking the exit set, the enabled
transitions and the entry set in the order the microstep runs them, or,
with none, stands at the first transition with no action (ADR 0086).
