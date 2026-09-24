# ADR 0070: A run that runs out of memory stops where it stands

**Status:** Accepted and implemented; refines ADR 0047, ADR 0061 (section 3 superseded: saving a pause cannot fail) and ADR 0069 (its open point on transition selection is closed). Adds `TA_OP_NOMEM` to the plugin contract. See the implementation notes. Open point 7 fixed in napi by 9ff6637 (a failed rendering throws and fails the call as `error.execution`; open point 1 remains).
**Date:** 2026-09-24

## Context

ADR 0047 decides what `TA_ERR_NOMEM` from `ta_actor_resume()` means: the
macrostep is suspended, not ended, "exactly as a `TA_RUN_PAUSED` return
leaves it, at the last interpreter boundary before the allocation that
failed", and a later resume continues it to the outcome it would have had.
It puts an obligation on the implementation: an allocation that can fail
happens before the microstep it serves becomes visible, or is undone
before the error is returned.

Core met that obligation only at the start of a microstep. ADR 0064
reserved room for one internal event and the actor's sparse state before
the microstep became visible. Any allocation after that point broke the
rule, and `ta_microstep` then ran into its normal ending:

```c
it->at = (ta_micro_position){0};   /* forget where it was */
ta_store_active(it);               /* store the half-updated active set */
it->steps += 1u;
```

The states already exited stayed exited, the entry never happened, and the
next resume started a new macrostep from that configuration. When every
active state had been exited, the configuration was empty, which ADR 0010
forbids. Allocations that could fail there:

- a second `<raise>`, or a second error, in one microstep: the reservation
  held one event;
- a delayed `<send>` growing its owner's storage (ADR 0054);
- interning an `eventexpr`, `typeexpr` or target, and scratch for `<param>`s,
  which were reported as `error.execution` instead;
- `done.state` events and a top-level final state's completion;
- recording an invocation, and holding a send or a `<log>` for an
  unpublished actor.

The same held outside a microstep. The bootstrap marked the actor
bootstrapped even when it failed half way. `<finalize>`, autoforwarding,
starting the macrostep's invocations and ending the session ran to their
end or failed half way, with nothing to continue from. A failed selection
kept the event it had taken (the open point of ADR 0069).

Two more gaps showed once a test failed every allocation in turn:

- **Saving a pause can fail.** ADR 0061 section 3 answered that with "a
  pause that cannot save does not pause". For running out of memory there
  is no such way out: the run must stop, so saving must not allocate.
- **Plugins cannot say they ran out of memory.** `ta_op_status` had OK and
  ERROR. Core itself hands a data model `TA_ERR_NOMEM` when it cannot
  render a system variable (`ta_dm_host_text`, ADR 0068), and the jq data
  model read the variable as null, while the ECMAScript data model threw,
  which the chart saw as `error.execution`. An invoker whose child could
  not be spawned had to answer ERROR as well.

## Decision

### 1. Running out of memory stops the run where it stands

A run is a sequence of steps. `ta_micro_position` says where a run stands
between two of them, in one of these phases (`ta_run_phase`):

| Phase | What runs | Where it resumes |
|---|---|---|
| `EXIT`, `TRANSITIONS`, `ENTER` | the microstep (ADR 0061) | the state, stage, handler and open block |
| `DATA` | the bootstrap's declarations, then its initializers | the declaration; the owner state |
| `SCRIPT` | the bootstrap's top-level `<script>` | the open block |
| `FINALIZE` | `<finalize>` for an external event just taken, then autoforwarding | the open block; the invocation |
| `SELECT` | selecting transitions for that event | the start of the selection |
| `INVOKES` | starting the invocations of the states the macrostep entered | the state and the `<invoke>` |
| `FINISH` | exiting every state at a top-level final state, then ending the session | the state and handler; the end |

When a step runs out of memory, the run saves its position, sets and frames
into a continuation the actor owns, exactly as a pause does, and
`ta_actor_resume()` answers `TA_ERR_NOMEM` with `TA_RUN_ERROR`. The next
resume continues from there. The actor stays in its macrostep:
`ta_actor_inspect()` reports `macrostep_in_progress`, and
`microstep_in_progress` when the phase is one of the microstep's;
`ta_actor_deliver()` answers `TA_ERR_BUSY` (or `TA_ERR_DONE` once it has
reached a top-level final state); the configuration it reports is the one
after its last completed microstep. The continuation travels in a session
image (ADR 0061 section 4), so an actor stopped this way can be captured,
unloaded, loaded or reconstructed, and the retry can happen in the new
actor.

Three places have nothing to continue, and give back instead: the
selection of eventless transitions, the selection for an event just taken
(the event goes back to its slot or the queue, closing the open point of
ADR 0069), and the reservation at the start of a run (section 2). The
next run does them again.

### 2. Stopping never allocates

Each system keeps one spare continuation and one spare sparse state.
`ta_actor_resume()` reserves both before it runs anything: a continuation
at least as large as the running definition can need (every state set,
the bound data declarations, every enabled transition, and every frame the
executor can open, ADR 0060), and a sparse state for an actor that has
none, to hang the continuation from. An actor that resumes from a
continuation stops into that one again. So a run can stop at most once
and always has what stopping takes. When the reservation itself fails,
the run has done nothing.

The spare is replaced when a run takes it. A continuation whose run went
on to the end goes back to the system as the spare, which keeps the larger
of the two.

A pause uses the same reservation, so saving a pause cannot fail. ADR 0061
section 3's rule that "a pause that cannot save does not pause" no longer
applies, and the budgets are exact again.

### 3. What a step does when it runs out of memory

The rule: **a step either completes, or it had no effect that doing it
again would repeat.** A retry does the step again from its start. Each
kind of step meets the rule one of two ways: it **reserves** what it will
need before it has any effect, or it is **done again**, which is safe
because what it did before was evaluation or is idempotent.

| Step | Reserves before it runs | Done again, and why that is safe |
|---|---|---|
| A unit of executable content (an action, a `<foreach>` iteration) | room for one internal event, its own or the error it raises; for a published actor's `<send>` with a delay, room to store it | a unit that runs out of memory evaluating (interning a name, taking scratch) or committing (holding for an unpublished actor, keeping a send the router refused with RETRY, which accepted nothing) has changed nothing but idempotent state |
| A `<send>`'s generated ID, an `<invoke>`'s generated ID | – | the counters move only once the step has an outcome, so `idlocation` receives the same text again |
| Activating a state | recording its invocations (the one allocation), before the state is marked active | – |
| Late data (ADR 0003 binding) | – | per declaration: a declaration whose error could not be queued failed, so its initializer runs again with no effect repeated |
| `<donedata>`, `done.state`, a completion | the sparse state that holds a completion | the events it queued are taken back; `<donedata>` is evaluated again |
| Exiting a state | – (it allocates nothing) | – |
| Selecting transitions | – | guards are evaluated again, and the errors they queued are taken back |
| Autoforwarding | – | per invocation: a router RETRY accepted nothing |
| Starting an invocation | the sparse state, a record, room for its error | per `<invoke>`: evaluation only, and the same generated ID |
| Bootstrap data | – | per declaration, as late data; which declarations the binding supplies is part of the continuation |
| Ending the session | – | cancelling what is already cancelled and releasing what is released do nothing; `done.invoke` is offered again |

Between two actions, the choice is a boundary where the step is **done
again** wherever the step's effect can be taken back or is idempotent
(events queued are truncated to a mark; counters move last), because a
reservation would have to cover the worst case, such as every guard of a
selection or every `<param>` of a `<donedata>`, on every step. It is a
**reservation** where the step would otherwise change something a second
run repeats: the active set before an invocation is recorded, and the
executable content before its error is queued (a `<script>` that changed
the data model and then failed must not run twice).

`ta_prepare_microstep_queue`, the reservation of ADR 0064 before every
microstep, is gone: each step reserves what it needs.

### 4. A plugin can say it ran out of memory

`ta_op_status` gains `TA_OP_NOMEM`: the operation could not get memory and
changed nothing. Core treats it as its own failed allocation: the step
stops, and the operation is called again when the host resumes the actor.
An operation that may have changed something before it ran out answers
`TA_OP_ERROR`, and the chart sees the error SCXML defines.

- A data model answers it for any operation (evaluation, assignment,
  `<script>`, `<foreach>`, declarations, `<param>`s).
- An invoker's `start()` answers it when nothing started.
- An I/O processor's `send()` answering it is treated as a router's RETRY:
  the send waits in the deadline index, due now (ADR 0040).

A router that sees `TA_ERR_NOMEM` from `ta_actor_deliver()` should answer
RETRY, not ERROR, so that running out of memory in the receiver is not a
communication error in the sender. The test host's routers do.

### 5. What a retry repeats

A retry evaluates expressions again: the guards of a selection, and the
attributes of a unit that ran out of memory after evaluating them. SCXML
expects conditions to have no side effects, and a data model whose
expressions cannot have any (jq) repeats nothing visible. An ECMAScript
expression with side effects runs twice in that case. Reserving before
each unit keeps the case narrow: it needs a unit that runs out of memory
after evaluating, which only interning a name or target, taking scratch
for `<param>`s, holding an unpublished actor's send or `<log>`, or keeping
an immediate send the router refused can do.

## Consequences

- `ta_actor_resume()` may answer `TA_ERR_NOMEM` in any phase. A host treats
  it as ADR 0047 says: free or grow memory, then resume.
- Per system: one spare continuation, from 248 bytes (3 states, one level
  of blocks) to about 1.3 KB (100 states, four levels), and one sparse
  state of 176 bytes. They are allocated by the first resume and replaced
  only after a run took them.
- Per actor: nothing. `sizeof(ta_actor)` stays 224, and
  `sizeof(ta_continuation)` stays 48; its layout gains the bound data
  declarations, one word per 64 declarations plus one.
- Per resume, in the steady state: no allocation. The reservation is a
  comparison when the spare is there.
- Per unit of executable content: a comparison, and a queue that grows
  one unit earlier than before when a unit exactly fills it. A microstep
  without executable content no longer grows the queue: ADR 0069's
  deviation (a header-only block grows as soon as its actor runs) now
  happens only when content runs.
- `ta_op_status` has a third value. Plugins that never answer it are
  unaffected; the test host's jq data model and SCXML invoker answer it.
- Session images of a stopped actor carry its continuation, as images of a
  paused one did (ADR 0061). The continuation's allocation may be larger
  than its shape needs, since it is the system's spare.

## Open points

1. **The ECMAScript data model cannot say it changed nothing.** A
   JavaScript expression may have changed the session before QuickJS ran
   out of memory, or before core could not render a system variable, so
   the adapter keeps answering `TA_OP_ERROR`, and the chart sees
   `error.execution`. The scenario "a system variable core could not render
   stops the run (ecmascript)" is marked as an expected failure naming this
   point.
2. **libjq aborts when its own allocation fails.** The jq data model
   reports what core fails to allocate for it, but `jv_mem_alloc()` ends
   the process on failure, so a jq data model is only as tolerant as the
   process's `malloc`.
3. **An ECMAScript expression with side effects runs twice** when the unit
   that evaluated it stops afterwards (section 5).
4. **Exiting a cancelled invoked session does not stop.** It runs inside
   the parent's step (ADR 0060), and running out of memory there still
   ends that session's exit at the state it was exiting, as before.
5. **Autoforwarding resumes by index.** An invocation the host reports as
   finished while the actor is stopped shifts the indices, so the
   continued forward can skip or repeat one invocation.
6. **A receiver out of memory can reorder a sender's events.** When a
   router answers RETRY because the receiver could not take an event, a
   later event from the same sender can reach the receiver first. Across
   actors the host decides the order (`docs/internal/event-ordering.md`);
   core keeps no per-receiver order for sends waiting in the deadline
   index.
7. **The napi JavaScript data model reads a failed rendering as
   undefined.** `tna_host_io_text` in `tinyactors-napi/src/napi_datamodel.c`
   treats any result other than `TA_OK` from `ta_dm_host_text` as "no
   value", as the jq test adapter did before this change. It has the same
   problem as open point 1 and was left for the napi work.
8. **Reentrant resume is not supported.** The spare is per system, as the
   scratch arena is: a plugin that resumes another actor from inside a
   callback would take it.

## Implementation notes

2026-09-24.

- **Phases.** `ta_run_phase` in `src/continuation.h` replaces
  `ta_micro_phase`; the stages of the new phases are `TA_DATA_*`,
  `TA_FINALIZE_*` and `TA_FINISH_*`, and `ENTER` gained a `DATA` stage for
  late binding, whose declaration cursor is `at.handler`. `took_event` is
  gone: a microstep that has started is never given back.
  `ta_run_phase_in_microstep()` is what inspection's
  `microstep_in_progress` tests.
- **The run loop.** `ta_run()` in `src/interpreter.c` continues the open
  phase (`ta_continue_phase()`), or picks the next step with nothing open
  (`ta_next_step()`), which follows SCXML's mainEventLoop: eventless
  transitions, then an internal event, then the macrostep's invocations,
  then an external event. After the invocations start, eventless
  transitions are selected again only if an internal event was queued
  (`after_invokes`), as the algorithm's `continue` does. The functions
  `ta_bootstrap`, `ta_finish`, `ta_run_microstep`, `ta_pause_save` and
  `ta_prepare_microstep_queue` are gone, and so is the definition's
  `microstep_needs_sparse`.
- **Stopping.** `ta_reserve_stop()` runs before `ta_interpreter_init()`;
  `ta_stop()` saves into the resumed continuation or the spare;
  `ta_stop_restore()` keeps the continuation in `it->resumed`, and
  `ta_continuation_recycle()` returns it to the system when the run did not
  stop again. `system->spare_continuation` and `system->spare_sparse` are
  freed by `ta_interpreter_release_spares()` at teardown.
- **The executor.** `ta_unit_reserve()` runs before every unit, and a unit
  that answers `TA_ERR_NOMEM` puts its frame's cursor back, leaving the
  frames open for the stop. `ta_foreach_bind()` advances the iteration
  only on an outcome.
- **Sends.** `ta_exec_send()` is `ta_send_evaluate()` then
  `ta_send_commit()`; the evaluation helpers return `ta_result`, with
  `TA_EXEC_FAILED` for an execution error. `ta_actor_reserve_send()` makes
  room in the owner's storage.
- **Invocations.** `ta_invoke_start()` reserves, evaluates, then
  `ta_invoke_launch()` records and starts; `ta_invoke_start_pending()`
  takes a state and a declaration cursor. `ta_invoke_before_external()`
  now only says whether `<finalize>` or autoforwarding apply;
  `ta_invoke_finalize_actions()` and `ta_invoke_autoforward()` do the
  work in the `FINALIZE` phase. `ta_interpreter_run_actions()` is gone.
- **Plugins.** `ta_op_result()` and `ta_op_failed()` map `TA_OP_NOMEM` to
  `TA_ERR_NOMEM` wherever core reads a plugin's status. In the test host,
  the jq data model answers `TA_OP_NOMEM` when core cannot render a system
  variable and when its own staging fails, the SCXML invoker answers it
  when importing, compiling or spawning the child runs out of memory, and
  `router.direct` and `router.recording` answer RETRY for a receiver's
  `TA_ERR_NOMEM`.
- **Tests.**
  - `tests/pause_test.c`: `test_pause_needs_no_allocation` replaces
    `test_unsaved_pause_keeps_running`. With every allocation refused
    from inside the microstep on, the actor still pauses after every unit.
  - `tinyactors-test/ruby/scenarios/out_of_memory.rb`, with
    `Context#assert_allocation_failures_invisible` in
    `ruby/tinyactors_test/scenario.rb`. A baseline run counts the
    allocations of every `ta_actor_resume()` the body makes, then one run
    per allocation fails it. The resume must answer `err_nomem`, the
    stopped actor must be in its macrostep, refuse an event and have
    queued no error event, and resuming it must end the run with the
    baseline's trace: configuration, data, queued events, delayed sends,
    invocations, completion, and every instrument's journal. Ten charts,
    each on jq and ECMAScript: many raises, delayed sends and cancels,
    `<foreach>`, final states and `<donedata>`, content in all three
    microstep phases, five invocations with `<finalize>`,
    autoforwarding and failing guards, failing guards in parallel
    regions, early and late bootstrap, and a parent with an SCXML child.
    Each runs three ways: every allocation failing once, three times in a
    row, and once with the stopped actor unloaded and loaded again before
    the retry. Explicit scenarios reconstruct an actor stopped inside a
    microstep from its image, make a second actor reserve a continuation
    after the first took the spare, and fail the rendering of a long
    `_event.name` on each data model.
  - Against the core before this change, the sweep fails on nine of the
    ten charts.
