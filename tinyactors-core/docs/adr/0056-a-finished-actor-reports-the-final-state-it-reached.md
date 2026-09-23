# ADR 0056: A finished actor reports the final state it reached

**Status:** Accepted and implemented; refines ADR 0010 and ADR 0042. See the implementation notes for where the code differs from the text.
**Date:** 2026-09-23

## Context

SCXML's `exitInterpreter` exits every active state when the machine reaches
a top-level `<final>`. Core does this in `ta_finish()`
(`src/interpreter.c:1710`). After that, the active configuration is empty,
`active_state_count` is 0, and `residency` is `TA_ACTOR_DONE`. This is
correct, but it loses one fact: *which* final state ended the session.

Hosts need that fact. A chart often has several top-level finals (`ok`,
`rejected`, `timed_out`), and the outcome is the one that was reached.
Because core does not keep it, the Node-API binding started copying every
top-level final of a definition. When an actor is done, it reports that whole
list as the actor's active states. With two or more top-level finals, it
reports states that were never entered. A binding cannot rebuild this fact
correctly, because the information is gone by the time it could ask.

`<donedata>` does not replace this. It is optional, it depends on the data
model, and the null data model has no data at all.

## Decision

1. **Core records the top-level `<final>` whose entry ended the session.**
   It stores a `ta_state_id` in the actor's sparse state, next to the
   completion. A finished actor already has sparse state for the completion,
   so live actors pay nothing.
2. **`ta_actor_inspection` gains `ta_state_id final_state`.** It holds that
   state when `residency == TA_ACTOR_DONE`, and `TA_STATE_NONE` otherwise.
   `ta_definition_state()` resolves it to its ID and kind, as with any other
   state.
3. **The active configuration stays empty after finishing.** Core does not
   pretend that the final state is still active. `ta_actor_active_states()`
   keeps SCXML semantics, so `In()` is false for everything after
   termination.
4. **Session images carry the final state together with `finished`.** It is
   captured and restored the same way (ADR 0038). No saved images exist
   yet, so the image layout changes without a version bump or migration.
5. An actor that ends in another way, such as cancellation of an invoked
   child (`ta_actor_exit_invoked`) or destruction, did not reach a final
   state. It reports `TA_STATE_NONE`.

## Consequences

- Bindings report outcomes such as "done in `rejected`" without holding
  definition data of their own. The Node-API `final_states` copy and its
  wrong output are removed.
- The public inspection struct grows by one field. `ta_actor_completion`
  does not change.
- ADR 0042 export is not affected, because it describes definitions, not
  sessions.

## Implementation notes

Recorded when this ADR was implemented (2026-09-23).

- **Where it is stored.** `ta_actor_sparse_state.final_state`, next to
  `completion`. It is set in `ta_enter_final()` once the completion is
  stored, which is the only place `finished` becomes true. Sparse state
  starts it at `TA_STATE_NONE`. Running actors have no sparse state for this
  reason, so their cost is unchanged: the scale test still measures 560
  bytes per actor. Inspection reports it only while `finished` is true.
- **Session images.** The image carries `final_state` next to `finished`.
  `ta_actor_reconstruct()` restores it, and `ta_actor_load()` goes through
  the same path. A finished actor restored from an image now always gets
  sparse state. Before, it got sparse state only when it had done data, an
  invocation, a parent or queued events, so a finished actor without done
  data came back with no completion to report.
- **D5, cancellation.** A cancelled invoked child is exited and destroyed
  while its parent's microstep runs, so no inspection can see it afterwards.
  The observable guarantee is that it reports `TA_STATE_NONE` while it runs,
  is no longer resident once cancelled, and never answers done. The exit path
  never sets `finished`, so core could not report a final state for it even
  if it stayed.
- **Paused actors (ADR 0061).** An actor paused inside a microstep has not
  finished and reports `TA_STATE_NONE`, including when that microstep is
  the one that will enter a top-level final.
- **Tests.** `tinyactors-test/ruby/scenarios/final_state.rb`: which of
  three top-level finals was reached, and not the nested final that ended a
  compound state on the way; an empty configuration after finishing; no
  final state while running or paused with an action budget; a cancelled
  invoked child that is gone rather than finished; and the final state
  surviving capture, reconstruct, unload and load. Removing the restore
  line from the session image makes the last one fail.
- **Node-API.** The binding still copies every top-level final
  (`tinyactors-napi/src/addon.c`, around lines 31, 1082, 1418 and 1882).
  It can now read `final_state` instead; that change is left to the session
  that owns napi.
