# TinyActors::System

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: all Raw procedures, Error, DataModel, Definition, Actor, Target, Message, Inspection, IORequest.

## Owned files

- `lib/tinyactors/system.rb`
- `test/high_level/system_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`System.new(data_model: nil, config: {})` creates an explicit hosted allocator, root arena, and raw system.
Require a DataModel for native state. Permit an explicit byte-only configuration without one.
The config Hash contains raw C configuration overrides.
A supplied DataModel installs its name, data callbacks, and value callbacks.
Reject conflicting config fields instead of silently overriding them.
Provide `define`, `prepare`, `spawn`, `publish`, `unpublish`, `lookup_definition`, `bind`, `unbind`, `lookup_actor`, `lookup_session`, and `post`.
Provide `run(now_ns:, work_units: 1000, actor_quantum: 100)`, `next_deadline`, `stats`, and `inspect_stream`.
Provide `io_next`, `receive`, `receive_error`, `take_errors`, `shutdown`, and `close`.
Expose `raw` for advanced procedural operations.

Use these signatures for methods shared with other class assignments:

| Method | Result |
| --- | --- |
| `define(specification)` | Definition |
| `prepare(raw_input)` | Definition |
| `spawn(definition, **raw_spawn_fields)` | Actor |
| `publish(key, definition, expected_revision: 0)` | nil |
| `unpublish(key, expected_revision: Raw::TA_REVISION_ANY)` | nil |
| `lookup_definition(key)` | Owned Definition |
| `bind(name, actor, expected: nil)` | nil |
| `unbind(name, expected:)` | nil |
| `lookup_actor(name)` | Actor for the current registered incarnation |
| `lookup_session(sessionid)` | Actor for that exact session |
| `post(target, name_or_message, **message_options)` | Accepted message ID |
| `inspect_stream(**raw_inspect_filters)` | Inspection |
| `io_next` | IORequest or nil |
| `receive(processor:, destination:, event:)` | nil |
| `receive_error(processor:, destination:, sendid: nil)` | nil |
| `take_errors` | Array of captured callback exceptions |
| `shutdown` | nil after the request succeeds |
| `close` | nil after destruction succeeds |

Spawn fields retain C names, including seed, initial_bytes, parameters, name, limits, and trace.
Wrap seed as a native Raw::Value when supplied. Keep omitted seed absent.
Message options follow Message.new. Event input for receive accepts Event or a raw event Hash.
Snapshot and Inspection use the System's DataModel snapshot callable when native data requires copying.
Expose that callable to those classes through `snapshot_policy`.

## Required behavior

1. Use real C configuration defaults before applying explicit overrides.
2. Keep allocator, arena, callbacks, and DataModel alive with the raw system.
3. Pass supplied time and work budgets exactly. Do not start a background scheduler.
4. Return the complete run record and treat TA_AGAIN as a bounded-run result.
5. Return nil from io_next only for TA_EMPTY.
6. Return nil from next_deadline only for TA_TIME_NEVER.
7. Convert destination Actors and Targets without losing system identity.
8. Preserve ingress metadata through receive. Keep host post origin-free.
9. Use the configured native snapshot policy for snapshots and transport observations.
10. Keep shutdown as a request. Make close fail with TA_BUSY while resources remain.
11. Close the owned root arena only after successful system destruction.
12. Leave transport registration fixed after system creation as required by the C API.

## Acceptance checks

- Run a complete request/reply example through the public convenience API.
- Advance a delayed reply with explicit nanosecond time.
- Keep two systems isolated despite matching numeric actor IDs.
- Exercise named routing, publication replacement, and stable upgrades.
- Drain real SEND, START, and CANCEL leases outside callbacks.
- Verify failed close preserves the system and later cleanup succeeds.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
