# `session_image.h`

A `ta_session_image` is an opaque, owned in-memory capture of Tinyactors-managed interpreter state.

It exists for two reasons:

- the runtime must be able to reconstruct the same interpreter state it can expose to operational inspection;
- optional durability/debugging/process-migration layers need a stable structural boundary below their own encoding/storage policy.

## Capture

`ta_actor_capture()` may capture an idle actor or a paused actor mid-macrostep. The image therefore preserves configuration, history, continuation/current/internal events when present, invocations, delayed sends, logical self identity, and exact definition revision.

The image is not a byte blob. Host values held directly by interpreter structures are retained using `ta_value` lifetime semantics.

## Reconstruction

`ta_actor_reconstruct()` requires the exact definition revision and a host-supplied data-model binding. It never treats a newer definition as compatible merely because an application schema appears unchanged. If the family exists but the revision differs, reconstruction returns `TA_ERR_DEFINITION_MISMATCH`.

Changing chart definitions is actor replacement, not reconstruction.

## Datamodel exclusion

The host object graph is deliberately absent. A CPython binding can reconstruct around an already-existing `PyObject *`; a large LLM transcript remains the application's object, database record, persistent vector, or other representation. Session image cost therefore scales with interpreter state rather than application data size.

## Durability

A durability module may encode a session image plus stable identities for referenced host values, invocations, and timers. Such an encoding is a **checkpoint**. The core does not prescribe its file/database format, commit protocol, command journal, replay policy, or effect deduplication strategy.
