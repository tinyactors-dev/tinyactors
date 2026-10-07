# ADR 0005: A freestanding host layer

**Status:** Accepted (2026-10-07).
**Date:** 2026-10-07

## Context

The host layer (`tinyactors-core/src/host`, `ta_host`) runs many systems as
one: a segment (one `ta_system`) per image, routing between segments and to
other systems, chart versions, migration jobs, the hibernation sweep and
growth by relocation. It does no I/O, reads no clock, starts no threads,
and calls back only into the data models (`include/tinyactors/host.h`).

It is still a hosted layer in `libtinyactors_runtime`, and it ties every
embedder to a C library. Counted at `854041fd`:

| What | Where | Count |
|---|---|---|
| `malloc`, `calloc`, `realloc`, `free` | `host.c` (47), `router.c` (28) | 75 |
| `snprintf` | `router.c`: message ids, the message id and key, the JSON tail | 5 |
| `memcpy`, `memset`, `memmove`, `memcmp`, `strlen` | both files | ~60 |
| `<stdlib.h>`, `<string.h>`, `<stdio.h>` | `host.c`, `host_internal.h`, `router.c` | 5 |

Nothing else in it needs an operating system. `ta_allocator` is declared in
`compile.h`, a hosted header, so the freestanding layers cannot name it.

Several allocations have no defined outcome when they fail: the moved-session
table dereferences a failed `calloc`; an effect copied out of a segment, an
`error.communication` for a sender and a retired segment's
`TA_HOST_EFFECT_SEGMENT_FREED` are dropped when their arrays cannot grow; a
string the effect pool cannot hold becomes an empty string; a segment whose
growth fails makes the run report `TA_RUN_FAULT`.

## Decisions

1. **`ta_allocator` moves to a header of its own,
   `include/tinyactors/allocator.h`, unchanged**:
   `{context, alloc(context, size, align), free(context, ptr, size)}`. It
   includes only `types.h`, so every layer can include it, and it is one of
   `libtinyactors`'s public headers. `compile.h`, `host.h` and `runtime.h`
   use it from there. There is no `realloc`:
   to grow, the host allocates, copies and frees, and it keeps the size of
   every block it owns, so `free` always gets the size `alloc` was given.
2. **The host takes the allocator in its configuration.**
   `ta_host_config.allocator` is required: `ta_host_new` and
   `ta_host_restore` refuse a configuration without `alloc` and `free`
   (`TA_ERROR_INVALID`), and the host never falls back to `malloc`. Every
   block the host owns, the segments' regions included, comes from it, and
   `ta_host_free` gives every block back through it. Hosted callers pass
   `ta_runtime_malloc()`; the PostgreSQL extension passes an allocator over
   a memory context.
3. **Allocation failure is a defined outcome everywhere.** A call whose
   allocation is refused returns `TA_ERROR_NO_MEMORY` and leaves the host as
   it was. Where a call does work in steps, it stops at the step it could
   not do, and the next call resumes there:
   - `ta_host_run` reports `TA_RUN_EXHAUSTED` when the allocator refuses a
     block: a segment's bigger region (`detail` = the segment's
     `TA_POOL_*`), or the host's own memory (effects, routing, migration
     jobs, `detail` = `TA_POOL_NONE`). A segment's effects that were not
     handled stay in its outbox (it is acknowledged only up to the first
     effect the host could not take), a migration step waits, a pending
     `error.communication` stays pending, and the next run continues from
     there. Nothing is lost or delivered twice. A segment that cannot grow
     for any other reason (its hard limits) still faults.
   - `ta_host_deliver` refuses an envelope it cannot take with
     `TA_ERROR_NO_MEMORY` and remembers nothing of it (it is not a
     duplicate when the transport delivers it again).
   - `ta_host_peer_up` and `ta_host_peer_remove` return
     `TA_ERROR_NO_MEMORY` with the envelopes they did not flush or fail
     still queued; calling again continues. `ta_host_advance_time` keeps
     an expired envelope it could not report queued, and the next
     `ta_host_run` reports it.
   - Before an irreversible step (exporting an actor to migrate it, freeing
     a retired segment, acknowledging a segment's effects) the host
     reserves what the step needs afterwards (effect slots, string space,
     the moved-session table), so the step cannot fail half done.
   - `ta_host_sweep` allocates nothing: it uses the caller's output array
     as its scratch space.
4. **Formatting moves to base.** `ta_format_u64` leaves
   `src/core/event_pool.c` for `src/base/format.c`, with a bounded text
   writer (`ta_text`) that appends views and decimal numbers and records an
   overflow instead of truncating. The five `snprintf` calls become
   `ta_text` writes; a message that would not fit is an error
   (`TA_ERROR_INVALID`), never a shorter message. Core and
   `ta_address_format` use the same function: one implementation.
5. **Memory functions come from `base/memory.h`**: `ta_memcpy`,
   `ta_memset`, `ta_memcmp`, and a new `ta_memmove` (a builtin, with a
   freestanding fallback). `strlen` gives way to lengths the host has.
6. **The layer becomes freestanding and moves to `libtinyactors`.**
   `host` is marked `freestanding` in `tinyactors-core/mise/core.rb`, so
   `mise run check:freestanding` compiles it for wasm32, x86_64, Cortex-M0+
   and RISC-V without the C library's headers. It moves from
   `libtinyactors_runtime` to `libtinyactors`, with `host.h`.
7. **Nothing else becomes freestanding here.** `compile` and `runtime`
   stay hosted; `runtime` keeps `ta_runtime_malloc` and its conveniences.

## The promise of `libtinyactors`

Before: no C library, no allocation. After:

> **No C library. `base` through `system` never allocate; `host` allocates
> only through the `ta_allocator` it was given.**

## What `compile` would need

Counted at `854041fd`, for a later decision:

- **Formatting.** Diagnostics are `printf`-style (`ta_diagnostics_add`,
  `vsnprintf` over 51 `%s`, 3 `%u`, 3 `%.*s` across about 60 messages), and
  `scxml.c` makes generated names with `snprintf` (6). Both would go to the
  `ta_text` writer, and the diagnostics to a list of parts instead of a
  format string, since `<stdarg.h>` is outside what a freestanding layer
  may include.
- **A default allocator.** `compile.c` falls back to `malloc` when the
  options carry no allocator. As with the host, the allocator would become
  required.
- **Sorting.** `link.c` calls `qsort` once (the event-name trie's
  children); base would need a sort.
- **Strings.** About 80 `strlen`, `strcmp`, `strncmp` and `strchr` calls
  (`compile.c`, `scxml.c`, `xml.c`, `link.c`) over NUL-terminated names in
  the XML document; mechanical, with base helpers or lengths.
- **Memory functions** (`memcpy`, `memset`, `memcmp`): mechanical, as
  here.

Its XML reader (`xml.c`) builds its tree in the compiler's chunked arena
and does no I/O; sources come through the resolver callback. It needs only
the string functions above.

## Consequences

- `libtinyactors` runs a host on every target a system runs on: wasm32
  without wasi-libc, Cortex-M, RISC-V, and inside PostgreSQL on its memory
  contexts. Its wasm module still imports nothing.
- Every caller of `ta_host_new` and `ta_host_restore` passes an allocator.
- A host's memory can be bounded: an allocator over a fixed arena runs it,
  and refusal is backpressure (`TA_RUN_EXHAUSTED`), as a full pool is for a
  system.
- `ta_host_run` reports `TA_RUN_EXHAUSTED` where it reported
  `TA_RUN_FAULT` when a segment could not get memory to grow.
