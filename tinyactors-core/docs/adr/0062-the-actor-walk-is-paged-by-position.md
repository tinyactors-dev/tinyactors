# ADR 0062: The actor walk is paged by position

**Status:** Accepted; refines ADR 0018, prepares for ADR 0059
**Date:** 2026-09-23

## Context

`ta_system_visit_actors(system, inspector)` is core's only way to enumerate
resident actors. It walks the whole population in one call, handing each
actor's inspection to a `ta_actor_inspector` (`{ self, visit }`), and stops
early only when a visit answers something other than `TA_OK`. Its order is
"not part of the contract".

That is enough for an admin endpoint over a small system, and wrong for two
consumers:

- **The Node-API addon** (napi ADR 0003) iterates the population in pages:
  `system.actors()` and `system.actorIds()` fill one page per call and must
  cost O(page size), at four million actors. With today's signature, page
  *k* can only be built by walking again and skipping the first *k* pages,
  and every skipped actor costs a full inspection. The addon cannot build
  pages until core can resume a walk.
- **An RPC or any other process boundary** cannot carry a callback, so a
  remote caller cannot walk at all. A position it can send back can cross.

Internally, actors live in a table of slots, and an actor's ID is
`generation << 32 | slot` (`src/system.h`). Freed slots form a free list
that spawn takes first, so the table only grows when every slot is in use.
ADR 0059 (Proposed) would make the slot and generation public; it is not
implemented.

## Decision

### 1. A new paged call; the old one stays

```c
/* The position of a finished walk: nothing is left to visit. */
#define TA_ACTOR_VISIT_END ((uint32_t)UINT32_MAX)

typedef struct {
    ta_actor_inspector inspector;
    /* Where to start: 0 for the first page, then the previous call's `next`. */
    uint32_t from;
    /* Visits after which the call returns TA_OK; 0 means no limit. */
    uint32_t limit;
    /* Where the next call starts, or TA_ACTOR_VISIT_END. Always written. */
    TA_OUT uint32_t *next;
} ta_actor_visit_params;

TA_API ta_result ta_system_visit_actors_from(ta_system *system,
                                             TA_IN const ta_actor_visit_params *params);
```

`ta_system_visit_actors` keeps its signature and meaning. It is the walk
from position 0 with no limit. No existing caller changes. Core's rule of
at most three parameters means a resumable walk needs a params struct either
way; adding one beside the old call is cheaper than migrating every caller,
and the old call remains the natural one for a walk that is never resumed.

A page limit is part of the call, not left to the inspector. A page that is
full is not an error, so an inspector should not have to return a made-up
failure to end it, and the caller should not have to tell "page full" apart
from a real failure.

### 2. The position

- A position is a `uint32_t`. The walk visits resident actors in increasing
  position order, starting at `from`.
- Today a position is the actor's slot: the low 32 bits of its
  `ta_actor_id`. Callers treat it as opaque and only pass back a `next`
  core gave them, or 0. If ADR 0059 lands, the position is exactly the
  public slot, and `[slot, generation]` pairs come from the IDs the walk
  visits.
- `from == 0` starts a full iteration. `from` at or beyond the end of the
  table, `TA_ACTOR_VISIT_END` included, visits nothing and answers `TA_OK`
  with `next == TA_ACTOR_VISIT_END`.

### 3. What `next` means

- **The walk reached the end:** `TA_OK`, `next == TA_ACTOR_VISIT_END`.
- **The limit was reached:** `TA_OK`, `next` is the position after the last
  actor visited, or `TA_ACTOR_VISIT_END` when that was the last position in
  the table.
- **A visit answered something other than `TA_OK`:** the call answers that
  result, and `next` is the position of that actor. A visit that fails did
  not take the actor. Calling again from `next` offers the same actor
  again, so an inspector that failed for want of memory can retry the page.
- **Bad arguments** (null `params`, `next` or `visit`): `TA_ERR_INVALID`,
  and `next` is written as `from` when `next` is not null.

So `next` is always the first position not yet taken by a visit that
answered `TA_OK`.

### 4. Changes between pages

A walk is a sequence of pages, and the system may change between them.
Pages are snapshots of the moment each was taken:

- An actor that is resident for the whole iteration is visited exactly once.
- An actor destroyed or unloaded before the walk reaches its position is not
  visited. One destroyed after it was visited is not visited again.
- An actor spawned during the iteration is visited if its position is at or
  after the cursor, and not if it is before. Spawn reuses freed slots first,
  so either can happen.
- No position is visited twice in one iteration, because the cursor only
  moves forward. When a slot is freed and reused, the actor visited at that
  position is whichever one was resident when the walk got there. Two actors
  that shared a slot, with different generations, are never both visited
  in one iteration.

A visit may call back into core, including spawning and destroying actors.
The same rules apply as between pages.

### 5. Invoked children

Invoked children are resident actors like any other. They are visited at
their own positions, not grouped under their parent. The walk may reach a
child before its parent.

### 6. Cost

A call costs O(actors visited + empty slots passed). Actors are inspected
only when visited, so skipping costs nothing per actor.

Empty slots exist only between a destroy and the next spawn that reuses the
slot, and the table never shrinks. After a mass destruction, one page may
pass many empty slots. Across a full iteration each slot is passed once, so
a full iteration is O(table size). This is accepted. A caller that needs a
hard bound per call can be given a scan limit later, if a measurement shows
it matters.

### 7. `ta_system_visit_actors`

Its documentation now promises the same order: increasing position. Its
behaviour does not change, since the implementation already walked slots in
order.

## Consequences

- The napi addon can build a page in O(page size + empty slots) by calling
  `ta_system_visit_actors_from` with its page size as `limit` and keeping
  `next`. It no longer needs to skip pages or return a fake error.
- A walk can cross a process boundary: the test host's RPC binds the paged
  call directly, and a scenario can walk page by page.
- A failed visit can be retried without skipping the actor.
- Position is tied to slots. If core ever stores actors differently, the
  position must still be monotone and stable for a resident actor, or this
  ADR changes.

## Sources

- `tinyactors-napi/docs/adr/0003-singular-calls-are-primary-enumeration-is-paged.md`
- `tinyactors-core/docs/adr/0059-an-actor-id-is-a-public-slot-and-generation.md`
- In-tree: `src/actor.c` (`ta_system_visit_actors`, `ta_actor_lookup`),
  `src/system.h` (the actor table and its free list).
