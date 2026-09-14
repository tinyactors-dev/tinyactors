# TinyActors::Send

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Action, Target, Message, CodeRegistry, Error.

## Owned files

- `lib/tinyactors/send.rb`
- `test/high_level/send_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`Send < Action` represents an actor-originated send.
`Send.new(name, target: Target.self, data: nil, payload: :native, type: nil, delay_ns: 0, sendid: nil)` accepts literals or supported callable expressions.
`Send.reply(name, **options)` derives target and type from the current event.
`compile(registry)` returns a core send action or a custom action when native conversion requires one.
`evaluate(context)` returns the evaluated raw ta_send_t record for Context#send.

## Required behavior

1. Evaluate event, target, type, payload, delay, and sendid expressions once per action.
2. Keep literal and expression variants distinct until compilation.
3. Use Context#reply semantics for missing origins, including an explicit Error.
4. Capture a delayed reply destination during action execution.
5. Leave delayed registered-name resolution to the core.
6. Preserve sendid as cancellation metadata and message_id as the runtime identifier.
7. Do not allow a reply descriptor to override its target or processor type.

## Acceptance checks

- Reply using both an action Proc and a declarative Send.reply.
- Send to different origins in successive callbacks without retaining a context.
- Verify delayed replies reach separate senders after intervening events.
- Exercise cancellation, timer overflow, and invalid destinations.
- Verify absent-origin replies do not send to self.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
