# TinyActors::Snapshot

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: raw actor and inspection records, Event.

## Owned files

- `lib/tinyactors/snapshot.rb`
- `test/high_level/snapshot_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

Snapshot is an immutable observation assembled from actor info and detail records.
Expose `ref`, `status`, `reason`, `revision`, `sessionid`, `registered_name`, `state`, `active`, `history`, and `event`.
Expose `event_included?` to distinguish an omitted event field from an unbound event.
Construction uses an explicit native snapshot policy.

## Required behavior

1. Copy text, bytes, metadata, active configurations, and history data.
2. Apply the configured snapshot policy to native state and event data.
3. Do not expose borrowed mutable native state as a durable snapshot.
4. Distinguish no current event from an event excluded by inspection filters.
5. Keep registered_name, definition name, and sessionid distinct.
6. Keep snapshots readable after actor release or upgrade.

## Acceptance checks

- Mutate a snapshot payload and verify the actor state is unchanged.
- Keep a snapshot after actor release.
- Include the origin of the last event.
- Verify idle and in-progress observations follow the requested filters.
- Represent omitted event details separately from an unbound event.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
