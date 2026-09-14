# TinyActors::MachineBuilder

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: StateBuilder.

## Owned files

- `lib/tinyactors/machine_builder.rb`
- `test/high_level/machine_builder_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`TinyActors.machine(name, **options, &block)` returns an immutable authoring specification.
MachineBuilder implements that module helper.
Provide `initial`, `state`, and `to_h`.
The block defines states through StateBuilder.
Building a specification creates no native system, definition, or actor.

## Required behavior

1. Keep the synthetic C root implicit.
2. Preserve explicit top-level binding and data-layout options.
3. Return reusable authoring data that can compile into different systems.
4. Do not share mutable native actor state through builder defaults.
5. Reject unsupported root executable content according to the chosen authoring contract.
6. Delegate native validation to Definition after local structural checks.

## Acceptance checks

- Build one specification and compile it into two systems.
- Verify builder output has deterministic document order.
- Reject malformed declarations with clear field paths.
- Verify authoring does not execute user actions or guards.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
