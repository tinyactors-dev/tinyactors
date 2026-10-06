/*
 * tinyactors_runtime.wasm's allocator (docs/adr/0001-packaging-and-releases.md,
 * decisions 6, 9 and 19): ta_wasm_alloc and ta_wasm_free over the C
 * library's malloc, which the module links in (wasi-libc's, growing linear
 * memory itself, with no import), so the host's blocks and the runtime's
 * own allocations come from one heap. The runtime module therefore does not
 * link tinyactors.wasm's page allocator (src/base/allocator_wasm.c), which
 * would claim the same memory above __heap_base.
 */
#include <tinyactors/wasm.h>

#include <stdlib.h>

void *ta_wasm_alloc(size_t size, size_t alignment) {
    if (alignment == 0u) alignment = 1u;
    if (alignment > 65536u || (alignment & (alignment - 1u)) != 0u) return NULL;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    /* aligned_alloc wants a size that is a multiple of the alignment. */
    size_t rounded = (size + alignment - 1u) & ~(alignment - 1u);
    if (rounded < size) return NULL;
    return aligned_alloc(alignment, rounded ? rounded : alignment);
}

void ta_wasm_free(void *pointer, size_t size, size_t alignment) {
    (void)size;
    (void)alignment;
    free(pointer);
}
