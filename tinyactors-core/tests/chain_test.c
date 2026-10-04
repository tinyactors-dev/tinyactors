/* test: standalone asan */
/*
 * Block A6 (src/chain.h): the chunk chain.
 *
 * Covers the geometry (payload per chunk, parameter errors), need against
 * the chunks a write really takes, writes of every interesting length at both
 * chunk sizes, appends, the all-or-nothing failure, free, the reader, chains
 * that interleave in one pool, a strict allocator that sees no allocation
 * outside refill, and a short seeded model check. It is fast, not exhaustive:
 * about twenty thousand steps over a few chains.
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

#define MAX_BYTES 2048u

/* Deterministic content: byte i of a chain seeded with `seed`. */
static unsigned char pattern(uint32_t seed, uint32_t i) {
    return (unsigned char)((seed * 131u + i * 7u + (i >> 8) * 13u) & 0xffu);
}

static void fill_pattern(unsigned char *out, uint32_t seed, uint32_t first, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) {
        out[i] = pattern(seed, first + i);
    }
}

static void chunk_pool(ta_pool *pool, uint32_t chunk_size, uint32_t page_chunks, uint32_t max_chunks) {
    ta_chain_pool_params params = { .chunk_size = chunk_size, .page_chunks = page_chunks, .max_chunks = max_chunks };
    CHECK(ta_chain_pool_init(pool, ta_test_allocator_get(&heap), &params) == TA_OK);
}

static ta_pool_counts counts_of(const ta_pool *pool) {
    ta_pool_counts counts;
    ta_pool_counts_get(pool, &counts);
    return counts;
}

/*
 * An oracle that does not use the reader: it follows the links by hand and
 * checks the invariants of chain.h (head/tail/length agree, every chunk but
 * the tail is full, the tail's next is 0) and that the bytes are the pattern.
 * Returns the number of chunks.
 */
static uint32_t verify_chain(const ta_pool *pool, const ta_chain *chain, uint32_t seed) {
    uint32_t payload = ta_chain_payload(pool);
    if (chain->length == 0) {
        CHECK(chain->head == 0 && chain->tail == 0);
        return 0;
    }
    uint32_t chunks = 0;
    uint32_t seen = 0;
    uint32_t index = chain->head;
    uint32_t last = 0;
    while (index != 0 && seen < chain->length) {
        const unsigned char *chunk = ta_pool_at(pool, index);
        uint32_t count = chain->length - seen < payload ? chain->length - seen : payload;
        for (uint32_t i = 0; i < count; i++) {
            if (chunk[4 + i] != pattern(seed, seen + i)) {
                CHECK(!"chain byte differs from the pattern");
                return chunks;
            }
        }
        uint32_t next;
        ta_mem_copy(&next, chunk, sizeof(next));
        seen += count;
        chunks += 1;
        last = index;
        index = next;
    }
    CHECK(seen == chain->length);
    CHECK(index == 0); /* the tail's next is 0 */
    CHECK(last == chain->tail);
    CHECK(chunks == (chain->length + payload - 1u) / payload);
    return chunks;
}

static void finish_clean(void) {
    CHECK(heap.stats.live_blocks == 0);
}

static void test_geometry(void) {
    ta_pool pool;
    ta_chain_pool_params small = { .chunk_size = 4, .page_chunks = 8 };
    ta_chain_pool_params odd = { .chunk_size = 6, .page_chunks = 8 };
    ta_chain_pool_params unaligned = { .chunk_size = 70, .page_chunks = 8 };
    ta_chain_pool_params paging = { .chunk_size = 64, .page_chunks = 6 };
    ta_allocator allocator = ta_test_allocator_get(&heap);
    CHECK(ta_chain_pool_init(&pool, allocator, &small) == TA_ERR_INVALID);
    CHECK(ta_chain_pool_init(&pool, allocator, &odd) == TA_ERR_INVALID);
    CHECK(ta_chain_pool_init(&pool, allocator, &unaligned) == TA_ERR_INVALID);
    CHECK(ta_chain_pool_init(&pool, allocator, &paging) == TA_ERR_INVALID);
    CHECK(ta_chain_pool_init(&pool, allocator, NULL) == TA_ERR_INVALID);
    CHECK(heap.stats.allocations == 0);

    chunk_pool(&pool, 64, 8, 0);
    CHECK(ta_chain_payload(&pool) == 60u);
    CHECK(heap.stats.allocations == 0);
    ta_pool_destroy(&pool);
    chunk_pool(&pool, 256, 8, 0);
    CHECK(ta_chain_payload(&pool) == 252u);
    ta_pool_destroy(&pool);
    chunk_pool(&pool, 8, 8, 0);
    CHECK(ta_chain_payload(&pool) == 4u);
    ta_pool_destroy(&pool);
    finish_clean();
}

static void test_need_arithmetic(void) {
    ta_pool pool;
    chunk_pool(&pool, 64, 8, 0);
    CHECK(ta_chain_need(&pool, NULL, 0) == 0);
    CHECK(ta_chain_need(&pool, NULL, 1) == 1);
    CHECK(ta_chain_need(&pool, NULL, 60) == 1);
    CHECK(ta_chain_need(&pool, NULL, 61) == 2);
    CHECK(ta_chain_need(&pool, NULL, 120) == 2);
    CHECK(ta_chain_need(&pool, NULL, 121) == 3);
    ta_chain empty = { 0 };
    CHECK(ta_chain_need(&pool, &empty, 61) == 2);

    /* A partly filled tail: only the overflow needs chunks. 70 bytes: chunk 2 holds 10, 50 are free. */
    ta_chain partial = { .head = 1, .tail = 2, .length = 70 };
    CHECK(ta_chain_need(&pool, &partial, 0) == 0);
    CHECK(ta_chain_need(&pool, &partial, 50) == 0);
    CHECK(ta_chain_need(&pool, &partial, 51) == 1);
    CHECK(ta_chain_need(&pool, &partial, 110) == 1);
    CHECK(ta_chain_need(&pool, &partial, 111) == 2);
    /* An exactly full tail needs a whole chunk for one more byte. */
    ta_chain full = { .head = 1, .tail = 2, .length = 120 };
    CHECK(ta_chain_need(&pool, &full, 0) == 0);
    CHECK(ta_chain_need(&pool, &full, 1) == 1);
    /* The largest request cannot overflow: ceil(UINT32_MAX / 60). */
    CHECK(ta_chain_need(&pool, NULL, UINT32_MAX) == 71582789u);
    ta_pool_destroy(&pool);
    finish_clean();
}

/* Write `bytes` of the pattern (offset `first`) onto the chain, preflighting as an owner would. */
static ta_result write_pattern(ta_pool *pool, ta_chain *chain, uint32_t seed, uint32_t first, uint32_t bytes) {
    unsigned char buffer[MAX_BYTES];
    fill_pattern(buffer, seed, first, bytes);
    return ta_chain_write(pool, chain, buffer, bytes);
}

static void test_lengths(uint32_t chunk_size) {
    ta_pool pool;
    chunk_pool(&pool, chunk_size, 8, 0);
    uint32_t payload = ta_chain_payload(&pool);
    uint32_t lengths[] = { 0, 1, payload - 1, payload, payload + 1, 2 * payload - 1, 2 * payload, 2 * payload + 1,
                           5 * payload, 5 * payload + 3 };
    CHECK(ta_pool_refill(&pool, 1) == TA_OK);
    for (size_t k = 0; k < sizeof(lengths) / sizeof(lengths[0]); k++) {
        uint32_t bytes = lengths[k];
        uint32_t need = (bytes + payload - 1u) / payload;
        CHECK(ta_pool_refill(&pool, need) == TA_OK);
        ta_pool_counts before = counts_of(&pool);
        ta_chain chain = { 0 };
        CHECK(ta_chain_need(&pool, &chain, bytes) == need);
        CHECK(write_pattern(&pool, &chain, (uint32_t)k, 0, bytes) == TA_OK);
        CHECK(chain.length == bytes);
        ta_pool_counts after = counts_of(&pool);
        CHECK(after.used - before.used == need);
        CHECK(verify_chain(&pool, &chain, (uint32_t)k) == need);
        if (bytes == 0) {
            CHECK(chain.head == 0 && chain.tail == 0 && after.used == before.used);
        }
        CHECK(ta_chain_free(&pool, &chain) == need);
        CHECK(counts_of(&pool).used == before.used);
        CHECK(chain.head == 0 && chain.tail == 0 && chain.length == 0);
        CHECK(ta_chain_free(&pool, &chain) == 0);
    }
    /* A chain's last chunk ends with next == 0 even when its chunk was recycled from another chain. */
    ta_chain first = { 0 };
    CHECK(write_pattern(&pool, &first, 1, 0, 3 * payload) == TA_OK);
    CHECK(ta_chain_free(&pool, &first) == 3);
    ta_chain second = { 0 };
    CHECK(write_pattern(&pool, &second, 2, 0, 2 * payload + 1) == TA_OK);
    CHECK(verify_chain(&pool, &second, 2) == 3);
    CHECK(ta_chain_free(&pool, &second) == 3);
    CHECK(counts_of(&pool).high_water >= 5);
    ta_pool_destroy(&pool);
    finish_clean();
}

static void test_appends(uint32_t chunk_size) {
    ta_pool pool;
    chunk_pool(&pool, chunk_size, 8, 0);
    uint32_t payload = ta_chain_payload(&pool);
    CHECK(ta_pool_refill(&pool, 64) == TA_OK);
    uint32_t pieces[] = { 1, payload - 2, 1, 1, payload, payload + 1, 0, 3, 2 * payload - 3 };
    ta_chain chain = { 0 };
    uint32_t total = 0;
    for (size_t k = 0; k < sizeof(pieces) / sizeof(pieces[0]); k++) {
        uint32_t used_before = counts_of(&pool).used;
        uint32_t need = ta_chain_need(&pool, &chain, pieces[k]);
        CHECK(write_pattern(&pool, &chain, 9, total, pieces[k]) == TA_OK);
        total += pieces[k];
        CHECK(chain.length == total);
        CHECK(counts_of(&pool).used - used_before == need);
        CHECK(verify_chain(&pool, &chain, 9) == (total + payload - 1u) / payload);
    }
    /* One write of the concatenation takes the same number of chunks. */
    ta_chain whole = { 0 };
    CHECK(write_pattern(&pool, &whole, 9, 0, total) == TA_OK);
    CHECK(whole.length == chain.length);
    CHECK(verify_chain(&pool, &whole, 9) == verify_chain(&pool, &chain, 9));
    /* An exactly full tail: need counts a whole chunk. */
    ta_chain exact = { 0 };
    CHECK(write_pattern(&pool, &exact, 4, 0, 2 * payload) == TA_OK);
    CHECK(ta_chain_need(&pool, &exact, 1) == 1);
    CHECK(write_pattern(&pool, &exact, 4, 2 * payload, 1) == TA_OK);
    CHECK(verify_chain(&pool, &exact, 4) == 3);
    ta_chain_free(&pool, &chain);
    ta_chain_free(&pool, &whole);
    ta_chain_free(&pool, &exact);
    CHECK(counts_of(&pool).used == 0);
    ta_pool_destroy(&pool);
    finish_clean();
}

static void test_all_or_nothing(void) {
    ta_pool pool;
    chunk_pool(&pool, 64, 4, 4); /* a ceiling of one page: four chunks */
    CHECK(ta_pool_refill(&pool, 4) == TA_OK);
    CHECK(ta_pool_refill(&pool, 5) == TA_ERR_NOMEM);

    /* Four chunks hold 240 bytes. 241 is one short, from empty. */
    ta_chain chain = { 0 };
    ta_pool_counts before = counts_of(&pool);
    CHECK(write_pattern(&pool, &chain, 3, 0, 241) == TA_ERR_NOMEM);
    ta_pool_counts after = counts_of(&pool);
    CHECK(chain.head == 0 && chain.tail == 0 && chain.length == 0);
    CHECK(after.used == before.used && after.free == before.free && after.extent == before.extent);
    CHECK(after.high_water == before.high_water);

    /* Two full chunks and 10 bytes: three used, one free. An append that needs two or three more must fail. */
    CHECK(write_pattern(&pool, &chain, 3, 0, 130) == TA_OK);
    ta_chain saved = chain;
    before = counts_of(&pool);
    CHECK(ta_chain_need(&pool, &chain, 120) == 2); /* 50 fit in the tail, 70 more need two */
    CHECK(ta_chain_need(&pool, &chain, 171) == 3);
    CHECK(write_pattern(&pool, &chain, 3, 130, 171) == TA_ERR_NOMEM);
    CHECK(chain.head == saved.head && chain.tail == saved.tail && chain.length == saved.length);
    after = counts_of(&pool);
    CHECK(after.used == before.used && after.free == before.free && after.extent == before.extent);
    CHECK(verify_chain(&pool, &chain, 3) == 3);
    /* The free list order is untouched: the next take after a give is the one given. */
    ta_chain other = { 0 };
    CHECK(write_pattern(&pool, &other, 5, 0, 60) == TA_OK);
    uint32_t index = other.head;
    CHECK(ta_chain_free(&pool, &other) == 1);
    CHECK(write_pattern(&pool, &other, 5, 0, 1) == TA_OK);
    CHECK(other.head == index);
    ta_chain_free(&pool, &other);
    /* A write that fits still works after the failure and reaches the exact fit. */
    CHECK(write_pattern(&pool, &chain, 3, 130, 110) == TA_OK);
    CHECK(verify_chain(&pool, &chain, 3) == 4);
    CHECK(counts_of(&pool).free == 0);
    CHECK(write_pattern(&pool, &chain, 3, 240, 1) == TA_ERR_NOMEM);
    CHECK(ta_chain_free(&pool, &chain) == 4);

    /* Argument errors. */
    CHECK(ta_chain_write(&pool, &chain, NULL, 1) == TA_ERR_INVALID);
    CHECK(ta_chain_write(&pool, NULL, "x", 1) == TA_ERR_INVALID);
    CHECK(ta_chain_write(NULL, &chain, "x", 1) == TA_ERR_INVALID);
    CHECK(ta_chain_write(&pool, &chain, NULL, 0) == TA_OK);
    ta_pool_destroy(&pool);
    finish_clean();
}

static void test_length_limit(void) {
    ta_pool pool;
    chunk_pool(&pool, 64, 8, 0);
    CHECK(ta_pool_refill(&pool, 8) == TA_OK);
    ta_chain chain = { 0 };
    CHECK(write_pattern(&pool, &chain, 1, 0, 61) == TA_OK);
    /* Only the u32 bound is exercised: pretend the chain is nearly 4 GiB long. */
    ta_chain real = chain;
    chain.length = UINT32_MAX - 1u;
    ta_pool_counts before = counts_of(&pool);
    CHECK(write_pattern(&pool, &chain, 1, 0, 2) == TA_ERR_NOMEM);
    CHECK(chain.length == UINT32_MAX - 1u);
    CHECK(counts_of(&pool).used == before.used);
    CHECK(ta_chain_need(&pool, &chain, UINT32_MAX) > 0); /* need itself cannot overflow */
    chain = real;
    CHECK(ta_chain_free(&pool, &chain) == 2);
    ta_pool_destroy(&pool);
    finish_clean();
}

static void test_no_allocation(void) {
    ta_pool pool;
    chunk_pool(&pool, 64, 8, 0);
    CHECK(ta_pool_refill(&pool, 16) == TA_OK);
    ta_test_allocator_reset_counts(&heap);
    ta_chain chain = { 0 };
    CHECK(write_pattern(&pool, &chain, 1, 0, 300) == TA_OK);
    CHECK(write_pattern(&pool, &chain, 1, 300, 100) == TA_OK);
    CHECK(verify_chain(&pool, &chain, 1) == 7);
    CHECK(ta_chain_free(&pool, &chain) == 7);
    CHECK(heap.stats.attempts == 0);
    ta_pool_destroy(&pool);
    CHECK(heap.stats.live_blocks == 0);
}

static void test_reader(uint32_t chunk_size) {
    ta_pool pool;
    chunk_pool(&pool, chunk_size, 8, 0);
    uint32_t payload = ta_chain_payload(&pool);
    CHECK(ta_pool_refill(&pool, 32) == TA_OK);
    uint32_t lengths[] = { 0, 1, payload, payload + 1, 3 * payload, 3 * payload + 5 };
    for (size_t k = 0; k < sizeof(lengths) / sizeof(lengths[0]); k++) {
        uint32_t bytes = lengths[k];
        ta_chain chain = { 0 };
        CHECK(write_pattern(&pool, &chain, 7, 0, bytes) == TA_OK);

        /* Spans: non-empty, at most payload, in order, covering the chain once. */
        ta_chain_reader reader;
        ta_chain_span span;
        ta_chain_reader_init(&reader, &pool, &chain);
        CHECK(ta_chain_reader_remaining(&reader) == bytes);
        uint32_t seen = 0;
        uint32_t spans = 0;
        while (ta_chain_reader_span(&reader, &span)) {
            CHECK(span.length > 0 && span.length <= payload);
            for (uint32_t i = 0; i < span.length; i++) {
                CHECK(span.bytes[i] == pattern(7, seen + i));
            }
            seen += span.length;
            spans += 1;
            CHECK(ta_chain_reader_remaining(&reader) == bytes - seen);
        }
        CHECK(seen == bytes);
        CHECK(spans == (bytes + payload - 1u) / payload);
        CHECK(!ta_chain_reader_span(&reader, &span)); /* stays exhausted */

        /* Copy in awkward pieces that straddle chunk boundaries. */
        unsigned char out[MAX_BYTES];
        ta_chain_reader_init(&reader, &pool, &chain);
        uint32_t position = 0;
        uint32_t step = 1;
        while (position < bytes) {
            uint32_t take = step < bytes - position ? step : bytes - position;
            CHECK(ta_chain_reader_copy(&reader, out + position, take));
            position += take;
            step = step * 2u + 1u;
            if (step > payload + 3u) {
                step = 2;
            }
        }
        for (uint32_t i = 0; i < bytes; i++) {
            CHECK(out[i] == pattern(7, i));
        }
        /* Too few remaining: false, nothing copied, nothing consumed. */
        CHECK(ta_chain_reader_remaining(&reader) == 0);
        CHECK(!ta_chain_reader_copy(&reader, out, 1));
        ta_chain_reader_init(&reader, &pool, &chain);
        unsigned char guard[4] = { 0xaa, 0xaa, 0xaa, 0xaa };
        if (bytes < UINT32_MAX) {
            CHECK(!ta_chain_reader_copy(&reader, guard, bytes + 1u));
            CHECK(ta_chain_reader_remaining(&reader) == bytes);
            CHECK(guard[0] == 0xaa && guard[3] == 0xaa);
        }
        CHECK(ta_chain_reader_copy(&reader, guard, 0));
        CHECK(ta_chain_reader_remaining(&reader) == bytes);
        ta_chain_free(&pool, &chain);
    }
    ta_pool_destroy(&pool);
    finish_clean();
}

#define INTERLEAVED 4u

static void test_interleaved(uint32_t chunk_size) {
    ta_pool pool;
    chunk_pool(&pool, chunk_size, 8, 0);
    uint32_t payload = ta_chain_payload(&pool);
    CHECK(ta_pool_refill(&pool, 64) == TA_OK);
    ta_chain chains[INTERLEAVED] = { { 0 } };
    uint32_t lengths[INTERLEAVED] = { 0 };
    /* Round-robin small pieces, so the chunks of the chains alternate in index order. */
    for (uint32_t round = 0; round < 6; round++) {
        for (uint32_t c = 0; c < INTERLEAVED; c++) {
            uint32_t piece = payload / 2u + c + round;
            CHECK(write_pattern(&pool, &chains[c], c + 1u, lengths[c], piece) == TA_OK);
            lengths[c] += piece;
        }
    }
    uint32_t chunks = 0;
    for (uint32_t c = 0; c < INTERLEAVED; c++) {
        chunks += verify_chain(&pool, &chains[c], c + 1u);
    }
    CHECK(counts_of(&pool).used == chunks);
    /* Free in a different order than written; the others stay intact. */
    uint32_t order[INTERLEAVED] = { 2, 0, 3, 1 };
    for (uint32_t k = 0; k < INTERLEAVED; k++) {
        uint32_t c = order[k];
        uint32_t own = (lengths[c] + payload - 1u) / payload;
        CHECK(ta_chain_free(&pool, &chains[c]) == own);
        chunks -= own;
        CHECK(counts_of(&pool).used == chunks);
        for (uint32_t other = 0; other < INTERLEAVED; other++) {
            verify_chain(&pool, &chains[other], other + 1u);
        }
    }
    CHECK(counts_of(&pool).used == 0);
    ta_pool_destroy(&pool);
    finish_clean();
}

int main(void) {
    test_geometry();
    test_need_arithmetic();
    test_lengths(64);
    test_lengths(256);
    test_appends(64);
    test_appends(256);
    test_all_or_nothing();
    test_length_limit();
    test_no_allocation();
    test_reader(64);
    test_reader(256);
    test_interleaved(64);
    test_interleaved(256);
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "chain_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("chain_test: ok");
    return 0;
}
