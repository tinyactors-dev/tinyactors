/*
 * The write-ahead log's records (kernel ADR 0002 section 4): every kind is
 * written and read back in place; the header is the layout's, byte for
 * byte; values stream through the data model's codec in pieces and come
 * back through a source read in pieces of any size; a value with no durable
 * form, and a buffer that cannot grow at any allocation, leave the buffer
 * as it was.
 */
#include "check.h"

#include <string.h>

#include "tinyactors_host.h"

/* A test value: a heap copy of a string, reference-counted. */
typedef struct {
    uint32_t refs;
    size_t length;
    char bytes[];
} text_box;

static void text_retain(void *self) {
    ((text_box *)self)->refs += 1u;
}
static void text_release(void *self) {
    text_box *box = self;
    if (--box->refs == 0) {
        free(box);
    }
}
static const ta_value_vtable text_vtable = { .retain = text_retain, .release = text_release };

static ta_value text_value(const char *bytes, size_t length) {
    text_box *box = malloc(sizeof *box + length);
    CHECK(box != NULL);
    box->refs = 1;
    box->length = length;
    memcpy(box->bytes, bytes, length);
    return (ta_value){ .self = box, .v = &text_vtable };
}

/* The codec: writes a value in 3-byte pieces; refuses one that starts with '!'. */
static ta_op_status text_write(void *self, const ta_datamodel_value_write_params *params) {
    (void)self;
    CHECK(params->durable);
    const text_box *box = params->value->self;
    if (box->length != 0 && box->bytes[0] == '!') {
        return TA_OP_ERROR;
    }
    for (size_t at = 0; at < box->length; at += 3u) {
        size_t count = box->length - at < 3u ? box->length - at : 3u;
        ta_op_status status = params->sink.v->write(params->sink.self,
                                                    (ta_bytes){ .ptr = (const uint8_t *)box->bytes + at,
                                                                .len = (uint32_t)count });
        if (status != TA_OP_OK) {
            return status;
        }
    }
    return TA_OP_OK;
}

static uint32_t read_piece = 1;

static ta_op_status text_read(void *self, const ta_datamodel_value_read_params *params) {
    (void)self;
    char buffer[4096];
    size_t length = 0;
    for (;;) {
        uint32_t got = 0;
        uint32_t capacity = read_piece < sizeof buffer - length ? read_piece : (uint32_t)(sizeof buffer - length);
        ta_op_status status = params->source.v->read(params->source.self, &(ta_byte_read_params){
                                                                              .buffer = (uint8_t *)buffer + length,
                                                                              .capacity = capacity,
                                                                              .got = &got });
        if (status != TA_OP_OK) {
            return status;
        }
        if (got == 0) {
            break;
        }
        length += got;
    }
    *params->out = text_value(buffer, length);
    return TA_OP_OK;
}

static ta_datamodel_vtable codec_vtable;

static ta_datamodel codec(void) {
    codec_vtable.value_write = text_write;
    codec_vtable.value_read = text_read;
    return (ta_datamodel){ .self = NULL, .v = &codec_vtable };
}

static ta_str str(const char *text) {
    return (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) };
}

static uint64_t le(const uint8_t *bytes, uint32_t count) {
    uint64_t value = 0;
    for (uint32_t index = count; index > 0; index -= 1u) {
        value = value << 8 | bytes[index - 1u];
    }
    return value;
}

static bool text_is(const tah_wal_view *view, uint32_t field, const char *expected) {
    ta_str text = { 0 };
    return tah_wal_text(view, field, &text) && text.len == strlen(expected) && memcmp(text.ptr, expected, text.len) == 0;
}

static bool value_is(const tah_wal_view *view, uint32_t index, const char *expected) {
    ta_value value = { 0 };
    if (tah_wal_read_value(view, index, codec(), &value) != TA_OK) {
        return false;
    }
    const text_box *box = value.self;
    bool same = box->length == strlen(expected) && memcmp(box->bytes, expected, box->length) == 0;
    ta_value_release(&value);
    return same;
}

/* Counting allocator whose `fail_at`th allocation fails (0: none). */
static uint32_t allocations;
static uint32_t fail_at;

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    allocations += 1u;
    if (fail_at != 0 && allocations == fail_at) {
        return NULL;
    }
    return check_platform_alloc(self, size, alignment);
}
static const ta_allocator_vtable counting_vtable = { .alloc = counting_alloc, .free = check_platform_free };

static void test_every_kind(void) {
    tah_vec out;
    tah_vec_init(&out, check_platform_allocator(), TAH_LAYOUT(uint8_t));
    ta_value binding = text_value("{\"n\":5}", 7);
    ta_value data = text_value("{\"by\":1}", 8);
    ta_str targets[] = { str("b"), str("c") };
    uint64_t lists[] = { 4, 7, 2, 1, 9 };
    tah_wal_write_params params = { .out = &out, .datamodel = codec() };
    tah_wal_record records[] = {
        { .kind = TAH_WAL_SPAWNED, .seq = 1, .time = 10, .field_count = 3,
          .fields = { tah_wal_text_field(true, str("counter")), tah_wal_text_field(true, str("c1")),
                      tah_wal_text_field(true, str("v1")) },
          .values = { &binding } },
        { .kind = TAH_WAL_ACCEPTED, .seq = 2, .time = 11, .field_count = 10,
          .fields = { tah_wal_number_field(77), tah_wal_number_field(0), tah_wal_text_field(true, str("inc")),
                      tah_wal_text_field(false, (ta_str){ 0 }), tah_wal_text_field(true, str("#_actor_pinger/p1")),
                      tah_wal_text_field(true, str("scxml")), tah_wal_text_field(false, (ta_str){ 0 }),
                      tah_wal_number_field(0), tah_wal_text_field(false, (ta_str){ 0 }), tah_wal_number_field(0) },
          .values = { &data } },
        { .kind = TAH_WAL_DELIVERED, .seq = 3, .time = 12, .field_count = 1, .fields = { tah_wal_number_field(77) } },
        { .kind = TAH_WAL_JUMP, .seq = 4, .time = 13, .field_count = 3,
          .fields = { tah_wal_number_field(78), tah_wal_text_field(false, (ta_str){ 0 }), tah_wal_number_field(2) },
          .texts = targets, .text_count = 2 },
        { .kind = TAH_WAL_DROPPED, .seq = 0, .time = 14, .field_count = 1, .fields = { tah_wal_number_field(79) } },
        { .kind = TAH_WAL_FIRED, .seq = 5, .time = 15, .field_count = 1, .fields = { tah_wal_text_field(true, str("tick1")) } },
        { .kind = TAH_WAL_WITHDRAWN, .seq = 6, .time = 16, .field_count = 1, .fields = { tah_wal_text_field(true, str("t2")) } },
        { .kind = TAH_WAL_ABANDONED, .seq = 7, .time = 17 },
        { .kind = TAH_WAL_RELEASED, .seq = 8, .time = 18 },
        { .kind = TAH_WAL_FINISHED, .seq = 9, .time = 19, .field_count = 1, .fields = { tah_wal_text_field(true, str("over")) } },
        { .kind = TAH_WAL_DESTROYED, .seq = 10, .time = 20 },
        { .kind = TAH_WAL_EVICTED, .seq = 11, .time = 21 },
        { .kind = TAH_WAL_CHECKPOINT, .seq = 11, .time = 22, .field_count = TAH_WAL_CHECKPOINT_FIELDS,
          .fields = { tah_wal_text_field(true, str("counter")), tah_wal_text_field(true, str("c1")),
                      tah_wal_text_field(true, str("v1")), tah_wal_number_field(3), tah_wal_number_field(4),
                      tah_wal_number_field(1), tah_wal_number_field(2), tah_wal_number_field(2),
                      tah_wal_number_field(0), tah_wal_number_field(2), tah_wal_number_field(1),
                      tah_wal_number_field(1) },
          .numbers = lists, .number_count = 5, .values = { &binding } },
        { .kind = TAH_WAL_CHECKPOINT_SEND, .seq = 11, .time = 22, .field_count = 8,
          .fields = { tah_wal_text_field(true, str("tick1")), tah_wal_text_field(true, str("tick")),
                      tah_wal_text_field(true, str("scxml")), tah_wal_text_field(false, (ta_str){ 0 }),
                      tah_wal_number_field(5000), tah_wal_number_field(TAH_WAL_SEND_DATA | TAH_WAL_SEND_SELF),
                      tah_wal_number_field(0), tah_wal_number_field(0) },
          .values = { &data } },
        { .kind = TAH_WAL_CHECKPOINT_INVOKE, .seq = 11, .time = 22, .field_count = 3,
          .fields = { tah_wal_text_field(true, str("work")), tah_wal_text_field(true, str("job")), tah_wal_number_field(0) } },
    };
    uint32_t count = sizeof records / sizeof records[0];
    for (uint32_t index = 0; index < count; index += 1u) {
        CHECK(tah_wal_write(&records[index], &params) == TA_OK);
    }
    CHECK(out.count % 8u == 0);

    /* The header, byte for byte: version 2, kind, fixed bytes, seq, time. */
    const uint8_t *bytes = out.items;
    CHECK(le(bytes, 2) == TAH_WAL_VERSION);
    CHECK(le(bytes + 2, 2) == TAH_WAL_SPAWNED);
    CHECK(le(bytes + 4, 4) == 32u + 3u * 8u);
    CHECK(le(bytes + 8, 8) == 1u);
    CHECK(le(bytes + 16, 8) == 10u);
    CHECK(le(bytes + 24, 4) == strlen("counterc1v1"));
    CHECK(le(bytes + 28, 2) == 1u);
    CHECK(le(bytes + 30, 2) == 0u);
    /* The first text reference: offset 0, length 7. */
    CHECK(le(bytes + 32, 4) == 0u && le(bytes + 36, 4) == 7u);

    /* Read every record back, with values in pieces of 1, 2, 5 and 4096. */
    uint32_t pieces[] = { 1, 2, 5, 4096 };
    for (uint32_t round = 0; round < 4u; round += 1u) {
        read_piece = pieces[round];
        size_t at = 0;
        for (uint32_t index = 0; index < count; index += 1u) {
            size_t span = tah_wal_span(bytes + at, out.count - at, false);
            CHECK(span != 0);
            tah_wal_view view;
            CHECK(tah_wal_read(bytes + at, out.count - at, &view) == TA_OK);
            CHECK(view.span == span);
            CHECK(view.kind == records[index].kind);
            CHECK(view.seq == records[index].seq);
            CHECK(view.time == records[index].time);
            switch (view.kind) {
            case TAH_WAL_SPAWNED:
                CHECK(text_is(&view, 0, "counter") && text_is(&view, 1, "c1") && text_is(&view, 2, "v1"));
                CHECK(value_is(&view, 0, "{\"n\":5}"));
                break;
            case TAH_WAL_ACCEPTED: {
                ta_str absent = { 0 };
                CHECK(tah_wal_number(&view, 0) == 77u);
                CHECK(text_is(&view, 2, "inc") && !tah_wal_text(&view, 3, &absent));
                CHECK(text_is(&view, 4, "#_actor_pinger/p1") && text_is(&view, 5, "scxml"));
                CHECK(value_is(&view, 0, "{\"by\":1}"));
                break;
            }
            case TAH_WAL_JUMP: {
                ta_str absent = { 0 };
                CHECK(tah_wal_number(&view, 0) == 78u && !tah_wal_text(&view, 1, &absent));
                CHECK(tah_wal_number(&view, 2) == 2u && text_is(&view, 3, "b") && text_is(&view, 4, "c"));
                break;
            }
            case TAH_WAL_FIRED:
                CHECK(text_is(&view, 0, "tick1"));
                break;
            case TAH_WAL_CHECKPOINT:
                CHECK(tah_wal_number(&view, 9) == 2u && tah_wal_number(&view, TAH_WAL_CHECKPOINT_FIELDS) == 4u);
                CHECK(tah_wal_number(&view, TAH_WAL_CHECKPOINT_FIELDS + 4u) == 9u);
                CHECK(value_is(&view, 0, "{\"n\":5}"));
                break;
            case TAH_WAL_CHECKPOINT_SEND:
                CHECK(tah_wal_number(&view, 4) == 5000u && value_is(&view, 0, "{\"by\":1}"));
                break;
            default:
                break;
            }
            at += span;
        }
        CHECK(at == out.count);
    }

    /* A checkpoint's unit: its record and its two parts. */
    size_t at = 0;
    for (uint32_t index = 0; index < 12u; index += 1u) {
        at += tah_wal_span(bytes + at, out.count - at, false);
    }
    size_t unit = tah_wal_span(bytes + at, out.count - at, true);
    CHECK(at + unit == out.count);
    CHECK(tah_wal_span(bytes + at, out.count - at - 1u, true) == 0);

    /* Incomplete bytes are not a record yet; a version or kind this build
     * does not know is refused. */
    CHECK(tah_wal_span(bytes, 31, false) == 0);
    tah_wal_view view;
    uint8_t *mutable = out.items;
    mutable[0] = 1;
    CHECK(tah_wal_read(bytes, out.count, &view) == TA_ERR_CORRUPT);
    mutable[0] = 2;
    mutable[2] = 99;
    CHECK(tah_wal_read(bytes, out.count, &view) == TA_ERR_CORRUPT);
    mutable[2] = TAH_WAL_SPAWNED;
    CHECK(tah_wal_read(bytes, out.count, &view) == TA_OK);

    ta_value_release(&binding);
    ta_value_release(&data);
    tah_vec_destroy(&out);
}

/* A value with no durable form, and a buffer that cannot grow at any of
 * its allocations, leave what was written before untouched. */
static void test_refusals(void) {
    ta_value refused = text_value("!nope", 5);
    ta_value long_value = text_value("abcdefghijklmnopqrstuvwxyz0123456789", 36);
    tah_wal_record accepted = {
        .kind = TAH_WAL_ACCEPTED, .seq = 1, .time = 1, .field_count = 10,
        .fields = { tah_wal_number_field(1), tah_wal_number_field(0), tah_wal_text_field(true, str("e")),
                    tah_wal_text_field(false, (ta_str){ 0 }), tah_wal_text_field(false, (ta_str){ 0 }),
                    tah_wal_text_field(false, (ta_str){ 0 }), tah_wal_text_field(false, (ta_str){ 0 }),
                    tah_wal_number_field(0), tah_wal_text_field(false, (ta_str){ 0 }), tah_wal_number_field(0) },
    };
    tah_vec out;
    tah_vec_init(&out, check_platform_allocator(), TAH_LAYOUT(uint8_t));
    tah_wal_write_params params = { .out = &out, .datamodel = codec() };
    CHECK(tah_wal_write(&(tah_wal_record){ .kind = TAH_WAL_ABANDONED, .seq = 1, .time = 1 }, &params) == TA_OK);
    uint32_t before = out.count;
    accepted.values[0] = &refused;
    CHECK(tah_wal_write(&accepted, &params) == TA_ERR_UNSUPPORTED);
    CHECK(out.count == before);
    tah_vec_destroy(&out);

    /* Out of memory at every allocation the write makes. */
    accepted.values[0] = &long_value;
    ta_allocator counting = { .self = NULL, .v = &counting_vtable };
    for (fail_at = 1;; fail_at += 1u) {
        allocations = 0;
        tah_vec_init(&out, counting, TAH_LAYOUT(uint8_t));
        params.out = &out;
        ta_result result = tah_wal_write(&accepted, &params);
        if (result == TA_OK) {
            tah_vec_destroy(&out);
            break;
        }
        CHECK(result == TA_ERR_NOMEM);
        CHECK(out.count == 0);
        tah_vec_destroy(&out);
    }
    CHECK(fail_at > 1u);
    fail_at = 0;
    ta_value_release(&refused);
    ta_value_release(&long_value);
}

int main(void) {
    test_every_kind();
    test_refusals();
    puts("wal test passed");
    return 0;
}
