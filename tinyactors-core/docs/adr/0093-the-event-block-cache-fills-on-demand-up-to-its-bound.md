# ADR 0093: The event-block cache fills on demand up to its bound

**Status:** Proposed (2026-09-29). Would amend ADR 0092 (the event-block cache is created up front) and refine ADR 0069 (event blocks) and ADR 0064 (the cache of blocks lent to running actors).
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

None yet.
