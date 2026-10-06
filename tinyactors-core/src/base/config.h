/* config.h — internal portability layer for freestanding code.
 *
 * Everything under src/base, src/image, src/core and src/system must compile
 * with -ffreestanding and only <stddef.h>/<stdint.h> (checked by
 * `mise run check:freestanding`). No libc, no malloc, no OS.
 */
#ifndef TINYACTORS_BASE_CONFIG_H
#define TINYACTORS_BASE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include <tinyactors/types.h>

#if defined(__GNUC__) || defined(__clang__)
#define TA_INLINE      static inline __attribute__((always_inline, unused))
#define TA_LIKELY(x)   __builtin_expect(!!(x), 1)
#define TA_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define TA_TRAP()      __builtin_trap()
#elif defined(_MSC_VER)
#define TA_INLINE      static __forceinline
#define TA_LIKELY(x)   (x)
#define TA_UNLIKELY(x) (x)
#define TA_TRAP()      __debugbreak()
#else
#define TA_INLINE      static inline
#define TA_LIKELY(x)   (x)
#define TA_UNLIKELY(x) (x)
#define TA_TRAP()      (*(volatile int *)0 = 0)
#endif

#ifndef TA_ASSERT
#ifdef TA_DEBUG
#define TA_ASSERT(x)                                                                               \
    do {                                                                                           \
        if (!(x)) TA_TRAP();                                                                       \
    } while (0)
#else
#define TA_ASSERT(x) ((void)0)
#endif
#endif

#define TA_STATIC_ASSERT(c, m) typedef char ta_static_assert_##m[(c) ? 1 : -1]

/* Largest alignment any region object needs (u64 fields). */
#define TA_ALIGN_MAX 8u

TA_INLINE size_t ta_align_up(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

/* Byte offset from a base pointer. Used for self-relative offsets and record
 * fields whose alignment is guaranteed by construction (returns void * so the
 * caller's typed pointer conversion does not trip -Wcast-align). */
TA_INLINE void *ta_pointer_add(const void *p, int64_t offset) {
    return (void *)((const unsigned char *)p + offset);
}

#endif /* TINYACTORS_BASE_CONFIG_H */
