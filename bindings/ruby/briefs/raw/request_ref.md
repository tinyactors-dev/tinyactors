# TinyActors::Raw::RequestRef

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap, records, callbacks, Result.
C anchors: `ta_request_ref_t`, `ta_io_request_t`, `ta_io_route_t`.

## Owned files

- `ext/tinyactors_raw/raw_request_ref.c`
- `test/raw/request_ref_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`RequestRef.new(system, id)` creates an immutable system-scoped request identity.
Expose `system`, `id`, equality, and hash.
`Raw.ta_io_next` returns a Result containing the complete copied request record.
Its `:request` field contains a RequestRef. The native lease remains outstanding.

Own these Raw singleton procedures:

- `TinyActors::Raw.ta_io_next`
- `TinyActors::Raw.ta_io_complete`
- `TinyActors::Raw.ta_io_receive`
- `TinyActors::Raw.ta_io_receive_error`

Read each exact signature and ownership comment in the public header before implementation.

## Required behavior

1. Keep the system alive through every taken request lease.
2. Copy origin, origintype, sendid, invokeid, body, trace, and timing fields.
3. Complete a lease only through ta_io_complete with an allowed result status.
4. Preserve TA_EMPTY and duplicate-completion TA_STALE.
5. Pass ingress event metadata through without authenticating user-supplied invokeid.
6. Keep byte/native payload and ordered parameter semantics intact.
7. Do not complete a request implicitly when its Ruby wrapper becomes unreachable.

## Acceptance checks

- Read all metadata from a real SEND lease and complete it once.
- Verify a second completion returns TA_STALE.
- Keep a lease after sender termination and verify metadata remains readable.
- Preserve supplied ingress origin and sendid.
- Exercise START/CANCEL ordering and destruction while a lease exists.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
Update only the assigned API ledger entries after their behavioral tests pass.
