/* test: standalone asan */
/*
 * The address table (src/address.h) on the pool and the hash table, and
 * what a caller who chooses the names can do to it.
 *
 * Names can come from outside (an HTTP endpoint mapping a request field to
 * a name), and the address hash is FNV-1a over 32 bits. The step of FNV-1a is
 * invertible, so names with one hash are cheap to build: for a state s, two
 * short strings that lead s to the same state are found by a birthday search
 * of about 2^16 tries, and k such stages make 2^k names, all with the same
 * hash. Linear hashing never separates equal hashes, so a table that
 * refused an insert into a full page could be made to fail every spawn. This
 * test builds such sets and checks the table grows instead:
 *
 *   1. 3 x 4096 names with ONE 32-bit hash: every intern is TA_OK, find and
 *      re-intern answer the same pointers, release in any order empties the
 *      table, and every free matches its allocation (strict allocator).
 *   2. 4300 names that share only the low 12 bits of the hash (so they stay
 *      in one page through 4096 pages' worth of splits): the same.
 *   3. Allocation failures: every allocation the first intern makes, and the
 *      two the 4097th equal-hash name makes (the record, the doubled page),
 *      refused once: TA_ERR_NOMEM, nothing leaked, nothing interned, and
 *      the same intern works afterwards.
 *   4. Identity and counting: equal addresses are one pointer, references
 *      count, ta_address_in tells tables apart, and detached copies are
 *      ignored by retain and release.
 *   5. Destroy frees records still held, and a table never used.
 *   6. 200,000 honest names: no block larger than one page, all found.
 *
 * Includes the amalgamation, because the table is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_allocator.h"

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

static ta_test_allocator strict_heap = { .strict = true };
static ta_test_allocator plain_heap = { 0 };

#define NAME_BYTES 64u
#define FAMILY "fam"

typedef struct name {
    char text[NAME_BYTES];
    uint32_t length;
} name;

static ta_actor_address address_of(const name *entry) {
    return (ta_actor_address){ .family = { .name = { .ptr = FAMILY, .len = 3u } },
                               .name = { .ptr = entry->text, .len = entry->length } };
}

/* The state the address hash is in when it starts on a name: after the family and the NUL separator. */
static uint32_t name_state(void) {
    const char separator = 0;
    uint32_t state = ta_fnv1a32(TA_FNV1A32_BASIS, FAMILY, 3u);
    return ta_fnv1a32(state, &separator, 1);
}

/* ------------------------------------------------------------------------
 * A set of names with one hash: stages of colliding 4-byte blocks.
 */

enum { STAGES = 14, BLOCK = 4 };

static void block_text(uint32_t number, char *out) {
    for (uint32_t digit = 0; digit < BLOCK; digit += 1u) {
        uint32_t value = number % 36u;
        out[digit] = (char)(value < 10u ? '0' + value : 'a' + (value - 10u));
        number /= 36u;
    }
}

/* Finds two different blocks that lead `state` to the same state, by a birthday search. */
static uint32_t find_collision(uint32_t state, char *first, char *second) {
    enum { SIZE = 1 << 19 };
    uint32_t *keys = calloc(SIZE, sizeof(uint32_t));
    uint32_t *numbers = calloc(SIZE, sizeof(uint32_t));
    uint32_t result = 0;
    for (uint32_t number = 1; number < 1679616u && result == 0; number += 1u) { /* 36^4 blocks */
        char block[BLOCK];
        block_text(number, block);
        uint32_t reached = ta_fnv1a32(state, block, BLOCK);
        uint32_t slot = (reached * 0x9E3779B1u) >> 13;
        while (numbers[slot] != 0 && keys[slot] != reached) {
            slot = (slot + 1u) & (SIZE - 1u);
        }
        if (numbers[slot] != 0) {
            block_text(numbers[slot], first);
            block_text(number, second);
            result = reached;
        } else {
            keys[slot] = reached;
            numbers[slot] = number;
        }
    }
    free(keys);
    free(numbers);
    return result;
}

/* name `mask` (STAGES bits) of the set: block `bit` of each stage; all have the same 32-bit hash. */
static char stage_blocks[STAGES][2][BLOCK];
static bool stages_ready = false;

static void prepare_stages(void) {
    if (stages_ready) {
        return;
    }
    uint32_t state = name_state();
    for (uint32_t stage = 0; stage < STAGES; stage += 1u) {
        state = find_collision(state, stage_blocks[stage][0], stage_blocks[stage][1]);
        CHECK(state != 0);
    }
    stages_ready = true;
}

static void equal_hash_name(uint32_t mask, name *out) {
    for (uint32_t stage = 0; stage < STAGES; stage += 1u) {
        memcpy(out->text + stage * BLOCK, stage_blocks[stage][(mask >> stage) & 1u], BLOCK);
    }
    out->length = STAGES * BLOCK;
}

static uint32_t hash_of(const name *entry) {
    ta_actor_address address = address_of(entry);
    return ta_address_hash(&address);
}

/* ------------------------------------------------------------------------
 * Helpers over a table
 */

static bool table_is_empty(const ta_address_table *table) {
    return ta_table_count(&table->index) == 0 && table->records.used == 0;
}

/* Releases every address of `names` once, in a scrambled order (7919 is prime). */
static void release_scrambled(ta_address **addresses, uint32_t count) {
    for (uint32_t step = 0; step < count; step += 1u) {
        ta_address_release(addresses[(uint32_t)(((uint64_t)step * 7919u) % count)]);
    }
}

/*
 * Interns every name of the set, checks that find answers the same pointers (every `stride`th name:
 * equal hashes make each lookup a walk over the set), releases them all in a scrambled order, and
 * checks the table is empty and everything came back. The strict allocator checks every free's size
 * but looks each pointer up in a list of the live blocks, so a big set uses the plain one.
 */
static void check_set(const name *names, uint32_t count, uint32_t stride, ta_test_allocator *heap) {
    ta_address_table table;
    ta_address_table_init(&table, ta_test_allocator_get(heap));
    ta_address **addresses = calloc(count, sizeof(*addresses));
    for (uint32_t index = 0; index < count; index += 1u) {
        ta_actor_address address = address_of(&names[index]);
        CHECK(ta_address_table_intern(&table, &address, &addresses[index]) == TA_OK);
    }
    CHECK(ta_table_count(&table.index) == count && table.records.used == count);
    for (uint32_t index = 0; index < count; index += stride) {
        ta_actor_address address = address_of(&names[index]);
        ta_address *again = NULL;
        CHECK(ta_address_table_intern(&table, &address, &again) == TA_OK);
        CHECK(again == addresses[index] && again->references == 2u);
        ta_address_release(again);
        CHECK(ta_address_in(addresses[index], &table));
    }
    CHECK(ta_table_count(&table.index) == count);
    release_scrambled(addresses, count);
    CHECK(table_is_empty(&table));
    /* Re-interning after the table emptied works, and the pages stay: no new page was needed. */
    ta_test_allocator_reset_counts(heap);
    ta_address *reborn = NULL;
    ta_actor_address first = address_of(&names[0]);
    CHECK(ta_address_table_intern(&table, &first, &reborn) == TA_OK);
    CHECK(heap->stats.allocations == 1u); /* only the record: the pool and the index have room */
    ta_address_release(reborn);
    free(addresses);
    ta_address_table_destroy(&table);
    CHECK(heap->stats.bad_frees == 0 && ta_test_allocator_finish(heap) == 0);
}

/*
 * 1. More than two pages' worth of names with one 32-bit hash: the 4097th
 * doubles the page and the 8193rd doubles it again (so three pages' worth
 * of slots are in play). The cost is quadratic by nature, which is what
 * the attacker pays too, so the set is no larger than that. A smaller set
 * runs on the strict allocator as well.
 */
static void test_equal_hash_names(void) {
    prepare_stages();
    enum { COUNT = 2 * 4096 + 1, SMALL = 600 };
    name *names = calloc(COUNT, sizeof(*names));
    for (uint32_t mask = 0; mask < COUNT; mask += 1u) {
        equal_hash_name(mask, &names[mask]);
        CHECK(hash_of(&names[mask]) == hash_of(&names[0]));
    }
    check_set(names, COUNT, 97u, &plain_heap);
    check_set(names, SMALL, 1u, &strict_heap);
    free(names);
}

/* 2. Names sharing the low 12 bits of the hash but not all 32: they stay in one page through every split. */
static void test_shared_low_bits(void) {
    enum { COUNT = 4300 };
    name *names = calloc(COUNT, sizeof(*names));
    uint32_t found = 0;
    uint32_t state = name_state();
    for (uint32_t number = 0; found < COUNT; number += 1u) {
        /* "k" and the decimal digits, hashed by hand: tens of millions of tries must be cheap. */
        char text[16];
        uint32_t length = 0;
        uint32_t digits = number;
        char reversed[12];
        uint32_t count = 0;
        do {
            reversed[count++] = (char)('0' + digits % 10u);
            digits /= 10u;
        } while (digits != 0);
        text[length++] = 'k';
        while (count != 0) {
            text[length++] = reversed[--count];
        }
        if ((ta_fnv1a32(state, text, length) & 0xfffu) == 0x5a3u) {
            memcpy(names[found].text, text, length);
            names[found].length = length;
            found += 1u;
        }
    }
    check_set(names, COUNT, 53u, &plain_heap);
    check_set(names, 700u, 1u, &strict_heap);
    free(names);
}

/* 3. Refusing each allocation of an intern: nothing leaked, nothing interned, the retry works. */
static void test_allocation_failures(void) {
    name one = { .length = 3u };
    memcpy(one.text, "abc", 3);
    ta_actor_address address = address_of(&one);
    ta_address_table table;
    ta_address_table_init(&table, ta_test_allocator_get(&strict_heap));
    ta_test_allocator_reset_counts(&strict_heap);
    ta_address *interned = NULL;
    CHECK(ta_address_table_intern(&table, &address, &interned) == TA_OK);
    uint64_t first_intern = strict_heap.stats.allocations; /* record, pool directory and page, index directory and page */
    CHECK(first_intern == 5u);
    ta_address_release(interned);
    ta_address_table_destroy(&table);
    CHECK(ta_test_allocator_finish(&strict_heap) == 0);
    for (uint64_t nth = 1; nth <= first_intern; nth += 1u) {
        ta_address_table_init(&table, ta_test_allocator_get(&strict_heap));
        ta_test_allocator_reset_counts(&strict_heap);
        ta_test_allocator_fail_nth(&strict_heap, nth);
        interned = NULL;
        CHECK(ta_address_table_intern(&table, &address, &interned) == TA_ERR_NOMEM && interned == NULL);
        ta_test_allocator_disarm(&strict_heap);
        CHECK(table_is_empty(&table));
        CHECK(ta_address_table_intern(&table, &address, &interned) == TA_OK);
        CHECK(interned != NULL && interned->references == 1u && ta_address_in(interned, &table));
        ta_address_release(interned);
        ta_address_table_destroy(&table);
        CHECK(strict_heap.stats.bad_frees == 0 && ta_test_allocator_finish(&strict_heap) == 0);
    }

    /*
     * The 4097th name with one hash must double a full page. Refuse each
     * allocation of that intern in turn (the record, the pool's next page,
     * a grow of the index that may only be postponed, the doubled page):
     * either NOMEM with nothing interned, or the intern succeeds because the
     * refused grow was only a postponement; and the retry always works.
     */
    prepare_stages();
    enum { FULL = 4096 };
    uint64_t total = 0;
    uint32_t refused = 0;
    for (uint64_t nth = 0; nth <= total; nth += 1u) {
        ta_address_table_init(&table, ta_test_allocator_get(&strict_heap));
        ta_address **held = calloc(FULL, sizeof(*held));
        for (uint32_t mask = 0; mask < FULL; mask += 1u) {
            name entry;
            equal_hash_name(mask, &entry);
            ta_actor_address held_address = address_of(&entry);
            CHECK(ta_address_table_intern(&table, &held_address, &held[mask]) == TA_OK);
        }
        name extra;
        equal_hash_name(FULL, &extra);
        ta_actor_address extra_address = address_of(&extra);
        ta_test_allocator_reset_counts(&strict_heap);
        if (nth != 0) {
            ta_test_allocator_fail_nth(&strict_heap, nth);
        }
        uint64_t bytes_before = ta_table_bytes(&table.index);
        ta_address *added = NULL;
        ta_result result = ta_address_table_intern(&table, &extra_address, &added);
        ta_test_allocator_disarm(&strict_heap);
        if (nth == 0) {
            total = strict_heap.stats.allocations;
            CHECK(result == TA_OK && total >= 3u && ta_table_count(&table.index) == FULL + 1u);
            CHECK(ta_table_bytes(&table.index) > bytes_before); /* a page was doubled */
            ta_address_release(added);
        } else if (result == TA_ERR_NOMEM) {
            refused += 1u;
            CHECK(added == NULL && ta_table_count(&table.index) == FULL && table.records.used == FULL);
            CHECK(ta_address_table_intern(&table, &extra_address, &added) == TA_OK); /* the retry works */
            CHECK(ta_table_count(&table.index) == FULL + 1u);
            ta_address_release(added);
        } else {
            CHECK(result == TA_OK && ta_table_count(&table.index) == FULL + 1u);
            ta_address_release(added);
        }
        release_scrambled(held, FULL);
        free(held);
        CHECK(table_is_empty(&table));
        ta_address_table_destroy(&table);
        CHECK(strict_heap.stats.bad_frees == 0 && ta_test_allocator_finish(&strict_heap) == 0);
    }
    CHECK(refused >= 3u);
}

/* 4. Pointer identity and reference counting; detached copies and other tables. */
static void test_identity_and_counting(void) {
    ta_address_table table;
    ta_address_table other;
    ta_address_table_init(&table, ta_test_allocator_get(&strict_heap));
    ta_address_table_init(&other, ta_test_allocator_get(&strict_heap));
    name one = { .length = 5u };
    memcpy(one.text, "alpha", 5);
    ta_actor_address address = address_of(&one);
    ta_address *first = NULL;
    ta_address *second = NULL;
    ta_address *elsewhere = NULL;
    CHECK(ta_address_table_intern(&table, &address, &first) == TA_OK);
    CHECK(ta_address_table_intern(&table, &address, &second) == TA_OK);
    CHECK(first == second && first->references == 2u);
    CHECK(ta_address_table_intern(&other, &address, &elsewhere) == TA_OK && elsewhere != first);
    CHECK(ta_address_in(first, &table) && !ta_address_in(first, &other) && !ta_address_in(NULL, &table));
    /* The view and the text are the ones interned, with their NULs. */
    ta_actor_address view = ta_address_view(first);
    CHECK(view.name.len == 5u && memcmp(view.name.ptr, "alpha", 5) == 0 && view.family.name.len == 3u);
    CHECK(first->text[3] == 0 && first->text[3 + 1 + 5] == 0);
    /* A different family is a different address, whatever the name. */
    ta_address *named_otherwise = NULL;
    ta_actor_address changed = address;
    changed.family.name.len = 2u;
    CHECK(ta_address_table_intern(&table, &changed, &named_otherwise) == TA_OK && named_otherwise != first);
    ta_address_release(named_otherwise);
    /* A detached copy is not counted, not in the table, and retain and release do nothing on it. */
    ta_arena arena;
    CHECK(ta_arena_init(&arena, ta_test_allocator_get(&strict_heap), 256) == TA_OK);
    ta_address *copy = NULL;
    CHECK(ta_address_detach(&arena, first, &copy) == TA_OK && copy != first);
    CHECK(copy->table == NULL && copy->index == 0 && copy->unloaded == 0 && copy->references == 0);
    CHECK(!ta_address_in(copy, &table) && ta_address_bytes(copy) == ta_address_bytes(first));
    ta_address_retain(copy);
    ta_address_release(copy);
    ta_address_release(copy);
    CHECK(copy->references == 0 && first->references == 2u && ta_table_count(&table.index) == 1u);
    ta_arena_destroy(&arena);
    ta_address_retain(first);
    CHECK(first->references == 3u);
    ta_address_release(first);
    ta_address_release(first);
    CHECK(first->references == 1u);
    ta_address_release(first);
    CHECK(table_is_empty(&table));
    ta_address_release(elsewhere);
    CHECK(table_is_empty(&other));
    ta_address_table_destroy(&table);
    ta_address_table_destroy(&other);
    CHECK(strict_heap.stats.bad_frees == 0 && ta_test_allocator_finish(&strict_heap) == 0);
}

/* 5. Destroy frees records still held, and a table that was never used (or never got its first page). */
static void test_destroy(void) {
    ta_address_table table;
    ta_address_table_init(&table, ta_test_allocator_get(&strict_heap));
    ta_address_table_destroy(&table); /* never used: nothing to free */
    ta_address_table_destroy(&table); /* idempotent on a zeroed table */
    ta_address_table_init(&table, ta_test_allocator_get(&strict_heap));
    ta_address *found = NULL;
    name entry = { .length = 1u };
    for (uint32_t index = 0; index < 100u; index += 1u) {
        entry.text[0] = (char)('A' + index % 26u);
        entry.text[1] = (char)('a' + index / 26u);
        entry.length = 2u;
        ta_actor_address address = address_of(&entry);
        CHECK(ta_address_table_intern(&table, &address, &found) == TA_OK); /* never released */
    }
    ta_address_table_destroy(&table);
    CHECK(strict_heap.stats.bad_frees == 0 && ta_test_allocator_finish(&strict_heap) == 0);
}

/* 6. 200,000 honest names: the largest block is one page, and every name is found. */
static void test_honest_population(void) {
    enum { COUNT = 200000 };
    ta_address_table table;
    ta_address_table_init(&table, ta_test_allocator_get(&plain_heap));
    ta_address **addresses = calloc(COUNT, sizeof(*addresses));
    ta_test_allocator_reset_counts(&plain_heap);
    char text[24];
    for (uint32_t index = 0; index < COUNT; index += 1u) {
        int length = snprintf(text, sizeof text, "actor_%u", index);
        ta_actor_address address = { .family = { .name = { .ptr = FAMILY, .len = 3u } },
                                     .name = { .ptr = text, .len = (uint32_t)length } };
        CHECK(ta_address_table_intern(&table, &address, &addresses[index]) == TA_OK);
    }
    /* A page of 4096 pointers or 4096 eight byte slots: 32 KiB; the records are far smaller. */
    CHECK(plain_heap.stats.largest_block <= 4096u * sizeof(ta_table_slot));
    uint32_t found = 0;
    for (uint32_t index = 0; index < COUNT; index += 1u) {
        int length = snprintf(text, sizeof text, "actor_%u", index);
        ta_actor_address address = { .family = { .name = { .ptr = FAMILY, .len = 3u } },
                                     .name = { .ptr = text, .len = (uint32_t)length } };
        ta_address *again = NULL;
        CHECK(ta_address_table_intern(&table, &address, &again) == TA_OK);
        found += again == addresses[index] ? 1u : 0u;
        ta_address_release(again);
    }
    CHECK(found == COUNT);
    release_scrambled(addresses, COUNT);
    CHECK(table_is_empty(&table));
    free(addresses);
    ta_address_table_destroy(&table);
    CHECK(plain_heap.stats.live_blocks == 0);
}

int main(void) {
    test_equal_hash_names();
    test_shared_low_bits();
    test_allocation_failures();
    test_identity_and_counting();
    test_destroy();
    test_honest_population();
    if (failures != 0) {
        fprintf(stderr, "address_table_test: %d checks failed\n", failures);
        return 1;
    }
    printf("address_table_test: ok\n");
    return 0;
}
