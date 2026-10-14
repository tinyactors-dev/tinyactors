/* Public table lifetime, growth refusal and generation exhaustion. */
#include <tinyactors/table.h>
#include "../support/counting_allocator.h"

static void test_handles_and_growth(void) {
    counting_allocator allocator;
    counting_init(&allocator);
    ta_table_config config = {sizeof config, 9, &allocator.api};
    ta_table table;
    ta_table_init(&table, &config);
    CHECK_EQUAL(allocator.attempts, 0);
    CHECK_EQUAL(table.row_size, 16);
    uint64_t handles[33];
    for (uint32_t i = 0; i < 33; ++i) {
        unsigned char *row = ta_table_add(&table, &handles[i]);
        CHECK(row != NULL && ((uintptr_t)row & 7u) == 0);
        for (uint32_t byte = 0; byte < table.row_size; ++byte) CHECK_EQUAL(row[byte], 0);
        *(uint32_t *)(void *)row = i + 1;
    }
    CHECK_EQUAL(table.count, 33);
    for (uint32_t i = 0; i < 33; ++i) {
        CHECK_EQUAL(*(uint32_t *)ta_table_get(&table, handles[i]), i + 1);
        CHECK_EQUAL(ta_table_handle(&table, i), handles[i]);
        CHECK(ta_table_at(&table, i) == ta_table_get(&table, handles[i]));
    }
    CHECK(ta_table_at(&table, table.capacity) == NULL);
    CHECK_EQUAL(ta_table_handle(&table, 40), 0);
    CHECK(ta_table_get(&table, 0) == NULL);
    CHECK(ta_table_get(&table, UINT64_MAX) == NULL);
    CHECK(ta_table_get(&table, handles[0] ^ (UINT64_C(2) << 32)) == NULL);
    void *other = ta_table_get(&table, handles[1]);
    ta_table_remove(&table, handles[0]);
    ta_table_remove(&table, handles[0]); /* stale removal is inert */
    CHECK_EQUAL(table.count, 32);
    CHECK(ta_table_get(&table, handles[0]) == NULL);
    CHECK(ta_table_at(&table, 0) == NULL);
    CHECK(ta_table_get(&table, handles[1]) == other);
    uint64_t reused;
    unsigned char *row = ta_table_add(&table, &reused);
    CHECK((uint32_t)reused == (uint32_t)handles[0] && reused != handles[0]);
    for (uint32_t byte = 0; byte < table.row_size; ++byte) CHECK_EQUAL(row[byte], 0);
    CHECK(ta_table_get(&table, handles[0]) == NULL);
    ta_table_free(&table);
    ta_table_free(&table);
    CHECK_EQUAL(allocator.live_bytes, 0);
    CHECK_EQUAL(allocator.allocations, allocator.frees);
    CHECK(ta_table_get(&table, reused) == NULL);
}

static void test_refused_growth(void) {
    counting_allocator allocator;
    counting_init(&allocator);
    ta_table_config config = {sizeof config, sizeof(uint64_t), &allocator.api};
    ta_table table;
    ta_table_init(&table, &config);
    allocator.refuse = 1;
    uint64_t handle = 99;
    CHECK(ta_table_add(&table, &handle) == NULL);
    CHECK_EQUAL(handle, 0);
    CHECK_EQUAL(table.capacity, 0);
    allocator.refuse = 0;
    uint64_t first;
    CHECK(ta_table_add(&table, &first) != NULL);
    uint32_t capacity = table.capacity;
    for (uint32_t i = 1; i < capacity; ++i) CHECK(ta_table_add(&table, &handle) != NULL);
    unsigned char *rows = table.rows;
    *(uint64_t *)ta_table_get(&table, first) = 42;
    allocator.refuse = 1;
    CHECK(ta_table_add(&table, &handle) == NULL);
    CHECK_EQUAL(handle, 0);
    CHECK(table.rows == rows && table.capacity == capacity && table.count == capacity);
    CHECK_EQUAL(*(uint64_t *)ta_table_get(&table, first), 42);
    allocator.refuse = 0;
    CHECK(ta_table_add(&table, &handle) != NULL);
    CHECK_EQUAL(*(uint64_t *)ta_table_get(&table, first), 42);
    ta_table_free(&table);
    CHECK_EQUAL(allocator.live_bytes, 0);
}

static void test_generation_retirement(void) {
    counting_allocator allocator;
    counting_init(&allocator);
    ta_table_config config = {sizeof config, 8, &allocator.api};
    ta_table table;
    ta_table_init(&table, &config);
    uint64_t handle;
    CHECK(ta_table_add(&table, &handle) != NULL);
    uint64_t oldest = handle;
    /* White-box setup reaches the last two live generations without a
     * billion-iteration sweep. Slot metadata starts after the aligned rows;
     * its first uint32_t is the generation (src/base/table.c). */
    uint32_t *generation = (uint32_t *)(void *)(table.rows + (size_t)table.row_size * table.capacity);
    *generation = UINT32_MAX - 2u;
    uint64_t penultimate = (uint64_t)(UINT32_MAX - 2u) << 32 | 1u;
    ta_table_remove(&table, penultimate);
    CHECK(ta_table_add(&table, &handle) != NULL);
    CHECK_EQUAL(handle >> 32, UINT32_MAX);
    ta_table_remove(&table, handle);
    CHECK(ta_table_get(&table, handle) == NULL);
    CHECK(ta_table_add(&table, &handle) != NULL);
    CHECK((uint32_t)handle != 1u);
    CHECK(ta_table_at(&table, 0) == NULL);
    CHECK(ta_table_get(&table, penultimate) == NULL);
    uint32_t capacity = table.capacity;
    for (uint32_t i = 0; i < capacity; ++i) CHECK(ta_table_add(&table, &handle) != NULL);
    CHECK(table.capacity > capacity);
    CHECK_EQUAL(ta_table_handle(&table, 0), 0);
    CHECK(ta_table_get(&table, oldest) == NULL); /* retirement survives growth */
    ta_table_free(&table);
    CHECK_EQUAL(allocator.live_bytes, 0);
}

static void test_invalid_config_and_arguments(void) {
    counting_allocator allocator;
    counting_init(&allocator);
    ta_table table;
    ta_table_config config = {sizeof config, UINT32_MAX, &allocator.api};
    uint64_t handle;
    ta_table_init(&table, &config);
    CHECK(ta_table_add(&table, &handle) == NULL);
    config.row_size = 0;
    ta_table_init(&table, &config);
    CHECK(ta_table_add(&table, &handle) == NULL);
    config.row_size = 8;
    config.size = 0;
    ta_table_init(&table, &config);
    CHECK(ta_table_add(&table, &handle) == NULL);
    ta_table_init(&table, NULL);
    CHECK(ta_table_add(&table, &handle) == NULL);
    config.size = sizeof config;
    ta_table_init(&table, &config);
    CHECK(ta_table_add(&table, NULL) == NULL);
    CHECK_EQUAL(allocator.attempts, 0);
    /* White-box arithmetic boundary: doubling cannot encode another slot.
     * No fake storage is dereferenced because growth refuses first. */
    table.capacity = UINT32_MAX;
    CHECK(ta_table_add(&table, &handle) == NULL);
    CHECK_EQUAL(allocator.attempts, 0);
    table.capacity = 0;
    config.allocator = NULL;
    ta_table_init(&table, &config);
    CHECK(ta_table_add(&table, &handle) == NULL);
    ta_table_free(&table);
}

/* The handle namespace saved and restored: live handles live again with
 * zeroed rows, removed ones stale, and later adds hand out what the
 * original table would have. */
static void test_snapshot_and_restore(void) {
    counting_allocator allocator;
    counting_init(&allocator);
    ta_table_config config = {sizeof config, 12, &allocator.api};
    ta_table table, again;
    ta_table_init(&table, &config);
    uint64_t handles[40];
    for (uint32_t i = 0; i < 40; ++i) *(uint32_t *)ta_table_add(&table, &handles[i]) = i + 1;
    for (uint32_t i = 0; i < 40; i += 3) ta_table_remove(&table, handles[i]);
    unsigned char bytes[2048];
    ta_buffer small = {bytes, 8, 0};
    CHECK_EQUAL(ta_table_snapshot(&table, &small), TA_ERROR_NO_MEMORY);
    CHECK(small.length > 8 && small.length <= sizeof bytes);
    ta_buffer out = {bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_table_snapshot(&table, &out), TA_OK);
    CHECK_EQUAL(out.length, small.length);
    ta_bytes saved = {bytes, out.length};
    ta_table_init(&again, &config);
    allocator.refuse = 1;
    CHECK_EQUAL(ta_table_restore(&again, saved), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(again.capacity, 0);
    allocator.refuse = 0;
    CHECK_EQUAL(ta_table_restore(&again, saved), TA_OK);
    CHECK_EQUAL(again.count, table.count);
    CHECK_EQUAL(ta_table_restore(&again, saved), TA_ERROR_STATE); /* not empty */
    for (uint32_t i = 0; i < 40; ++i) {
        uint32_t *row = ta_table_get(&again, handles[i]);
        if (i % 3 == 0) {
            CHECK(row == NULL);
        } else {
            CHECK(row != NULL && *row == 0);
        }
    }
    for (uint32_t i = 0; i < 20; ++i) {
        uint64_t a, b;
        CHECK(ta_table_add(&table, &a) != NULL);
        CHECK(ta_table_add(&again, &b) != NULL);
        CHECK_EQUAL(a, b);
    }
    /* bytes that are not a namespace, or one of another row size */
    ta_table other;
    ta_table_config wider = {sizeof wider, 24, &allocator.api};
    ta_table_init(&other, &wider);
    CHECK_EQUAL(ta_table_restore(&other, saved), TA_ERROR_INVALID);
    bytes[0] ^= 1;
    ta_table_init(&other, &config);
    CHECK_EQUAL(ta_table_restore(&other, saved), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_table_restore(&other, (ta_bytes){bytes, 3}), TA_ERROR_INVALID);
    bytes[0] ^= 1;
    uint32_t loop[4 + 2 * 2] = {0x54415442u, 16, 2, 1, 0, 2, 0, 1}; /* a free list that cycles */
    CHECK_EQUAL(ta_table_restore(&other, (ta_bytes){loop, sizeof loop}), TA_ERROR_INVALID);
    ta_table empty, empty_again;
    ta_table_init(&empty, &config);
    ta_table_init(&empty_again, &config);
    out = (ta_buffer){bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_table_snapshot(&empty, &out), TA_OK);
    CHECK_EQUAL(ta_table_restore(&empty_again, (ta_bytes){bytes, out.length}), TA_OK);
    CHECK_EQUAL(empty_again.capacity, 0);
    ta_table_free(&table);
    ta_table_free(&again);
    CHECK_EQUAL(allocator.live_bytes, 0);
}

int main(void) {
    RUN(test_handles_and_growth);
    RUN(test_refused_growth);
    RUN(test_snapshot_and_restore);
    RUN(test_generation_retirement);
    RUN(test_invalid_config_and_arguments);
    TEST_MAIN_END();
}
