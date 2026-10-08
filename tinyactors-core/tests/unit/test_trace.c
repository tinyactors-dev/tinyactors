/* ta_trace values: span IDs, traceparent text, the generator and sampling,
 * against the shared vectors tests/conformance/span_ids.json and
 * traceparent.json (the JS bindings run the same files through
 * tests/conformance/run_js.mjs). The vectors are read with quickjs (JSON
 * only). */
#include "../harness/harness.h"
#include <tinyactors/trace.h>
#include "quickjs.h"

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
    ta_step_key key = {u64_of(e, "session"), u64_of(e, "macrostep"), (uint32_t)u64_of(e, "microstep"),
                       0};
    str_of(e, "span", span, sizeof span);
    ta_span_id got = ta_step_key_span_id(key);
    CHECK_EQUAL(got, hex_of(span));
    char text[17] = {0};
    CHECK_EQUAL(ta_span_id_format(got, text, 16), 16);
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
    CHECK_EQUAL(ta_traceparent_format(&context, out, sizeof out), TA_TRACEPARENT_LENGTH);
    if (strcmp(out, want) != 0) fprintf(stderr, "  formats as %s, expected %s\n", out, want);
    CHECK(strcmp(out, want) == 0);
    char id[33] = {0};
    CHECK_EQUAL(ta_trace_id_format(context.trace, id, 32), 32);
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
    ta_trace_context none = {{0, 0}, 1, 1, 0};
    CHECK_EQUAL(ta_traceparent_format(&none, out, sizeof out), 0); /* no trace */
    ta_trace_context no_parent = {{1, 2}, 0, 1, 0};
    CHECK_EQUAL(ta_traceparent_format(&no_parent, out, sizeof out), 0);
    ta_trace_context ok = {{1, 2}, 3, 1, 0};
    CHECK_EQUAL(ta_traceparent_format(&ok, out, sizeof out - 1), 0); /* too small */
    CHECK_EQUAL(ta_trace_id_format(ok.trace, out, 31), 0);
    CHECK_EQUAL(ta_span_id_format(3, out, 15), 0);
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

int main(void) {
    JSRuntime *runtime = JS_NewRuntime();
    J = JS_NewContext(runtime);
    RUN(test_span_ids);
    RUN(test_traceparent_vectors);
    RUN(test_traceparent_every_byte);
    RUN(test_format_refusals);
    RUN(test_child_context);
    RUN(test_generator);
    RUN(test_sampler);
    JS_FreeContext(J);
    JS_FreeRuntime(runtime);
    TEST_MAIN_END();
}
