# ADR 0063: An actor is one allocation in a paged table

**Status:** Accepted and implemented; applies ADR 0018 and ADR 0023. See the implementation notes for where the code differs from the text.
**Date:** 2026-09-23

## Context

ADR 0018 sets the target at about four million actors in one process. Two
parts of an actor's storage do not scale well to that target.

**The actor table grows by doubling.** `system->actors` is a `ta_vec` of
16-byte `ta_actor_slot`s (`src/system.h`, `src/actor.h`). When the free list
is empty, `ta_actor_claim_slot()` pushes onto the vector, and `ta_vec`
doubles its capacity by allocating a new block and copying the old one
(`src/types.c`). The spawn that crosses 2,097,152 actors copies 32 MB and,
for a moment, holds 96 MB of table. The cost of the average spawn stays
flat, so the scale test passes, but one spawn pays for all of them. In a
host that shares one thread among many actors, that spawn blocks every
other actor, and every other request, while it copies. A single block that
large is also hard for a small or fragmented heap to supply.

**An actor is three allocations.** `ta_actor_new()` (`src/actor.c`)
allocates:
- the `ta_actor` record;
- the session storage, sized by the definition;
- the data-model instance, sized and aligned by the definition.

All three are sized from the definition when the actor is created, and
`ta_actor_free()` frees them together. They never change size and never
live apart. Three allocations cost three times the allocator calls at
spawn and destroy, and three times the per-block overhead a general
allocator keeps. They also spread one actor over three places in memory,
while every macrostep reads all three.

## Decision

1. **The actor table is paged.** Slots live in fixed pages of
   `TA_ACTOR_PAGE_SLOTS` slots (4,096 by default, 64 KiB per page). A small
   page directory, a `ta_vec` of page pointers, finds them. Slot `s` is in
   page `s / TA_ACTOR_PAGE_SLOTS`, at offset `s % TA_ACTOR_PAGE_SLOTS`.
   - When the free list is empty, the next slot comes from the last page.
     A new page is allocated only when that page is full.
   - Pages are never moved or copied. Only the directory doubles, and at
     four million actors it has about a thousand entries (8 KB).
   - Lookup costs one more load than today: the directory, then the page.
   - Pages are not freed while the system lives, just as slots are not
     today. The free list, the generations and ADR 0062's positions are
     unchanged, because a slot's number is unchanged.
2. **An actor is one allocation.** `ta_actor_new()` computes one layout
   from the definition: the `ta_actor` record, then the session storage,
   then the data-model instance, each at its required alignment. It makes
   one allocation of that total size. `ta_actor_free()` makes one free with
   the same size and alignment.
   - The definition computes the layout once, when it is compiled, next to
     `actor_fixed_bytes` (`src/definition.c`), which already adds the same
     sizes.
   - Pointers into the block (`session_storage`, `datamodel_instance`)
     stay as fields, so no code that uses them changes.
   - Sparse state (ADR 0018) stays a separate, optional allocation. It
     exists only for actors that use uncommon facilities, and its lifetime
     differs.
3. **Measurement.** The scale test (`tinyactors-test/tests/scale_test.c`)
   also records the longest single spawn at each population. That spawn
   must stay within a small factor of the median spawn, where today it
   grows with the population. `allocated_bytes` in inspection reports the
   one block.

## Consequences

- No spawn copies the table. The largest allocation the actor table needs
  is one page, whatever the population.
- Spawn and destroy make one allocator call each for an actor without
  sparse state, instead of three. An actor's hot data is contiguous.
- An allocator that pools by size (for example napi ADR 0014) sees one
  size per definition, which fits a size class well.
- A system whose population shrinks keeps its pages, as it keeps its slots
  today. Freeing empty pages is left until a measurement asks for it.
- Session images are unaffected. They copy the session storage by size,
  wherever it lives.

## Implementation notes

2026-09-23.

- **The table** is `ta_actor_table` in `src/actor.h`: a `ta_vec` of page
  pointers and the count of slots issued. `ta_actor_slot_at()` is the one
  lookup; lookup, claim, release, the actor walk (ADR 0062) and system
  teardown all go through it. A slot past the last one issued is NULL, not a
  read past the end. Slot numbers, generations (ADR 0059), the free list and
  walk positions are unchanged.
- **The layout** is computed by `ta_actor_layout()` in `src/actor.c`, called
  from the compiler once the data model reports its instance size and
  alignment. The definition keeps `actor_block_bytes`, its alignment, and the
  offsets of the session storage and the instance. `actor_fixed_bytes` in
  definition inspection and `allocated_bytes` in actor inspection report the
  block.
- **An instance of size 0** (the null data model, ADR 0057) takes no bytes:
  `datamodel_instance` points at the end of the block and is never
  dereferenced. The old code allocated a 1-byte instance for it, so
  per-actor bytes went down slightly, from about 557 to 555.
- **Replacement, load and reconstruct** need nothing special. Each creates
  its actor with `ta_actor_new()`, so a replacement with another definition
  gets a new block laid out for that definition, and the incumbent's block
  is freed when it is destroyed. Nothing frees the session storage or the
  instance on its own any more.
- **`ta_definition_instance_bytes()` is removed:** only the separate
  instance allocation used it. `ta_align_up()` moved from `continuation.c`
  to `types.h`, now that two modules need it.
- **The measurement differs from §3.** The scale test reports the median
  and the longest single spawn, timed with the thread's CPU clock, but it
  does not bound the longest one. Measured with the old code, the longest
  spawn was 18, 161, 206 and 807 µs at 50,000, 200,000, 1,000,000 and
  4,000,000 actors (median 0.46 µs); with paging it is 6, 15, 39 and
  176 µs. The rest is allocator and page-fault noise at random positions,
  and at one million actors it is as large as the copy it would have to
  catch, so a time bound would pass or fail by accident. The test bounds
  what the copy actually does instead: the largest block core allocates
  while spawning must not grow from one population to the next. With the
  old code that block was 1 MiB at 50,000 actors and 4 MiB at 200,000, so
  the check fails already in the quick run; with paging it is one 64 KiB
  page at every population.
- **The actor walk** reads one more pointer per slot. A page of the walk
  costs about 0.028 µs per actor visited, up from about 0.013, and is flat
  across populations.
