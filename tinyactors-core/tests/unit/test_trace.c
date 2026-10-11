/* ta_trace values: span IDs, traceparent text, the generator and sampling,
 * against the shared vectors tests/conformance/span_ids.json and
 * traceparent.json. The vectors are read with quickjs (JSON only). */
#include "../harness/harness.h"
#include <tinyactors/trace.h>
#include "quickjs.h"
#include "trace/record.h"

#include <stdlib.h>
#include <string.h>

#ifndef TA_SPAN_VECTORS
#define TA_SPAN_VECTORS "tests/conformance/span_ids.json"
#endif
#ifndef TA_TRACEPARENT_VECTORS
#define TA_TRACEPARENT_VECTORS "tests/conformance/traceparent.json"
#endif

static JSContext *J;

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) b[n] = 0;
    return b;
}

static JSValue load(const char *path) {
    char *text = slurp(path);
    CHECK(text != NULL);
    if (!text) return JS_UNDEFINED;
    JSValue v = JS_ParseJSON(J, text, strlen(text), path);
    free(text);
    return v;
}

static const char *str_of(JSValueConst o, const char *k, char *buffer, size_t capacity) {
    JSValue v = JS_GetPropertyStr(J, o, k);
    buffer[0] = 0;
    int has = JS_IsString(v);
    if (has) {
        size_t n;
        const char *s = JS_ToCStringLen(J, &n, v);
        if (n < capacity) {
            memcpy(buffer, s, n);
            buffer[n] = 0;
        }
        JS_FreeCString(J, s);
    }
    JS_FreeValue(J, v);
    return has ? buffer : NULL;
}

static uint64_t u64_of(JSValueConst o, const char *k) {
    char text[32];
    if (str_of(o, k, text, sizeof text)) return strtoull(text, NULL, 10);
    JSValue v = JS_GetPropertyStr(J, o, k);
    double d = 0;
    JS_ToFloat64(J, &d, v);
    JS_FreeValue(J, v);
    return (uint64_t)d;
}

static uint64_t hex_of(const char *text) {
    return strtoull(text, NULL, 16);
}

static ta_string_view S(const char *s) {
    return (ta_string_view){s, (uint32_t)strlen(s)};
}

static void each(JSValueConst root, const char *list, void (*function)(JSValueConst)) {
    JSValue arr = JS_GetPropertyStr(J, root, list);
    int64_t n = 0;
    JS_GetLength(J, arr, &n);
    CHECK(n > 0);
    for (int64_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(J, arr, (uint32_t)i);
        function(e);
        JS_FreeValue(J, e);
    }
    JS_FreeValue(J, arr);
}

/* ---- span IDs ------------------------------------------------------------------------- */

static void span_vector(JSValueConst e) {
    char span[32];
    ta_step_key key = {u64_of(e, "session"), u64_of(e, "macrostep"),
                       (uint32_t)u64_of(e, "microstep"), 0};
    str_of(e, "span", span, sizeof span);
    ta_span_id got = ta_step_key_span_id(key);
    CHECK_EQUAL(got, hex_of(span));
    char text[17] = {0};
    ta_buffer out = {text, 16, 0};
    CHECK_EQUAL(ta_span_id_format(got, &out), TA_OK);
    CHECK_EQUAL(out.length, 16);
    CHECK(strcmp(text, span) == 0);
}

static void test_span_ids(void) {
    JSValue root = load(TA_SPAN_VECTORS);
    each(root, "keys", span_vector);
    JS_FreeValue(J, root);
    /* the padding is not part of the key */
    ta_span_id a = ta_step_key_span_id((ta_step_key){1, 1, 0, 0});
    CHECK(a == ta_step_key_span_id((ta_step_key){1, 1, 0, 7})); /* pad is not part of the key */
}

/* ---- traceparent ---------------------------------------------------------------------- */

static void valid_vector(JSValueConst e) {
    char text[128], trace[40], parent[24], format[128];
    str_of(e, "text", text, sizeof text);
    str_of(e, "trace", trace, sizeof trace);
    str_of(e, "parent", parent, sizeof parent);
    ta_trace_context context;
    CHECK_EQUAL(ta_traceparent_parse(S(text), &context), TA_OK);
    char high[17] = {0};
    memcpy(high, trace, 16);
    CHECK_EQUAL(context.trace.high, hex_of(high));
    CHECK_EQUAL(context.trace.low, hex_of(trace + 16));
    CHECK_EQUAL(context.parent, hex_of(parent));
    CHECK_EQUAL(context.flags, u64_of(e, "flags"));
    const char *want = str_of(e, "format", format, sizeof format) ? format : text;
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
    CHECK(strcmp(id, trace) == 0);
}

static void invalid_vector(JSValueConst e) {
    char text[128], why[128];
    str_of(e, "text", text, sizeof text);
    str_of(e, "why", why, sizeof why);
    ta_trace_context context;
    ta_status status = ta_traceparent_parse(S(text), &context);
    if (status != TA_ERROR_INVALID) fprintf(stderr, "  accepted (%s): %s\n", why, text);
    CHECK_EQUAL(status, TA_ERROR_INVALID);
}

static void test_traceparent_vectors(void) {
    JSValue root = load(TA_TRACEPARENT_VECTORS);
    each(root, "valid", valid_vector);
    each(root, "invalid", invalid_vector);
    JS_FreeValue(J, root);
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
    JSRuntime *runtime = JS_NewRuntime();
    J = JS_NewContext(runtime);
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
    JS_FreeContext(J);
    JS_FreeRuntime(runtime);
    TEST_MAIN_END();
}
