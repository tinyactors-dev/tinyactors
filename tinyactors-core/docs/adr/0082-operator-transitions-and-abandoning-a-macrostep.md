# ADR 0082: An operator transition is a transition, and a macrostep that never ends can be abandoned

**Status:** Accepted and implemented (2026-09-26). Narrows ADR 0030; applies ADR 0009, ADR 0047, ADR 0055, ADR 0061, ADR 0067, ADR 0069 and ADR 0070; abandons a macrostep ADR 0075 stopped as a runaway (section 6). See the implementation notes.
**Date:** 2026-09-26

## Context

An actor is wedged at 3am. Its chart waits in a state for an event that
will never come, or its eventless transitions cycle forever, so the
macrostep never becomes stable and the actor answers `TA_ERR_BUSY` to
everything. The operator needs to move it out of the state it is stuck
in. The chart's author did not prepare admin transitions for that, and
nobody can redeploy the chart in the middle of the night.

The debug adapter proposal
([Unwedging without foresight](../../../docs/proposals/debug-adapter.md#unwedging-without-foresight))
asks core for two operations that work on any chart:

- a jump: move a live actor to states the operator names;
- abandoning a macrostep that never ends, so that a jump is possible at
  all.

ADR 0030 says that "state configuration changes occur only through the
interpreter's transition semantics. There is no public `goto_state()`
escape hatch." The reason it gives is that a configuration written from
outside makes ordering, history, parallel exit and entry, inspection and
verification incoherent. A `goto_state()` that writes the configuration
would skip `<onexit>` and `<onentry>`, leave history unrecorded, leave
invocations of exited states running and never start those of entered
states, and could produce a configuration no SCXML transition can: two
children of one compound state, or a parallel state with a region
missing.

What ADR 0030 does not rule out is a transition that core builds at run
time and takes through the same engine. An admin transition the author
had written would have done exactly that. The engine does not care who
wrote the transition; it cares that the transition has a source, targets
and a type, and it computes the rest.

A macrostep that never ends leaves its actor in a state ADR 0061
section 2 already describes as legal: between two microsteps, the
configuration the actor reports is the one after its last completed
microstep, "a legal configuration (SCXML 3.11)". What makes it
impossible to use is that the actor is busy, forever, with the event that
started the macrostep.

## Decision

### 1. An operator transition rides on delivery

`ta_actor_deliver_params` gains one optional member:

```c
typedef struct {
    ta_state_id source;    /* an active state; TA_STATE_NONE: the root */
    TA_COUNTED_BY(target_count) const ta_state_id *targets;
    uint32_t target_count;
} ta_operator_transition;

typedef struct {
    ta_actor_id actor;
    TA_IN const ta_event *event;
    ta_trace_parent trace;
    TA_IN TA_OPTIONAL const ta_operator_transition *transition;
} ta_actor_deliver_params;
```

When `transition` is present, the delivered event is taken by this
transition instead of by transition selection. No chart transition sees
the event.

- **The transition** is internal, has no guard and no executable content.
  Its `source` is an active state or the root. A region of a `<parallel>`
  as the source moves that region only: an internal transition whose
  targets are all its descendants exits nothing outside it. Its
  `targets` are any states of the definition, history states included
  ("put it back where it was") and a top-level `<final>`, which ends the
  session with its `<donedata>` and completion as a chart transition to
  it would.
- **The normal engine does the rest:** the exit set, `<onexit>`,
  recording history, cancelling the exited states' invocations, the entry
  set, `<onentry>`, default initial states and history defaults, late
  data, `done.state` events, and starting the entered states'
  invocations when the macrostep ends. Eventless transitions and the
  internal queue then run to a stable configuration, as after any
  transition. The actor cannot end up in a configuration the engine could
  not produce.
- **`_event`** in every handler of the operator's microstep, and after
  it, is the delivered event, as for any external event. A host names it
  as it likes, for example `operator.goto`, and gives it data and an
  origin, so an `<onentry>` can tell a jump from a normal entry.

Riding on delivery reuses what delivery already is: admission
(ADR 0055), the trace parent (ADR 0067), the budgets (ADR 0009,
ADR 0061), running out of memory (ADR 0070), and `_event`. A separate
function would repeat all of it. The rule that an API changes in place
also says one shape: deliver with or without a transition.

### 2. What deliver refuses

An operator transition is a caller's input, so a malformed one is
`TA_ERR_INVALID` with nothing retained and the event untouched, as for
deliver's other refusals:

- no targets, or a null target list;
- a target or source that is not a state of the definition;
- targets that cannot form a legal configuration: two targets where one
  is the other or its ancestor, or two targets whose nearest common
  ancestor is not a `<parallel>` (the root counts as compound). A history
  state stands for its parent here, the region it restores; so a history
  state and a state below its parent conflict, and so do a shallow and a
  deep history of one state;
- a source that is not active, a history state included, since history
  states are never active.

Deliver checks the event and the transition's shape first, then whether
the actor is finished (`TA_ERR_DONE`) or busy (`TA_ERR_BUSY`), then
whether the source is active. A malformed transition is a caller error
whatever the actor's state (ADR 0055's rule for a malformed event).
Whether the source is active is a question only an actor that would
otherwise accept the event can answer: a busy actor's configuration is
about to change.

The operator transition is copied into one allocation the event block
owns. Deliver can run out of memory for it, which is `TA_ERR_NOMEM` with
nothing retained, the answer ADR 0069 already gives for the event block.

### 3. It is the first microstep of its macrostep

The transition was checked against the actor's configuration when it was
delivered, and the actor was idle then. For that check to hold when it
runs, the operator's event is taken as the first step of the macrostep,
before anything else can change the configuration:

- before eventless transitions, as for any delivered event already
  (ADR 0067's implementation notes: the last macrostep ended stable);
- before internal events queued after the delivery and before the
  resume, such as an `error.communication` of a delayed send that failed
  in between. For a plain event SCXML takes those first; for an operator
  transition that order would run it from a configuration it was not
  checked against. The operator's jump happens where the operator saw
  the actor, and those events are processed after it.

`<finalize>` and autoforwarding do not run for the operator's event. They
are for events the chart and its invoked services exchange, and no chart
transition sees this one.

Taking the event cannot fail: selecting nothing and beginning a
microstep allocate nothing, so there is nothing to give back (ADR 0070),
and the take commits the macrostep.

### 4. Pauses, continuations and session images

The operator's microstep is an ordinary one. `action_budget` pauses it
between units of its handlers (ADR 0061), and running out of memory stops
it in any of its phases (ADR 0070).

The continuation stores the enabled transitions by compiled ID. The
operator transition has none, and it needs none once its microstep has
begun: the exit set, the entry set, the default-entry set and the
default-history map are computed when the microstep begins (ADR 0067's
implementation notes) and stored in the continuation; the only thing
read from the enabled transitions afterwards is their executable
content, and this transition has none. So `enabled` holds a mark,
`TA_ENABLED_OPERATOR`, beside the `TA_ENABLED_BOOTSTRAP` mark the
bootstrap microstep already uses, and a continuation, a session image of
one, and a reconstruction carry the mark alone.

Between delivery and the take, the operator transition waits in the
event block next to the external event, as the event's trace parent does
(ADR 0069). A session image captured then carries a copy in its arena,
and loading or reconstructing the image gives the new actor its own copy.
The image restores only against the revision it was captured from
(ADR 0038), so its state IDs mean the same states.

### 5. Tracing

The operator's microstep writes one `MICROSTEP` record, within
ADR 0067's record shape:

- `transitions` is empty, as for the bootstrap microstep;
- `state` is the source, `TA_STATE_NONE` for the root;
- `states` are the targets the operator named, in the order given,
  history states included; `exited`, `entered` and `history` are as for
  any microstep.

A chart's `MICROSTEP` never carries `states`, so a sink tells a jump
from a chart transition by `state_count` being non-zero. Its `event` is
the delivered one, and `MACROSTEP_BEGIN` names it too, so "who jumped
where" is in the trace.

### 6. Abandoning a macrostep

```c
TA_API ta_result ta_actor_abandon(ta_system *system, ta_actor_id actor);
```

It abandons the actor's macrostep. The macrostep's external event, its
internal queue (the waiting internal events are released) and what
`_event` names are discarded as when a macrostep ends (ADR 0069: an
invoked session keeps its last event for its `<onexit>`). The actor is
idle with the configuration after its last completed microstep, which
ADR 0061 section 2 already makes a legal one. Effects already released
stay released: its sends, its invocations, its data, its history.
Invocations of states the abandoned macrostep entered, which were to
start when it ended, start when the next macrostep ends, if their states
are still active then.

- **Between microsteps only.** An actor paused or stopped inside a step
  (`microstep_in_progress`, or a run that ran out of memory in
  `<finalize>`, selection or starting invocations) answers `TA_ERR_BUSY`.
  Entering states started invocations and recorded history that the
  stored configuration does not reflect, and a half-started batch of
  invocations would start again. The host first resumes with `budget` 1
  and `action_budget` 0, which finishes that step and pauses before the
  next microstep, then abandons.
- **After the bootstrap's microstep.** An actor whose first microstep has
  not completed has no configuration to keep, and answers `TA_ERR_BUSY`
  too; resuming it finishes the bootstrap.
- **An actor with no macrostep in progress** answers `TA_ERR_INVALID`:
  there is nothing to abandon, and a host that asked is confused about
  the actor's state. `TA_OK` would hide that. A finished actor answers
  `TA_ERR_DONE` and an unknown one `TA_ERR_NOT_FOUND`, as delivery does.
- **An event delivered and not yet taken** is a macrostep in progress
  (`macrostep_in_progress` is true), and abandoning drops it, with its
  operator transition if it had one. That lets a host take back a jump it
  delivered by mistake.

Abandoning needs the interpreter's scratch memory to write its records;
without it, it answers `TA_ERR_NOMEM` and changes nothing.

`TA_TRACE_ABANDONED`, a new kind at the end of the enum, after ADR
0080's definition kinds, so no kind's number changes, records it: `event` is the macrostep's external event,
taken or waiting, or none; `dropped` is the number of internal events
discarded. `ta_trace_record` gains `dropped` for it. When the macrostep
had begun (it wrote `MACROSTEP_BEGIN`), `MACROSTEP_END` follows with the
configuration the actor keeps, so a sink that opens a span at `BEGIN`
closes it as for any macrostep. A macrostep that never began writes
`ABANDONED` alone, with the key of the session outside any macrostep.

On a live actor this is the one case where the host takes an actor out
of scheduling inside a macrostep. That is safe for the reason the
proposal gives: the macrostep never ends, so no waiting caller would ever
have seen it finish.

**A runaway is abandoned like any other macrostep that never ends.**
ADR 0075 stops a macrostep at its microstep limit with `TA_ERR_RUNAWAY`,
between two microsteps, "as after a budget pause": nothing is open, the
macrostep has begun, and deliver answers `TA_ERR_BUSY`. That is exactly
the position abandoning accepts, so no new rule is needed:

- `ta_actor_abandon()` ends the runaway macrostep, with `ABANDONED` and
  `MACROSTEP_END` after ADR 0075's `RUNAWAY` record, and the actor keeps
  the configuration of its last completed microstep, somewhere inside its
  loop;
- a jump is refused as busy until then, and taken after, which is how an
  operator moves a runaway out of its loop instead of releasing it with
  `no_microstep_limit` (ADR 0075 section 3), which only lets the loop go
  on;
- the limit is a property of each resume, not of the actor, so the next
  macrostep, the jump's included, is limited again.

A debugger shows a runaway as a stop of its own: the actor is between two
microsteps of a macrostep it cannot finish, and the choices are the ones
above (abandon, then jump; or release). How each host names the stop is
its own decision.

### 7. ADR 0030 is narrowed, not replaced

ADR 0030's rule stands: the configuration changes only through the
interpreter's transition semantics, and there is no `goto_state()`. An
operator transition is not an exception to it. It is a transition, taken
by the same engine, with the same exits, entries, history and
invocations, which only its author differs from. Abandoning changes no
configuration at all: the actor keeps the one after its last completed
microstep.

## Alternatives considered

- **`ta_actor_goto(system, actor, targets)`**, a separate function that
  runs a transition at once. It would duplicate admission, budgets,
  pausing, out-of-memory stops and tracing, and it would need its own
  event for `_event`, where an `<onentry>` that reads `_event.name` would
  otherwise see the previous event.
- **Writing the configuration.** What ADR 0030 rejects, for the reasons
  it gives.
- **Admin transitions compiled into every chart.** Foresight again, and
  they would appear in every export, coverage report and verification.
- **Checking the source when the event is taken, not when it is
  delivered.** The take would then need an answer for a source that is
  no longer active, and none is good: ignoring the event loses the
  operator's request silently, and falling back to the root jumps
  further than asked. Taking it first keeps deliver's answer true.
- **Carrying the operator transition's targets in the continuation.**
  Nothing reads them after the microstep has begun, so the continuation
  would grow for no reader; the mark is enough.
- **Abandoning inside a microstep**, rolling back to the stored
  configuration. The states already entered have started invocations and
  late data, and those already exited have recorded history and
  cancelled theirs; undoing that is not something the engine can do, and
  the stored configuration would not describe the actor.
- **`TA_OK` for abandoning an idle actor.** It would make a host that
  lost track of its actor look right.

## Consequences

- An operator can move any actor of any chart, through the engine, and
  unwedge a macrostep that never ends. The debug adapter's `goto` and
  `abandon` build on these two calls.
- The chart cannot refuse a jump; that is the point. An author's own
  assumptions, such as data matching the state, can break, which is why
  the adapter offers a fork first and records who jumped where.
- Costs, measured on x86-64 Linux with clang:

  | | Before | After |
  |---|---|---|
  | `sizeof(ta_actor)` | 232 | 232 |
  | `ta_actor_deliver_params` | 64 | 72 |
  | `ta_trace_record` | 368 | 376 |
  | event block header (`offsetof(ta_event_block, events)`) | 200 | 208 |
  | event block with room for 8 events | 776 | 784 |
  | an operator transition waiting to be taken | – | one allocation of 8 + 4 per target bytes, freed when its microstep begins |
  | continuation, session image | – | unchanged, apart from the operator transition's copy while one waits |

  An actor that never receives a jump pays the 8 bytes of the event block
  pointer only while it holds a block, and nothing when idle. Allocations
  per delivery without a transition: unchanged.
- `ta_trace_kind` gains `TA_TRACE_ABANDONED`, after ADR 0080's
  `TA_TRACE_DEFINITION_FREED`; sinks that name kinds by index grow one
  name (napi's two tables, core's `trace_test`, the test host's
  `trace_line.rb`), and napi's OTLP encoder, which skips the definition
  kinds (ADR 0080), now skips that range rather than every kind after
  `TA_TRACE_DEFINITION_REGISTERED`. `ta_trace_record` gains `dropped`.
- Bindings: the test host binds `ta_actor_abandon` and the new member of
  `ta_actor_deliver_params`. napi binds `ta_actor_abandon` as generated;
  delivery stays plumbing-owned there, so an operator transition from
  JavaScript waits for the facade work that owns delivery.

## Implementation notes

2026-09-26.

- **Where it lives.** `ta_operator_jump` (`src/actor.h`) is the copy in
  the event block's `slots.jump`. `ta_actor_deliver()` checks its shape
  with `ta_interpreter_jump_valid()` and its source with
  `ta_actor_state_active()`, then allocates it before taking the event
  block, and frees it if the block cannot be had. `ta_take_jump()` in
  `src/interpreter.c` binds the event, begins the macrostep and the
  microstep with `TA_ENABLED_OPERATOR` in `enabled` and `it->jump` set,
  and frees the copy; `ta_next_step()` calls it before anything else when
  a waiting external event has one and the macrostep has not begun.
  `ta_release_external()` frees it wherever an external event is released
  without being taken.
- **Targets are lists, not spans.** The engine read a transition's
  targets as a span of the definition's target array. The view, the
  entry frames and the effective-target walk now take a `ta_state_list`
  (a pointer and a count), which a definition's span and an operator
  transition both give. The walk and its order are unchanged.
- **Abandon** (`ta_interpreter_abandon()`) starts an interpreter over the
  actor only for its scratch: the `ABANDONED` and `MACROSTEP_END` records
  and the active set. It holds no continuation, since one is refused as
  busy. It truncates the queue to the taken head, ends the macrostep's
  events as `ta_next_step()` does, marks the actor idle, touches its
  epoch, compacts its sparse state and settles its block.
- **Counters** (ADR 0072): the operator's event counts in
  `events_taken`, its microstep in `microsteps`, its macrostep in
  `macrosteps`. Abandoning counts nothing of its own; an
  `abandoned` counter can be added when a host asks for one.
- **Tests.** `tests/operator_test.c`: a jump from the root, the history
  target restoring a deep history, a region of a `<parallel>` as the
  source and a target in one region, a top-level final target finishing
  the actor, each refusal with nothing retained, busy and done, a pause
  before every unit of the jump's `<onentry>` with a trace identical to
  the unpaused run, capture and reconstruction at every such pause and
  between delivery and the take, abandoning a macrostep of eventless
  transitions that never ends (records, dropped count, legal
  configuration, a jump afterwards), abandoning refused inside a microstep
  and before the bootstrap's microstep, abandoning an event not yet taken
  and a jump not yet taken, a macrostep stopped as a runaway (ADR 0075)
  abandoned and then jumped out of its loop, with the next loop stopped
  at the limit again, and every allocation of a paused jump failing
  in turn, with the same log, configuration and records as the run that
  failed nothing. The test host's scenarios are in
  `tinyactors-test/ruby/scenarios/operator.rb`.
- **Not tested:** the jump taken before an internal event queued between
  delivery and the resume (section 3). Queueing one needs a delayed send
  that fails while the actor holds the delivered event, which no current
  test instrument arranges.
