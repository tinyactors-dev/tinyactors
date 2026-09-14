# TinyActors::Definition

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Raw::Definition, CodeRegistry, Action, Send, Invoke, MachineBuilder, Error.

## Owned files

- `lib/tinyactors/definition.rb`
- `test/high_level/definition_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`Definition.build(system, specification)` compiles and builds a nested definition.
`Definition.prepare(system, raw_input)` exposes normalized preparation without lossy conversion.
Expose `raw`, `revision`, `description`, `footprint`, and `release`.
The system argument follows the System protocol and supplies its raw handle.

## Required behavior

1. Compile nested authoring data to C chart records using one CodeRegistry.
2. Compile every supplied guard, action, eventless transition, invoke, and finalize block.
3. Attach the registry as code_owner and preserve its lifetime through Raw.
4. Validate all authored keys. Never accept a feature and discard it.
5. Preserve definition name independently of actor registration names.
6. Keep publication, caller references, and actor ownership distinct.
7. Return useful status-bearing build errors with copied diagnostics.
8. Reject use after release and cross-system inline definitions.

## Acceptance checks

- Run a guarded request/reply chart compiled from builder output.
- Run the same behavior through normalized prepare.
- Execute distinct action tokens and false guards.
- Release a caller handle while an actor still uses the definition.
- Verify definition upgrade keeps the correct registry and closures.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
