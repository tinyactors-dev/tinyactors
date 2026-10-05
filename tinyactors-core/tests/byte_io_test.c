/* test: standalone asan */
/*
 * Block D1 (src/byte_io.h): the byte sinks and sources.
 *
 * Covers the arena sink and the memory source (round trips in every reader
 * piece size, growth by doubling, begin resetting, end(keep=false)
 * discarding, an arena that cannot grow), and the chain sink and source
 * (round trips at both chunk sizes around the chunk boundaries, the preflight,
 * running out of chunks mid-unit, all or nothing, no allocation, and the
 * chain bytes equal to the arena bytes for the same unit). It is fast, not
 * exhaustive.
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

#define MAX_BYTES 4096u

/* Deterministic content: byte i of a unit. */
static unsigned char pattern(uint32_t i) {
    return (unsigned char)((i * 7u + (i >> 8) * 13u + 5u) & 0xffu);
}

static void fill_pattern(unsigned char *out, uint32_t first, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) {
        out[i] = pattern(first + i);
    }
}

/* write_unit writes `total` pattern bytes in pieces of `piece` through *sink (0-byte pieces included once). */
static ta_op_status write_unit(ta_byte_sink sink, uint32_t total, uint32_t piece) {
    unsigned char buffer[MAX_BYTES];
    uint32_t at = 0;
    ta_op_status status = sink.v->write(sink.self, (ta_bytes){ .ptr = buffer, .len = 0 });
    while (status == TA_OP_OK && at < total) {
        uint32_t count = total - at < piece ? total - at : piece;
        fill_pattern(buffer, at, count);
        status = sink.v->write(sink.self, (ta_bytes){ .ptr = buffer, .len = count });
        at += count;
    }
    return status;
}

/* read_unit reads a source to its end in pieces of `piece`, checks the pattern, and returns the byte count. */
static uint32_t read_unit(ta_byte_source source, uint32_t piece) {
    unsigned char buffer[MAX_BYTES];
    uint32_t at = 0;
    for (;;) {
        uint32_t got = 12345;
        ta_op_status status = source.v->read(source.self, &(ta_byte_read_params){ .buffer = buffer, .capacity = piece, .got = &got });
        CHECK(status == TA_OP_OK);
        CHECK(got <= piece);
        if (got == 0) {
            return at;
        }
        for (uint32_t i = 0; i < got; i++) {
            if (buffer[i] != pattern(at + i)) {
                CHECK(!"byte differs from the pattern");
                return at;
            }
        }
        at += got;
    }
}

static const uint32_t sizes[] = { 0, 1, 63, 64, 65, 200, 1000, 3000 };
static const uint32_t pieces[] = { 1, 7, 64, 1000 };

static void test_arena_round_trip(void) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, ta_test_allocator_get(&heap), 256) == TA_OK);
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        for (size_t p = 0; p < sizeof pieces / sizeof pieces[0]; p++) {
            ta_arena_sink state;
            ta_arena_sink_init(&state, &arena);
            ta_byte_sink sink = ta_arena_sink_as(&state);
            CHECK(sink.v->begin(sink.self) == TA_OP_OK);
            CHECK(write_unit(sink, sizes[s], pieces[p]) == TA_OP_OK);
            sink.v->end(sink.self, true);
            CHECK(state.len == sizes[s]);
            for (size_t r = 0; r < sizeof pieces / sizeof pieces[0]; r++) {
                ta_memory_source memory;
                ta_memory_source_init(&memory, (ta_bytes){ .ptr = state.ptr, .len = (uint32_t)state.len });
                CHECK(read_unit(ta_memory_source_as(&memory), pieces[r]) == sizes[s]);
            }
        }
    }
    ta_arena_destroy(&arena);
}

static void test_arena_begin_and_discard(void) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, ta_test_allocator_get(&heap), 256) == TA_OK);
    ta_arena_sink state;
    ta_arena_sink_init(&state, &arena);
    ta_byte_sink sink = ta_arena_sink_as(&state);
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_unit(sink, 100, 10) == TA_OP_OK);
    CHECK(state.len == 100);
    sink.v->end(sink.self, false);
    CHECK(state.len == 0);

    /* A second unit reuses the run: begin starts at 0, and no new run is needed. */
    uint8_t *run = state.ptr;
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_unit(sink, 50, 50) == TA_OP_OK);
    sink.v->end(sink.self, true);
    CHECK(state.len == 50);
    CHECK(state.ptr == run);
    ta_memory_source memory;
    ta_memory_source_init(&memory, (ta_bytes){ .ptr = state.ptr, .len = (uint32_t)state.len });
    CHECK(read_unit(ta_memory_source_as(&memory), 7) == 50);
    ta_arena_destroy(&arena);
}

static void test_arena_cannot_grow(void) {
    _Alignas(16) unsigned char memory[512];
    ta_arena arena;
    ta_arena_init_fixed(&arena, memory, sizeof memory);
    ta_arena_sink state;
    ta_arena_sink_init(&state, &arena);
    ta_byte_sink sink = ta_arena_sink_as(&state);
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_unit(sink, 60, 60) == TA_OP_OK);
    CHECK(state.len == 60);
    /* 60 + 3000 needs a 4096-byte run, which a 512-byte arena cannot give; the failed piece changes nothing. */
    CHECK(write_unit(sink, 3000, 3000) == TA_OP_NOMEM);
    CHECK(state.len == 60);
    sink.v->end(sink.self, false);
    CHECK(state.len == 0);
}

static void test_memory_source(void) {
    ta_memory_source memory;
    unsigned char buffer[8];
    uint32_t got = 99;
    ta_memory_source_init(&memory, (ta_bytes){ .ptr = NULL, .len = 0 });
    ta_byte_source source = ta_memory_source_as(&memory);
    CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = buffer, .capacity = 8, .got = &got }) == TA_OP_OK);
    CHECK(got == 0);

    unsigned char data[5];
    fill_pattern(data, 0, 5);
    ta_memory_source_init(&memory, (ta_bytes){ .ptr = data, .len = 5 });
    CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = buffer, .capacity = 8, .got = &got }) == TA_OP_OK);
    CHECK(got == 5);
    CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = buffer, .capacity = 8, .got = &got }) == TA_OP_OK);
    CHECK(got == 0);
    CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = buffer, .capacity = 8, .got = &got }) == TA_OP_OK);
    CHECK(got == 0);
    ta_memory_source_init(&memory, (ta_bytes){ .ptr = data, .len = 5 });
    CHECK(read_unit(source, 2) == 5);
}

static ta_pool_counts counts_of(const ta_pool *pool) {
    ta_pool_counts counts;
    ta_pool_counts_get(pool, &counts);
    return counts;
}

static void chunk_pool(ta_pool *pool, uint32_t chunk_size, uint32_t max_chunks) {
    ta_chain_pool_params params = { .chunk_size = chunk_size, .page_chunks = max_chunks != 0 ? max_chunks : 16, .max_chunks = max_chunks };
    CHECK(ta_chain_pool_init(pool, ta_test_allocator_get(&heap), &params) == TA_OK);
}

static ta_chain_sink chain_sink(ta_pool *pool, ta_chain *chain, uint32_t reserve) {
    ta_chain_sink sink;
    ta_chain_sink_init(&sink, &(ta_chain_sink_params){ .pool = pool, .chain = chain, .reserve = reserve });
    return sink;
}

static void test_chain_round_trip(uint32_t chunk_size) {
    ta_pool pool;
    chunk_pool(&pool, chunk_size, 0);
    uint32_t payload = ta_chain_payload(&pool);
    uint32_t unit_sizes[] = { 0, 1, payload - 1, payload, payload + 1, 3 * payload + 5 };
    for (size_t s = 0; s < sizeof unit_sizes / sizeof unit_sizes[0]; s++) {
        uint32_t size = unit_sizes[s];
        for (size_t p = 0; p < sizeof pieces / sizeof pieces[0]; p++) {
            CHECK(ta_pool_refill(&pool, ta_chain_need(&pool, NULL, size)) == TA_OK);
            ta_chain chain = { 0 };
            ta_chain_sink state = chain_sink(&pool, &chain, size);
            ta_byte_sink sink = ta_chain_sink_as(&state);
            uint32_t used = counts_of(&pool).used;
            CHECK(sink.v->begin(sink.self) == TA_OP_OK);
            CHECK(write_unit(sink, size, pieces[p]) == TA_OP_OK);
            sink.v->end(sink.self, true);
            CHECK(chain.length == size);
            CHECK(counts_of(&pool).used - used == ta_chain_need(&pool, NULL, size));
            for (size_t r = 0; r < sizeof pieces / sizeof pieces[0]; r++) {
                ta_chain_source source;
                ta_chain_source_init(&source, &pool, &chain);
                CHECK(read_unit(ta_chain_source_as(&source), pieces[r]) == size);
            }
            CHECK(ta_chain_free(&pool, &chain) == ta_chain_need(&pool, NULL, size));
            CHECK(counts_of(&pool).used == used);
        }
    }
    ta_pool_destroy(&pool);
}

static void test_chain_all_or_nothing(void) {
    ta_pool pool;
    chunk_pool(&pool, 64, 4); /* 4 chunks, 60 bytes each: 240 bytes at most */
    CHECK(ta_pool_refill(&pool, 4) == TA_OK);
    CHECK(counts_of(&pool).free == 4);

    /* Out of chunks mid-unit: the failing piece takes nothing, end(false) returns everything. */
    ta_chain chain = { 0 };
    ta_chain_sink state = chain_sink(&pool, &chain, 0);
    ta_byte_sink sink = ta_chain_sink_as(&state);
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_unit(sink, 200, 50) == TA_OP_OK);
    ta_pool_counts before = counts_of(&pool);
    ta_chain chain_before = chain;
    CHECK(write_unit(sink, 100, 100) == TA_OP_NOMEM);
    ta_pool_counts after = counts_of(&pool);
    CHECK(after.used == before.used && after.free == before.free && after.extent == before.extent);
    CHECK(chain.head == chain_before.head && chain.tail == chain_before.tail && chain.length == chain_before.length);
    sink.v->end(sink.self, false);
    CHECK(state.freed == 4);
    CHECK(chain.length == 0 && chain.head == 0 && chain.tail == 0);
    CHECK(counts_of(&pool).used == 0);

    /* The pool then serves a smaller unit, and the sink can be reused. */
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_unit(sink, 100, 7) == TA_OP_OK);
    sink.v->end(sink.self, true);
    CHECK(state.freed == 0);
    ta_chain_source source;
    ta_chain_source_init(&source, &pool, &chain);
    CHECK(read_unit(ta_chain_source_as(&source), 13) == 100);
    CHECK(ta_chain_free(&pool, &chain) == 2);

    /* A unit larger than the pool whose size is known is refused by begin, taking nothing. */
    ta_chain_sink big = chain_sink(&pool, &chain, 241);
    ta_byte_sink big_sink = ta_chain_sink_as(&big);
    CHECK(big_sink.v->begin(big_sink.self) == TA_OP_NOMEM);
    CHECK(counts_of(&pool).used == 0);
    CHECK(write_unit(big_sink, 1, 1) == TA_OP_ERROR); /* never opened */
    big_sink.v->end(big_sink.self, false);            /* harmless */
    CHECK(chain.length == 0);

    /* begin on a non-empty chain leaves it untouched. */
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_unit(sink, 10, 10) == TA_OP_OK);
    sink.v->end(sink.self, true);
    ta_chain_sink again = chain_sink(&pool, &chain, 0);
    ta_byte_sink again_sink = ta_chain_sink_as(&again);
    CHECK(again_sink.v->begin(again_sink.self) == TA_OP_ERROR);
    CHECK(chain.length == 10);
    CHECK(sink.v->begin(sink.self) == TA_OP_ERROR); /* its own chain is not empty either */
    CHECK(ta_chain_free(&pool, &chain) == 1);

    /* A sink that is open cannot begin again. */
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(sink.v->begin(sink.self) == TA_OP_ERROR);
    sink.v->end(sink.self, false);
    CHECK(counts_of(&pool).used == 0);
    ta_pool_destroy(&pool);
}

static void test_chain_no_allocation(void) {
    ta_pool pool;
    chunk_pool(&pool, 64, 0);
    CHECK(ta_pool_refill(&pool, 8) == TA_OK);
    ta_chain chain = { 0 };
    ta_chain_sink state = chain_sink(&pool, &chain, 300);
    ta_byte_sink sink = ta_chain_sink_as(&state);
    uint64_t attempts = heap.stats.attempts;
    uint64_t frees = heap.stats.frees;
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_unit(sink, 300, 33) == TA_OP_OK);
    sink.v->end(sink.self, true);
    ta_chain_source source;
    ta_chain_source_init(&source, &pool, &chain);
    CHECK(read_unit(ta_chain_source_as(&source), 17) == 300);
    CHECK(sink.v->begin(sink.self) == TA_OP_ERROR);
    CHECK(ta_chain_free(&pool, &chain) == 5);
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_unit(sink, 100, 100) == TA_OP_OK);
    sink.v->end(sink.self, false);
    CHECK(heap.stats.attempts == attempts);
    CHECK(heap.stats.frees == frees);
    ta_pool_destroy(&pool);
}

/* The same unit through the arena sink and the chain sink gives the same bytes. */
static void test_chain_equals_arena(void) {
    ta_pool pool;
    chunk_pool(&pool, 64, 0);
    ta_arena arena;
    CHECK(ta_arena_init(&arena, ta_test_allocator_get(&heap), 256) == TA_OK);
    for (size_t p = 0; p < sizeof pieces / sizeof pieces[0]; p++) {
        CHECK(ta_pool_refill(&pool, 60) == TA_OK);
        ta_chain chain = { 0 };
        ta_chain_sink chain_state = chain_sink(&pool, &chain, 0);
        ta_arena_sink arena_state;
        ta_arena_sink_init(&arena_state, &arena);
        ta_byte_sink sinks[2] = { ta_chain_sink_as(&chain_state), ta_arena_sink_as(&arena_state) };
        for (int i = 0; i < 2; i++) {
            CHECK(sinks[i].v->begin(sinks[i].self) == TA_OP_OK);
            CHECK(write_unit(sinks[i], 3000, pieces[p]) == TA_OP_OK);
            sinks[i].v->end(sinks[i].self, true);
        }
        CHECK(chain.length == arena_state.len);
        unsigned char copy[3000];
        ta_chain_reader reader;
        ta_chain_reader_init(&reader, &pool, &chain);
        CHECK(ta_chain_reader_copy(&reader, copy, 3000));
        for (uint32_t i = 0; i < 3000; i++) {
            CHECK(copy[i] == arena_state.ptr[i]);
        }
        CHECK(ta_chain_free(&pool, &chain) == 50);
    }
    ta_arena_destroy(&arena);
    ta_pool_destroy(&pool);
}

int main(void) {
    test_arena_round_trip();
    test_arena_begin_and_discard();
    test_arena_cannot_grow();
    test_memory_source();
    test_chain_round_trip(64);
    test_chain_round_trip(256);
    test_chain_all_or_nothing();
    test_chain_no_allocation();
    test_chain_equals_arena();
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "byte_io_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("byte_io_test: ok");
    return 0;
}
