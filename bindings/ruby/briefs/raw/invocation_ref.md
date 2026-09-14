# TinyActors::Raw::InvocationRef

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap, records, callbacks, Result.
C anchors: `ta_invocation_ref_t`.

## Owned files

- `ext/tinyactors_raw/raw_invocation_ref.c`
- `test/raw/invocation_ref_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`InvocationRef.new(system, id)` creates an immutable system-scoped invocation identity.
Expose `system`, `id`, equality, and hash.
Receive real invocation identities from I/O request records.

Own these Raw singleton procedures:

- `TinyActors::Raw.ta_invocation_emit`
- `TinyActors::Raw.ta_invocation_done`
- `TinyActors::Raw.ta_invocation_fail`
- `TinyActors::Raw.ta_invocation_is_cancelled`

Read each exact signature and ownership comment in the public header before implementation.

## Required behavior

1. Preserve the distinction between request ID, invocation ID, textual invokeid, and actor ID.
2. Keep invocation output authenticated through the corresponding C procedure.
3. Preserve emit, done, fail, and cancellation-query status values.
4. Allow retry after a failed done admission when the core keeps the invocation live.
5. Reject late output after cancellation or successful completion.
6. Do not invent parent origin or invokeid from application data.

## Acceptance checks

- Complete START before emitting native and byte results.
- Verify automatic invokeid and origin metadata.
- Exercise done admission failure and retry.
- Cancel the owner and reject late emits.
- Reject references from another system.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
Update only the assigned API ledger entries after their behavioral tests pass.
