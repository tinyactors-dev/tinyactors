# TinyActors::Message

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Raw::Value and raw payload records.

## Owned files

- `lib/tinyactors/message.rb`
- `test/high_level/message_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`Message.new(name, data: nil, payload: :native, trace: nil)` creates an outbound host message.
Use `payload: :none` for absent content and `payload: :bytes` for an explicit binary String.
Expose `name`, `data`, `trace`, and `to_raw`.
A Message has no origin, invokeid, sendid, or caller-assigned message ID.

## Required behavior

1. Preserve the distinction between native nil and absent data through the payload tag.
2. Reject unsupported payload tags and non-String byte payloads.
3. Wrap native content in Raw::Value before calling Raw.
4. Let the runtime perform the configured admission snapshot.
5. Do not allow host posting to fabricate an actor sender.
6. Keep application correlation IDs in payload data.

## Acceptance checks

- Post native nil, false, zero, nested data, empty bytes, and arbitrary binary bytes.
- Mutate source data after admission and verify the receiver snapshot policy.
- Reject origin and invokeid as Message constructor fields.
- Verify Unicode event names keep their original spelling.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
