/* ta_compile: compile time grows linearly with the document.
 *
 * Each case generates a pathological document at size n and 8n (deep
 * nesting, many siblings, many event names, deep event names, long descriptor lists, big
 * executable content, colliding generated ids, deep namespace scopes, many
 * inline documents, many documents per image) and compares the best-of-3
 * compile times. Linear work gives a ratio near 8; every case was quadratic
 * once (ratio near 64), which the bound below catches with a wide margin
 * for timer noise and sanitizers. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <time.h>

#include "../harness/harness.h"
#include "../support/compile_util.h"

#define MAX_RATIO 24.0

typedef struct string_buffer {
    char *data;
    size_t length, capacity;
} string_buffer;

static void put(string_buffer *b, const char *format, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, format);
        int k = vsnprintf(b->data ? b->data + b->length : NULL,
                          b->data ? b->capacity - b->length : 0, format, ap);
        va_end(ap);
        if (k >= 0 && b->data && (size_t)k < b->capacity - b->length) {
            b->length += (size_t)k;
            return;
        }
        size_t capacity = b->capacity ? b->capacity * 2 : 1 << 16;
        while (capacity - b->length <= (size_t)k)
            capacity *= 2;
        b->data = (char *)realloc(b->data, capacity);
        b->capacity = capacity;
    }
}

#define HEAD "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0'>"

static void g_deep(string_buffer *b, int n) { /* rejected (state nesting), but parsed in full */
    put(b, HEAD);
    for (int i = 0; i < n; i++)
        put(b, "<state id='s%d'>", i);
    for (int i = 0; i < n; i++)
        put(b, "</state>");
    put(b, "</scxml>");
}

static void g_wide(string_buffer *b, int n) { /* distinct events; every state has code */
    put(b, HEAD);
    for (int i = 0; i < n; i++)
        put(b,
            "<state id='s%d'><onentry><log label='in'/></onentry><transition event='e%d' "
            "target='s%d'><log label='t'/></transition></state>",
            i, i, (i + 1) % n);
    put(b, "</scxml>");
}

static void g_ifs(string_buffer *b, int n) { /* one long block of jumps */
    put(b, HEAD "<state id='a'><onentry>");
    for (int i = 0; i < n; i++)
        put(b, "<if cond='c'><raise event='r'/><elseif cond='d'/><log label='z'/><else/>"
               "<foreach array='a' item='i'><raise event='q'/></foreach></if>");
    put(b, "</onentry></state></scxml>");
}

static void g_autoid(string_buffer *b, int n) { /* id-less states vs. ids like the generated ones */
    put(b, HEAD);
    for (int i = 0; i < n; i++)
        put(b, "<state/>");
    for (int i = 0; i < n; i++)
        put(b, "<state id='_ta_s%d'/>", i + 1);
    put(b, "</scxml>");
}

static void g_nsdeep(string_buffer *b,
                     int n) { /* deep foreign markup, a prefix declared per level */
    put(b, "<scxml xmlns='http://www.w3.org/2005/07/scxml' xmlns:f='urn:f' version='1.0'>"
           "<state id='a'><onentry><f:x>");
    for (int i = 0; i < n; i++)
        put(b, "<f:y xmlns:p%d='urn:%d'>", i, i);
    for (int i = 0; i < n; i++)
        put(b, "</f:y>");
    put(b, "</f:x></onentry></state></scxml>");
}

static void g_events(string_buffer *b, int n) { /* one long descriptor list */
    put(b, HEAD "<state id='a'><transition event='");
    for (int i = 0; i < n; i++)
        put(b, "ev%d.x ", i);
    put(b, "' target='a'/></state></scxml>");
}

static void g_dotted(string_buffer *b, int n) { /* deep event-trie paths, distinct at the top */
    put(b, HEAD "<state id='a'>");
    for (int i = 0; i < n; i++) {
        put(b, "<transition event='p%d", i);
        for (int k = 0; k < 62; k++)
            put(b, ".x%d", k);
        put(b, "' target='a'/>");
    }
    put(b, "</state></scxml>");
}

static void g_inline(string_buffer *b, int n) { /* many inline child documents */
    put(b, HEAD "<state id='a'>");
    for (int i = 0; i < n; i++)
        put(b, "<invoke><content><scxml><state id='x%d'/></scxml></content></invoke>", i);
    put(b, "</state></scxml>");
}

static void g_deepinline(string_buffer *b, int n) { /* inline documents nested in each other */
    put(b, HEAD);
    for (int i = 0; i < n; i++)
        put(b, "<state id='s'><invoke><content><scxml>");
    put(b, "<state id='z'/>");
    for (int i = 0; i < n; i++)
        put(b, "</scxml></content></invoke></state>");
    put(b, "</scxml>");
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Best of 3 compiles of n sources (all `text`, or `gen(n)` once). */
static double compile_time(void (*generate)(string_buffer *, int), int n, int document_count,
                           int *ok) {
    string_buffer b = {NULL, 0, 0};
    if (generate)
        generate(&b, n);
    else
        put(&b, HEAD "<state id='a'><transition event='go' target='a'/></state></scxml>");
    ta_source *source = (ta_source *)calloc((size_t)document_count, sizeof *source);
    char *names = (char *)malloc((size_t)document_count * 16);
    for (int i = 0; i < document_count; i++) {
        snprintf(names + 16 * i, 16, "c%d", i);
        source[i].name.pointer = names + 16 * i;
        source[i].name.length = (uint32_t)strlen(names + 16 * i);
        source[i].text.pointer = b.data;
        source[i].text.length = (uint32_t)b.length;
    }
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    double best = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        ta_compile_result *result = NULL;
        double t0 = now();
        *ok = ta_compile_charts(&options, (ta_source_list){source, (uint32_t)document_count},
                                &result) == TA_OK;
        double t = now() - t0;
        ta_compile_free(result);
        if (t < best) best = t;
    }
    free(names);
    free(source);
    free(b.data);
    return best;
}

static void linear(const char *name, void (*generate)(string_buffer *, int), int n, int expect_ok) {
    int ok1, ok8;
    int documents = generate ? 1 : n;
    double t1 = compile_time(generate, n, documents, &ok1);
    double t8 = compile_time(generate, 8 * n, generate ? 1 : 8 * n, &ok8);
    double ratio = t8 / (t1 > 1e-6 ? t1 : 1e-6);
    fprintf(stderr, "  %-11s n=%-6d %8.2f ms   8n %8.2f ms   x%.1f\n", name, n, t1 * 1e3, t8 * 1e3,
            ratio);
    CHECK_EQUAL(ok1, expect_ok);
    CHECK_EQUAL(ok8, expect_ok);
    CHECK(ratio < MAX_RATIO);
}

static void test_linear(void) {
    linear("deep", g_deep, 4000, 0);
    linear("wide", g_wide, 2000, 1);
    linear("ifs", g_ifs, 1000, 1);
    linear("autoid", g_autoid, 1000, 1);
    linear("nsdeep", g_nsdeep, 2000, 1);
    linear("events", g_events, 4000, 1);
    linear("dotted", g_dotted, 100, 1);
    linear("inline", g_inline, 1000, 1);
    linear("deepinline", g_deepinline, 200, 1);
    linear("documents", NULL, 500, 1);
}

int main(void) {
    RUN(test_linear);
    TEST_MAIN_END();
}
