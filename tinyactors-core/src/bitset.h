/**
 * @file bitset.h
 *
 * # Bitset
 *
 * A bitset is an array of `uint64_t` words in which bit `b` is bit
 * `b % 64` of word `b / 64`. It has no struct: the caller keeps the words
 * and knows how many bits they hold. Core uses bitsets for sets of states
 * and other dense sets of small IDs.
 *
 * ## Contract
 *
 * - `ta_bits_words(bits)` is the number of words that hold `bits` bits.
 * - `ta_bits_get` and `ta_bits_put` read and write one bit. The bit must
 *   lie within the words the caller has: that is a precondition, not
 *   checked.
 *
 * ## Memory
 *
 * Nothing here owns or allocates memory: the words belong to the caller
 * (an actor's state sets, a definition's tables, a configuration). Every
 * function may run inside a macrostep.
 *
 * ## Complexity
 *
 * Every operation is O(1).
 *
 * ## Failure
 *
 * Nothing fails. A bit beyond the caller's words is a programmer error.
 *
 * ## Users
 *
 * State configurations and history, an actor's active and
 * late-initialised states, the builder's and the interpreter's state sets.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** The number of 64-bit words that hold `bits` bits. */
static inline uint32_t ta_bits_words(uint32_t bits) { return (bits + 63u) / 64u; }
/** Whether bit `bit` of `words` is set. */
static inline bool ta_bits_get(const uint64_t *words, uint32_t bit) {
    return (words[bit / 64u] >> (bit % 64u)) & 1u;
}
/** Sets bit `bit` of `words` to `value`. */
static inline void ta_bits_put(uint64_t *words, uint32_t bit, bool value) {
    uint64_t mask = (uint64_t)1u << (bit % 64u);
    if (value) {
        words[bit / 64u] |= mask;
    } else {
        words[bit / 64u] &= ~mask;
    }
}
