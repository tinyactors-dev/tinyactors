# ADR 0086: A resume can stop before a microstep's first unit

**Status:** Accepted and implemented (2026-09-26). Refines ADR 0061 section 1 (a pause point at a microstep's start) and ADR 0083 section 2 (what the position names there); applies ADR 0009, ADR 0047, ADR 0060, ADR 0067, ADR 0070, ADR 0075 and ADR 0082. Revised 2026-09-26: it no longer applies the withdrawn ADR on images across systems (see section 4).
**Date:** 2026-09-26

## Context

A debugger's breakpoint stops before its line runs. The debug adapter in
the test host (`docs/proposals/debug-adapter.md`) puts breakpoints on a
state's entry, a state's exit and a transition's content, and it stops
there by resuming with `action_budget: 1` (ADR 0061) and reading where
the actor stands (ADR 0083) after each call.

That cannot stop before a microstep's first unit. `action_budget` counts
units the call has run, and zero means no limit, so a call runs at least
one unit before it can pause inside a microstep; the microstep `budget`
pauses only between microsteps, before transitions are selected. A resume
that selects a microstep therefore always runs its first unit, and ADR
0083 reports the position after it: the second unit. A breakpoint whose
block is the first the microstep runs, such as the `<onexit>` of the state
an event leaves, stops one unit late, after that unit's effects. And a
step by units never lands on "before the first unit of the next
microstep", which is where a user who steps out of one microstep expects
to be.

The first unit is also not always the first thing a microstep does. It
exits states highest first, and a state with no `<onexit>` is exited
(its invocations cancelled, its bit cleared in the live set) without
running a unit. Pausing where the first unit actually is would stop after
those exits. Nothing a debugger shows about the microstep may have
happened yet when it stops for a breakpoint on the microstep's first
block.

Existing callers (napi's pump, the test host's scheduler, every scenario
and C test) resume with a microstep budget and an action budget, and
their observable behaviour, including how many times each call pauses,
must not change.

## Decision

### 1. A resume asks for it

`ta_actor_resume_params` gains one member:

```c
typedef struct {
    ta_actor_id actor;
    uint32_t budget;
    TA_OUT ta_run_status *status;
    uint32_t action_budget;
    bool pause_at_microstep_start;
} ta_actor_resume_params;
```

When it is true, every microstep the call begins pauses at its **start**
and the call answers `TA_RUN_PAUSED`. When it is false, the value of every
existing caller, nothing changes. A call that resumes an actor paused at
a start passes that start: it continues the microstep, so a stepping host
does not stop at the same place twice.

The member composes with the two budgets. `budget` still counts completed
microsteps; `action_budget` still counts units this call ran. A debugger
steps with `action_budget: 1` and the flag: from a start the call runs the
first unit and pauses before the second, and from the last unit of a
microstep it pauses at the next microstep's start. With `action_budget: 0`
and the flag, a call runs to the next microstep's start: stepping by
microsteps.

### 2. What has happened at the start, and what has not

The start is right after the microstep begins: its transitions are
selected and the sets are computed, as ADR 0067's implementation notes
already do before any action runs.

Done by then, and nothing else:

- taking the event: for an external event, its `<finalize>` and
  autoforwarding, which SCXML runs before selection; for an operator
  transition (ADR 0082), taking it in place of selection;
- selection: guards evaluated, their `error.execution` events queued,
  their `CONDITION` records written;
- the microstep begun: its exit set, its entry set, the default-entry
  set and the default-history map computed; its `MICROSTEP` record
  written, naming the sets; the microstep counted in its key and in the
  `microsteps` counter (ADR 0072); history recorded for the states it
  exits; the session epoch touched.

Not done: no `<onexit>` ran, no state was exited, no invocation was
cancelled, no transition content ran, no state was entered or had its
late data initialized, no `done.state` event was raised. The actor's
configuration is the one before the microstep, as in every pause inside a
microstep (ADR 0061 section 2).

History stays recorded when the microstep begins, before the start. The
entry set depends on it (a transition to a history state of a state it
exits restores what it just recorded), and the `MICROSTEP` record names
the entry set and the recorded history before anything runs. Moving the
recording after the start would mean computing the entry set from a copy
of the history and recording twice. The recording is not visible outside
the actor: the data model does not see it, and no I/O depends on it. That
is also why an actor stopped there cannot be abandoned (section 4).

### 3. A phase, and where it stands

The continuation's phase (`ta_run_phase`) gains `START`, the first of a
microstep's phases: its sets are computed, and nothing else of it is
held. `microstep_in_progress` is true there. `ta_microstep_begin()`
leaves every microstep at its start; running the microstep passes it,
unless the call asked to pause there. Passing it sets the position to the
first state of the exit phase, which is what beginning a microstep did
before.

Where it stands, for inspection (ADR 0083) and for the `PAUSED` and
`RESUMED` records, is **the first unit the microstep runs**, found from
the sets without running anything, in the order the microstep runs units:

1. the `<onexit>` handlers of the exit set, highest state first;
2. the content of the enabled transitions, in order;
3. for each state of the entry set, lowest first: its `<onentry>`
   handlers, then its initial transition's content if it is entered by
   default, then its history default's content if its history is entered
   without a recorded configuration.

The first block with an action gives the position, with the phase
mapping of ADR 0083: `EXIT` and the state, `TRANSITION` with the
transition and its source, or `ENTER` and the state (for initial and
history content, the state being entered, as ADR 0083 already reports
them). `action` is the block's first action and `iteration` is 0. It is
the same position a pause before that unit would report, so a breakpoint
on that action matches at the start.

**A microstep with no executable content** stands at its first
transition, with no action: `at` is `TRANSITION`, `transition` the first
enabled transition and `state` its source. For an operator transition
`transition` and `state` are `NONE`, since it has no compiled ID and the
continuation keeps only its mark (ADR 0082 section 4). The position is
well defined and names what the microstep is about to take, before any
of its effects; its `step` key names the microstep.

### 4. Interactions

- **The bootstrap's microstep never pauses at its start**, as it never
  pauses inside its content (ADR 0061 section 2).
- **Operator transitions** get the same stop: the jump's microstep is
  begun by taking the delivered event, then stands at its start. Its
  operator transition is freed as before; nothing after the start reads
  it.
- **Delivery** answers `TA_ERR_BUSY`, as for any paused macrostep.
- **Abandoning** (`ta_actor_abandon()`) answers `TA_ERR_BUSY`, as for any
  actor holding a continuation (ADR 0082 section 6): history was recorded
  for states the stored configuration still holds. Resuming with `budget`
  1 and `action_budget` 0 finishes the microstep and pauses between
  microsteps, where abandoning is allowed.
- **Cancelling an invoked session** stopped at a start exits its live
  active set, which there is the configuration before the microstep, and
  drops the rest (ADR 0061 section 6).
- **Session images** carry the stop: the continuation holds the phase and
  the sets, so a capture, an unload and load, and a reconstruction stand
  at the same start and report the same position. An image restores only
  into the system that captured it (ADR 0038, ADR 0074): this ADR first
  said "or another that registers the revision", after an ADR that let
  images cross systems for a debugger's forks; that ADR was withdrawn when
  the debugger stopped forking (tinyactors-napi ADR 0026).

### 5. Running out of memory

Pausing at the start cannot fail, and adds no allocation. Nothing
allocates between selection and the start, and the stop moves the
position and sets into the continuation the call reserved before it ran
(ADR 0070 section 3), or the one it resumed from. A resume from a start
that cannot get its scratch memory answers `TA_ERR_NOMEM` before doing
anything, and the actor stays at its start. Running out of memory after
the start is the case ADR 0070 already covers: the run stops before the
step that failed, and the retry continues from there.

## Alternatives considered

- **Count the start as a unit of `action_budget`.** Every microstep would
  add a pause opportunity for every caller with an action budget: napi's
  pump, the scheduler, and the tests that count pauses. Existing callers
  would pause more often, which the constraint rules out.
- **A reserved action budget that means "stop before any unit"**, such as
  a `TA_ACTION_BUDGET_BEFORE` constant. A count then carries a flag, and
  one value cannot say both "run one unit" and "stop at the next start",
  which is what a step from a start needs. A separate member composes
  with both budgets.
- **Letting the ADR 0061 pause check fire before the first unit where it
  sits.** It stops after the handler-less exits before it: states exited,
  invocations cancelled, and after entries that ran late data
  initialization. A microstep with no content would have no stop at all.
- **A second function**, such as `ta_actor_step()`. It would repeat
  resume's budgets, stops, records and out-of-memory rules, and the repo
  changes an API in place.
- **The existing pause between microsteps (`budget`).** It stops before
  selection: which transitions are taken, and so which block runs first,
  is not known yet, and the guards have not run, so the position can only
  be nowhere.
- **Storing the first unit's position in the continuation.** Inspection
  computes it from the sets the continuation already holds, in
  O(states + handlers), inside the O(size of the actor) ADR 0054 allows.
  Storing it would add bytes to every continuation and every image for a
  value that can be derived.
- **Recording history after the start.** See section 2.
- **Standing at the first exited state, or nowhere, when there is no
  content.** A targetless transition exits and enters nothing, so "the
  first exited state" is not defined for every microstep; "nowhere" is
  what a pause between microsteps reports, and would hide that a
  microstep has begun. Every microstep has a first transition.

**The microstep limit (ADR 0075) checks first.** Its check sits between
microsteps, before the next step is selected, and this stop comes after
selection. A resume at the limit therefore answers `TA_ERR_RUNAWAY` before
it would stop at the next microstep's start, and a resume with
`no_microstep_limit` stops at that start as any other.

## Consequences

- A debugger stops before a breakpoint's block runs, when that block is a
  microstep's first, and its step lands on each microstep's start. A
  breakpoint compares the stop's position with the action it is set on,
  the same test at a start as anywhere else.
- The position at a start names a unit of the microstep's future, found
  by lookahead. The unit runs next, but the non-unit effects before it
  (exiting states with no `<onexit>`, cancelling their invocations,
  activating states) happen first, as they do before any pause's next
  unit.
- Callers that do not set the member see no change: their pauses, records
  and results are the same, which the C sweep and the scenario suite
  check.
- Costs, measured on x86-64 Linux with clang -O2:

  | | Before | After |
  |---|---|---|
  | `ta_actor_resume_params` | 32 | 32 (the bool fills padding) |
  | `sizeof(ta_actor)` | 232 | 232 |
  | continuation, session image | – | unchanged: a phase value, no new field |
  | allocations per resume, per microstep | – | unchanged |
  | a microstep, ping-pong with `<onentry>` and `<onexit>` scripts, per event | 324–331 ns (min of 15 runs), 348–366 ns mean | 331–334 ns, 365–368 ns mean: within the run-to-run spread |
  | inspecting an actor stopped at a start | – | O(states + handlers), no allocation, no data-model call |

  The hot path gains one comparison per microstep: `ta_microstep()` checks
  for the start phase before passing it. Beginning a microstep no longer
  sets the exit position; passing the start does.
- Bindings: the test host binds the member through the generator
  (`pause_at_microstep_start:`), and its scenario DSL passes it
  (`Actor#resume`, the Scheduler's budgets, and the Context option
  `resume_pause_at_microstep_start:`). napi's resume stays plumbing-owned
  (napi ADR 0007); its generated bindings are regenerated, and its pump
  passes false until its debug adapter chooses the member.

## Implementation notes

2026-09-26.

- **Where it lives.** `TA_PHASE_START` in `src/continuation.h`.
  `ta_microstep_begin()` sets it; `ta_microstep()` pauses there when
  `pause_at_start` is set and the actor is bootstrapped, and otherwise
  passes it with `ta_microstep_pass_start()`. `ta_interpreter_resume()`
  passes a start it resumed from after writing `RESUMED`, so the record
  carries the start's position. `ta_exit_begin()` no longer sets the exit
  position.
- **The position.** `ta_run_view` carries the exit, entry and
  default-entry sets and the default-history map, read from scratch for
  records and from the continuation for inspection. `ta_run_position()`
  hands a start to `ta_start_position()`, which walks the sets as
  `ta_exit_states()` and `ta_enter_states()` do, without running anything,
  and maps each candidate through `ta_phase_position()` so the phase,
  state and transition are reported as for any other pause.
  `ta_enabled_actions()` gives an entry of `enabled` its content, the
  bootstrap and operator marks included.
- **Tests.** `tests/microstep_start_test.c`: a stop before the first unit
  of an `<onentry>`, an `<onexit>` (also of a parent whose child, exited
  first, has none), an initial transition's content, a
  history default's content, transition content behind a guard (its
  `CONDITION` written, the unit not run), and an operator transition's
  entry; microsteps with no content, a chart's and an operator's; at each,
  inspection equals the `PAUSED` record, the configuration and log are
  unchanged, delivery and abandoning are busy, and one step runs the
  first unit alone. The flag changes no log and no configuration at
  action budgets 0 to 4 and pauses once per microstep at 0; without it,
  action budget 1 pauses only before the second unit of the one block
  that has two, as ADR 0061 does. A microstep budget of 1 from a
  start finishes the microstep and stands between microsteps. An actor
  captured at a start and reconstructed in the same system and in another
  stands where it stood and finishes the same way. Every allocation of a
  run that pauses at each start and before each unit fails in turn, with
  the same log and configurations. Scenarios: `pausing.rb` (a stop before
  the nested chart's first `<onexit>` action, a step from it, the next
  microstep's start at its transition content, and two equivalence
  variants), `operator.rb` (a jump stopped at its start, across unload
  and load, and an equivalence variant), `out_of_memory.rb` (the phases
  chart's allocation sweep with every start paused and action budget 1,
  on jq and ECMAScript).
