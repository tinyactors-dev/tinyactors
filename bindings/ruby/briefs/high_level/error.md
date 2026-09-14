# TinyActors::Error

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Raw::Result and raw constants.

## Owned files

- `lib/tinyactors/error.rb`
- `test/high_level/error_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`Error < StandardError` exposes `status`, `operation`, `diagnostic`, and `cause`.
`Error.check(status_or_result, operation:, allowed: [])` returns a successful value or raises.
For an allowed non-OK status, return the original raw status/result unchanged.
Keep this helper internal to the convenience layer.

## Required behavior

1. Preserve the exact C status Integer in every raised error.
2. Include copied definition diagnostics when the raw result provides them.
3. Do not classify EMPTY, AGAIN, BUSY, or STALE without the calling method policy.
4. Let the callback bridge recover failed context-effect statuses from Error instances.
5. Use TA_EXECUTION_ERROR for a reply without an origin and identify Context#reply as the operation.
6. Keep programming TypeError, ArgumentError, and RangeError exceptions distinct.

## Acceptance checks

- Check status-only and status-plus-output failures.
- Preserve definition error detail and source indices.
- Allow explicitly expected EMPTY and AGAIN statuses.
- Verify a failed context send produces error.communication once.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
