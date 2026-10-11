/* Unit tests for ta_base building blocks. */
#include <string.h>

#include "../harness/harness.h"
#include "base/arena.h"
#include "base/bitset.h"
#include "base/format.h"
#include "base/heap.h"
#include "base/json.h"
#include "base/index_queue.h"
#include "base/memory.h"
#include "base/pool.h"
#include "base/slab.h"

/* A "carve" function used in both measure and real mode, mirroring how
 * ta_system_layout()/ta_system_init() share one code path. */
typedef struct region {
    ta_pool pool;
    ta_heap heap;
} region;

static size_t carve(ta_arena *arena, uint32_t capacity, region **out) {
    size_t header = ta_arena_alloc(arena, sizeof(region), TA_ALIGN_MAX);
    size_t slots = ta_arena_alloc(arena, ta_pool_slots_bytes(24, capacity), TA_ALIGN_MAX);
    size_t generations = ta_arena_alloc(arena, ta_pool_generations_bytes(capacity), 1);
    size_t nodes = ta_arena_alloc(arena, ta_heap_nodes_bytes(capacity), TA_ALIGN_MAX);
    size_t position = ta_arena_alloc(arena, ta_heap_position_bytes(capacity), 4);
    if (arena->base && !arena->failed) {
        region *r = (region *)ta_arena_pointer(arena, header);
        ta_pool_init(&r->pool, ta_arena_pointer(arena, slots),
                     (unsigned char *)ta_arena_pointer(arena, generations), 24, capacity);
        ta_heap_init(&r->heap, (ta_heap_node *)ta_arena_pointer(arena, nodes),
                     (uint32_t *)ta_arena_pointer(arena, position), capacity);
        *out = r;
    }
    return ta_arena_required(arena);
}

static void test_arena_measure_matches_init(void) {
    ta_arena arena;
    region *r = NULL;
    ta_arena_init_measure(&arena);
    size_t need = carve(&arena, 100, &r);
    CHECK(!arena.failed);

    static unsigned char buffer[8192];
    /* deliberately misaligned base: the measured slack must cover it */
    ta_arena_init(&arena, buffer + 3, need);
    carve(&arena, 100, &r);
    CHECK(!arena.failed);
    CHECK(r != NULL);

    ta_arena_init(&arena, buffer + 3, need - 8);
    carve(&arena, 100, &r);
    CHECK(arena.failed);
}

static void test_arena_mark_reset(void) {
    static unsigned char buffer[256];
    ta_arena arena;
    ta_arena_init(&arena, buffer, sizeof buffer);
    size_t m = ta_arena_mark(&arena);
    ta_arena_alloc(&arena, 100, 8);
    ta_arena_reset(&arena, m);
    CHECK_EQUAL(ta_arena_alloc(&arena, 8, 8), 0);
    ta_arena_alloc(&arena, 1000, 8);
    CHECK(arena.failed);
}

static void test_freed_storage_is_zeroed(void) {
    enum { N = 8 };
    static const unsigned char zeros[TA_SLAB_PAGE];
    /* pool: a freed slot keeps only its free-list link */
    static unsigned char slots[N * 16];
    static unsigned char generations[N];
    memset(slots, 0xA5, sizeof slots);
    ta_pool pool;
    ta_pool_init(&pool, slots, generations, 12, N);
    uint32_t i = ta_pool_alloc(&pool);
    memset((unsigned char *)ta_pool_at(&pool, i) + 4, 0x5C, 12);
    ta_pool_free(&pool, i);
    CHECK(memcmp(slots + 4, zeros, 12) == 0); /* payload zeroed on free */
    CHECK(memcmp(slots, zeros, 4) != 0);      /* the link survives */

    /* slab: a freed block keeps only its 2-byte free-slot link */
    static unsigned long long pages_memory[TA_SLAB_PAGE / 8];
    static ta_slab_page metadata[1];
    ta_slab slab;
    ta_slab_init(&slab, pages_memory, metadata, 1);
    uint32_t offset = ta_slab_alloc(&slab, 100);
    memset(ta_slab_pointer(&slab, offset), 0x5C, 100);
    ta_slab_free(&slab, offset);
    unsigned char *blk = (unsigned char *)ta_slab_pointer(&slab, offset);
    CHECK(memcmp(blk + 2, zeros, 14) == 0); /* 16-byte class */

    /* heap: a popped or removed node leaves no bytes behind */
    static ta_heap_node nodes[N];
    static uint32_t position[N];
    memset(nodes, 0xA5, sizeof nodes);
    ta_heap heap;
    ta_heap_init(&heap, nodes, position, N);
    for (uint32_t k = 0; k < 4; k++)
        CHECK_EQUAL(ta_heap_push(&heap, 100 - k, k, k), TA_OK);
    CHECK(ta_heap_remove(&heap, 1));
    CHECK_EQUAL(ta_heap_pop(&heap), 3u);
    CHECK_EQUAL(ta_heap_pop(&heap), 2u);
    CHECK_EQUAL(ta_heap_pop(&heap), 0u);
    for (uint32_t k = 0; k < 4; k++) {
        ta_heap_node z = {0, 0, 0};
        CHECK(memcmp(&nodes[k], &z, sizeof z) == 0);
    }
}

static void test_pool_handles_and_reuse(void) {
    static unsigned char slots[16 * 4];
    static unsigned char generations[4];
    ta_pool pool;
    ta_pool_init(&pool, slots, generations, 12, 4);
    CHECK_EQUAL(pool.stride, 16);

    uint32_t a = ta_pool_alloc(&pool), b = ta_pool_alloc(&pool);
    CHECK_EQUAL(a, 0);
    CHECK_EQUAL(b, 1);
    uint32_t ha = ta_pool_handle(&pool, a);
    CHECK(ha != 0);
    CHECK_EQUAL(ta_pool_resolve(&pool, ha), a);
    CHECK_EQUAL(ta_pool_resolve(&pool, 0), TA_NIL);

    ta_pool_free(&pool, a);
    CHECK_EQUAL(ta_pool_resolve(&pool, ha), TA_NIL); /* stale */
    uint32_t c = ta_pool_alloc(&pool);
    CHECK_EQUAL(c, a);                     /* reused slot ... */
    CHECK(ta_pool_handle(&pool, c) != ha); /* ... with a new generation */

    CHECK(ta_pool_alloc(&pool) != TA_NIL);
    CHECK(ta_pool_alloc(&pool) != TA_NIL);
    CHECK_EQUAL(ta_pool_alloc(&pool), TA_NIL); /* exhausted */
    CHECK_EQUAL(pool.used, 4);
}

static void test_pool_relocatable(void) {
    /* Pool header + storage in one region; memcpy the region and use the copy. */
    static unsigned char r1[256], r2[256];
    ta_arena arena;
    region *r = NULL;
    ta_arena_init(&arena, r1, sizeof r1);
    carve(&arena, 4, &r);
    CHECK(!arena.failed);
    uint32_t i = ta_pool_alloc(&r->pool);
    *(uint32_t *)ta_pool_at(&r->pool, i) = 0xC0FFEE;
    uint32_t h = ta_pool_handle(&r->pool, i);

    memcpy(r2, r1, sizeof r1);
    region *moved = (region *)(void *)(r2 + ((unsigned char *)r - r1));
    CHECK_EQUAL(ta_pool_resolve(&moved->pool, h), i);
    CHECK_EQUAL(*(uint32_t *)ta_pool_at(&moved->pool, i), 0xC0FFEE);
    CHECK((unsigned char *)ta_pool_at(&moved->pool, i) >= r2);
}

static void test_bitset(void) {
    ta_bitset_word b[3] = {0};
    uint32_t n = 70;
    ta_bitset_set(b, 0);
    ta_bitset_set(b, 31);
    ta_bitset_set(b, 32);
    ta_bitset_set(b, 69);
    CHECK_EQUAL(ta_bitset_count(b, 3), 4);
    CHECK_EQUAL(ta_bitset_next(b, n, 0), 0);
    CHECK_EQUAL(ta_bitset_next(b, n, 1), 31);
    CHECK_EQUAL(ta_bitset_next(b, n, 33), 69);
    CHECK_EQUAL(ta_bitset_next(b, n, 70), TA_NIL);
    CHECK_EQUAL(ta_bitset_previous(b, 69), 69);
    CHECK_EQUAL(ta_bitset_previous(b, 68), 32);
    CHECK_EQUAL(ta_bitset_previous(b, 30), 0);
    ta_bitset_clear(b, 0);
    CHECK_EQUAL(ta_bitset_previous(b, 30), TA_NIL);

    /* exit set = config ∩ (domain, domain.end) */
    ta_bitset_word ex[3];
    ta_bitset_and_range(ex, b, 3, 31, 33);
    CHECK_EQUAL(ta_bitset_count(ex, 3), 2);
    CHECK(ta_bitset_any_in_range(b, 33, 70));
    CHECK(!ta_bitset_any_in_range(b, 33, 69));
}

static void test_index_queue(void) {
    static unsigned char slots[16 * 8];
    static unsigned char generations[8];
    ta_pool pool;
    ta_pool_init(&pool, slots, generations, 16, 8);
    ta_index_queue queue;
    ta_index_queue_init(&queue);
    CHECK(ta_index_queue_empty(&queue));
    for (int k = 0; k < 5; k++)
        ta_index_queue_push(&queue, &pool, 4, ta_pool_alloc(&pool));
    for (uint32_t k = 0; k < 5; k++)
        CHECK_EQUAL(ta_index_queue_pop(&queue, &pool, 4), k);
    CHECK_EQUAL(ta_index_queue_pop(&queue, &pool, 4), TA_NIL);
    CHECK(ta_index_queue_empty(&queue));
}

static void test_heap(void) {
    enum { N = 64 };
    static ta_heap_node nodes[N];
    static uint32_t position[N];
    ta_heap heap;
    ta_heap_init(&heap, nodes, position, N);
    /* pseudo-random dues, with ties broken by seq */
    uint32_t x = 12345;
    for (uint32_t i = 0; i < N; i++) {
        x = x * 1103515245u + 12345u;
        CHECK_EQUAL(ta_heap_push(&heap, (x >> 16) % 16, i, i), TA_OK);
    }
    CHECK_EQUAL(ta_heap_push(&heap, 0, 0, 3), TA_ERROR_INVALID); /* already queued */
    CHECK(ta_heap_remove(&heap, 7));
    CHECK(!ta_heap_remove(&heap, 7));
    CHECK(!ta_heap_contains(&heap, 7));
    CHECK(ta_heap_find(&heap, 7) == NULL);
    CHECK(ta_heap_find(&heap, N) == NULL);
    x = 12345;
    for (uint32_t i = 0; i < N; i++) { /* every queued item reports its own due/seq */
        x = x * 1103515245u + 12345u;
        if (i == 7) continue;
        const ta_heap_node *f = ta_heap_find(&heap, i);
        CHECK(f && f->item == i && f->sequence == i && f->due == (x >> 16) % 16);
    }

    uint64_t last_due = 0;
    uint32_t last_seq = 0, count = 0;
    int first = 1;
    while (ta_heap_top(&heap)) {
        ta_heap_node t = *ta_heap_top(&heap);
        CHECK_EQUAL(ta_heap_pop(&heap), t.item);
        if (!first) CHECK(t.due > last_due || (t.due == last_due && t.sequence > last_seq));
        CHECK(t.item != 7);
        last_due = t.due;
        last_seq = t.sequence;
        first = 0;
        count++;
    }
    CHECK_EQUAL(count, N - 1);
}

static void test_slab(void) {
    enum { NP = 4 };
    static unsigned long long pages_memory[NP * TA_SLAB_PAGE / 8];
    static ta_slab_page metadata[NP];
    ta_slab slab;
    ta_slab_init(&slab, pages_memory, metadata, NP);
    CHECK_EQUAL(ta_slab_alloc(&slab, 0), TA_NIL);
    CHECK_EQUAL(ta_slab_alloc(&slab, TA_SLAB_PAGE + 1), TA_NIL);

    uint32_t a = ta_slab_alloc(&slab, 10), b = ta_slab_alloc(&slab, 16),
             c = ta_slab_alloc(&slab, 17);
    CHECK(a != TA_NIL && b != TA_NIL && c != TA_NIL);
    CHECK_EQUAL(ta_slab_size_of(&slab, a), 16);
    CHECK_EQUAL(ta_slab_size_of(&slab, c), 24);
    CHECK_EQUAL(a / TA_SLAB_PAGE, b / TA_SLAB_PAGE); /* same class shares a page */
    CHECK(a / TA_SLAB_PAGE != c / TA_SLAB_PAGE);     /* different class, different page */
    CHECK_EQUAL(slab.used_pages, 2);
    memset(ta_slab_pointer(&slab, b), 0xAB, 16);
    ta_slab_free(&slab, a);
    CHECK_EQUAL(ta_slab_alloc(&slab, 8), a); /* slot reuse */
    ta_slab_free(&slab, a);
    ta_slab_free(&slab, b);
    CHECK_EQUAL(slab.used_pages, 1); /* empty page returned */

    /* fill a whole page of 4096-byte objects: each takes one page */
    uint32_t big[NP];
    for (int i = 0; i < NP - 1; i++)
        CHECK((big[i] = ta_slab_alloc(&slab, 4096)) != TA_NIL);
    CHECK_EQUAL(ta_slab_alloc(&slab, 4096), TA_NIL);
    ta_slab_free(&slab, c);
    big[NP - 1] = ta_slab_alloc(&slab, 4096);
    CHECK(big[NP - 1] != TA_NIL);
    for (int i = 0; i < NP; i++)
        ta_slab_free(&slab, big[i]);
    CHECK_EQUAL(slab.used_pages, 0);

    /* 40-byte objects use the 48-byte class: 85 per page, 340 in 4 pages */
    static uint32_t objs[340];
    for (int i = 0; i < 340; i++)
        CHECK((objs[i] = ta_slab_alloc(&slab, 40)) != TA_NIL);
    CHECK_EQUAL(slab.used_pages, NP);
    CHECK_EQUAL(ta_slab_alloc(&slab, 40), TA_NIL);
    CHECK_EQUAL(ta_slab_size_of(&slab, objs[0]), 48);
    for (int i = 0; i < 340; i++)
        ta_slab_free(&slab, objs[i]);
    CHECK_EQUAL(slab.used_pages, 0);
}

/* ta_format_u64: every length, the extremes; no terminator written. */
static void test_format_u64(void) {
    char buffer[TA_FORMAT_U64_MAX + 1];
    memset(buffer, '#', sizeof buffer);
    CHECK_EQUAL(ta_format_u64(buffer, 0), 1u);
    CHECK(buffer[0] == '0' && buffer[1] == '#');
    CHECK_EQUAL(ta_format_u64(buffer, UINT64_MAX), TA_FORMAT_U64_MAX);
    CHECK(memcmp(buffer, "18446744073709551615", 20) == 0 && buffer[20] == '#');
    uint64_t value = 1;
    for (uint32_t digits = 1; digits <= 19; digits++, value *= 10) {
        CHECK_EQUAL(ta_format_u64(buffer, value), digits);
        CHECK_EQUAL(ta_format_u64(buffer, value * 10 - 1), digits);
    }
}

/* ta_text: views and numbers up to the capacity exactly; past it, nothing
 * more is written and the overflow stays set. */
static void test_text(void) {
    char buffer[16];
    memset(buffer, '#', sizeof buffer);
    ta_text text = ta_text_init(buffer, 12);
    ta_text_append(&text, (ta_string_view){"msg/", 4});
    ta_text_append_u64(&text, 4096);
    ta_text_append(&text, (ta_string_view){NULL, 0});
    ta_text_append(&text, (ta_string_view){".7", 2});
    CHECK_EQUAL(text.length, 10u);
    CHECK(!text.overflow && memcmp(buffer, "msg/4096.7", 10) == 0);
    ta_text_append(&text, (ta_string_view){"ab", 2}); /* exactly full */
    CHECK(!text.overflow && text.length == 12);
    ta_text_append(&text, (ta_string_view){"c", 1});
    CHECK(text.overflow && text.length == 12 && buffer[12] == '#');
    ta_text_append(&text, (ta_string_view){NULL, 0}); /* stays overflowed */
    CHECK(text.overflow);
    text = ta_text_init(buffer, 3);
    ta_text_append_u64(&text, 1234); /* a number is written whole or not at all */
    CHECK(text.overflow && text.length == 0);
}

/* ta_memmove: overlapping either way, and the freestanding fallback's
 * direction choice through the same macro. */
static void test_memmove(void) {
    char forward[] = "abcdefgh", backward[] = "abcdefgh";
    ta_memmove(forward + 2, forward, 5);
    CHECK(memcmp(forward, "ababcdeh", 8) == 0);
    ta_memmove(backward, backward + 2, 5);
    CHECK(memcmp(backward, "cdefgfgh", 8) == 0);
}

#define VIEW_OF(text) ((ta_string_view){(text), (uint32_t)strlen(text)})

/* a JSON string for the state value: quotes, backslashes and control
 * characters escaped, measured without writing */
static void test_json(void) {
    char written[32];
    size_t n = ta_json_write_string(written, VIEW_OF("a\"b\\\x01"));
    CHECK(n == 14 && !memcmp(written, "\"a\\\"b\\\\\\u0001\"", 14));
    CHECK_EQUAL(ta_json_write_string(NULL, VIEW_OF("a\"b\\\x01")), n);
}

int main(void) {
    RUN(test_json);
    RUN(test_arena_measure_matches_init);
    RUN(test_arena_mark_reset);
    RUN(test_freed_storage_is_zeroed);
    RUN(test_pool_handles_and_reuse);
    RUN(test_pool_relocatable);
    RUN(test_bitset);
    RUN(test_index_queue);
    RUN(test_heap);
    RUN(test_slab);
    RUN(test_format_u64);
    RUN(test_text);
    RUN(test_memmove);
    TEST_MAIN_END();
}
