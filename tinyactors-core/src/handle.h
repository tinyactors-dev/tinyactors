/**
 * @file handle.h
 *
 * # Generation handles
 *
 * A generation handle names a run-time object by the slot it occupies in
 * a table and the slot's generation, so a stale handle to a slot that was
 * freed and reused is told apart from the object that lives there now.
 * This unit is the one place that packs and unpacks them: a 64-bit value
 * with the slot in the low 32 bits and the generation in the high 32
 * bits, `(generation << 32) | slot`.
 *
 * ## Contract
 *
 * - `ta_handle_make(slot, generation)` packs; `ta_handle_slot` and
 *   `ta_handle_generation` unpack. They round-trip for every slot and
 *   generation, the boundaries 0 and `UINT32_MAX` included, and the
 *   packing never changes: actor and definition IDs carry it out of core
 *   in the public API, session images and traces.
 * - Generation 0 names nothing: a table starts every slot at generation 1,
 *   so 0 is never a valid handle, and a host may use generation 0 for
 *   "none". `ta_handle_valid` answers whether a handle could name an
 *   object at all; it does not say the object exists. Only the owning
 *   table can say that, by comparing the handle's generation with its
 *   slot's current one.
 * - A handle is a fixed-width integer, never a pointer, so it stays
 *   meaningful while the table it indexes grows or moves.
 * - The owning table keeps each slot's generation and advances it with
 *   `ta_handle_next_generation` when the slot's object is freed, so every
 *   handle to the old object goes stale. The next generation skips 0: a
 *   wrap from `UINT32_MAX` goes to 1, so no table ever issues generation 0
 *   (ADR 0059), however often a slot is reused. After a wrap, a stale
 *   handle names the slot's object again only once the slot has been
 *   freed `UINT32_MAX` times more.
 * - Handles are process-local. What a durable or portable encoding
 *   records about identity is described in
 *   `docs/internal/memory-model.md` ("Handles and run-time IDs").
 *
 * ## Memory
 *
 * Nothing here owns or allocates memory. Every function is pure and may
 * run inside a macrostep.
 *
 * ## Complexity
 *
 * Every function is O(1).
 *
 * ## Failure
 *
 * Nothing fails. A handle whose slot holds no object, or whose generation
 * is not the slot's current one, is answered by the owning table where the
 * handle is used (`TA_ERR_NOT_FOUND` through the public API).
 *
 * ## Users
 *
 * The actor table, for `ta_actor_id` (ADR 0059, which also makes the
 * slot and generation public through `ta_actor_id_make`,
 * `ta_actor_id_slot` and `ta_actor_id_generation`), and the definition
 * table, for `ta_definition_id` (ADR 0043). Both advance a freed slot
 * with `ta_handle_next_generation`. Both public types are
 * `uint64_t`, the type of a handle. Run-time IDs are compact handles, and
 * persistence resolves logical identity instead (ADR 0028).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** A generation handle: the slot in the low 32 bits, the generation in the high 32 bits. */
typedef uint64_t ta_handle;

/**
 * Packs `slot` and `generation` into a handle,
 * `(generation << 32) | slot`. Any slot and generation are accepted; a
 * generation of 0 makes a handle that names nothing. Never allocates; may
 * run inside a macrostep. Does not fail.
 */
static inline ta_handle ta_handle_make(uint32_t slot, uint32_t generation) {
    return ((ta_handle)generation << 32) | slot;
}

/**
 * The slot of `handle`: its low 32 bits. Never allocates; may run inside
 * a macrostep. Does not fail.
 */
static inline uint32_t ta_handle_slot(ta_handle handle) {
    return (uint32_t)(handle & 0xffffffffu);
}

/**
 * The generation of `handle`: its high 32 bits. Never allocates; may run
 * inside a macrostep. Does not fail.
 */
static inline uint32_t ta_handle_generation(ta_handle handle) {
    return (uint32_t)(handle >> 32);
}

/**
 * True when `handle` could name an object: its generation is not 0.
 * False for 0 and for every handle made with generation 0. Says nothing
 * about whether the object exists; the owning table checks that. Never
 * allocates; may run inside a macrostep. Does not fail.
 */
static inline bool ta_handle_valid(ta_handle handle) {
    return ta_handle_generation(handle) != 0u;
}

/**
 * The generation a slot gets after its object at `generation` is freed:
 * `generation + 1`, except that `UINT32_MAX` wraps to 1, never to 0, so a
 * table never issues a handle that names nothing. Never allocates; may run
 * inside a macrostep. Does not fail.
 */
static inline uint32_t ta_handle_next_generation(uint32_t generation) {
    return generation == UINT32_MAX ? 1u : generation + 1u;
}
