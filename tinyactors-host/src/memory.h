#ifndef TAH_MEMORY_H
#define TAH_MEMORY_H

/*
 * The kernel's own memory helpers. The kernel links no C library (kernel ADR
 * 0001, section 2), so it does not call memcpy, memset, memcmp, strlen or
 * snprintf: these do what the kernel needs of them, and the compiler may
 * still turn the memory ones into its builtins.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void tah_mem_copy(void *to, const void *from, size_t size);
void tah_mem_set(void *to, unsigned char byte, size_t size);
bool tah_mem_equal(const void *left, const void *right, size_t size);
/* The first `byte` among `size` bytes at `from`, or NULL; as memchr. */
const void *tah_mem_find(const void *from, unsigned char byte, size_t size);
/* The length of a NUL-terminated text, as strlen. */
size_t tah_str_length(const char *text);
/* Writes `value` in decimal to `out`, which has room for 20 bytes, with no
 * NUL; returns how many bytes it wrote. */
uint32_t tah_format_u64(char *out, uint64_t value);

#endif
