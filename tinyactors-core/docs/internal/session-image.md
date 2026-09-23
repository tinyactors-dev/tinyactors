# `session_image.h`

A `ta_session_image` is an opaque, owned in-memory capture of Tinyactors-managed interpreter state.

It exists for two reasons:

- the runtime must be able to reconstruct the same interpreter state it can expose to operational inspection;
- optional durability/debugging/process-migration layers need a stable structural boundary below their own encoding/storage policy.

## Capture

`ta_actor_capture()` may capture an idle actor or a paused actor mid-macrostep. The image therefore preserves configuration, history, continuation/current/internal events when present, invocations, delayed sends, logical self identity, and exact definition revision.

A terminal actor's final data and an invoked child's pending completion linkage
are also preserved. An unpublished prepared replacement cannot be captured:
its held activation effects belong to an incomplete replacement transaction,
so capture returns `TA_ERR_BUSY` until activation.

The image is not a byte blob. Host values held directly by interpreter structures are retained using `ta_value` lifetime semantics.

The image also records the source actor ID and session epoch. These are local
freshness metadata, not portable identity. `ta_actor_unload()` rejects an image
with `TA_ERR_STALE` if the source actor has changed since capture.

`ta_session_image_next_deadline()` returns the earliest captured delayed-send
deadline, or `TA_ERR_NOT_FOUND`. This is sufficient for a host durability layer
to maintain a wake-up index for an unloaded actor without exposing the image's
private structure. `ta_session_image_definition()` names the definition the
image must be restored against, the same exact-revision lookup load and
reconstruct use, so a host can build a binding with that definition's data
model before loading it.

Active invocation records are captured logically, but opaque invoker service
handles are runtime-only and are not retained by the image.

## Reconstruction

Restoring an image requires the exact definition revision and a host-supplied data-model binding. It never treats a newer definition as compatible merely because an application schema appears unchanged. If the family exists but the revision differs, `ta_actor_load()` returns `TA_ERR_DEFINITION_MISMATCH`.

For a named virtual actor, `ta_actor_load()` wraps reconstruction in the
directory residency protocol. It prepares the session while the directory still
says `UNLOADED` and publishes the new runtime ID only after preparation
succeeds. `ta_actor_reconstruct()` is the route-neutral primitive underneath:
it returns a fresh runtime ID without reading or changing the directory (ADR
0052). A named reconstruction keeps the image's logical self-address, so sends
to that address follow its current directory entry rather than implicitly
publishing the reconstructed session. An anonymous reconstruction uses its new
runtime ID as self; the captured runtime ID remains stale.

After restoring the captured interpreter state, reconstruction retires each
invocation that was active in the image and appends an internal
`error.communication.invoke.<id>.unavailable` platform event for it. These events
are appended in captured invocation order after internal events already in the
image. The name identifies the invocation. The envelope has
`TA_INVOKE_ID_NONE`, because the processor generated the event rather than the
invoked service. Reconstruction still returns `TA_OK`; the communication failure
is ordinary statechart control flow. Late events from the lost invocation are
ignored.

Changing chart definitions is actor replacement, not reconstruction.

## Datamodel exclusion

The host object graph is deliberately absent. A CPython binding can reconstruct around an already-existing `PyObject *`; a large LLM transcript remains the application's object, database record, persistent vector, or other representation. Session image cost therefore scales with interpreter state rather than application data size.

## Durability

A durability module may encode a session image plus stable identities for referenced host values and timers, and may keep external protocol state used to reconcile an invocation after the chart receives its reconstruction communication error. Such an encoding is a **checkpoint**. The core does not prescribe its file/database format, commit protocol, command journal, replay policy, or effect deduplication strategy.
