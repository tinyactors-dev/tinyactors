/*
 * ta_fifo: the first-in, first-out queue of fixed-size entries in one
 * circular array (src/fifo.h) that holds the internal queue.
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Memory comes from the test
 * allocator (test_allocator.h), which can fail after a number of
 * allocations and records the size of every request.
 *
 * Claims proven here, in order:
 *
 * 1. init refuses an entry size of 0, an alignment of 0 or not a power of
 *    two, and an entry size that is not a multiple of the alignment, with
 *    TA_ERR_INVALID, leaving the queue untouched; a valid init allocates
 *    nothing and gives an empty queue of capacity 0, on which push
 *    answers TA_ERR_FULL, pop TA_ERR_NOT_FOUND, peek and at NULL, and
 *    destroy frees nothing;
 * 2. reserve allocates one array of exactly the asked entries at the
 *    entry alignment and frees the old one, and a reserve to 0 or within
 *    the capacity changes nothing and allocates nothing;
 * 3. entries leave in the order they came in; with one entry the head is
 *    that entry; peek answers the head and leaves it in place until the
 *    next pop, and a pop copies the head out, or discards it when out is
 *    NULL;
 * 4. the queue wraps around the end of its array: at(i) answers the
 *    entry i places behind the head at slot (head + i) mod capacity, and
 *    a push moves no entry, so a peeked address stays the head's;
 * 5. a push into a full queue answers TA_ERR_FULL and changes nothing:
 *    the same head, count and array bytes; after one pop the next push
 *    fits, at the slot the pop freed; a queue of capacity 1 fills and
 *    empties in turn;
 * 6. a pop of an empty queue answers TA_ERR_NOT_FOUND and changes
 *    nothing, and at answers NULL for every place at or behind the count;
 * 7. reserve of a wrapped queue copies its entries in order to the start
 *    of the new array, and pushes then use the new room;
 * 8. a reserve the allocator refuses, or whose size does not fit in
 *    size_t, answers TA_ERR_NOMEM and changes nothing: the same array,
 *    capacity, head, entries, and no block leaked; reserve at the largest
 *    capacity, 2^32 - 1 entries, asks for that many and, refused, changes
 *    nothing;
 * 9. clear empties the queue, keeps its array and capacity, and touches
 *    no entry; the queue then fills from its first slot;
 * 10. push, pop, peek, at, clear, count and capacity never allocate;
 * 11. destroy frees the array and leaves an empty queue of capacity 0,
 *     which a later reserve sets up again;
 * 12. a run of 4000 random pushes, pops, peeks, clears and reserves
 *     agrees with a plain array model at every step.
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


/* Entries of 16 bytes aligned to 8. */
typedef struct {
    uint64_t key;
    uint32_t value;
    uint32_t spare;
} entry;

static ta_fifo_params entry_params(test_allocator *heap) {
    return (ta_fifo_params){
        .allocator = test_allocator_of(heap),
        .entry_size = sizeof(entry),
        .entry_alignment = _Alignof(entry),
    };
}

static entry make(uint32_t n) { return (entry){ .key = 1000u + n, .value = n * 3u + 1u, .spare = n }; }

static bool is(const void *address, uint32_t n) {
    if (address == NULL) {
        return false;
    }
    entry value;
    memcpy(&value, address, sizeof(value));
    return value.key == 1000u + n && value.value == n * 3u + 1u && value.spare == n;
}

static ta_result push(ta_fifo *fifo, uint32_t n) {
    entry value = make(n);
    return ta_fifo_push(fifo, &value);
}

static bool pops(ta_fifo *fifo, uint32_t n) {
    entry value;
    memset(&value, 0, sizeof(value));
    return ta_fifo_pop(fifo, &value) == TA_OK && is(&value, n);
}

static ta_fifo new_fifo(test_allocator *heap, uint32_t capacity) {
    ta_fifo fifo;
    ta_fifo_params params = entry_params(heap);
    CHECK(ta_fifo_init(&fifo, &params) == TA_OK);
    CHECK(ta_fifo_reserve(&fifo, capacity) == TA_OK);
    return fifo;
}

/* A copy of the queue and its array, to show that a call changed nothing. */
typedef struct {
    ta_fifo fifo;
    unsigned char bytes[64 * sizeof(entry)];
} snapshot;

static void take_snapshot(const ta_fifo *fifo, snapshot *shot) {
    memcpy(&shot->fifo, fifo, sizeof(*fifo));
    size_t size = (size_t)fifo->capacity * fifo->entry_size;
    if (size > sizeof(shot->bytes)) {
        size = sizeof(shot->bytes);
    }
    if (size != 0) {
        memcpy(shot->bytes, fifo->entries, size);
    }
}

static bool unchanged(const ta_fifo *fifo, const snapshot *shot) {
    size_t size = (size_t)fifo->capacity * fifo->entry_size;
    if (size > sizeof(shot->bytes)) {
        size = sizeof(shot->bytes);
    }
    return memcmp(fifo, &shot->fifo, sizeof(*fifo)) == 0 &&
           (size == 0 || memcmp(fifo->entries, shot->bytes, size) == 0);
}

/* 1 */
static void test_init(void) {
    test_allocator heap = {0};
    ta_fifo_params good = entry_params(&heap);
    ta_fifo_params bad[] = { good, good, good, good };
    bad[0].entry_size = 0;
    bad[1].entry_alignment = 0;
    bad[2].entry_alignment = 12;
    bad[3].entry_size = 12; /* not a multiple of 8 */
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(bad); index += 1u) {
        ta_fifo fifo;
        memset(&fifo, 0x5A, sizeof(fifo));
        ta_fifo before;
        memcpy(&before, &fifo, sizeof(fifo));
        CHECK(ta_fifo_init(&fifo, &bad[index]) == TA_ERR_INVALID);
        CHECK(memcmp(&fifo, &before, sizeof(fifo)) == 0);
    }

    ta_fifo fifo;
    CHECK(ta_fifo_init(&fifo, &good) == TA_OK);
    CHECK(heap.attempts == 0);
    CHECK(ta_fifo_capacity(&fifo) == 0 && ta_fifo_count(&fifo) == 0);
    CHECK(fifo.entries == NULL);
    CHECK(push(&fifo, 1) == TA_ERR_FULL);
    CHECK(ta_fifo_pop(&fifo, NULL) == TA_ERR_NOT_FOUND);
    CHECK(ta_fifo_peek(&fifo) == NULL && ta_fifo_at(&fifo, 0) == NULL);
    CHECK(ta_fifo_count(&fifo) == 0 && fifo.entries == NULL);
    ta_fifo_destroy(&fifo);
    CHECK(heap.attempts == 0 && heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 2 */
static void test_reserve(void) {
    test_allocator heap = {0};
    ta_fifo fifo;
    ta_fifo_params params = entry_params(&heap);
    CHECK(ta_fifo_init(&fifo, &params) == TA_OK);

    CHECK(ta_fifo_reserve(&fifo, 0) == TA_OK);
    CHECK(heap.attempts == 0 && ta_fifo_capacity(&fifo) == 0);

    CHECK(ta_fifo_reserve(&fifo, 5) == TA_OK);
    CHECK(ta_fifo_capacity(&fifo) == 5 && heap.live == 1 && heap.attempts == 1);
    CHECK(heap.last_size == 5 * sizeof(entry) && heap.last_alignment == _Alignof(entry));
    CHECK((uintptr_t)fifo.entries % _Alignof(entry) == 0);

    CHECK(ta_fifo_reserve(&fifo, 5) == TA_OK);
    CHECK(ta_fifo_reserve(&fifo, 3) == TA_OK);
    CHECK(ta_fifo_reserve(&fifo, 0) == TA_OK);
    CHECK(heap.attempts == 1 && ta_fifo_capacity(&fifo) == 5);

    unsigned char *old = fifo.entries;
    CHECK(ta_fifo_reserve(&fifo, 9) == TA_OK);
    CHECK(ta_fifo_capacity(&fifo) == 9 && heap.live == 1 && heap.attempts == 2);
    CHECK(heap.last_size == 9 * sizeof(entry));
    CHECK(fifo.entries != old);

    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 3 */
static void test_order(void) {
    test_allocator heap = {0};
    ta_fifo fifo = new_fifo(&heap, 8);

    /* One entry. */
    CHECK(push(&fifo, 7) == TA_OK);
    CHECK(ta_fifo_count(&fifo) == 1);
    CHECK(is(ta_fifo_peek(&fifo), 7) && ta_fifo_peek(&fifo) == ta_fifo_at(&fifo, 0));
    CHECK(ta_fifo_at(&fifo, 1) == NULL);
    CHECK(pops(&fifo, 7));
    CHECK(ta_fifo_count(&fifo) == 0 && ta_fifo_peek(&fifo) == NULL);

    for (uint32_t n = 0; n < 6; n += 1u) {
        CHECK(push(&fifo, n) == TA_OK);
    }
    CHECK(ta_fifo_count(&fifo) == 6);

    /* The head stays while it is used, through pushes, until the pop. */
    void *head = ta_fifo_peek(&fifo);
    CHECK(is(head, 0));
    CHECK(push(&fifo, 6) == TA_OK);
    CHECK(ta_fifo_peek(&fifo) == head && is(head, 0));
    CHECK(ta_fifo_peek(&fifo) == ta_fifo_peek(&fifo));

    /* A pop with NULL discards the head. */
    CHECK(ta_fifo_pop(&fifo, NULL) == TA_OK);
    CHECK(is(ta_fifo_peek(&fifo), 1));
    for (uint32_t n = 1; n < 7; n += 1u) {
        CHECK(is(ta_fifo_peek(&fifo), n));
        CHECK(pops(&fifo, n));
    }
    CHECK(ta_fifo_count(&fifo) == 0);
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 4 */
static void test_wrap(void) {
    test_allocator heap = {0};
    ta_fifo fifo = new_fifo(&heap, 5);
    for (uint32_t n = 0; n < 4; n += 1u) {
        CHECK(push(&fifo, n) == TA_OK);
    }
    CHECK(pops(&fifo, 0) && pops(&fifo, 1) && pops(&fifo, 2));
    CHECK(fifo.head == 3);
    /* 3 at slot 3; 4 at slot 4; 5, 6, 7 wrap to slots 0, 1, 2. */
    void *head = ta_fifo_peek(&fifo);
    for (uint32_t n = 4; n < 8; n += 1u) {
        CHECK(push(&fifo, n) == TA_OK);
        CHECK(ta_fifo_peek(&fifo) == head);
    }
    CHECK(ta_fifo_count(&fifo) == 5);
    for (uint32_t place = 0; place < 5; place += 1u) {
        uint32_t slot = (3u + place) % 5u;
        CHECK(ta_fifo_at(&fifo, place) == fifo.entries + slot * sizeof(entry));
        CHECK(is(ta_fifo_at(&fifo, place), 3u + place));
    }
    CHECK(ta_fifo_at(&fifo, 5) == NULL);
    /* The head reaches the last slot and wraps to the first. */
    CHECK(pops(&fifo, 3) && pops(&fifo, 4));
    CHECK(fifo.head == 0 && is(ta_fifo_peek(&fifo), 5));
    for (uint32_t n = 5; n < 8; n += 1u) {
        CHECK(pops(&fifo, n));
    }
    CHECK(ta_fifo_count(&fifo) == 0);
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 5 */
static void test_full(void) {
    test_allocator heap = {0};
    ta_fifo fifo = new_fifo(&heap, 4);
    for (uint32_t n = 0; n < 4; n += 1u) {
        CHECK(push(&fifo, n) == TA_OK);
    }
    snapshot shot;
    take_snapshot(&fifo, &shot);
    CHECK(push(&fifo, 99) == TA_ERR_FULL);
    CHECK(push(&fifo, 99) == TA_ERR_FULL);
    CHECK(unchanged(&fifo, &shot));

    /* One pop frees the head's slot, and the next push takes it. */
    CHECK(pops(&fifo, 0));
    CHECK(push(&fifo, 4) == TA_OK);
    CHECK(ta_fifo_at(&fifo, 3) == fifo.entries);
    take_snapshot(&fifo, &shot);
    CHECK(push(&fifo, 99) == TA_ERR_FULL);
    CHECK(unchanged(&fifo, &shot));
    for (uint32_t n = 1; n < 5; n += 1u) {
        CHECK(pops(&fifo, n));
    }
    ta_fifo_destroy(&fifo);

    /* Capacity 1: full after one push, empty after one pop. */
    fifo = new_fifo(&heap, 1);
    for (uint32_t n = 0; n < 5; n += 1u) {
        CHECK(push(&fifo, n) == TA_OK);
        CHECK(push(&fifo, 99) == TA_ERR_FULL);
        CHECK(is(ta_fifo_peek(&fifo), n));
        CHECK(pops(&fifo, n));
        CHECK(ta_fifo_pop(&fifo, NULL) == TA_ERR_NOT_FOUND);
    }
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 6 */
static void test_empty(void) {
    test_allocator heap = {0};
    ta_fifo fifo = new_fifo(&heap, 3);
    snapshot shot;
    take_snapshot(&fifo, &shot);
    entry out = make(42);
    CHECK(ta_fifo_pop(&fifo, &out) == TA_ERR_NOT_FOUND);
    CHECK(ta_fifo_pop(&fifo, NULL) == TA_ERR_NOT_FOUND);
    CHECK(is(&out, 42));
    CHECK(unchanged(&fifo, &shot));
    CHECK(ta_fifo_peek(&fifo) == NULL);
    for (uint32_t place = 0; place < 5; place += 1u) {
        CHECK(ta_fifo_at(&fifo, place) == NULL);
    }
    CHECK(ta_fifo_at(&fifo, UINT32_MAX) == NULL);

    CHECK(push(&fifo, 1) == TA_OK && push(&fifo, 2) == TA_OK);
    CHECK(ta_fifo_at(&fifo, 2) == NULL && ta_fifo_at(&fifo, 3) == NULL);
    CHECK(ta_fifo_at(&fifo, UINT32_MAX) == NULL);
    CHECK(pops(&fifo, 1) && pops(&fifo, 2));
    take_snapshot(&fifo, &shot);
    CHECK(ta_fifo_pop(&fifo, NULL) == TA_ERR_NOT_FOUND);
    CHECK(unchanged(&fifo, &shot));
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 7 */
static void test_reserve_moves_in_order(void) {
    test_allocator heap = {0};
    ta_fifo fifo = new_fifo(&heap, 4);
    for (uint32_t n = 0; n < 4; n += 1u) {
        CHECK(push(&fifo, n) == TA_OK);
    }
    CHECK(pops(&fifo, 0) && pops(&fifo, 1) && pops(&fifo, 2));
    CHECK(push(&fifo, 4) == TA_OK && push(&fifo, 5) == TA_OK && push(&fifo, 6) == TA_OK);
    /* 3 at slot 3, then 4, 5, 6 at slots 0 to 2. */
    CHECK(fifo.head == 3 && ta_fifo_count(&fifo) == 4);

    CHECK(ta_fifo_reserve(&fifo, 7) == TA_OK);
    CHECK(fifo.head == 0 && ta_fifo_count(&fifo) == 4 && ta_fifo_capacity(&fifo) == 7);
    for (uint32_t place = 0; place < 4; place += 1u) {
        CHECK(ta_fifo_at(&fifo, place) == fifo.entries + place * sizeof(entry));
        CHECK(is(ta_fifo_at(&fifo, place), 3u + place));
    }
    for (uint32_t n = 7; n < 10; n += 1u) {
        CHECK(push(&fifo, n) == TA_OK);
    }
    CHECK(push(&fifo, 99) == TA_ERR_FULL);
    for (uint32_t n = 3; n < 10; n += 1u) {
        CHECK(pops(&fifo, n));
    }

    /* A queue whose entries do not wrap moves the same way. */
    CHECK(push(&fifo, 20) == TA_OK && push(&fifo, 21) == TA_OK);
    CHECK(fifo.head == 0);
    CHECK(pops(&fifo, 20));
    CHECK(fifo.head == 1);
    CHECK(ta_fifo_reserve(&fifo, 8) == TA_OK);
    CHECK(fifo.head == 0 && ta_fifo_count(&fifo) == 1 && is(ta_fifo_peek(&fifo), 21));

    /* So does an empty one. */
    CHECK(pops(&fifo, 21));
    CHECK(ta_fifo_reserve(&fifo, 9) == TA_OK);
    CHECK(fifo.head == 0 && ta_fifo_count(&fifo) == 0 && ta_fifo_capacity(&fifo) == 9);
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 8 */
static void test_failed_reserve(void) {
    test_allocator heap = {0};
    ta_fifo fifo = new_fifo(&heap, 4);
    for (uint32_t n = 0; n < 4; n += 1u) {
        CHECK(push(&fifo, n) == TA_OK);
    }
    CHECK(pops(&fifo, 0) && push(&fifo, 4) == TA_OK);

    snapshot shot;
    take_snapshot(&fifo, &shot);
    heap.policy.fail_from = heap.attempts + 1u;
    CHECK(ta_fifo_reserve(&fifo, 10) == TA_ERR_NOMEM);
    CHECK(unchanged(&fifo, &shot));
    CHECK(heap.live == 1);
    heap.policy.fail_from = 0;

    /* The largest capacity asks for its whole size, and is refused. */
    heap.policy.largest = 1024;
    uint32_t attempts = heap.attempts;
    CHECK(ta_fifo_reserve(&fifo, UINT32_MAX) == TA_ERR_NOMEM);
    CHECK(heap.attempts == attempts + 1u);
    CHECK(heap.last_size == (size_t)UINT32_MAX * sizeof(entry));
    CHECK(unchanged(&fifo, &shot));
    CHECK(heap.live == 1);
    heap.policy.largest = 0;

    for (uint32_t n = 1; n < 5; n += 1u) {
        CHECK(pops(&fifo, n));
    }
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);

    /* A size that does not fit in size_t is refused before the allocator
     * is asked. */
    ta_fifo_params params = {
        .allocator = test_allocator_of(&heap),
        .entry_size = SIZE_MAX / 2u + 1u,
        .entry_alignment = 1,
    };
    CHECK(ta_fifo_init(&fifo, &params) == TA_OK);
    take_snapshot(&fifo, &shot);
    attempts = heap.attempts;
    CHECK(ta_fifo_reserve(&fifo, 2) == TA_ERR_NOMEM);
    CHECK(heap.attempts == attempts);
    CHECK(unchanged(&fifo, &shot));
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 9 */
static void test_clear(void) {
    test_allocator heap = {0};
    ta_fifo fifo = new_fifo(&heap, 4);
    for (uint32_t n = 0; n < 4; n += 1u) {
        CHECK(push(&fifo, n) == TA_OK);
    }
    CHECK(pops(&fifo, 0) && push(&fifo, 4) == TA_OK);
    unsigned char *entries = fifo.entries;
    unsigned char bytes[4 * sizeof(entry)];
    memcpy(bytes, fifo.entries, sizeof(bytes));
    uint32_t attempts = heap.attempts;

    ta_fifo_clear(&fifo);
    CHECK(ta_fifo_count(&fifo) == 0 && ta_fifo_capacity(&fifo) == 4);
    CHECK(fifo.entries == entries && memcmp(fifo.entries, bytes, sizeof(bytes)) == 0);
    CHECK(ta_fifo_peek(&fifo) == NULL && ta_fifo_pop(&fifo, NULL) == TA_ERR_NOT_FOUND);
    CHECK(heap.attempts == attempts && heap.live == 1);

    for (uint32_t n = 10; n < 14; n += 1u) {
        CHECK(push(&fifo, n) == TA_OK);
    }
    CHECK(ta_fifo_peek(&fifo) == fifo.entries);
    CHECK(push(&fifo, 99) == TA_ERR_FULL);
    for (uint32_t n = 10; n < 14; n += 1u) {
        CHECK(pops(&fifo, n));
    }
    ta_fifo_clear(&fifo); /* an empty queue too */
    CHECK(ta_fifo_count(&fifo) == 0);
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 10 */
static void test_never_allocates(void) {
    test_allocator heap = {0};
    ta_fifo fifo = new_fifo(&heap, 3);
    uint32_t attempts = heap.attempts;
    heap.policy.fail_from = attempts + 1u;
    for (uint32_t round = 0; round < 10; round += 1u) {
        CHECK(push(&fifo, round) == TA_OK);
        CHECK(push(&fifo, round + 100u) == TA_OK);
        CHECK(push(&fifo, round + 200u) == TA_OK);
        CHECK(push(&fifo, 99) == TA_ERR_FULL);
        CHECK(ta_fifo_count(&fifo) == 3 && ta_fifo_capacity(&fifo) == 3);
        CHECK(is(ta_fifo_peek(&fifo), round) && is(ta_fifo_at(&fifo, 2), round + 200u));
        CHECK(pops(&fifo, round));
        CHECK(ta_fifo_pop(&fifo, NULL) == TA_OK);
        if (round % 2u == 0) {
            ta_fifo_clear(&fifo);
        } else {
            CHECK(pops(&fifo, round + 200u));
        }
        CHECK(ta_fifo_pop(&fifo, NULL) == TA_ERR_NOT_FOUND);
    }
    CHECK(heap.attempts == attempts);
    heap.policy.fail_from = 0;
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 11 */
static void test_destroy(void) {
    test_allocator heap = {0};
    ta_fifo fifo = new_fifo(&heap, 6);
    CHECK(push(&fifo, 1) == TA_OK && push(&fifo, 2) == TA_OK);
    CHECK(heap.live == 1);
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(fifo.entries == NULL && ta_fifo_capacity(&fifo) == 0 && ta_fifo_count(&fifo) == 0);
    CHECK(fifo.head == 0);
    CHECK(push(&fifo, 3) == TA_ERR_FULL && ta_fifo_peek(&fifo) == NULL);
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);

    CHECK(ta_fifo_reserve(&fifo, 2) == TA_OK);
    CHECK(heap.live == 1 && heap.last_size == 2 * sizeof(entry));
    CHECK(push(&fifo, 4) == TA_OK && pops(&fifo, 4));
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 12 */
static uint64_t random_state = 0x9E3779B97F4A7C15u;

static uint32_t random_below(uint32_t bound) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return (uint32_t)(random_state % bound);
}

static void test_random(void) {
    test_allocator heap = {0};
    ta_fifo fifo = new_fifo(&heap, 3);
    uint32_t model[64];
    uint32_t length = 0;
    uint32_t capacity = 3;
    uint32_t next = 0;
    for (uint32_t step = 0; step < 4000u; step += 1u) {
        uint32_t action = random_below(100);
        if (action < 45) {
            ta_result result = push(&fifo, next);
            if (length == capacity) {
                CHECK(result == TA_ERR_FULL);
            } else {
                CHECK(result == TA_OK);
                model[length++] = next;
            }
            next += 1u;
        } else if (action < 85) {
            if (length == 0) {
                CHECK(ta_fifo_pop(&fifo, NULL) == TA_ERR_NOT_FOUND);
            } else {
                CHECK(is(ta_fifo_peek(&fifo), model[0]));
                CHECK(pops(&fifo, model[0]));
                memmove(model, model + 1, (length - 1u) * sizeof(model[0]));
                length -= 1u;
            }
        } else if (action < 88) {
            ta_fifo_clear(&fifo);
            length = 0;
        } else if (action < 91 && capacity < 64) {
            uint32_t wanted = capacity + 1u + random_below(64u - capacity);
            CHECK(ta_fifo_reserve(&fifo, wanted) == TA_OK);
            capacity = wanted;
        } else {
            uint32_t place = random_below(capacity + 2u);
            if (place < length) {
                CHECK(is(ta_fifo_at(&fifo, place), model[place]));
            } else {
                CHECK(ta_fifo_at(&fifo, place) == NULL);
            }
        }
        CHECK(ta_fifo_count(&fifo) == length && ta_fifo_capacity(&fifo) == capacity);
        CHECK(fifo.head < (capacity == 0 ? 1u : capacity));
        CHECK(length != 0 || fifo.head == 0);
    }
    for (uint32_t place = 0; place < length; place += 1u) {
        CHECK(is(ta_fifo_at(&fifo, place), model[place]));
    }
    CHECK(heap.live == 1);
    ta_fifo_destroy(&fifo);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

int main(void) {
    test_init();
    test_reserve();
    test_order();
    test_wrap();
    test_full();
    test_empty();
    test_reserve_moves_in_order();
    test_failed_reserve();
    test_clear();
    test_never_allocates();
    test_destroy();
    test_random();
    if (failures != 0) {
        fprintf(stderr, "fifo_test: %d checks failed\n", failures);
        return 1;
    }
    printf("fifo_test: ok\n");
    return 0;
}
