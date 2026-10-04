#pragma once

#include "types.h"

/*
 * Package hash is block D5 of the ring protocol's catalog: the three hash
 * functions core uses, in one place.
 *
 * What it is for. Core hashes in three situations and wants each one to have
 * exactly one implementation:
 *
 *   - bucket keys for tables (atoms, actor addresses, the event-name trie):
 *     ta_fnv1a32, 32 bits, cheap, any distribution that spreads text;
 *   - the content hash of a compiled definition, which is persisted as
 *     definition->content_hash and, when the chart names no revision, written
 *     out as the 16-digit revision fingerprint: ta_fnv1a64;
 *   - integer scrambling: runtime session IDs differ mostly in their low
 *     slot bits, and span IDs are derived from step keys, so both need every
 *     input bit to reach every output bit: ta_mix64.
 *
 * Contract. All three are pure functions of their arguments. They keep no
 * state between calls and read no global. ta_fnv1a32 and ta_fnv1a64 take a
 * running state, not a seed: the result of one call is the state for the
 * next, so hashing "foo" and then "bar" gives the same value as hashing
 * "foobar" in one call. Start from TA_FNV1A32_BASIS or TA_FNV1A64_BASIS.
 * This is why a caller can hash a structure field by field, as the
 * definition compiler does, without building a buffer first.
 *
 * Lifetime. Call. A function owns nothing and keeps no pointer after it
 * returns; "bytes" is only read, and only during the call. The running state
 * a caller threads through is the caller's own value and lives as long as the
 * caller's object: the compiler's state lives for one compile and its final
 * value becomes definition->content_hash (definition lifetime); an atom's
 * hash is stored with the atom entry (system lifetime); a session hash is
 * recomputed on every lookup and stored nowhere.
 *
 * Allocation. Never. There is no allocator parameter, no static mutable
 * state, no failure result. The functions are safe at any point, including
 * inside a macrostep and from the render arena's paths.
 *
 * Invariants. The values are fixed forever. ta_fnv1a64 is persisted
 * (content_hash, revision fingerprints) and ta_mix64 derives span IDs that
 * every host must reproduce, so neither may change; tests/hash_format_test.c
 * pins vectors captured from the code this block replaced. ta_fnv1a32 over
 * integers hashes their in-memory bytes, so its value depends on endianness
 * and is only for in-process tables, never written out.
 *
 * Complexity. ta_fnv1a32 and ta_fnv1a64 are O(size), one xor and one multiply
 * per byte. ta_mix64 is O(1), two multiplies and three shifts.
 *
 * Not here. Neither FNV-1a nor Murmur's finalizer is cryptographic or
 * resistant to chosen input; nothing in core relies on that. Where core
 * needs a pseudo-random sequence rather than a hash (ta_splitmix64 in
 * trace.c) it stays with its one user.
 *
 * Example:
 *
 *     uint32_t bucket = ta_fnv1a32(TA_FNV1A32_BASIS, name.ptr, name.len);
 *
 *     uint64_t state = TA_FNV1A64_BASIS;
 *     state = ta_fnv1a64(state, &length, sizeof(length));
 *     state = ta_fnv1a64(state, text, length);        // same as one call
 *
 *     uint32_t slot = (uint32_t)ta_mix64(session_id);  // spread low bits
 */

/* TA_FNV1A32_BASIS is the FNV-1a offset basis of the 32-bit variant: the state to start a hash from. */
#define TA_FNV1A32_BASIS 2166136261u
/* TA_FNV1A64_BASIS is the FNV-1a offset basis of the 64-bit variant: the state to start a hash from. */
#define TA_FNV1A64_BASIS 14695981039346656037ull

/*
 * ta_fnv1a32 continues a 32-bit FNV-1a hash over size bytes and returns the
 * new state. Start from TA_FNV1A32_BASIS; pass the result back as state to
 * add more bytes. size 0 returns state unchanged, and bytes may be NULL
 * then. It reads bytes[0..size) during the call, keeps nothing, never
 * allocates. The value of multi-byte integers depends on endianness, so do
 * not persist it.
 */
uint32_t ta_fnv1a32(uint32_t state, const void *bytes, size_t size);

/*
 * ta_fnv1a64 is ta_fnv1a32 in 64 bits. Its value is persisted as a
 * definition's content hash and revision fingerprint, so it is fixed
 * forever. size 0 returns state unchanged; bytes may be NULL then.
 */
uint64_t ta_fnv1a64(uint64_t state, const void *bytes, size_t size);

/*
 * ta_mix64 is the finalizer of MurmurHash3's 64-bit variant (fmix64): every
 * input bit reaches every output bit, and ta_mix64(0) is 0. Fixed, because
 * span IDs derive from it and every host must agree. Truncate to 32 bits
 * with a cast for a table index; the low bits are as good as any.
 */
uint64_t ta_mix64(uint64_t value);
