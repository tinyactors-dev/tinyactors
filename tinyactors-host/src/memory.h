#ifndef TAH_MEMORY_H
#define TAH_MEMORY_H

/*
 * The kernel's own memory helpers. The kernel links no C library (kernel ADR
 * 0001, section 2), so it does not call memcpy, memset or memcmp: these do
 * the same, and the compiler may still turn them into its builtins.
 */

#include <stdbool.h>
#include <stddef.h>

void tah_mem_copy(void *to, const void *from, size_t size);
void tah_mem_set(void *to, unsigned char byte, size_t size);
bool tah_mem_equal(const void *left, const void *right, size_t size);

#endif
