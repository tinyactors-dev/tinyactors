# ADR 0093: The event-block cache fills on demand up to its bound

**Status:** Superseded by ADR 0099 (2026-10-04, implementation pending): the event-block cache goes; the pool and the name area fill on demand up to their bounds in the same way. Accepted and implemented (2026-09-29); see the implementation notes. Amends ADR 0092 (the event-block cache is created up front) and refine ADR 0069 (event blocks) and ADR 0064 (the cache of blocks lent to running actors).
**Date:** 2026-09-29

## Context

An actor that holds an event keeps it in an event block (ADR 0069). A
system keeps up to `eventBlockCache` blocks for reuse, so a delivery takes
a cached block instead of calling the allocator. ADR 0092 made the cache a
parameter and **created every block at creation**, following its rule 3
for buffers every run needs.

Implementing napi ADR 0035 showed what that costs:
- **With core's default of 4 blocks,** a warm napi system made one allocator
  call per delivery past the fourth: 16,320 in `scale.js`'s steady state.
  `scale.js` and `allocations.js`, which require zero allocations per
  event, failed.
- **So napi kept its old default,** which ties the cache to `messageBudget`
  (1,024). Prefilled at creation, that is about 811 KB per system, most of
  napi's roughly 1 MB RSS per system. A quiet system, or one of the many
  systems a test suite creates, pays it for blocks it never uses.

The cache is not like interpreter region 0, which every run needs whole.
How many blocks a system needs depends on how many actors hold an event
at once. That depends on its load, not on its structure. ADR 0092's
amended rule 3 already allows a second shape for such buffers:
*allocated on first use, up to their bound, and kept.* Nested interpreter
regions use it.

## Decision

**The event-block cache fills on demand, up to `eventBlockCache`, and
keeps what it has.**

- **At creation the cache is empty.** A delivery that finds no cached block
  allocates one. That is the existing path, with its existing
  `TA_ERR_NOMEM` (ADR 0069).
- **A block released when a macrostep ends goes back to the cache** while
  the cache holds fewer than `eventBlockCache`. Otherwise it is freed.
- **So a system allocates blocks only while its concurrency rises to a new
  peak.** Once warm, a delivery allocates nothing, as long as the number of
  actors holding an event stays within the bound.
- **The parameter's meaning becomes "at most this many kept",** not "this
  many created".
- **Its default changes from 4 to 1,024,** the number of actors napi resumes
  in one turn (`resumesPerTurn`). It is safe to raise, because blocks are
  only created as they are needed.
- **napi stops deriving it from `messageBudget`,** and uses core's default
  unless the application sets it.

### Alternatives considered

| | A: fill on demand up to the bound, and keep (chosen) | B: a small eager cache (for example 64 blocks) |
|---|---|---|
| Memory for a quiet system | only the blocks its peak needed | 64 blocks, about 50 KB, always |
| Allocations for a busy system in steady state | zero once warm | one per delivery past 64, until someone raises the parameter |
| napi's zero-allocation tests | pass with the default | fail unless the tests raise the parameter |
| Rule 3 of ADR 0092 | the "first use, up to the bound, kept" shape nested regions use | the "at creation" shape |
| Cost | warm-up allocates, up to the peak; a system that was once very busy keeps its peak | every growing application has to tune the parameter |

**Why A.** It keeps both properties the two failed options each gave up:
memory proportional to use, and no allocation per delivery in steady state.
B moves the cost onto every application that grows. And the benchmark's
zero-allocation guarantee would hold only for workloads within 64 blocks.

## Consequences

- **A default napi system no longer reserves about 811 KB.** Its cache grows
  to its peak concurrency, and stays there.
- **Allocator calls per event stay at zero** in `scale.js` and
  `allocations.js` once warm, with the default.
- **A system's memory for event blocks is bounded by `eventBlockCache` ×
  the block size,** reached only if that many actors hold events at once.
  Blocks beyond the bound are freed when released, as today.
- **Costs:**
  - per system at creation: 4 blocks (about 3 KB) fewer than ADR 0092;
  - per actor and per event: unchanged.

  The implementation measures RSS per default napi system before and
  after.

## Implementation notes

- **Where:** `src/system.c` (creation no longer fills the cache),
  `src/interpreter.c` (`ta_interpreter_block_cache_fill` removed; the take
  and settle paths already made a block on demand and kept one while there
  was room), `src/system.h` and `include/tinyactors.h` (the comments, and
  `TA_PARAM_DEFAULT_EVENT_BLOCK_CACHE` 4 → 1024). napi: `lib/params.js` no
  longer derives the cache from `messageBudget`; `index.d.ts` says what the
  parameter now means. Commit `ee1db2c`.
- **Tests:** `tests/params_test.c` (`test_block_cache_on_demand`, with a
  counting allocator):
  - creation allocates the same with a bound of 0, of 3 and the default;
  - concurrency rising to 2 makes 2 blocks, and then nothing;
  - rising to 5 with a bound of 3 makes 3 more and frees 2 on release;
  - two rounds within the bound allocate nothing;
  - rounds past the bound take and free a block per delivery past it;
  - out of memory on the on-demand path answers `TA_ERR_NOMEM`, the actor
    is unchanged, and a retry is delivered (ADR 0069).

  The test host's `scale_test` batched-delivery check now reads "0.00
  allocator calls per event with the default cache (1024), 0.98 with a cache
  of 4; creation allocated 97,272 bytes with either bound, 97,272 with
  none". napi's `scale.js` and `allocations.js` pass with the default.
- **Memory per default napi system** (200 systems, M4 Max): native bytes
  908,472 → 97,464; RSS about 1.05 MB → 124 KB on Node and 120 KB on Bun.
  The cache was most of both.
- **Warm-up is now measured.** The blocks that creation made before, a
  system now makes as its concurrency first rises: about 1,024 allocator
  calls once, for a system that reaches the default bound. napi's tracing
  bench measures from the first delivery, so its `alloc/event` reads 0.01
  (about 1,024 calls over the run's events) where it read 0.00. Steady state
  is still zero, which the three tests above check after their warm-up.
