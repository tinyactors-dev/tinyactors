# TinyActors::Action

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: CodeRegistry and raw action records.

## Owned files

- `lib/tinyactors/action.rb`
- `test/high_level/action_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

Provide immutable declarative action descriptors.
Constructors: `.custom(&block)`, `.script(&block)`, `.raise(name, data: nil)`, `.assign(location, value)`, `.cancel(sendid)`.
Also support conditional, foreach, and log descriptors through `.if`, `.foreach`, and `.log`.
`compile(registry)` returns the exact raw ta_action_t record.
Send is a specialized Action class with a separate brief.

## Required behavior

1. Preserve action order and block boundaries.
2. Route scripts and custom actions through their distinct C action kinds.
3. Compile guards and expressions to registry tokens with the proper use.
4. Implement native payload on raise through a custom action calling Context#raise when the C authored raise cannot carry data.
5. Do not silently discard Proc actions or unknown descriptor keys.
6. Compile control flow to core actions. Do not run a second Ruby action interpreter.
7. Preserve assignment and cancellation failure statuses.

## Acceptance checks

- Run multiple distinct actions in document order.
- Exercise false branches, foreach snapshots, cancellation, and log callbacks.
- Verify unknown action fields fail at construction or compilation.
- Verify one failed context effect aborts its block and emits one error.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
