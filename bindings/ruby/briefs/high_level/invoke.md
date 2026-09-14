# TinyActors::Invoke

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: CodeRegistry, raw invoke records, and raw definition handles.

## Owned files

- `lib/tinyactors/invoke.rb`
- `test/high_level/invoke_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`Invoke.new(**fields)` describes a state-owned invocation.
Use the raw invoke field names for advanced options.
Accept built Definition objects for inline definitions through their raw handle protocol.
`compile(registry)` returns a raw ta_invoke_desc_t record.
An Invoke is a descriptor. Invocation is a live external-service token.

## Required behavior

1. Preserve literal versus evaluated type, source, IDs, and payload expressions.
2. Preserve ordered parameters and their location metadata.
3. Preserve autoforward and every finalize mode.
4. Keep inline definitions rooted in the same system.
5. Delegate invocation start, cancellation, and completion semantics to C.
6. Do not turn invoking a service into direct Ruby I/O.

## Acceptance checks

- Invoke an inline SCXML child and an external service.
- Verify autoforward preserves origin.
- Verify explicit sends from a child identify that child.
- Exercise finalize before transition guards.
- Reject unsupported fields rather than ignoring them.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
