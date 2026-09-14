# TinyActors::Raw::Definition

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap, records, callbacks, Result, Value.
C anchors: `ta_definition_t`, `ta_definition_input_t`, `ta_definition_result_t`.

## Owned files

- `ext/tinyactors_raw/raw_definition.c`
- `test/raw/definition_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

Definition is an opaque handle with a counted owned-reference balance.
Build and prepare return a Result whose value is the full ta_definition_result_t record.
A lookup returns a new owned reference.
Direct construction is private.

Own these Raw singleton procedures:

- `TinyActors::Raw.ta_definition_prepare`
- `TinyActors::Raw.ta_definition_build`
- `TinyActors::Raw.ta_definition_retain`
- `TinyActors::Raw.ta_definition_release`
- `TinyActors::Raw.ta_definition_revision`
- `TinyActors::Raw.ta_definition_description`
- `TinyActors::Raw.ta_definition_footprint`
- `TinyActors::Raw.ta_definition_publish`
- `TinyActors::Raw.ta_definition_unpublish`
- `TinyActors::Raw.ta_definition_lookup`

Read each exact signature and ownership comment in the public header before implementation.

## Required behavior

1. Convert nested charts and normalized tables through the record adapter.
2. Preserve COPY and BORROW ownership exactly, including the storage release hook.
3. Root code_owner and every inline definition for the required lifetime.
4. Copy failure diagnostics before another call changes scratch memory.
5. Track explicit retain/release operations without leaking or over-releasing references.
6. Keep publication and actor ownership separate from the caller-owned reference.
7. Reject cross-system definitions before passing pointers into C.

## Acceptance checks

- Build a chart with a false guard and distinct action tokens.
- Round-trip description and prepare while keeping inline definitions alive.
- Verify failed builds preserve diagnostics.
- Verify BORROW storage survives GC until its release hook.
- Release the caller reference while an actor or publication still uses the definition.
- Reject duplicate release without touching freed storage.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
Update only the assigned API ledger entries after their behavioral tests pass.
