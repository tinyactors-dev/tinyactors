# TinyActors::Raw::Value

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap root registry.
C anchors: `ta_value_t`, `ta_value_ops_t`.

## Owned files

- `ext/tinyactors_raw/raw_value.c`
- `lib/tinyactors/raw/value.rb` (remove the obsolete Ruby-only wrapper)
- `test/raw/value_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`Value.new(object)` creates an explicit native-value wrapper.
`value.object` returns its Ruby object while the wrapper is live.
Make native handle numbers private.
Bare nil at the raw boundary represents handle zero.
`Value.new(nil)` represents a real native nil value.

## Required behavior

1. Keep stable C handle indirection while Ruby moves objects during compaction.
2. Track core-owned references separately from the wrapper reference.
3. Balance retain and release on admission failures, queue replacement, and actor teardown.
4. Use the configured native snapshot callback for snapshot operations.
5. Reject accidental cross-system adoption of a value already owned by another binding domain.
6. Do not claim a retained mutable object is a snapshot.

## Acceptance checks

- Distinguish absent, native nil, false, and zero values.
- Run GC.start and GC.compact during queued and current-event lifetimes.
- Verify an isolated snapshot does not share mutable payload data.
- Verify every retained root is released once after failure and teardown.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
