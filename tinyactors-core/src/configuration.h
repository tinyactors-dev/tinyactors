/**
 * @file configuration.h
 *
 * # Configuration
 *
 * A `ta_configuration` is an actor's stored SCXML configuration: the set of
 * its active **atomic** states (final states without children included).
 * Ancestors are not stored; they follow from the definition. A chart with
 * parallel regions has several active atomic states at once, so code that
 * reads a configuration never assumes a single leaf: `In(state)` is true
 * when any active atomic state is `state` or one of its descendants.
 *
 * ## Contract and invariants
 *
 * - **One bit per state ID**, in whole 64-bit words (`bitset.h`). The
 *   compiler sizes every definition's configuration this way and keeps the
 *   result in a `ta_configuration_layout`: `atomic_state_count` is the
 *   number of addressable bits (the definition's state count), `bytes`
 *   the storage in whole words. The compiler refuses a chart whose
 *   configuration would need more than `UINT16_MAX` bytes. Only atomic states' bits are
 *   ever set.
 * - A state ID at or past `atomic_state_count` is outside the
 *   configuration: it reads as inactive, and setting it changes nothing.
 * - The interpreter works on the full active set (atomic states and their
 *   ancestors) while it runs and writes the atomic ones back here between
 *   macrosteps; a session image restores an actor's configuration through
 *   the same calls.
 *
 * ## Memory
 *
 * The unit owns nothing and never allocates, so every function may run
 * inside a macrostep. `storage` points into the actor's session storage
 * (the first region of it, zeroed when the actor is made) and lives as long
 * as the actor; `layout` points at the definition's layout and lives as
 * long as the definition. The late-binding bits and each history slot are
 * sized like the configuration.
 *
 * ## Complexity
 *
 * Testing or setting a state is O(1); clearing is O(bytes).
 *
 * ## Failure
 *
 * Nothing fails. `config` and its `storage` and `layout` must be valid: an
 * unchecked precondition, since only the actor's own setup makes one.
 *
 * ## Users
 *
 * The resident actor (its session storage, `In(state)`, inspection and
 * restore), the interpreter (loading and storing the active set, the
 * occupancy counts), the compiler (sizing), and history storage (sized
 * like it).
 */
#pragma once

#include "../include/tinyactors.h"

/*
 * A definition's configuration layout: `atomic_state_count` addressable
 * bits in `bytes` of whole 64-bit words, aligned to `alignment`.
 */
typedef struct {
    uint16_t bytes;
    uint16_t alignment;
    uint32_t atomic_state_count;
} ta_configuration_layout;

typedef struct ta_configuration ta_configuration;
struct ta_configuration {
    void *storage;
    const ta_configuration_layout *layout;
};

/**
 * Whether `atomic_state` is active. False for a state ID outside the
 * layout. Borrows `config`; never allocates; may run inside a macrostep.
 */
bool ta_configuration_contains_atomic(const ta_configuration *config,
                                      ta_state_id atomic_state);
/**
 * Marks `atomic_state` active or inactive. A state ID outside the layout
 * changes nothing. Borrows `config`; never allocates; may run inside a
 * macrostep.
 */
void ta_configuration_set_atomic(ta_configuration *config,
                                 ta_state_id atomic_state,
                                 bool active);
/**
 * Marks every state inactive by zeroing the storage's `bytes`. Borrows
 * `config`; never allocates; may run inside a macrostep.
 */
void ta_configuration_clear(ta_configuration *config);
