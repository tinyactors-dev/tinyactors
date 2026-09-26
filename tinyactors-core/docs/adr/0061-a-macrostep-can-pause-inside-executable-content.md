# ADR 0061: A macrostep can pause inside executable content

**Status:** Accepted and implemented; completes ADR 0033, applies ADR 0009, ADR 0038, ADR 0047 and ADR 0060; section 3 superseded by ADR 0070 (saving a pause cannot fail); section 5 refined by ADR 0083 (inspection reports where a paused actor stands)
**Date:** 2026-09-23

## Context

ADR 0009 makes run-to-completion semantic rather than a C-stack boundary:
`ta_actor_resume()` runs up to `budget` microsteps and answers
`TA_RUN_PAUSED`, and the next resume continues the macrostep. ADR 0033 goes
further. It says a macrostep may pause and resume *inside* a `<foreach>`,
keeping the loop's snapshot and position, and `src/continuation.h` declares
a `ta_continuation` for exactly that: a phase, cursors, the exit and entry
sets, and a stack of `ta_exec_frame`s. None of it is used.
`continuation.c` is one `#include` line, and the budget check in
`ta_interpreter_run` pauses only between microsteps.

So a microstep is the smallest unit a host can interrupt. One `<foreach>`
over a million items, or one entry handler with a long script of actions,
runs to its end inside a single `ta_actor_resume()` whatever the budget. A
host that shares a thread between many actors (ADR 0002, the napi pump)
cannot bound the latency of one call.

ADR 0060 removed recursion from the executor. Executable content now runs
as one loop over `ta_exec_frame`s held in the interpreter's scratch, which
is what makes stopping in the middle of a block possible at all: the whole
position is data, not C-stack frames.

The existing budget has a contract that tests and hosts rely on: it counts
microsteps. `pausing.rb` asserts one microstep per `resume(budget: 1)` and
an exact number of pauses. Redefining what `budget` counts would break that
contract for every caller.

## Decision

### 1. A second budget counts actions

`ta_actor_resume_params` gains `action_budget`:

```c
typedef struct {
    ta_actor_id actor;
    uint32_t budget;          /* microsteps, as before (ADR 0009) */
    TA_OUT ta_run_status *status;
    /* Units of executable content this call may run before it pauses; zero
     * means no limit. A unit is one action of a block (an <if> and a
     * <foreach> count as one each, like any other action) or one <foreach>
     * iteration. */
    uint32_t action_budget;
} ta_actor_resume_params;
```

`budget` keeps its meaning exactly. When `action_budget` is not zero, the
call also pauses before running a unit once it has run `action_budget`
units, and answers `TA_RUN_PAUSED`, as a budget pause does. Both budgets
are per call. Zero, the value of every existing caller, keeps today's
behaviour exactly.

An action is the unit because it is the smallest step the executor takes
and the one whose cost a host cannot bound from outside. A `<foreach>`
iteration is also a unit, so an empty-bodied loop over a large array still
pauses.

### 2. A pause inside a block is a pause inside a microstep

A block runs inside a microstep: exit handlers, transition content, and
entry handlers, initial and history content. So a paused actor may be
halfway through exiting states, between two transitions' content, or in
the entry handlers of the third of five states being entered. The
continuation records exactly where:

| Field | Holds |
|---|---|
| `phase` | `NONE` (no microstep open), `EXIT`, `TRANSITIONS` or `ENTER` |
| `state` | `EXIT`: the states still to exit are the ones in the exit set below this ID, highest first. `TRANSITIONS`: the index into the enabled set of the transition whose content runs. `ENTER`: the next state to consider, lowest first. |
| `stage` | Within one state. `EXIT`: 0 running handlers, 1 handlers done. `ENTER`: 0 not yet activated, 1 handlers, 2 the initial transition's content, 3 the history default's content, 4 done data and `done.state`. |
| `handler` | The index of the `<onentry>`/`<onexit>` handler being run. |
| `block_open` | Whether the frames hold an open block that the phase's current step resumes. |
| the sets | The live active set, the exit set, the entry set, the default-entry set and the default-history map, as computed when the microstep began, and the enabled transitions. |
| the frames | The executor's `ta_exec_frame`s: block positions, and for each running `<foreach>` its snapshot, position and count. |
| two flags | Whether the microstep's event came from the internal queue or the external slot, which the `TA_ERR_NOMEM` path needs to put it back (ADR 0047). |

Every step that has an effect outside the interpreter happens exactly
once and is recorded by `stage` before the next pause point: activating a
state, starting its invocations, late data initialization, recording
history, cancelling an exited state's invocations. Nothing is recomputed
on resume. The sets are the ones the microstep began with.

A pause point is only between units, inside a block. The configuration
the actor reports (`ta_actor_active_states()`, the session storage) stays
the one after the last *completed* microstep, which is a legal
configuration (SCXML 3.11). The half-changed live set is continuation
state, used by `In()` when the microstep resumes, exactly as while it
ran.

Pausing inside content is limited to microsteps of a bootstrapped actor.
The bootstrap (data initialization, the top-level `<script>`, the initial
microstep), `<finalize>` (ADR 0039), exiting at a top-level final state,
and exiting a cancelled invoked session (ADR 0060) run to their end, as
before. Each of those is either bounded by the definition or happens
outside `ta_actor_resume()`.

### 3. A paused actor's frames move to its continuation

While a microstep runs, its position, sets and frames live in the
interpreter's scratch (ADR 0060). The scratch is shared by every actor the
system runs, so when the actor pauses they move into one allocation owned
by the actor, reached from its sparse state. Actors that never pause mid-
microstep have none, so the per-actor cost is unchanged. When the actor
resumes, they move back into scratch and the allocation is freed. Moving
transfers ownership of the `<foreach>` snapshots: nothing is retained or
released.

Saving can fail. **A pause that cannot save does not pause.** The
microstep keeps running, and the next unit boundary tries again. The
budgets are therefore upper bounds on work between pause *opportunities*,
not guarantees, which is consistent with what `budget` already is: it
never splits a microstep. This keeps ADR 0047 whole. A failed allocation
at a pause point is not an error, has no visible effect, and never leaves
a half-applied microstep; the microstep simply goes on. The alternative,
reserving the save before every microstep of every call with an
`action_budget`, would allocate on every microstep to protect a rare path,
and would put a new `TA_ERR_NOMEM` before microsteps that never pause.

ADR 0060's rule still holds: nothing allocates frames while actions run.
The save happens between units, after the executor has stopped.

### 4. Session images carry the continuation

A capture of an actor paused inside a microstep carries its continuation:
the fields and sets are copied into the image's arena, and each
`<foreach>` snapshot is retained, as the image retains every other host
value (ADR 0050). Loading or reconstructing the image (ADR 0051, ADR 0052)
gives the new actor a copy, retaining the snapshots again, and its first
resume continues the microstep where the captured one stopped. Releasing
the image releases its snapshots.

Positions are indices into the definition, so an image restores only
against the exact revision it was captured from, which ADR 0038 already
requires. A replacement (`ta_actor_prepare_replacement`) is a fresh actor
of the new definition and inherits no continuation.

### 5. Inspection and refusals

`ta_actor_inspection` gains `microstep_in_progress`: true while the actor
is paused inside a microstep. `macrostep_in_progress` is true then too.
`ta_actor_deliver()` answers `TA_ERR_BUSY`, as for any paused macrostep.
`active_state_count` and `ta_actor_active_states()` report the
configuration after the last completed microstep.

### 6. Cleanup and interactions

- Destroying, unloading or freeing an actor frees its continuation and
  releases its snapshots. Retiring a definition changes nothing: the actor
  still holds it.
- Cancelling an invoked session that is paused inside a microstep exits
  the states of its *live* active set, the one the interrupted microstep
  was changing. States it already exited are not exited again, and states
  it already entered are exited. The rest of the continuation is dropped
  and its snapshots released.
- Delayed sends and invocations are unaffected: sends a paused microstep
  already made are scheduled; invocations of states it entered start when
  the macrostep ends, as before.

## Consequences

- A host can bound the work of one `ta_actor_resume()` call by actions as
  well as by microsteps. With `action_budget = 1`, a run pauses before
  every action and every `<foreach>` iteration, and its observable result
  is identical to an unpaused run. Tests assert that for every budget.
- Existing callers see no change: `action_budget` is zero for them.
- `ta_actor_resume_params` and `ta_actor_inspection` each gain a member.
  Hosts that build them with designated initializers are unaffected. The
  napi bindings need regenerating.
- A paused-in-microstep actor costs one allocation for its continuation
  until it resumes, sized by its definition.
- The bootstrap, `<finalize>`, and exits at termination or cancellation
  remain uninterruptible.

## Implementation notes

- **Where it lives.** `ta_micro_position` (`src/continuation.h`) is the
  position while a microstep runs, held in the interpreter. `ta_microstep`
  is resumable: it computes the exit set (recording history) and the entry
  set once, as it reaches each phase, and continues from `at` otherwise.
  `ta_run_block` stops only at the microstep's own level, never in a block
  nested under another run of the executor, so a pause always leaves the
  frames from the bottom of the stack up.
- **The saved form holds no pointers.** `ta_continuation` is a header
  followed by the four sets, the default-history map, the enabled
  transitions and the frames, at offsets computed from the counts. So a
  copy for a session image, or back into an actor, is a copy of `bytes`
  plus a retain of each `<foreach>` snapshot.
- **A failed save** raises the call's action budget by one unit and
  continues the same microstep, so the next unit boundary tries again.
- **Resuming a paused microstep touches the session epoch**, like
  starting one: an image captured while paused is stale once the actor
  continues.
- **Activation stays one stage.** Setting the state active, noting its
  invocations and late data initialization happen together in
  `TA_ENTER_ACTIVATE`, before any content of the state can pause, as
  before. The existing order, in which a late-initialization result
  replaces the invocation hook's result, is unchanged.
- **Tests.** `tests/pause_test.c` runs a chart with nested `<if>` and
  `<foreach>` in exit handlers, transition content and entry handlers,
  and gets a trace identical to an unpaused run for every action budget
  from 1 to 80. It also captures an actor paused inside a nested
  `<foreach>`, destroys it, reconstructs it from the image and finishes
  identically; refuses every save and finishes in one call; and checks
  that no allocation or snapshot leaks. `pausing.rb` runs the
  equivalence workload with action budgets 1 to 6. It also checks that an
  actor paused inside a microstep is busy and reports only whole
  configurations, and that cancelling a child paused inside a microstep
  exits its live states.
