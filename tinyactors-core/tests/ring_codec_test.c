/*
 * The record codec (src/generated/ring_codec.h): the writers, size
 * functions and validating reader rendered from protocol/catalog.rb for the
 * records of src/generated/ring_records.h.
 *
 * The test includes the amalgamation so it reaches the private units; it
 * uses the C library, the core does not. Records under test live in
 * buffers of exactly their size from the C heap, so AddressSanitizer
 * (`mise run asan`) catches a read or write past a record.
 *
 * Claims proven here, in order:
 *
 * 1. the table of kinds below is every kind the codec knows:
 *    ta_record_describe answers TA_OK for exactly these 16-bit codes, with
 *    the ring their code range names, `sizeof` their struct as the fixed
 *    size, a multiple of 8 and at least the 32-byte header, and their span
 *    count; it answers TA_ERR_NOT_FOUND for the pad and every other code,
 *    and writes nothing then;
 * 2. every kind round-trips through its writer and the reader, with full
 *    texts and with empty ones: the size function answers the fixed part
 *    plus the text lengths rounded up to 8; the writer sets kind and size,
 *    keeps the seq already in the buffer, copies every other fixed byte
 *    from `fixed`, lays the texts end to end after the fixed part in
 *    declaration order, points each span at its text and zeroes the rest;
 *    the reader, given the kind's ring, answers the record, and each span
 *    names its text;
 * 3. typed access: an h2c.event, a c2h.send and a tel.trace written from
 *    named fields read back field by field through `_from_view` and
 *    ta_record_view_text / ta_record_view_bytes, binary bytes with zeros
 *    included; `_from_view` of another kind answers NULL, and a span that
 *    does not lie in the record answers empty text with a NULL pointer;
 * 4. a writer fills a buffer larger than the record (size = the buffer,
 *    zeroes after the texts) and may take `fixed` from the buffer itself;
 * 5. a writer answers TA_ERR_INVALID for NULL, unaligned or odd-sized
 *    bytes, a NULL `fixed` or `texts`, a text with a length and no bytes,
 *    and texts too long for any record, and TA_ERR_FULL for a buffer
 *    smaller than the record; each changes no byte of the buffer; a size
 *    function answers TA_ERR_INVALID for the same texts and does not write
 *    its result;
 * 6. the reader answers an error and writes nothing to its result for
 *    NULL or unaligned bytes and a ring that is not one (TA_ERR_INVALID),
 *    an unknown kind (TA_ERR_UNSUPPORTED), and (TA_ERR_CORRUPT) fewer
 *    bytes than the header, a pad, a kind of another ring (every kind,
 *    every other ring), a size that is not a multiple of 8, a size past
 *    the bytes given, a size below the kind's fixed part (every kind), a
 *    span that starts inside the fixed part, a span that ends past the
 *    size, an empty span past the size, and a span whose end wraps a u32;
 * 7. end to end through a lossless ta_ring: records written into
 *    reservations keep the ring's gap-free seq and read back through
 *    ta_ring_peek and ta_record_read; a record whose seq skips one is
 *    refused by ta_ring_peek with TA_ERR_CORRUPT before the codec sees it.
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

/* ------------------------------------------------------------------------
 * Every kind, through one signature. A `_texts` struct is a run of ta_str
 * and ta_bytes, which have the same representation, so it is copied from
 * an array of ta_bytes.
 */

typedef ta_result (*write_fn)(ta_record_buffer out, const void *fixed, const ta_bytes *tails);
typedef ta_result (*size_fn)(const ta_bytes *tails, uint32_t *out);

#define WITH_TEXTS(type) \
    static ta_result type##_write_any(ta_record_buffer out, const void *fixed, const ta_bytes *tails) { \
        type##_texts texts; \
        memcpy(&texts, tails, sizeof(texts)); \
        return type##_write(out, (const type *)fixed, &texts); \
    } \
    static ta_result type##_size_any(const ta_bytes *tails, uint32_t *out) { \
        type##_texts texts; \
        memcpy(&texts, tails, sizeof(texts)); \
        return type##_size(&texts, out); \
    }

#define FIXED_ONLY(type) \
    static ta_result type##_write_any(ta_record_buffer out, const void *fixed, const ta_bytes *tails) { \
        (void)tails; \
        return type##_write(out, (const type *)fixed); \
    }

FIXED_ONLY(ta_h2c_time)
WITH_TEXTS(ta_h2c_event)
WITH_TEXTS(ta_h2c_spawn)
WITH_TEXTS(ta_h2c_publish)
WITH_TEXTS(ta_h2c_load)
WITH_TEXTS(ta_h2c_load_failed)
WITH_TEXTS(ta_h2c_unload)
WITH_TEXTS(ta_h2c_checkpoint)
WITH_TEXTS(ta_h2c_invoke_report)
WITH_TEXTS(ta_h2c_destroy)
FIXED_ONLY(ta_h2c_definition_register)
FIXED_ONLY(ta_h2c_definition_release)
FIXED_ONLY(ta_h2c_definition_retire)
FIXED_ONLY(ta_h2c_withdraw)
WITH_TEXTS(ta_h2c_type_declare)
WITH_TEXTS(ta_c2h_spawned)
WITH_TEXTS(ta_c2h_input)
WITH_TEXTS(ta_c2h_send)
WITH_TEXTS(ta_c2h_invoke)
WITH_TEXTS(ta_c2h_invoke_cancel)
WITH_TEXTS(ta_c2h_invoke_forward)
WITH_TEXTS(ta_c2h_log)
FIXED_ONLY(ta_c2h_macrostep_ended)
FIXED_ONLY(ta_c2h_wake)
WITH_TEXTS(ta_c2h_bounced)
WITH_TEXTS(ta_c2h_load_needed)
WITH_TEXTS(ta_c2h_loaded)
WITH_TEXTS(ta_c2h_unloaded)
WITH_TEXTS(ta_c2h_state)
WITH_TEXTS(ta_c2h_published)
FIXED_ONLY(ta_c2h_finished)
WITH_TEXTS(ta_c2h_destroyed)
FIXED_ONLY(ta_c2h_definition_result)
FIXED_ONLY(ta_c2h_input_refused)
FIXED_ONLY(ta_c2h_input_completed)
FIXED_ONLY(ta_c2h_load_result)
WITH_TEXTS(ta_tel_trace)
FIXED_ONLY(ta_tel_dropped)

typedef struct {
    const char *name;
    uint16_t kind;
    size_t fixed_size;
    /* Spans of the kind: sizeof its texts over sizeof(ta_bytes). */
    uint32_t span_count;
    write_fn write;
    size_fn size;
} kind_case;

#define CASE_TEXTS(type, KIND) \
    { #type, KIND, sizeof(type), (uint32_t)(sizeof(type##_texts) / sizeof(ta_bytes)), type##_write_any, \
      type##_size_any }
#define CASE_FIXED(type, KIND) { #type, KIND, sizeof(type), 0u, type##_write_any, NULL }

static const kind_case CASES[] = {
    CASE_FIXED(ta_h2c_time, TA_RECORD_KIND_H2C_TIME),
    CASE_TEXTS(ta_h2c_event, TA_RECORD_KIND_H2C_EVENT),
    CASE_TEXTS(ta_h2c_spawn, TA_RECORD_KIND_H2C_SPAWN),
    CASE_TEXTS(ta_h2c_publish, TA_RECORD_KIND_H2C_PUBLISH),
    CASE_TEXTS(ta_h2c_load, TA_RECORD_KIND_H2C_LOAD),
    CASE_TEXTS(ta_h2c_load_failed, TA_RECORD_KIND_H2C_LOAD_FAILED),
    CASE_TEXTS(ta_h2c_unload, TA_RECORD_KIND_H2C_UNLOAD),
    CASE_TEXTS(ta_h2c_checkpoint, TA_RECORD_KIND_H2C_CHECKPOINT),
    CASE_TEXTS(ta_h2c_invoke_report, TA_RECORD_KIND_H2C_INVOKE_REPORT),
    CASE_TEXTS(ta_h2c_destroy, TA_RECORD_KIND_H2C_DESTROY),
    CASE_FIXED(ta_h2c_definition_register, TA_RECORD_KIND_H2C_DEFINITION_REGISTER),
    CASE_FIXED(ta_h2c_definition_release, TA_RECORD_KIND_H2C_DEFINITION_RELEASE),
    CASE_FIXED(ta_h2c_definition_retire, TA_RECORD_KIND_H2C_DEFINITION_RETIRE),
    CASE_FIXED(ta_h2c_withdraw, TA_RECORD_KIND_H2C_WITHDRAW),
    CASE_TEXTS(ta_h2c_type_declare, TA_RECORD_KIND_H2C_TYPE_DECLARE),
    CASE_TEXTS(ta_c2h_spawned, TA_RECORD_KIND_C2H_SPAWNED),
    CASE_TEXTS(ta_c2h_input, TA_RECORD_KIND_C2H_INPUT),
    CASE_TEXTS(ta_c2h_send, TA_RECORD_KIND_C2H_SEND),
    CASE_TEXTS(ta_c2h_invoke, TA_RECORD_KIND_C2H_INVOKE),
    CASE_TEXTS(ta_c2h_invoke_cancel, TA_RECORD_KIND_C2H_INVOKE_CANCEL),
    CASE_TEXTS(ta_c2h_invoke_forward, TA_RECORD_KIND_C2H_INVOKE_FORWARD),
    CASE_TEXTS(ta_c2h_log, TA_RECORD_KIND_C2H_LOG),
    CASE_FIXED(ta_c2h_macrostep_ended, TA_RECORD_KIND_C2H_MACROSTEP_ENDED),
    CASE_FIXED(ta_c2h_wake, TA_RECORD_KIND_C2H_WAKE),
    CASE_TEXTS(ta_c2h_bounced, TA_RECORD_KIND_C2H_BOUNCED),
    CASE_TEXTS(ta_c2h_load_needed, TA_RECORD_KIND_C2H_LOAD_NEEDED),
    CASE_TEXTS(ta_c2h_loaded, TA_RECORD_KIND_C2H_LOADED),
    CASE_TEXTS(ta_c2h_unloaded, TA_RECORD_KIND_C2H_UNLOADED),
    CASE_TEXTS(ta_c2h_state, TA_RECORD_KIND_C2H_STATE),
    CASE_TEXTS(ta_c2h_published, TA_RECORD_KIND_C2H_PUBLISHED),
    CASE_FIXED(ta_c2h_finished, TA_RECORD_KIND_C2H_FINISHED),
    CASE_TEXTS(ta_c2h_destroyed, TA_RECORD_KIND_C2H_DESTROYED),
    CASE_FIXED(ta_c2h_definition_result, TA_RECORD_KIND_C2H_DEFINITION_RESULT),
    CASE_FIXED(ta_c2h_input_refused, TA_RECORD_KIND_C2H_INPUT_REFUSED),
    CASE_FIXED(ta_c2h_input_completed, TA_RECORD_KIND_C2H_INPUT_COMPLETED),
    CASE_FIXED(ta_c2h_load_result, TA_RECORD_KIND_C2H_LOAD_RESULT),
    CASE_TEXTS(ta_tel_trace, TA_RECORD_KIND_TEL_TRACE),
    CASE_FIXED(ta_tel_dropped, TA_RECORD_KIND_TEL_DROPPED),
};

#define CASE_COUNT (sizeof(CASES) / sizeof(CASES[0]))
#define MAX_SPANS 16u

/* The ring a code names: 0x01xx h2c, 0x02xx c2h, 0x03xx tel. */
static ta_record_ring ring_of(uint16_t kind) { return (ta_record_ring)(kind >> 8); }

static ta_record_description describe(uint16_t kind) {
    ta_record_description description;
    memset(&description, 0, sizeof(description));
    CHECK(ta_record_describe(kind, &description) == TA_OK);
    return description;
}

/* ------------------------------------------------------------------------
 * Buffers of exactly a record's size, 8-byte aligned.
 */

static unsigned char *new_buffer(uint32_t size) {
    unsigned char *bytes = aligned_alloc(8, size == 0 ? 8 : size);
    CHECK(bytes != NULL);
    memset(bytes, 0xA5, size);
    return bytes;
}

static void put_seq(unsigned char *bytes, uint64_t seq) { memcpy(bytes + 8, &seq, sizeof(seq)); }

static uint16_t get_u16(const unsigned char *bytes, size_t offset) {
    uint16_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static uint32_t get_u32(const unsigned char *bytes, size_t offset) {
    uint32_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static uint64_t get_u64(const unsigned char *bytes, size_t offset) {
    uint64_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static void put_u16(unsigned char *bytes, size_t offset, uint16_t value) { memcpy(bytes + offset, &value, sizeof(value)); }
static void put_u32(unsigned char *bytes, size_t offset, uint32_t value) { memcpy(bytes + offset, &value, sizeof(value)); }

static ta_result read_as(const unsigned char *bytes, uint32_t size, ta_record_ring ring, ta_record_view *out) {
    ta_record_read_params params = { .bytes = bytes, .size = size, .ring = ring };
    return ta_record_read(&params, out);
}

/* A view the reader must leave alone on failure. */
static ta_record_view untouched_view(void) {
    ta_record_view view;
    memset(&view, 0x5A, sizeof(view));
    return view;
}

static bool view_is_untouched(const ta_record_view *view) {
    ta_record_view expected = untouched_view();
    return memcmp(view, &expected, sizeof(expected)) == 0;
}

static bool is_span_byte(const ta_record_description *description, size_t offset) {
    for (uint32_t span = 0; span < description->span_count; span += 1u) {
        if (offset >= description->span_offsets[span] && offset < description->span_offsets[span] + 8u) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Claim 1: the table is every kind, and describe agrees with the structs.
 */

static void test_kinds(void) {
    uint32_t known = 0;
    for (uint32_t code = 0; code <= 0xFFFFu; code += 1u) {
        ta_record_description description;
        memset(&description, 0x5A, sizeof(description));
        ta_result result = ta_record_describe((uint16_t)code, &description);
        const kind_case *found = NULL;
        for (size_t index = 0; index < CASE_COUNT; index += 1) {
            if (CASES[index].kind == code) {
                found = &CASES[index];
            }
        }
        if (found == NULL) {
            CHECK(result == TA_ERR_NOT_FOUND);
            ta_record_description untouched;
            memset(&untouched, 0x5A, sizeof(untouched));
            CHECK(memcmp(&description, &untouched, sizeof(untouched)) == 0);
            continue;
        }
        known += 1u;
        CHECK(result == TA_OK);
        CHECK(description.ring == ring_of(found->kind));
        CHECK(description.fixed_size == found->fixed_size);
        CHECK(description.fixed_size % 8u == 0u);
        CHECK(description.fixed_size >= TA_RECORD_HEADER_BYTES);
        CHECK(description.span_count == found->span_count);
        CHECK(description.span_count <= MAX_SPANS);
        for (uint32_t span = 0; span < description.span_count; span += 1u) {
            CHECK(description.span_offsets[span] >= TA_RECORD_HEADER_BYTES);
            CHECK(description.span_offsets[span] + 8u <= description.fixed_size);
            CHECK(span == 0 || description.span_offsets[span] > description.span_offsets[span - 1u]);
        }
    }
    CHECK(known == CASE_COUNT);
    CHECK(ta_record_describe(TA_RECORD_KIND_PAD, &(ta_record_description){ 0 }) == TA_ERR_NOT_FOUND);
    CHECK(sizeof(ta_str) == sizeof(ta_bytes));
}

/* ------------------------------------------------------------------------
 * Claim 2: every kind round-trips, with full and with empty texts.
 */

/* A fixed part whose every byte is non-zero, with flags, session and at. */
static unsigned char *patterned_fixed(const kind_case *c, uint32_t salt) {
    unsigned char *fixed = new_buffer((uint32_t)c->fixed_size);
    for (size_t offset = 0; offset < c->fixed_size; offset += 1) {
        fixed[offset] = (unsigned char)((offset * 31u + salt * 7u) % 251u + 1u);
    }
    return fixed;
}

static void round_trip(const kind_case *c, bool empty) {
    const ta_record_description description = describe(c->kind);
    char storage[MAX_SPANS][40];
    ta_bytes tails[MAX_SPANS];
    memset(tails, 0, sizeof(tails));
    uint64_t lengths = 0;
    for (uint32_t span = 0; span < c->span_count; span += 1u) {
        int length = snprintf(storage[span], sizeof(storage[span]), "%s#%u", c->name, span);
        /* Different lengths, so a mixed-up span shows. */
        tails[span].ptr = empty ? NULL : (const uint8_t *)storage[span];
        tails[span].len = empty ? 0u : (uint32_t)length - (span % 3u);
        lengths += tails[span].len;
    }
    uint32_t size = (uint32_t)((c->fixed_size + lengths + 7u) & ~(uint64_t)7u);
    if (c->size != NULL) {
        uint32_t measured = 0;
        CHECK(c->size(tails, &measured) == TA_OK);
        CHECK(measured == size);
    } else {
        CHECK(size == c->fixed_size);
    }

    unsigned char *fixed = patterned_fixed(c, c->kind);
    unsigned char *bytes = new_buffer(size);
    put_seq(bytes, 41u);
    CHECK(c->write((ta_record_buffer){ bytes, size }, fixed, tails) == TA_OK);

    CHECK(get_u16(bytes, 0) == c->kind);
    CHECK(get_u16(bytes, 2) == get_u16(fixed, 2));
    CHECK(get_u32(bytes, 4) == size);
    CHECK(get_u64(bytes, 8) == 41u);
    for (size_t offset = 16; offset < c->fixed_size; offset += 1) {
        if (!is_span_byte(&description, offset)) {
            CHECK(bytes[offset] == fixed[offset]);
        }
    }
    uint32_t cursor = (uint32_t)c->fixed_size;
    for (uint32_t span = 0; span < c->span_count; span += 1u) {
        CHECK(get_u32(bytes, description.span_offsets[span]) == cursor);
        CHECK(get_u32(bytes, description.span_offsets[span] + 4u) == tails[span].len);
        cursor += tails[span].len;
    }
    for (uint32_t offset = cursor; offset < size; offset += 1u) {
        CHECK(bytes[offset] == 0u);
    }

    ta_record_view view = untouched_view();
    CHECK(read_as(bytes, size, ring_of(c->kind), &view) == TA_OK);
    CHECK(view.bytes == bytes);
    CHECK(view.size == size);
    CHECK(view.kind == c->kind);
    for (uint32_t span = 0; span < c->span_count; span += 1u) {
        ta_span32 at = { get_u32(bytes, description.span_offsets[span]),
                         get_u32(bytes, description.span_offsets[span] + 4u) };
        ta_bytes got = ta_record_view_bytes(&view, at);
        CHECK(got.len == tails[span].len);
        CHECK(got.len == 0 || memcmp(got.ptr, tails[span].ptr, got.len) == 0);
    }
    free(bytes);
    free(fixed);
}

static void test_round_trip(void) {
    for (size_t index = 0; index < CASE_COUNT; index += 1) {
        round_trip(&CASES[index], false);
        round_trip(&CASES[index], true);
    }
}

/* ------------------------------------------------------------------------
 * Claim 3: typed access by field name.
 */

static bool text_is(ta_str text, const char *expected) {
    return text.len == strlen(expected) && memcmp(text.ptr, expected, text.len) == 0;
}

static void test_typed_event(void) {
    ta_h2c_event fixed = { 0 };
    fixed.header.flags = TA_RECORD_FLAG_REPLAYING;
    fixed.header.session = 7u;
    fixed.header.at = 99u;
    fixed.invocation_seq = 3u;
    fixed.data = 5u;
    fixed.trace_id_high = 0x1111u;
    fixed.trace_id_low = 0x2222u;
    fixed.parent_span_id = 0x3333u;
    fixed.type = TA_RECORD_EVENT_TYPE_EXTERNAL;
    fixed.trace_flags = 1u;
    ta_h2c_event_texts texts = {
        .target = ta_str_from_cstr("orders/42"),
        .name = ta_str_from_cstr("order.placed"),
        .sendid = ta_str_from_cstr("s-1"),
        .origin = { NULL, 0u },
        .origin_type = ta_str_from_cstr("http"),
        .invokeid = { NULL, 0u },
    };
    uint32_t size = 0;
    CHECK(ta_h2c_event_size(&texts, &size) == TA_OK);
    CHECK(size == sizeof(ta_h2c_event) + 32u);
    unsigned char *bytes = new_buffer(size);
    CHECK(ta_h2c_event_write((ta_record_buffer){ bytes, size }, &fixed, &texts) == TA_OK);

    ta_record_view view;
    CHECK(read_as(bytes, size, TA_RECORD_RING_H2C, &view) == TA_OK);
    const ta_h2c_event *event = ta_h2c_event_from_view(&view);
    CHECK(event != NULL);
    CHECK(ta_c2h_send_from_view(&view) == NULL);
    CHECK(ta_h2c_time_from_view(&view) == NULL);
    if (event != NULL) {
        CHECK(event->header.kind == TA_RECORD_KIND_H2C_EVENT);
        CHECK(event->header.flags == TA_RECORD_FLAG_REPLAYING);
        CHECK(event->header.session == 7u);
        CHECK(event->header.at == 99u);
        CHECK(event->invocation_seq == 3u);
        CHECK(event->data == 5u);
        CHECK(event->trace_id_high == 0x1111u);
        CHECK(event->trace_id_low == 0x2222u);
        CHECK(event->parent_span_id == 0x3333u);
        CHECK(event->type == TA_RECORD_EVENT_TYPE_EXTERNAL);
        CHECK(event->trace_flags == 1u);
        CHECK(text_is(ta_record_view_text(&view, event->target), "orders/42"));
        CHECK(text_is(ta_record_view_text(&view, event->name), "order.placed"));
        CHECK(text_is(ta_record_view_text(&view, event->sendid), "s-1"));
        CHECK(text_is(ta_record_view_text(&view, event->origin), ""));
        CHECK(text_is(ta_record_view_text(&view, event->origin_type), "http"));
        CHECK(text_is(ta_record_view_text(&view, event->invokeid), ""));
        CHECK(event->target.offset == sizeof(ta_h2c_event));
    }
    /* A span that does not lie in the record names nothing. */
    ta_str outside = ta_record_view_text(&view, (ta_span32){ size - 4u, 8u });
    CHECK(outside.ptr == NULL && outside.len == 0u);
    outside = ta_record_view_text(&view, (ta_span32){ size + 8u, 0u });
    CHECK(outside.ptr == NULL && outside.len == 0u);
    ta_bytes wrapped = ta_record_view_bytes(&view, (ta_span32){ 8u, UINT32_MAX });
    CHECK(wrapped.ptr == NULL && wrapped.len == 0u);
    free(bytes);
}

static void test_typed_send(void) {
    ta_c2h_send fixed = { 0 };
    fixed.header.session = 12u;
    fixed.macrostep = 4u;
    fixed.data = 77u;
    fixed.content = 78u;
    fixed.span_id = 0xABCDu;
    ta_c2h_send_texts texts = {
        .sendid = ta_str_from_cstr("reply-1"),
        .io_type = ta_str_from_cstr("http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor"),
        .target = ta_str_from_cstr("http://example.test/hook"),
        .name = ta_str_from_cstr("done"),
    };
    uint32_t size = 0;
    CHECK(ta_c2h_send_size(&texts, &size) == TA_OK);
    unsigned char *bytes = new_buffer(size);
    CHECK(ta_c2h_send_write((ta_record_buffer){ bytes, size }, &fixed, &texts) == TA_OK);
    ta_record_view view;
    CHECK(read_as(bytes, size, TA_RECORD_RING_C2H, &view) == TA_OK);
    const ta_c2h_send *send = ta_c2h_send_from_view(&view);
    CHECK(send != NULL);
    if (send != NULL) {
        CHECK(send->header.session == 12u);
        CHECK(send->macrostep == 4u);
        CHECK(send->data == 77u);
        CHECK(send->content == 78u);
        CHECK(send->span_id == 0xABCDu);
        CHECK(text_is(ta_record_view_text(&view, send->sendid), "reply-1"));
        CHECK(text_is(ta_record_view_text(&view, send->io_type),
                      "http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor"));
        CHECK(text_is(ta_record_view_text(&view, send->target), "http://example.test/hook"));
        CHECK(text_is(ta_record_view_text(&view, send->name), "done"));
    }
    free(bytes);
}

static void test_typed_trace(void) {
    static const uint8_t transitions[] = { 1, 0, 0, 0, 9, 0, 0, 0, 2, 0, 0, 0, 't', '1' };
    static const uint8_t states[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    ta_tel_trace fixed = { 0 };
    fixed.macrostep = 9u;
    fixed.microstep = 2u;
    fixed.trace_kind = 17u;
    fixed.runaway_cause = TA_RECORD_RUNAWAY_CAUSE_CHAIN_LIMIT;
    ta_tel_trace_texts texts = { 0 };
    texts.address = ta_str_from_cstr("orders/42");
    texts.state_name = ta_str_from_cstr("open");
    texts.transitions = (ta_bytes){ transitions, sizeof(transitions) };
    texts.states = (ta_bytes){ states, sizeof(states) };
    uint32_t size = 0;
    CHECK(ta_tel_trace_size(&texts, &size) == TA_OK);
    unsigned char *bytes = new_buffer(size);
    CHECK(ta_tel_trace_write((ta_record_buffer){ bytes, size }, &fixed, &texts) == TA_OK);
    ta_record_view view;
    CHECK(read_as(bytes, size, TA_RECORD_RING_TEL, &view) == TA_OK);
    const ta_tel_trace *trace = ta_tel_trace_from_view(&view);
    CHECK(trace != NULL);
    if (trace != NULL) {
        CHECK(trace->macrostep == 9u);
        CHECK(trace->microstep == 2u);
        CHECK(trace->trace_kind == 17u);
        CHECK(trace->runaway_cause == TA_RECORD_RUNAWAY_CAUSE_CHAIN_LIMIT);
        CHECK(text_is(ta_record_view_text(&view, trace->address), "orders/42"));
        CHECK(text_is(ta_record_view_text(&view, trace->state_name), "open"));
        CHECK(text_is(ta_record_view_text(&view, trace->fault_message), ""));
        ta_bytes got = ta_record_view_bytes(&view, trace->transitions);
        CHECK(got.len == sizeof(transitions) && memcmp(got.ptr, transitions, got.len) == 0);
        got = ta_record_view_bytes(&view, trace->states);
        CHECK(got.len == sizeof(states) && memcmp(got.ptr, states, got.len) == 0);
    }
    free(bytes);
}

/* ------------------------------------------------------------------------
 * Claim 4: a larger buffer, and `fixed` taken from the buffer itself.
 */

static void test_larger_and_in_place(void) {
    ta_h2c_destroy_texts texts = { .address = ta_str_from_cstr("a/b") };
    uint32_t size = 0;
    CHECK(ta_h2c_destroy_size(&texts, &size) == TA_OK);
    CHECK(size == sizeof(ta_h2c_destroy) + 8u);
    unsigned char *bytes = new_buffer(size + 64u);
    ta_h2c_destroy fixed = { 0 };
    fixed.header.session = 3u;
    CHECK(ta_h2c_destroy_write((ta_record_buffer){ bytes, size + 64u }, &fixed, &texts) == TA_OK);
    CHECK(get_u32(bytes, 4) == size + 64u);
    for (uint32_t offset = (uint32_t)sizeof(ta_h2c_destroy) + 3u; offset < size + 64u; offset += 1u) {
        CHECK(bytes[offset] == 0u);
    }
    ta_record_view view;
    CHECK(read_as(bytes, size + 64u, TA_RECORD_RING_H2C, &view) == TA_OK);
    CHECK(view.size == size + 64u);
    free(bytes);

    /* The producer builds the fixed part in the reservation itself. */
    bytes = new_buffer(size);
    memset(bytes, 0, size);
    put_seq(bytes, 5u);
    ta_h2c_destroy *in_place = (ta_h2c_destroy *)(void *)bytes;
    in_place->header.session = 21u;
    in_place->header.at = 1000u;
    CHECK(ta_h2c_destroy_write((ta_record_buffer){ bytes, size }, in_place, &texts) == TA_OK);
    CHECK(read_as(bytes, size, TA_RECORD_RING_H2C, &view) == TA_OK);
    const ta_h2c_destroy *destroy = ta_h2c_destroy_from_view(&view);
    CHECK(destroy != NULL);
    if (destroy != NULL) {
        CHECK(destroy->header.session == 21u);
        CHECK(destroy->header.at == 1000u);
        CHECK(destroy->header.seq == 5u);
        CHECK(text_is(ta_record_view_text(&view, destroy->address), "a/b"));
    }
    free(bytes);
}

/* ------------------------------------------------------------------------
 * Claim 5: writers and size functions refuse and change nothing.
 */

static void expect_write_refused(ta_result expected, ta_record_buffer out, const ta_h2c_event *fixed,
                                 const ta_h2c_event_texts *texts) {
    unsigned char before[512];
    size_t length = out.bytes != NULL && out.size <= sizeof(before) ? out.size : 0u;
    if (length != 0u) {
        memcpy(before, out.bytes, length);
    }
    CHECK(ta_h2c_event_write(out, fixed, texts) == expected);
    CHECK(length == 0u || memcmp(before, out.bytes, length) == 0);
}

static void test_write_refused(void) {
    ta_h2c_event fixed = { 0 };
    ta_h2c_event_texts texts = { .name = ta_str_from_cstr("go") };
    uint32_t size = 0;
    CHECK(ta_h2c_event_size(&texts, &size) == TA_OK);
    unsigned char *storage = new_buffer(size + 16u);
    unsigned char *bytes = storage + 8;

    expect_write_refused(TA_ERR_INVALID, (ta_record_buffer){ NULL, size }, &fixed, &texts);
    expect_write_refused(TA_ERR_INVALID, (ta_record_buffer){ storage + 4, size }, &fixed, &texts);
    expect_write_refused(TA_ERR_INVALID, (ta_record_buffer){ bytes, size + 4u }, &fixed, &texts);
    expect_write_refused(TA_ERR_INVALID, (ta_record_buffer){ bytes, size }, NULL, &texts);
    expect_write_refused(TA_ERR_INVALID, (ta_record_buffer){ bytes, size }, &fixed, NULL);
    ta_h2c_event_texts no_bytes = texts;
    no_bytes.sendid = (ta_str){ NULL, 3u };
    expect_write_refused(TA_ERR_INVALID, (ta_record_buffer){ bytes, size }, &fixed, &no_bytes);
    /* Lengths no record holds; the writer measures before it reads them. */
    ta_h2c_event_texts too_long = texts;
    too_long.target = (ta_str){ "x", UINT32_MAX };
    expect_write_refused(TA_ERR_INVALID, (ta_record_buffer){ bytes, size }, &fixed, &too_long);
    too_long.target = (ta_str){ "x", UINT32_MAX - (uint32_t)sizeof(ta_h2c_event) - 1u };
    too_long.name = (ta_str){ "x", 1u };
    expect_write_refused(TA_ERR_INVALID, (ta_record_buffer){ bytes, size }, &fixed, &too_long);
    expect_write_refused(TA_ERR_FULL, (ta_record_buffer){ bytes, size - 8u }, &fixed, &texts);
    expect_write_refused(TA_ERR_FULL, (ta_record_buffer){ bytes, 0u }, &fixed, &texts);
    CHECK(ta_h2c_event_write((ta_record_buffer){ bytes, size }, &fixed, &texts) == TA_OK);

    /* A record without texts: the same buffer checks, and its fixed size. */
    ta_h2c_time time = { 0 };
    CHECK(ta_h2c_time_write((ta_record_buffer){ bytes, 24u }, &time) == TA_ERR_FULL);
    CHECK(ta_h2c_time_write((ta_record_buffer){ bytes, 32u }, NULL) == TA_ERR_INVALID);
    CHECK(ta_h2c_time_write((ta_record_buffer){ storage + 4, 32u }, &time) == TA_ERR_INVALID);

    uint32_t measured = 12345u;
    CHECK(ta_h2c_event_size(NULL, &measured) == TA_ERR_INVALID);
    CHECK(ta_h2c_event_size(&no_bytes, &measured) == TA_ERR_INVALID);
    too_long.target = (ta_str){ "x", UINT32_MAX };
    CHECK(ta_h2c_event_size(&too_long, &measured) == TA_ERR_INVALID);
    CHECK(measured == 12345u);
    /* The largest record a u32 holds is still measured. */
    ta_h2c_event_texts largest = { .target = { "x", UINT32_MAX - 7u - (uint32_t)sizeof(ta_h2c_event) } };
    CHECK(ta_h2c_event_size(&largest, &measured) == TA_OK);
    CHECK(measured == UINT32_MAX - 7u);
    free(storage);
}

/* ------------------------------------------------------------------------
 * Claim 6: malformed records answer an error and change nothing.
 */

static void expect_read(ta_result expected, const unsigned char *bytes, uint32_t size, ta_record_ring ring) {
    ta_record_view view = untouched_view();
    CHECK(read_as(bytes, size, ring, &view) == expected);
    if (expected == TA_OK) {
        CHECK(view.bytes == bytes);
    } else {
        CHECK(view_is_untouched(&view));
    }
}

/* A copy of a good h2c.event, to break one thing in. */
static unsigned char *good_event(uint32_t *size) {
    ta_h2c_event fixed = { 0 };
    ta_h2c_event_texts texts = { .target = ta_str_from_cstr("t/1"), .name = ta_str_from_cstr("ping") };
    CHECK(ta_h2c_event_size(&texts, size) == TA_OK);
    unsigned char *bytes = new_buffer(*size);
    CHECK(ta_h2c_event_write((ta_record_buffer){ bytes, *size }, &fixed, &texts) == TA_OK);
    return bytes;
}

static void test_read_refused(void) {
    uint32_t size = 0;
    unsigned char *bytes = good_event(&size);
    unsigned char *copy = new_buffer(size);
    memcpy(copy, bytes, size);
    const size_t target = offsetof(ta_h2c_event, target);
    const size_t name = offsetof(ta_h2c_event, name);

    expect_read(TA_OK, bytes, size, TA_RECORD_RING_H2C);
    expect_read(TA_ERR_INVALID, NULL, size, TA_RECORD_RING_H2C);
    unsigned char *storage = new_buffer(size + 8u);
    memcpy(storage + 4, bytes, size);
    expect_read(TA_ERR_INVALID, storage + 4, size, TA_RECORD_RING_H2C);
    free(storage);
    expect_read(TA_ERR_INVALID, bytes, size, (ta_record_ring)0);
    expect_read(TA_ERR_INVALID, bytes, size, (ta_record_ring)(TA_RECORD_RING_COUNT + 1u));

    expect_read(TA_ERR_CORRUPT, bytes, TA_RECORD_HEADER_BYTES - 8u, TA_RECORD_RING_H2C);
    expect_read(TA_ERR_CORRUPT, bytes, 0u, TA_RECORD_RING_H2C);

    put_u16(copy, 0, TA_RECORD_KIND_PAD);
    expect_read(TA_ERR_CORRUPT, copy, size, TA_RECORD_RING_H2C);
    put_u16(copy, 0, 0x01FFu);
    expect_read(TA_ERR_UNSUPPORTED, copy, size, TA_RECORD_RING_H2C);
    put_u16(copy, 0, 0x7777u);
    expect_read(TA_ERR_UNSUPPORTED, copy, size, TA_RECORD_RING_H2C);
    memcpy(copy, bytes, size);

    put_u32(copy, 4, size - 4u);
    expect_read(TA_ERR_CORRUPT, copy, size, TA_RECORD_RING_H2C);
    put_u32(copy, 4, size + 8u);
    expect_read(TA_ERR_CORRUPT, copy, size, TA_RECORD_RING_H2C);
    put_u32(copy, 4, (uint32_t)sizeof(ta_h2c_event) - 8u);
    expect_read(TA_ERR_CORRUPT, copy, size, TA_RECORD_RING_H2C);
    memcpy(copy, bytes, size);

    /* A span that starts inside the fixed part. */
    put_u32(copy, name, 16u);
    expect_read(TA_ERR_CORRUPT, copy, size, TA_RECORD_RING_H2C);
    put_u32(copy, name, (uint32_t)sizeof(ta_h2c_event) - 1u);
    expect_read(TA_ERR_CORRUPT, copy, size, TA_RECORD_RING_H2C);
    memcpy(copy, bytes, size);
    /* A span that ends past the record's size. */
    put_u32(copy, name + 4u, size);
    expect_read(TA_ERR_CORRUPT, copy, size, TA_RECORD_RING_H2C);
    put_u32(copy, name, size - 2u);
    put_u32(copy, name + 4u, 3u);
    expect_read(TA_ERR_CORRUPT, copy, size, TA_RECORD_RING_H2C);
    /* An empty span ends exactly at the size, and not past it. */
    put_u32(copy, name, size);
    put_u32(copy, name + 4u, 0u);
    ta_record_view view;
    CHECK(read_as(copy, size, TA_RECORD_RING_H2C, &view) == TA_OK);
    put_u32(copy, name, size + 8u);
    expect_read(TA_ERR_CORRUPT, copy, size, TA_RECORD_RING_H2C);
    /* A span whose end wraps a u32. */
    put_u32(copy, target, (uint32_t)sizeof(ta_h2c_event));
    put_u32(copy, target + 4u, UINT32_MAX);
    put_u32(copy, name, (uint32_t)sizeof(ta_h2c_event));
    put_u32(copy, name + 4u, 0u);
    expect_read(TA_ERR_CORRUPT, copy, size, TA_RECORD_RING_H2C);
    /* The record itself was never written to. */
    CHECK(read_as(bytes, size, TA_RECORD_RING_H2C, &view) == TA_OK);
    free(copy);
    free(bytes);

    /* Every kind: on every other ring, and with a size below its fixed part. */
    for (size_t index = 0; index < CASE_COUNT; index += 1) {
        const kind_case *c = &CASES[index];
        ta_bytes tails[MAX_SPANS];
        memset(tails, 0, sizeof(tails));
        unsigned char *fixed = patterned_fixed(c, 3u);
        unsigned char *record = new_buffer((uint32_t)c->fixed_size);
        CHECK(c->write((ta_record_buffer){ record, (uint32_t)c->fixed_size }, fixed, tails) == TA_OK);
        for (uint32_t ring = 1; ring <= TA_RECORD_RING_COUNT; ring += 1u) {
            expect_read(ring == ring_of(c->kind) ? TA_OK : TA_ERR_CORRUPT, record, (uint32_t)c->fixed_size,
                        (ta_record_ring)ring);
        }
        if (c->fixed_size > TA_RECORD_HEADER_BYTES) {
            put_u32(record, 4, (uint32_t)c->fixed_size - 8u);
            expect_read(TA_ERR_CORRUPT, record, (uint32_t)c->fixed_size, ring_of(c->kind));
        }
        put_u32(record, 4, TA_RECORD_HEADER_BYTES - 8u);
        expect_read(TA_ERR_CORRUPT, record, (uint32_t)c->fixed_size, ring_of(c->kind));
        free(record);
        free(fixed);
    }
}

/* ------------------------------------------------------------------------
 * Claim 7: seq end to end through a ring.
 */

static void post_destroy(ta_ring *ring, uint64_t session, const char *address) {
    ta_h2c_destroy_texts texts = { .address = ta_str_from_cstr(address) };
    uint32_t size = 0;
    CHECK(ta_h2c_destroy_size(&texts, &size) == TA_OK);
    ta_ring_record record;
    CHECK(ta_ring_reserve(ring, size, &record) == TA_OK);
    ta_h2c_destroy fixed = { 0 };
    fixed.header.session = session;
    CHECK(ta_h2c_destroy_write((ta_record_buffer){ record.bytes, record.size }, &fixed, &texts) == TA_OK);
    CHECK(get_u64(record.bytes, 8) == record.seq);
}

static void test_seq_through_ring(void) {
    test_allocator heap = {0};
    ta_ring ring;
    ta_ring_params params = {
        .allocator = test_allocator_of(&heap),
        .capacity = 512u,
        .max_record_bytes = 128u,
        .mode = TA_RING_LOSSLESS,
    };
    CHECK(ta_ring_init(&ring, &params) == TA_OK);
    post_destroy(&ring, 1u, "a/1");
    post_destroy(&ring, 2u, "a/2");
    ta_ring_record record;
    CHECK(ta_ring_reserve(&ring, sizeof(ta_h2c_time), &record) == TA_OK);
    ta_h2c_time time = { 0 };
    time.header.at = 500u;
    CHECK(ta_h2c_time_write((ta_record_buffer){ record.bytes, record.size }, &time) == TA_OK);
    ta_ring_commit(&ring);

    for (uint64_t seq = 1; seq <= 3u; seq += 1u) {
        ta_ring_record read;
        CHECK(ta_ring_peek(&ring, &read) == TA_OK);
        CHECK(read.seq == seq);
        ta_record_view view;
        CHECK(read_as(read.bytes, read.size, TA_RECORD_RING_H2C, &view) == TA_OK);
        if (seq < 3u) {
            const ta_h2c_destroy *destroy = ta_h2c_destroy_from_view(&view);
            CHECK(destroy != NULL && destroy->header.session == seq && destroy->header.seq == seq);
        } else {
            const ta_h2c_time *moved = ta_h2c_time_from_view(&view);
            CHECK(moved != NULL && moved->header.at == 500u && moved->header.seq == 3u);
        }
        CHECK(ta_ring_release(&ring) == TA_OK);
    }

    /* A producer that skips a seq: the ring refuses it before the codec. */
    post_destroy(&ring, 4u, "a/4");
    CHECK(ta_ring_reserve(&ring, sizeof(ta_h2c_time), &record) == TA_OK);
    CHECK(ta_h2c_time_write((ta_record_buffer){ record.bytes, record.size }, &time) == TA_OK);
    put_seq(record.bytes, record.seq + 1u);
    ta_ring_commit(&ring);
    ta_ring_record read;
    CHECK(ta_ring_peek(&ring, &read) == TA_OK);
    CHECK(read.seq == 4u);
    CHECK(ta_ring_release(&ring) == TA_OK);
    ta_ring_record untouched;
    memset(&untouched, 0x5A, sizeof(untouched));
    read = untouched;
    CHECK(ta_ring_peek(&ring, &read) == TA_ERR_CORRUPT);
    CHECK(memcmp(&read, &untouched, sizeof(read)) == 0);
    /* The record itself is well formed: only the seq gives it away. */
    ta_record_view view;
    CHECK(read_as(record.bytes, record.size, TA_RECORD_RING_H2C, &view) == TA_OK);
    ta_ring_destroy(&ring);
    CHECK(test_allocator_finish(&heap) == 0);
}

int main(void) {
    test_kinds();
    test_round_trip();
    test_typed_event();
    test_typed_send();
    test_typed_trace();
    test_larger_and_in_place();
    test_write_refused();
    test_read_refused();
    test_seq_through_ring();
    if (failures != 0) {
        fprintf(stderr, "ring_codec_test: %d checks failed\n", failures);
        return 1;
    }
    printf("ring_codec_test: ok\n");
    return 0;
}
