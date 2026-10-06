/* bitset.h — fixed-width bitsets over u32 words.
 *
 * u32 words keep snapshots identical on 32- and 64-bit hosts. Bit i lives in
 * word i/32, bit i%32. Used for configurations (bit = state in document
 * order), exit/entry sets, history, enabled-transition sets.
 */
#ifndef TINYACTORS_BASE_BITSET_H
#define TINYACTORS_BASE_BITSET_H

#include "base/config.h"

typedef uint32_t ta_bitset_word;

TA_INLINE uint32_t ta_bitset_words(uint32_t nbits) {
    return (nbits + 31u) >> 5;
}

TA_INLINE uint32_t ta_ctz32(uint32_t x) {
    TA_ASSERT(x != 0);
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_ctz(x);
#else
    uint32_t n = 0;
    while (!(x & 1u)) {
        x >>= 1;
        n++;
    }
    return n;
#endif
}
TA_INLINE uint32_t ta_clz32(uint32_t x) {
    TA_ASSERT(x != 0);
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_clz(x);
#else
    uint32_t n = 0;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
#endif
}
TA_INLINE uint32_t ta_popcount32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_popcount(x);
#else
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    return (((x + (x >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
#endif
}

TA_INLINE int ta_bitset_test(const ta_bitset_word *b, uint32_t i) {
    return (b[i >> 5] >> (i & 31u)) & 1u;
}
TA_INLINE void ta_bitset_set(ta_bitset_word *b, uint32_t i) {
    b[i >> 5] |= 1u << (i & 31u);
}
TA_INLINE void ta_bitset_clear(ta_bitset_word *b, uint32_t i) {
    b[i >> 5] &= ~(1u << (i & 31u));
}

TA_INLINE void ta_bitset_zero(ta_bitset_word *b, uint32_t nw) {
    for (uint32_t i = 0; i < nw; i++)
        b[i] = 0;
}
TA_INLINE void ta_bitset_or(ta_bitset_word *d, const ta_bitset_word *s, uint32_t nw) {
    for (uint32_t i = 0; i < nw; i++)
        d[i] |= s[i];
}
TA_INLINE void ta_bitset_and_not(ta_bitset_word *d, const ta_bitset_word *s, uint32_t nw) {
    for (uint32_t i = 0; i < nw; i++)
        d[i] &= ~s[i];
}
TA_INLINE int ta_bitset_intersects(const ta_bitset_word *a, const ta_bitset_word *b, uint32_t nw) {
    for (uint32_t i = 0; i < nw; i++)
        if (a[i] & b[i]) return 1;
    return 0;
}
TA_INLINE int ta_bitset_any(const ta_bitset_word *b, uint32_t nw) {
    for (uint32_t i = 0; i < nw; i++)
        if (b[i]) return 1;
    return 0;
}
TA_INLINE uint32_t ta_bitset_count(const ta_bitset_word *b, uint32_t nw) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < nw; i++)
        n += ta_popcount32(b[i]);
    return n;
}

/* Mask of bits [lo, hi) within word w (word-local helper). */
TA_INLINE ta_bitset_word ta_bitset__range_mask(uint32_t w, uint32_t lo, uint32_t hi) {
    uint32_t wlo = w << 5, whi = wlo + 32u;
    if (hi <= wlo || lo >= whi) return 0;
    uint32_t a = lo > wlo ? lo - wlo : 0;
    uint32_t b = hi < whi ? hi - wlo : 32u;
    ta_bitset_word m = b == 32u ? 0xFFFFFFFFu : ((1u << b) - 1u);
    return m & ~((1u << a) - 1u);
}

/* d = s ∩ [lo, hi). Exit set of a transition = config ∩ (domain, domain.end). */
TA_INLINE void ta_bitset_and_range(ta_bitset_word *d, const ta_bitset_word *s, uint32_t nw,
                                   uint32_t lo, uint32_t hi) {
    for (uint32_t w = 0; w < nw; w++)
        d[w] = s[w] & ta_bitset__range_mask(w, lo, hi);
}
/* Any bit set in [lo, hi)? */
TA_INLINE int ta_bitset_any_in_range(const ta_bitset_word *s, uint32_t lo, uint32_t hi) {
    if (lo >= hi) return 0;
    for (uint32_t w = lo >> 5; w <= ((hi - 1u) >> 5); w++)
        if (s[w] & ta_bitset__range_mask(w, lo, hi)) return 1;
    return 0;
}

/* First set bit >= from, or TA_NIL. Ascending walks (entry order). */
TA_INLINE uint32_t ta_bitset_next(const ta_bitset_word *b, uint32_t nbits, uint32_t from) {
    if (from >= nbits) return TA_NIL;
    uint32_t w = from >> 5, nw = ta_bitset_words(nbits);
    ta_bitset_word x = b[w] & (0xFFFFFFFFu << (from & 31u));
    for (;;) {
        if (x) {
            uint32_t i = (w << 5) + ta_ctz32(x);
            return i < nbits ? i : TA_NIL;
        }
        if (++w >= nw) return TA_NIL;
        x = b[w];
    }
}
/* Last set bit <= from, or TA_NIL. Descending walks (exit order). */
TA_INLINE uint32_t ta_bitset_previous(const ta_bitset_word *b, uint32_t from) {
    if (from == TA_NIL) return TA_NIL;
    uint32_t w = from >> 5;
    uint32_t sh = 31u - (from & 31u);
    ta_bitset_word x = (b[w] << sh) >> sh;
    for (;;) {
        if (x) return (w << 5) + 31u - ta_clz32(x);
        if (w == 0) return TA_NIL;
        x = b[--w];
    }
}

#endif /* TINYACTORS_BASE_BITSET_H */
