/* ta_trace values: span IDs, traceparent text, the generator and sampling.
 * The span ID and traceparent vectors are every implementation's (docs/adr/0006
 * in the repository root, section 2). */
#include "../harness/harness.h"
#include <tinyactors/trace.h>
#include "trace/record.h"

#include <stdlib.h>
#include <string.h>

static uint64_t hex_of(const char *text) {
    return strtoull(text, NULL, 16);
}

static ta_string_view S(const char *s) {
    return (ta_string_view){s, (uint32_t)strlen(s)};
}

/* ---- span IDs ------------------------------------------------------------------------- */

/* The span ID of a step key (ta_step_key_span_id, ported bit for bit from
 * 0.1); a key with session 0 is a host's cause and returns its macrostep. */
typedef struct span_vector {
    uint64_t session, macrostep;
    uint32_t microstep;
    const char *span;
} span_vector;

static const span_vector span_vectors[] = {
    {1, 1, 0, "049e7f1d71e53363"},
    {1, 1, 1, "304917c348f35bf4"},
    {1, 2, 0, "79368ebf22c90f5b"},
    {2, 1, 0, "6412a7d0f1c7345a"},
    {1, 0, 1, "65c672b9ddbfb556"},
    {1, 0, 3, "b4ed65a25541f516"},
    {4294967297u, 7, 3, "4e8137c818db79ba"},
    {8589934593u, 4, 1, "c94a2a9027fa8e30"},
    {9007199254740993u, 1, 0, "686d5315f8778237"},
    {UINT64_MAX, UINT64_MAX, UINT32_MAX, "03938147c4082ae7"},
    {0, 4660, 0, "0000000000001234"},
    {0, 0, 0, "0000000000000000"},
};

static void test_span_ids(void) {
    for (size_t i = 0; i < sizeof span_vectors / sizeof span_vectors[0]; i++) {
        const span_vector *vector = &span_vectors[i];
        ta_span_id got = ta_step_key_span_id((ta_step_key){vector->session, vector->macrostep, vector->microstep, 0});
        CHECK_EQUAL(got, hex_of(vector->span));
        char text[17] = {0};
        ta_buffer out = {text, 16, 0};
        CHECK_EQUAL(ta_span_id_format(got, &out), TA_OK);
        CHECK_EQUAL(out.length, 16);
        CHECK(strcmp(text, vector->span) == 0);
    }
    /* the padding is not part of the key */
    ta_span_id a = ta_step_key_span_id((ta_step_key){1, 1, 0, 0});
    CHECK(a == ta_step_key_span_id((ta_step_key){1, 1, 0, 7})); /* pad is not part of the key */
}

/* ---- traceparent ---------------------------------------------------------------------- */

/* W3C traceparent text that parses (ta_traceparent_parse), and the text the
 * context formats back to (ta_traceparent_format): `format`, or NULL when it
 * is the same text. */
typedef struct traceparent_vector {
    const char *text, *trace, *parent;
    uint32_t flags;
    const char *format;
} traceparent_vector;

static const traceparent_vector traceparent_vectors[] = {
    {"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", "4bf92f3577b34da6a3ce929d0e0e4736",
     "00f067aa0ba902b7", 1, NULL},
    {" \t00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00 ", "4bf92f3577b34da6a3ce929d0e0e4736",
     "00f067aa0ba902b7", 0, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00"},
    {"cc-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-09-extra", "4bf92f3577b34da6a3ce929d0e0e4736",
     "00f067aa0ba902b7", 9, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-09"},
    {"01-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-03", "0af7651916cd43dd8448eb211c80319c",
     "b7ad6b7169203331", 3, "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-03"},
    {"00-00000000000000000000000000000001-0000000000000001-ff", "00000000000000000000000000000001",
     "0000000000000001", 255, NULL},
};

/* Text that does not parse, and why. */
static const char *const invalid_traceparents[][2] = {
    {"", "empty"},
    {"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0", "short"},
    {"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-x", "version 00 is exactly 55 characters"},
    {"cc-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01x", "no dash after the flags"},
    {"ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", "version ff"},
    {"00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01", "uppercase"},
    {"00-00000000000000000000000000000000-00f067aa0ba902b7-01", "zero trace ID"},
    {"00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01", "zero parent ID"},
    {"00_4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01", "separator"},
    {"00-4bf92f3577b34da6a3ce929d0e0e473g-00f067aa0ba902b7-01", "not hex"},
    {"0-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-", "short version"},
};

static void valid_traceparent(const traceparent_vector *vector) {
    ta_trace_context context;
    CHECK_EQUAL(ta_traceparent_parse(S(vector->text), &context), TA_OK);
    char high[17] = {0};
    memcpy(high, vector->trace, 16);
    CHECK_EQUAL(context.trace.high, hex_of(high));
    CHECK_EQUAL(context.trace.low, hex_of(vector->trace + 16));
    CHECK_EQUAL(context.parent, hex_of(vector->parent));
    CHECK_EQUAL(context.flags, vector->flags);
    const char *want = vector->format ? vector->format : vector->text;
    char out[TA_TRACEPARENT_LENGTH + 1] = {0};
    ta_buffer written = {out, TA_TRACEPARENT_LENGTH, 0};
    CHECK_EQUAL(ta_traceparent_format(&context, &written), TA_OK);
    CHECK_EQUAL(written.length, TA_TRACEPARENT_LENGTH);
    if (strcmp(out, want) != 0) fprintf(stderr, "  formats as %s, expected %s\n", out, want);
    CHECK(strcmp(out, want) == 0);
    char id[33] = {0};
    written = (ta_buffer){id, 32, 0};
    CHECK_EQUAL(ta_trace_id_format(context.trace, &written), TA_OK);
    CHECK_EQUAL(written.length, 32);
    CHECK(strcmp(id, vector->trace) == 0);
}

static void test_traceparent_vectors(void) {
    for (size_t i = 0; i < sizeof traceparent_vectors / sizeof traceparent_vectors[0]; i++)
        valid_traceparent(&traceparent_vectors[i]);
    for (size_t i = 0; i < sizeof invalid_traceparents / sizeof invalid_traceparents[0]; i++) {
        ta_trace_context context;
        ta_status status = ta_traceparent_parse(S(invalid_traceparents[i][0]), &context);
        if (status != TA_ERROR_INVALID)
            fprintf(stderr, "  accepted (%s): %s\n", invalid_traceparents[i][1], invalid_traceparents[i][0]);
        CHECK_EQUAL(status, TA_ERROR_INVALID);
    }
}

/* The IDs are read eight digits at a time: every byte value at every position
 * of both IDs and the parent is taken exactly when it is a lowercase hex digit,
 * and lands in the right nibble. */
static void test_traceparent_every_byte(void) {
    const char *example = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    for (uint32_t at = 3; at < 52; at++) {
        if (at == 35) continue; /* the dash between the IDs */
        for (int byte = 1; byte < 256; byte++) {
            char changed[56];
            memcpy(changed, example, sizeof changed);
            changed[at] = (char)byte;
            int value = byte >= '0' && byte <= '9'   ? byte - '0'
                        : byte >= 'a' && byte <= 'f' ? byte - 'a' + 10
                                                     : -1;
            ta_trace_context context;
            ta_status status = ta_traceparent_parse((ta_string_view){changed, 55}, &context);
            if (value < 0) {
                CHECK_EQUAL(status, TA_ERROR_INVALID);
                continue;
            }
            CHECK_EQUAL(status, TA_OK);
            uint32_t field = at < 19 ? 0 : at < 35 ? 1 : 2;
            uint32_t start = field == 0 ? 3 : field == 1 ? 19 : 36;
            uint64_t read = field == 0   ? context.trace.high
                            : field == 1 ? context.trace.low
                                         : context.parent;
            CHECK_EQUAL((read >> (4u * (15u - (at - start)))) & 0xfu, (uint64_t)value);
        }
    }
}

static void test_format_refusals(void) {
    char out[TA_TRACEPARENT_LENGTH];
    ta_buffer room = {out, sizeof out, 0};
    ta_trace_context none = {{0, 0}, 1, 1, 0};
    CHECK_EQUAL(ta_traceparent_format(&none, &room), TA_ERROR_INVALID); /* no trace */
    ta_trace_context no_parent = {{1, 2}, 0, 1, 0};
    CHECK_EQUAL(ta_traceparent_format(&no_parent, &room), TA_ERROR_INVALID);
    ta_trace_context ok = {{1, 2}, 3, 1, 0};
    ta_buffer small = {out, sizeof out - 1, 0};
    CHECK_EQUAL(ta_traceparent_format(&ok, &small), TA_ERROR_NO_MEMORY); /* too small */
    CHECK_EQUAL(small.length, TA_TRACEPARENT_LENGTH);                    /* what it needs */
    small.capacity = 31;
    CHECK_EQUAL(ta_trace_id_format(ok.trace, &small), TA_ERROR_NO_MEMORY);
    small.capacity = 15;
    CHECK_EQUAL(ta_span_id_format(3, &small), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(ta_traceparent_parse(S("x"), NULL), TA_ERROR_INVALID);
}

/* A step's children keep the trace and flags and have the step as parent. */
static void test_child_context(void) {
    ta_trace_context context = {{0x11, 0x22}, 0x33, TA_TRACE_FLAG_SAMPLED, 0};
    ta_step_key key = {4, 5, 6, 0};
    ta_trace_context child = ta_trace_context_child(&context, key);
    CHECK_EQUAL(child.trace.high, 0x11);
    CHECK_EQUAL(child.trace.low, 0x22);
    CHECK_EQUAL(child.flags, TA_TRACE_FLAG_SAMPLED);
    CHECK_EQUAL(child.parent, ta_step_key_span_id(key));
}

/* ---- the generator and sampling ------------------------------------------------------- */

/* A seed gives the same sequence on every host (0.1's golden values). */
static void test_generator(void) {
    ta_trace_generator generator;
    ta_trace_generator_seed(&generator, 0);
    ta_trace_id first = ta_trace_generator_next(&generator);
    CHECK_EQUAL(first.high, 0x99ec5f36cb75f2b4ull);
    CHECK_EQUAL(first.low, 0xbf6e1f784956452aull);
    ta_trace_generator_seed(&generator, 42);
    ta_trace_id again = ta_trace_generator_next(&generator);
    CHECK_EQUAL(again.high, 0x15780b2e0c2ec716ull);
    CHECK_EQUAL(again.low, 0x6104d9866d113a7eull);
    ta_trace_id second = ta_trace_generator_next(&generator);
    CHECK(second.high != again.high || second.low != again.low);
    ta_trace_generator_seed(&generator, 42);
    CHECK_EQUAL(ta_trace_generator_next(&generator).low, 0x6104d9866d113a7eull);
    /* the state is a value: copying it forks the sequence */
    ta_trace_generator copy = generator;
    CHECK_EQUAL(ta_trace_generator_next(&copy).high, ta_trace_generator_next(&generator).high);
}

static void test_sampler(void) {
    const uint64_t all = (uint64_t)1u << 56;
    CHECK(ta_trace_ratio_threshold(1, 1) == 0 && ta_trace_ratio_threshold(3, 2) == 0);
    CHECK(ta_trace_ratio_threshold(0, 1) == all && ta_trace_ratio_threshold(1, 0) == all);
    CHECK(ta_trace_ratio_threshold(1, 2) == all / 2u);
    CHECK(ta_trace_ratio_threshold(250000000u, 1000000000u) == all / 4u * 3u);
    CHECK(ta_trace_ratio_threshold(1, 3) == 48038396025285291ull); /* round(2/3 * 2^56) */
    uint64_t half = ta_trace_ratio_threshold(1, 2);
    /* the high byte of the low word is not randomness, and is ignored */
    ta_trace_id below = {0, 0xff00000000000000ull | (half - 1u)};
    ta_trace_id at = {0, half};
    CHECK(!ta_trace_sampled(below, half) && ta_trace_sampled(at, half));
    CHECK(ta_trace_sampled(below, 0) && !ta_trace_sampled(at, all));
    /* about a quarter of generated traces at 1/4 */
    ta_trace_generator generator;
    ta_trace_generator_seed(&generator, 7);
    uint64_t quarter = ta_trace_ratio_threshold(1, 4);
    int sampled = 0;
    for (int i = 0; i < 20000; i++)
        sampled += ta_trace_sampled(ta_trace_generator_next(&generator), quarter);
    CHECK(sampled > 4500 && sampled < 5500);
}

/* ---- the record schema ------------------------------------------------------------------ */

static uint64_t journal_storage[1024];

/* Writes a record of `kind` with `extra` list entries: the header's fields
 * from `n`, the body's bytes counting up from `n`. */
static int write_kind(ta_journal *journal, uint32_t kind, uint32_t extra, uint32_t n) {
    uint32_t body = ta_trace_body_size(kind);
    uint32_t size = (uint32_t)sizeof(ta_trace_record) + body + 4u * extra;
    ta_trace_record *record = ta_trace_record_reserve(journal, kind, size, 1000u + n, 2000u + n);
    if (!record) return 0;
    record->flags = TA_TRACE_FLAG_SAMPLED;
    record->actor = n;
    record->chart = n + 1;
    record->key = (ta_step_key){n, n + 1, n + 2, 0};
    record->trace = (ta_trace_id){n, n + 3};
    record->span = ta_step_key_span_id(record->key);
    record->parent = n + 4;
    record->fuel = 1000u + n;
    record->now = 2000u + n;
    unsigned char *p = ta_trace_record_body(record);
    for (uint32_t i = 0; i < body; i++)
        p[i] = (unsigned char)(n + i);
    if (kind == TA_TRACE_MACROSTEP_END) ((ta_trace_macrostep_end *)(void *)p)->state_count = extra;
    if (kind == TA_TRACE_MICROSTEP) {
        ta_trace_microstep *micro = (ta_trace_microstep *)(void *)p;
        micro->transition_count = extra > 0;
        micro->exited_count = extra > 1;
        micro->entered_count = extra - micro->transition_count - micro->exited_count;
    }
    uint32_t *list = (uint32_t *)(void *)(p + body);
    for (uint32_t i = 0; i < extra; i++)
        list[i] = n * 100u + i;
    ta_trace_record_commit(journal, record);
    return 1;
}

static void check_kind(const ta_trace_record *record, uint32_t kind, uint32_t extra, uint32_t n) {
    CHECK(record != NULL);
    if (!record) return;
    CHECK_EQUAL(record->kind, kind);
    CHECK_EQUAL(record->flags, TA_TRACE_FLAG_SAMPLED);
    CHECK_EQUAL(record->actor, n);
    CHECK_EQUAL(record->chart, n + 1);
    CHECK(record->key.session == n && record->key.macrostep == n + 1 &&
          record->key.microstep == n + 2);
    CHECK(record->trace.high == n && record->trace.low == n + 3);
    CHECK_EQUAL(record->parent, n + 4);
    CHECK(record->fuel == 1000u + n && record->now == 2000u + n);
    CHECK_EQUAL(record->size % 8, 0);
    const unsigned char *p = ta_trace_body(record);
    uint32_t body = ta_trace_body_size(kind);
    int same = 1;
    for (uint32_t i = 0; i < body; i++) {
        if (kind == TA_TRACE_MACROSTEP_END && i < 4) continue; /* the count */
        if (kind == TA_TRACE_MICROSTEP && i >= 8 && i < 20) continue;
        same &= p[i] == (unsigned char)(n + i);
    }
    CHECK(same);
    const uint32_t *list = ta_trace_list(record);
    for (uint32_t i = 0; i < extra; i++)
        CHECK_EQUAL(list[i], n * 100u + i);
}

static uint32_t list_entries(uint32_t kind) {
    return kind == TA_TRACE_MACROSTEP_END ? 3 : kind == TA_TRACE_MICROSTEP ? 5 : 0;
}

/* Every kind goes in and comes out as it was written. */
static void test_records_every_kind(void) {
    ta_journal journal;
    ta_journal_init(&journal, journal_storage, sizeof journal_storage);
    for (uint32_t kind = TA_TRACE_SPAWNED; kind < TA_TRACE_KIND_COUNT; kind++)
        CHECK(write_kind(&journal, kind, list_entries(kind), kind));
    const unsigned char *bytes;
    uint32_t length = ta_journal_peek(&journal, &bytes), cursor = 0;
    ta_trace_view view = {bytes, length, 0, NULL};
    for (uint32_t kind = TA_TRACE_SPAWNED; kind < TA_TRACE_KIND_COUNT; kind++)
        check_kind(ta_trace_next(&view, &cursor), kind, list_entries(kind), kind);
    CHECK(ta_trace_next(&view, &cursor) == NULL);
    CHECK_EQUAL(cursor, length);
    /* every body but the lists' heads is 8-aligned in size */
    for (uint32_t kind = TA_TRACE_DROPPED; kind < TA_TRACE_KIND_COUNT; kind++)
        CHECK_EQUAL(ta_trace_body_size(kind) % 8, 0);
    CHECK_EQUAL(ta_trace_body_size(0), 0);
    CHECK_EQUAL(ta_trace_body_size(TA_TRACE_KIND_COUNT), 0);
}

/* When the journal lost records, the next record written is preceded by a
 * TA_TRACE_DROPPED record with their count. */
static void test_records_dropped(void) {
    ta_journal journal;
    ta_journal_init(&journal, journal_storage, 400);
    uint32_t written = 0;
    while (write_kind(&journal, TA_TRACE_CONDITION, 0, written))
        written++;
    CHECK(!write_kind(&journal, TA_TRACE_RAISED, 0, 50));
    const unsigned char *bytes;
    uint32_t length = ta_journal_peek(&journal, &bytes);
    ta_journal_release(&journal, length);
    CHECK(write_kind(&journal, TA_TRACE_CANCELLED, 0, 60));
    length = ta_journal_peek(&journal, &bytes);
    ta_trace_view view = {bytes, length, 0, NULL};
    uint32_t cursor = 0;
    const ta_trace_record *dropped = ta_trace_next(&view, &cursor);
    CHECK(dropped && dropped->kind == TA_TRACE_DROPPED);
    if (dropped) {
        CHECK_EQUAL(((const ta_trace_dropped *)ta_trace_body(dropped))->count, 2);
        CHECK(dropped->fuel == 1060u && dropped->now == 2060u);
    }
    check_kind(ta_trace_next(&view, &cursor), TA_TRACE_CANCELLED, 0, 60);
    CHECK_EQUAL(journal.lost, 0);
    CHECK(ta_trace_next(&view, &cursor) == NULL);
}

/* The reader refuses what is not a record. */
static void test_records_malformed(void) {
    ta_journal journal;
    ta_journal_init(&journal, journal_storage, sizeof journal_storage);
    CHECK(write_kind(&journal, TA_TRACE_MICROSTEP, 5, 1));
    const unsigned char *bytes;
    uint32_t length = ta_journal_peek(&journal, &bytes), cursor;
    static unsigned char copy[512];
    memcpy(copy, bytes, length);
    ta_trace_record *record = (ta_trace_record *)(void *)copy;
    ta_trace_view view = {copy, length, 0, NULL};
    cursor = 0;
    CHECK(ta_trace_next(&view, &cursor) != NULL);
    view.length = length - 8; /* cut */
    cursor = 0;
    CHECK(ta_trace_next(&view, &cursor) == NULL);
    view.length = length;
    record->kind = 0;
    cursor = 0;
    CHECK(ta_trace_next(&view, &cursor) == NULL);
    record->kind = TA_TRACE_KIND_COUNT;
    cursor = 0;
    CHECK(ta_trace_next(&view, &cursor) == NULL);
    record->kind = TA_TRACE_MICROSTEP;
    ((ta_trace_microstep *)ta_trace_record_body(record))->entered_count =
        100; /* lists past the size */
    cursor = 0;
    CHECK(ta_trace_next(&view, &cursor) == NULL);
    ((ta_trace_microstep *)ta_trace_record_body(record))->entered_count = 3;
    record->size = 20; /* below the header */
    cursor = 0;
    CHECK(ta_trace_next(&view, &cursor) == NULL);
    record->size = (uint32_t)length + 4; /* not a multiple of 8 */
    cursor = 0;
    CHECK(ta_trace_next(&view, &cursor) == NULL);
    cursor = 4; /* not at a record */
    CHECK(ta_trace_next(&view, &cursor) == NULL);
}

/* Dynamic strings resolve against the view's string storage. */
static void test_records_strings(void) {
    static uint32_t strings[16];
    strings[4] = 1; /* references */
    strings[5] = 5; /* length */
    memcpy(&strings[6], "hello", 6);
    ta_trace_view view = {NULL, 0, 0, (const unsigned char *)strings};
    ta_string_view s =
        ta_trace_dynamic_string(&view, TA_TRACE_STRING_DYNAMIC | 2u); /* 16 bytes in */
    CHECK(s.length == 5 && memcmp(s.pointer, "hello", 5) == 0);
    CHECK_EQUAL(ta_trace_dynamic_string(&view, 7).length, 0);      /* an image string */
    CHECK_EQUAL(ta_trace_dynamic_string(&view, TA_NIL).length, 0); /* none */
}

/* kind and detail names, as the bindings spell them */
static void test_names(void) {
    for (uint32_t kind = 1; kind < TA_TRACE_KIND_COUNT; kind++)
        CHECK_EQUAL(ta_trace_kind_by_name(ta_trace_kind_name(kind)), kind);
    ta_string_view begin = ta_trace_kind_name(TA_TRACE_MACROSTEP_BEGIN);
    CHECK(begin.length == 14 && !memcmp(begin.pointer, "macrostepBegin", 14));
    CHECK_EQUAL(ta_trace_kind_name(0).length, 0u);
    CHECK_EQUAL(ta_trace_kind_name(TA_TRACE_KIND_COUNT).length, 0u);
    CHECK_EQUAL(ta_trace_kind_by_name((ta_string_view){"nonsense", 8}), 0u);
    CHECK_EQUAL(ta_trace_kind_by_name((ta_string_view){"", 0}), 0u);
    uint32_t kinds = 7;
    CHECK_EQUAL(ta_trace_detail_by_name((ta_string_view){"decisions", 9}, &kinds), TA_OK);
    CHECK_EQUAL(kinds, TA_TRACE_DECISIONS);
    CHECK_EQUAL(ta_trace_detail_by_name((ta_string_view){"off", 3}, &kinds), TA_OK);
    CHECK_EQUAL(kinds, 0u);
    CHECK_EQUAL(ta_trace_detail_by_name((ta_string_view){"loud", 4}, &kinds), TA_ERROR_INVALID);
}

int main(void) {
    RUN(test_names);
    RUN(test_span_ids);
    RUN(test_traceparent_vectors);
    RUN(test_traceparent_every_byte);
    RUN(test_format_refusals);
    RUN(test_child_context);
    RUN(test_generator);
    RUN(test_sampler);
    RUN(test_records_every_kind);
    RUN(test_records_dropped);
    RUN(test_records_malformed);
    RUN(test_records_strings);
    TEST_MAIN_END();
}
