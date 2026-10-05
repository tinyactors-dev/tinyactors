/*
 * ta_table: an open-addressing hash table of non-zero uint32_t values,
 * keys hashed and compared by their owner, linear probing, deletion by
 * backward shift, fixed or growing only on an explicit reserve
 * (src/table.h). The atom table runs on it.
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Memory comes from the test
 * allocator (test_allocator.h), which can refuse any allocation and
 * counts the blocks that are live.
 *
 * Claims proven here, in order:
 *
 * 1. init refuses an unknown mode and a fixed capacity of 0 with
 *    TA_ERR_INVALID, and a capacity above TA_TABLE_MAX_KEYS with
 *    TA_ERR_FULL, allocating nothing; a growing table of capacity 0
 *    allocates nothing, finds nothing, answers TA_ERR_FULL to insert and
 *    TA_ERR_NOT_FOUND to remove; init sizes the slots to the smallest
 *    power of two of at least 8 whose three quarters hold the capacity,
 *    and a fixed table's limit is its capacity; an init whose allocation
 *    is refused answers TA_ERR_NOMEM, holds nothing and leaks nothing;
 * 2. insert, find and remove with one key and with several: find answers
 *    the value stored, remove answers it (or takes NULL) and the key is
 *    gone, an absent key answers TA_ERR_NOT_FOUND, an insert of value 0
 *    answers TA_ERR_INVALID and changes nothing, and equal is called only
 *    for slots whose hash equals the key's;
 * 3. linear probing: a key lands in the first empty slot from its home,
 *    wrapping past the last slot, and keys with equal hashes are told
 *    apart by equal;
 * 4. backward shift: removing any key of a run, including runs that wrap
 *    and runs with keys from several homes, leaves every other key
 *    reachable, no slot marked deleted, and exactly the slots that
 *    inserting the remaining keys in the same order into an empty table
 *    gives;
 * 5. fixed mode: the table holds exactly its capacity, an insert beyond
 *    answers TA_ERR_FULL and a reserve beyond answers TA_ERR_FULL, both
 *    changing nothing; a reserve within the capacity succeeds; nothing
 *    after init allocates, and a removal makes room again;
 * 6. growing mode: an insert into a full table answers TA_ERR_FULL and
 *    changes nothing; reserve allocates only when the room is missing,
 *    then to the smallest power of two that has it, and every key stays
 *    reachable with its value; a refused reserve answers TA_ERR_NOMEM and
 *    a reserve past TA_TABLE_MAX_KEYS answers TA_ERR_FULL, both changing
 *    nothing and leaking nothing;
 * 7. the layout is deterministic: the same operations give the same
 *    slots, and a small sequence gives the slots pinned here;
 * 8. a long random run of inserts, removals, finds and reserves agrees
 *    with a model, in both modes, keys colliding on purpose;
 * 9. the atom table on it: atoms are numbered in interning order across
 *    growth, each keeps the 32-bit FNV-1a hash of its text from
 *    TA_HASH_FNV1A32_BASIS, interning again or finding answers the same
 *    atom, find never
 *    allocates, an intern refused at any allocation answers TA_ERR_NOMEM
 *    and adds no atom, and every text stays found;
 * 10. insert, find and remove never allocate; destroy frees the slots and
 *    may be called again.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

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

/*
 * The owner's key storage: the key stored under value v is keys[v]. The
 * test chooses each key's hash freely, so it can make keys collide.
 */
#define STORE_KEYS 4096u

typedef struct {
    uint32_t keys[STORE_KEYS];
    uint32_t equal_calls;
} store;

typedef struct {
    store *store;
    uint32_t key;
} lookup;

static bool store_equal(const void *context, uint32_t value) {
    const lookup *look = context;
    look->store->equal_calls += 1u;
    return look->store->keys[value] == look->key;
}

/* Finds `key` with hash `hash`; answers the result and stores the value. */
static ta_result find(const ta_table *table, store *keys, uint32_t key, uint32_t hash, uint32_t *value) {
    lookup look = { .store = keys, .key = key };
    return ta_table_find(table, &(ta_table_key){ .hash = hash, .equal = store_equal, .context = &look }, value);
}

static ta_result removed(ta_table *table, store *keys, uint32_t key, uint32_t hash, uint32_t *value) {
    lookup look = { .store = keys, .key = key };
    return ta_table_remove(table, &(ta_table_key){ .hash = hash, .equal = store_equal, .context = &look }, value);
}

/* The number of slots that hold a value. */
static uint32_t occupied(const ta_table *table) {
    uint32_t total = 0;
    for (uint32_t slot = 0; slot < table->slot_count; slot += 1u) {
        total += table->slots[slot].value != 0 ? 1u : 0u;
    }
    return total;
}

/* A copy of a table's visible state, to show a failed call changed nothing. */
typedef struct {
    ta_table_slot *slots_pointer;
    uint32_t slot_count;
    uint32_t count;
    uint32_t limit;
    ta_table_slot slots[256];
} snapshot;

static void take_snapshot(const ta_table *table, snapshot *out) {
    out->slots_pointer = table->slots;
    out->slot_count = table->slot_count;
    out->count = table->count;
    out->limit = table->limit;
    if (table->slot_count <= 256u && table->slots != NULL) {
        memcpy(out->slots, table->slots, table->slot_count * sizeof(ta_table_slot));
    }
}

static bool same_as(const ta_table *table, const snapshot *before) {
    if (table->slots != before->slots_pointer || table->slot_count != before->slot_count ||
        table->count != before->count || table->limit != before->limit) {
        return false;
    }
    return table->slot_count > 256u || table->slots == NULL ||
           memcmp(table->slots, before->slots, table->slot_count * sizeof(ta_table_slot)) == 0;
}

static ta_table_params growing(ta_allocator allocator, uint32_t capacity) {
    return (ta_table_params){ .allocator = allocator, .mode = TA_TABLE_GROWING, .capacity = capacity };
}

static ta_table_params fixed(ta_allocator allocator, uint32_t capacity) {
    return (ta_table_params){ .allocator = allocator, .mode = TA_TABLE_FIXED, .capacity = capacity };
}

/* ------------------------------------------------------------------------
 * Claim 1: init.
 */

static void test_init(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    ta_table table;

    CHECK(ta_table_init(&table, &(ta_table_params){ .allocator = allocator, .mode = 0, .capacity = 4 }) ==
          TA_ERR_INVALID);
    CHECK(ta_table_init(&table, &(ta_table_params){ .allocator = allocator, .mode = 7, .capacity = 4 }) ==
          TA_ERR_INVALID);
    CHECK(ta_table_init(&table, &(ta_table_params){ .allocator = allocator, .mode = 7, .capacity = 0 }) ==
          TA_ERR_INVALID);
    CHECK(ta_table_init(&table, (ta_table_params[]){ fixed(allocator, 0) }) == TA_ERR_INVALID);
    CHECK(table.slots == NULL && table.count == 0 && table.limit == 0);
    CHECK(ta_table_init(&table, (ta_table_params[]){ fixed(allocator, TA_TABLE_MAX_KEYS + 1u) }) == TA_ERR_FULL);
    CHECK(ta_table_init(&table, (ta_table_params[]){ growing(allocator, UINT32_MAX) }) == TA_ERR_FULL);
    CHECK(table.slots == NULL && table.limit == 0);
    ta_table_destroy(&table);
    CHECK(heap.attempts == 0);

    /* An empty growing table. */
    store keys = {0};
    CHECK(ta_table_init(&table, (ta_table_params[]){ growing(allocator, 0) }) == TA_OK);
    CHECK(heap.attempts == 0);
    CHECK(table.slot_count == 0 && table.count == 0 && table.limit == 0);
    uint32_t value = 99;
    CHECK(find(&table, &keys, 1, 1, &value) == TA_ERR_NOT_FOUND && value == 99);
    CHECK(removed(&table, &keys, 1, 1, &value) == TA_ERR_NOT_FOUND && value == 99);
    CHECK(ta_table_insert(&table, 1, 1) == TA_ERR_FULL);
    CHECK(keys.equal_calls == 0);
    ta_table_destroy(&table);
    CHECK(heap.attempts == 0 && heap.frees == 0);

    /* Sizes: three quarters of a power of two of at least 8. */
    const struct {
        uint32_t capacity;
        uint32_t slots;
        uint32_t growing_limit;
    } sizes[] = { { 1, 8, 6 }, { 6, 8, 6 }, { 7, 16, 12 }, { 12, 16, 12 }, { 13, 32, 24 }, { 48, 64, 48 },
                  { 49, 128, 96 } };
    for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index += 1) {
        CHECK(ta_table_init(&table, (ta_table_params[]){ growing(allocator, sizes[index].capacity) }) == TA_OK);
        CHECK(table.slot_count == sizes[index].slots && table.limit == sizes[index].growing_limit);
        CHECK(table.count == 0 && occupied(&table) == 0);
        CHECK(heap.last_size == sizes[index].slots * sizeof(ta_table_slot));
        ta_table_destroy(&table);
        CHECK(ta_table_init(&table, (ta_table_params[]){ fixed(allocator, sizes[index].capacity) }) == TA_OK);
        CHECK(table.slot_count == sizes[index].slots && table.limit == sizes[index].capacity);
        ta_table_destroy(&table);
    }

    /* A refused allocation. */
    heap.policy.refuse = true;
    CHECK(ta_table_init(&table, (ta_table_params[]){ fixed(allocator, 10) }) == TA_ERR_NOMEM);
    CHECK(table.slots == NULL && table.slot_count == 0 && table.count == 0 && table.limit == 0);
    CHECK(ta_table_init(&table, (ta_table_params[]){ growing(allocator, 10) }) == TA_ERR_NOMEM);
    CHECK(table.slots == NULL && table.limit == 0);
    ta_table_destroy(&table);
    heap.policy.refuse = false;
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 2: insert, find and remove.
 */

static void test_basic(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    store keys = {0};
    ta_table table;
    CHECK(ta_table_init(&table, (ta_table_params[]){ growing(allocator, 6) }) == TA_OK);

    /* One key. */
    keys.keys[5] = 500;
    CHECK(ta_table_insert(&table, 0x1234u, 5) == TA_OK);
    CHECK(table.count == 1 && occupied(&table) == 1);
    uint32_t value = 0;
    CHECK(find(&table, &keys, 500, 0x1234u, &value) == TA_OK && value == 5);
    CHECK(keys.equal_calls == 1);
    /* A different hash never reaches equal. */
    keys.equal_calls = 0;
    value = 77;
    CHECK(find(&table, &keys, 500, 0x1235u, &value) == TA_ERR_NOT_FOUND && value == 77);
    CHECK(keys.equal_calls == 0);
    /* Same hash, another key: equal says no. */
    CHECK(find(&table, &keys, 501, 0x1234u, &value) == TA_ERR_NOT_FOUND && value == 77);
    CHECK(keys.equal_calls == 1);
    CHECK(removed(&table, &keys, 501, 0x1234u, &value) == TA_ERR_NOT_FOUND && table.count == 1);
    CHECK(removed(&table, &keys, 500, 0x1234u, &value) == TA_OK && value == 5);
    CHECK(table.count == 0 && occupied(&table) == 0);
    CHECK(find(&table, &keys, 500, 0x1234u, &value) == TA_ERR_NOT_FOUND);
    CHECK(removed(&table, &keys, 500, 0x1234u, &value) == TA_ERR_NOT_FOUND);

    /* Value 0 is refused. */
    snapshot before;
    take_snapshot(&table, &before);
    CHECK(ta_table_insert(&table, 3, 0) == TA_ERR_INVALID);
    CHECK(same_as(&table, &before));

    /* Several keys, removal with NULL. */
    for (uint32_t v = 1; v <= 6; v += 1u) {
        keys.keys[v] = v * 10u;
        CHECK(ta_table_insert(&table, v * 0x9E3779B9u, v) == TA_OK);
    }
    CHECK(table.count == 6);
    for (uint32_t v = 1; v <= 6; v += 1u) {
        CHECK(find(&table, &keys, v * 10u, v * 0x9E3779B9u, &value) == TA_OK && value == v);
    }
    CHECK(removed(&table, &keys, 30, 3u * 0x9E3779B9u, NULL) == TA_OK && table.count == 5);
    CHECK(find(&table, &keys, 30, 3u * 0x9E3779B9u, &value) == TA_ERR_NOT_FOUND);
    ta_table_destroy(&table);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 3: linear probing.
 */

static void test_probing(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    store keys = {0};
    ta_table table;
    CHECK(ta_table_init(&table, (ta_table_params[]){ fixed(allocator, 6) }) == TA_OK);
    CHECK(table.slot_count == 8);

    /* Three keys at home 6: slots 6, 7, then 0 after the wrap. */
    for (uint32_t v = 1; v <= 3; v += 1u) {
        keys.keys[v] = 100u + v;
        CHECK(ta_table_insert(&table, 6u + 8u * v, v) == TA_OK);
    }
    CHECK(table.slots[6].value == 1 && table.slots[7].value == 2 && table.slots[0].value == 3);
    /* A key at home 0 goes past the wrapped one. */
    keys.keys[4] = 104;
    CHECK(ta_table_insert(&table, 0, 4) == TA_OK);
    CHECK(table.slots[1].value == 4);
    /* Two keys with one hash, told apart by equal. */
    keys.keys[5] = 105;
    keys.keys[6] = 106;
    CHECK(ta_table_insert(&table, 3, 5) == TA_OK);
    CHECK(ta_table_insert(&table, 3, 6) == TA_OK);
    CHECK(table.slots[3].value == 5 && table.slots[4].value == 6);
    uint32_t value = 0;
    CHECK(find(&table, &keys, 106, 3, &value) == TA_OK && value == 6);
    CHECK(find(&table, &keys, 105, 3, &value) == TA_OK && value == 5);
    CHECK(find(&table, &keys, 103, 6u + 24u, &value) == TA_OK && value == 3);
    CHECK(find(&table, &keys, 104, 0, &value) == TA_OK && value == 4);
    /* An absent key with home 6 walks the run 6, 7, 0, 1 and stops at 2. */
    keys.equal_calls = 0;
    CHECK(find(&table, &keys, 999, 6u + 8u, &value) == TA_ERR_NOT_FOUND);
    CHECK(keys.equal_calls == 1);
    ta_table_destroy(&table);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 4: backward shift.
 */

/* Fills `table` (8 slots) with keys 1..n, value v, key 100 + v, hash hashes[v - 1]. */
static void fill(ta_table *table, store *keys, const uint32_t *hashes, uint32_t n, uint32_t skip) {
    for (uint32_t v = 1; v <= n; v += 1u) {
        keys->keys[v] = 100u + v;
        if (v != skip) {
            CHECK(ta_table_insert(table, hashes[v - 1u], v) == TA_OK);
        }
    }
}

static void test_backward_shift(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    store keys = {0};
    /* Runs with several homes that wrap: homes 6, 7, 6, 0, 7, 1 in 8 slots. */
    const uint32_t layouts[][6] = {
        { 6, 7, 14, 0, 15, 1 },
        { 5, 5, 5, 5, 5, 5 },
        { 7, 7, 7, 0, 0, 1 },
        { 2, 3, 2, 4, 3, 2 },
        { 0, 8, 16, 1, 9, 2 },
    };
    for (size_t layout = 0; layout < sizeof(layouts) / sizeof(layouts[0]); layout += 1) {
        for (uint32_t victim = 1; victim <= 6; victim += 1u) {
            ta_table table;
            ta_table expected;
            CHECK(ta_table_init(&table, (ta_table_params[]){ fixed(allocator, 6) }) == TA_OK);
            CHECK(ta_table_init(&expected, (ta_table_params[]){ fixed(allocator, 6) }) == TA_OK);
            fill(&table, &keys, layouts[layout], 6, 0);
            fill(&expected, &keys, layouts[layout], 6, victim);
            uint32_t value = 0;
            CHECK(removed(&table, &keys, 100u + victim, layouts[layout][victim - 1u], &value) == TA_OK);
            CHECK(value == victim);
            CHECK(table.count == 5 && occupied(&table) == 5);
            CHECK(memcmp(table.slots, expected.slots, 8 * sizeof(ta_table_slot)) == 0);
            for (uint32_t v = 1; v <= 6; v += 1u) {
                ta_result result = find(&table, &keys, 100u + v, layouts[layout][v - 1u], &value);
                if (v == victim) {
                    CHECK(result == TA_ERR_NOT_FOUND);
                } else {
                    CHECK(result == TA_OK && value == v);
                }
            }
            ta_table_destroy(&table);
            ta_table_destroy(&expected);
        }
    }

    /* Remove every key of a full table, one by one, in an order that
     * breaks runs in the middle; the rest stay reachable each time. */
    ta_table table;
    CHECK(ta_table_init(&table, (ta_table_params[]){ fixed(allocator, 6) }) == TA_OK);
    fill(&table, &keys, layouts[0], 6, 0);
    const uint32_t order[] = { 3, 1, 5, 6, 2, 4 };
    for (uint32_t step = 0; step < 6; step += 1u) {
        CHECK(removed(&table, &keys, 100u + order[step], layouts[0][order[step] - 1u], NULL) == TA_OK);
        for (uint32_t later = step + 1u; later < 6; later += 1u) {
            uint32_t value = 0;
            CHECK(find(&table, &keys, 100u + order[later], layouts[0][order[later] - 1u], &value) == TA_OK &&
                  value == order[later]);
        }
        CHECK(occupied(&table) == table.count);
    }
    CHECK(table.count == 0 && occupied(&table) == 0);
    ta_table_destroy(&table);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 5: fixed mode.
 */

static void test_fixed(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    store keys = {0};
    ta_table table;
    CHECK(ta_table_init(&table, (ta_table_params[]){ fixed(allocator, 10) }) == TA_OK);
    CHECK(table.slot_count == 16 && table.limit == 10);
    uint32_t attempts = heap.attempts;

    CHECK(ta_table_reserve(&table, 10) == TA_OK);
    snapshot before;
    take_snapshot(&table, &before);
    CHECK(ta_table_reserve(&table, 11) == TA_ERR_FULL);
    CHECK(ta_table_reserve(&table, UINT32_MAX) == TA_ERR_FULL);
    CHECK(same_as(&table, &before));
    for (uint32_t v = 1; v <= 10; v += 1u) {
        keys.keys[v] = v;
        CHECK(ta_table_insert(&table, v * 7u, v) == TA_OK);
    }
    CHECK(table.count == 10);
    CHECK(ta_table_reserve(&table, 0) == TA_OK);
    take_snapshot(&table, &before);
    CHECK(ta_table_insert(&table, 3, 11) == TA_ERR_FULL);
    CHECK(ta_table_reserve(&table, 1) == TA_ERR_FULL);
    CHECK(same_as(&table, &before));
    CHECK(removed(&table, &keys, 4, 28, NULL) == TA_OK);
    CHECK(ta_table_reserve(&table, 1) == TA_OK);
    keys.keys[11] = 11;
    CHECK(ta_table_insert(&table, 3, 11) == TA_OK);
    for (uint32_t v = 1; v <= 11; v += 1u) {
        uint32_t value = 0;
        ta_result result = find(&table, &keys, v, v == 11 ? 3u : v * 7u, &value);
        CHECK(v == 4 ? result == TA_ERR_NOT_FOUND : (result == TA_OK && value == v));
    }
    CHECK(heap.attempts == attempts);
    ta_table_destroy(&table);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 6: growing mode.
 */

static uint32_t weak_hash(uint32_t key) {
    return key * 0x01000193u;
}

static void test_growing(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    store keys = {0};
    ta_table table;
    CHECK(ta_table_init(&table, (ta_table_params[]){ growing(allocator, 0) }) == TA_OK);

    uint32_t held = 0;
    uint32_t grown = 0;
    for (uint32_t v = 1; v <= 1000; v += 1u) {
        keys.keys[v] = v ^ 0x5555u;
        if (table.count == table.limit) {
            snapshot before;
            take_snapshot(&table, &before);
            CHECK(ta_table_insert(&table, weak_hash(keys.keys[v]), v) == TA_ERR_FULL);
            CHECK(same_as(&table, &before));
            /* Refused: nothing changes, nothing leaks. */
            heap.policy.refuse = true;
            CHECK(ta_table_reserve(&table, 1) == TA_ERR_NOMEM);
            heap.policy.refuse = false;
            CHECK(same_as(&table, &before));
            uint32_t old_slots = table.slot_count;
            uint32_t attempts = heap.attempts;
            CHECK(ta_table_reserve(&table, 1) == TA_OK);
            CHECK(heap.attempts == attempts + 1u);
            CHECK(table.slot_count == (old_slots == 0 ? 8u : old_slots * 2u));
            CHECK(table.limit == table.slot_count - table.slot_count / 4u);
            CHECK(heap.live == 1);
            grown += 1u;
        } else {
            uint32_t attempts = heap.attempts;
            CHECK(ta_table_reserve(&table, 1) == TA_OK);
            CHECK(heap.attempts == attempts);
        }
        CHECK(ta_table_insert(&table, weak_hash(keys.keys[v]), v) == TA_OK);
        held += 1u;
        CHECK(table.count == held);
    }
    CHECK(grown == 9); /* 8, 16, ..., 2048 slots */
    CHECK(table.slot_count == 2048);
    for (uint32_t v = 1; v <= 1000; v += 1u) {
        uint32_t value = 0;
        CHECK(find(&table, &keys, keys.keys[v], weak_hash(keys.keys[v]), &value) == TA_OK && value == v);
    }

    /* A reserve for many at once goes straight to the size that holds them. */
    CHECK(ta_table_reserve(&table, 3000) == TA_OK);
    CHECK(table.slot_count == 8192 && table.limit == 6144);
    for (uint32_t v = 1; v <= 1000; v += 1u) {
        uint32_t value = 0;
        CHECK(find(&table, &keys, keys.keys[v], weak_hash(keys.keys[v]), &value) == TA_OK && value == v);
    }

    /* Past the most keys a table holds; then the largest table, refused. */
    snapshot before;
    take_snapshot(&table, &before);
    CHECK(ta_table_reserve(&table, TA_TABLE_MAX_KEYS - 999u) == TA_ERR_FULL);
    CHECK(ta_table_reserve(&table, UINT32_MAX) == TA_ERR_FULL);
    heap.policy.largest = 1u << 20;
    CHECK(ta_table_reserve(&table, TA_TABLE_MAX_KEYS - 1000u) == TA_ERR_NOMEM);
    CHECK(heap.last_size == (size_t)TA_TABLE_MAX_SLOTS * sizeof(ta_table_slot) ||
          sizeof(size_t) < 8); /* a 32-bit size_t refuses before it asks */
    heap.policy.largest = 0;
    CHECK(same_as(&table, &before));
    CHECK(heap.live == 1);

    ta_table_destroy(&table);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 7: deterministic layout.
 */

static void build(ta_table *table, store *keys, ta_allocator allocator) {
    CHECK(ta_table_init(table, (ta_table_params[]){ growing(allocator, 0) }) == TA_OK);
    for (uint32_t v = 1; v <= 9; v += 1u) {
        keys->keys[v] = v * 3u;
        CHECK(ta_table_reserve(table, 1) == TA_OK);
        CHECK(ta_table_insert(table, v * 5u, v) == TA_OK);
    }
    CHECK(removed(table, keys, 6, 10, NULL) == TA_OK);
    CHECK(removed(table, keys, 15, 25, NULL) == TA_OK);
}

static void test_deterministic(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    store keys = {0};
    ta_table one;
    ta_table two;
    build(&one, &keys, allocator);
    build(&two, &keys, allocator);
    CHECK(one.slot_count == 16 && two.slot_count == 16);
    CHECK(memcmp(one.slots, two.slots, 16 * sizeof(ta_table_slot)) == 0);
    /* In 8 slots, hashes 5v for v = 1..6 have homes 5, 2, 7, 4, 1, 6. The
     * growth to 16 slots reinserts them in old slot order (v = 5, 2, 4, 1,
     * 6, 3 at 9, 10, 4, 5, 14, 15); v = 7, 8, 9 land at 3, 8, 13; then
     * v = 2 and v = 5 are removed. */
    uint32_t values[16];
    for (uint32_t slot = 0; slot < 16; slot += 1u) {
        values[slot] = one.slots[slot].value;
    }
    const uint32_t expected[16] = { 0, 0, 0, 7, 4, 1, 0, 0, 8, 0, 0, 0, 0, 9, 6, 3 };
    CHECK(memcmp(values, expected, sizeof(values)) == 0);
    ta_table_destroy(&one);
    ta_table_destroy(&two);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 8: a random run against a model.
 */

static uint64_t rng_state;

static uint32_t rng_next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

/* Keys from 1 to 600 collide on purpose: hash is key % 97 times a constant. */
static uint32_t model_hash(uint32_t key) {
    return (key % 97u) * 0x2545F491u;
}

static void random_run(ta_table_mode mode, uint32_t steps) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    static store keys;
    memset(&keys, 0, sizeof(keys));
    /* The model: value of key k, 0 when absent; values are k + 1. */
    static uint32_t model[601];
    memset(model, 0, sizeof(model));
    uint32_t model_count = 0;
    ta_table table;
    uint32_t capacity = mode == TA_TABLE_FIXED ? 200u : 0u;
    CHECK(ta_table_init(&table, &(ta_table_params){ .allocator = allocator, .mode = mode, .capacity = capacity }) ==
          TA_OK);
    for (uint32_t k = 1; k <= 600; k += 1u) {
        keys.keys[k + 1u] = k;
    }
    rng_state = 0x9E3779B97F4A7C15ull ^ (uint64_t)mode;
    for (uint32_t step = 0; step < steps; step += 1u) {
        uint32_t k = 1u + rng_next() % 600u;
        uint32_t op = rng_next() % 10u;
        uint32_t value = 0;
        if (op < 5) {
            ta_result found = find(&table, &keys, k, model_hash(k), &value);
            if (model[k] != 0) {
                CHECK(found == TA_OK && value == model[k]);
                continue;
            }
            CHECK(found == TA_ERR_NOT_FOUND);
            if (mode == TA_TABLE_GROWING && op == 0) {
                /* Now and then a refused reserve. */
                heap.policy.refuse = true;
                ta_result refused = ta_table_reserve(&table, table.limit - table.count + 1u);
                heap.policy.refuse = false;
                CHECK(refused == TA_ERR_NOMEM);
            }
            ta_result reserved = ta_table_reserve(&table, 1);
            if (mode == TA_TABLE_FIXED && model_count == capacity) {
                CHECK(reserved == TA_ERR_FULL);
                CHECK(ta_table_insert(&table, model_hash(k), k + 1u) == TA_ERR_FULL);
                continue;
            }
            CHECK(reserved == TA_OK);
            CHECK(ta_table_insert(&table, model_hash(k), k + 1u) == TA_OK);
            model[k] = k + 1u;
            model_count += 1u;
        } else if (op < 9) {
            ta_result result = removed(&table, &keys, k, model_hash(k), &value);
            if (model[k] != 0) {
                CHECK(result == TA_OK && value == model[k]);
                model[k] = 0;
                model_count -= 1u;
            } else {
                CHECK(result == TA_ERR_NOT_FOUND);
            }
        } else {
            uint32_t more = 1u + rng_next() % 50u;
            bool room = more <= table.limit - table.count;
            uint32_t attempts = heap.attempts;
            ta_result result = ta_table_reserve(&table, more);
            CHECK(result == (room || mode == TA_TABLE_GROWING ? TA_OK : TA_ERR_FULL));
            CHECK(room ? heap.attempts == attempts : true);
            CHECK(result != TA_OK || more <= table.limit - table.count);
        }
        CHECK(table.count == model_count && occupied(&table) == model_count);
        if (step % 64u == 0) {
            for (uint32_t key = 1; key <= 600; key += 1u) {
                ta_result result = find(&table, &keys, key, model_hash(key), &value);
                CHECK(model[key] != 0 ? (result == TA_OK && value == model[key]) : result == TA_ERR_NOT_FOUND);
            }
        }
    }
    ta_table_destroy(&table);
    CHECK(test_allocator_finish(&heap) == 0);
}

static void test_random(void) {
    random_run(TA_TABLE_GROWING, 20000);
    random_run(TA_TABLE_FIXED, 20000);
}

/* ------------------------------------------------------------------------
 * Claim 9: the atom table on it.
 */

static void test_atoms(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    ta_atom_table atoms;
    CHECK(ta_atom_table_init(&atoms, allocator) == TA_OK);
    ta_atom atom = TA_ATOM_NONE;
    CHECK(ta_atom_find(&atoms, (ta_str){ .ptr = "", .len = 0 }, &atom) == TA_OK && atom == 0);
    char text[32];
    for (uint32_t index = 1; index <= 500; index += 1u) {
        int len = snprintf(text, sizeof(text), "atom-%u", index);
        CHECK(ta_atom_intern(&atoms, (ta_str){ .ptr = text, .len = (size_t)len }, &atom) == TA_OK);
        CHECK(atom == index);
    }
    CHECK(atoms.index.count == 501 && atoms.entries.count == 501);
    uint32_t attempts = heap.attempts;
    for (uint32_t index = 1; index <= 500; index += 1u) {
        int len = snprintf(text, sizeof(text), "atom-%u", index);
        ta_str str = { .ptr = text, .len = (size_t)len };
        CHECK(ta_atom_find(&atoms, str, &atom) == TA_OK && atom == index);
        CHECK(ta_atom_intern(&atoms, str, &atom) == TA_OK && atom == index);
        CHECK(ta_str_equal(ta_atom_text(&atoms, index), str));
        const ta_atom_entry *entry = ta_vec_at(&atoms.entries, index);
        CHECK(entry->hash == ta_hash_fnv1a32(TA_HASH_FNV1A32_BASIS, str.ptr, str.len));
    }
    CHECK(ta_atom_find(&atoms, (ta_str){ .ptr = "atom-501", .len = 8 }, &atom) == TA_ERR_NOT_FOUND);
    CHECK(heap.attempts == attempts);

    /* Every allocation of a new intern refused in turn, until one succeeds. */
    uint32_t next = 501;
    for (uint32_t fail = 1; fail <= 64; fail += 1u) {
        int len = snprintf(text, sizeof(text), "late-%u", next);
        ta_str str = { .ptr = text, .len = (size_t)len };
        uint32_t count = atoms.entries.count;
        heap.policy.fail_at = heap.attempts + fail;
        ta_result result = ta_atom_intern(&atoms, str, &atom);
        heap.policy.fail_at = 0;
        if (result == TA_OK) {
            CHECK(atom == next);
            next += 1u;
            continue;
        }
        CHECK(result == TA_ERR_NOMEM);
        CHECK(atoms.entries.count == count && atoms.index.count == count);
        CHECK(ta_atom_find(&atoms, str, &atom) == TA_ERR_NOT_FOUND);
    }
    /* Refuse everything while the table is full, for an intern that must grow it. */
    while (atoms.index.count < atoms.index.limit) {
        int len = snprintf(text, sizeof(text), "fill-%u", next);
        CHECK(ta_atom_intern(&atoms, (ta_str){ .ptr = text, .len = (size_t)len }, &atom) == TA_OK && atom == next);
        next += 1u;
    }
    heap.policy.refuse = true;
    CHECK(ta_atom_intern(&atoms, (ta_str){ .ptr = "grow", .len = 4 }, &atom) == TA_ERR_NOMEM);
    heap.policy.refuse = false;
    CHECK(atoms.entries.count == next);
    for (uint32_t index = 1; index <= 500; index += 1u) {
        int len = snprintf(text, sizeof(text), "atom-%u", index);
        CHECK(ta_atom_find(&atoms, (ta_str){ .ptr = text, .len = (size_t)len }, &atom) == TA_OK && atom == index);
    }
    CHECK(ta_atom_intern(&atoms, (ta_str){ .ptr = "grow", .len = 4 }, &atom) == TA_OK && atom == next);
    ta_atom_table_destroy(&atoms);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 10: what allocates and what is freed.
 */

static void test_memory(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    store keys = {0};
    ta_table table;
    CHECK(ta_table_init(&table, (ta_table_params[]){ growing(allocator, 100) }) == TA_OK);
    CHECK(heap.allocations == 1 && heap.live == 1);
    uint32_t attempts = heap.attempts;
    for (uint32_t v = 1; v <= 100; v += 1u) {
        keys.keys[v] = v;
        CHECK(ta_table_insert(&table, v, v) == TA_OK);
    }
    for (uint32_t v = 1; v <= 100; v += 2u) {
        uint32_t value = 0;
        CHECK(find(&table, &keys, v, v, &value) == TA_OK);
        CHECK(removed(&table, &keys, v, v, &value) == TA_OK);
    }
    CHECK(heap.attempts == attempts);
    ta_table_destroy(&table);
    CHECK(heap.live == 0 && heap.frees == 1);
    CHECK(table.slots == NULL && table.count == 0 && table.limit == 0);
    ta_table_destroy(&table);
    CHECK(heap.frees == 1);
    CHECK(test_allocator_finish(&heap) == 0);
}

int main(void) {
    test_init();
    test_basic();
    test_probing();
    test_backward_shift();
    test_fixed();
    test_growing();
    test_deterministic();
    test_random();
    test_atoms();
    test_memory();
    if (failures != 0) {
        fprintf(stderr, "table_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("table_test: ok\n");
    return 0;
}
