# TinyActors::Raw::Arena

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap, records, Result, Allocator, Memory.
C anchors: `ta_arena_t`, `ta_arena_options_t`.

## Owned files

- `ext/tinyactors_raw/raw_arena.c`
- `test/raw/arena_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

Arena is an opaque resource handle. Make direct construction private.
Procedures create owned arenas. Context access can return borrowed arenas.
Expose only lifetime diagnostics such as `closed?` on the handle.

Own these Raw singleton procedures:

- `TinyActors::Raw.ta_arena_create`
- `TinyActors::Raw.ta_arena_open`
- `TinyActors::Raw.ta_arena_alloc`
- `TinyActors::Raw.ta_arena_defer`
- `TinyActors::Raw.ta_arena_close`
- `TinyActors::Raw.ta_arena_stats`
- `TinyActors::Raw.ta_arena_set_limit`

Read each exact signature and ownership comment in the public header before implementation.

## Required behavior

1. Preserve the parent-child lifetime tree and the original allocator.
2. Keep parents alive while descendants, systems, or borrowed views depend on them.
3. Reject close operations that would invalidate a live system or an outstanding native lease.
4. Invalidate descendant memory and arena handles when an allowed parent close succeeds.
5. Return Raw::Memory from allocation. Never return a native address.
6. Root deferred cleanup arguments until execution, including exceptional cleanup paths.

## Acceptance checks

- Close siblings in either order without invalidating the survivor.
- Verify alignment, zero-length allocations, limits, and allocation failure.
- Run every deferred callback once and preserve its argument.
- Reject early parent closure beneath a live system.
- Reject use of memory after its arena closes.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
Update only the assigned API ledger entries after their behavioral tests pass.
