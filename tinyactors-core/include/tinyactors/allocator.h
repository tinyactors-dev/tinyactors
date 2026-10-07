/* tinyactors/allocator.h — the allocator a caller lends a layer that needs
 * memory beyond one block it was given.
 *
 * base through system never allocate. The host (tinyactors/host.h) takes one
 * in its configuration and gets every byte through it; the compiler
 * (tinyactors/compile.h) takes one in its options. ta_runtime_malloc()
 * (tinyactors/runtime.h) is one over the C library.
 *
 *   alloc  returns `size` bytes aligned to `align` (a power of two, at most
 *          8 from this library), or NULL to refuse; the caller then reports
 *          TA_ERROR_NO_MEMORY (or TA_RUN_EXHAUSTED from a run).
 *   free   takes back a block with the size it was allocated with.
 *
 * There is no realloc: growing is alloc, copy, free. */
#ifndef TINYACTORS_ALLOCATOR_H
#define TINYACTORS_ALLOCATOR_H

#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_allocator {
    void *context;
    void *(*alloc)(void *context, size_t size, size_t align);
    void (*free)(void *context, void *ptr, size_t size);
} ta_allocator;

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_ALLOCATOR_H */
