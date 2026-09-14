# TinyActors::CodeRegistry

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Raw::Value and Raw.context_code_owner.

## Owned files

- `lib/tinyactors/code_registry.rb`
- `test/high_level/code_registry_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

Create one registry per compiled definition.
`register(callable, use:)` returns a nonzero code token.
`fetch(token, use:)` returns the matching callable or raises.
`seal` prevents later mutation. `code_owner` returns the rooted Raw::Value used by the descriptor.
Zero means an absent expression and is never assigned to a callable.
Use `Raw.context_code_owner(raw_context)` to find the active registry during a callback.

## Required behavior

1. Keep action, guard, expression, location, and validation uses distinguishable.
2. Keep tokens stable for the lifetime of their definition.
3. Resolve tokens through the active definition code_owner, including after upgrade.
4. Root closures while any actor, publication, or definition reference requires them.
5. Reject unknown and wrong-use tokens instead of invoking a global callback.
6. Do not eval strings or implement an expression language.

## Acceptance checks

- Execute two different action tokens and verify different behavior.
- Reject token zero and an unknown token.
- Keep old and new definition registries distinct during upgrade.
- Force GC and compaction while closures remain referenced by actors.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
