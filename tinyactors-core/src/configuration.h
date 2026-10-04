#pragma once

#include "types.h"

/*
 * Configuration is the set of active states of one actor session.
 *
 * A configuration is a bitset with one bit per state ID. Only atomic and
 * final states are ever set; ancestors are derived from the definition when
 * In(state) is asked, so the set stays small and a parallel's regions are
 * simply several bits set at once. The bitset is stored in whole 64-bit
 * words. There is one layout, so there is no kind to choose and none to
 * switch on.
 *
 * Lifetime. The layout is part of the definition and lives as long as it
 * does (definition lifetime); the compiler fills it once and nothing writes
 * it afterwards. The storage is carved out of the actor's own block at
 * creation (session lifetime) and freed with that block, so a
 * ta_configuration holds borrowed pointers: it is valid while its actor is
 * and its definition is retained, and must not outlive either.
 *
 * Allocation. Nothing in this unit allocates or frees. Every function works
 * on storage the caller already owns, so none can fail for lack of memory.
 *
 * Invariants. layout->bytes is ta_bits_words(atomic_state_count) * 8, and
 * storage points at that many bytes, aligned for uint64_t. A state ID at or
 * beyond atomic_state_count is never active: reads report false and writes
 * are ignored, so a stale or foreign ID cannot touch memory past the set.
 *
 * Complexity. Reading or writing one state is O(1); clearing is O(bytes).
 *
 * Example:
 *
 *	ta_configuration config = { .storage = words, .layout = &definition->configuration };
 *	ta_configuration_clear(&config);
 *	ta_configuration_set_atomic(&config, leaf, true);
 *	bool active = ta_configuration_contains_atomic(&config, leaf);
 */

/*
 * ta_configuration_layout describes the storage of a configuration. `bytes`
 * is its size in whole 64-bit words, in bytes; `atomic_state_count` is the
 * number of addressable bits, which is the definition's state count.
 */
typedef struct {
    uint16_t bytes;
    uint16_t alignment;
    uint32_t atomic_state_count;
} ta_configuration_layout;

/*
 * ta_configuration is a view of one actor's active states: `storage` is
 * borrowed from the actor block and `layout` from its definition.
 */
typedef struct ta_configuration ta_configuration;
struct ta_configuration {
    void *storage;
    const ta_configuration_layout *layout;
};

/* ta_configuration_contains_atomic reports whether `atomic_state` is active.
 * An ID beyond the layout's state count is not active. Never allocates. */
bool ta_configuration_contains_atomic(const ta_configuration *config,
                                      ta_state_id atomic_state);
/* ta_configuration_set_atomic marks `atomic_state` active or inactive. An ID
 * beyond the layout's state count is ignored. Never allocates. */
void ta_configuration_set_atomic(ta_configuration *config,
                                 ta_state_id atomic_state,
                                 bool active);
/* ta_configuration_clear makes every state inactive by zeroing layout->bytes
 * bytes of storage. Never allocates. */
void ta_configuration_clear(ta_configuration *config);
