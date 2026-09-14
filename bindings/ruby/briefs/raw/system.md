# TinyActors::Raw::System

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap, records, callbacks, Result, Arena, Value.
C anchors: `ta_system_config_t`, `ta_run_options_t`, `ta_run_result_t`.

## Owned files

- `ext/tinyactors_raw/raw_system.c`
- `test/raw/system_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

System is an opaque handle created by `Raw.ta_system_create(parent, config)`.
Require an explicit parent Arena at this layer.
Expose `closed?` for lifetime checks.
Use `Raw.ta_system_config_init` to obtain the C defaults before overrides.

Own these Raw singleton procedures:

- `TinyActors::Raw.ta_system_config_init`
- `TinyActors::Raw.ta_system_create`
- `TinyActors::Raw.ta_system_set_limits`
- `TinyActors::Raw.ta_system_run`
- `TinyActors::Raw.ta_system_next_deadline`
- `TinyActors::Raw.ta_system_stats`
- `TinyActors::Raw.ta_system_shutdown`
- `TinyActors::Raw.ta_system_destroy`

Read each exact signature and ownership comment in the public header before implementation.

## Required behavior

1. Copy configuration records and root callbacks for the complete system lifetime.
2. Preserve host_user as the supplied Ruby object.
3. Honor the supplied now_ns and both work budget fields on every run.
4. Return all ta_run_result_t fields without converting TA_AGAIN to an exception.
5. Enforce serialized access and reject same-system callback reentry.
6. Keep a failed TA_BUSY destruction attempt reversible and all handles valid.
7. Invalidate the system only after successful destruction. Keep its parent caller-owned.

## Acceptance checks

- Build byte and native systems with independent state.
- Advance a delayed send using explicit nanosecond timestamps.
- Exercise budgets, clock regression, shutdown, and TA_BUSY destruction.
- Verify callbacks and roots survive GC.
- Verify there is no hidden clock, thread, or I/O loop.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
Update only the assigned API ledger entries after their behavioral tests pass.
