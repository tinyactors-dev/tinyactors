#include "base/slab.h"
#include "base/memory.h"

#define NO_SLOT 0xFFFFu

static ta_slab_page *meta(const ta_slab *slab) {
    return (ta_slab_page *)ta_pointer_add(slab, slab->metadata_relative);
}
static unsigned char *pages(const ta_slab *slab) {
    return (unsigned char *)ta_pointer_add(slab, slab->pages_relative);
}

static uint32_t class_size(uint32_t c) {
    /* even c: 16 << c/2 ; odd c: 24 << c/2 */
    return (c & 1u) ? 24u << (c >> 1) : 16u << (c >> 1);
}
static uint32_t class_of(uint32_t size) {
    uint32_t c = 0;
    while (class_size(c) < size)
        c++;
    return c;
}

void ta_slab_init(ta_slab *slab, void *pages, ta_slab_page *metadata, uint32_t page_count) {
    slab->pages_relative = (int64_t)((intptr_t)pages - (intptr_t)slab);
    slab->metadata_relative = (int64_t)((intptr_t)metadata - (intptr_t)slab);
    slab->page_count = page_count;
    slab->free_head = TA_NIL;
    slab->high_water_mark = 0;
    slab->used_pages = 0;
    for (uint32_t i = 0; i < TA_SLAB_CLASSES; i++)
        slab->class_head[i] = TA_NIL;
    slab->pad = 0;
}

static void list_remove(ta_slab *slab, uint32_t c, uint32_t p) {
    ta_slab_page *m = meta(slab);
    if (m[p].previous != TA_NIL)
        m[m[p].previous].next = m[p].next;
    else
        slab->class_head[c] = m[p].next;
    if (m[p].next != TA_NIL) m[m[p].next].previous = m[p].previous;
    m[p].next = m[p].previous = TA_NIL;
}

static void list_push(ta_slab *slab, uint32_t c, uint32_t p) {
    ta_slab_page *m = meta(slab);
    m[p].previous = TA_NIL;
    m[p].next = slab->class_head[c];
    if (slab->class_head[c] != TA_NIL) m[slab->class_head[c]].previous = p;
    slab->class_head[c] = p;
}

uint32_t ta_slab_alloc(ta_slab *slab, uint32_t size) {
    if (size == 0 || size > TA_SLAB_PAGE) return TA_NIL;
    uint32_t c = class_of(size), csz = class_size(c), per_page = TA_SLAB_PAGE / csz;
    ta_slab_page *m = meta(slab);
    uint32_t p = slab->class_head[c];
    if (p == TA_NIL) {
        if (slab->free_head != TA_NIL) {
            p = slab->free_head;
            slab->free_head = m[p].next;
        } else if (slab->high_water_mark < slab->page_count) {
            p = slab->high_water_mark++;
        } else {
            return TA_NIL;
        }
        m[p].size_class = (uint8_t)c;
        m[p].free_slot = NO_SLOT;
        m[p].used = 0;
        m[p].carved = 0;
        m[p].pad = 0;
        slab->used_pages++;
        list_push(slab, c, p);
    }
    uint32_t slot;
    if (m[p].free_slot != NO_SLOT) {
        slot = m[p].free_slot;
        uint16_t nxt;
        ta_memcpy(&nxt, pages(slab) + (size_t)p * TA_SLAB_PAGE + slot * csz, sizeof nxt);
        m[p].free_slot = nxt;
    } else {
        slot = m[p].carved++;
    }
    m[p].used++;
    if (m[p].free_slot == NO_SLOT && m[p].carved == per_page) list_remove(slab, c, p);
    return p * TA_SLAB_PAGE + slot * csz;
}

void ta_slab_free(ta_slab *slab, uint32_t offset) {
    ta_slab_page *m = meta(slab);
    uint32_t p = offset / TA_SLAB_PAGE;
    TA_ASSERT(p < slab->high_water_mark && m[p].size_class != 0xFF && m[p].used > 0);
    uint32_t c = m[p].size_class, csz = class_size(c), per_page = TA_SLAB_PAGE / csz;
    uint32_t slot = (offset % TA_SLAB_PAGE) / csz;
    int was_full = m[p].free_slot == NO_SLOT && m[p].carved == per_page;
    uint16_t nxt = m[p].free_slot;
    unsigned char *blk = pages(slab) + (size_t)p * TA_SLAB_PAGE + (size_t)slot * csz;
    ta_memcpy(blk, &nxt, sizeof nxt);
    ta_memset(blk + sizeof nxt, 0, csz - sizeof nxt); /* dead bytes: never residue */
    m[p].free_slot = (uint16_t)slot;
    m[p].used--;
    if (m[p].used == 0) {
        if (!was_full) list_remove(slab, c, p);
        m[p].size_class = 0xFF;
        m[p].next = slab->free_head;
        slab->free_head = p;
        slab->used_pages--;
    } else if (was_full) {
        list_push(slab, c, p);
    }
}

uint32_t ta_slab_size_of(const ta_slab *slab, uint32_t offset) {
    return class_size(meta(slab)[offset / TA_SLAB_PAGE].size_class);
}

void ta_slab_copy(ta_slab *dst, void *dst_pages, ta_slab_page *dst_meta, uint32_t dst_n_pages,
                  const ta_slab *source) {
    TA_ASSERT(dst_n_pages >= source->high_water_mark);
    ta_slab_init(dst, dst_pages, dst_meta, dst_n_pages);
    ta_memcpy(dst_pages, pages(source), (size_t)source->high_water_mark * TA_SLAB_PAGE);
    ta_memcpy(dst_meta, meta(source), (size_t)source->high_water_mark * sizeof(ta_slab_page));
    dst->free_head = source->free_head;
    dst->high_water_mark = source->high_water_mark;
    dst->used_pages = source->used_pages;
    for (uint32_t i = 0; i < TA_SLAB_CLASSES; i++)
        dst->class_head[i] = source->class_head[i];
}
