# TinyActors::Invocation

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Raw::InvocationRef, Message, Error.

## Owned files

- `lib/tinyactors/invocation.rb`
- `test/high_level/invocation_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

Invocation wraps a live external-service identity from a START request.
Expose `system`, `ref`, and `id`.
Provide `emit(name_or_message, **options)`, `done(body)`, `fail`, and `cancelled?`.
Invoke is the authored descriptor. Invocation is the live service interface.

## Required behavior

1. Require successful START completion before accepting service output.
2. Delegate provenance and invokeid construction to Raw invocation procedures.
3. Preserve native and byte output payloads.
4. Keep the invocation live after a retryable failed done admission.
5. Reject late results after cancellation or completion.
6. Do not implicitly complete a related I/O request from an invocation method.

## Acceptance checks

- Emit an event with authentic invokeid and origin.
- Complete with done data and verify finalize ordering.
- Cancel the owner and reject late emissions.
- Retry done after an admission failure.
- Reject tokens from another System.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
