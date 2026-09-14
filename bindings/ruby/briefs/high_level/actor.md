# TinyActors::Actor

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Raw::ActorRef, Definition, Target, Message, Snapshot, Inspection, Error.

## Owned files

- `lib/tinyactors/actor.rb`
- `test/high_level/actor_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`Actor` holds its owning high-level System and Raw::ActorRef.
Expose `system`, `ref`, and `id`.
Provide `post(name_or_message, **options)`, `send` as a documented host-post alias, `stop`, `release`, `snapshot`, `set_limits`, and `upgrade`.
Provide `to_target` for exact-instance actor sends from a Context.
For snapshot, collect one actor's requested detail records before constructing the Snapshot.
`snapshot` requests data, active states, history, and the last event.
`set_limits(raw_limits)` preserves the C limit fields.
`upgrade(definition, expected_revision: Raw::TA_REVISION_ANY, &migrate)` delegates to the raw upgrade procedure.
The migration block receives the copied raw migration record and its scoped mutable memory.
Wrap the block's native output through the configured DataModel.

## Required behavior

1. Keep this object bound to one actor incarnation.
2. Never resolve the registered name again when posting through an Actor.
3. Document that host post/send has no actor origin.
4. Use Context#send or Context#reply inside actor callbacks.
5. Keep stop asynchronous and release explicit after terminal state.
6. Assemble snapshots through the real inspection API, including active states and last event.
7. Preserve identity across upgrade and expose failed migration without changing the actor.
8. Check system ownership before operations with another actor or definition.

## Acceptance checks

- Post to the original Actor after its name is rebound.
- Reject Actor operations after release.
- Inspect the last event origin through a durable snapshot.
- Verify a host send has no fabricated origin.
- Upgrade a stable actor and verify rollback after migration failure.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
