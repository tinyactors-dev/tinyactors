# TinyActors::Raw::Context

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap, records, callbacks, Result, Memory, Value.
C anchors: `ta_context_t`, `ta_system_values_t`, `ta_event_t`.

## Owned files

- `ext/tinyactors_raw/raw_context.c`
- `test/raw/context_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

Context is a callback-only handle created by the bridge.
Direct construction is private.
`Raw.ta_context_inspect(context)` returns the complete C view.
The event path is `view[:system][:event][:origin]` when an event exists.
Also implement `Raw.context_code_owner(context)` as a documented bridge helper.
It exposes the active definition's code_owner without permitting arbitrary definition API calls from a callback.

Own these Raw singleton procedures:

- `TinyActors::Raw.ta_context_inspect`
- `TinyActors::Raw.ta_context_in`
- `TinyActors::Raw.ta_context_host_user`
- `TinyActors::Raw.ta_context_bytes`
- `TinyActors::Raw.ta_context_arena`
- `TinyActors::Raw.ta_context_raise`
- `TinyActors::Raw.ta_context_send`
- `TinyActors::Raw.ta_context_cancel`

Read each exact signature and ownership comment in the public header before implementation.

## Required behavior

1. Expose the same current event to guards, exit actions, transition actions, and entry actions.
2. Preserve all event fields even when payload conversion fails.
3. Expose absent initial events as nil and absent origin fields as nil.
4. Keep ta_context_host_user tied to the original Ruby configuration object.
5. Check the lifetime token before every procedure.
6. Use the core phase restrictions for send, raise, cancel, bytes, and arenas.
7. Preserve exact send failure statuses and message IDs.
8. Copy any escaping text or metadata before invalidating the callback token.

## Acceptance checks

- Read actor-originated origin in a guard and reply in an action.
- Retain contexts after normal and exceptional returns. Verify every procedure rejects them.
- Reject effects from guards without delivering a message.
- Verify eventless retention and internal-event replacement.
- Verify full and missing destinations preserve their C statuses.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
Update only the assigned API ledger entries after their behavioral tests pass.
