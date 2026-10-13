/*
 * GENERATED FILE. Do not edit; run `mise run amalgamate` in staticjs.
 * The wasm platform files, those in src/ named with a _wasm.c
 * suffix: a second translation unit, compiled next to staticjs.c only
 * when building for wasm. They use the public API alone.
 */
#include "staticjs.h"

/*
 * Functions not declared in staticjs.h are private to this file. A private
 * helper may be unused in a given build, so it does not warn.
 */
#if defined(__GNUC__) || defined(__clang__)
#define SJS_KEEP __attribute__((unused))
#else
#define SJS_KEEP
#endif
#ifndef SJS_PRIVATE
#define SJS_PRIVATE static SJS_KEEP
#endif

/* ---- src/allocator_wasm.c ---- */
/*
 * staticjs.wasm's allocator (docs/adr/0001-packaging-and-releases.md at the
 * repository root, decisions 6 and 9): built only for wasm, linked into the
 * module, so a host that loads it supplies no memory management of its own.
 *
 * A platform file: its own translation unit next to the amalgamation
 * (include/staticjs_wasm.c), which includes only the public header and so
 * cannot reach the library's internals. It is the one place with mutable
 * state outside a context: the module's heap, which a module has one of.
 *
 * Blocks come from linear memory above the linker's __heap_base, handed out
 * by a bump pointer that grows memory a page (64 KiB) at a time. Every block
 * has a power-of-two size class of at least 16 bytes and is aligned to its
 * class size, capped at a page, so a block on a class's free list satisfies
 * any alignment the class was asked for. Freed blocks go on their class's
 * free list and are handed out again first; memory is never returned to the
 * host, since wasm cannot shrink linear memory.
 */

#define WASM_PAGE 65536u
#define MIN_SHIFT 4u /* 16 bytes */
#define CLASSES 28u  /* 16 bytes to 2 GiB */

extern unsigned char __heap_base;

static uintptr_t heap_next;
static void *free_lists[CLASSES];

/* The class of a block of `size` bytes aligned to `alignment`, or CLASSES
 * when none fits. */
static unsigned size_class(size_t size, size_t alignment) {
    if (alignment > WASM_PAGE || (alignment & (alignment - 1u)) != 0u) return CLASSES;
    size_t need = size > alignment ? size : alignment;
    unsigned shift = MIN_SHIFT;
    while (shift < MIN_SHIFT + CLASSES && ((size_t)1u << shift) < need) shift++;
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
    if (heap_next == 0u) heap_next = (uintptr_t)&__heap_base;
    uintptr_t start = (heap_next + (align - 1u)) & ~(uintptr_t)(align - 1u);
    uintptr_t end = start + size;
    if (end < start) return NULL;
    uintptr_t limit = (uintptr_t)__builtin_wasm_memory_size(0) * WASM_PAGE;
    if (end > limit) {
        size_t pages = (size_t)((end - limit + WASM_PAGE - 1u) / WASM_PAGE);
        if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) return NULL;
    }
    heap_next = end;
    return (void *)start;
}

void *sjs_wasm_alloc(size_t size, size_t alignment) {
    if (alignment == 0u) alignment = 1u;
    unsigned index = size_class(size, alignment);
    if (index >= CLASSES) return NULL;
    void *block = free_lists[index];
    if (block != NULL) {
        free_lists[index] = *(void **)block;
        return block;
    }
    return carve(index);
}

void sjs_wasm_free(void *ptr, size_t size, size_t alignment) {
    if (ptr == NULL) return;
    if (alignment == 0u) alignment = 1u;
    unsigned index = size_class(size, alignment);
    if (index >= CLASSES) return;
    *(void **)ptr = free_lists[index];
    free_lists[index] = ptr;
}

static void *vtable_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    return sjs_wasm_alloc(size, alignment);
}

static void vtable_free(void *self, const sjs_allocator_free_params *params) {
    (void)self;
    sjs_wasm_free(params->ptr, params->size, params->alignment);
}

static const sjs_allocator_vtable wasm_vtable = { vtable_alloc, vtable_free };

sjs_allocator sjs_wasm_allocator(void) {
    return (sjs_allocator){ NULL, &wasm_vtable };
}
