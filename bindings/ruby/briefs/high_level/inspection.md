# TinyActors::Inspection

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Raw::InspectCursor, Snapshot, Event, Error.

## Owned files

- `lib/tinyactors/inspection.rb`
- `test/high_level/inspection_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

Inspection represents one resumable scan.
`Inspection.new(system, **filters)` stores immutable filters.
`next_page(work_units:)` returns a Hash with records, status, and done.
Each page contains copied detail records with the raw C field names.
Do not construct partial Snapshot objects across page boundaries.
`reset` restarts the scan. `each` streams records without materializing every actor.
Expose the Raw cursor for advanced use through a read-only accessor.

## Required behavior

1. Preserve the core cursor instead of inventing numeric page offsets.
2. Treat TA_AGAIN as continuation and TA_OK as completion.
3. Surface TA_STALE explicitly. Do not silently restart and duplicate records.
4. Copy records during each visitor callback.
5. Apply the configured snapshot policy outside core callbacks when the lifetime allows it.
6. For native data that must survive the visit, retain its Raw::Value during the visit before later snapshotting.
7. Keep caller callbacks outside the raw visitor when they may interact with the same system.
8. Do not retain an unbounded actor list between pages.

## Acceptance checks

- Scan with a one-unit budget and resume without duplicate records.
- Mutate the system between pages and observe a status-bearing stale error.
- Stream active states, history, and last-event origin.
- Verify early enumeration termination releases temporary roots.
- Exercise both stable-only and unfiltered scans.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
