# ADR 0056: A finished actor reports the final state it reached

**Status:** Proposed; refines ADR 0010 and ADR 0042
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
