#pragma once

#include "types.h"

/*
 * Package handle is block E1 of the ring protocol's catalog: the one place
 * that knows how a generation and a slot become a 64-bit ID.
 *
 * What it is for. Actors and definitions live in slot tables that reuse
 * freed slots. A bare index would then name a different occupant after a
 * reuse, so every ID carries the slot's generation too:
 *
 *     handle = (generation << 32) | slot
 *
 * ta_actor_id and ta_definition_id are both uint64_t and both use this
 * layout (ADR 0059, ADR 0043), so they pass as a ta_handle without a cast.
 * The layout is persisted in traces and snapshots and visible to every
 * host; it never changes. The public ta_actor_id_slot, _generation and
 * _make are one-line calls into this block, so the layout exists once.
 *
 * Contract. Every function is pure and O(1). Any (slot, generation) pair is
 * representable. Generation 0 is never issued, so the zero handle, and any
 * handle with generation 0, was never a real ID. Slot 0 is a valid slot.
 *
 * Staleness. Freeing a slot stores ta_handle_next_generation(current) in it,
 * so every handle issued before the free stops matching: the table answers
 * NOT_FOUND (actors) or NULL (definitions) through ta_handle_matches. A
 * stale handle is still a perfectly good value that unpacks to the same slot
 * and generation; only the table can say it is stale. A slot can be reused
 * 2^32 - 2 times before its generation wraps back to a value an old handle
 * may hold (ADR 0059 accepts this).
 *
 * Lifetime. A handle is a value with call lifetime: it owns nothing and
 * points at nothing, and is copied freely, stored wherever its holder keeps
 * it. The slot it names lives as long as its table, which is system
 * lifetime for the actor and definition tables; a slot is never returned to
 * the allocator before ta_system_destroy, only recycled with a new
 * generation.
 *
 * Allocation. Never. No function takes an allocator, none can fail, none
 * touches the heap, a ta_layout or any global. They are safe at any point,
 * including inside a macrostep.
 *
 * Example:
 *
 *     ta_handle id = ta_handle_make(slot, entry->generation);   // issue
 *     if (ta_handle_matches(id, entry->generation)) { ... }     // look up
 *     entry->generation = ta_handle_next_generation(entry->generation); // free
 *     // ta_handle_matches(id, entry->generation) is now false: id is stale.
 */

typedef uint64_t ta_handle;

/*
 * ta_handle_make packs a slot and the generation that slot holds. Any pair
 * is representable; (0, 0) is the zero handle, which is never valid.
 */
ta_handle ta_handle_make(uint32_t slot, uint32_t generation);

/* ta_handle_slot returns the low 32 bits: the index of the slot in its table. */
uint32_t ta_handle_slot(ta_handle handle);

/* ta_handle_generation returns the high 32 bits: the generation the slot had when the handle was issued. */
uint32_t ta_handle_generation(ta_handle handle);

/*
 * ta_handle_valid reports whether the handle can have been issued: its
 * generation is not 0. Slot 0 with generation 1 is valid. It says nothing
 * about whether the slot still holds the thing; only the table knows that.
 */
bool ta_handle_valid(ta_handle handle);

/*
 * ta_handle_matches reports whether the handle is valid and its generation
 * equals current_generation, the one now stored in its slot, i.e. the handle
 * is not stale. The caller has already checked that the slot is in range.
 */
bool ta_handle_matches(ta_handle handle, uint32_t current_generation);

/*
 * ta_handle_next_generation returns the generation a slot takes when it is
 * freed: current + 1, except that UINT32_MAX (and 0) go to 1, so a slot is
 * never issued generation 0. The caller stores it at the moment it frees the
 * slot, which is what makes older handles stale.
 */
uint32_t ta_handle_next_generation(uint32_t current_generation);
