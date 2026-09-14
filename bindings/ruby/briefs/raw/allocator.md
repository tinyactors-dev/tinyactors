# TinyActors::Raw::Allocator

Status: planned. Assignment type: one class.

## Read first

Read [the shared contract](../CONTRACT.md) and `bindings/ruby/AGENTS.md`.
Dependencies: bootstrap and callback infrastructure.
C anchors: `ta_chunk_allocator_t`, `ta_chunk_t`.

## Owned files

- `ext/tinyactors_raw/raw_allocator.c`
- `test/raw/allocator_test.rb`

Paths are relative to `bindings/ruby/`.
The integrator owns central registration and shared helper declarations.

## Required interface

`Allocator.malloc` selects an explicit hosted C allocator.
`Allocator.new(acquire:, release:, user: nil)` creates a callback adapter.
Pass an Allocator to `Raw.ta_arena_create`.
The acquire callback returns a Result containing a chunk record backed by owned Raw::Memory.
The release callback receives the original user object and exact chunk record.
Use `Raw::Memory.allocate(size:, alignment:)` for custom allocator chunks.

## Required behavior

1. Keep allocator callbacks and their user object rooted through the last descendant arena.
2. Implement requested alignment and checked size arithmetic in the hosted adapter.
3. Accept custom chunks only when their backing memory outlives the arena loan.
4. Keep the original chunk address, size, and alignment for its matching release.
5. Do not call Ruby from a GC finalizer. Use the shared cleanup protocol.

## Acceptance checks

- Create and close nested arenas using both allocator modes.
- Check actual alignment with a native test helper.
- Inject allocation failures and verify no double release.
- Verify oversized rounded chunks return to the original allocator.
- Run GC.compact while an allocator has live arenas.

Run the test file against the real native runtime where this class crosses Raw.
Do not replace missing dependencies with fake success or no-op callbacks.

## Completion boundary

Implement this class only. Do not add transport adapters or change the C runtime.
Do not add convenience instance methods that duplicate the assigned C procedures.
Report changed files, test commands, results, and remaining limitations.
