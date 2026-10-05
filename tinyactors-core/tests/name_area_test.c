/*
 * ta_name_area: an envelope's text as length-prefixed fields in a chain
 * of 64-byte chunks, with its quota, the admission watermark and the
 * reserve; and a traced envelope's trace parent as its last field
 * (src/name_area.h).
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Memory comes from the test
 * allocator (test_allocator.h), which can refuse any allocation and counts
 * the blocks that are live. Setting its `refuse` policy stands for a
 * macrostep: from then on every allocation would fail.
 *
 * Claims proven here, in order:
 *
 * 1. init refuses fewer than 64 bytes, more than 1 GiB, a watermark of 0%
 *    or 100%, and a reserve of 0 or above the chunks, with
 *    TA_ERR_INVALID, leaving the area untouched; a valid init allocates
 *    nothing, holds bytes / 64 chunks of 60 bytes of text, and its
 *    watermark is max(1, chunks * percent / 100);
 * 2. a field is a little-endian u16 length and its bytes: for every
 *    field length from 0 to past two chunks, written after a first field
 *    that leaves the chain at every offset of a chunk (so the prefix
 *    straddles a boundary at offset 59), the raw chain holds exactly the
 *    prefix and the bytes, the chain holds ceil(length / 60) chunks, and
 *    the reader gives the fields back in order;
 * 3. field_need is exact: a write takes exactly the chunks it answers,
 *    and the quota counts them; it answers UINT32_MAX for a field longer
 *    than 65,535 bytes;
 * 4. a trace parent is a 40-byte field, length 38 then the context and
 *    the cause little-endian: written after names of every length near a
 *    chunk boundary it reads back equal (family 0), its raw bytes are as
 *    laid out, and a 18-byte name with it fills one chunk while a 19-byte
 *    name takes two; a chain without one answers TA_ERR_NOT_FOUND, and a
 *    trace parent with no trace ID is refused with TA_ERR_INVALID,
 *    changing nothing;
 * 5. the reader skips what is left of a field, answers TA_ERR_NOT_FOUND
 *    after the last field and for an empty chain, reads spans within one
 *    chunk at every limit, and answers TA_ERR_CORRUPT, changing nothing,
 *    for a chain that ends inside a prefix or a field;
 * 6. every failing write changes nothing (chain, writer, chunk bytes,
 *    quota and free count): a field over 65,535 bytes is TA_ERR_INVALID;
 *    too few free chunks, the bound, and, for admission, the watermark
 *    are TA_ERR_FULL; an ordinary write still fits above the watermark;
 *    admitting says whether one more chunk fits below it;
 * 7. free returns a chain's chunks to the area and its quota and empties
 *    the chain; freeing an empty chain changes nothing;
 * 8. field_need, admitting, write, write_trace_parent, free, next, read
 *    and trace_parent never allocate, so they may run inside a macrostep;
 *    refill makes min(reserve, chunks - used) chunks free and allocates
 *    nothing when enough are; a refill that cannot allocate answers
 *    TA_ERR_NOMEM and changes nothing; trim keeps the reserve free;
 *    destroy frees every page.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

#define TEXT TA_NAME_AREA_TEXT_BYTES
/* Chunks per page. */
#define PAGE (1u << TA_NAME_AREA_PAGE_SHIFT)
/* The longest field the layout tests write. */
#define FIELD_MAX (2u * TEXT + 10u)

/* An area over `heap` of `bytes`, `watermark` percent and `reserve` chunks. */
static ta_name_area area_over(test_allocator *heap, const uint32_t shape[3]) {
    ta_name_area area;
    ta_result result = ta_name_area_init(&area, &(ta_name_area_params){ .allocator = test_allocator_of(heap),
                                                                         .bytes = shape[0],
                                                                         .watermark_percent = shape[1],
                                                                         .reserve = shape[2] });
    CHECK(result == TA_OK);
    return area;
}

/* Byte i of the test pattern `seed`. */
static uint8_t pattern(uint32_t seed, uint32_t index) {
    return (uint8_t)(index * 13u + seed * 29u + 1u);
}

/* Writes a field of `length` bytes of pattern `seed`. */
static ta_result write_field(ta_name_area *area, ta_chunk_writer *writer, const uint32_t seed_length[2]) {
    uint8_t bytes[TA_NAME_AREA_FIELD_MAX + 1u];
    for (uint32_t i = 0; i < seed_length[1]; i += 1u) {
        bytes[i] = pattern(seed_length[0], i);
    }
    return ta_name_area_write(area, &(ta_name_area_write_params){
                                        .writer = writer, .text = { .ptr = bytes, .len = seed_length[1] } });
}

/* The raw bytes of `chain`, as the chunks hold them; answers how many. */
static uint32_t raw(const ta_name_area *area, ta_chunk_chain chain, uint8_t *out) {
    ta_chunk_reader reader = ta_chunk_reader_of(chain);
    uint32_t total = 0;
    for (ta_bytes span = ta_chunks_read(&area->chunks, &reader, UINT32_MAX); span.len != 0u;
         span = ta_chunks_read(&area->chunks, &reader, UINT32_MAX)) {
        memcpy(out + total, span.ptr, span.len);
        total += span.len;
    }
    return total;
}

/* Reads the current field whole into `out`, in spans of at most `max`; answers its bytes. */
static uint32_t read_field(const ta_name_area *area, ta_name_area_reader *reader, const uint32_t max_expected[2],
                           uint8_t *out) {
    uint32_t total = 0;
    for (ta_bytes span = ta_name_area_read(area, reader, max_expected[0]); span.len != 0u;
         span = ta_name_area_read(area, reader, max_expected[0])) {
        CHECK(span.len <= max_expected[0]);
        CHECK(total + span.len <= max_expected[1]);
        memcpy(out + total, span.ptr, span.len);
        total += span.len;
    }
    return total;
}

/* Chunks a chain of `length` bytes holds. */
static uint32_t chunks_of(uint32_t length) {
    return (length + TEXT - 1u) / TEXT;
}

/* Claim 1. */
static void test_init(void) {
    test_allocator heap = { 0 };
    static const uint32_t invalid[][3] = {
        { 63u, 50u, 1u }, { TA_NAME_AREA_MAX_BYTES + 1u, 50u, 1u }, { 640u, 0u, 1u },
        { 640u, 100u, 1u }, { 640u, 50u, 0u }, { 640u, 50u, 11u }, { 127u, 50u, 2u },
    };
    for (size_t row = 0; row < sizeof invalid / sizeof invalid[0]; row += 1u) {
        ta_name_area area;
        memset(&area, 0xEE, sizeof(area));
        ta_name_area before = area;
        ta_result result = ta_name_area_init(&area, &(ta_name_area_params){ .allocator = test_allocator_of(&heap),
                                                                             .bytes = invalid[row][0],
                                                                             .watermark_percent = invalid[row][1],
                                                                             .reserve = invalid[row][2] });
        CHECK(result == TA_ERR_INVALID);
        CHECK(memcmp(&area, &before, sizeof(area)) == 0);
    }
    /* bytes, percent, chunks, watermark */
    static const uint32_t shapes[][4] = {
        { 64u, 50u, 1u, 1u },           { 127u, 99u, 1u, 1u },         { 640u, 50u, 10u, 5u },
        { 262144u, 50u, 4096u, 2048u }, { 262144u, 1u, 4096u, 40u },   { 262144u, 99u, 4096u, 4055u },
        { TA_NAME_AREA_MAX_BYTES, 99u, 16777216u, 16609443u },
    };
    for (size_t row = 0; row < sizeof shapes / sizeof shapes[0]; row += 1u) {
        ta_name_area area = area_over(&heap, (uint32_t[3]){ shapes[row][0], shapes[row][1], 1u });
        CHECK(area.quota.capacity == shapes[row][2]);
        CHECK(area.chunks.pool.ceiling == shapes[row][2]);
        CHECK(area.watermark == shapes[row][3]);
        CHECK(area.chunks.payload == 60u);
        ta_name_area_destroy(&area);
    }
    CHECK(heap.attempts == 0u);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 2. */
static void test_layout(void) {
    test_allocator heap = { 0 };
    ta_name_area area = area_over(&heap, (uint32_t[3]){ 64u * 64u, 50u, 64u });
    CHECK(ta_name_area_refill(&area) == TA_OK);
    static uint8_t bytes[2u * (TA_NAME_AREA_FIELD_MAX + 2u)];
    static uint8_t field[TA_NAME_AREA_FIELD_MAX];
    for (uint32_t offset = 0; offset < TEXT; offset += 1u) {
        /* A first field of `offset - 2` bytes, or none, leaves the chain at `offset`. */
        uint32_t first = offset >= 2u ? offset - 2u : 0u;
        uint32_t start = offset >= 2u ? offset : 0u;
        for (uint32_t length = 0; length <= FIELD_MAX; length += 1u) {
            ta_chunk_writer writer = ta_chunk_writer_new();
            if (start != 0u) {
                CHECK(write_field(&area, &writer, (uint32_t[2]){ 1u, first }) == TA_OK);
            }
            CHECK(write_field(&area, &writer, (uint32_t[2]){ 2u, length }) == TA_OK);
            ta_chunk_chain chain = writer.chain;
            CHECK(chain.length == start + 2u + length);
            CHECK(area.chunks.pool.used == chunks_of(chain.length));
            CHECK(area.quota.used == area.chunks.pool.used);

            uint32_t total = raw(&area, chain, bytes);
            CHECK(total == chain.length);
            CHECK(bytes[start] == (uint8_t)(length & 0xFFu) && bytes[start + 1u] == (uint8_t)(length >> 8));
            for (uint32_t i = 0; i < length; i += 1u) {
                CHECK(bytes[start + 2u + i] == pattern(2u, i));
            }

            ta_name_area_reader reader = ta_name_area_reader_of(chain);
            uint32_t got = 0;
            if (start != 0u) {
                CHECK(ta_name_area_next(&area, &reader, &got) == TA_OK);
                CHECK(got == first);
                CHECK(read_field(&area, &reader, (uint32_t[2]){ UINT32_MAX, first }, field) == first);
                for (uint32_t i = 0; i < first; i += 1u) {
                    CHECK(field[i] == pattern(1u, i));
                }
            }
            CHECK(ta_name_area_next(&area, &reader, &got) == TA_OK);
            CHECK(got == length);
            CHECK(read_field(&area, &reader, (uint32_t[2]){ UINT32_MAX, length }, field) == length);
            for (uint32_t i = 0; i < length; i += 1u) {
                CHECK(field[i] == pattern(2u, i));
            }
            CHECK(ta_name_area_next(&area, &reader, &got) == TA_ERR_NOT_FOUND);
            ta_name_area_free(&area, &chain);
            CHECK(area.quota.used == 0u && area.chunks.pool.used == 0u);
        }
    }
    ta_name_area_destroy(&area);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 3. */
static void test_need(void) {
    test_allocator heap = { 0 };
    ta_name_area area = area_over(&heap, (uint32_t[3]){ 64u * 64u, 50u, 64u });
    CHECK(ta_name_area_refill(&area) == TA_OK);
    for (uint32_t before = 0; before <= 2u * TEXT + 2u; before += 1u) {
        for (uint32_t length = 0; length <= FIELD_MAX; length += 7u) {
            ta_chunk_writer writer = ta_chunk_writer_new();
            if (before >= 2u) {
                CHECK(write_field(&area, &writer, (uint32_t[2]){ 3u, before - 2u }) == TA_OK);
            }
            uint32_t need = ta_name_area_field_need(&area, writer.chain.length, length);
            uint32_t used = area.quota.used;
            uint32_t pool_used = area.chunks.pool.used;
            CHECK(write_field(&area, &writer, (uint32_t[2]){ 4u, length }) == TA_OK);
            CHECK(area.quota.used - used == need);
            CHECK(area.chunks.pool.used - pool_used == need);
            ta_chunk_chain chain = writer.chain;
            ta_name_area_free(&area, &chain);
        }
    }
    CHECK(ta_name_area_field_need(&area, 0u, TA_NAME_AREA_FIELD_MAX) == chunks_of(TA_NAME_AREA_FIELD_MAX + 2u));
    CHECK(ta_name_area_field_need(&area, 0u, TA_NAME_AREA_FIELD_MAX + 1u) == UINT32_MAX);
    CHECK(ta_name_area_field_need(&area, UINT32_MAX - 1u, 1u) == UINT32_MAX);
    ta_name_area_destroy(&area);
    CHECK(test_allocator_finish(&heap) == 0u);
}

static const ta_trace_parent sample_parent = {
    .context = { .trace_id_high = 0x0102030405060708u, .trace_id_low = 0x1112131415161718u, .flags = 0x03u, .detail = 0x01u },
    .cause = { .session = 0x2122232425262728u, .macrostep = 0x3132333435363738u, .microstep = 0x41424344u, .family = 7u },
};

/* Claim 4. */
static void test_trace_parent(void) {
    test_allocator heap = { 0 };
    ta_name_area area = area_over(&heap, (uint32_t[3]){ 64u * 64u, 50u, 64u });
    CHECK(ta_name_area_refill(&area) == TA_OK);
    static uint8_t bytes[4u * TEXT];
    for (uint32_t name = 0; name <= 2u * TEXT; name += 1u) {
        ta_chunk_writer writer = ta_chunk_writer_new();
        CHECK(write_field(&area, &writer, (uint32_t[2]){ 5u, name }) == TA_OK);
        CHECK(ta_name_area_write_trace_parent(&area, &(ta_name_area_trace_params){ .writer = &writer,
                                                                                   .parent = &sample_parent }) == TA_OK);
        ta_chunk_chain chain = writer.chain;
        CHECK(chain.length == name + 2u + TA_NAME_AREA_TRACE_FIELD_BYTES);
        CHECK(area.quota.used == chunks_of(chain.length));

        ta_trace_parent parent;
        memset(&parent, 0xEE, sizeof(parent));
        CHECK(ta_name_area_trace_parent(&area, chain, &parent) == TA_OK);
        CHECK(parent.context.trace_id_high == sample_parent.context.trace_id_high);
        CHECK(parent.context.trace_id_low == sample_parent.context.trace_id_low);
        CHECK(parent.context.flags == sample_parent.context.flags);
        CHECK(parent.context.detail == sample_parent.context.detail);
        CHECK(parent.cause.session == sample_parent.cause.session);
        CHECK(parent.cause.macrostep == sample_parent.cause.macrostep);
        CHECK(parent.cause.microstep == sample_parent.cause.microstep);
        CHECK(parent.cause.family == 0u);

        /* The raw layout: 38, then every number little-endian. */
        CHECK(raw(&area, chain, bytes) == chain.length);
        const uint8_t *at = bytes + name + 2u;
        static const uint8_t expected[TA_NAME_AREA_TRACE_FIELD_BYTES] = {
            38, 0, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15,
            0x14, 0x13, 0x12, 0x11, 0x03, 0x01, 0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
            0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31, 0x44, 0x43, 0x42, 0x41,
        };
        CHECK(memcmp(at, expected, sizeof expected) == 0);

        /* It is a field like any other to the reader, and the last one. */
        ta_name_area_reader reader = ta_name_area_reader_of(chain);
        uint32_t got = 0;
        CHECK(ta_name_area_next(&area, &reader, &got) == TA_OK && got == name);
        CHECK(ta_name_area_next(&area, &reader, &got) == TA_OK && got == TA_NAME_AREA_TRACE_PARENT_BYTES);
        CHECK(ta_name_area_next(&area, &reader, &got) == TA_ERR_NOT_FOUND);
        ta_name_area_free(&area, &chain);
    }

    /* The chunk boundary: 18 bytes of name, 2 + 18 + 40 = 60, fill one chunk; 19 take two. */
    for (uint32_t name = 18u; name <= 19u; name += 1u) {
        ta_chunk_writer writer = ta_chunk_writer_new();
        CHECK(write_field(&area, &writer, (uint32_t[2]){ 6u, name }) == TA_OK);
        CHECK(ta_name_area_write_trace_parent(&area, &(ta_name_area_trace_params){ .writer = &writer,
                                                                                   .parent = &sample_parent }) == TA_OK);
        CHECK(area.quota.used == name - 17u);
        ta_chunk_chain chain = writer.chain;
        ta_name_area_free(&area, &chain);
    }

    /* Untraced chains: too short, and long enough but not ending with length 38. */
    ta_trace_parent parent;
    memset(&parent, 0xEE, sizeof(parent));
    ta_trace_parent untouched = parent;
    CHECK(ta_name_area_trace_parent(&area, (ta_chunk_chain){ 0u, 0u }, &parent) == TA_ERR_NOT_FOUND);
    for (uint32_t name = 0; name <= 2u * TEXT; name += 1u) {
        if (name == TA_NAME_AREA_TRACE_PARENT_BYTES) {
            continue; /* a lone 38-byte field is indistinguishable from a trace parent: the flag tells */
        }
        ta_chunk_writer writer = ta_chunk_writer_new();
        CHECK(write_field(&area, &writer, (uint32_t[2]){ 7u, name }) == TA_OK);
        ta_chunk_chain chain = writer.chain;
        CHECK(ta_name_area_trace_parent(&area, chain, &parent) == TA_ERR_NOT_FOUND);
        ta_name_area_free(&area, &chain);
    }
    CHECK(memcmp(&parent, &untouched, sizeof(parent)) == 0);

    /* No trace ID: refused, nothing written. */
    ta_chunk_writer writer = ta_chunk_writer_new();
    CHECK(write_field(&area, &writer, (uint32_t[2]){ 8u, 10u }) == TA_OK);
    ta_chunk_writer before = writer;
    uint32_t used = area.quota.used;
    ta_trace_parent untraced = sample_parent;
    untraced.context.trace_id_high = 0u;
    untraced.context.trace_id_low = 0u;
    CHECK(ta_name_area_write_trace_parent(&area, &(ta_name_area_trace_params){ .writer = &writer,
                                                                               .parent = &untraced }) == TA_ERR_INVALID);
    CHECK(memcmp(&writer, &before, sizeof(writer)) == 0 && area.quota.used == used);
    /* Either half of the trace ID makes it traced. */
    untraced.context.trace_id_low = 1u;
    CHECK(ta_name_area_write_trace_parent(&area, &(ta_name_area_trace_params){ .writer = &writer,
                                                                               .parent = &untraced }) == TA_OK);
    CHECK(ta_name_area_trace_parent(&area, writer.chain, &parent) == TA_OK);
    CHECK(parent.context.trace_id_high == 0u && parent.context.trace_id_low == 1u);
    ta_chunk_chain chain = writer.chain;
    ta_name_area_free(&area, &chain);
    ta_name_area_destroy(&area);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 5. */
static void test_reader(void) {
    test_allocator heap = { 0 };
    ta_name_area area = area_over(&heap, (uint32_t[3]){ 64u * 64u, 50u, 64u });
    CHECK(ta_name_area_refill(&area) == TA_OK);
    uint32_t got = 99u;
    ta_name_area_reader reader = ta_name_area_reader_of((ta_chunk_chain){ 0u, 0u });
    CHECK(ta_name_area_next(&area, &reader, &got) == TA_ERR_NOT_FOUND && got == 99u);
    CHECK(ta_name_area_read(&area, &reader, UINT32_MAX).len == 0u);

    static const uint32_t lengths[] = { 0u, 59u, 1u, 60u, 61u, 130u, 0u, 3u };
    enum { FIELDS = sizeof lengths / sizeof lengths[0] };
    ta_chunk_writer writer = ta_chunk_writer_new();
    for (uint32_t i = 0; i < FIELDS; i += 1u) {
        CHECK(write_field(&area, &writer, (uint32_t[2]){ 10u + i, lengths[i] }) == TA_OK);
    }
    ta_chunk_chain chain = writer.chain;

    /* Skipping: reading only every other field, and part of one, still finds each next prefix. */
    reader = ta_name_area_reader_of(chain);
    for (uint32_t i = 0; i < FIELDS; i += 1u) {
        CHECK(ta_name_area_next(&area, &reader, &got) == TA_OK && got == lengths[i]);
        if (i % 2u == 1u && lengths[i] != 0u) {
            ta_bytes span = ta_name_area_read(&area, &reader, 1u);
            CHECK(span.len == 1u && span.ptr[0] == pattern(10u + i, 0u));
        }
    }
    CHECK(ta_name_area_next(&area, &reader, &got) == TA_ERR_NOT_FOUND);
    CHECK(ta_name_area_next(&area, &reader, &got) == TA_ERR_NOT_FOUND);

    /* Every limit gives spans of at most that many bytes, the field whole. */
    static uint8_t field[TA_NAME_AREA_FIELD_MAX];
    for (uint32_t max = 1u; max <= TEXT + 1u; max += 1u) {
        reader = ta_name_area_reader_of(chain);
        for (uint32_t i = 0; i < FIELDS; i += 1u) {
            CHECK(ta_name_area_next(&area, &reader, &got) == TA_OK);
            CHECK(read_field(&area, &reader, (uint32_t[2]){ max, lengths[i] }, field) == lengths[i]);
            for (uint32_t b = 0; b < lengths[i]; b += 1u) {
                CHECK(field[b] == pattern(10u + i, b));
            }
            CHECK(ta_name_area_read(&area, &reader, max).len == 0u);
        }
    }
    CHECK(ta_name_area_read(&area, &reader, 0u).len == 0u);
    ta_name_area_free(&area, &chain);

    /* Corrupt chains, written past the area's field writer: they end inside a prefix or a field. */
    static const uint8_t short_prefix[] = { 5u };
    static const uint8_t short_field[] = { 5u, 0u, 'a', 'b' };
    static const uint8_t short_second[] = { 1u, 0u, 'x', 70u, 0u, 'y' };
    const ta_bytes corrupt[] = {
        { .ptr = short_prefix, .len = sizeof short_prefix },
        { .ptr = short_field, .len = sizeof short_field },
        { .ptr = short_second, .len = sizeof short_second },
    };
    for (size_t i = 0; i < sizeof corrupt / sizeof corrupt[0]; i += 1u) {
        ta_chunk_writer raw_writer = ta_chunk_writer_new();
        CHECK(ta_chunks_write(&area.chunks, &raw_writer, corrupt[i]) == TA_OK);
        reader = ta_name_area_reader_of(raw_writer.chain);
        if (i == 2u) {
            CHECK(ta_name_area_next(&area, &reader, &got) == TA_OK && got == 1u);
        }
        ta_name_area_reader before = reader;
        got = 99u;
        CHECK(ta_name_area_next(&area, &reader, &got) == TA_ERR_CORRUPT);
        CHECK(got == 99u && memcmp(&reader, &before, sizeof(reader)) == 0);
        ta_chunks_free(&area.chunks, &raw_writer.chain);
    }
    ta_name_area_destroy(&area);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* What a failed write must leave as it was. */
typedef struct {
    ta_chunk_writer writer;
    ta_quota quota;
    uint32_t free;
    uint8_t bytes[10u * TEXT];
    uint32_t length;
} snapshot;

static snapshot snapshot_of(const ta_name_area *area, const ta_chunk_writer *writer) {
    snapshot shot;
    memset(&shot, 0, sizeof(shot));
    shot.writer = *writer;
    shot.quota = area->quota;
    shot.free = area->chunks.pool.free;
    shot.length = raw(area, writer->chain, shot.bytes);
    return shot;
}

static bool unchanged(const ta_name_area *area, const ta_chunk_writer *writer, const snapshot *shot) {
    snapshot now = snapshot_of(area, writer);
    return memcmp(&now.writer, &shot->writer, sizeof(now.writer)) == 0 && now.quota.used == shot->quota.used &&
           now.quota.reserved == shot->quota.reserved && now.free == shot->free && now.length == shot->length &&
           memcmp(now.bytes, shot->bytes, now.length) == 0;
}

/* Claim 6. */
static void test_failures(void) {
    test_allocator heap = { 0 };
    /* 10 chunks, watermark 5, reserve 10. */
    ta_name_area area = area_over(&heap, (uint32_t[3]){ 640u, 50u, 10u });

    /* No chunk free yet: FULL. */
    ta_chunk_writer writer = ta_chunk_writer_new();
    snapshot shot = snapshot_of(&area, &writer);
    CHECK(write_field(&area, &writer, (uint32_t[2]){ 1u, 0u }) == TA_ERR_FULL);
    CHECK(unchanged(&area, &writer, &shot));
    CHECK(ta_name_area_write_trace_parent(&area, &(ta_name_area_trace_params){ .writer = &writer,
                                                                               .parent = &sample_parent }) == TA_ERR_FULL);
    CHECK(unchanged(&area, &writer, &shot));

    CHECK(ta_name_area_refill(&area) == TA_OK);
    CHECK(area.chunks.pool.free == 10u);
    shot = snapshot_of(&area, &writer);

    /* Too long a field: INVALID, at any room. */
    static uint8_t huge[TA_NAME_AREA_FIELD_MAX + 1u];
    CHECK(ta_name_area_write(&area, &(ta_name_area_write_params){
                                        .writer = &writer, .text = { .ptr = huge, .len = TA_NAME_AREA_FIELD_MAX + 1u } }) ==
          TA_ERR_INVALID);
    CHECK(unchanged(&area, &writer, &shot));

    /* Admission: a chain up to the watermark of 5 chunks, and not one byte that needs a sixth. */
    CHECK(ta_name_area_admitting(&area));
    uint8_t text[5u * TEXT];
    memset(text, 'n', sizeof text);
    CHECK(ta_name_area_write(&area, &(ta_name_area_write_params){
                                        .writer = &writer, .text = { .ptr = text, .len = 4u * TEXT - 2u }, .admission = true }) ==
          TA_OK);
    CHECK(area.quota.used == 4u && ta_name_area_admitting(&area));
    shot = snapshot_of(&area, &writer);
    CHECK(ta_name_area_write(&area, &(ta_name_area_write_params){
                                        .writer = &writer, .text = { .ptr = text, .len = TEXT - 1u }, .admission = true }) ==
          TA_ERR_FULL);
    CHECK(unchanged(&area, &writer, &shot));
    CHECK(ta_name_area_write(&area, &(ta_name_area_write_params){
                                        .writer = &writer, .text = { .ptr = text, .len = TEXT - 2u }, .admission = true }) ==
          TA_OK);
    CHECK(area.quota.used == 5u && !ta_name_area_admitting(&area));
    shot = snapshot_of(&area, &writer);
    CHECK(ta_name_area_write(&area, &(ta_name_area_write_params){
                                        .writer = &writer, .text = { .ptr = text, .len = 0u }, .admission = true }) ==
          TA_ERR_FULL);
    CHECK(unchanged(&area, &writer, &shot));
    CHECK(ta_name_area_write_trace_parent(&area, &(ta_name_area_trace_params){
                                                     .writer = &writer, .parent = &sample_parent, .admission = true }) ==
          TA_ERR_FULL);
    CHECK(unchanged(&area, &writer, &shot));

    /* Above the watermark, ordinary writes fit up to the bound of 10 chunks. */
    ta_chunk_writer other = ta_chunk_writer_new();
    CHECK(write_field(&area, &other, (uint32_t[2]){ 2u, 5u * TEXT - 2u }) == TA_OK);
    CHECK(area.quota.used == 10u && area.chunks.pool.free == 0u);
    shot = snapshot_of(&area, &other);
    CHECK(write_field(&area, &other, (uint32_t[2]){ 2u, 0u }) == TA_ERR_FULL);
    CHECK(unchanged(&area, &other, &shot));
    CHECK(ta_name_area_refill(&area) == TA_OK); /* at the bound: nothing to make free */
    CHECK(area.chunks.pool.free == 0u);

    ta_name_area_free(&area, &other.chain);
    ta_name_area_free(&area, &writer.chain);
    CHECK(area.quota.used == 0u && ta_name_area_admitting(&area));
    ta_name_area_destroy(&area);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 7. */
static void test_free(void) {
    test_allocator heap = { 0 };
    ta_name_area area = area_over(&heap, (uint32_t[3]){ 64u * 64u, 50u, 64u });
    CHECK(ta_name_area_refill(&area) == TA_OK);
    ta_chunk_chain empty = { 0u, 0u };
    ta_name_area_free(&area, &empty);
    CHECK(area.quota.used == 0u && area.chunks.pool.free == 64u);
    ta_chunk_writer a = ta_chunk_writer_new();
    ta_chunk_writer b = ta_chunk_writer_new();
    for (uint32_t i = 0; i < 6u; i += 1u) {
        CHECK(write_field(&area, &a, (uint32_t[2]){ 20u + i, 17u * i }) == TA_OK);
        CHECK(write_field(&area, &b, (uint32_t[2]){ 30u + i, 23u * i }) == TA_OK);
    }
    uint32_t held_b = chunks_of(b.chain.length);
    ta_name_area_free(&area, &a.chain);
    CHECK(a.chain.first == 0u && a.chain.length == 0u);
    CHECK(area.quota.used == held_b && area.chunks.pool.used == held_b);
    /* b is intact. */
    ta_name_area_reader reader = ta_name_area_reader_of(b.chain);
    static uint8_t field[TA_NAME_AREA_FIELD_MAX];
    for (uint32_t i = 0; i < 6u; i += 1u) {
        uint32_t got = 0;
        CHECK(ta_name_area_next(&area, &reader, &got) == TA_OK && got == 23u * i);
        CHECK(read_field(&area, &reader, (uint32_t[2]){ UINT32_MAX, got }, field) == got);
        for (uint32_t byte = 0; byte < got; byte += 1u) {
            CHECK(field[byte] == pattern(30u + i, byte));
        }
    }
    ta_name_area_free(&area, &b.chain);
    CHECK(area.quota.used == 0u && area.chunks.pool.used == 0u && area.chunks.pool.free == 64u);
    ta_name_area_destroy(&area);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 8. */
static void test_no_allocation(void) {
    test_allocator heap = { 0 };
    /* 4 pages of chunks, reserve one page and one chunk. */
    uint32_t count = 4u * PAGE;
    ta_name_area area = area_over(&heap, (uint32_t[3]){ count * 64u, 50u, PAGE + 1u });
    CHECK(ta_name_area_refill(&area) == TA_OK);
    CHECK(area.chunks.pool.free == 2u * PAGE && area.chunks.pool.pages.count == 2u);
    uint32_t attempts = heap.attempts;
    CHECK(ta_name_area_refill(&area) == TA_OK);
    CHECK(heap.attempts == attempts);

    /* A macrostep: every allocation from here on would fail. */
    heap.policy.refuse = true;
    ta_chunk_writer writer = ta_chunk_writer_new();
    uint32_t written = 0;
    while (write_field(&area, &writer, (uint32_t[2]){ 40u, TEXT - 2u }) == TA_OK) {
        written += 1u;
        (void)ta_name_area_admitting(&area);
        (void)ta_name_area_field_need(&area, writer.chain.length, 3u);
    }
    CHECK(written == 2u * PAGE); /* what the last safe point left free, and no more */
    CHECK(ta_name_area_write_trace_parent(&area, &(ta_name_area_trace_params){ .writer = &writer,
                                                                               .parent = &sample_parent }) == TA_ERR_FULL);
    ta_name_area_reader reader = ta_name_area_reader_of(writer.chain);
    uint32_t got = 0;
    while (ta_name_area_next(&area, &reader, &got) == TA_OK) {
        (void)ta_name_area_read(&area, &reader, 7u);
    }
    ta_trace_parent parent;
    CHECK(ta_name_area_trace_parent(&area, writer.chain, &parent) == TA_ERR_NOT_FOUND);
    ta_chunk_chain chain = writer.chain;
    ta_name_area_free(&area, &chain);
    writer = ta_chunk_writer_new();
    CHECK(write_field(&area, &writer, (uint32_t[2]){ 41u, 5u }) == TA_OK);
    CHECK(ta_name_area_write_trace_parent(&area, &(ta_name_area_trace_params){ .writer = &writer,
                                                                               .parent = &sample_parent }) == TA_OK);
    CHECK(ta_name_area_trace_parent(&area, writer.chain, &parent) == TA_OK);
    CHECK(heap.attempts == attempts);

    /* Take all but a few chunks, then a refill that cannot allocate changes nothing. */
    ta_chunk_writer filler = ta_chunk_writer_new();
    while (area.chunks.pool.free > 0u) {
        CHECK(write_field(&area, &filler, (uint32_t[2]){ 42u, TEXT - 2u }) == TA_OK);
    }
    uint32_t used = area.quota.used;
    uint32_t pages = area.chunks.pool.pages.count;
    uint32_t live = heap.live;
    CHECK(ta_name_area_refill(&area) == TA_ERR_NOMEM);
    CHECK(area.quota.used == used && area.chunks.pool.free == 0u);
    CHECK(area.chunks.pool.pages.count == pages && heap.live == live);
    CHECK(ta_name_area_trace_parent(&area, writer.chain, &parent) == TA_OK);

    /* A safe point: refill makes min(reserve, chunks - used) free; near the bound, all that is left. */
    heap.policy.refuse = false;
    ta_name_area_free(&area, &filler.chain);
    filler = ta_chunk_writer_new(); /* the freed chain's writer is spent */
    CHECK(ta_name_area_refill(&area) == TA_OK);
    CHECK(area.chunks.pool.free >= PAGE + 1u);
    while (area.quota.used < count - 3u) {
        if (area.chunks.pool.free == 0u) {
            CHECK(ta_name_area_refill(&area) == TA_OK);
        }
        CHECK(write_field(&area, &filler, (uint32_t[2]){ 42u, TEXT - 2u }) == TA_OK); /* one chunk each */
    }
    CHECK(ta_name_area_refill(&area) == TA_OK);
    CHECK(area.chunks.pool.free == 3u && area.chunks.pool.pages.count == 4u);

    /* Trim gives whole free pages back and keeps the reserve free; the traced chain is intact. */
    ta_name_area_free(&area, &filler.chain);
    ta_name_area_trim(&area);
    CHECK(area.chunks.pool.free >= PAGE + 1u && area.chunks.pool.free < 2u * PAGE + 1u);
    CHECK(area.chunks.pool.pages.count < 4u);
    CHECK(ta_name_area_trace_parent(&area, writer.chain, &parent) == TA_OK);
    CHECK(parent.cause.microstep == sample_parent.cause.microstep);
    chain = writer.chain;
    ta_name_area_free(&area, &chain);
    ta_name_area_destroy(&area);
    CHECK(heap.live == 0u);
    CHECK(test_allocator_finish(&heap) == 0u);
}

int main(void) {
    test_init();
    test_layout();
    test_need();
    test_trace_parent();
    test_reader();
    test_failures();
    test_free();
    test_no_allocation();
    if (failures != 0) {
        fprintf(stderr, "name_area_test: %d checks failed\n", failures);
        return 1;
    }
    printf("name_area_test: ok\n");
    return 0;
}
