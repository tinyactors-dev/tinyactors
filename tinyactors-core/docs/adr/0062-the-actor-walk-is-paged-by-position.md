# ADR 0062: The actor walk is paged by position

**Status:** Accepted and implemented; refines ADR 0018, applies ADR 0059. See the implementation notes. ADR 0078 walks definitions by page in the same model, returning IDs instead of visiting.
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
ADR 0059 makes the slot and generation public (`ta_actor_id_slot()`,
`ta_actor_id_generation()`, `ta_actor_id_make()`).

## Decision

### 1. One walk, taking a params struct

```c
/* The position of a finished walk: nothing is left to visit. */
#define TA_ACTOR_VISIT_END ((uint32_t)UINT32_MAX)

typedef struct {
    ta_actor_inspector inspector;
    /* Where to start: 0 for the first page, then the previous call's `next`. */
    uint32_t from;
    /* Visits after which the call returns TA_OK; 0 means no limit. */
    uint32_t limit;
    /* Where the next call starts, or TA_ACTOR_VISIT_END. Written whatever
     * the result; may be null for a walk of everything. */
    TA_OUT TA_OPTIONAL TA_ALWAYS uint32_t *next;
} ta_actor_visit_params;

TA_API ta_result ta_system_visit_actors(ta_system *system,
                                        TA_IN const ta_actor_visit_params *params);
```

`ta_system_visit_actors` takes the params struct, and there is one walk. A
walk of everything is `from` 0 and `limit` 0, and may leave `next` null; a
page passes a start, a limit and `next`. Core's rule of at most three
parameters means a resumable walk needs a params struct, and the library is
private (root `AGENTS.md`), so the function changed in place and its
callers moved with it.

A page limit is part of the call, not left to the inspector. A page that is
full is not an error, so an inspector should not have to return a made-up
failure to end it, and the caller should not have to tell "page full" apart
from a real failure.

### 2. The position

- A position is a `uint32_t`. The walk visits resident actors in increasing
  position order, starting at `from`.
- A position is an actor's slot, `ta_actor_id_slot()` of its ID (ADR
  0059). It is not a separate opaque value: a host that holds an actor can
  resume a walk at it with `from = ta_actor_id_slot(id)`, and the
  `[slot, generation]` pairs a binding builds come from the same IDs the
  walk visits. No slot is `TA_ACTOR_VISIT_END`: the free list already uses
  `UINT32_MAX` as its end, and the table cannot reach that many slots.
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
- **Bad arguments** (null `params` or `visit`): `TA_ERR_INVALID`, and
  `next` is written as `from` when there is one.

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

### 7. The order is part of the contract

The walk has always gone through slots in order. It is now promised:
increasing position, for a walk of everything as for a page.

## Consequences

- The napi addon can build a page in O(page size + empty slots) by calling
  `ta_system_visit_actors` with its page size as `limit` and keeping
  `next`. It no longer needs to skip pages or return a fake error.
- A walk can cross a process boundary: the test host's RPC binds the paged
  call directly, and a scenario can walk page by page.
- A failed visit can be retried without skipping the actor.
- Position is the slot. If core ever stores actors differently, the slot
  and the position change together, in place, with every caller in this
  repo (the private-library rule in the root `AGENTS.md`).

## Implementation notes

- **`next` is `TA_ALWAYS`.** The header annotates `next` so that bindings
  return it whatever the result. Without the annotation, the test host
  RPC dropped outputs on a non-`TA_OK` result, and a caller could not learn
  where a failed page stopped.
- **At the limit, `next` is the position after the last actor visited,
  even when no resident actor follows it.** Only a position past the end
  of the table is `TA_ACTOR_VISIT_END`. So a walk whose last page is
  exactly full may take one more call, which visits nothing and answers
  the end. Finding out that nothing follows would mean scanning ahead,
  which a page must not do.
- **Bad arguments:** `next` is written as `from` before the system and
  `visit` are checked, so it is set whenever it is not null.
- **Tests** are in `tinyactors-test`: `ruby/scenarios/actor_walk.rb`
  covers the walk of everything and pages of 1, 7 and more than the population, changes between
  pages with slot reuse on both sides of the cursor, a failed visit, an
  invoked child placed before its parent, and an empty system. The scale
  test measures one page of 64 per actor visited, 3,000 pages from where
  the last ended: 0.015 us at 50,000 residents and 0.012 us at 200,000.
- **ADR 0059** has since landed. The position is documented as the public
  slot, `ta_actor_id_slot()`, in the header, the manual and this ADR; the
  walk itself did not change.

- **2026-09-23: one function.** As first implemented, this ADR added
  `ta_system_visit_actors_from` beside an unchanged
  `ta_system_visit_actors(ta_system *, ta_actor_inspector)`, so that no
  caller would change. Under the rule that the libraries here are private
  and their APIs change in place (root `AGENTS.md`), the second function
  was removed the same day: `ta_system_visit_actors` now takes
  `ta_actor_visit_params`, `next` became optional for a walk of everything,
  and every caller (core's `inspection_test.c`, the test host's `server.c`
  and `scale_test.c`, and its Ruby `visit_actors`, now one entry point with
  paging as optional arguments) moved with it. The two scenario files
  became one.

## Sources

- `tinyactors-napi/docs/adr/0003-singular-calls-are-primary-enumeration-is-paged.md`
- `tinyactors-core/docs/adr/0059-an-actor-id-is-a-public-slot-and-generation.md`
- In-tree: `src/actor.c` (`ta_system_visit_actors`, `ta_actor_lookup`),
  `src/system.h` (the actor table and its free list).
