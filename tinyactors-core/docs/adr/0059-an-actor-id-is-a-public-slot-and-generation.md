# ADR 0059: An actor ID is a public slot and generation

**Status:** Accepted and implemented; refines ADR 0028. See the implementation notes.
**Date:** 2026-09-23

## Context

`ta_actor_id` is a `uint64_t`. Internally it is
`generation << 32 | slot` (`ta_actor_lookup`, `src/actor.c:8`). The slot
indexes the actor table, and the table entry stores the slot's current
generation, which increases every time the slot is released. A stale ID fails
with `TA_ERR_NOT_FOUND` because its generation no longer matches.

The layout is not public, so bindings must carry the whole 64-bit value.
In JavaScript that means a `BigInt` per ID: a heap allocation for every
value, with slow comparison and hashing. That cost multiplies at the
four-million-actor target (ADR 0018). The two halves each fit in 32 bits,
and a binding could carry them as two small integers, which V8 and JSC store
without allocating. But it can only do that if core promises the layout.

`ta_definition_id` already documents the same slot/generation layout in the
header (ADR 0043). The actor ID does not.

## Decision

1. **The layout of `ta_actor_id` is public and stable:** the slot in the low
   32 bits and the generation in the high 32 bits. Zero is never a valid
   ID. The header documents this next to `ta_actor_id`, in the same way as
   for `ta_definition_id`.
2. **Core provides pure functions to split and join IDs**, so hosts do not
   write the bit arithmetic themselves:

   ```c
   TA_API uint32_t ta_actor_id_slot(ta_actor_id id);
   TA_API uint32_t ta_actor_id_generation(ta_actor_id id);
   TA_API ta_actor_id ta_actor_id_make(uint32_t slot, uint32_t generation);
   ```

   These functions take no system argument and cannot fail. A made ID is
   checked when it is used, like any other ID: a wrong generation gives
   `TA_ERR_NOT_FOUND`.
3. **Generation 0 is never issued.** Slots start at generation 1 (as they
   already do), and a wrap from `UINT32_MAX` skips 0. Hosts can therefore
   use generation 0 as "no actor".
4. **Stale IDs are detected at full strength.** A binding that carries both
   halves keeps all 32 generation bits. A stale ID aliases a new actor only
   after the same slot has been reused 2^32 − 1 times.

## Consequences

- Bindings can represent an actor as two small integers, or as two
  `Uint32Array` lanes in bulk, without losing stale-ID detection.
- The layout is a documented contract that every caller in this repo
  relies on: bindings, the test host, and the paged walk's position
  (ADR 0062). The libraries are private (root `AGENTS.md`), so a change to
  the slot or generation width is made in place, with every caller
  migrated in the same change, and recorded here.
- Session images and other durable formats are unaffected. Runtime IDs are
  still process-local handles (ADR 0028).

## Implementation notes

- **The functions** are declared in `include/tinyactors.h` next to the
  documented layout of `ta_actor_id`, and defined in `src/actor.c`. Core
  uses them wherever it split or joined an actor ID by hand: the lookup,
  claiming and releasing a slot (`src/actor.c`), and system teardown
  (`src/system.c`). Definition IDs keep their own arithmetic (ADR 0043).
- **Decision 3 was violated before this landed.** Releasing a slot did
  `generation += 1`, which wraps from `UINT32_MAX` to 0. It now wraps to
  1. `tests/actor_id_test.c` sets a free slot's generation to
  `UINT32_MAX`, spawns into it, destroys that actor, and checks that the
  next occupant gets generation 1, that the `UINT32_MAX` ID and a made
  generation-0 ID are stale, and that the slot's very first ID names the
  slot again: the aliasing decision 4 accepts after 2^32 − 1 reuses.
- **Tests of the public behaviour** are scenarios in
  `tinyactors-test/ruby/scenarios/actor_ids.rb`, over the generated RPC
  (`ta.actor.id_slot`, `ta.actor.id_generation`, `ta.actor.id_make`):
  split and join round-trip on real IDs, a wrong or zero generation is
  not found, and a destroyed actor's ID stays stale while the slot's next
  occupant has a newer generation.
- **ADR 0062's position** is now documented as the public slot, not an
  opaque value that happens to be the slot.
- **napi** did not split or join actor IDs yet, so nothing there changed.
  Its `actorIds()` pairs (napi ADR 0002) can rely on this layout.
