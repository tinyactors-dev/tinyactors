/* The containers (ADR 0009): growth, wrapping, tombstones, reuse. */
#include "check.h"
#include <stdint.h>
#include <stdio.h>

#include "tinyactors_host/allocator.h"
#include "tinyactors_host/containers.h"
#include "allocator_private.h"

static tah_allocator allocator;

static ta_allocator other(void) {
    return tah_allocator_for(&allocator, TAH_MEMORY_OTHER);
}

static void vec_grows(void) {
    tah_vec vec;
    tah_vec_init(&vec, other(), TAH_LAYOUT(uint64_t));
    for (uint64_t i = 0; i < 1000; i += 1) {
        CHECK(tah_vec_push(&vec, &i) == TA_OK);
    }
    CHECK(vec.count == 1000);
    CHECK(vec.capacity >= 1000);
    for (uint32_t i = 0; i < 1000; i += 1) {
        CHECK(*(uint64_t *)tah_vec_at(&vec, i) == i);
    }
    tah_slice slice = tah_vec_slice(&vec);
    CHECK(slice.len == 1000 && ((const uint64_t *)slice.ptr)[999] == 999);
    tah_vec_destroy(&vec);
    CHECK(tah_allocator_total(&allocator).bytes_in_use == 0);
}

static void ring_keeps_fifo_order_across_chunks(void) {
    tah_ring ring;
    tah_ring_init(&ring, other(), (tah_ring_shape){ .layout = TAH_LAYOUT(uint32_t), .chunk_capacity = 4 });
    uint32_t next_in = 0;
    uint32_t next_out = 0;
    /* Uneven pushes and pops wrap through many chunks. */
    for (int round = 0; round < 200; round += 1) {
        for (int i = 0; i < 7; i += 1) {
            CHECK(tah_ring_push(&ring, &next_in) == TA_OK);
            next_in += 1;
        }
        for (int i = 0; i < 5; i += 1) {
            CHECK(*(uint32_t *)tah_ring_front(&ring) == next_out);
            uint32_t value = 0;
            CHECK(tah_ring_pop(&ring, &value));
            CHECK(value == next_out);
            next_out += 1;
        }
    }
    CHECK(tah_ring_count(&ring) == next_in - next_out);
    uint32_t value = 0;
    while (tah_ring_pop(&ring, &value)) {
        CHECK(value == next_out);
        next_out += 1;
    }
    CHECK(next_out == next_in);
    CHECK(tah_ring_front(&ring) == NULL);
    tah_ring_destroy(&ring);
    CHECK(tah_allocator_total(&allocator).bytes_in_use == 0);
}

static void ring_reuses_its_spare_chunk(void) {
    tah_ring ring;
    tah_ring_init(&ring, other(), (tah_ring_shape){ .layout = TAH_LAYOUT(uint32_t), .chunk_capacity = 8 });
    uint32_t value = 7;
    CHECK(tah_ring_push(&ring, &value) == TA_OK);
    CHECK(tah_ring_pop(&ring, &value));
    /* A queue that fills and drains within one chunk allocates nothing more. */
    uint64_t before = tah_allocator_calls(&allocator);
    for (int round = 0; round < 100; round += 1) {
        for (uint32_t i = 0; i < 8; i += 1) {
            CHECK(tah_ring_push(&ring, &i) == TA_OK);
        }
        while (tah_ring_pop(&ring, &value)) {
        }
    }
    CHECK(tah_allocator_calls(&allocator) == before);
    tah_ring_destroy(&ring);
    CHECK(tah_allocator_total(&allocator).bytes_in_use == 0);
}

/* ring_reuses_two_spare_chunks checks that a ring which peaked at two chunks keeps both as spares, so later
 * fill-and-drain cycles make no allocator call. It needs only an allocator; it leaves nothing allocated. */
static void ring_reuses_two_spare_chunks(void) {
    tah_ring ring;
    tah_ring_init(&ring, other(), (tah_ring_shape){ .layout = TAH_LAYOUT(uint32_t), .chunk_capacity = 8 });
    uint32_t value = 7;
    /* Fills a chunk exactly and takes one entry more, then drains: two chunks at the peak. After the first cycle
     * both are spares, and the cycles that follow allocate nothing. */
    for (uint32_t i = 0; i < 9; i += 1) {
        CHECK(tah_ring_push(&ring, &i) == TA_OK);
    }
    while (tah_ring_pop(&ring, &value)) {
    }
    uint64_t before = tah_allocator_calls(&allocator);
    for (int round = 0; round < 100; round += 1) {
        for (uint32_t i = 0; i < 9; i += 1) {
            CHECK(tah_ring_push(&ring, &i) == TA_OK);
        }
        while (tah_ring_pop(&ring, &value)) {
        }
    }
    CHECK(tah_allocator_calls(&allocator) == before);
    tah_ring_destroy(&ring);
    CHECK(tah_allocator_total(&allocator).bytes_in_use == 0);
}

static void map_deletes_with_tombstones(void) {
    tah_map map;
    tah_map_init(&map, other());
    for (uint64_t key = 1; key <= 1000; key += 1) {
        CHECK(tah_map_put(&map, key, key * 10) == TA_OK);
    }
    CHECK(tah_map_count(&map) == 1000);
    CHECK(tah_map_put(&map, 5, 55) == TA_OK); /* replace */
    uint64_t value = 0;
    CHECK(tah_map_get(&map, 5, &value) && value == 55);
    CHECK(tah_map_count(&map) == 1000);

    for (uint64_t key = 1; key <= 1000; key += 2) {
        CHECK(tah_map_remove(&map, key));
    }
    CHECK(!tah_map_remove(&map, 1));
    CHECK(tah_map_count(&map) == 500);
    CHECK(tah_map_tombstones(&map) == 500);
    for (uint64_t key = 1; key <= 1000; key += 1) {
        /* Probes pass tombstones to reach the keys behind them. */
        CHECK(tah_map_get(&map, key, &value) == (key % 2 == 0));
        if (key % 2 == 0) {
            CHECK(value == (key == 5 ? 55 : key * 10));
        }
    }

    /* Reinserting reuses tombstones; churn never lets them fill the table. */
    for (int round = 0; round < 20; round += 1) {
        for (uint64_t key = 2001; key <= 2400; key += 1) {
            CHECK(tah_map_put(&map, key, key) == TA_OK);
        }
        for (uint64_t key = 2001; key <= 2400; key += 1) {
            CHECK(tah_map_remove(&map, key));
        }
    }
    CHECK(tah_map_count(&map) == 500);
    CHECK((uint64_t)(tah_map_count(&map) + tah_map_tombstones(&map)) * 4u <= (uint64_t)map.capacity * 3u);
    CHECK(tah_map_get(&map, 1000, &value) && value == 10000);
    CHECK(!tah_map_get(&map, 2001, &value));
    /* Iteration visits every live entry once and skips tombstones. */
    uint32_t cursor = 0;
    uint32_t seen = 0;
    uint64_t key = 0;
    while (tah_map_next(&map, &cursor, &key, &value)) {
        CHECK(tah_map_get(&map, key, NULL));
        seen += 1u;
    }
    CHECK(seen == tah_map_count(&map));
    tah_map_destroy(&map);
    CHECK(tah_allocator_total(&allocator).bytes_in_use == 0);
}

typedef struct {
    uint64_t a;
    uint64_t b;
} record;

static void free_list_reuses_records(void) {
    tah_free_list list;
    tah_free_list_init(&list, tah_allocator_for(&allocator, TAH_MEMORY_ENVELOPES), TAH_LAYOUT(record));
    record *taken[10];
    for (int i = 0; i < 10; i += 1) {
        taken[i] = tah_free_list_take(&list);
        CHECK(taken[i] != NULL);
        taken[i]->a = (uint64_t)i;
    }
    CHECK(list.live_count == 10);
    for (int i = 0; i < 10; i += 1) {
        tah_free_list_give(&list, taken[i]);
    }
    CHECK(list.free_count >= 10 && list.live_count == 0);

    /* Taking again reuses the records: no allocator call. */
    uint64_t before = tah_allocator_calls(&allocator);
    for (int round = 0; round < 100; round += 1) {
        for (int i = 0; i < 10; i += 1) {
            taken[i] = tah_free_list_take(&list);
        }
        for (int i = 0; i < 10; i += 1) {
            tah_free_list_give(&list, taken[i]);
        }
    }
    CHECK(tah_allocator_calls(&allocator) == before);

    tah_free_list_destroy(&list);
    CHECK(tah_allocator_total(&allocator).bytes_in_use == 0);
}

/* A burst of records costs a chunk per up-to-1024 records, not a call each,
 * and records from one chunk never overlap. */
static void free_list_allocates_in_chunks(void) {
    enum { N = 10000 };
    static record *taken[N];
    tah_free_list list;
    tah_free_list_init(&list, tah_allocator_for(&allocator, TAH_MEMORY_ENVELOPES), TAH_LAYOUT(record));
    uint64_t before = tah_allocator_calls(&allocator);
    for (int i = 0; i < N; i += 1) {
        taken[i] = tah_free_list_take(&list);
        CHECK(taken[i] != NULL);
        CHECK(((uintptr_t)taken[i] & (_Alignof(record) - 1u)) == 0);
        taken[i]->a = (uint64_t)i;
        taken[i]->b = ~(uint64_t)i;
    }
    uint64_t calls = tah_allocator_calls(&allocator) - before;
    CHECK(calls < N / 100);
    for (int i = 0; i < N; i += 1) {
        CHECK(taken[i]->a == (uint64_t)i && taken[i]->b == ~(uint64_t)i);
    }
    for (int i = 0; i < N; i += 1) {
        tah_free_list_give(&list, taken[i]);
    }
    CHECK(list.live_count == 0);
    tah_free_list_destroy(&list);
    CHECK(tah_allocator_total(&allocator).bytes_in_use == 0);
}

int main(void) {
    tah_allocator_init(&allocator, check_platform_allocator());
    vec_grows();
    ring_keeps_fifo_order_across_chunks();
    ring_reuses_its_spare_chunk();
    ring_reuses_two_spare_chunks();
    map_deletes_with_tombstones();
    free_list_reuses_records();
    free_list_allocates_in_chunks();
    /* Accounting balances to zero over everything above. */
    tah_memory_counters total = tah_allocator_total(&allocator);
    CHECK(total.bytes_in_use == 0);
    CHECK(total.allocations == total.frees);
    puts("containers_test passed");
    return 0;
}
