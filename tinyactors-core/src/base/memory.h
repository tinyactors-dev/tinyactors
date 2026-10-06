/* memory.h — memory primitives without libc headers.
 *
 * GCC/Clang builtins lower to inline code or to memcpy/memset symbols; in
 * freestanding builds those symbols are provided by the compiler runtime
 * (e.g. wasm bulk-memory, compiler-rt) or the embedder.
 */
#ifndef TINYACTORS_BASE_MEMORY_H
#define TINYACTORS_BASE_MEMORY_H

#include "base/config.h"

/* memcmp never lowers to an intrinsic, so it is always a local loop: the
 * freestanding layers then depend on at most memcpy/memset (which GCC/Clang
 * may emit implicitly anyway and which wasm bulk-memory provides). */
TA_INLINE int ta_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (; n; --n, ++x, ++y)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}

#if defined(__GNUC__) || defined(__clang__)
#define ta_memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define ta_memset(d, c, n) __builtin_memset((d), (c), (n))
#else
TA_INLINE void ta__memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--)
        *dp++ = *sp++;
}
TA_INLINE void ta__memset(void *d, int c, size_t n) {
    unsigned char *dp = (unsigned char *)d;
    while (n--)
        *dp++ = (unsigned char)c;
}
#define ta_memcpy(d, s, n) ta__memcpy((d), (s), (n))
#define ta_memset(d, c, n) ta__memset((d), (c), (n))
#endif

#endif /* TINYACTORS_BASE_MEMORY_H */
