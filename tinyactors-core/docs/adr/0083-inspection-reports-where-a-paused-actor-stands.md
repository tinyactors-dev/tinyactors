# ADR 0083: Inspection reports where a paused actor stands

**Status:** Accepted and implemented (2026-09-26). Applies ADR 0060, ADR 0061, ADR 0067, ADR 0070 and ADR 0071; refines ADR 0067 section 3 (what the position of PAUSED, RESUMED and STOPPED names). Section 2 refined by ADR 0086 (at a microstep's start, the position is its first unit).
**Date:** 2026-09-26

## Context

A debugger stops an actor with `action_budget` (ADR 0061) and shows where
it stopped: the action, the `<foreach>` iteration, the block (`<onexit>` of
S, content of T, `<onentry>` of S), the microstep and the macrostep. Today
a host learns that only from the `PAUSED` trace record (ADR 0067), whose
`key` and `position` say it. A host that attaches to an actor that is
already paused has no record: it was written before anyone was watching,
or to a sink that did not want it. `ta_actor_inspection` says only that a
macrostep, and perhaps a microstep, is in progress.

The information is on the actor. A run that stops inside a phase moves
its position and frames into the actor's continuation (ADR 0061 section 3,
ADR 0070): the phase, the state, the stage, the handler, the enabled
transitions, and the executor's frames (ADR 0060), each open block with
its cursor and each running `<foreach>` with its iteration. The step
counters are on the actor (ADR 0067 section 3). Nothing needs to be
added to an actor; inspection needs to read what is there.

Reading the `PAUSED` record's position from the code showed that it said
less than it seemed to. `position.action` was the interpreter's
`trace_action`, the unit running now, but every block of a microstep runs
through `ta_run_block_isolated()`, which clears it when the block returns,
pause included, so a `PAUSED` record inside a microstep always named no
action. `iteration` was the innermost loop's current iteration, which
before a `<foreach>`'s next iteration is the iteration that just
finished. The phase and state were right.

## Decision

### 1. The inspection view carries the step and the position

`ta_actor_inspection` gains two members:

```c
ta_step_key step;
ta_trace_position position;
```

**While `macrostep_in_progress` is true** they are exactly the `key` and
`position` of the `PAUSED` or `STOPPED` record the actor's last run wrote,
whether or not a sink was installed or wanted that kind. `RESUMED`, which
the next run writes, carries the same two. A macrostep that has not begun
(an event was delivered and no run has taken it, or a run stopped before
its selection committed) writes no control record; then `step` is
`(session_id, 0, 0)` and `position` is nowhere, as below.

**Outside a macrostep**, idle or finished, `step` is `(session_id, 0, 0)`,
the key of lifecycle records, and `position` is nowhere: `at` is
`TA_TRACE_AT_NONE`, `state`, `transition` and `action` are their `NONE`
values, and `iteration` is 0.

`step` is included beside `position` because a record is identified by
both, and a stack trace needs the microstep and macrostep frames as much as
the action.

### 2. What the position names

The position of a stopped run names **where it stands: the unit it runs
next**, which is also the first unit a single step (`action_budget: 1`)
runs.

**Paused between microsteps** (the microstep `budget` ran out, ADR 0009):
nothing is open. `position` is nowhere, and `step.microstep` counts the
microsteps of the macrostep that completed. The next thing the run does
is select transitions.

**Paused inside a microstep** (`action_budget` ran out, ADR 0061): the
phase is `EXIT`, `TRANSITIONS` or `ENTER`, and the frames hold an open
block, because a pause happens only before a unit inside one.

| Continuation | `at` | `state` | `transition` |
|---|---|---|---|
| `EXIT` | `EXIT` | the state whose `<onexit>` runs | `NONE` |
| `TRANSITIONS` | `TRANSITION` | the transition's source | the transition whose content runs |
| `ENTER` | `ENTER` | the state whose `<onentry>`, initial or history content runs | `NONE` |

`action` and `iteration` come from the top frame:

- **An open block** (`ACTIONS` frame): `action` is the next action of the
  block, the one its cursor names. `iteration` is the iteration, from 0,
  of the innermost `<foreach>` below it on the stack, whose body the block
  is, or 0 when there is none.
- **A `<foreach>` whose next iteration is next** (`FOREACH` frame on top):
  `action` is the `<foreach>` and `iteration` is the iteration it binds
  next. That is the `position` of the `FOREACH` record the run then writes.

`stage` and `handler` are not reported. They are recoverable: the
action's place is in `ta_definition_action()` (ADR 0071), whose `block`,
`owner` and `block_index` lead from any action out through `<if>` and
`<foreach>` to the `<onexit>`, `<onentry>` or transition it sits in. The
position does not need to say whether `<onentry>`, the initial transition
or the history default is running; the action says it.

**Stopped by an allocation failure** (ADR 0070, a `STOPPED` record): the
same rules apply to the phases a stop can leave, with the phase mapping
ADR 0067 already uses for records (`SELECT`, `BOOTSTRAP`, `FINALIZE`,
`INVOKE`, and `FINISH` as `EXIT`). A stop between blocks has no open
block, and `action` is `NONE`. When a failed unit left its block open,
`action` is that unit, which the retry runs.

### 3. The control records use the same rule

`PAUSED`, `RESUMED` and `STOPPED` records now carry this position,
computed by one function from the interpreter's scratch, the same data the
continuation is copied from. Every other record keeps its position: the
unit that wrote it. So a sink sees `PAUSED` at the unit the next record
comes from, and `RESUMED` at the same place.

Inspection computes the position from the continuation with the same
function. The two cannot drift: they read the same fields, before and
after `ta_stop()` copies them.

### 4. Cost

Inspection reads the continuation's header and its top frames: O(frames),
bounded by the definition's nesting depth, inside the O(size of the actor)
ADR 0054 allows. It never calls the data-model adapter. Nothing new is
stored per actor, per event, per pending send or in a session image, and
nothing allocates. `ta_actor_inspection`, a host-side value, grows from
168 to 216 bytes: a `ta_step_key` (24) and a `ta_trace_position` (20),
with padding.

## Consequences

- A debugger that attaches to a paused actor reads where it stands with
  one inspection, the same answer a `PAUSED` record would have given, and
  after a session image is reconstructed (ADR 0061 section 4), because the
  continuation is in the image.
- A `PAUSED` record's position now names an action and the right
  iteration inside a microstep. No test depended on the old value; the
  trace sweeps compare records without the control kinds.
- The bindings in both hosts gain the two members from the generator.

## Open points

1. **The same position twice.** A `<foreach>` action that has not run yet
   and the same loop before its iteration *i* are both named (`<foreach>`,
   *i*) when the enclosing iteration is also *i*, most often 0: stepping
   with `action_budget: 1` stops twice at what looks like one place. The
   remedy is a listing of the open loops (below).
2. **Only the innermost iteration.** A stack trace wants every enclosing
   `<foreach>` iteration, and its count. The frames hold both. A function
   that lists the open loops of a stopped run, outermost first, like
   `ta_actor_active_states()` lists states, would give them and resolve
   point 1; it is left until a host needs it.

## Implementation notes

- `ta_run_position()` in `src/interpreter.c` computes the position of a
  stopped run from a `ta_run_view` (the position, the enabled transitions
  and the frames). The interpreter builds the view from scratch for the
  control records; `ta_actor_position()` builds it from the continuation
  for inspection. `ta_actor_step()` gives the key, which records use too.
- **Tests.** `tests/pause_test.c` installs a sink that keeps the last
  control record and, after every resume of the equivalence chart (nested
  `<if>` and `<foreach>` in `<onexit>`, transition content and
  `<onentry>`) at every action budget from 1 to 80, checks that the
  inspected step and position equal the `PAUSED` record's, that `RESUMED`
  repeats them, and that an actor outside a macrostep stands nowhere. At
  budget 1 it checks what each position names against
  `ta_definition_action()`, pauses between microsteps with a microstep
  budget of 1, and checks that a reconstructed image stands where the
  captured actor stood. `pausing.rb` compares inspection with the `PAUSED`
  records in the test host, and `out_of_memory.rb` with the `STOPPED`
  record of an actor stopped inside a microstep, before and after its
  image is reconstructed.
