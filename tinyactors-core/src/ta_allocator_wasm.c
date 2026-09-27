/*
 * The WebAssembly module's allocator (root ADR 0001,
 * docs/adr/0001-packaging-and-releases.md, decisions 6 and 9): built only
 * for wasm, linked into tinyactors.wasm, so a host that loads the module
 * supplies no memory management of its own. Core's own copy, written
 * against core's allocator vtable (ADR 0023), not shared with txml's.
 *
 * A platform file: its own translation unit next to the amalgamation, which
 * includes only the public header and so cannot reach core's internals.
 *
 * Blocks come from linear memory above the linker's __heap_base, handed out
 * by a bump pointer that grows memory a page (64 KiB) at a time. Every block
 * has a power-of-two size class of at least 16 bytes and is aligned to its
 * class size, capped at a page, so a block on a class's free list satisfies
 * any alignment the class was asked for. Freed blocks go on their class's
 * free list and are handed out again first; memory is never returned to the
 * host, since wasm cannot shrink linear memory.
 */
#include "tinyactors.h"

#define WASM_PAGE 65536u
#define MIN_SHIFT 4u    /* 16 bytes */
#define CLASSES 28u     /* 16 bytes to 2 GiB */

extern unsigned char __heap_base;

static uintptr_t heap_next;
static void *free_lists[CLASSES];

/* The class of a block of `size` bytes aligned to `alignment`, or CLASSES
 * when none fits. */
static unsigned size_class(size_t size, size_t alignment) {
    if (alignment > WASM_PAGE || (alignment & (alignment - 1u)) != 0u) {
        return CLASSES;
    }
    size_t need = size > alignment ? size : alignment;
    unsigned shift = MIN_SHIFT;
    while (shift < MIN_SHIFT + CLASSES && ((size_t)1u << shift) < need) {
        shift++;
    }
    return shift - MIN_SHIFT;
}

static size_t class_size(unsigned index) {
    return (size_t)1u << (index + MIN_SHIFT);
}

/* A fresh block of class `index` from the end of the heap, growing memory
 * when it does not fit; NULL when memory cannot grow. */
static void *carve(unsigned index) {
    size_t size = class_size(index);
    size_t align = size < WASM_PAGE ? size : WASM_PAGE;
    if (heap_next == 0u) {
        heap_next = (uintptr_t)&__heap_base;
    }
    uintptr_t start = (heap_next + (align - 1u)) & ~(uintptr_t)(align - 1u);
    uintptr_t end = start + size;
    if (end < start) {
        return NULL;
    }
    uintptr_t limit = (uintptr_t)__builtin_wasm_memory_size(0) * WASM_PAGE;
    if (end > limit) {
        size_t pages = (size_t)((end - limit + WASM_PAGE - 1u) / WASM_PAGE);
        if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) {
            return NULL;
        }
    }
    heap_next = end;
    return (void *)start;
}

void *ta_wasm_alloc(size_t size, size_t alignment) {
    if (alignment == 0u) {
        alignment = 1u;
    }
    unsigned index = size_class(size, alignment);
    if (index >= CLASSES) {
        return NULL;
    }
    void *block = free_lists[index];
    if (block != NULL) {
        free_lists[index] = *(void **)block;
        return block;
    }
    return carve(index);
}

void ta_wasm_free(void *ptr, size_t size, size_t alignment) {
    if (ptr == NULL) {
        return;
    }
    if (alignment == 0u) {
        alignment = 1u;
    }
    unsigned index = size_class(size, alignment);
    if (index >= CLASSES) {
        return;
    }
    *(void **)ptr = free_lists[index];
    free_lists[index] = ptr;
}

static void *vtable_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    return ta_wasm_alloc(size, alignment);
}

static void vtable_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    ta_wasm_free(params->ptr, params->size, params->alignment);
}

static const ta_allocator_vtable wasm_vtable = { vtable_alloc, vtable_free };

ta_allocator ta_wasm_allocator(void) {
    return (ta_allocator){ NULL, &wasm_vtable };
}
