/*
 * Implementation of block D5 (hash). The contract, lifetime and usage are in
 * hash.h; this comment records what the code relies on.
 *
 * Lifetime and allocation. Call lifetime only. Nothing here allocates, frees
 * or keeps a pointer: no function takes an allocator, so none can fail.
 *
 * Constants. The FNV prime for 32 bits is 16777619 and for 64 bits
 * 1099511628211. The bases live in hash.h. The mixer is the fixed 64-bit
 * Murmur3 finalizer (shifts of 33, multipliers 0xff51afd7ed558ccd and
 * 0xc4ceb9fe1a85ec53). These values are persisted or visible: content
 * hashes become revision fingerprints, and span and trace IDs derive from
 * ta_mix64, so changing any constant changes IDs that tests compare
 * bit for bit (span_test).
 *
 * Byte order. FNV-1a consumes bytes one at a time in memory order, through
 * an unsigned char pointer, so the result does not depend on the host's
 * endianness or alignment. Multiplication wraps modulo 2^32 or 2^64 by
 * definition of the unsigned types; that wrap is intended, and it is the only
 * arithmetic in the file, so there is no size computation to check.
 *
 * Complexity. ta_fnv1a32 and ta_fnv1a64 are O(size); ta_mix64 is O(1).
 */

#include "hash.h"

uint32_t ta_fnv1a32(uint32_t state, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    for (size_t index = 0; index < size; index += 1) {
        state ^= in[index];
        state *= 16777619u;
    }
    return state;
}

uint64_t ta_fnv1a64(uint64_t state, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    for (size_t index = 0; index < size; index += 1) {
        state ^= in[index];
        state *= 1099511628211ull;
    }
    return state;
}

uint64_t ta_mix64(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}
