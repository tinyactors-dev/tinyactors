# TinyActors::StateBuilder

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Action, Send, Invoke.

## Owned files

- `lib/tinyactors/state_builder.rb`
- `test/high_level/state_builder_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

StateBuilder creates one state specification.
Provide `state`, `initial`, `on`, `always`, `entry`, `exit`, `invoke`, `final`, `parallel`, and `history`.
`on(name, target: nil, guard: nil, actions: [])` preserves an optional guard Proc.
`to_h` returns the immutable authoring specification for Definition compilation.

## Required behavior

1. Preserve state, transition, action, and invocation order.
2. Keep event names unchanged, including hierarchy and case.
3. Keep false guards and empty action blocks as meaningful authored input.
4. Keep targetless and internal transitions distinct from external reentry.
5. Preserve eventless transitions in the specification.
6. Reject duplicate state IDs and unsupported options with a useful location.
7. Do not resolve nested targets independently from Definition compilation.

## Acceptance checks

- Create nested, parallel, final, and history state specifications.
- Compile a false guard and verify its action never executes.
- Preserve entry and exit Procs instead of discarding non-Hash actions.
- Exercise eventless transitions and targetless actions.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
