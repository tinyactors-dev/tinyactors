# TinyActors::DataModel

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Raw::Value, CodeRegistry, Context, Event, Error.

## Owned files

- `lib/tinyactors/data_model.rb`
- `test/high_level/data_model_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

DataModel supplies the raw value and data callback tables for native Ruby actors.
`DataModel.new(factory:, snapshot:, name: "ruby")` requires an explicit snapshot policy.
Expose `name` and `snapshot_policy` as readers.
Call the factory as `factory.call(context, seed)` with the unwrapped seed.
Call the snapshot policy as `snapshot.call(object)`.
Provide `raw_value_ops` and `raw_data_ops` for System configuration.
Application callbacks receive high-level Context objects.
The high-level System requires a DataModel for native storage.
Call action and guard Procs with one Context argument.
Call registered expression and location functions according to their declared CodeRegistry use.

## Required behavior

1. Create independent actor state through the supplied factory.
2. Provide every data callback required by supported authored features.
3. Resolve code tokens from the active definition code_owner.
4. Preserve action kind and boolean guard results.
5. Keep system values and event metadata separate from application-owned mutable state.
6. Expose metadata when event payload conversion fails.
7. Use the snapshot policy for message, spawn, inspection, and migration isolation where applicable.
8. Keep native nil distinct from an absent Raw::Value.
9. Return raw status and output objects using the shared callback convention.
10. Do not use Marshal, eval, or shallow dup as an undocumented snapshot fallback.

## Acceptance checks

- Create two actors with independent nested state.
- Run actions, false guards, expressions, assignment, iteration, and finalize.
- Verify origin remains visible after failed data conversion.
- Force GC and compaction with queued native messages.
- Use a custom Ruby value class with an explicit snapshot policy.
- Preserve original context-effect statuses through the callback bridge.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
