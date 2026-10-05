/*
 * ta_chunks: byte streams stored as chains of fixed-size chunks taken
 * from an index pool (src/chunks.h). The name area uses 64-byte chunks,
 * the park area 256-byte chunks; every claim from 2 on is proven for both.
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Memory comes from the test
 * allocator (test_allocator.h), which can refuse any allocation and counts
 * the blocks that are live.
 *
 * Claims proven here, in order:
 *
 * 1. init refuses a chunk size that is not a multiple of 4 or is below 8,
 *    a ceiling of 0 or UINT32_MAX, and a page shift the pool refuses,
 *    with TA_ERR_INVALID, leaving the area untouched; a valid init
 *    allocates nothing, has a payload of chunk size - 4 and no chunk, and
 *    an area with no page frees nothing when destroyed;
 * 2. need is exact: for every length of a chain from 0 to past three
 *    chunks and every write size from 0 to past two chunks, it answers
 *    ceil((length + n) / payload) - ceil(length / payload), and a write of
 *    that size onto a chain of that length takes exactly that many chunks
 *    from the pool; a write that would pass UINT32_MAX bytes needs
 *    UINT32_MAX;
 * 3. a chain written in one piece, or in pieces of 1, payload - 1,
 *    payload and payload + 1 bytes, reads back the same bytes at every
 *    length near a chunk boundary (from 0 to past three chunks); it holds
 *    ceil(length / payload) chunks linked from first, the last linked to
 *    0; reading with no limit answers each chunk's payload whole and the
 *    last chunk's part of it, and reading at every limit from 1 to past
 *    a chunk answers spans of at most that limit within one chunk;
 * 4. a writer opened on an existing chain, at every length near a chunk
 *    boundary, appends after its last byte: the chain reads back as the
 *    old bytes followed by the new;
 * 5. a read of an empty chain answers length 0 at once; a read of limit 0
 *    answers length 0 and does not move; once a chain is read to its end
 *    every later read answers length 0;
 * 6. a write that needs more chunks than are free answers TA_ERR_FULL and
 *    changes nothing: the same writer, chain, chunk bytes, used and free
 *    counts and next chunk taken; with exactly need chunks free it
 *    succeeds and leaves none free; a write that fits in the space left
 *    in the last chunk succeeds with no chunk free; a write that would
 *    pass UINT32_MAX bytes answers TA_ERR_FULL and changes nothing;
 * 7. free returns every chunk of a chain to the pool, so used and free are
 *    as before the chain was written, and sets the chain to empty; freeing
 *    an empty chain changes nothing; the chunks freed are taken again by
 *    the next write;
 * 8. two chains written in alternating pieces into one pool interleave
 *    their chunks and each reads back its own bytes; freeing one leaves
 *    the other intact, and freeing both returns every chunk;
 * 9. need, writer_new, writer_open, write, reader_of, read and free never
 *    allocate; only the pool's refill does, and destroy frees every page,
 *    also with chains still in the area.
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

/* The two chunk sizes core uses: the name area's and the park area's. */
static const uint32_t chunk_sizes[] = { 64u, 256u };
#define CHUNK_SIZES (sizeof chunk_sizes / sizeof chunk_sizes[0])

/* The largest stream a test writes, bytes. */
#define STREAM_MAX 4096u

/* Byte i of the test pattern `seed`. */
static uint8_t pattern(uint32_t seed, size_t index) {
    return (uint8_t)(index * 7u + seed * 31u + 3u);
}

static void fill(uint8_t *bytes, size_t count, uint32_t seed, size_t from) {
    for (size_t index = 0; index < count; index += 1u) {
        bytes[index] = pattern(seed, from + index);
    }
}

/* An area of `chunk_size` chunks, 8 per page, over `heap`, refilled to `free_chunks`. */
static ta_chunks area_over(test_allocator *heap, uint32_t chunk_size, uint32_t free_chunks) {
    ta_chunks chunks;
    ta_result result = ta_chunks_init(&chunks, &(ta_chunks_params){ .allocator = test_allocator_of(heap),
                                                                    .chunk_size = chunk_size,
                                                                    .page_shift = 3u,
                                                                    .ceiling = 4096u });
    CHECK(result == TA_OK);
    if (free_chunks != 0) {
        CHECK(ta_index_pool_refill(&chunks.pool, free_chunks) == TA_OK);
    }
    return chunks;
}

/* Writes, as one piece, `pattern_at[2]` bytes of pattern `pattern_at[0]` from byte `pattern_at[1]`. */
static ta_result write_pattern(ta_chunks *chunks, ta_chunk_writer *writer, const uint32_t pattern_at[3]) {
    uint32_t seed = pattern_at[0];
    uint32_t from = pattern_at[1];
    uint32_t count = pattern_at[2];
    uint8_t bytes[STREAM_MAX];
    fill(bytes, count, seed, from);
    return ta_chunks_write(chunks, writer, (ta_bytes){ .ptr = bytes, .len = count });
}

/* Reads the whole chain through spans of at most `max` bytes into `out`, checking each span's shape. */
static uint32_t read_all(const ta_chunks *chunks, ta_chunk_chain chain, uint32_t max, uint8_t *out) {
    ta_chunk_reader reader = ta_chunk_reader_of(chain);
    uint32_t total = 0;
    for (;;) {
        uint32_t offset = total % chunks->payload;
        ta_bytes span = ta_chunks_read(chunks, &reader, max);
        if (span.len == 0) {
            break;
        }
        uint32_t to_chunk_end = chunks->payload - offset;
        uint32_t left = chain.length - total;
        uint32_t expected = to_chunk_end < left ? to_chunk_end : left;
        expected = expected < max ? expected : max;
        CHECK(span.len == expected);
        if (total + span.len > STREAM_MAX) {
            CHECK(false);
            break;
        }
        memcpy(out + total, span.ptr, span.len);
        total += span.len;
    }
    return total;
}

/* Whether the chain holds exactly `count` bytes of pattern `seed` from 0. */
static bool holds(const ta_chunks *chunks, ta_chunk_chain chain, uint32_t seed, uint32_t count) {
    uint8_t out[STREAM_MAX];
    if (chain.length != count || read_all(chunks, chain, UINT32_MAX, out) != count) {
        return false;
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        if (out[index] != pattern(seed, index)) {
            return false;
        }
    }
    return true;
}

/* The link of chunk `index`. */
static uint32_t link_of(const ta_chunks *chunks, uint32_t index) {
    uint32_t link;
    memcpy(&link, ta_index_pool_at(&chunks->pool, index), sizeof link);
    return link;
}

/* Chunks in the chain, by following links from first; checks the last links to 0. */
static uint32_t chunks_linked(const ta_chunks *chunks, ta_chunk_chain chain) {
    if (chain.length == 0) {
        CHECK(chain.first == 0);
        return 0;
    }
    uint32_t expected = (chain.length + chunks->payload - 1u) / chunks->payload;
    uint32_t count = 1;
    uint32_t chunk = chain.first;
    while (count < expected && link_of(chunks, chunk) != 0) {
        chunk = link_of(chunks, chunk);
        count += 1u;
    }
    CHECK(link_of(chunks, chunk) == 0);
    return count;
}

/* The lengths near chunk boundaries: 0 to 3 chunks + 2, every one around a boundary. */
static bool near_boundary(uint32_t payload, uint32_t length) {
    uint32_t within = length % payload;
    return length <= 3u * payload + 2u && (within <= 2u || within >= payload - 2u || length < 4u);
}

/* ------------------------------------------------------------------------
 * Claim 1: init checks its parameters and allocates nothing.
 */
static void test_init(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    ta_chunks untouched;
    memset(&untouched, 0xCD, sizeof untouched);
    ta_chunks chunks = untouched;
    ta_chunks_params bad[] = {
        { .allocator = allocator, .chunk_size = 0u, .page_shift = 3u, .ceiling = 16u },
        { .allocator = allocator, .chunk_size = 4u, .page_shift = 3u, .ceiling = 16u },
        { .allocator = allocator, .chunk_size = 62u, .page_shift = 3u, .ceiling = 16u },
        { .allocator = allocator, .chunk_size = 65u, .page_shift = 3u, .ceiling = 16u },
        { .allocator = allocator, .chunk_size = 64u, .page_shift = 3u, .ceiling = 0u },
        { .allocator = allocator, .chunk_size = 64u, .page_shift = 3u, .ceiling = UINT32_MAX },
        { .allocator = allocator, .chunk_size = 64u, .page_shift = 32u, .ceiling = 16u },
    };
    for (size_t index = 0; index < sizeof bad / sizeof bad[0]; index += 1u) {
        CHECK(ta_chunks_init(&chunks, &bad[index]) == TA_ERR_INVALID);
        CHECK(memcmp(&chunks, &untouched, sizeof chunks) == 0);
    }

    ta_chunks_params smallest = { .allocator = allocator, .chunk_size = 8u, .page_shift = 0u, .ceiling = UINT32_MAX - 1u };
    CHECK(ta_chunks_init(&chunks, &smallest) == TA_OK);
    CHECK(chunks.payload == 4u && chunks.pool.used == 0 && chunks.pool.free == 0);
    ta_chunks_destroy(&chunks);

    for (size_t size = 0; size < CHUNK_SIZES; size += 1u) {
        CHECK(ta_chunks_init(&chunks, &(ta_chunks_params){ .allocator = allocator,
                                                           .chunk_size = chunk_sizes[size],
                                                           .page_shift = 3u,
                                                           .ceiling = 64u }) == TA_OK);
        CHECK(chunks.payload == chunk_sizes[size] - 4u);
        CHECK(chunks.pool.item_size == chunk_sizes[size] && chunks.pool.link_offset == 0);
        CHECK(chunks.pool.used == 0 && chunks.pool.free == 0);
        ta_chunk_writer writer = ta_chunk_writer_new();
        uint8_t byte = 1;
        CHECK(ta_chunks_write(&chunks, &writer, (ta_bytes){ .ptr = &byte, .len = 1 }) == TA_ERR_FULL);
        CHECK(writer.chain.first == 0 && writer.chain.length == 0 && writer.last == 0);
        ta_chunks_destroy(&chunks);
    }
    CHECK(heap.attempts == 0 && heap.frees == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 2: need is exact, and a write takes exactly what need says.
 */
static void test_need(uint32_t chunk_size) {
    test_allocator heap = {0};
    ta_chunks chunks = area_over(&heap, chunk_size, 64u);
    uint32_t payload = chunks.payload;
    for (uint32_t length = 0; length <= 3u * payload + 2u; length += 1u) {
        for (uint32_t n = 0; n <= 2u * payload + 2u; n += 1u) {
            uint32_t expected = (length + n + payload - 1u) / payload - (length + payload - 1u) / payload;
            CHECK(ta_chunks_need(&chunks, length, n) == expected);
            if (!near_boundary(payload, length) || (n > 3u && n % payload > 2u && n % payload < payload - 2u)) {
                continue;
            }
            ta_chunk_writer writer = ta_chunk_writer_new();
            CHECK(write_pattern(&chunks, &writer, (uint32_t[3]){ 1u, 0u, length }) == TA_OK);
            uint32_t used = chunks.pool.used;
            CHECK(write_pattern(&chunks, &writer, (uint32_t[3]){ 1u, length, n }) == TA_OK);
            CHECK(chunks.pool.used - used == expected);
            CHECK(holds(&chunks, writer.chain, 1u, length + n));
            ta_chunks_free(&chunks, &writer.chain);
        }
    }
    CHECK(ta_chunks_need(&chunks, 0u, 0u) == 0);
    CHECK(ta_chunks_need(&chunks, UINT32_MAX, 0u) == 0);
    CHECK(ta_chunks_need(&chunks, 0u, UINT32_MAX) == (uint32_t)(((uint64_t)UINT32_MAX + payload - 1u) / payload));
    CHECK(ta_chunks_need(&chunks, UINT32_MAX, 1u) == UINT32_MAX);
    CHECK(ta_chunks_need(&chunks, 1u, UINT32_MAX) == UINT32_MAX);
    CHECK(chunks.pool.used == 0);
    ta_chunks_destroy(&chunks);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 3: written in any pieces, a chain reads back the same bytes.
 */
static void test_write_read(uint32_t chunk_size) {
    test_allocator heap = {0};
    ta_chunks chunks = area_over(&heap, chunk_size, 64u);
    uint32_t payload = chunks.payload;
    uint32_t pieces[] = { 0u, 1u, payload - 1u, payload, payload + 1u };
    for (uint32_t length = 0; length <= 3u * payload + 2u; length += 1u) {
        if (!near_boundary(payload, length)) {
            continue;
        }
        for (size_t kind = 0; kind < sizeof pieces / sizeof pieces[0]; kind += 1u) {
            uint32_t piece = pieces[kind] == 0u ? (length == 0u ? 1u : length) : pieces[kind];
            ta_chunk_writer writer = ta_chunk_writer_new();
            for (uint32_t at = 0; at < length; at += piece) {
                uint32_t count = length - at < piece ? length - at : piece;
                CHECK(write_pattern(&chunks, &writer, (uint32_t[3]){ 2u, at, count }) == TA_OK);
            }
            CHECK(writer.chain.length == length);
            CHECK(holds(&chunks, writer.chain, 2u, length));
            uint32_t expected_chunks = (length + payload - 1u) / payload;
            CHECK(chunks_linked(&chunks, writer.chain) == expected_chunks);
            CHECK(chunks.pool.used == expected_chunks);
            CHECK((writer.last == 0) == (length == 0));
            if (length != 0) {
                CHECK(link_of(&chunks, writer.last) == 0);
            }

            /* Reading at every limit: spans of at most the limit, within one chunk. */
            if (kind == 0) {
                for (uint32_t max = 1; max <= payload + 2u; max += (max < 4u || max + 4u > payload) ? 1u : 17u) {
                    uint8_t out[STREAM_MAX];
                    CHECK(read_all(&chunks, writer.chain, max, out) == length);
                    bool same = true;
                    for (uint32_t index = 0; index < length; index += 1u) {
                        same = same && out[index] == pattern(2u, index);
                    }
                    CHECK(same);
                }
            }
            ta_chunks_free(&chunks, &writer.chain);
        }
    }
    CHECK(chunks.pool.used == 0);
    ta_chunks_destroy(&chunks);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 4: a writer opened on a chain appends after its last byte.
 */
static void test_open_appends(uint32_t chunk_size) {
    test_allocator heap = {0};
    ta_chunks chunks = area_over(&heap, chunk_size, 64u);
    uint32_t payload = chunks.payload;
    for (uint32_t length = 0; length <= 3u * payload + 2u; length += 1u) {
        if (!near_boundary(payload, length)) {
            continue;
        }
        ta_chunk_writer writer = ta_chunk_writer_new();
        CHECK(write_pattern(&chunks, &writer, (uint32_t[3]){ 3u, 0u, length }) == TA_OK);
        ta_chunk_chain owned = writer.chain;
        uint32_t last = writer.last;

        ta_chunk_writer reopened = ta_chunk_writer_open(&chunks, owned);
        CHECK(reopened.chain.first == owned.first && reopened.chain.length == length && reopened.last == last);
        uint32_t more = payload + 3u;
        CHECK(write_pattern(&chunks, &reopened, (uint32_t[3]){ 3u, length, more }) == TA_OK);
        owned = reopened.chain;
        CHECK(holds(&chunks, owned, 3u, length + more));
        CHECK(chunks_linked(&chunks, owned) == (length + more + payload - 1u) / payload);
        ta_chunks_free(&chunks, &owned);
    }
    CHECK(chunks.pool.used == 0);
    ta_chunks_destroy(&chunks);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 5: the ends of reading.
 */
static void test_read_ends(uint32_t chunk_size) {
    test_allocator heap = {0};
    ta_chunks chunks = area_over(&heap, chunk_size, 8u);

    ta_chunk_reader empty = ta_chunk_reader_of((ta_chunk_chain){ 0u, 0u });
    for (int round = 0; round < 2; round += 1) {
        CHECK(ta_chunks_read(&chunks, &empty, UINT32_MAX).len == 0);
    }

    ta_chunk_writer writer = ta_chunk_writer_new();
    CHECK(write_pattern(&chunks, &writer, (uint32_t[3]){ 4u, 0u, chunks.payload + 5u }) == TA_OK);
    ta_chunk_reader reader = ta_chunk_reader_of(writer.chain);
    ta_chunk_reader before = reader;
    CHECK(ta_chunks_read(&chunks, &reader, 0u).len == 0);
    CHECK(memcmp(&reader, &before, sizeof reader) == 0);
    ta_bytes first = ta_chunks_read(&chunks, &reader, UINT32_MAX);
    CHECK(first.len == chunks.payload && first.ptr[0] == pattern(4u, 0));
    ta_bytes second = ta_chunks_read(&chunks, &reader, UINT32_MAX);
    CHECK(second.len == 5u && second.ptr[4] == pattern(4u, chunks.payload + 4u));
    for (int round = 0; round < 3; round += 1) {
        CHECK(ta_chunks_read(&chunks, &reader, UINT32_MAX).len == 0);
        CHECK(reader.remaining == 0);
    }
    ta_chunks_free(&chunks, &writer.chain);
    ta_chunks_destroy(&chunks);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* A copy of everything a failed write must leave as it was. */
typedef struct {
    ta_chunk_writer writer;
    uint32_t used;
    uint32_t free;
    uint8_t bytes[STREAM_MAX];
} snapshot;

static void take_snapshot(const ta_chunks *chunks, const ta_chunk_writer *writer, snapshot *out) {
    out->writer = *writer;
    out->used = chunks->pool.used;
    out->free = chunks->pool.free;
    CHECK(read_all(chunks, writer->chain, UINT32_MAX, out->bytes) == writer->chain.length);
}

static bool same_as(const ta_chunks *chunks, const ta_chunk_writer *writer, const snapshot *before) {
    uint8_t now[STREAM_MAX];
    return memcmp(&before->writer, writer, sizeof *writer) == 0 && chunks->pool.used == before->used &&
           chunks->pool.free == before->free &&
           read_all(chunks, writer->chain, UINT32_MAX, now) == writer->chain.length &&
           memcmp(now, before->bytes, writer->chain.length) == 0;
}

/* ------------------------------------------------------------------------
 * Claim 6: a write that does not fit answers TA_ERR_FULL and changes nothing.
 */
static void test_full(uint32_t chunk_size) {
    test_allocator heap = {0};
    ta_chunks chunks = area_over(&heap, chunk_size, 4u);
    uint32_t payload = chunks.payload;
    CHECK(chunks.pool.free == 8u); /* one page */

    /* A chain of one and a half chunks, then 6 more chunks taken by another chain: 1 free. */
    ta_chunk_writer writer = ta_chunk_writer_new();
    CHECK(write_pattern(&chunks, &writer, (uint32_t[3]){ 5u, 0u, payload + payload / 2u }) == TA_OK);
    ta_chunk_writer other = ta_chunk_writer_new();
    CHECK(write_pattern(&chunks, &other, (uint32_t[3]){ 6u, 0u, 5u * payload + 1u }) == TA_OK);
    CHECK(chunks.pool.free == 0);
    ta_chunks_free(&chunks, &other.chain);
    other = ta_chunk_writer_new();
    CHECK(write_pattern(&chunks, &other, (uint32_t[3]){ 6u, 0u, 5u * payload }) == TA_OK);
    CHECK(chunks.pool.free == 1u);
    uint32_t next_free = chunks.pool.free_list.head;

    /* Needs 2, 1 free: refused, nothing changed. */
    uint32_t length = writer.chain.length;
    uint32_t too_much = (payload - payload / 2u) + payload + 1u;
    CHECK(ta_chunks_need(&chunks, length, too_much) == 2u);
    snapshot before;
    take_snapshot(&chunks, &writer, &before);
    CHECK(write_pattern(&chunks, &writer, (uint32_t[3]){ 5u, length, too_much }) == TA_ERR_FULL);
    CHECK(same_as(&chunks, &writer, &before));
    CHECK(chunks.pool.free_list.head == next_free);

    /* Needs exactly 1, 1 free: succeeds, none left. */
    uint32_t exact = too_much - 1u;
    CHECK(ta_chunks_need(&chunks, length, exact) == 1u && chunks.pool.free == 1u);
    CHECK(write_pattern(&chunks, &writer, (uint32_t[3]){ 5u, length, exact }) == TA_OK);
    CHECK(chunks.pool.free == 0 && writer.last == next_free);
    length += exact;
    CHECK(holds(&chunks, writer.chain, 5u, length));

    /* Now the last chunk is full: one more byte needs a chunk and is refused. */
    CHECK(length % payload == 0);
    take_snapshot(&chunks, &writer, &before);
    CHECK(write_pattern(&chunks, &writer, (uint32_t[3]){ 5u, length, 1u }) == TA_ERR_FULL);
    CHECK(same_as(&chunks, &writer, &before));

    /* Free a chain of 5 chunks; a write that fits in the last chunk needs none. */
    ta_chunks_free(&chunks, &other.chain);
    ta_chunk_writer small = ta_chunk_writer_new();
    CHECK(write_pattern(&chunks, &small, (uint32_t[3]){ 7u, 0u, 4u * payload + 1u }) == TA_OK);
    CHECK(chunks.pool.free == 0);
    CHECK(ta_chunks_need(&chunks, small.chain.length, payload - 1u) == 0);
    CHECK(write_pattern(&chunks, &small, (uint32_t[3]){ 7u, small.chain.length, payload - 1u }) == TA_OK);
    CHECK(holds(&chunks, small.chain, 7u, 5u * payload));
    ta_chunks_free(&chunks, &small.chain);

    /* A chain that would pass UINT32_MAX bytes: refused before anything is touched. */
    CHECK(chunks.pool.free == 5u);
    ta_chunk_writer huge = writer;
    huge.chain.length = UINT32_MAX - 2u; /* as if; only its length is read before refusing */
    ta_chunk_writer huge_before = huge;
    uint32_t free_before = chunks.pool.free;
    uint8_t three[3] = { 1, 2, 3 };
    CHECK(ta_chunks_write(&chunks, &huge, (ta_bytes){ .ptr = three, .len = 3 }) == TA_ERR_FULL);
    CHECK(memcmp(&huge, &huge_before, sizeof huge) == 0 && chunks.pool.free == free_before);
    CHECK(holds(&chunks, writer.chain, 5u, length));

    ta_chunks_free(&chunks, &writer.chain);
    CHECK(chunks.pool.used == 0 && chunks.pool.free == 8u);
    ta_chunks_destroy(&chunks);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 7: free returns every chunk.
 */
static void test_free(uint32_t chunk_size) {
    test_allocator heap = {0};
    ta_chunks chunks = area_over(&heap, chunk_size, 16u);
    uint32_t payload = chunks.payload;
    uint32_t free_before = chunks.pool.free;

    ta_chunk_chain empty = { 0u, 0u };
    ta_chunks_free(&chunks, &empty);
    CHECK(empty.first == 0 && empty.length == 0 && chunks.pool.free == free_before && chunks.pool.used == 0);

    for (uint32_t length = 1; length <= 3u * payload + 2u; length += 1u) {
        if (!near_boundary(payload, length)) {
            continue;
        }
        ta_chunk_writer writer = ta_chunk_writer_new();
        CHECK(write_pattern(&chunks, &writer, (uint32_t[3]){ 8u, 0u, length }) == TA_OK);
        uint32_t taken = (length + payload - 1u) / payload;
        CHECK(chunks.pool.used == taken && chunks.pool.free == free_before - taken);
        uint32_t first = writer.chain.first;
        ta_chunks_free(&chunks, &writer.chain);
        CHECK(writer.chain.first == 0 && writer.chain.length == 0);
        CHECK(chunks.pool.used == 0 && chunks.pool.free == free_before);

        /* The chunks freed are taken again by the next write. */
        ta_chunk_writer again = ta_chunk_writer_new();
        CHECK(write_pattern(&chunks, &again, (uint32_t[3]){ 9u, 0u, length }) == TA_OK);
        bool reused = false;
        for (uint32_t chunk = again.chain.first, left = taken; left > 0; left -= 1u) {
            reused = reused || chunk == first;
            chunk = link_of(&chunks, chunk);
        }
        CHECK(reused && chunks.pool.used == taken);
        ta_chunks_free(&chunks, &again.chain);
    }
    CHECK(chunks.pool.used == 0 && chunks.pool.free == free_before);
    ta_chunks_destroy(&chunks);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 8: two chains interleaved in one pool.
 */
static void test_interleaved(uint32_t chunk_size) {
    test_allocator heap = {0};
    ta_chunks chunks = area_over(&heap, chunk_size, 32u);
    uint32_t payload = chunks.payload;
    ta_chunk_writer a = ta_chunk_writer_new();
    ta_chunk_writer b = ta_chunk_writer_new();
    uint32_t pieces[] = { 1u, payload - 1u, payload, payload + 1u, 3u, 2u * payload + 5u };
    uint32_t at_a = 0;
    uint32_t at_b = 0;
    for (size_t index = 0; index < sizeof pieces / sizeof pieces[0]; index += 1u) {
        CHECK(write_pattern(&chunks, &a, (uint32_t[3]){ 10u, at_a, pieces[index] }) == TA_OK);
        at_a += pieces[index];
        uint32_t piece_b = pieces[(index + 3u) % (sizeof pieces / sizeof pieces[0])];
        CHECK(write_pattern(&chunks, &b, (uint32_t[3]){ 11u, at_b, piece_b }) == TA_OK);
        at_b += piece_b;
    }
    CHECK(holds(&chunks, a.chain, 10u, at_a) && holds(&chunks, b.chain, 11u, at_b));
    uint32_t chunks_a = chunks_linked(&chunks, a.chain);
    uint32_t chunks_b = chunks_linked(&chunks, b.chain);
    CHECK(chunks.pool.used == chunks_a + chunks_b);

    /* The chains' chunks interleave: neither is one ascending run of indices. */
    bool a_ascending_run = true;
    for (uint32_t chunk = a.chain.first; link_of(&chunks, chunk) != 0; chunk = link_of(&chunks, chunk)) {
        a_ascending_run = a_ascending_run && link_of(&chunks, chunk) == chunk + 1u;
    }
    CHECK(!a_ascending_run);

    ta_chunks_free(&chunks, &a.chain);
    CHECK(chunks.pool.used == chunks_b);
    CHECK(holds(&chunks, b.chain, 11u, at_b));
    /* Chunks a gave back go into b's next write, still read back right. */
    CHECK(write_pattern(&chunks, &b, (uint32_t[3]){ 11u, at_b, 3u * payload }) == TA_OK);
    at_b += 3u * payload;
    CHECK(holds(&chunks, b.chain, 11u, at_b));
    ta_chunks_free(&chunks, &b.chain);
    CHECK(chunks.pool.used == 0);
    ta_chunks_destroy(&chunks);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 9: only refill allocates; destroy frees every page.
 */
static void test_allocation(uint32_t chunk_size) {
    test_allocator heap = {0};
    ta_chunks chunks = area_over(&heap, chunk_size, 0u);
    CHECK(heap.attempts == 0);
    CHECK(ta_index_pool_refill(&chunks.pool, ta_chunks_need(&chunks, 0u, 10u * chunks.payload)) == TA_OK);
    CHECK(chunks.pool.free == 16u); /* two pages of 8 */
    uint32_t attempts = heap.attempts;
    uint32_t frees = heap.frees;

    ta_chunk_writer writer = ta_chunk_writer_new();
    CHECK(write_pattern(&chunks, &writer, (uint32_t[3]){ 12u, 0u, 10u * chunks.payload }) == TA_OK);
    ta_chunk_writer open = ta_chunk_writer_open(&chunks, writer.chain);
    CHECK(write_pattern(&chunks, &open, (uint32_t[3]){ 12u, open.chain.length, 3u }) == TA_OK);
    (void)ta_chunks_need(&chunks, open.chain.length, 1000u);
    uint8_t out[STREAM_MAX];
    CHECK(read_all(&chunks, open.chain, 7u, out) == open.chain.length);
    ta_chunk_writer kept = ta_chunk_writer_new();
    CHECK(write_pattern(&chunks, &kept, (uint32_t[3]){ 13u, 0u, 2u }) == TA_OK);
    ta_chunks_free(&chunks, &open.chain);
    CHECK(heap.attempts == attempts && heap.frees == frees);

    /* Destroy with a chain still in the area frees every page. */
    ta_chunks_destroy(&chunks);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

int main(void) {
    test_init();
    for (size_t size = 0; size < CHUNK_SIZES; size += 1u) {
        test_need(chunk_sizes[size]);
        test_write_read(chunk_sizes[size]);
        test_open_appends(chunk_sizes[size]);
        test_read_ends(chunk_sizes[size]);
        test_full(chunk_sizes[size]);
        test_free(chunk_sizes[size]);
        test_interleaved(chunk_sizes[size]);
        test_allocation(chunk_sizes[size]);
    }
    if (failures != 0) {
        fprintf(stderr, "chunks_test: %d checks failed\n", failures);
        return 1;
    }
    printf("chunks_test: ok\n");
    return 0;
}
