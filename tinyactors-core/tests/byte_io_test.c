/* test: standalone asan */
/*
 * Block D1 (src/byte_io.h): the byte sinks and sources.
 *
 * Covers the arena sink and the memory source (round trips in every reader
 * piece size, growth by doubling, begin resetting, end(keep=false)
 * discarding, an arena that cannot grow). It is fast, not exhaustive.
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

int main(void) {
    test_arena_round_trip();
    test_arena_begin_and_discard();
    test_arena_cannot_grow();
    test_memory_source();
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "byte_io_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("byte_io_test: ok");
    return 0;
}
