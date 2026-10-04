/*
 * Block B3 (src/table.h): the open addressing hash table.
 *
 * Checks, in order: FIXED mode (full, then refuses; never allocates after
 * init), the reserved value, GROWING mode (the grow trigger, one page per
 * grow, every key still found after every grow, no block larger than a page
 * or the directory), backward-shift deletion on clustered keys (including a
 * run that wraps the end of the page) with the no-gap invariant checked
 * after every delete, equal hashes with different values, random insert and
 * remove against a reference model, an allocation failure sweep over init
 * and grow, and the strict allocator's verdict on every free (size and
 * alignment must be those of the allocation).
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

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint32_t random32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static bool contains(const ta_table *table, uint32_t hash, uint32_t value) {
    ta_table_probe probe;
    uint32_t candidate = 0;
    ta_table_probe_start(table, hash, &probe);
    while (ta_table_probe_next(&probe, &candidate)) {
        if (candidate == value) {
            return true;
        }
    }
    return false;
}

static uint32_t candidates(const ta_table *table, uint32_t hash) {
    ta_table_probe probe;
    uint32_t candidate = 0;
    uint32_t seen = 0;
    ta_table_probe_start(table, hash, &probe);
    while (ta_table_probe_next(&probe, &candidate)) {
        seen += 1u;
    }
    return seen;
}

/*
 * The invariant of open addressing without tombstones: from an entry's home
 * slot to the entry, no slot is empty. Checked over every page.
 */
static bool no_gaps(const ta_table *table) {
    uint32_t pages = table->pages.count;
    for (uint32_t index = 0; index < pages; index += 1u) {
        const ta_table_slot *slots = *(ta_table_slot **)ta_vec_at(&table->pages, index);
        uint32_t count = table->page_slots;
        for (uint32_t position = 0; position < count; position += 1u) {
            if (slots[position].value == TA_TABLE_NONE) {
                continue;
            }
            for (uint32_t at = ta_table_home(slots[position].hash, count); at != position; at = ta_table_after(at, count)) {
                if (slots[at].value == TA_TABLE_NONE) {
                    return false;
                }
            }
        }
    }
    return true;
}

static uint32_t max_page_fill_percent(const ta_table *table) {
    uint32_t worst = 0;
    for (uint32_t index = 0; index < table->pages.count; index += 1u) {
        const ta_table_slot *slots = *(ta_table_slot **)ta_vec_at(&table->pages, index);
        uint32_t used = 0;
        for (uint32_t position = 0; position < table->page_slots; position += 1u) {
            used += slots[position].value != TA_TABLE_NONE;
        }
        uint32_t percent = used * 100u / table->page_slots;
        worst = percent > worst ? percent : worst;
    }
    return worst;
}

static void test_fixed(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_table table;
    ta_table_params params = { .mode = TA_TABLE_FIXED, .max_entries = 100u };
    CHECK(ta_table_init(&table, ta_test_allocator_get(&heap), &params) == TA_OK);
    CHECK(table.page_slots == 135u && table.pages.count == 1u);
    uint64_t attempts = heap.stats.attempts;
    uint32_t hashes[100];
    for (uint32_t index = 0; index < 100u; index += 1u) {
        hashes[index] = random32();
        CHECK(ta_table_insert(&table, hashes[index], index) == TA_OK);
        CHECK(!ta_table_wants_grow(&table));
    }
    CHECK(ta_table_count(&table) == 100u);
    CHECK(ta_table_insert(&table, random32(), 100u) == TA_ERR_NOMEM);
    CHECK(ta_table_count(&table) == 100u);
    CHECK(ta_table_grow(&table) == TA_ERR_INVALID);
    CHECK(heap.stats.attempts == attempts); /* neither insert nor the refused grow asked for memory */
    for (uint32_t index = 0; index < 100u; index += 1u) {
        CHECK(contains(&table, hashes[index], index));
    }
    CHECK(no_gaps(&table));
    CHECK(ta_table_remove(&table, hashes[7], 7u));
    CHECK(ta_table_insert(&table, hashes[7], 7u) == TA_OK);
    CHECK(ta_table_bytes(&table) == table.page_bytes + ta_vec_bytes(&table.pages));
    ta_table_destroy(&table);
    ta_table_destroy(&table); /* idempotent */
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);

    CHECK(ta_table_init(&table, ta_test_allocator_get(&heap),
                        &(ta_table_params){ .mode = TA_TABLE_FIXED }) == TA_ERR_INVALID);
    CHECK(table.pages.count == 0 && table.page_bytes == 0);
    CHECK(ta_table_init(&table, ta_test_allocator_get(&heap),
                        &(ta_table_params){ .mode = TA_TABLE_FIXED, .max_entries = UINT32_MAX }) == TA_ERR_NOMEM);
    CHECK(ta_test_allocator_finish(&heap) == 0);
}

static void test_reserved_and_params(void) {
    ta_table table;
    ta_allocator allocator = ta_test_allocator_get(&heap);
    CHECK(ta_table_init(&table, allocator, &(ta_table_params){ .mode = TA_TABLE_GROWING, .page_slots = 8u }) == TA_ERR_INVALID);
    CHECK(ta_table_init(&table, allocator,
                        &(ta_table_params){ .mode = TA_TABLE_GROWING, .page_slots = (1u << 20) + 1u }) == TA_ERR_INVALID);
    CHECK(ta_table_init(&table, allocator, &(ta_table_params){ .mode = (ta_table_mode)7 }) == TA_ERR_INVALID);
    CHECK(ta_table_init(&table, allocator, &(ta_table_params){ .mode = TA_TABLE_GROWING, .page_slots = 16u }) == TA_OK);
    CHECK(ta_table_insert(&table, 5u, TA_TABLE_NONE) == TA_ERR_INVALID);
    CHECK(ta_table_count(&table) == 0);
    CHECK(!ta_table_remove(&table, 5u, TA_TABLE_NONE));
    CHECK(!ta_table_remove(&table, 5u, 1u));
    CHECK(candidates(&table, 5u) == 0);
    ta_table_destroy(&table);
    CHECK(ta_test_allocator_finish(&heap) == 0);
}

static void test_growing(void) {
    enum { SLOTS = 256, KEYS = 20000 };
    static uint32_t hashes[KEYS];
    ta_test_allocator_reset_counts(&heap);
    ta_table table;
    CHECK(ta_table_init(&table, ta_test_allocator_get(&heap),
                        &(ta_table_params){ .mode = TA_TABLE_GROWING, .page_slots = SLOTS }) == TA_OK);
    size_t page_bytes = SLOTS * sizeof(ta_table_slot);
    CHECK(table.page_bytes == page_bytes);

    /* The trigger: more than 3/8 of one 256 slot page is 97 entries. */
    for (uint32_t index = 0; index < 96u; index += 1u) {
        hashes[index] = random32();
        CHECK(ta_table_insert(&table, hashes[index], index) == TA_OK);
    }
    CHECK(!ta_table_wants_grow(&table));
    hashes[96] = random32();
    CHECK(ta_table_insert(&table, hashes[96], 96u) == TA_OK);
    CHECK(ta_table_wants_grow(&table));
    uint32_t inserted = 97u;

    uint32_t grows = 0;
    size_t largest = 0;
    while (inserted < KEYS) {
        if (ta_table_wants_grow(&table)) {
            uint64_t before = heap.stats.allocations;
            uint32_t pages = table.pages.count;
            CHECK(ta_table_grow(&table) == TA_OK);
            grows += 1u;
            uint64_t made = heap.stats.allocations - before;
            /* One page; the directory too, when it was full. */
            CHECK(made == 1u || made == 2u);
            CHECK(table.pages.count == pages + 1u);
            if (grows <= 20u || grows % 17u == 0) {
                for (uint32_t index = 0; index < inserted; index += 1u) {
                    CHECK(contains(&table, hashes[index], index));
                }
                CHECK(no_gaps(&table));
            }
        }
        hashes[inserted] = random32();
        CHECK(ta_table_insert(&table, hashes[inserted], inserted) == TA_OK);
        inserted += 1u;
    }
    for (uint32_t index = 0; index < KEYS; index += 1u) {
        CHECK(contains(&table, hashes[index], index));
    }
    CHECK(ta_table_count(&table) == KEYS);
    CHECK(max_page_fill_percent(&table) < 85u);
    CHECK(grows > 100u);
    CHECK(ta_table_bytes(&table) == (size_t)table.pages.count * page_bytes + ta_vec_bytes(&table.pages));
    /* Nothing larger than a page or the directory was ever asked for. */
    largest = heap.stats.largest_block;
    CHECK(largest <= (page_bytes > ta_vec_bytes(&table.pages) ? page_bytes : ta_vec_bytes(&table.pages)));
    ta_table_destroy(&table);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

/* Hashes whose home slot in a page of `slots` slots lies in [first, first + width), wrapping. */
static uint32_t clustered_hashes(uint32_t *out, uint32_t wanted, uint32_t slots, uint32_t first) {
    uint32_t found = 0;
    for (uint32_t hash = 1; found < wanted; hash += 1u) {
        uint32_t home = ta_table_home(hash, slots);
        if ((home + slots - first) % slots < 12u) {
            out[found++] = hash;
        }
    }
    return found;
}

static void check_survivors(const ta_table *table, const uint32_t *hashes, const bool *live, uint32_t count) {
    for (uint32_t index = 0; index < count; index += 1u) {
        CHECK(contains(table, hashes[index], index) == live[index]);
    }
    CHECK(no_gaps(table));
}

static void delete_in_order(uint32_t order_kind) {
    enum { SLOTS = 256, KEYS = 200 };
    uint32_t hashes[KEYS];
    bool live[KEYS];
    uint32_t order[KEYS];
    ta_test_allocator_reset_counts(&heap);
    ta_table table;
    CHECK(ta_table_init(&table, ta_test_allocator_get(&heap),
                        &(ta_table_params){ .mode = TA_TABLE_GROWING, .page_slots = SLOTS }) == TA_OK);
    /* Twelve consecutive home slots starting at 250: the run wraps the end of the page. */
    CHECK(clustered_hashes(hashes, KEYS, SLOTS, 250u) == KEYS);
    for (uint32_t index = 0; index < KEYS; index += 1u) {
        CHECK(ta_table_insert(&table, hashes[index], index) == TA_OK);
        live[index] = true;
        order[index] = order_kind == 1u ? KEYS - 1u - index : index;
    }
    if (order_kind == 2u) {
        for (uint32_t index = KEYS - 1u; index > 0; index -= 1u) {
            uint32_t other = random32() % (index + 1u);
            uint32_t swap = order[index];
            order[index] = order[other];
            order[other] = swap;
        }
    }
    check_survivors(&table, hashes, live, KEYS);
    for (uint32_t step = 0; step < KEYS; step += 1u) {
        uint32_t victim = order[step];
        CHECK(ta_table_remove(&table, hashes[victim], victim));
        CHECK(!ta_table_remove(&table, hashes[victim], victim));
        live[victim] = false;
        check_survivors(&table, hashes, live, KEYS);
    }
    CHECK(ta_table_count(&table) == 0);
    ta_table_destroy(&table);
    CHECK(ta_test_allocator_finish(&heap) == 0);
}

static void test_backward_shift(void) {
    delete_in_order(0); /* forward */
    delete_in_order(1); /* reverse */
    delete_in_order(2); /* random */
}

static void test_equal_hashes(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_table table;
    CHECK(ta_table_init(&table, ta_test_allocator_get(&heap),
                        &(ta_table_params){ .mode = TA_TABLE_GROWING, .page_slots = 64u }) == TA_OK);
    for (uint32_t value = 0; value < 10u; value += 1u) {
        CHECK(ta_table_insert(&table, 0xdeadbeefu, value) == TA_OK);
        CHECK(ta_table_insert(&table, 0xdeadbef0u, 100u + value) == TA_OK);
    }
    CHECK(candidates(&table, 0xdeadbeefu) == 10u);
    CHECK(candidates(&table, 0xdeadbef0u) == 10u);
    /* Removal takes exactly (hash, value): the same value under another hash stays. */
    CHECK(!ta_table_remove(&table, 0xdeadbeefu, 105u));
    CHECK(ta_table_remove(&table, 0xdeadbeefu, 5u));
    CHECK(!contains(&table, 0xdeadbeefu, 5u));
    CHECK(candidates(&table, 0xdeadbeefu) == 9u && candidates(&table, 0xdeadbef0u) == 10u);
    CHECK(no_gaps(&table));
    ta_table_destroy(&table);
    CHECK(ta_test_allocator_finish(&heap) == 0);
}

/* A page of identical hashes cannot be split apart: insert fails cleanly when it is full. */
static void test_page_full(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_table table;
    CHECK(ta_table_init(&table, ta_test_allocator_get(&heap),
                        &(ta_table_params){ .mode = TA_TABLE_GROWING, .page_slots = 16u }) == TA_OK);
    for (uint32_t value = 0; value < 16u; value += 1u) {
        CHECK(ta_table_insert(&table, 77u, value) == TA_OK);
    }
    CHECK(ta_table_insert(&table, 77u, 16u) == TA_ERR_NOMEM);
    CHECK(ta_table_count(&table) == 16u);
    CHECK(candidates(&table, 77u) == 16u);
    CHECK(ta_table_remove(&table, 77u, 3u));
    CHECK(ta_table_insert(&table, 77u, 16u) == TA_OK);
    /* Growing a table with a full page keeps all of its entries. */
    CHECK(ta_table_grow(&table) == TA_OK);
    CHECK(candidates(&table, 77u) == 16u);
    CHECK(ta_table_remove(&table, 77u, 16u));
    for (uint32_t value = 0; value < 16u; value += 1u) {
        CHECK(ta_table_remove(&table, 77u, value) == (value != 3u));
    }
    CHECK(ta_table_count(&table) == 0);
    ta_table_destroy(&table);
    CHECK(ta_test_allocator_finish(&heap) == 0);
}

static void test_random(uint32_t operations) {
    enum { VALUES = 20000, SLOTS = 256 };
    static uint32_t hashes[VALUES];
    static bool present[VALUES];
    ta_test_allocator_reset_counts(&heap);
    ta_table table;
    CHECK(ta_table_init(&table, ta_test_allocator_get(&heap),
                        &(ta_table_params){ .mode = TA_TABLE_GROWING, .page_slots = SLOTS }) == TA_OK);
    uint32_t reference = 0;
    uint32_t worst = 0;
    for (uint32_t value = 0; value < VALUES; value += 1u) {
        /* A quarter of the keys share one of 512 hashes: duplicates are normal. */
        hashes[value] = value % 4u == 0 ? random32() & 0x1ffu : random32();
        present[value] = false;
    }
    for (uint32_t operation = 0; operation < operations; operation += 1u) {
        uint32_t value = random32() % VALUES;
        if (present[value]) {
            CHECK(ta_table_remove(&table, hashes[value], value));
            present[value] = false;
            reference -= 1u;
        } else {
            if (ta_table_wants_grow(&table)) {
                CHECK(ta_table_grow(&table) == TA_OK);
            }
            CHECK(ta_table_insert(&table, hashes[value], value) == TA_OK);
            present[value] = true;
            reference += 1u;
        }
        CHECK(ta_table_count(&table) == reference);
        if (operation % 5000u == 0) {
            uint32_t fill = max_page_fill_percent(&table);
            worst = fill > worst ? fill : worst;
            CHECK(no_gaps(&table));
        }
    }
    for (uint32_t value = 0; value < VALUES; value += 1u) {
        CHECK(contains(&table, hashes[value], value) == present[value]);
    }
    CHECK(no_gaps(&table));
    CHECK(worst < 85u);
    ta_table_destroy(&table);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

/* Every allocation init and grow make is refused in turn: the table stays correct and nothing leaks. */
static void test_allocation_failures(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_table table;
    ta_table_params params = { .mode = TA_TABLE_GROWING, .page_slots = 64u };
    CHECK(ta_table_init(&table, ta_test_allocator_get(&heap), &params) == TA_OK);
    uint64_t init_allocations = heap.stats.allocations;
    ta_table_destroy(&table);
    CHECK(init_allocations == 2u && ta_test_allocator_finish(&heap) == 0);
    for (uint64_t nth = 1; nth <= init_allocations; nth += 1u) {
        ta_test_allocator_reset_counts(&heap);
        ta_test_allocator_fail_nth(&heap, nth);
        CHECK(ta_table_init(&table, ta_test_allocator_get(&heap), &params) == TA_ERR_NOMEM);
        CHECK(table.pages.count == 0 && table.pages.items == NULL && table.page_bytes == 0);
        ta_table_destroy(&table);
        ta_test_allocator_disarm(&heap);
        CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
    }

    /* Thirty grows, with an entry inserted between them; refuse each allocation of each grow once. */
    for (uint64_t nth = 1; nth <= 2u; nth += 1u) {
        ta_test_allocator_reset_counts(&heap);
        CHECK(ta_table_init(&table, ta_test_allocator_get(&heap), &params) == TA_OK);
        uint32_t hashes[400];
        uint32_t inserted = 0;
        uint32_t refused = 0;
        for (uint32_t round = 0; round < 30u; round += 1u) {
            for (uint32_t more = 0; more < 12u; more += 1u) {
                hashes[inserted] = random32();
                CHECK(ta_table_insert(&table, hashes[inserted], inserted) == TA_OK);
                inserted += 1u;
            }
            uint32_t pages = table.pages.count;
            size_t bytes = ta_table_bytes(&table);
            uint32_t base = table.base;
            uint32_t split = table.split;
            ta_test_allocator_fail_nth(&heap, nth);
            ta_result result = ta_table_grow(&table);
            ta_test_allocator_disarm(&heap);
            if (result == TA_ERR_NOMEM) {
                refused += 1u;
                CHECK(table.pages.count == pages);
                /* A directory block already enlarged is kept: the shape is unchanged, not the bytes. */
                CHECK(nth != 1u || ta_table_bytes(&table) == bytes);
                CHECK(table.base == base && table.split == split);
                for (uint32_t index = 0; index < inserted; index += 1u) {
                    CHECK(contains(&table, hashes[index], index));
                }
                CHECK(no_gaps(&table));
                CHECK(ta_table_grow(&table) == TA_OK); /* a retry works */
            } else {
                CHECK(result == TA_OK);
            }
        }
        /* The first allocation of a grow is a directory block only when the directory is full: a few times. */
        CHECK(refused >= 1u);
        for (uint32_t index = 0; index < inserted; index += 1u) {
            CHECK(contains(&table, hashes[index], index));
        }
        ta_table_destroy(&table);
        CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
    }
}

int main(void) {
    test_fixed();
    test_reserved_and_params();
    test_growing();
    test_backward_shift();
    test_equal_hashes();
    test_page_full();
    test_random(300000u);
    test_allocation_failures();
    if (failures != 0) {
        fprintf(stderr, "table_test: %d checks failed\n", failures);
        return 1;
    }
    printf("table_test: ok\n");
    return 0;
}
