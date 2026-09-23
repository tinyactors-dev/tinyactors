# ADR 0009: One native toolkit for memory and containers

**Status:** Accepted
**Date:** 2026-09-23

## Context

The first addon wrote its own queue logic twice (runnable and external).
Each had its own head, count, capacity, compaction and `realloc` doubling.
Its decoders freed memory by hand along each error path, and it built
sentinel-terminated `ta_str` arrays with `calloc`. Contiguous queues grown by
doubling copy the whole block at millions of entries. No one could see how
much memory the addon itself used.

## Decision

1. **One small container module** (`src/containers.{c,h}`) provides:
   - `vec`: a growable array;
   - `ring`: a FIFO made of fixed-size chunks, so growing it never copies
     the whole queue;
   - `map`: an open-addressing hash map keyed by `uint64_t`, used for
     mailboxes and the directory;
   - `slice`: a view of a pointer and a length, used in every interface
     that passes a sequence.

   Queues, enumeration pages and tables use these types and nothing else.
2. **Temporary allocations use a scratch arena**, such as decoding a spec or
   building an inspection page. It is reset after each call, so error
   paths free nothing by hand.
3. **All allocations go through one counting allocator per system.** That
   allocator is also handed to core. `system.stats()` reports the addon's
   bytes by category (mailboxes, envelopes, value boxes, scratch high-water
   mark), next to what core reports for each actor.
4. **The addon follows core's C rules where they make sense**: functions
   take at most three parameters, and helpers get their own module instead
   of living in `addon.c`.

## Consequences

- One tested implementation of each container, instead of one per queue.
- The memory an operator sees includes the addon's own queues, not only
  core's actors.
