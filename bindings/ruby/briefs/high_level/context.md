# TinyActors::Context

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Raw::Context, Event, Target, Message, Send, Error.

## Owned files

- `lib/tinyactors/context.rb`
- `test/high_level/context_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

Context wraps one active Raw::Context. Direct application construction is private.
Expose `state`, `event`, `_event`, `self_ref`, `now_ns`, `phase`, `in_state?`, and scoped byte access.
`_event` aliases `event`.
Provide `send(name_or_send, **options)`, `reply(name, **options)`, `raise(name, **options)`, and `cancel(sendid)`.
Send and reply return the accepted message ID.

## Required behavior

1. Inspect the raw context once per callback and reuse its copied metadata.
2. Check the raw lifetime token before every getter and effect.
3. Build at most one Event wrapper per current callback event.
4. Keep event metadata available in guards and every action phase.
5. For reply, require a nonempty origin and an origintype before constructing a URI target.
6. Preserve failed effect statuses through Error so the callback bridge emits the correct core event.
7. Reject reply target or type overrides. The incoming event defines both.
8. Document native state as borrowed and prohibit mutation in guards and other read-only phases.
9. Do not call high-level System or Actor host operations from execution callbacks.

## Acceptance checks

- Read origin from guard, exit, transition, and entry callbacks.
- Reply to two different senders and verify exact incarnation routing.
- Raise on missing origin without delivering to self.
- Reject saved getters and effects after callback success and exceptions.
- Verify delayed reply scheduling copies the destination.
- Verify a failed reply emits one error.communication.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
