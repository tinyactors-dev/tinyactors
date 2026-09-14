# TinyActors::Raw::Memory

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap lifetime and owner primitives.
C anchors: `ta_mut_bytes_t`, `ta_arena_alloc`, `ta_context_bytes`.

## Owned files

- `ext/tinyactors_raw/raw_memory.c`
- `test/raw/memory_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

Provide `Memory.allocate(size:, alignment:)` for caller-owned custom allocator storage.
Other Memory objects come from arena allocation, context bytes, or migration views.
Expose `size`, `read(offset, length)`, `write(offset, bytes)`, and `closed?`.
Provide `release` only for caller-owned storage that has no outstanding arena loan.
Make pointer-based construction private.

## Required behavior

1. Check owner lifetime and read/write permission before every access.
2. Keep a memory view within its allocation bounds using overflow-safe arithmetic.
3. Return a copied binary String from read.
4. Write through to native memory only while the scope permits mutation.
5. Invalidate callback byte views after the callback returns.
6. Reject guard, inspection, and other read-only mutations.
7. Pin caller-owned custom allocator chunks until the matching release callback finishes.

## Acceptance checks

- Exercise empty, boundary, out-of-range, negative, and overflowing accesses.
- Keep binary data with NUL bytes intact.
- Verify byte writes change real actor byte state.
- Reject escaped context and migration memory.
- Reject release while an arena holds the allocated chunk.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
