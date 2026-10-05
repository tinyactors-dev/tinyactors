/*
 * ta_arena_sink and ta_memory_source, ta_chunk_sink and ta_chunk_source:
 * the byte sinks and sources core provides itself (src/bytes.h). A data
 * model writes a value into a sink and reads it back from a source;
 * parking writes a parked execution into a chunk chain and resuming reads
 * it back.
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Memory comes from the test
 * allocator (test_allocator.h) behind an arena of small blocks, so every
 * run the sink takes is one request to the allocator; the allocator can
 * fail from a given attempt on, and refuses requests above a size.
 *
 * Claims proven here, in order:
 *
 * 1. a new arena sink has no run and allocates nothing; begin and an empty
 *    unit kept with end(keep=true) leave it empty and allocate nothing; a
 *    write of zero bytes changes nothing and allocates nothing;
 * 2. pieces written between begin and end(keep=true) are the unit, in
 *    order, in one contiguous run aligned for uint64_t; one piece of one
 *    byte is a unit of one byte;
 * 3. the first run holds 64 bytes: a unit of exactly 64 bytes takes one
 *    run, and the 65th byte doubles it to 128, copying the unit so far;
 *    a piece larger than twice the run takes the power-of-two multiple of
 *    64 that fits it, in one allocation;
 * 4. end(keep=false) and begin discard the unit and keep the run, so the
 *    next unit reuses it without allocating;
 * 5. a write the arena cannot serve answers TA_OP_NOMEM and changes
 *    nothing: the same run, length, capacity and bytes; the writer's
 *    end(keep=false) then empties the unit, and destroying the arena
 *    leaves no block behind; the same holds over a fixed arena that is
 *    full;
 * 6. a write whose unit would not fit in size_t, or whose doubled run
 *    would not, answers TA_OP_NOMEM, changes nothing and returns;
 * 7. a memory source hands out its unit in the reader's pieces, in order,
 *    at every piece size from 1 to past the unit's length, then answers
 *    *got == 0 with TA_OP_OK on every later read; a read of capacity 0
 *    hands out nothing and does not advance; an empty source has ended
 *    from its first read; reads never allocate;
 * 8. a unit written through an arena sink and read back through a memory
 *    source over its bytes is the same unit, through the public adapters
 *    a data model is given;
 * 9. a chunk sink writes one unit into a new chain, for both chunk sizes
 *    (64 and 256 bytes): an empty unit holds no chunk, pieces of every
 *    size near a chunk's payload land in order, and after
 *    end(keep=true) the chain is the caller's, which the next begin
 *    leaves alone; it never allocates;
 * 10. a chunk sink write that needs more chunks than are free answers
 *    TA_OP_NOMEM and changes nothing: the same chain, the same free
 *    chunks; end(keep=false) then returns every chunk of the unit to the
 *    pool;
 * 11. a chunk source hands a chain out in the reader's pieces, in order,
 *    across chunk boundaries, at every piece size from 1 to past two
 *    chunks, then answers *got == 0 on every later read; a read of
 *    capacity 0 hands out nothing; an empty chain has ended from its
 *    first read; reading never allocates and leaves the chain as it was;
 * 12. a unit written through a chunk sink and read back through a chunk
 *    source is the same unit, at every length near a chunk boundary and
 *    for both chunk sizes, through the public adapters.
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

/* An arena over `heap` whose blocks are as small as can be, so every run
 * the sink takes is its own request. */
static ta_arena arena_over(test_allocator *heap) {
    ta_arena arena;
    ta_result result = ta_arena_init(&arena, test_allocator_of(heap), 1u);
    CHECK(result == TA_OK);
    return arena;
}

/* Byte i of a test pattern. */
static uint8_t pattern(size_t index) {
    return (uint8_t)(index * 7u + 3u);
}

static void fill(uint8_t *bytes, size_t count, size_t from) {
    for (size_t index = 0; index < count; index += 1u) {
        bytes[index] = pattern(from + index);
    }
}

static bool holds_pattern(const uint8_t *bytes, size_t count) {
    for (size_t index = 0; index < count; index += 1u) {
        if (bytes[index] != pattern(index)) {
            return false;
        }
    }
    return true;
}

/* Writes `count` bytes of the pattern from `from` as one piece. */
static ta_op_status write_pattern(ta_byte_sink sink, size_t count, size_t from) {
    uint8_t piece[1024];
    fill(piece, count, from);
    return sink.v->write(sink.self, (ta_bytes){ .ptr = piece, .len = (uint32_t)count });
}

/* ------------------------------------------------------------------------
 * Claim 1: an empty sink and empty writes allocate nothing.
 */
static void test_empty_sink(void) {
    test_allocator heap = {0};
    ta_arena arena = arena_over(&heap);
    ta_arena_sink state;
    ta_arena_sink_init(&state, &arena);
    CHECK(state.arena == &arena && state.ptr == NULL && state.len == 0 && state.cap == 0);

    ta_byte_sink sink = ta_arena_sink_as_byte_sink(&state);
    CHECK(sink.self == &state && sink.v != NULL);
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    uint8_t unused = 0;
    CHECK(sink.v->write(sink.self, (ta_bytes){ .ptr = &unused, .len = 0 }) == TA_OP_OK);
    CHECK(sink.v->write(sink.self, (ta_bytes){0}) == TA_OP_OK);
    sink.v->end(sink.self, true);
    CHECK(state.ptr == NULL && state.len == 0 && state.cap == 0);
    CHECK(heap.attempts == 0);
    ta_arena_destroy(&arena);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 2: pieces are gathered in order into one aligned run.
 */
static void test_pieces_in_order(void) {
    test_allocator heap = {0};
    ta_arena arena = arena_over(&heap);
    ta_arena_sink state;
    ta_arena_sink_init(&state, &arena);
    ta_byte_sink sink = ta_arena_sink_as_byte_sink(&state);

    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_pattern(sink, 1, 0) == TA_OP_OK);
    sink.v->end(sink.self, true);
    CHECK(state.len == 1 && state.ptr != NULL && state.ptr[0] == pattern(0));
    CHECK(((uintptr_t)state.ptr % _Alignof(uint64_t)) == 0);

    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    size_t sizes[] = { 3, 0, 17, 1, 40, 2 };
    size_t at = 0;
    for (size_t index = 0; index < sizeof sizes / sizeof sizes[0]; index += 1u) {
        CHECK(write_pattern(sink, sizes[index], at) == TA_OP_OK);
        at += sizes[index];
    }
    sink.v->end(sink.self, true);
    CHECK(state.len == at && holds_pattern(state.ptr, at));
    CHECK(((uintptr_t)state.ptr % _Alignof(uint64_t)) == 0);
    ta_arena_destroy(&arena);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 3: the run starts at 64 bytes and doubles.
 */
static void test_growth(void) {
    test_allocator heap = {0};
    ta_arena arena = arena_over(&heap);
    ta_arena_sink state;
    ta_arena_sink_init(&state, &arena);
    ta_byte_sink sink = ta_arena_sink_as_byte_sink(&state);

    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_pattern(sink, 63, 0) == TA_OP_OK);
    CHECK(state.cap == 64 && heap.attempts == 1);
    CHECK(write_pattern(sink, 1, 63) == TA_OP_OK);
    CHECK(state.cap == 64 && state.len == 64 && heap.attempts == 1);
    uint8_t *first = state.ptr;
    CHECK(write_pattern(sink, 1, 64) == TA_OP_OK);
    CHECK(state.cap == 128 && state.len == 65 && heap.attempts == 2 && state.ptr != first);
    CHECK(holds_pattern(state.ptr, 65));
    /* 65 + 400 = 465 bytes: 128 doubles to 512 in one allocation. */
    CHECK(write_pattern(sink, 400, 65) == TA_OP_OK);
    CHECK(state.cap == 512 && state.len == 465 && heap.attempts == 3);
    CHECK(holds_pattern(state.ptr, 465));
    sink.v->end(sink.self, true);
    CHECK(state.len == 465 && holds_pattern(state.ptr, 465));

    /* A first piece of 200 bytes takes a run of 256 at once. */
    ta_arena_sink fresh;
    ta_arena_sink_init(&fresh, &arena);
    ta_byte_sink other = ta_arena_sink_as_byte_sink(&fresh);
    CHECK(other.v->begin(other.self) == TA_OP_OK);
    CHECK(write_pattern(other, 200, 0) == TA_OP_OK);
    CHECK(fresh.cap == 256 && fresh.len == 200 && heap.attempts == 4 && holds_pattern(fresh.ptr, 200));
    other.v->end(other.self, true);
    ta_arena_destroy(&arena);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 4: a discarded unit keeps the run for the next one.
 */
static void test_discard_keeps_run(void) {
    test_allocator heap = {0};
    ta_arena arena = arena_over(&heap);
    ta_arena_sink state;
    ta_arena_sink_init(&state, &arena);
    ta_byte_sink sink = ta_arena_sink_as_byte_sink(&state);

    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_pattern(sink, 100, 0) == TA_OP_OK);
    uint8_t *run = state.ptr;
    sink.v->end(sink.self, false);
    CHECK(state.len == 0 && state.ptr == run && state.cap == 128);
    uint32_t attempts = heap.attempts;

    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_pattern(sink, 128, 0) == TA_OP_OK);
    CHECK(state.ptr == run && heap.attempts == attempts);
    sink.v->end(sink.self, true);
    CHECK(state.len == 128 && holds_pattern(state.ptr, 128));

    /* begin also starts a new unit over the same run. */
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(state.len == 0 && state.ptr == run && state.cap == 128);
    CHECK(write_pattern(sink, 10, 0) == TA_OP_OK);
    sink.v->end(sink.self, true);
    CHECK(state.len == 10 && holds_pattern(state.ptr, 10) && heap.attempts == attempts);
    ta_arena_destroy(&arena);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 5: a refused write changes nothing and leaks nothing.
 */
static void test_failed_write(void) {
    test_allocator heap = {0};
    ta_arena arena = arena_over(&heap);
    ta_arena_sink state;
    ta_arena_sink_init(&state, &arena);
    ta_byte_sink sink = ta_arena_sink_as_byte_sink(&state);

    /* Refused at the first run: still no run. */
    heap.policy.fail_from = 1;
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_pattern(sink, 10, 0) == TA_OP_NOMEM);
    CHECK(state.ptr == NULL && state.len == 0 && state.cap == 0);
    sink.v->end(sink.self, false);
    CHECK(heap.live == 0);

    /* Refused while growing: the same run, length, capacity and bytes. */
    heap.policy.fail_from = 0;
    heap.attempts = 0;
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_pattern(sink, 60, 0) == TA_OP_OK);
    uint8_t *run = state.ptr;
    uint8_t before[64];
    memcpy(before, state.ptr, sizeof before);
    heap.policy.fail_from = heap.attempts + 1u;
    CHECK(write_pattern(sink, 5, 60) == TA_OP_NOMEM);
    CHECK(state.ptr == run && state.len == 60 && state.cap == 64);
    CHECK(memcmp(before, state.ptr, sizeof before) == 0);
    /* A piece that still fits needs no allocation, so it is taken. */
    CHECK(write_pattern(sink, 4, 60) == TA_OP_OK);
    CHECK(state.len == 64 && holds_pattern(state.ptr, 64));
    sink.v->end(sink.self, false);
    CHECK(state.len == 0);
    ta_arena_destroy(&arena);
    CHECK(heap.live == 0);

    /* A fixed arena that is full refuses the same way. */
    _Alignas(ta_arena_block) uint8_t memory[256];
    ta_arena fixed;
    ta_arena_init_fixed(&fixed, memory, sizeof memory);
    ta_arena_sink bounded;
    ta_arena_sink_init(&bounded, &fixed);
    ta_byte_sink small = ta_arena_sink_as_byte_sink(&bounded);
    CHECK(small.v->begin(small.self) == TA_OP_OK);
    CHECK(write_pattern(small, 64, 0) == TA_OP_OK);
    uint8_t *kept = bounded.ptr;
    CHECK(write_pattern(small, 300, 64) == TA_OP_NOMEM);
    CHECK(bounded.ptr == kept && bounded.len == 64 && bounded.cap == 64 && holds_pattern(bounded.ptr, 64));
    small.v->end(small.self, false);
    ta_arena_destroy(&fixed);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 6: sizes beyond size_t answer TA_OP_NOMEM and return.
 */
static void test_size_overflow(void) {
    test_allocator heap = { .policy.largest = 4096 };
    ta_arena arena = arena_over(&heap);
    uint8_t stand_in[8] = {0};
    uint8_t piece[32] = {0};

    /* The unit would pass SIZE_MAX: nothing is asked of the arena. */
    ta_arena_sink state = { .arena = &arena, .ptr = stand_in, .len = SIZE_MAX - 10u, .cap = SIZE_MAX - 5u };
    ta_byte_sink sink = ta_arena_sink_as_byte_sink(&state);
    CHECK(sink.v->write(sink.self, (ta_bytes){ .ptr = piece, .len = 20 }) == TA_OP_NOMEM);
    CHECK(state.ptr == stand_in && state.len == SIZE_MAX - 10u && state.cap == SIZE_MAX - 5u);
    CHECK(heap.attempts == 0);

    /* Doubling the run would pass SIZE_MAX: the run asked for is the unit's
     * size, which the arena refuses. */
    state = (ta_arena_sink){ .arena = &arena, .ptr = stand_in, .len = SIZE_MAX / 2u + 1u, .cap = SIZE_MAX / 2u + 1u };
    CHECK(sink.v->write(sink.self, (ta_bytes){ .ptr = piece, .len = 1 }) == TA_OP_NOMEM);
    CHECK(state.ptr == stand_in && state.len == SIZE_MAX / 2u + 1u && state.cap == SIZE_MAX / 2u + 1u);
    ta_arena_destroy(&arena);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 7: a memory source hands out its unit in the reader's pieces.
 */
static void test_memory_source(void) {
    uint8_t unit[37];
    fill(unit, sizeof unit, 0);
    for (uint32_t piece = 1; piece <= sizeof unit + 3u; piece += 1u) {
        ta_memory_source state;
        ta_memory_source_init(&state, (ta_bytes){ .ptr = unit, .len = sizeof unit });
        ta_byte_source source = ta_memory_source_as_byte_source(&state);
        CHECK(source.self == &state && source.v != NULL);
        uint8_t out[64];
        memset(out, 0, sizeof out);
        uint32_t total = 0;
        for (;;) {
            uint32_t got = 0xFFFFFFFFu;
            ta_op_status status = source.v->read(
                source.self, &(ta_byte_read_params){ .buffer = out + total, .capacity = piece, .got = &got });
            CHECK(status == TA_OP_OK);
            if (got == 0) {
                break;
            }
            uint32_t left = (uint32_t)sizeof unit - total;
            CHECK(got == (left < piece ? left : piece));
            total += got;
            if (total > sizeof unit) {
                break;
            }
        }
        CHECK(total == sizeof unit && memcmp(out, unit, sizeof unit) == 0);
        uint32_t got = 0xFFFFFFFFu;
        CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = out, .capacity = piece, .got = &got }) ==
              TA_OP_OK);
        CHECK(got == 0 && state.at == sizeof unit);
    }

    /* Capacity 0 hands out nothing and does not advance. */
    ta_memory_source state;
    ta_memory_source_init(&state, (ta_bytes){ .ptr = unit, .len = sizeof unit });
    ta_byte_source source = ta_memory_source_as_byte_source(&state);
    uint32_t got = 0xFFFFFFFFu;
    CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = NULL, .capacity = 0, .got = &got }) ==
          TA_OP_OK);
    CHECK(got == 0 && state.at == 0);

    /* One byte, then the end. */
    uint8_t one = 0;
    CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = &one, .capacity = 1, .got = &got }) ==
          TA_OP_OK);
    CHECK(got == 1 && one == pattern(0) && state.at == 1);

    /* An empty source has ended from its first read. */
    ta_memory_source empty;
    ta_memory_source_init(&empty, (ta_bytes){0});
    ta_byte_source nothing = ta_memory_source_as_byte_source(&empty);
    uint8_t buffer[4] = {0};
    for (int round = 0; round < 2; round += 1) {
        got = 0xFFFFFFFFu;
        CHECK(nothing.v->read(nothing.self, &(ta_byte_read_params){ .buffer = buffer, .capacity = 4, .got = &got }) ==
              TA_OP_OK);
        CHECK(got == 0 && empty.at == 0);
    }
}

/* ------------------------------------------------------------------------
 * Claim 8: a unit round-trips from an arena sink through a memory source.
 */
static void test_round_trip(void) {
    test_allocator heap = {0};
    ta_arena arena = arena_over(&heap);
    ta_arena_sink written;
    ta_arena_sink_init(&written, &arena);
    ta_byte_sink sink = ta_arena_sink_as_byte_sink(&written);
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    size_t at = 0;
    for (size_t piece = 1; at + piece <= 900u; piece += 13u) {
        CHECK(write_pattern(sink, piece, at) == TA_OP_OK);
        at += piece;
    }
    sink.v->end(sink.self, true);
    CHECK(written.len == at);

    ta_memory_source read;
    ta_memory_source_init(&read, (ta_bytes){ .ptr = written.ptr, .len = (uint32_t)written.len });
    ta_byte_source source = ta_memory_source_as_byte_source(&read);
    uint32_t attempts = heap.attempts;
    uint8_t out[1024];
    uint32_t total = 0;
    uint32_t got = 0;
    do {
        CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = out + total, .capacity = 50, .got = &got }) ==
              TA_OP_OK);
        total += got;
    } while (got != 0 && total <= at);
    CHECK(total == at && holds_pattern(out, at));
    CHECK(heap.attempts == attempts);
    ta_arena_destroy(&arena);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* An area of `chunk_size` chunks over `heap`, with `free_chunks` free. */
static ta_chunks chunk_area(test_allocator *heap, uint32_t chunk_size, uint32_t free_chunks) {
    ta_chunks chunks;
    CHECK(ta_chunks_init(&chunks, &(ta_chunks_params){ .allocator = test_allocator_of(heap),
                                                       .chunk_size = chunk_size,
                                                       .page_shift = 2u,
                                                       .ceiling = 1024u }) == TA_OK);
    CHECK(ta_index_pool_refill(&chunks.pool, free_chunks) == TA_OK);
    return chunks;
}

/* Reads the whole chain through a chunk source in pieces of `piece` bytes into `out`. */
static uint32_t read_chain(const ta_chunks *chunks, ta_chunk_chain chain, uint32_t piece, uint8_t *out) {
    ta_chunk_source state;
    ta_chunk_source_init(&state, chunks, chain);
    ta_byte_source source = ta_chunk_source_as_byte_source(&state);
    CHECK(source.self == &state && source.v != NULL);
    uint32_t total = 0;
    for (;;) {
        uint32_t got = 0xFFFFFFFFu;
        CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = out + total, .capacity = piece, .got = &got }) ==
              TA_OP_OK);
        if (got == 0) {
            break;
        }
        uint32_t left = chain.length - total;
        CHECK(got == (left < piece ? left : piece));
        total += got;
        if (total > chain.length) {
            break;
        }
    }
    for (int round = 0; round < 2; round += 1) {
        uint32_t got = 0xFFFFFFFFu;
        CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = out, .capacity = piece, .got = &got }) ==
              TA_OP_OK);
        CHECK(got == 0);
    }
    return total;
}

/* The two chunk sizes core uses: the name area's and the park area's. */
static const uint32_t chunk_sizes[] = { 64u, 256u };

/* ------------------------------------------------------------------------
 * Claim 9: a chunk sink writes one unit into a new chain.
 */
static void test_chunk_sink(void) {
    for (size_t size = 0; size < sizeof chunk_sizes / sizeof chunk_sizes[0]; size += 1u) {
        test_allocator heap = {0};
        ta_chunks chunks = chunk_area(&heap, chunk_sizes[size], 16u);
        uint32_t payload = chunks.payload;
        uint32_t attempts = heap.attempts;
        ta_chunk_sink state;
        ta_chunk_sink_init(&state, &chunks);
        CHECK(state.chunks == &chunks && state.writer.chain.first == 0 && state.writer.chain.length == 0);
        ta_byte_sink sink = ta_chunk_sink_as_byte_sink(&state);
        CHECK(sink.self == &state && sink.v != NULL);

        /* An empty unit holds no chunk. */
        CHECK(sink.v->begin(sink.self) == TA_OP_OK);
        CHECK(sink.v->write(sink.self, (ta_bytes){0}) == TA_OP_OK);
        sink.v->end(sink.self, true);
        CHECK(state.writer.chain.first == 0 && state.writer.chain.length == 0 && chunks.pool.used == 0);

        /* Pieces of every size near a payload land in order. */
        CHECK(sink.v->begin(sink.self) == TA_OP_OK);
        uint32_t sizes[] = { 1u, payload - 1u, payload, payload + 1u, 2u, payload - 2u };
        size_t at = 0;
        for (size_t index = 0; index < sizeof sizes / sizeof sizes[0]; index += 1u) {
            CHECK(write_pattern(sink, sizes[index], at) == TA_OP_OK);
            at += sizes[index];
        }
        sink.v->end(sink.self, true);
        ta_chunk_chain first = state.writer.chain;
        CHECK(first.length == at);
        uint8_t out[1024];
        CHECK(read_chain(&chunks, first, 1024u, out) == at && holds_pattern(out, at));
        uint32_t used = chunks.pool.used;
        CHECK(used == (uint32_t)((at + payload - 1u) / payload));

        /* The kept chain is the caller's: the next unit leaves it alone. */
        CHECK(sink.v->begin(sink.self) == TA_OP_OK);
        CHECK(state.writer.chain.first == 0 && state.writer.chain.length == 0);
        CHECK(write_pattern(sink, 5, 0) == TA_OP_OK);
        sink.v->end(sink.self, true);
        ta_chunk_chain second = state.writer.chain;
        CHECK(second.length == 5 && second.first != first.first && chunks.pool.used == used + 1u);
        CHECK(read_chain(&chunks, first, 7u, out) == at && holds_pattern(out, at));
        CHECK(read_chain(&chunks, second, 7u, out) == 5u && holds_pattern(out, 5u));
        CHECK(heap.attempts == attempts);

        ta_chunks_free(&chunks, &first);
        ta_chunks_free(&chunks, &second);
        CHECK(chunks.pool.used == 0);
        ta_chunks_destroy(&chunks);
        CHECK(test_allocator_finish(&heap) == 0);
    }
}

/* ------------------------------------------------------------------------
 * Claim 10: a chunk sink write that does not fit changes nothing.
 */
static void test_chunk_sink_full(void) {
    test_allocator heap = {0};
    ta_chunks chunks = chunk_area(&heap, 64u, 4u);
    CHECK(chunks.pool.free == 4u);
    ta_chunk_sink state;
    ta_chunk_sink_init(&state, &chunks);
    ta_byte_sink sink = ta_chunk_sink_as_byte_sink(&state);

    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_pattern(sink, 100, 0) == TA_OP_OK); /* 2 chunks, 20 bytes of room left */
    ta_chunk_writer before = state.writer;
    CHECK(write_pattern(sink, 20u + 2u * 60u + 1u, 100) == TA_OP_NOMEM); /* needs 3, 2 free */
    CHECK(memcmp(&state.writer, &before, sizeof before) == 0 && chunks.pool.free == 2u && chunks.pool.used == 2u);
    uint8_t out[512];
    CHECK(read_chain(&chunks, state.writer.chain, 64u, out) == 100u && holds_pattern(out, 100u));
    /* Exactly what is free still fits. */
    CHECK(write_pattern(sink, 20u + 2u * 60u, 100) == TA_OP_OK);
    CHECK(chunks.pool.free == 0 && state.writer.chain.length == 240u);
    CHECK(write_pattern(sink, 1, 240) == TA_OP_NOMEM);
    CHECK(state.writer.chain.length == 240u);

    /* Discarding returns every chunk of the unit. */
    sink.v->end(sink.self, false);
    CHECK(state.writer.chain.first == 0 && state.writer.chain.length == 0);
    CHECK(chunks.pool.used == 0 && chunks.pool.free == 4u);

    /* A unit refused at its first chunk holds none, and discarding it changes nothing. */
    ta_chunk_writer keep = ta_chunk_writer_new();
    CHECK(ta_chunks_write(&chunks, &keep, (ta_bytes){ .ptr = out, .len = 4u * 60u }) == TA_OK);
    CHECK(sink.v->begin(sink.self) == TA_OP_OK);
    CHECK(write_pattern(sink, 1, 0) == TA_OP_NOMEM);
    sink.v->end(sink.self, false);
    CHECK(chunks.pool.used == 4u && keep.chain.length == 240u);
    ta_chunks_free(&chunks, &keep.chain);
    ta_chunks_destroy(&chunks);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 11: a chunk source hands a chain out in the reader's pieces.
 */
static void test_chunk_source(void) {
    test_allocator heap = {0};
    ta_chunks chunks = chunk_area(&heap, 64u, 8u);
    uint8_t unit[150];
    fill(unit, sizeof unit, 0);
    ta_chunk_writer writer = ta_chunk_writer_new();
    CHECK(ta_chunks_write(&chunks, &writer, (ta_bytes){ .ptr = unit, .len = sizeof unit }) == TA_OK);
    ta_chunk_chain chain = writer.chain;
    uint32_t attempts = heap.attempts;
    for (uint32_t piece = 1; piece <= 2u * chunks.payload + 3u; piece += 1u) {
        uint8_t out[256];
        memset(out, 0, sizeof out);
        CHECK(read_chain(&chunks, chain, piece, out) == sizeof unit);
        CHECK(memcmp(out, unit, sizeof unit) == 0);
    }
    CHECK(chain.first == writer.chain.first && chain.length == sizeof unit && chunks.pool.used == 3u);

    /* Capacity 0 hands out nothing and does not advance. */
    ta_chunk_source state;
    ta_chunk_source_init(&state, &chunks, chain);
    ta_byte_source source = ta_chunk_source_as_byte_source(&state);
    ta_chunk_reader start = state.reader;
    uint32_t got = 0xFFFFFFFFu;
    CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = NULL, .capacity = 0, .got = &got }) ==
          TA_OP_OK);
    CHECK(got == 0 && memcmp(&state.reader, &start, sizeof start) == 0);
    uint8_t one = 0;
    CHECK(source.v->read(source.self, &(ta_byte_read_params){ .buffer = &one, .capacity = 1, .got = &got }) ==
          TA_OP_OK);
    CHECK(got == 1 && one == pattern(0));

    /* An empty chain has ended from its first read. */
    uint8_t nothing[4];
    CHECK(read_chain(&chunks, (ta_chunk_chain){ 0u, 0u }, 4u, nothing) == 0);
    CHECK(heap.attempts == attempts);
    ta_chunks_free(&chunks, &chain);
    ta_chunks_destroy(&chunks);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 12: a unit round-trips from a chunk sink through a chunk source.
 */
static void test_chunk_round_trip(void) {
    for (size_t size = 0; size < sizeof chunk_sizes / sizeof chunk_sizes[0]; size += 1u) {
        test_allocator heap = {0};
        ta_chunks chunks = chunk_area(&heap, chunk_sizes[size], 8u);
        uint32_t payload = chunks.payload;
        ta_chunk_sink written;
        ta_chunk_sink_init(&written, &chunks);
        ta_byte_sink sink = ta_chunk_sink_as_byte_sink(&written);
        for (uint32_t length = 0; length <= 3u * payload + 2u; length += 1u) {
            uint32_t within = length % payload;
            if (within > 2u && within < payload - 2u) {
                continue;
            }
            CHECK(sink.v->begin(sink.self) == TA_OP_OK);
            for (uint32_t at = 0; at < length; at += 13u) {
                CHECK(write_pattern(sink, length - at < 13u ? length - at : 13u, at) == TA_OP_OK);
            }
            sink.v->end(sink.self, true);
            ta_chunk_chain chain = written.writer.chain;
            CHECK(chain.length == length);
            uint8_t out[1024];
            CHECK(read_chain(&chunks, chain, 50u, out) == length && holds_pattern(out, length));
            ta_chunks_free(&chunks, &chain);
            CHECK(chunks.pool.used == 0);
        }
        ta_chunks_destroy(&chunks);
        CHECK(test_allocator_finish(&heap) == 0);
    }
}

int main(void) {
    test_empty_sink();
    test_pieces_in_order();
    test_growth();
    test_discard_keeps_run();
    test_failed_write();
    test_size_overflow();
    test_memory_source();
    test_round_trip();
    test_chunk_sink();
    test_chunk_sink_full();
    test_chunk_source();
    test_chunk_round_trip();
    if (failures != 0) {
        fprintf(stderr, "bytes_test: %d checks failed\n", failures);
        return 1;
    }
    printf("bytes_test: ok\n");
    return 0;
}
