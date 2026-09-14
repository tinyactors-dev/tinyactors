# TinyActors::Raw::Result

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap constants.

## Owned files

- `lib/tinyactors/raw/result.rb`
- `test/raw/result_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`Result.new(status:, value: nil)` creates an immutable result.
Expose `status`, `value`, and `ok?`.
`ok?` means `status == Raw::TA_OK` only.
Do not define implicit conversion to Integer or boolean.

## Required behavior

1. Preserve the exact status Integer and the output object supplied by the wrapper.
2. Do not decide whether an output is valid. The procedure wrapper applies its C contract.
3. Keep definition failure diagnostics accessible through value.
4. Do not raise an exception merely because status is nonzero.

## Acceptance checks

- TA_OK, TA_EMPTY, TA_AGAIN, TA_BUSY, and TA_STALE remain distinct.
- Mutation of the result fails.
- A failed definition result retains its diagnostic Hash.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
