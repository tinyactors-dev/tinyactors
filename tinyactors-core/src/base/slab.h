/* slab.h — page-based size-class allocator over a fixed region.
 *
 * Pages of TA_SLAB_PAGE bytes are assigned to a size class on
 * demand and returned to the free-page list when they become empty. Objects
 * are addressed by u32 byte offsets from the start of the page area, so the
 * slab is relocatable like every other region object. Pages are initialized
 * lazily (O(1) init). Max object size is TA_SLAB_PAGE.
 *
 * Used for chart-dependent instances and refcounted dynamic strings.
 */
#ifndef TINYACTORS_BASE_SLAB_H
#define TINYACTORS_BASE_SLAB_H

#include "base/config.h"

#ifndef TA_SLAB_PAGE
#define TA_SLAB_PAGE 4096u
#endif
/* 16, 24, 32, 48, 64, 96, ... 3072, 4096: half-step classes keep internal
 * fragmentation <= 33% (power-of-two classes waste up to 50%). */
#define TA_SLAB_CLASSES 17u

typedef struct ta_slab_page {
    uint32_t next, previous; /* class list (pages with free slots) or free-page list */
    uint16_t free_slot;      /* head of free slot list, 0xFFFF = none */
    uint16_t used;           /* live objects */
    uint16_t carved;         /* slots handed out at least once (lazy carving) */
    uint8_t size_class;      /* 0xFF = free page */
    uint8_t pad;
} ta_slab_page;

typedef struct ta_slab {
    int64_t pages_relative; /* self-relative */
    int64_t metadata_relative;
    uint32_t page_count;
    uint32_t free_head;       /* free-page list */
    uint32_t high_water_mark; /* pages [high_water_mark, page_count) never used */
    uint32_t used_pages;
    uint32_t class_head[TA_SLAB_CLASSES];
    uint32_t refused; /* 1 once an alloc found no page; its owner clears it */
} ta_slab;

TA_INLINE size_t ta_slab_pages_bytes(uint32_t page_count) {
    return (size_t)page_count * TA_SLAB_PAGE;
}
TA_INLINE size_t ta_slab_metadata_bytes(uint32_t page_count) {
    return (size_t)page_count * sizeof(ta_slab_page);
}

/* `pages` must be TA_ALIGN_MAX aligned. */
void ta_slab_init(ta_slab *s, void *pages, ta_slab_page *meta, uint32_t page_count);
uint32_t ta_slab_alloc(ta_slab *s, uint32_t size); /* offset (8-aligned) or TA_NIL */
void ta_slab_free(ta_slab *s, uint32_t offset);
uint32_t ta_slab_size_of(const ta_slab *s, uint32_t offset); /* class size of an object */

TA_INLINE void *ta_slab_pointer(const ta_slab *slab, uint32_t offset) {
    return ta_pointer_add(ta_pointer_add(slab, slab->pages_relative), offset);
}

/* Copy all state into a (possibly larger) slab, preserving offsets. */
void ta_slab_copy(ta_slab *dst, void *dst_pages, ta_slab_page *dst_meta, uint32_t dst_n_pages,
                  const ta_slab *source);

#endif /* TINYACTORS_BASE_SLAB_H */
