# Error model

Tinyactors has two orthogonal failure channels.

## `ta_result`

Used for API/runtime failures such as invalid arguments, allocation failure, unsupported operation, busy actor, corrupt external input, or inability to reconstruct non-invocation interpreter state. A negative `ta_result` means the requested C operation itself did not complete normally.

Session reconstruction returns `TA_ERR_NOT_FOUND` when no registered definition has the image's family and `TA_ERR_DEFINITION_MISMATCH` when the family exists but the exact revision is unavailable. `TA_ERR_NOT_RECONSTRUCTIBLE` indicates non-invocation interpreter state cannot be captured or recreated. A runtime-only invoker service handle is not such an API error: reconstruction succeeds, retires that invocation, and queues `error.communication.invoke.<id>.unavailable`. These are C/API errors, not SCXML `error.*` events.

Named routing returns `TA_ERR_UNLOADED` when the address exists but must be
loaded before it has a runtime actor ID. `TA_ERR_STALE` reports a failed
directory comparison or an unload image whose source session epoch no longer
matches. Both are retry/control results for the host and do not generate SCXML
events.

`TA_ERR_DONE` reports that an actor has reached a top-level final state and
accepts nothing more (ADR 0055). `ta_actor_deliver()` returns it instead of
`TA_ERR_INVALID`, which from deliver means a caller error only. It is a
control result for the host's scheduler, like `TA_ERR_FULL` from a pool at its
admission watermark, and generates no SCXML event.

## SCXML `error.*`

Used for statechart-visible runtime errors. These are ordinary internal platform events. The broad standard categories are:

- `error.execution` — failures internal to executing the machine, such as expression/assignment evaluation;
- `error.communication` — failures communicating with external entities;
- `error.platform` — reserved platform/application namespace.

Tinyactors may append structural detail suffixes while retaining those prefixes.

`error.communication.invoke.<id>.unavailable` reports that an invoked service is
no longer available. Its name identifies the logical invocation without
exposing whether loss was learned during reconstruction, from an invoker, or by
another runtime mechanism. It has platform event type and is appended to the
internal queue. Its envelope `invoke_id` remains `TA_INVOKE_ID_NONE`, because the
processor generated the error rather than receiving it from the invoked child.

A callback should not directly enqueue the error. It reports `TA_OP_ERROR`
and a `ta_fault`; the interpreter, which knows the operation it called, maps
it to the appropriate event and implements the correct local semantic
consequence:

- a condition, value, location, assignment or script failure, or an invalid
  send target or type found while evaluating, is `error.execution...`;
- an I/O processor unable to dispatch is `error.communication...`;
- an invocation's failure to start, cancel or forward is
  `error.communication...`.

The invocation ID names the detailed `error.communication.invoke.<id>...`
event; it does not populate the resulting event's invoke-ID field.
Reconstruction raises `...unavailable` itself: no invoker callback or
invoker-supplied fault is required for that core-generated error.

Error events are appended to the internal event queue and processed in normal
FIFO order; they are not exception-like immediate jumps.
