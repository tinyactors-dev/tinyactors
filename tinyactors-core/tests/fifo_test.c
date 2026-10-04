/* test: standalone asan */
/*
 * Block B4 (src/fifo.h): the fixed-entry FIFO.
 *
 * Covers init validation and its failure sweep, the allocation rules (one
 * allocation at init and none after, a matching sized free, provided storage
 * never freed or written outside), wraparound at every offset and fill for
 * several capacities and entry widths, empty and full, peek without pop and
 * the taken head of ADR 0069, clear, and a short seeded model test against a
 * plain array queue. Fast, not exhaustive: about twenty thousand random steps.
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

#define MAX_ENTRY 48u
#define CANARY 0xA5u

/* An entry of `size` bytes whose every byte derives from `id`, so a copy of the wrong width shows. */
static void fill(unsigned char *entry, size_t size, uint32_t id) {
    for (size_t i = 0; i < size; i += 1) {
        entry[i] = (unsigned char)(id * 31u + i * 7u + 1u);
    }
}

static bool matches(const unsigned char *entry, size_t size, uint32_t id) {
    unsigned char expect[MAX_ENTRY];
    fill(expect, size, id);
    for (size_t i = 0; i < size; i += 1) {
        if (entry[i] != expect[i]) return false;
    }
    return true;
}

static ta_fifo_params owned_params(size_t size, size_t alignment, uint32_t capacity) {
    return (ta_fifo_params){ .allocator = ta_test_allocator_get(&heap),
                             .storage = NULL,
                             .entry_size = size,
                             .entry_alignment = alignment,
                             .capacity = capacity };
}

static void finish_clean(void) {
    CHECK(heap.stats.bad_frees == 0);
    CHECK(heap.stats.live_blocks == 0 && heap.stats.live_bytes == 0);
}

static bool is_zero(const ta_fifo *fifo) {
    const unsigned char *bytes = (const unsigned char *)fifo;
    for (size_t i = 0; i < sizeof *fifo; i += 1) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

static void test_init_validation(void) {
    ta_fifo fifo;
    ta_fifo_params params = owned_params(16, 8, 4);
    CHECK(ta_fifo_init(NULL, &params) == TA_ERR_INVALID);
    CHECK(ta_fifo_init(&fifo, NULL) == TA_ERR_INVALID && is_zero(&fifo));
    params = owned_params(0, 8, 4);
    CHECK(ta_fifo_init(&fifo, &params) == TA_ERR_INVALID && is_zero(&fifo));
    params = owned_params(16, 3, 4);
    CHECK(ta_fifo_init(&fifo, &params) == TA_ERR_INVALID && is_zero(&fifo));
    params = owned_params(16, 0, 4);
    CHECK(ta_fifo_init(&fifo, &params) == TA_ERR_INVALID);
    params = owned_params(12, 8, 4); /* not a multiple of the alignment */
    CHECK(ta_fifo_init(&fifo, &params) == TA_ERR_INVALID);
    _Alignas(16) unsigned char storage[64];
    params = owned_params(16, 16, 4);
    params.storage = storage + 8; /* misaligned */
    CHECK(ta_fifo_init(&fifo, &params) == TA_ERR_INVALID && is_zero(&fifo));
    params = owned_params(SIZE_MAX / 2 + 1u, 1, 4); /* capacity * entry_size overflows */
    CHECK(ta_fifo_init(&fifo, &params) == TA_ERR_NOMEM && is_zero(&fifo));
    CHECK(heap.stats.attempts == 0);

    /* Capacity 0 is valid, allocates nothing, and every operation answers cleanly. */
    params = owned_params(16, 8, 0);
    CHECK(ta_fifo_init(&fifo, &params) == TA_OK && heap.stats.attempts == 0);
    unsigned char entry[16] = { 0 };
    CHECK(!ta_fifo_push(&fifo, entry) && !ta_fifo_pop(&fifo, entry) && ta_fifo_peek(&fifo, 0) == NULL);
    CHECK(ta_fifo_capacity(&fifo) == 0 && ta_fifo_count(&fifo) == 0);
    ta_fifo_clear(&fifo);
    ta_fifo_destroy(&fifo);
    finish_clean();
}

static void test_allocation_rules(void) {
    ta_fifo fifo;
    ta_test_allocator_reset_counts(&heap);
    ta_fifo_params params = owned_params(48, 16, 5);
    CHECK(ta_fifo_init(&fifo, &params) == TA_OK);
    CHECK(heap.stats.allocations == 1u && heap.stats.live_bytes == 5u * 48u);
    ta_test_allocator_refuse_all(&heap, true);
    unsigned char entry[48];
    for (uint32_t round = 0; round < 40u; round += 1) {
        fill(entry, sizeof entry, round);
        CHECK(ta_fifo_push(&fifo, entry));
        CHECK(ta_fifo_peek(&fifo, 0) != NULL);
        CHECK(ta_fifo_pop(&fifo, NULL));
        if (round % 7u == 0) ta_fifo_clear(&fifo);
    }
    ta_test_allocator_disarm(&heap);
    CHECK(heap.stats.attempts == 1u && heap.stats.refused == 0);
    ta_fifo_destroy(&fifo);
    CHECK(is_zero(&fifo) && heap.stats.frees == 1u);
    finish_clean();
    ta_fifo_destroy(&fifo); /* idempotent on a zeroed fifo */
    CHECK(heap.stats.frees == 1u);

    /* Provided storage: never allocated, never freed, never written outside. */
    ta_test_allocator_reset_counts(&heap);
    _Alignas(16) unsigned char block[16 + 3 * 16 + 16];
    for (size_t i = 0; i < sizeof block; i += 1) block[i] = CANARY;
    params = owned_params(16, 16, 3);
    params.storage = block + 16;
    CHECK(ta_fifo_init(&fifo, &params) == TA_OK);
    for (uint32_t id = 0; id < 20u; id += 1) {
        unsigned char small[16];
        fill(small, sizeof small, id);
        if (!ta_fifo_push(&fifo, small)) ta_fifo_pop(&fifo, NULL);
    }
    ta_fifo_destroy(&fifo);
    CHECK(heap.stats.attempts == 0 && heap.stats.frees == 0);
    for (size_t i = 0; i < 16; i += 1) CHECK(block[i] == CANARY && block[sizeof block - 1 - i] == CANARY);
    finish_clean();
}

static void test_init_failure_sweep(void) {
    for (uint64_t nth = 1; nth <= 2; nth += 1) {
        ta_fifo fifo;
        ta_test_allocator_reset_counts(&heap);
        ta_fifo_params params = owned_params(16, 8, 4);
        ta_test_allocator_fail_nth(&heap, nth);
        ta_result result = ta_fifo_init(&fifo, &params);
        ta_test_allocator_disarm(&heap);
        if (nth == 1) {
            CHECK(result == TA_ERR_NOMEM && is_zero(&fifo));
        } else {
            CHECK(result == TA_OK); /* init makes one allocation; the second never happens */
            ta_fifo_destroy(&fifo);
        }
        finish_clean();
    }
}

/* Wraparound at every offset and fill, entries wider than a word, canaries around provided storage. */
static void test_wraparound(void) {
    static const uint32_t capacities[] = { 1, 2, 3, 4, 5, 8 };
    static const struct {
        size_t size, alignment;
    } shapes[] = { { 8, 8 }, { 16, 8 }, { 48, 8 }, { 16, 16 }, { 48, 16 } };
    for (size_t s = 0; s < TA_ARRAY_COUNT(shapes); s += 1) {
        for (size_t c = 0; c < TA_ARRAY_COUNT(capacities); c += 1) {
            uint32_t capacity = capacities[c];
            size_t size = shapes[s].size;
            for (uint32_t offset = 0; offset < capacity; offset += 1) {
                for (uint32_t fillcount = 0; fillcount <= capacity; fillcount += 1) {
                    _Alignas(16) unsigned char block[16 + 8 * 48 + 16];
                    for (size_t i = 0; i < sizeof block; i += 1) block[i] = CANARY;
                    ta_fifo fifo;
                    ta_fifo_params params = owned_params(size, shapes[s].alignment, capacity);
                    params.storage = block + 16;
                    CHECK(ta_fifo_init(&fifo, &params) == TA_OK);
                    unsigned char entry[MAX_ENTRY], out[MAX_ENTRY];
                    uint32_t next = 1000;
                    for (uint32_t i = 0; i < offset; i += 1) { /* walk the head to `offset` */
                        fill(entry, size, next);
                        CHECK(ta_fifo_push(&fifo, entry) && ta_fifo_pop(&fifo, out) && matches(out, size, next));
                        next += 1u;
                    }
                    uint32_t first = next;
                    for (uint32_t i = 0; i < fillcount; i += 1) {
                        fill(entry, size, next);
                        CHECK(ta_fifo_push(&fifo, entry));
                        next += 1u;
                    }
                    CHECK(ta_fifo_count(&fifo) == fillcount && ta_fifo_capacity(&fifo) == capacity);
                    for (uint32_t i = 0; i < fillcount; i += 1) {
                        CHECK(matches(ta_fifo_peek(&fifo, i), size, first + i));
                    }
                    CHECK(ta_fifo_peek(&fifo, fillcount) == NULL);
                    for (uint32_t i = 0; i < fillcount; i += 1) {
                        CHECK(ta_fifo_pop(&fifo, out) && matches(out, size, first + i));
                    }
                    CHECK(ta_fifo_count(&fifo) == 0 && !ta_fifo_pop(&fifo, out));
                    ta_fifo_destroy(&fifo);
                    for (size_t i = 0; i < 16; i += 1) CHECK(block[i] == CANARY && block[sizeof block - 1 - i] == CANARY);
                    for (size_t i = 16 + (size_t)capacity * size; i < 16 + 8 * 48; i += 1) CHECK(block[i] == CANARY);
                }
            }
        }
    }
}

static void test_empty_and_full(void) {
    ta_fifo fifo;
    ta_fifo_params params = owned_params(16, 8, 3);
    CHECK(ta_fifo_init(&fifo, &params) == TA_OK);
    unsigned char entry[16], out[16];
    for (size_t i = 0; i < sizeof out; i += 1) out[i] = 0x77;
    CHECK(!ta_fifo_pop(&fifo, out));
    for (size_t i = 0; i < sizeof out; i += 1) CHECK(out[i] == 0x77);
    for (uint32_t id = 1; id <= 3; id += 1) {
        fill(entry, sizeof entry, id);
        CHECK(ta_fifo_push(&fifo, entry));
    }
    fill(entry, sizeof entry, 99);
    for (int attempt = 0; attempt < 3; attempt += 1) {
        CHECK(!ta_fifo_push(&fifo, entry) && ta_fifo_count(&fifo) == 3u);
        CHECK(matches(ta_fifo_peek(&fifo, 0), 16, 1) && matches(ta_fifo_peek(&fifo, 2), 16, 3));
    }
    CHECK(ta_fifo_pop(&fifo, out) && matches(out, 16, 1));
    CHECK(ta_fifo_push(&fifo, entry) && !ta_fifo_push(&fifo, entry)); /* lands at the wrapped slot */
    CHECK(matches(ta_fifo_peek(&fifo, 2), 16, 99) && matches(ta_fifo_peek(&fifo, 0), 16, 2));
    ta_fifo_destroy(&fifo);
    finish_clean();
}

/* The taken head of ADR 0069: it stays queued, valid across pushes, until the next take pops it. */
static void test_peek_and_taken_head(void) {
    ta_fifo fifo;
    ta_fifo_params params = owned_params(16, 8, 3);
    CHECK(ta_fifo_init(&fifo, &params) == TA_OK);
    unsigned char entry[16];
    fill(entry, 16, 1);
    CHECK(ta_fifo_push(&fifo, entry));
    unsigned char *taken = ta_fifo_peek(&fifo, 0);
    CHECK(taken == ta_fifo_peek(&fifo, 0) && ta_fifo_count(&fifo) == 1u);
    for (uint32_t id = 2; id <= 3; id += 1) {
        fill(entry, 16, id);
        CHECK(ta_fifo_push(&fifo, entry));
        CHECK(ta_fifo_peek(&fifo, 0) == taken && matches(taken, 16, 1));
    }
    uint32_t waiting = ta_fifo_count(&fifo) - 1u; /* the owner's arithmetic */
    CHECK(waiting == 2u);
    CHECK(ta_fifo_pop(&fifo, NULL));                 /* next take drops the old head... */
    taken = ta_fifo_peek(&fifo, 0);                  /* ...and takes the new one */
    CHECK(taken != NULL && matches(taken, 16, 2));
    fill(entry, 16, 4);
    CHECK(ta_fifo_push(&fifo, entry)); /* wraps; the taken entry does not move */
    CHECK(ta_fifo_peek(&fifo, 0) == taken && matches(taken, 16, 2));
    ta_fifo_destroy(&fifo);
    finish_clean();
}

static void test_clear(void) {
    for (uint32_t offset = 0; offset < 4; offset += 1) {
        ta_fifo fifo;
        ta_test_allocator_reset_counts(&heap);
        ta_fifo_params params = owned_params(16, 8, 4);
        CHECK(ta_fifo_init(&fifo, &params) == TA_OK);
        unsigned char entry[16], out[16];
        for (uint32_t i = 0; i < offset; i += 1) {
            fill(entry, 16, i);
            CHECK(ta_fifo_push(&fifo, entry) && ta_fifo_pop(&fifo, out));
        }
        fill(entry, 16, 50);
        CHECK(ta_fifo_push(&fifo, entry) && ta_fifo_push(&fifo, entry));
        ta_fifo_clear(&fifo);
        CHECK(ta_fifo_count(&fifo) == 0 && fifo.head == 0 && ta_fifo_peek(&fifo, 0) == NULL);
        for (uint32_t i = 0; i < 4; i += 1) {
            fill(entry, 16, i);
            CHECK(ta_fifo_push(&fifo, entry));
        }
        CHECK(!ta_fifo_push(&fifo, entry) && matches(ta_fifo_peek(&fifo, 0), 16, 0));
        CHECK(heap.stats.allocations == 1u && heap.stats.frees == 0);
        ta_fifo_destroy(&fifo);
        CHECK(heap.stats.frees == 1u);
        finish_clean();
    }
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_model(void) {
    for (uint32_t capacity = 1; capacity <= 9; capacity += 1) {
        ta_fifo fifo;
        ta_fifo_params params = owned_params(24, 8, capacity);
        CHECK(ta_fifo_init(&fifo, &params) == TA_OK);
        uint32_t model[16];
        uint32_t count = 0, next = 1;
        for (int step = 0; step < 2200 && failures == 0; step += 1) {
            unsigned op = (unsigned)(rng() % 20u);
            unsigned char entry[24], out[24];
            if (op < 9) {
                fill(entry, 24, next);
                bool ok = ta_fifo_push(&fifo, entry);
                CHECK(ok == (count < capacity));
                if (ok) model[count++] = next++;
            } else if (op < 17) {
                bool ok = ta_fifo_pop(&fifo, out);
                CHECK(ok == (count > 0));
                if (ok) {
                    CHECK(matches(out, 24, model[0]));
                    for (uint32_t i = 1; i < count; i += 1) model[i - 1] = model[i];
                    count -= 1u;
                }
            } else if (op < 19) {
                uint32_t index = (uint32_t)(rng() % (capacity + 2u));
                unsigned char *peeked = ta_fifo_peek(&fifo, index);
                CHECK((peeked != NULL) == (index < count));
                if (peeked != NULL) CHECK(matches(peeked, 24, model[index]));
            } else {
                ta_fifo_clear(&fifo);
                count = 0;
            }
            CHECK(ta_fifo_count(&fifo) == count && ta_fifo_capacity(&fifo) == capacity);
        }
        ta_fifo_destroy(&fifo);
        finish_clean();
    }
}

int main(void) {
    test_init_validation();
    test_allocation_rules();
    test_init_failure_sweep();
    test_wraparound();
    test_empty_and_full();
    test_peek_and_taken_head();
    test_clear();
    test_model();
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "fifo_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("fifo_test: ok");
    return 0;
}
