/*
 * ta_paged_array: the dense array in power-of-two pages (src/paged_array.h)
 * that holds the deadline index's heap positions.
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Memory comes from a counting
 * allocator local to this test, which can fail after a number of
 * allocations and records the size of every request.
 *
 * Claims proven here, in order:
 *
 * 1. init refuses an entry size of 0, an alignment of 0 or not a power of
 *    two, an entry size that is not a multiple of the alignment, a page
 *    shift above 31 and a page too large for size_t, with TA_ERR_INVALID,
 *    leaving the array untouched; a valid init allocates nothing and
 *    gives capacity 0, and shrink and destroy of the empty array free
 *    nothing;
 * 2. grow rounds up to whole pages (one page for 1 and for exactly a
 *    page's entries, two for one more), and a grow to 0 or within the
 *    capacity changes nothing and allocates nothing;
 * 3. position p lives in page p >> k at entry p & mask, with entries of
 *    entry_size bytes at the entry alignment; set and get round-trip
 *    across page boundaries, from the first to the last position, and
 *    the positions grow adds read as zero;
 * 4. growing copies no entry: every entry keeps its address and value
 *    when pages are added and when the directory is replaced by a larger
 *    one, and the directory doubles;
 * 5. shrink frees the pages wholly at or above n, keeps a page partly
 *    below n whole with its values, changes nothing for an n at or above
 *    the capacity, and with no page left frees the directory; a page that
 *    grow adds again reads as zero;
 * 6. a grow that fails at any of its allocations (the directory or any
 *    page) answers TA_ERR_NOMEM and changes nothing: the same capacity,
 *    directory, pages and values, and no block leaked;
 * 7. at the largest count, 2^32 - 1, grow asks for the pages that count
 *    needs (two pages of 2^31 entries) and, refused, answers TA_ERR_NOMEM
 *    and changes nothing;
 * 8. at, get, set and capacity never allocate;
 * 9. destroy frees every page and the directory and leaves an array that
 *    holds nothing.
 */
#include "../include/tinyactors.c"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

/* ------------------------------------------------------------------------
 * An allocator that counts, can fail, and refuses any request above
 * `largest` bytes (when set) without allocating it.
 */

#define MAX_BLOCKS 256u

typedef struct {
    void *ptrs[MAX_BLOCKS];
    size_t sizes[MAX_BLOCKS];
    uint32_t live;
    uint32_t attempts;
    /* The attempt, counting from 1, from which every allocation fails; 0
     * never fails. */
    uint32_t fail_from;
    size_t largest;
    size_t requested[MAX_BLOCKS];
} counting_allocator;

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    counting_allocator *heap = self;
    if (heap->attempts < MAX_BLOCKS) {
        heap->requested[heap->attempts] = size;
    }
    heap->attempts += 1u;
    if ((heap->fail_from != 0 && heap->attempts >= heap->fail_from) || heap->live == MAX_BLOCKS ||
        (heap->largest != 0 && size > heap->largest)) {
        return NULL;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size_t rounded = (size + alignment - 1u) / alignment * alignment;
    void *ptr = aligned_alloc(alignment, rounded);
    if (ptr != NULL) {
        memset(ptr, 0xA5, rounded);
        heap->sizes[heap->live] = size;
        heap->ptrs[heap->live++] = ptr;
    }
    return ptr;
}

static void counting_free(void *self, const ta_allocator_free_params *params) {
    counting_allocator *heap = self;
    for (uint32_t index = 0; index < heap->live; index += 1u) {
        if (heap->ptrs[index] == params->ptr) {
            if (heap->sizes[index] != params->size) {
                fprintf(stderr, "free of %zu bytes, allocated as %zu\n", params->size, heap->sizes[index]);
                failures += 1;
            }
            heap->live -= 1u;
            heap->ptrs[index] = heap->ptrs[heap->live];
            heap->sizes[index] = heap->sizes[heap->live];
            free(params->ptr);
            return;
        }
    }
    fprintf(stderr, "free of %p, which this allocator never handed out\n", params->ptr);
    failures += 1;
}

static const ta_allocator_vtable counting_vtable = { .alloc = counting_alloc, .free = counting_free };

static ta_allocator allocator_of(counting_allocator *heap) {
    return (ta_allocator){ .self = heap, .v = &counting_vtable };
}

/* Entries of 16 bytes aligned to 8, four to a page. */
typedef struct {
    uint64_t key;
    uint32_t value;
    uint32_t spare;
} entry;

static ta_paged_array_params entry_params(counting_allocator *heap) {
    return (ta_paged_array_params){
        .allocator = allocator_of(heap),
        .entry_size = sizeof(entry),
        .entry_alignment = _Alignof(entry),
        .page_shift = 2,
    };
}

static void put(ta_paged_array *array, uint32_t position) {
    entry value = { .key = 1000u + position, .value = position * 3u + 1u };
    ta_paged_array_set(array, position, &value);
}

static bool holds(const ta_paged_array *array, uint32_t position) {
    entry value;
    ta_paged_array_get(array, position, &value);
    return value.key == 1000u + position && value.value == position * 3u + 1u;
}

static bool zero_at(const ta_paged_array *array, uint32_t position) {
    static const unsigned char zero[sizeof(entry)];
    return memcmp(ta_paged_array_at(array, position), zero, sizeof(entry)) == 0;
}

/* 1 */
static void test_init(void) {
    counting_allocator heap = {0};
    ta_paged_array_params good = entry_params(&heap);
    ta_paged_array_params bad[] = { good, good, good, good, good, good };
    bad[0].entry_size = 0;
    bad[1].entry_alignment = 0;
    bad[2].entry_alignment = 12;
    bad[3].entry_size = 12; /* not a multiple of 8 */
    bad[4].page_shift = 32;
    bad[5].entry_size = ((SIZE_MAX >> 20) & ~(size_t)7u) + 8u;
    bad[5].page_shift = 20;
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(bad); index += 1u) {
        ta_paged_array array;
        memset(&array, 0x5A, sizeof(array));
        ta_paged_array before;
        memcpy(&before, &array, sizeof(array));
        CHECK(ta_paged_array_init(&array, &bad[index]) == TA_ERR_INVALID);
        CHECK(memcmp(&array, &before, sizeof(array)) == 0);
    }

    ta_paged_array array;
    CHECK(ta_paged_array_init(&array, &good) == TA_OK);
    CHECK(heap.attempts == 0);
    CHECK(ta_paged_array_capacity(&array) == 0);
    CHECK(array.pages == NULL && array.page_count == 0);
    ta_paged_array_shrink(&array, 0);
    ta_paged_array_destroy(&array);
    CHECK(heap.attempts == 0 && heap.live == 0);

    /* The widest page shift is accepted. */
    good.page_shift = 31;
    good.entry_size = 8;
    CHECK(ta_paged_array_init(&array, &good) == TA_OK);
    CHECK(array.page_mask == 0x7FFFFFFFu);
}

/* 2 */
static void test_grow_rounds_to_pages(void) {
    counting_allocator heap = {0};
    ta_paged_array array;
    ta_paged_array_params params = entry_params(&heap);
    CHECK(ta_paged_array_init(&array, &params) == TA_OK);

    CHECK(ta_paged_array_grow(&array, 0) == TA_OK);
    CHECK(heap.attempts == 0 && ta_paged_array_capacity(&array) == 0);

    CHECK(ta_paged_array_grow(&array, 1) == TA_OK);
    CHECK(ta_paged_array_capacity(&array) == 4 && array.page_count == 1);
    CHECK(heap.live == 2); /* the directory and one page */

    uint32_t attempts = heap.attempts;
    CHECK(ta_paged_array_grow(&array, 4) == TA_OK);
    CHECK(ta_paged_array_grow(&array, 2) == TA_OK);
    CHECK(ta_paged_array_grow(&array, 0) == TA_OK);
    CHECK(heap.attempts == attempts && ta_paged_array_capacity(&array) == 4);

    CHECK(ta_paged_array_grow(&array, 5) == TA_OK);
    CHECK(ta_paged_array_capacity(&array) == 8 && array.page_count == 2);

    ta_paged_array_destroy(&array);
    CHECK(heap.live == 0);
}

/* 3 */
static void test_positions(void) {
    counting_allocator heap = {0};
    ta_paged_array array;
    ta_paged_array_params params = entry_params(&heap);
    CHECK(ta_paged_array_init(&array, &params) == TA_OK);
    CHECK(ta_paged_array_grow(&array, 13) == TA_OK);
    CHECK(ta_paged_array_capacity(&array) == 16);

    for (uint32_t position = 0; position < 16; position += 1u) {
        CHECK(zero_at(&array, position));
        unsigned char *expected = array.pages[position >> 2] + (position & 3u) * sizeof(entry);
        CHECK(ta_paged_array_at(&array, position) == expected);
        CHECK((uintptr_t)ta_paged_array_at(&array, position) % _Alignof(entry) == 0);
    }
    for (uint32_t position = 0; position < 16; position += 1u) {
        put(&array, position);
    }
    for (uint32_t position = 0; position < 16; position += 1u) {
        CHECK(holds(&array, position));
    }

    /* A page boundary: the last entry of one page and the first of the next. */
    entry value = { .key = 7, .value = 77 };
    ta_paged_array_set(&array, 3, &value);
    CHECK(holds(&array, 4) && holds(&array, 2));
    entry out = {0};
    ta_paged_array_get(&array, 3, &out);
    CHECK(out.key == 7 && out.value == 77);

    /* uint32_t entries, as the deadline index keeps them, one per page. */
    counting_allocator words_heap = {0};
    ta_paged_array words;
    CHECK(ta_paged_array_init(&words, &(ta_paged_array_params){
                                          .allocator = allocator_of(&words_heap),
                                          .entry_size = sizeof(uint32_t),
                                          .entry_alignment = _Alignof(uint32_t),
                                          .page_shift = 0,
                                      }) == TA_OK);
    CHECK(ta_paged_array_grow(&words, 3) == TA_OK);
    for (uint32_t position = 0; position < 3; position += 1u) {
        uint32_t word = 0xC0DE0000u + position;
        ta_paged_array_set(&words, position, &word);
    }
    for (uint32_t position = 0; position < 3; position += 1u) {
        uint32_t word = 0;
        ta_paged_array_get(&words, position, &word);
        CHECK(word == 0xC0DE0000u + position);
        CHECK(ta_paged_array_at(&words, position) == words.pages[position]);
    }

    ta_paged_array_destroy(&words);
    ta_paged_array_destroy(&array);
    CHECK(heap.live == 0 && words_heap.live == 0);
}

/* 4 */
static void test_grow_copies_nothing(void) {
    counting_allocator heap = {0};
    ta_paged_array array;
    ta_paged_array_params params = entry_params(&heap);
    CHECK(ta_paged_array_init(&array, &params) == TA_OK);
    CHECK(ta_paged_array_grow(&array, 4) == TA_OK);
    CHECK(array.directory_capacity == 1);

    void *addresses[64];
    uint32_t filled = 4;
    for (uint32_t position = 0; position < filled; position += 1u) {
        put(&array, position);
        addresses[position] = ta_paged_array_at(&array, position);
    }

    /* Add a page at a time: the directory fills at 1, 2, 4 and 8 pages and
     * each time doubles. */
    uint32_t expected_directory[] = { 2, 4, 4, 8, 8, 8, 8, 16 };
    for (uint32_t step = 0; step < TA_ARRAY_COUNT(expected_directory); step += 1u) {
        unsigned char **directory = array.pages;
        uint32_t directory_capacity = array.directory_capacity;
        CHECK(ta_paged_array_grow(&array, filled + 1u) == TA_OK);
        CHECK(array.directory_capacity == expected_directory[step]);
        if (array.directory_capacity != directory_capacity) {
            CHECK(array.pages != directory);
        }
        for (uint32_t position = 0; position < filled; position += 1u) {
            CHECK(ta_paged_array_at(&array, position) == addresses[position]);
            CHECK(holds(&array, position));
        }
        for (uint32_t position = filled; position < filled + 4u; position += 1u) {
            CHECK(zero_at(&array, position));
            put(&array, position);
            addresses[position] = ta_paged_array_at(&array, position);
        }
        filled += 4u;
    }
    CHECK(array.page_count == 9 && heap.live == 10);

    /* A grow past twice the directory takes what it needs at once. */
    CHECK(ta_paged_array_grow(&array, 4u * 40u) == TA_OK);
    CHECK(array.directory_capacity == 40 && array.page_count == 40);
    for (uint32_t position = 0; position < filled; position += 1u) {
        CHECK(ta_paged_array_at(&array, position) == addresses[position]);
        CHECK(holds(&array, position));
    }

    ta_paged_array_destroy(&array);
    CHECK(heap.live == 0);
}

/* 5 */
static void test_shrink(void) {
    counting_allocator heap = {0};
    ta_paged_array array;
    ta_paged_array_params params = entry_params(&heap);
    CHECK(ta_paged_array_init(&array, &params) == TA_OK);
    CHECK(ta_paged_array_grow(&array, 16) == TA_OK);
    for (uint32_t position = 0; position < 16; position += 1u) {
        put(&array, position);
    }
    CHECK(heap.live == 5);

    /* At or above the capacity: nothing changes. */
    ta_paged_array_shrink(&array, 16);
    ta_paged_array_shrink(&array, UINT32_MAX);
    CHECK(array.page_count == 4 && heap.live == 5);

    /* 9 positions occupy three pages; the third is kept whole. */
    unsigned char **directory = array.pages;
    ta_paged_array_shrink(&array, 9);
    CHECK(array.page_count == 3 && ta_paged_array_capacity(&array) == 12);
    CHECK(heap.live == 4 && array.pages == directory && array.directory_capacity == 4);
    for (uint32_t position = 0; position < 12; position += 1u) {
        CHECK(holds(&array, position));
    }

    /* Exactly a page boundary keeps the pages below it. */
    ta_paged_array_shrink(&array, 8);
    CHECK(array.page_count == 2 && heap.live == 3);

    /* Growing back adds a zeroed page and keeps the rest. */
    CHECK(ta_paged_array_grow(&array, 12) == TA_OK);
    for (uint32_t position = 0; position < 8; position += 1u) {
        CHECK(holds(&array, position));
    }
    for (uint32_t position = 8; position < 12; position += 1u) {
        CHECK(zero_at(&array, position));
    }

    /* With no page left, the directory goes too. */
    ta_paged_array_shrink(&array, 0);
    CHECK(array.page_count == 0 && array.pages == NULL && array.directory_capacity == 0);
    CHECK(heap.live == 0 && ta_paged_array_capacity(&array) == 0);

    /* And the array grows again from empty. */
    CHECK(ta_paged_array_grow(&array, 1) == TA_OK);
    CHECK(zero_at(&array, 0) && heap.live == 2);
    ta_paged_array_destroy(&array);
    CHECK(heap.live == 0);
}

/* 6 */
/* Grows to `from`, shrinks to `keep`, then fails each allocation of a
 * grow to `to` in turn. */
static void check_failed_grow(uint32_t from, uint32_t keep, uint32_t to) {
    for (uint32_t failing = 1;; failing += 1u) {
        counting_allocator heap = {0};
        ta_paged_array array;
        ta_paged_array_params params = entry_params(&heap);
    CHECK(ta_paged_array_init(&array, &params) == TA_OK);
        CHECK(ta_paged_array_grow(&array, from) == TA_OK);
        ta_paged_array_shrink(&array, keep);
        for (uint32_t position = 0; position < keep; position += 1u) {
            put(&array, position);
        }
        ta_paged_array before;
        memcpy(&before, &array, sizeof(array));
        unsigned char *pages[16] = {0};
        for (uint32_t page = 0; page < array.page_count; page += 1u) {
            pages[page] = array.pages[page];
        }
        uint32_t live = heap.live;

        heap.fail_from = heap.attempts + failing;
        ta_result result = ta_paged_array_grow(&array, to);
        if (result == TA_OK) {
            /* Every allocation of the grow has been failed once. */
            CHECK(failing > 1u);
            ta_paged_array_destroy(&array);
            CHECK(heap.live == 0);
            return;
        }
        CHECK(result == TA_ERR_NOMEM);
        CHECK(memcmp(&array, &before, sizeof(array)) == 0);
        CHECK(heap.live == live);
        for (uint32_t page = 0; page < array.page_count; page += 1u) {
            CHECK(array.pages[page] == pages[page]);
        }
        for (uint32_t position = 0; position < keep; position += 1u) {
            CHECK(holds(&array, position));
        }
        heap.fail_from = 0;
        ta_paged_array_destroy(&array);
        CHECK(heap.live == 0);
    }
}

static void test_failed_grow(void) {
    check_failed_grow(0, 0, 12);   /* a first directory, then three pages */
    check_failed_grow(4, 4, 20);   /* a larger directory, then four pages */
    check_failed_grow(16, 12, 16); /* room in the directory: one page */
}

/* 7 */
static void test_largest_count(void) {
    counting_allocator heap = { .largest = 4096 };
    ta_paged_array array;
    CHECK(ta_paged_array_init(&array, &(ta_paged_array_params){
                                          .allocator = allocator_of(&heap),
                                          .entry_size = 1,
                                          .entry_alignment = 1,
                                          .page_shift = 31,
                                      }) == TA_OK);
    CHECK(ta_paged_array_grow(&array, UINT32_MAX) == TA_ERR_NOMEM);
    CHECK(heap.attempts == 2);
    CHECK(heap.requested[0] == 2u * sizeof(unsigned char *));
    CHECK(heap.requested[1] == (size_t)1u << 31);
    CHECK(array.page_count == 0 && array.pages == NULL && heap.live == 0);
    CHECK(ta_paged_array_capacity(&array) == 0);

    /* With one entry per page, the largest count needs a directory of
     * 2^32 - 1 page addresses, asked for before any page. */
    CHECK(ta_paged_array_init(&array, &(ta_paged_array_params){
                                          .allocator = allocator_of(&heap),
                                          .entry_size = 1,
                                          .entry_alignment = 1,
                                          .page_shift = 0,
                                      }) == TA_OK);
    heap.attempts = 0;
    CHECK(ta_paged_array_grow(&array, UINT32_MAX) == TA_ERR_NOMEM);
    CHECK(heap.attempts == 1);
    CHECK(heap.requested[0] == (size_t)UINT32_MAX * sizeof(unsigned char *));
    CHECK(array.pages == NULL && heap.live == 0);

    /* Capacity reaches 2^32 when every page of the widest shift is held. */
    ta_paged_array full = array;
    full.page_shift = 31;
    full.page_count = 2;
    CHECK(ta_paged_array_capacity(&full) == UINT64_C(1) << 32);
}

/* 8 */
static void test_access_never_allocates(void) {
    counting_allocator heap = {0};
    ta_paged_array array;
    ta_paged_array_params params = entry_params(&heap);
    CHECK(ta_paged_array_init(&array, &params) == TA_OK);
    CHECK(ta_paged_array_grow(&array, 8) == TA_OK);
    uint32_t attempts = heap.attempts;
    heap.fail_from = attempts + 1u; /* any allocation from here on fails */
    for (uint32_t position = 0; position < 8; position += 1u) {
        put(&array, position);
        CHECK(holds(&array, position));
        CHECK(ta_paged_array_at(&array, position) != NULL);
    }
    CHECK(ta_paged_array_capacity(&array) == 8);
    CHECK(heap.attempts == attempts);
    heap.fail_from = 0;
    ta_paged_array_destroy(&array);
}

/* 9 */
static void test_destroy(void) {
    counting_allocator heap = {0};
    ta_paged_array array;
    ta_paged_array_params params = entry_params(&heap);
    CHECK(ta_paged_array_init(&array, &params) == TA_OK);
    CHECK(ta_paged_array_grow(&array, 30) == TA_OK);
    CHECK(heap.live == 9);
    ta_paged_array_destroy(&array);
    CHECK(heap.live == 0);
    CHECK(array.pages == NULL && array.page_count == 0 && array.directory_capacity == 0);
    CHECK(ta_paged_array_capacity(&array) == 0);
    ta_paged_array_destroy(&array);
    CHECK(heap.live == 0);
}

int main(void) {
    test_init();
    test_grow_rounds_to_pages();
    test_positions();
    test_grow_copies_nothing();
    test_shrink();
    test_failed_grow();
    test_largest_count();
    test_access_never_allocates();
    test_destroy();
    if (failures != 0) {
        fprintf(stderr, "paged_array_test: %d checks failed\n", failures);
        return 1;
    }
    printf("paged_array_test: ok\n");
    return 0;
}
