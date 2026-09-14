# TinyActors::Raw::InspectCursor

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap, records, callbacks, Result.
C anchors: `ta_inspect_cursor_t`, `ta_inspect_record_t`.

## Owned files

- `ext/tinyactors_raw/raw_inspect_cursor.c`
- `test/raw/inspect_cursor_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`InspectCursor.new` creates a zeroed opaque cursor.
Expose `reset` only when no inspection call is active.
`Raw.ta_system_inspect(system, options, cursor)` returns an Integer status.
The options include the raw inspection sink callback.

Own these Raw singleton procedures:

- `TinyActors::Raw.ta_system_inspect`

Read each exact signature and ownership comment in the public header before implementation.

## Required behavior

1. Keep opaque C words private. Do not expose mutable integer arrays.
2. Associate a started cursor with its system and required filters.
3. Allow the budget and sink to change where the C contract permits it.
4. Preserve TA_AGAIN, TA_CANCELLED, and TA_STALE behavior.
5. Copy visitor records before their callback ends.
6. Preserve O(1) cursor storage. Do not materialize all actors inside Raw.

## Acceptance checks

- Pause with a one-unit budget and resume without duplicate records.
- Change system state between pages and observe TA_STALE.
- Verify active states, history, data, and last-event records.
- Exercise PAUSE and STOP sink results.
- Abandon a cursor without retaining actor or arena resources.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
Update only the assigned API ledger entries after their behavioral tests pass.
