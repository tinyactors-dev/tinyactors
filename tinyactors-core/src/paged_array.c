#include "paged_array.h"
#include "../include/tinyactors.h"
#include "memory.h"

static size_t ta_paged_array_page_bytes(const ta_paged_array *array) {
    return array->entry_size << array->page_shift;
}

/* Pages that positions 0 to count - 1 occupy. */
static uint32_t ta_paged_array_pages_for(const ta_paged_array *array, uint32_t count) {
    return count == 0 ? 0u : ((count - 1u) >> array->page_shift) + 1u;
}

static void ta_paged_array_free_page(const ta_paged_array *array, unsigned char *page) {
    ta_mem_free(array->allocator, page,
                &(ta_allocator_free_params){
                    .size = ta_paged_array_page_bytes(array),
                    .alignment = array->entry_alignment,
                });
}

static void ta_paged_array_free_directory(ta_allocator allocator, unsigned char **pages, uint32_t capacity) {
    ta_mem_free(allocator, pages,
                &(ta_allocator_free_params){
                    .size = (size_t)capacity * sizeof(unsigned char *),
                    .alignment = _Alignof(unsigned char *),
                });
}

ta_result ta_paged_array_init(ta_paged_array *array, const ta_paged_array_params *params) {
    size_t alignment = params->entry_alignment;
    if (params->entry_size == 0 || alignment == 0 || (alignment & (alignment - 1u)) != 0 ||
        params->entry_size % alignment != 0 || params->page_shift > 31u ||
        params->entry_size > (SIZE_MAX >> params->page_shift)) {
        return TA_ERR_INVALID;
    }
    *array = (ta_paged_array){
        .allocator = params->allocator,
        .entry_size = params->entry_size,
        .entry_alignment = alignment,
        .page_shift = params->page_shift,
        .page_mask = (uint32_t)((UINT64_C(1) << params->page_shift) - 1u),
    };
    return TA_OK;
}

void ta_paged_array_destroy(ta_paged_array *array) {
    ta_paged_array_shrink(array, 0);
    *array = (ta_paged_array){0};
}

ta_result ta_paged_array_grow(ta_paged_array *array, uint32_t count) {
    uint32_t needed = ta_paged_array_pages_for(array, count);
    if (needed <= array->page_count) {
        return TA_OK;
    }

    /* New page addresses go into a new directory when the old one is full,
     * or into the old one's unused slots, which nothing reads until
     * page_count moves. Either way the array is unchanged until the end. */
    unsigned char **directory = array->pages;
    uint32_t directory_capacity = array->directory_capacity;
    if (needed > directory_capacity) {
        uint64_t doubled = (uint64_t)directory_capacity * 2u;
        uint64_t capacity = doubled > needed ? doubled : needed;
        if (capacity > UINT32_MAX) {
            capacity = UINT32_MAX;
        }
        if (capacity > SIZE_MAX / sizeof(unsigned char *)) {
            return TA_ERR_NOMEM;
        }
        directory_capacity = (uint32_t)capacity;
        directory = ta_mem_alloc(array->allocator, (size_t)directory_capacity * sizeof(unsigned char *),
                                 _Alignof(unsigned char *));
        if (directory == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t page = 0; page < array->page_count; page += 1u) {
            directory[page] = array->pages[page];
        }
    }

    size_t page_bytes = ta_paged_array_page_bytes(array);
    for (uint32_t page = array->page_count; page < needed; page += 1u) {
        unsigned char *memory = ta_mem_alloc(array->allocator, page_bytes, array->entry_alignment);
        if (memory == NULL) {
            while (page > array->page_count) {
                page -= 1u;
                ta_paged_array_free_page(array, directory[page]);
            }
            if (directory != array->pages) {
                ta_paged_array_free_directory(array->allocator, directory, directory_capacity);
            }
            return TA_ERR_NOMEM;
        }
        ta_mem_zero(memory, page_bytes);
        directory[page] = memory;
    }

    if (directory != array->pages) {
        ta_paged_array_free_directory(array->allocator, array->pages, array->directory_capacity);
        array->pages = directory;
        array->directory_capacity = directory_capacity;
    }
    array->page_count = needed;
    return TA_OK;
}

void ta_paged_array_shrink(ta_paged_array *array, uint32_t count) {
    uint32_t keep = ta_paged_array_pages_for(array, count);
    while (array->page_count > keep) {
        array->page_count -= 1u;
        ta_paged_array_free_page(array, array->pages[array->page_count]);
    }
    if (array->page_count == 0 && array->pages != NULL) {
        ta_paged_array_free_directory(array->allocator, array->pages, array->directory_capacity);
        array->pages = NULL;
        array->directory_capacity = 0;
    }
}
