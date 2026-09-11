/* Freestanding compiler support, not a dependency on a host C library.
 * Compilers lower aggregate C assignments/initializers to these primitives
 * even with -ffreestanding. Volatile accesses prevent recursive loop-idiom
 * lowering inside the primitives themselves. Embedders supplying their own
 * compiler runtime may omit this object from their build. */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *destination, const void *source, size_t size)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    const volatile unsigned char *in = (const volatile unsigned char *)source;
    size_t i;
    for (i = 0; i < size; ++i) out[i] = in[i];
    return destination;
}

void *memset(void *destination, int value, size_t size)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    size_t i;
    for (i = 0; i < size; ++i) out[i] = (unsigned char)value;
    return destination;
}

void bzero(void *destination, size_t size)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    size_t i;
    for (i = 0; i < size; ++i) out[i] = 0;
}

void *memmove(void *destination, const void *source, size_t size)
{
    volatile unsigned char *out = (volatile unsigned char *)destination;
    const volatile unsigned char *in = (const volatile unsigned char *)source;
    uintptr_t to = (uintptr_t)destination, from = (uintptr_t)source;
    size_t i;
    if (to > from && to - from < size) {
        for (i = size; i != 0; --i) out[i - 1] = in[i - 1];
    } else {
        for (i = 0; i < size; ++i) out[i] = in[i];
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t size)
{
    const volatile unsigned char *a = (const volatile unsigned char *)left;
    const volatile unsigned char *b = (const volatile unsigned char *)right;
    size_t i;
    for (i = 0; i < size; ++i) {
        unsigned char x = a[i], y = b[i];
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
