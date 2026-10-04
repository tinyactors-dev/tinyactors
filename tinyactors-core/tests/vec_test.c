/*
 * Block B6 (src/vec.h): the growable tables, ta_vec and ta_columns.
 *
 * Every growth is swept: for each allocation a growth makes, the test
 * refuses exactly that one and checks that the table is untouched, that the
 * old items still read back, that a retry succeeds, and that the strict
 * allocator saw no bad free (a free whose size or alignment differs from its
 * allocation) and no leak. Overflow cases check that a size that does not
 * fit fails before it allocates.
 *
 * Includes the amalgamation, because the unit is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>

#include "test_allocator.h"

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

static ta_test_allocator heap = { .strict = true };

#define ITEMS 40u

typedef struct {
    uint64_t wide;
    uint8_t tag;
} wide_item;

static void test_vec_sweep(void) {
    /* Pushes of 40 elements make allocations at capacities 8, 16, 32, 64. */
    ta_test_allocator_reset_counts(&heap);
    ta_vec probe;
    ta_vec_init(&probe, ta_test_allocator_get(&heap), TA_VEC_LAYOUT(wide_item));
    for (uint32_t index = 0; index < ITEMS; index += 1u) {
        wide_item item = { .wide = index * 7u + 1u, .tag = (uint8_t)index };
        CHECK(ta_vec_push(&probe, &item) == TA_OK);
    }
    CHECK(ta_vec_bytes(&probe) == 64u * sizeof(wide_item));
    ta_vec_destroy(&probe);
    uint64_t allocations = heap.stats.allocations;
    CHECK(allocations == 4u);
    CHECK(ta_test_allocator_finish(&heap) == 0);

    for (uint64_t nth = 1; nth <= allocations; nth += 1u) {
        ta_vec vec;
        ta_vec_init(&vec, ta_test_allocator_get(&heap), TA_VEC_LAYOUT(wide_item));
        ta_test_allocator_fail_nth(&heap, nth);
        uint32_t failed = 0;
        for (uint32_t index = 0; index < ITEMS; index += 1u) {
            wide_item item = { .wide = index * 7u + 1u, .tag = (uint8_t)index };
            uint32_t count = vec.count;
            uint32_t capacity = vec.capacity;
            void *items = vec.items;
            ta_result result = ta_vec_push(&vec, &item);
            if (result != TA_OK) {
                failed += 1u;
                CHECK(result == TA_ERR_NOMEM);
                CHECK(vec.count == count && vec.capacity == capacity && vec.items == items);
                for (uint32_t old = 0; old < count; old += 1u) {
                    const wide_item *kept = ta_vec_at(&vec, old);
                    CHECK(kept->wide == old * 7u + 1u && kept->tag == (uint8_t)old);
                }
                CHECK(ta_vec_push(&vec, &item) == TA_OK);
            }
        }
        CHECK(failed == 1u);
        CHECK(vec.count == ITEMS);
        for (uint32_t index = 0; index < ITEMS; index += 1u) {
            const wide_item *kept = ta_vec_at(&vec, index);
            CHECK(kept->wide == index * 7u + 1u && kept->tag == (uint8_t)index);
        }
        ta_vec_destroy(&vec);
        ta_test_allocator_disarm(&heap);
        CHECK(heap.stats.bad_frees == 0);
        CHECK(ta_test_allocator_finish(&heap) == 0);
    }
}

static void test_vec_overflow(void) {
    ta_test_allocator_reset_counts(&heap);
    /* An element so large that eight of them do not fit in a size_t. */
    ta_tree_list_layout huge = { .element_size = SIZE_MAX / 4u, .element_alignment = 1u };
    ta_vec vec;
    ta_vec_init(&vec, ta_test_allocator_get(&heap), &huge);
    CHECK(ta_vec_reserve(&vec, 1) == TA_ERR_NOMEM);
    CHECK(vec.items == NULL && vec.capacity == 0 && vec.count == 0);
    CHECK(heap.stats.attempts == 0);
    /* A count whose doubling passes UINT32_MAX. */
    ta_vec_init(&vec, ta_test_allocator_get(&heap), TA_VEC_LAYOUT(uint8_t));
    CHECK(ta_vec_reserve(&vec, UINT32_MAX) == TA_ERR_NOMEM);
    CHECK(vec.items == NULL && heap.stats.attempts == 0);
    CHECK(ta_vec_extend(&vec, 1, NULL) == TA_OK);
    CHECK(ta_vec_extend(&vec, UINT32_MAX, NULL) == TA_ERR_NOMEM);
    CHECK(vec.count == 1u && vec.capacity == 8u);
    ta_vec_destroy(&vec);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

/* Three columns of different sizes and alignments, as a real table has. */
static const ta_column three_columns[] = {
    { sizeof(uint32_t), _Alignof(uint32_t) },
    { sizeof(uint8_t), _Alignof(uint8_t) },
    { sizeof(uint64_t), _Alignof(uint64_t) },
};
static const ta_columns_layout three = { three_columns, 3u, 4u };

static void fill(ta_columns *table, uint32_t index) {
    ((uint32_t *)ta_columns_at(&three, table, 0))[index] = index * 3u + 1u;
    ((uint8_t *)ta_columns_at(&three, table, 1))[index] = (uint8_t)(index ^ 0x5au);
    ((uint64_t *)ta_columns_at(&three, table, 2))[index] = ((uint64_t)index << 40) | index;
}

static bool intact(const ta_columns *table) {
    bool good = true;
    for (uint32_t index = 0; index < table->count; index += 1u) {
        good = good && ((uint32_t *)ta_columns_at(&three, table, 0))[index] == index * 3u + 1u;
        good = good && ((uint8_t *)ta_columns_at(&three, table, 1))[index] == (uint8_t)(index ^ 0x5au);
        good = good && ((uint64_t *)ta_columns_at(&three, table, 2))[index] == (((uint64_t)index << 40) | index);
    }
    return good;
}

static ta_result append(ta_columns *table) {
    ta_result result = ta_columns_grow(ta_test_allocator_get(&heap), &three, table);
    if (result == TA_OK) {
        fill(table, table->count);
        table->count += 1u;
    }
    return result;
}

static void test_columns_sweep(void) {
    /* 40 items from first capacity 4: blocks at capacities 4, 8, 16, 32, 64. */
    ta_test_allocator_reset_counts(&heap);
    ta_columns probe = { 0 };
    for (uint32_t index = 0; index < ITEMS; index += 1u) {
        CHECK(append(&probe) == TA_OK);
    }
    CHECK(intact(&probe) && probe.capacity == 64u);
    CHECK(ta_columns_bytes(&three, &probe) > 0);
    ta_columns_release(ta_test_allocator_get(&heap), &three, &probe);
    CHECK(probe.block == NULL && probe.count == 0 && probe.capacity == 0);
    CHECK(ta_columns_bytes(&three, &probe) == 0);
    uint64_t allocations = heap.stats.allocations;
    CHECK(allocations == 5u);
    CHECK(ta_test_allocator_finish(&heap) == 0);

    for (uint64_t nth = 1; nth <= allocations; nth += 1u) {
        ta_columns table = { 0 };
        ta_test_allocator_fail_nth(&heap, nth);
        uint32_t failed = 0;
        for (uint32_t index = 0; index < ITEMS; index += 1u) {
            ta_columns before = table;
            ta_result result = append(&table);
            if (result != TA_OK) {
                failed += 1u;
                CHECK(result == TA_ERR_NOMEM);
                CHECK(table.block == before.block && table.count == before.count &&
                      table.capacity == before.capacity);
                CHECK(intact(&table));
                CHECK(append(&table) == TA_OK);
            }
        }
        CHECK(failed == 1u && table.count == ITEMS && intact(&table));
        ta_columns_release(ta_test_allocator_get(&heap), &three, &table);
        ta_test_allocator_disarm(&heap);
        CHECK(heap.stats.bad_frees == 0);
        CHECK(ta_test_allocator_finish(&heap) == 0);
    }
}

static void test_columns_layout(void) {
    /* Column 0 starts the block; columns are aligned; bytes is the unpadded end. */
    ta_test_allocator_reset_counts(&heap);
    ta_columns table = { 0 };
    CHECK(ta_columns_at(&three, &table, 0) == NULL);
    CHECK(ta_columns_grow(ta_test_allocator_get(&heap), &three, &table) == TA_OK);
    CHECK(table.capacity == 4u && table.count == 0);
    CHECK(ta_columns_at(&three, &table, 0) == table.block);
    CHECK(ta_columns_at(&three, &table, 3) == NULL);
    CHECK((uintptr_t)ta_columns_at(&three, &table, 2) % _Alignof(uint64_t) == 0);
    /* 4 * 4 = 16, 4 * 1 = 4 (end 20), the wide column at 24 for 32 bytes. */
    CHECK(ta_columns_bytes(&three, &table) == 56u);
    ta_columns_release(ta_test_allocator_get(&heap), &three, &table);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

static void test_columns_overflow(void) {
    ta_test_allocator_reset_counts(&heap);
    static const ta_column big_columns[] = { { SIZE_MAX / 2u, 1u }, { SIZE_MAX / 2u, 1u } };
    static const ta_columns_layout big = { big_columns, 2u, 4u };
    ta_columns table = { 0 };
    CHECK(ta_columns_grow(ta_test_allocator_get(&heap), &big, &table) == TA_ERR_NOMEM);
    CHECK(table.block == NULL && table.capacity == 0 && heap.stats.attempts == 0);

    /* A table already at the largest capacity cannot double. */
    static const ta_column byte_column[] = { { 1u, 1u } };
    static const ta_columns_layout bytes = { byte_column, 1u, 4u };
    ta_columns full = { .block = &full, .count = 0x80000000u, .capacity = 0x80000000u };
    CHECK(ta_columns_grow(ta_test_allocator_get(&heap), &bytes, &full) == TA_ERR_NOMEM);
    CHECK(full.block == &full && full.capacity == 0x80000000u && heap.stats.attempts == 0);
}

int main(void) {
    test_vec_sweep();
    test_vec_overflow();
    test_columns_layout();
    test_columns_sweep();
    test_columns_overflow();
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "vec_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("vec_test: ok");
    return 0;
}
