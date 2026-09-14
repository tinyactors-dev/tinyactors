# TinyActors::Event

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: raw event records.

## Owned files

- `lib/tinyactors/event.rb`
- `test/high_level/event_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`Event.from_raw(record)` creates a read-only event wrapper. Nil input returns nil.
Expose `name`, `type`, `data`, `sendid`, `origin`, `origintype`, `invokeid`, `message_id`, and `trace`.
Map the raw event type to `:external`, `:internal`, or `:platform`.
`to_raw` returns a raw event record with the numeric event type.

## Required behavior

1. Keep event names byte-for-byte unchanged.
2. Use nil consistently for absent metadata.
3. Keep origin as the routable session address string. Do not substitute an actor name.
4. Copy metadata strings and record containers so they survive later native calls.
5. Unwrap native payloads and copy byte payloads according to the configured data model.
6. Do not claim mutable event data is an immutable snapshot without applying the snapshot policy.
7. Keep output serialization separate from authenticated invocation routing.

## Acceptance checks

- Expose every event field independently.
- Distinguish native nil, no payload, and empty bytes through to_raw.
- Read origin after another event and after sender release.
- Keep an unmatched event as the current event.
- Verify metadata cannot be replaced by application assignment.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
