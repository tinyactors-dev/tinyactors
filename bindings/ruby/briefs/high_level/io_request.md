# TinyActors::IORequest

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: Raw::RequestRef, Event, Message, Invocation, Error.

## Owned files

- `lib/tinyactors/io_request.rb`
- `test/high_level/io_request_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

IORequest wraps one taken raw request record and its owning System.
Expose `ref`, `kind`, `actor_ref`, `revision`, `invocation`, `invokeid`, `type`, `target`, `event`, `body`, `trace`, `accepted_ns`, and `due_ns`.
Provide `complete(status: Raw::TA_OK)` and `completed?`.
Kinds distinguish SEND, START, and CANCEL.

## Required behavior

1. Keep the native lease outstanding until explicit successful completion.
2. Retain the copied raw metadata and native payload roots for their required lifetimes.
3. Preserve source event origin and ordered request body parameters.
4. Complete the request exactly once. Preserve a failed completion status.
5. Allow access to copied metadata after completion without reviving the lease.
6. Do not perform transport work in the runtime callback or a finalizer.
7. Keep invocation identity separate from request identity.

## Acceptance checks

- Read a SEND origin after the source actor terminates.
- Complete a request once and reject a second completion.
- Preserve START/CANCEL ordering and separate tokens.
- Keep body parameter duplicates and byte payloads intact.
- Verify system destruction remains busy while a request is outstanding.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Use Raw procedures only. Do not introduce another native entry point or Ruby execution engine.
Report changed files, test commands, results, and remaining limitations.
