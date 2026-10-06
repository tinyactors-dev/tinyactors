#ifndef TAO_MEMORY_H
#define TAO_MEMORY_H

/**
 * @file
 * The encoder's memory helpers. The library links no C library, so it does
 * not call memcpy, memset, memcmp or strlen by name: copy, fill and compare
 * go through the compiler's builtins, which inline small, fixed-size ones
 * and call memcpy, memset or memcmp for the rest. Those symbols are the only
 * ones the library may leave undefined beside core's (check-freestanding):
 * a release resolves them with zig's compiler-rt, a test program with its C
 * library.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline void tao_mem_copy(void *to, const void *from, size_t size) {
    if (size != 0u) {
        __builtin_memcpy(to, from, size);
    }
}

static inline void tao_mem_set(void *to, unsigned char byte, size_t size) {
    if (size != 0u) {
        __builtin_memset(to, byte, size);
    }
}

static inline bool tao_mem_equal(const void *left, const void *right, size_t size) {
    return size == 0u || __builtin_memcmp(left, right, size) == 0;
}

/* The length of a NUL-terminated text, as strlen. */
static inline size_t tao_str_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length += 1u;
    }
    return length;
}

#endif
