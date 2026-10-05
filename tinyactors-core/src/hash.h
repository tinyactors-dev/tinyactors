/**
 * @file hash.h
 *
 * # Hash functions
 *
 * The two hash functions of core, each defined once: FNV-1a over bytes,
 * in a 32-bit and a 64-bit form, and a fixed 64-bit mixer, the finalizer
 * of MurmurHash3's 64-bit variant. They are fixed functions, not seeded
 * per process: every host and every run derives the same values, so a
 * value computed here may be compared across hosts and runs (span IDs,
 * definition fingerprints) as well as used for buckets.
 *
 * ## Contract
 *
 * - `ta_hash_fnv1a32(hash, bytes, size)` and `ta_hash_fnv1a64` continue
 *   an FNV-1a hash: for each byte, XOR it in, then multiply by the FNV
 *   prime. `hash` is the hash so far; a fresh hash starts from
 *   `TA_HASH_FNV1A32_BASIS` or `TA_HASH_FNV1A64_BASIS`, the FNV offset
 *   bases. Hashing a text in pieces gives the same value as hashing it
 *   whole, when each piece continues from the last. A caller that wants a
 *   seeded or separated hash XORs its seed into the basis.
 * - `bytes` may be NULL when `size` is 0; the hash is then `hash`
 *   unchanged.
 * - `ta_hash_mix64(value)` is a bijection on 64-bit values in which every
 *   input bit reaches every output bit. It maps 0 to 0. Its low 32 bits
 *   are a good bucket hash for IDs that differ mostly in their low bits.
 * - The values are part of what core promises: the test pins the
 *   standard FNV-1a vectors and the mixer's outputs, and span IDs, which
 *   leave core in traces, are built from the mixer.
 *
 * ## Memory
 *
 * Nothing here owns or allocates memory: the bytes belong to the caller
 * and are only read. Every function may run inside a macrostep.
 *
 * ## Complexity
 *
 * The FNV-1a functions are O(size); `ta_hash_mix64` is O(1).
 *
 * ## Failure
 *
 * Nothing fails. NULL `bytes` with a non-zero `size` is a programmer
 * error, not checked.
 *
 * ## Users
 *
 * FNV-1a 32: the atom table, the address table and the event-name table,
 * for their buckets. FNV-1a 64: the compiler, for a definition's content
 * hash and the fingerprint revision written from it (ADR 0077). The
 * mixer: span IDs from step keys (ADR 0067) and the session table's
 * buckets.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** The 32-bit FNV offset basis: the hash of no bytes. */
#define TA_HASH_FNV1A32_BASIS 2166136261u
/** The 32-bit FNV prime. */
#define TA_HASH_FNV1A32_PRIME 16777619u
/** The 64-bit FNV offset basis: the hash of no bytes. */
#define TA_HASH_FNV1A64_BASIS 14695981039346656037ull
/** The 64-bit FNV prime. */
#define TA_HASH_FNV1A64_PRIME 1099511628211ull

/**
 * Continues the 32-bit FNV-1a hash `hash` over `size` bytes at `bytes`
 * and answers the new hash; start from `TA_HASH_FNV1A32_BASIS`. Borrows
 * `bytes` (NULL when `size` is 0). Never allocates; may run inside a
 * macrostep. Does not fail.
 */
static inline uint32_t ta_hash_fnv1a32(uint32_t hash, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    for (size_t index = 0; index < size; index += 1) {
        hash ^= in[index];
        hash *= TA_HASH_FNV1A32_PRIME;
    }
    return hash;
}

/**
 * Continues the 64-bit FNV-1a hash `hash` over `size` bytes at `bytes`
 * and answers the new hash; start from `TA_HASH_FNV1A64_BASIS`. Borrows
 * `bytes` (NULL when `size` is 0). Never allocates; may run inside a
 * macrostep. Does not fail.
 */
static inline uint64_t ta_hash_fnv1a64(uint64_t hash, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    for (size_t index = 0; index < size; index += 1) {
        hash ^= in[index];
        hash *= TA_HASH_FNV1A64_PRIME;
    }
    return hash;
}

/**
 * Mixes `value` with the finalizer of MurmurHash3's 64-bit variant, so
 * every input bit reaches every output bit; maps 0 to 0. Fixed, so every
 * host derives the same span IDs. Never allocates; may run inside a
 * macrostep. Does not fail.
 */
static inline uint64_t ta_hash_mix64(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}
