/* test: standalone asan */
/*
 * Block B2 (src/index_heap.h): the indexed min-heap.
 *
 * Covers the contract of each function (init validation, ordering with ties,
 * the heap_index bookkeeping, remove of every position, the invalid inputs),
 * the allocation rules (init and insert never allocate; reserve is the only
 * allocating call, geometric, clamped to the ceiling, all or nothing and
 * swept over every allocation it makes; sized frees match under a strict
 * allocator), pointer stability across pool refills, and a short seeded model
 * test against a scan of the members. It is fast, not exhaustive: twenty
 * thousand steps over at most 64 items.
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

static ta_test_allocator heap_mem = { .strict = true };

/* A 24-byte item shaped like an envelope's deadline fields, `next` being the pool's free-list link. */
typedef struct {
    uint64_t due;
    uint64_t order;
    uint32_t heap_index;
    uint32_t next;
} item;

#define PAGE_ITEMS 8u

typedef struct {
    ta_pool pool;
    ta_index_heap heap;
    uint64_t sequence;
} fixture;

static ta_index_heap_params heap_params(fixture *f, uint32_t max_items) {
    return (ta_index_heap_params){ .pool = &f->pool,
                                   .due_offset = offsetof(item, due),
                                   .order_offset = offsetof(item, order),
                                   .heap_index_offset = offsetof(item, heap_index),
                                   .max_items = max_items };
}

static void fixture_init(fixture *f, uint32_t max_items) {
    ta_mem_zero(f, sizeof *f);
    ta_pool_params params = { .item_size = sizeof(item),
                              .item_alignment = _Alignof(item),
                              .link_offset = offsetof(item, next),
                              .page_items = PAGE_ITEMS };
    CHECK(ta_pool_init(&f->pool, ta_test_allocator_get(&heap_mem), &params) == TA_OK);
    ta_index_heap_params hp = heap_params(f, max_items);
    CHECK(ta_index_heap_init(&f->heap, ta_test_allocator_get(&heap_mem), &hp) == TA_OK);
}

static void fixture_destroy(fixture *f) {
    ta_index_heap_destroy(&f->heap);
    ta_pool_destroy(&f->pool);
}

/* Takes an item (refilling the pool at this "safe point" when it is full) and sets its key. Does not insert. */
static uint32_t make(fixture *f, uint64_t due) {
    uint32_t index = ta_pool_take(&f->pool);
    if (index == 0) {
        CHECK(ta_pool_refill(&f->pool, 1) == TA_OK);
        index = ta_pool_take(&f->pool);
    }
    item *it = ta_pool_at(&f->pool, index);
    it->due = due;
    it->order = f->sequence;
    f->sequence += 1u;
    it->heap_index = 0;
    return index;
}

static item *at(fixture *f, uint32_t index) {
    return ta_pool_at(&f->pool, index);
}

static bool item_before(fixture *f, uint32_t a, uint32_t b) {
    return at(f, a)->due != at(f, b)->due ? at(f, a)->due < at(f, b)->due : at(f, a)->order < at(f, b)->order;
}

/* Checks every invariant of the header from outside the functions. */
static void verify(fixture *f) {
    ta_index_heap *h = &f->heap;
    CHECK(h->count <= h->capacity && h->capacity <= h->max_capacity);
    for (uint32_t position = 1; position <= h->count; position += 1u) {
        uint32_t index = h->array[position - 1u];
        CHECK(index != 0 && at(f, index) != NULL);
        CHECK(at(f, index)->heap_index == position);
        if (position > 1u) {
            CHECK(!item_before(f, index, h->array[position / 2u - 1u]));
        }
    }
    uint32_t in_heap = 0;
    for (uint32_t index = 1; index <= ta_pool_extent(&f->pool); index += 1u) {
        if (at(f, index)->heap_index != 0) {
            in_heap += 1u;
        }
    }
    CHECK(in_heap == h->count);
}

static void test_init_validation(void) {
    fixture f;
    fixture_init(&f, 0);
    ta_test_allocator_reset_counts(&heap_mem);
    ta_index_heap h;
    ta_index_heap_params good = heap_params(&f, 0);
    CHECK(ta_index_heap_init(&h, ta_test_allocator_get(&heap_mem), &good) == TA_OK);
    CHECK(heap_mem.stats.attempts == 0);
    CHECK(h.max_capacity == UINT32_MAX && h.count == 0 && h.capacity == 0);
    ta_index_heap_destroy(&h);

    ta_index_heap_params bad = good;
    bad.pool = NULL;
    CHECK(ta_index_heap_init(&h, ta_test_allocator_get(&heap_mem), &bad) == TA_ERR_INVALID);
    bad = good;
    bad.due_offset = 4; /* misaligned */
    CHECK(ta_index_heap_init(&h, ta_test_allocator_get(&heap_mem), &bad) == TA_ERR_INVALID);
    bad = good;
    bad.order_offset = bad.due_offset; /* overlap */
    CHECK(ta_index_heap_init(&h, ta_test_allocator_get(&heap_mem), &bad) == TA_ERR_INVALID);
    bad = good;
    bad.heap_index_offset = 2; /* misaligned and overlapping */
    CHECK(ta_index_heap_init(&h, ta_test_allocator_get(&heap_mem), &bad) == TA_ERR_INVALID);
    bad = good;
    bad.heap_index_offset = offsetof(item, order) + 4u; /* overlaps the second half of order */
    CHECK(ta_index_heap_init(&h, ta_test_allocator_get(&heap_mem), &bad) == TA_ERR_INVALID);
    {
        const unsigned char *bytes = (const unsigned char *)&h;
        bool zero = true;
        for (size_t i = 0; i < sizeof h; i += 1u) {
            zero = zero && bytes[i] == 0;
        }
        CHECK(zero);
    }
    CHECK(ta_index_heap_init(&h, ta_test_allocator_get(&heap_mem), NULL) == TA_ERR_INVALID);
    CHECK(heap_mem.stats.attempts == 0);
    fixture_destroy(&f);
}

static void test_ordering_and_ties(void) {
    fixture f;
    fixture_init(&f, 0);
    CHECK(ta_index_heap_reserve(&f.heap, 40) == TA_OK);
    CHECK(ta_index_heap_peek_min(&f.heap) == 0 && ta_index_heap_pop_min(&f.heap) == 0);

    /* Equal due: pops in insertion (order) sequence, inserted in a scrambled sequence. */
    uint32_t ties[9];
    for (int i = 0; i < 9; i += 1) {
        ties[i] = make(&f, 50);
    }
    static const int scramble[9] = { 4, 8, 0, 6, 2, 7, 1, 5, 3 };
    for (int i = 0; i < 9; i += 1) {
        CHECK(at(&f, ties[scramble[i]])->heap_index == 0);
        CHECK(ta_index_heap_insert(&f.heap, ties[scramble[i]]) == TA_OK);
        verify(&f);
    }
    for (int i = 0; i < 9; i += 1) {
        CHECK(ta_index_heap_peek_min(&f.heap) == ties[i]);
        CHECK(ta_index_heap_pop_min(&f.heap) == ties[i]);
        CHECK(at(&f, ties[i])->heap_index == 0);
        verify(&f);
    }
    CHECK(ta_index_heap_count(&f.heap) == 0);

    /* Distinct due, with the extremes; a later send with an earlier due comes first. */
    uint32_t late = make(&f, UINT64_MAX);
    uint32_t zero = make(&f, 0);
    uint32_t mid = make(&f, 7);
    uint32_t mid_again = make(&f, 7);
    CHECK(ta_index_heap_insert(&f.heap, late) == TA_OK);
    CHECK(ta_index_heap_insert(&f.heap, mid_again) == TA_OK);
    CHECK(ta_index_heap_insert(&f.heap, mid) == TA_OK);
    CHECK(ta_index_heap_insert(&f.heap, zero) == TA_OK);
    verify(&f);
    CHECK(ta_index_heap_pop_min(&f.heap) == zero);
    CHECK(ta_index_heap_pop_min(&f.heap) == mid);
    CHECK(ta_index_heap_pop_min(&f.heap) == mid_again);
    CHECK(ta_index_heap_pop_min(&f.heap) == late);

    /* Ascending, descending and all-equal runs. */
    for (int pattern = 0; pattern < 3; pattern += 1) {
        uint32_t items[24];
        for (uint64_t i = 0; i < 24; i += 1) {
            uint64_t due = pattern == 0 ? i : (pattern == 1 ? 23 - i : 9);
            items[i] = make(&f, due);
            CHECK(ta_index_heap_insert(&f.heap, items[i]) == TA_OK);
        }
        verify(&f);
        uint32_t previous = 0;
        for (int i = 0; i < 24; i += 1) {
            uint32_t popped = ta_index_heap_pop_min(&f.heap);
            CHECK(previous == 0 || item_before(&f, previous, popped));
            previous = popped;
        }
        for (int i = 0; i < 24; i += 1) {
            ta_pool_give(&f.pool, items[i]);
        }
    }
    fixture_destroy(&f);
}

static void test_invalid_inputs(void) {
    fixture f;
    fixture_init(&f, 0);
    CHECK(ta_index_heap_reserve(&f.heap, 4) == TA_OK);
    uint32_t a = make(&f, 1);
    uint32_t b = make(&f, 2);
    CHECK(ta_index_heap_insert(&f.heap, a) == TA_OK);
    CHECK(ta_index_heap_insert(&f.heap, a) == TA_ERR_INVALID); /* already in the heap */
    CHECK(ta_index_heap_insert(&f.heap, 0) == TA_ERR_INVALID);
    CHECK(ta_index_heap_insert(&f.heap, ta_pool_extent(&f.pool) + 1u) == TA_ERR_INVALID);
    CHECK(ta_index_heap_count(&f.heap) == 1 && at(&f, a)->heap_index == 1);
    verify(&f);
    CHECK(!ta_index_heap_remove(&f.heap, b)); /* never inserted */
    CHECK(!ta_index_heap_remove(&f.heap, 0));
    CHECK(!ta_index_heap_remove(&f.heap, ta_pool_extent(&f.pool) + 1u));
    CHECK(ta_index_heap_remove(&f.heap, a));
    CHECK(!ta_index_heap_remove(&f.heap, a)); /* double remove */
    CHECK(ta_index_heap_count(&f.heap) == 0);
    verify(&f);
    fixture_destroy(&f);
}

/* For n = 1..40, removes the item at every position p and checks the rest pops in order. */
static void test_remove_every_position(void) {
    for (uint32_t n = 1; n <= 40; n += 1u) {
        for (uint32_t p = 1; p <= n; p += 1u) {
            fixture f;
            fixture_init(&f, 0);
            CHECK(ta_index_heap_reserve(&f.heap, n) == TA_OK);
            /* Dues that interleave subtrees, so the moved-up last entry sometimes sifts up. */
            for (uint32_t i = 0; i < n; i += 1u) {
                uint32_t index = make(&f, (uint64_t)((i * 7u) % n) * 3u + (i % 3u));
                CHECK(ta_index_heap_insert(&f.heap, index) == TA_OK);
            }
            verify(&f);
            uint32_t victim = f.heap.array[p - 1u];
            CHECK(ta_index_heap_remove(&f.heap, victim));
            CHECK(at(&f, victim)->heap_index == 0);
            CHECK(ta_index_heap_count(&f.heap) == n - 1u);
            verify(&f);
            uint32_t previous = 0;
            for (uint32_t i = 0; i + 1u < n; i += 1u) {
                uint32_t popped = ta_index_heap_pop_min(&f.heap);
                CHECK(popped != 0 && popped != victim);
                CHECK(previous == 0 || item_before(&f, previous, popped));
                previous = popped;
            }
            CHECK(ta_index_heap_pop_min(&f.heap) == 0);
            fixture_destroy(&f);
        }
    }
}

/* A case where the moved last entry must sift up: two subtrees, last sits in the cheaper one. */
static void test_remove_sifts_up(void) {
    fixture f;
    fixture_init(&f, 0);
    CHECK(ta_index_heap_reserve(&f.heap, 8) == TA_OK);
    /* Positions: 1:1 2:100 3:2 4:101 5:102 6:3 7:4 (then 8 inserted below). Removing 4 (the 101 child) moves 4. */
    static const uint64_t dues[7] = { 1, 100, 2, 101, 102, 3, 4 };
    uint32_t ids[7];
    for (int i = 0; i < 7; i += 1) {
        ids[i] = make(&f, dues[i]);
        CHECK(ta_index_heap_insert(&f.heap, ids[i]) == TA_OK);
    }
    verify(&f);
    uint32_t victim = ids[3];
    CHECK(ta_index_heap_remove(&f.heap, victim));
    verify(&f);
    uint32_t order[6];
    for (int i = 0; i < 6; i += 1) {
        order[i] = ta_index_heap_pop_min(&f.heap);
    }
    for (int i = 1; i < 6; i += 1) {
        CHECK(item_before(&f, order[i - 1], order[i]));
    }
    fixture_destroy(&f);
}

static void test_reserve_basics_and_ceiling(void) {
    fixture f;
    fixture_init(&f, 100);
    ta_test_allocator_reset_counts(&heap_mem);
    CHECK(ta_index_heap_reserve(&f.heap, 0) == TA_OK && heap_mem.stats.attempts == 0);
    CHECK(ta_index_heap_reserve(&f.heap, 1) == TA_OK);
    CHECK(ta_index_heap_capacity(&f.heap) == 16u && heap_mem.stats.allocations == 1u);
    CHECK(ta_index_heap_reserve(&f.heap, 16) == TA_OK && heap_mem.stats.attempts == 1u); /* fits: no allocation */
    CHECK(ta_index_heap_reserve(&f.heap, 17) == TA_OK && ta_index_heap_capacity(&f.heap) == 32u);
    CHECK(heap_mem.stats.allocations == 2u && heap_mem.stats.frees == 1u);
    CHECK(ta_index_heap_reserve(&f.heap, 70) == TA_OK && ta_index_heap_capacity(&f.heap) == 100u); /* clamped */
    CHECK(f.heap.array_bytes == 100u * sizeof(uint32_t));
    CHECK(heap_mem.stats.live_blocks == 1u); /* the array; the pool has no pages yet */
    uint64_t attempts = heap_mem.stats.attempts;
    CHECK(ta_index_heap_reserve(&f.heap, 101) == TA_ERR_NOMEM);
    CHECK(ta_index_heap_reserve(&f.heap, UINT32_MAX) == TA_ERR_NOMEM);
    CHECK(heap_mem.stats.attempts == attempts && ta_index_heap_capacity(&f.heap) == 100u);
    fixture_destroy(&f);
    CHECK(heap_mem.stats.live_blocks == 0 && heap_mem.stats.live_bytes == 0);
}

static void test_reserve_sweep(void) {
    for (int from_full = 0; from_full < 2; from_full += 1) {
        fixture f;
        fixture_init(&f, 0);
        uint32_t held[16];
        if (from_full) {
            CHECK(ta_index_heap_reserve(&f.heap, 16) == TA_OK);
            for (int i = 0; i < 16; i += 1) {
                held[i] = make(&f, (uint64_t)(16 - i));
                CHECK(ta_index_heap_insert(&f.heap, held[i]) == TA_OK);
            }
        }
        uint32_t want = from_full ? 17u : 5u;
        ta_test_allocator_reset_counts(&heap_mem);
        /* Measure: one allocation, plus one free of the old array when growing. */
        CHECK(ta_index_heap_reserve(&f.heap, want) == TA_OK);
        uint64_t attempts = heap_mem.stats.attempts;
        CHECK(attempts == 1u && heap_mem.stats.frees == (from_full ? 1u : 0u));
        fixture_destroy(&f);

        for (uint64_t k = 1; k <= attempts; k += 1u) {
            fixture_init(&f, 0);
            if (from_full) {
                CHECK(ta_index_heap_reserve(&f.heap, 16) == TA_OK);
                for (int i = 0; i < 16; i += 1) {
                    held[i] = make(&f, (uint64_t)(16 - i));
                    CHECK(ta_index_heap_insert(&f.heap, held[i]) == TA_OK);
                }
            }
            uint32_t *array = f.heap.array;
            uint32_t capacity = f.heap.capacity;
            size_t live = heap_mem.stats.live_blocks;
            ta_test_allocator_fail_nth(&heap_mem, k);
            CHECK(ta_index_heap_reserve(&f.heap, want) == TA_ERR_NOMEM);
            ta_test_allocator_disarm(&heap_mem);
            CHECK(f.heap.array == array && f.heap.capacity == capacity);
            CHECK(heap_mem.stats.live_blocks == live);
            verify(&f);
            if (from_full) {
                CHECK(ta_index_heap_count(&f.heap) == 16u && ta_index_heap_peek_min(&f.heap) == held[15]);
            }
            ta_test_allocator_refuse_all(&heap_mem, true);
            CHECK(ta_index_heap_reserve(&f.heap, want) == TA_ERR_NOMEM);
            ta_test_allocator_refuse_all(&heap_mem, false);
            CHECK(heap_mem.stats.live_blocks == live);
            ta_test_allocator_refuse_from(&heap_mem, 1);
            CHECK(ta_index_heap_reserve(&f.heap, want) == TA_ERR_NOMEM);
            ta_test_allocator_refuse_from(&heap_mem, 0);
            CHECK(ta_index_heap_reserve(&f.heap, want) == TA_OK); /* and it works once memory is back */
            verify(&f);
            fixture_destroy(&f);
            CHECK(heap_mem.stats.live_blocks == 0);
        }
    }
}

static void test_insert_never_allocates(void) {
    fixture f;
    fixture_init(&f, 0);
    CHECK(ta_index_heap_reserve(&f.heap, 16) == TA_OK);
    uint32_t items[17];
    for (int i = 0; i < 17; i += 1) {
        items[i] = make(&f, (uint64_t)((i * 5) % 17));
    }
    ta_test_allocator_reset_counts(&heap_mem);
    ta_test_allocator_refuse_all(&heap_mem, true);
    for (int i = 0; i < 16; i += 1) {
        CHECK(ta_index_heap_insert(&f.heap, items[i]) == TA_OK);
    }
    CHECK(ta_index_heap_insert(&f.heap, items[16]) == TA_ERR_NOMEM); /* not reserved: a checked answer */
    CHECK(at(&f, items[16])->heap_index == 0 && ta_index_heap_count(&f.heap) == 16u);
    (void)ta_index_heap_peek_min(&f.heap);
    CHECK(ta_index_heap_remove(&f.heap, items[3]));
    CHECK(ta_index_heap_pop_min(&f.heap) != 0);
    ta_test_allocator_refuse_all(&heap_mem, false);
    CHECK(heap_mem.stats.allocations == 0 && heap_mem.stats.frees == 0);
    CHECK(heap_mem.stats.attempts == 0);
    verify(&f);
    fixture_destroy(&f);
}

static void test_growth_is_geometric(void) {
    fixture f;
    fixture_init(&f, 0);
    CHECK(ta_pool_refill(&f.pool, 40) == TA_OK); /* so the pool's pages are not counted below */
    ta_test_allocator_reset_counts(&heap_mem);
    for (uint32_t n = 1; n <= 1000u; n += 1u) {
        CHECK(ta_index_heap_reserve(&f.heap, n) == TA_OK);
        CHECK(ta_index_heap_capacity(&f.heap) >= n);
        if (n <= 40u) {
            uint32_t index = make(&f, 1000u - n);
            CHECK(ta_index_heap_insert(&f.heap, index) == TA_OK);
            verify(&f); /* contents survive each regrow */
        }
    }
    /* 16, 32, 64, ..., 1024 is seven steps */
    CHECK(heap_mem.stats.allocations <= 7u);
    fixture_destroy(&f);
}

static void test_destroy(void) {
    fixture f;
    fixture_init(&f, 0);
    CHECK(heap_mem.stats.live_blocks == 0);
    ta_index_heap_destroy(&f.heap); /* never reserved */
    ta_index_heap_destroy(&f.heap); /* twice */
    ta_index_heap_destroy(NULL);
    fixture_init(&f, 0);
    CHECK(ta_index_heap_reserve(&f.heap, 10) == TA_OK);
    CHECK(ta_index_heap_insert(&f.heap, make(&f, 3)) == TA_OK);
    CHECK(heap_mem.stats.live_blocks == 3u); /* the array, one pool page and the pool's directory */
    ta_index_heap_destroy(&f.heap);
    ta_index_heap_destroy(&f.heap);
    ta_pool_destroy(&f.pool);
    CHECK(heap_mem.stats.live_blocks == 0 && heap_mem.stats.live_bytes == 0);
    CHECK(heap_mem.stats.bad_frees == 0);
}

/* Holds item pointers across pool refills and heap reserves; keys and positions stay right. */
static void test_pointer_stability(void) {
    fixture f;
    fixture_init(&f, 0);
    uint32_t first = make(&f, 500);
    item *held = at(&f, first);
    CHECK(ta_index_heap_reserve(&f.heap, 1) == TA_OK);
    CHECK(ta_index_heap_insert(&f.heap, first) == TA_OK);
    for (uint32_t i = 0; i < 100; i += 1u) {
        uint32_t index = make(&f, 1000u + i); /* refills the pool as it goes */
        CHECK(ta_index_heap_reserve(&f.heap, ta_index_heap_count(&f.heap) + 1u) == TA_OK);
        CHECK(ta_index_heap_insert(&f.heap, index) == TA_OK);
        CHECK(at(&f, first) == held && held->due == 500 && held->heap_index == 1u);
    }
    verify(&f);
    CHECK(ta_index_heap_pop_min(&f.heap) == first && held->heap_index == 0);
    fixture_destroy(&f);
}

static uint64_t next_random(uint64_t *state) {
    *state += 0x9E3779B97F4A7C15ull;
    uint64_t z = *state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

#define MODEL_ITEMS 64u

/* Compares the heap with a scan of the member set after every step. */
static void test_model(void) {
    fixture f;
    fixture_init(&f, 0);
    CHECK(ta_index_heap_reserve(&f.heap, MODEL_ITEMS) == TA_OK);
    uint32_t ids[MODEL_ITEMS];
    for (uint32_t i = 0; i < MODEL_ITEMS; i += 1u) {
        ids[i] = make(&f, 0);
    }
    bool member[MODEL_ITEMS] = { false };
    uint32_t members = 0;
    uint64_t state = 12345;
    for (int step = 0; step < 20000; step += 1) {
        uint32_t slot = (uint32_t)(next_random(&state) % MODEL_ITEMS);
        uint64_t choice = next_random(&state) % 8u;
        if (choice < 4u) {
            if (!member[slot]) {
                at(&f, ids[slot])->due = next_random(&state) % 16u; /* few distinct dues: many ties */
                at(&f, ids[slot])->order = f.sequence;
                f.sequence += 1u;
                CHECK(ta_index_heap_insert(&f.heap, ids[slot]) == TA_OK);
                member[slot] = true;
                members += 1u;
            } else {
                CHECK(ta_index_heap_insert(&f.heap, ids[slot]) == TA_ERR_INVALID);
            }
        } else if (choice < 6u) {
            uint32_t popped = ta_index_heap_pop_min(&f.heap);
            if (members == 0) {
                CHECK(popped == 0);
            } else {
                uint32_t best = 0;
                for (uint32_t i = 0; i < MODEL_ITEMS; i += 1u) {
                    if (member[i] && (best == 0 || item_before(&f, ids[i], best))) {
                        best = ids[i];
                    }
                }
                CHECK(popped == best);
                for (uint32_t i = 0; i < MODEL_ITEMS; i += 1u) {
                    if (ids[i] == popped) {
                        member[i] = false;
                    }
                }
                members -= 1u;
            }
        } else {
            bool removed = ta_index_heap_remove(&f.heap, ids[slot]);
            CHECK(removed == member[slot]);
            if (member[slot]) {
                member[slot] = false;
                members -= 1u;
            }
        }
        CHECK(ta_index_heap_count(&f.heap) == members);
        uint32_t best = 0;
        for (uint32_t i = 0; i < MODEL_ITEMS; i += 1u) {
            if (member[i] && (best == 0 || item_before(&f, ids[i], best))) {
                best = ids[i];
            }
        }
        CHECK(ta_index_heap_peek_min(&f.heap) == best);
        if (step % 16 == 0) {
            verify(&f);
        }
    }
    verify(&f);
    fixture_destroy(&f);
}

int main(void) {
    test_init_validation();
    test_ordering_and_ties();
    test_invalid_inputs();
    test_remove_every_position();
    test_remove_sifts_up();
    test_reserve_basics_and_ceiling();
    test_reserve_sweep();
    test_insert_never_allocates();
    test_growth_is_geometric();
    test_destroy();
    test_pointer_stability();
    test_model();
    CHECK(heap_mem.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap_mem) == 0);
    if (failures != 0) {
        fprintf(stderr, "index_heap_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("index_heap_test: ok");
    return 0;
}
