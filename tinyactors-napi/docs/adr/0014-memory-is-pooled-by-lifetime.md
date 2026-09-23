# ADR 0014: Memory is pooled by lifetime

**Status:** Accepted; refines ADR 0009 and ADR 0011
**Date:** 2026-09-23

## Context

The target is four million actors in one process, with fine-grained actors
such as one actor per websocket connection (tinyactors-core ADR 0018). At
that scale, every allocation on the per-message path matters. Each one is
paid millions of times per second, and it fragments a heap in which millions
of objects die in any order.

The ADRs before this one used a general allocator. ADR 0009 counts
allocations but does not say what supplies memory, so in practice it is
`malloc`. When a post and its reply were traced through core and the planned
addon, the result was about four to six allocator calls per message:

| When | Where | What |
|---|---|---|
| every macrostep | core `interpreter.c` (`ta_sparse`, `ta_compact_sparse`) | sparse state and an internal-event array are allocated at the start and freed at the end, even for a trivial actor |
| every spawn | core `actor.c` | the actor record, session storage and data-model instance |
| actor table growth | core `actor.c` | the table doubles by copying, 32 MB at four million actors |
| delayed sends | core `timer.c` | per-owner send arrays and the owner heap |
| a named destination or origin | core `routing.c` | a string copy |
| every post (as planned) | addon | a mailbox record, a ring chunk, and hash-map growth |
| every value (as planned) | Node-API | `napi_create_reference` |

Arenas alone do not solve this. An arena frees everything at once, but actors
and messages die one at a time. An arena per actor costs at least one chunk
per actor, far more than the "handful of words" of core ADR 0018.

Core's allocator interface helps. `ta_allocator` passes the size and
alignment to `free`, so a pool needs no header per block. Core already uses
`ta_arena` for its own scratch memory, and `ta_arena_reset()` keeps its
blocks for the next use.

## Decision

1. **Every allocation has one of four lifetimes, and each lifetime has one
   strategy:**

   | Lifetime | Examples | Strategy |
   |---|---|---|
   | System | actors, session storage, sparse state, envelopes, value-box records, queue chunks, scheduling-table pages, directory entries | size-class pools over large chunks |
   | Definition | compiled layouts, spec trees | core's arenas (unchanged) |
   | Pump turn | `done`, `fault` and `deadLetter` records waiting for their hooks | a turn arena, reset at the end of each turn |
   | Call | spec decoding, an enumeration page, formatting buffers | a call arena, reset after the call |

2. **The system allocator is a chunk source with size-class pools on it.**
   - The chunk source takes memory from the operating system in large
     chunks (1 MiB by default). It is the only code that calls `malloc` or
     `mmap`.
   - Each size class is a free list threaded through its free blocks.
     Allocation pops a block, and free pushes it back. Classes are multiples
     of 16 bytes up to a maximum (for example 2 KiB), so every block is
     16-byte aligned.
   - A request that is larger than the largest class, or that needs more
     alignment, goes to the chunk source as a dedicated allocation. It is
     counted separately.
   - The same allocator is handed to core as its `ta_allocator`. Core's
     allocations therefore become free-list operations with no core change.
   - A system is used by one thread (ADR 0001), so the pools have no locks.

3. **Chunks stay with the system until `close()`.** Close returns all
   chunks at once (ADR 0012). A long-running system therefore keeps its peak
   memory. `stats()` shows the free bytes in each pool. Returning chunks
   whose blocks are all free is deferred until a measurement asks for it.

4. **Scratch memory is core's `ta_arena`, backed by the chunk source.** The
   turn arena and the call arena are reset, not destroyed, so after
   warm-up they allocate nothing.

5. **Addon structures do not allocate per message:**
   - envelopes come from a pool, and a mailbox is a list threaded through
     them (ADR 0005);
   - mailbox heads live in the scheduling table, so there are no mailbox
     records and no mailbox hash map (ADR 0005);
   - queues are made of chunks taken from the pools (ADR 0009);
   - value boxes are pool records that index one handle table per system
     (ADR 0011);
   - named addresses are core's interned handles, shared by reference
     (ADR 0006, tinyactors-core ADR 0065).

6. **Accounting (ADR 0009).** For each lifetime and each size class,
   `stats()` reports the chunks held, the bytes in use and the bytes free.
   It also reports the number of chunk requests. That count is the test
   signal: after warm-up, a steady workload requests no new chunks.

7. **Core changes that help.** Each is a proposed tinyactors-core ADR:
   - ADR 0063: an actor is one allocation, in a table that grows in fixed
     pages instead of doubling;
   - ADR 0064: a running actor is lent an internal queue from a small
     system cache, so a macrostep allocates nothing for it;
   - ADR 0065: named addresses are reference-counted handles, so keeping a
     delivery copies no strings.

   With pools in place, 0063 and 0064 are optimizations, not
   prerequisites. 0065 also fixes the lifetime of named origins, so named
   routing waits for it (ADR 0006).

8. **What stays outside this design.** V8 and Node-API allocate for
   promises, handle scopes and JavaScript objects. There, the aim is only
   short-lived young-generation objects on the hot path and no handles that
   are kept per actor or per value (ADRs 0002 and 0011).

## Consequences

- `malloc` runs at the rate chunks are needed, not at the rate messages
  arrive. Freeing one block is O(1), with no search and no header.
- Fragmentation stays inside a size class, and objects of one size are kept
  together.
- Memory used at a peak stays with the system until `close()`. The stats
  make this visible.
- Close frees addon memory in O(chunks). Core still walks its actors to
  release their values, but each free it makes is a push onto a free list.
- The step exit criteria can test for zero chunk requests in steady
  state. That result is exact, unlike a timing ratio.
