/* The host's session map: put, get, remove, iteration, rebuilds that drop
 * removed entries, and a refused reserve that leaves the map as it was. */
#include "host/session_map.h"
#include "../support/counting_allocator.h"

static void test_put_get_remove_and_rebuild(void) {
    counting_allocator allocator;
    counting_init(&allocator);
    session_map map = {0};
    CHECK_EQUAL(session_map_get(&map, 7), TA_NIL);
    session_map_put(&map, 7, TA_NIL); /* removing from an empty map: nothing */
    enum { N = 1000 };
    for (uint64_t k = 1; k <= N; k++) {
        CHECK(session_map_reserve(&map, 1, &allocator.api));
        session_map_put(&map, k << 32 | k, (uint32_t)k);
    }
    CHECK_EQUAL(map.count, N);
    CHECK((uint64_t)map.used * 2 <= map.capacity);
    for (uint64_t k = 1; k <= N; k++)
        CHECK_EQUAL(session_map_get(&map, k << 32 | k), (uint32_t)k);
    CHECK_EQUAL(session_map_get(&map, 5), TA_NIL);
    for (uint64_t k = 1; k <= N; k += 2)
        session_map_put(&map, k << 32 | k, TA_NIL);
    CHECK_EQUAL(map.count, N / 2);
    CHECK_EQUAL(map.used, N); /* removed entries keep their keys */
    session_map_put(&map, 1ull << 32 | 1, 9); /* a removed key back: no new slot */
    CHECK_EQUAL(map.used, N);
    CHECK_EQUAL(session_map_get(&map, 1ull << 32 | 1), 9u);
    session_map_put(&map, 1ull << 32 | 1, TA_NIL);
    uint32_t seen = 0, cursor = 0, value;
    uint64_t key, sum = 0;
    while ((value = session_map_next(&map, &cursor, &key)) != TA_NIL) {
        CHECK_EQUAL(value, (uint32_t)key);
        CHECK(value % 2 == 0);
        sum += value;
        seen++;
    }
    CHECK_EQUAL(seen, N / 2);
    CHECK_EQUAL(sum, (uint64_t)(N / 2) * (N / 2 + 1)); /* 2 + 4 + ... + N */
    /* a reserve past half used rebuilds: the removed entries go, and it shrinks */
    uint32_t capacity = map.capacity;
    CHECK(session_map_reserve(&map, capacity / 2, &allocator.api));
    CHECK_EQUAL(map.used, map.count);
    CHECK_EQUAL(map.count, N / 2);
    for (uint64_t k = 2; k <= N; k += 2)
        CHECK_EQUAL(session_map_get(&map, k << 32 | k), (uint32_t)k);
    session_map_free(&map, &allocator.api);
    session_map_free(&map, &allocator.api);
    CHECK_EQUAL(allocator.live_bytes, 0);
    CHECK_EQUAL(allocator.allocations, allocator.frees);
}

static void test_refused_reserve(void) {
    counting_allocator allocator;
    counting_init(&allocator);
    session_map map = {0};
    CHECK(session_map_reserve(&map, 30, &allocator.api));
    for (uint64_t k = 1; k <= 30; k++)
        session_map_put(&map, k, 1);
    session_map before = map;
    allocator.refuse = 1;
    CHECK(!session_map_reserve(&map, 100, &allocator.api));
    CHECK(map.keys == before.keys && map.capacity == before.capacity && map.count == 30);
    for (uint64_t k = 1; k <= 30; k++)
        CHECK_EQUAL(session_map_get(&map, k), 1u);
    allocator.refuse = 0;
    session_map_free(&map, &allocator.api);
    CHECK_EQUAL(allocator.live_bytes, 0);
}

int main(void) {
    RUN(test_put_get_remove_and_rebuild);
    RUN(test_refused_reserve);
    TEST_MAIN_END();
}
