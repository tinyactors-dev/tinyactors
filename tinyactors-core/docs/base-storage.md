# Base storage for embedders and bindings

These APIs serve C embedders and binding authors. Core already supplies
fixed-region storage for actor execution; these optional base utilities
hold the caller's own values and call-scoped conversions. No per-event or
per-actor work is added to the runtime. The caller pays for the utilities
it uses: table capacity (aligned rows plus 8-byte slot records), arena
chunks retained until free, and an immutable status-name lookup. Actor
scheduling, language-value cleanup and the compiler's arena migration are
outside this increment (ADR 0012, decision 5).

All calls on an object are serialized by its owner. A backing allocator
must not reenter or mutate that same object. Allocator structs and contexts
are borrowed; store them for the whole lifetime of their consumers rather
than pass the address of a temporary value.

| Object | Owner | Created and read | Ends |
|---|---|---|---|
| Status-name text | Library | Returned by ta_status_name; read by caller | Library lifetime |
| Table allocation | Table, through caller allocator | First add and growth; rows read through handles | Next growth replaces it; table_free releases the last block |
| Table row/handle | Caller, within table storage | Successful add; get/at/handle resolves it | Remove/free ends both; the next successful add can end row pointers |
| Arena chunks | Arena, through caller allocator | Alloc obtains or reuses a chunk | Only arena_free returns chunks to the allocator |
| Arena allocation | Call that owns its mark | Alloc returns a stable pointer | Release to a preceding mark, or arena_free |
| Arena mark | Owning call on the same thread | Position captures current history | An enclosing release or arena_free |
| Allocator view | Caller stores returned struct | Consumer borrows its address and bumps arena | Consumer finishes before enclosing release/free |

## Status names

Use `ta_status_name(status)` in diagnostics. It returns the exact public
constant name, such as `"TA_ERROR_STATE"`; unknown values return `NULL`.
The result is immutable NUL-terminated library-owned text. The call does
not allocate or name run statuses and application errors.

## Tables

Initialize `ta_table` with a full `ta_table_config`, a nonzero row size and
both callbacks in a persistent `ta_allocator`. Init does not allocate.
Invalid configurations leave an inert table whose add refuses. Row sizes
round up to 8 bytes. Storage initially holds 16 rows and doubles as needed.

`ta_table_add` returns a zeroed row and its 64-bit handle. A refusal
returns `NULL`, writes handle 0, and preserves the table. Store the handle;
reacquire a row through `ta_table_get` after an add, because growth can
move storage. Iterate slot indices below `table.capacity` with
`ta_table_at` and `ta_table_handle`, skipping NULL/0 results.

Remove invalidates the handle and zeros the row. It does not release
objects referenced by that row: the binding must do that first. Stale
removals do nothing. Odd generations name live rows; removing the final
generation retires the slot so no stale handle can become live again.
`ta_table_free` ends the whole handle namespace. Later initialization
starts a new table lifetime, so earlier handles must not be reused.

## Arenas and marks

Initialize a `ta_growable_arena` with a persistent backing allocator and
a regular chunk payload size (0 selects 65536 bytes). Init does not
allocate. Alloc accepts nonzero sizes aligned to 1, 2, 4 or 8; invalid
requests, size/accounting overflow and backing refusal return NULL
without changing existing allocations or marks. Storage is uninitialized.

Every new chunk becomes the active head, including an oversized request's
own chunk. Earlier pointers stay fixed. Take a mark at call entry and
release it on every exit after all consumers finish. Nested calls take
later marks, so their release leaves outer allocations intact.

Release retains all chunks, including oversized ones. Repeating an
identical warmed allocation sequence needs no backing allocator calls.
The arena tries the current chunk and then a best-fitting kept chunk
before obtaining another. `used` counts active payload use including
alignment gaps; `reserved` counts headers and payloads of active and kept
chunks. Different request sizes can still require new chunks.

Marks belong to one active allocation history. Do not use a mark after
releasing an enclosing scope. Detectably foreign or invalid marks are
ignored, but pointer-and-offset marks cannot identify every stale mark
once a chunk is reused. Only `ta_growable_arena_free` returns chunks;
it ends pointers, marks and allocator views, and leaves the configured
arena ready for a new allocation history.

## A CBOR writer using the arena

Store the by-value allocator view while the writer borrows its address:

```c
ta_growable_arena_mark mark = ta_growable_arena_position(&arena);
ta_allocator view = ta_growable_arena_allocator(&arena);
ta_cbor_writer writer;
ta_cbor_writer_init(&writer, (ta_buffer){0}, &view);
ta_cbor_write_text(&writer, ta_string("hello"));
ta_bytes wire;
ta_status status = ta_cbor_writer_bytes(&writer, &wire);
if (status == TA_OK) {
    /* Consume wire now; it is invalid after release below. */
}
ta_cbor_writer_free(&writer);
ta_growable_arena_release(&arena, mark); /* also on every error path */
```

The view's free callback does nothing. A writer's replaced blocks stay in
the arena until release, so budget for its growth history, not just its
final buffer. Free the writer before releasing its mark; the allocator
view itself does not extend the arena or allocation lifetimes.
