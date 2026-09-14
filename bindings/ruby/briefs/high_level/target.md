# TinyActors::Target

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Raw.ta_target_parse, scoped reference representations, and Error.

## Owned files

- `lib/tinyactors/target.rb`
- `test/high_level/target_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

Provide `Target.self`, `.internal`, `.parent`, `.instance(actor_or_ref)`, `.name(string)`, `.invoke(string)`, and `.uri(string)`.
Provide `Target.parse(string_or_nil)` for raw SCXML target text.
Expose `kind`, `instance`, `text`, and `to_raw` on immutable values.
Construct raw records using exact numeric target constants.

## Required behavior

1. Keep each constructor kind distinct. Never derive kind from the Ruby class name.
2. Keep literal registered names separate from session URIs and invocation IDs.
3. Preserve nil target as the ordinary send-to-self default.
4. Reject empty or invalid text according to the selected target kind.
5. Check system identity when normalizing an Actor or Raw::ActorRef for an operation.
6. Delegate reserved SCXML parsing to Raw.ta_target_parse.
7. Do not resolve named destinations before the core dispatches a delayed send.

## Acceptance checks

- Deliver between two actors through every applicable target constructor.
- Verify Target.uri(sender_origin) reaches the sender.
- Verify malformed and unknown target inputs fail instead of becoming self.
- Verify delayed name routing follows the current name while an instance stays pinned.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
