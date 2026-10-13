/* Stable allocation addresses, nested marks, retained chunks and writer use. */
#include <tinyactors/arena.h>
#include <tinyactors/cbor.h>
#include "../support/counting_allocator.h"

static void test_nested_marks_and_oversized(void) {
    counting_allocator allocator;
    counting_init(&allocator);
    ta_growable_arena_config config = {sizeof config, 0, 32, &allocator.api};
    ta_growable_arena arena;
    ta_growable_arena_init(&arena, &config);
    ta_growable_arena_mark empty = ta_growable_arena_position(&arena);
    CHECK(empty.chunk == NULL && empty.offset == 0);
    unsigned char *outer = ta_growable_arena_alloc(&arena, 8, 8);
    memset(outer, 0x5a, 8);
    ta_growable_arena_mark nested = ta_growable_arena_position(&arena);
    CHECK(ta_growable_arena_alloc(&arena, 3, 4) != NULL); /* same-chunk nested scope */
    unsigned char *big = ta_growable_arena_alloc(&arena, 100, 8);
    memset(big, 0x6b, 100);
    ta_growable_arena_mark inner = ta_growable_arena_position(&arena);
    CHECK(ta_growable_arena_alloc(&arena, 24, 8) != NULL);
    ta_growable_arena_release(&arena, inner);
    for (uint32_t i = 0; i < 100; ++i) CHECK_EQUAL(big[i], 0x6b);
    ta_growable_arena_release(&arena, nested);
    CHECK_EQUAL(arena.used, 8);
    for (uint32_t i = 0; i < 8; ++i) CHECK_EQUAL(outer[i], 0x5a);
    size_t reserved = arena.reserved;
    uint32_t allocations = allocator.allocations;
    allocator.refuse = 1;
    CHECK(ta_growable_arena_alloc(&arena, 100, 8) == big);
    CHECK(ta_growable_arena_alloc(&arena, 24, 8) != NULL);
    CHECK_EQUAL(allocator.allocations, allocations);
    CHECK_EQUAL(arena.reserved, reserved);
    ta_growable_arena_release(&arena, empty);
    CHECK_EQUAL(arena.used, 0);
    CHECK_EQUAL(allocator.frees, 0);
    ta_growable_arena_free(&arena);
    CHECK_EQUAL(arena.reserved, 0);
    CHECK_EQUAL(allocator.live_bytes, 0);
    CHECK_EQUAL(allocator.allocations, allocator.frees);
    ta_growable_arena_free(&arena);
}

static void test_warm_reuse_and_alignment(void) {
    counting_allocator allocator;
    counting_init(&allocator);
    ta_growable_arena_config config = {sizeof config, 0, 32, &allocator.api};
    ta_growable_arena arena;
    ta_growable_arena_init(&arena, &config);
    const size_t sizes[] = {1, 7, 31, 80, 8, 45, 16};
    const size_t alignments[] = {1, 2, 4, 8, 8, 1, 4};
    for (unsigned round = 0; round < 3; ++round) {
        ta_growable_arena_mark mark = ta_growable_arena_position(&arena);
        uint32_t attempts = allocator.attempts;
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
            void *pointer = ta_growable_arena_alloc(&arena, sizes[i], alignments[i]);
            CHECK(pointer != NULL && ((uintptr_t)pointer & (alignments[i] - 1u)) == 0);
        }
        if (round) CHECK_EQUAL(allocator.attempts, attempts);
        ta_growable_arena_release(&arena, mark);
        allocator.refuse = 1;
    }
    ta_growable_arena_free(&arena);
    CHECK_EQUAL(allocator.live_bytes, 0);
}

static void test_refusal_and_bad_marks(void) {
    counting_allocator allocator;
    counting_init(&allocator);
    ta_growable_arena_config config = {sizeof config, 0, 32, &allocator.api};
    ta_growable_arena arena;
    ta_growable_arena_init(&arena, &config);
    CHECK(ta_growable_arena_alloc(&arena, 8, 8) != NULL);
    ta_growable_arena_mark mark = ta_growable_arena_position(&arena);
    size_t used = arena.used, reserved = arena.reserved;
    uint32_t attempts = allocator.attempts;
    CHECK(ta_growable_arena_alloc(&arena, 0, 8) == NULL);
    CHECK(ta_growable_arena_alloc(&arena, 1, 0) == NULL);
    CHECK(ta_growable_arena_alloc(&arena, 1, 3) == NULL);
    CHECK(ta_growable_arena_alloc(&arena, 1, 16) == NULL);
    CHECK(ta_growable_arena_alloc(&arena, SIZE_MAX, 8) == NULL);
    CHECK_EQUAL(allocator.attempts, attempts);
    /* White-box accounting boundary, restored before the real free. */
    arena.reserved = SIZE_MAX;
    CHECK(ta_growable_arena_alloc(&arena, 100, 8) == NULL);
    CHECK_EQUAL(allocator.attempts, attempts);
    arena.reserved = reserved;
    allocator.refuse = 1;
    CHECK(ta_growable_arena_alloc(&arena, 100, 8) == NULL);
    CHECK_EQUAL(arena.used, used);
    CHECK_EQUAL(arena.reserved, reserved);
    CHECK(ta_growable_arena_position(&arena).chunk == mark.chunk);
    ta_growable_arena_release(&arena, (ta_growable_arena_mark){NULL, 1});
    ta_growable_arena_release(&arena, (ta_growable_arena_mark){mark.chunk, mark.offset + 1});
    ta_growable_arena_release(&arena, (ta_growable_arena_mark){(ta_growable_arena_chunk *)(void *)&allocator, 0});
    CHECK_EQUAL(arena.used, used);
    CHECK_EQUAL(arena.reserved, reserved);
    CHECK(ta_growable_arena_alloc(&arena, 8, 8) != NULL); /* failed growth is not sticky */
    ta_growable_arena_free(&arena);
    CHECK_EQUAL(allocator.live_bytes, 0);
    ta_growable_arena_init(&arena, NULL);
    CHECK(ta_growable_arena_alloc(&arena, 8, 8) == NULL);
    ta_growable_arena_free(&arena);
    config.chunk_size = SIZE_MAX;
    ta_growable_arena_init(&arena, &config);
    attempts = allocator.attempts;
    CHECK(ta_growable_arena_alloc(&arena, 1, 8) == NULL);
    CHECK_EQUAL(allocator.attempts, attempts);
    CHECK_EQUAL(arena.used, 0);
    CHECK_EQUAL(arena.reserved, 0);
    ta_growable_arena_free(&arena);
    config.size = 0;
    ta_growable_arena_init(&arena, &config);
    CHECK(ta_growable_arena_alloc(&arena, 1, 8) == NULL);
}

static void test_writer_allocator_view(void) {
    counting_allocator allocator;
    counting_init(&allocator);
    ta_growable_arena_config config = {sizeof config, 0, 32, &allocator.api};
    ta_growable_arena arena;
    ta_growable_arena_init(&arena, &config);
    ta_allocator view = ta_growable_arena_allocator(&arena);
    char text[1024];
    memset(text, 'x', sizeof text);
    for (unsigned round = 0; round < 2; ++round) {
        ta_growable_arena_mark mark = ta_growable_arena_position(&arena);
        uint32_t attempts = allocator.attempts;
        ta_cbor_writer writer;
        ta_cbor_writer_init(&writer, (ta_buffer){0}, &view);
        ta_cbor_write_array(&writer, 2);
        ta_cbor_write_text(&writer, (ta_string_view){text, sizeof text});
        ta_cbor_write_text(&writer, (ta_string_view){text, sizeof text});
        ta_bytes wire;
        CHECK_EQUAL(ta_cbor_writer_bytes(&writer, &wire), TA_OK);
        CHECK(wire.length > sizeof text * 2);
        ta_cbor_reader reader;
        ta_cbor_item item;
        ta_cbor_reader_init(&reader, wire);
        CHECK_EQUAL(ta_cbor_read(&reader, &item), 1);
        CHECK(item.kind == TA_CBOR_ARRAY && item.count == 2);
        for (unsigned i = 0; i < 2; ++i) {
            CHECK_EQUAL(ta_cbor_read(&reader, &item), 1);
            CHECK(item.kind == TA_CBOR_TEXT && item.bytes.length == sizeof text);
            CHECK(memcmp(item.bytes.pointer, text, sizeof text) == 0);
        }
        CHECK_EQUAL(ta_cbor_read(&reader, &item), 0);
        ta_cbor_writer_free(&writer); /* view's free retains backing chunks */
        CHECK_EQUAL(allocator.frees, 0);
        if (round) CHECK_EQUAL(allocator.attempts, attempts);
        ta_growable_arena_release(&arena, mark);
        allocator.refuse = 1;
    }
    ta_growable_arena_free(&arena);
    CHECK_EQUAL(allocator.live_bytes, 0);
}

int main(void) {
    RUN(test_nested_marks_and_oversized);
    RUN(test_warm_reuse_and_alignment);
    RUN(test_refusal_and_bad_marks);
    RUN(test_writer_allocator_view);
    TEST_MAIN_END();
}
