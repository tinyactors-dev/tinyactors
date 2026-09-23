# ADR 0059: An actor ID is a public slot and generation

**Status:** Proposed; refines ADR 0028
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
- The layout becomes part of the ABI. A change to the slot or generation
  width is a breaking change and needs its own ADR.
- Session images and other durable formats are unaffected. Runtime IDs are
  still process-local handles (ADR 0028).
