/*
 * JSON.parse and JSON.stringify on random inputs.
 *
 *   json_test                 each random input below, 3,000 of each, checked
 *                             without Node: every valid document parses and
 *                             writes back without a fault, the text it writes
 *                             reads back to itself, every mutated document
 *                             reads or faults, and nothing leaks
 *   json_test --print COUNT   lines for diff-node to compare with Node, each a
 *                             tag, a space, the input as a JSON string, a tab,
 *                             and what staticjs gave: the result as a JSON
 *                             string, `u` for undefined, `!S` for a fault that
 *                             is a SyntaxError in JavaScript, `!O` for any
 *                             other fault:
 *     J <document> <text>     COUNT random JSON documents (arrays and objects
 *                             nested, white space, strings with every escape,
 *                             surrogate pairs and text that is not ASCII,
 *                             numbers with fractions and exponents, long and
 *                             at the doubles' edges, integer-like and
 *                             duplicate keys): JSON.stringify(JSON.parse(t))
 *     E <document> <text>     COUNT of them with a byte or two deleted,
 *                             inserted or replaced: the same, or a SyntaxError
 *     K <source> <text>       COUNT expressions JSON.stringify(value, replacer,
 *                             space): values with undefined, functions, maps,
 *                             sets, iterators, NaN, -0 and the infinities;
 *                             array replacers and others; spaces of numbers
 *                             and strings
 *
 * The inputs are deterministic (xorshift64), and avoid what staticjs faults
 * on by design (a lone surrogate, the key __proto__, a BigInt, toJSON, a
 * space between 0 and 1 or cutting a pair): a fault on J or K is a failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "staticjs.h"

static uint64_t state = 0x2545F4914F6CDD1Dull;

static uint64_t next(void) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static uint32_t below(uint32_t n) {
    return (uint32_t)(next() % n);
}

/* ------------------------------------------------------------------------
 * Text
 */

typedef struct {
    char *ptr;
    size_t length;
    size_t capacity;
} text;

static void put_bytes(text *t, const char *bytes, size_t n) {
    if (t->length + n + 1 > t->capacity) {
        size_t capacity = t->capacity < 256 ? 256 : t->capacity;
        while (capacity < t->length + n + 1) capacity *= 2;
        t->ptr = realloc(t->ptr, capacity);
        if (t->ptr == NULL) abort();
        t->capacity = capacity;
    }
    memcpy(t->ptr + t->length, bytes, n);
    t->length += n;
    t->ptr[t->length] = '\0';
}

static void put(text *t, const char *s) {
    put_bytes(t, s, strlen(s));
}

static void put_char(text *t, char c) {
    put_bytes(t, &c, 1);
}

static void pick(text *t, const char *const *choices, size_t count) {
    put(t, choices[below((uint32_t)count)]);
}

#define PICK(t, choices) pick((t), (choices), sizeof(choices) / sizeof((choices)[0]))

/* A JSON string of `bytes` for the printed line: `"`, `\` and the
 * controls escaped, everything else as it is. */
static void put_quoted(text *t, const char *bytes, size_t n) {
    put_char(t, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];
        char escape[8];
        if (c == '"' || c == '\\') {
            put_char(t, '\\');
            put_char(t, (char)c);
        } else if (c < 0x20) {
            snprintf(escape, sizeof escape, "\\u%04x", c);
            put(t, escape);
        } else {
            put_char(t, (char)c);
        }
    }
    put_char(t, '"');
}

/* ------------------------------------------------------------------------
 * Random JSON documents
 */

static void space(text *t) {
    static const char *const spaces[] = { " ", "\t", "\n", "\r", "  ", "\r\n" };
    if (below(3) == 0) PICK(t, spaces);
}

static void digits(text *t, uint32_t count, bool leading_zero) {
    for (uint32_t i = 0; i < count; i++) put_char(t, (char)('0' + (i == 0 && !leading_zero ? 1 + below(9) : below(10))));
}

static void number(text *t) {
    char buffer[64];
    switch (below(7)) {
    case 0:
        snprintf(buffer, sizeof buffer, "%d", (int)below(2001) - 1000);
        put(t, buffer);
        break;
    case 1: {
        static const char *const zeros[] = { "0", "-0", "0.0", "-0.0", "0e5", "-0E-3", "0.000" };
        PICK(t, zeros);
        break;
    }
    case 2: {
        /* -? int frac? exp?, by the grammar. */
        if (below(2) == 0) put_char(t, '-');
        if (below(4) == 0) put_char(t, '0');
        else digits(t, 1 + below(20), false);
        if (below(2) == 0) {
            put_char(t, '.');
            digits(t, 1 + below(20), true);
        }
        if (below(2) == 0) {
            put_char(t, below(2) == 0 ? 'e' : 'E');
            if (below(2) == 0) put_char(t, below(2) == 0 ? '+' : '-');
            digits(t, 1 + below(3), true);
        }
        break;
    }
    case 3: {
        uint64_t bits = next();
        double d;
        memcpy(&d, &bits, sizeof d);
        if (d != d || d - d != 0) d = 1.5;
        snprintf(buffer, sizeof buffer, "%.17g", d);
        put(t, buffer);
        break;
    }
    case 4:
        /* At the edges of the doubles' range. */
        if (below(2) == 0) put_char(t, '-');
        digits(t, 1 + below(3), false);
        if (below(2) == 0) {
            put_char(t, '.');
            digits(t, 1 + below(17), true);
        }
        snprintf(buffer, sizeof buffer, "e%d", below(2) == 0 ? 280 + (int)below(60) : -300 - (int)below(50));
        put(t, buffer);
        break;
    case 5:
        /* Long: past what a double holds. */
        digits(t, 20 + below(40), false);
        put_char(t, '.');
        digits(t, 1 + below(40), true);
        break;
    default: {
        /* 17 significant digits, as a double prints. */
        uint64_t bits = next() & 0x3FFFFFFFFFFFFFFFull;
        double d;
        memcpy(&d, &bits, sizeof d);
        snprintf(buffer, sizeof buffer, "%.16e", d);
        put(t, buffer);
        break;
    }
    }
}

static void hex4(text *t, uint32_t unit) {
    char buffer[8];
    snprintf(buffer, sizeof buffer, below(2) == 0 ? "\\u%04x" : "\\u%04X", unit);
    put(t, buffer);
}

/* A string's text between its quotes, as JSON writes it. */
static void json_string_body(text *t, uint32_t most) {
    static const char *const escapes[] = { "\\\"", "\\\\", "\\/", "\\b", "\\f", "\\n", "\\r", "\\t" };
    static const char *const raw[] = { "\xc3\xa9", "\xe6\x97\xa5", "\xe6\x9c\xac", "\xf0\x9f\x98\x80", "\x7f", "\xc3\x9f",
                                       "\xe2\x82\xac", "\xe2\x80\xa8", "\xef\xbb\xbf" };
    uint32_t n = below(most + 1);
    for (uint32_t i = 0; i < n; i++) {
        switch (below(10)) {
        case 0:
        case 1:
        case 2: {
            char c = (char)(0x20 + below(0x5F));
            if (c == '"' || c == '\\') c = 'q';
            put_char(t, c);
            break;
        }
        case 3:
            PICK(t, escapes);
            break;
        case 4: {
            uint32_t unit = below(2) == 0 ? below(0xD800) : 0xE000 + below(0x2000);
            hex4(t, unit);
            break;
        }
        case 5:
            hex4(t, 0xD800 + below(0x400));
            hex4(t, 0xDC00 + below(0x400));
            break;
        case 6:
            PICK(t, raw);
            break;
        case 7:
            hex4(t, below(0x20));
            break;
        default:
            put_char(t, (char)('0' + below(10)));
            break;
        }
    }
}

static void json_value(text *t, uint32_t depth) {
    static const char *const words[] = { "true", "false", "null" };
    static const char *const keys[] = { "\"0\"", "\"1\"", "\"2\"", "\"10\"", "\"01\"", "\"-1\"", "\"1.5\"", "\"4294967294\"",
                                        "\"4294967295\"", "\"a\"", "\"b\"", "\"\"", "\"\\u0061\"" };
    space(t);
    /* A document is most often an array or an object. */
    switch (depth >= 4 ? below(4) : depth == 0 && below(4) != 0 ? 4 + below(4) : below(8)) {
    case 0:
    case 3:
        number(t);
        break;
    case 1:
        put_char(t, '"');
        json_string_body(t, 12);
        put_char(t, '"');
        break;
    case 2:
        PICK(t, words);
        break;
    case 4:
    case 5: {
        put_char(t, '[');
        uint32_t n = below(5);
        for (uint32_t i = 0; i < n; i++) {
            if (i > 0) put_char(t, ',');
            json_value(t, depth + 1);
        }
        if (n == 0) space(t);
        put_char(t, ']');
        break;
    }
    default: {
        put_char(t, '{');
        uint32_t n = below(6);
        for (uint32_t i = 0; i < n; i++) {
            if (i > 0) put_char(t, ',');
            space(t);
            if (below(3) == 0) {
                PICK(t, keys);
            } else {
                put_char(t, '"');
                json_string_body(t, 6);
                put_char(t, '"');
            }
            space(t);
            put_char(t, ':');
            json_value(t, depth + 1);
        }
        if (n == 0) space(t);
        put_char(t, '}');
        break;
    }
    }
    space(t);
}

/* A byte or two of `t` deleted, inserted or replaced; only ASCII, at a
 * character's boundary, so that it stays well-formed UTF-8. */
static void mutate(text *t) {
    static const char inserted[] = ",:[]{}\"\\ 019-+.eEtfnua/\x01\t";
    uint32_t times = 1 + below(2);
    for (uint32_t k = 0; k < times; k++) {
        size_t at = t->length == 0 ? 0 : next() % t->length;
        while (at < t->length && ((unsigned char)t->ptr[at] & 0xC0) == 0x80) at++;
        char c = inserted[below(sizeof inserted - 1)];
        uint32_t what = below(3);
        bool ascii = at < t->length && (unsigned char)t->ptr[at] < 0x80;
        if (what == 0 && ascii) {
            memmove(t->ptr + at, t->ptr + at + 1, t->length - at);
            t->length--;
        } else if (what == 1 && ascii) {
            t->ptr[at] = c;
        } else {
            put_char(t, ' ');
            memmove(t->ptr + at + 1, t->ptr + at, t->length - at - 1);
            t->ptr[at] = c;
        }
    }
}

/* ------------------------------------------------------------------------
 * Random JSON.stringify calls, as JavaScript source
 */

static const char *const KEYS[] = { "a", "b", "c", "\xc3\xa9", "1", "2", "10", "01", "k\\n", "\xe6\x97\xa5\xe6\x9c\xac", "", "x y" };

static void js_string(text *t, uint32_t most) {
    static const char *const raw[] = { "\xc3\xa9", "\xe6\x97\xa5", "\xf0\x9f\x98\x80", "\x7f", "\\\"", "\\\\", "\\n", "\\t" };
    put_char(t, '"');
    uint32_t n = below(most + 1);
    for (uint32_t i = 0; i < n; i++) {
        switch (below(5)) {
        case 0:
            PICK(t, raw);
            break;
        case 1: {
            char escape[8];
            snprintf(escape, sizeof escape, "\\u%04x", below(0x20));
            put(t, escape);
            break;
        }
        default: {
            char c = (char)(0x20 + below(0x5F));
            if (c == '"' || c == '\\') c = 'q';
            put_char(t, c);
            break;
        }
        }
    }
    put_char(t, '"');
}

static void js_number(text *t) {
    static const char *const special[] = { "NaN", "Infinity", "-Infinity", "-0", "0", "1e21", "5e-324", "0.1", "-1.5",
                                           "1.7976931348623157e308", "123456789012345680000", "1e-7" };
    char buffer[64];
    if (below(2) == 0) {
        PICK(t, special);
        return;
    }
    uint64_t bits = next();
    double d;
    memcpy(&d, &bits, sizeof d);
    if (d != d || d - d != 0) d = -2.5;
    snprintf(buffer, sizeof buffer, "%.17g", d);
    put(t, buffer);
}

static void js_value(text *t, uint32_t depth) {
    static const char *const others[] = { "true", "false", "null", "undefined", "x => x", "Math.max",
                                          "new Map([[1, 2]])", "new Set([1, \"a\"])", "new Map([[\"k\", 1]]).keys()",
                                          "new Map()" };
    switch (depth >= 4 ? below(4) : depth == 0 && below(4) != 0 ? 4 + below(4) : below(8)) {
    case 0:
        js_number(t);
        break;
    case 1:
        js_string(t, 8);
        break;
    case 2:
    case 3:
        PICK(t, others);
        break;
    case 4:
    case 5: {
        put_char(t, '[');
        uint32_t n = below(5);
        for (uint32_t i = 0; i < n; i++) {
            if (i > 0) put(t, ", ");
            js_value(t, depth + 1);
        }
        put_char(t, ']');
        break;
    }
    default: {
        put_char(t, '{');
        uint32_t n = below(5);
        uint32_t used = 0;
        bool first = true;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t k = below(sizeof KEYS / sizeof KEYS[0]);
            if ((used & (1u << k)) != 0) continue;
            used |= 1u << k;
            if (!first) put(t, ", ");
            first = false;
            put_char(t, '"');
            put(t, KEYS[k]);
            put(t, "\": ");
            js_value(t, depth + 1);
        }
        put_char(t, '}');
        break;
    }
    }
}

static void js_call(text *t) {
    static const char *const replacers[] = { "null", "undefined", "5", "\"a\"", "{a: 1}", "new Set([\"a\"])" };
    static const char *const spaces[] = { "-1", "0", "1", "2", "3", "4", "7", "10", "11", "2.5", "-0.5", "NaN", "Infinity",
                                          "\"\"", "\" \"", "\"\\t\"", "\"--\"", "\"abcdefghijkl\"", "\"\xc3\xa9\"",
                                          "\"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\"",
                                          "\"\xf0\x9f\x98\x80\xf0\x9f\x98\x80\"",
                                          "\"\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\xf0\x9f\x98\x80\"",
                                          "true", "null", "[2]" };
    put(t, "JSON.stringify(");
    js_value(t, 0);
    uint32_t arguments = below(3);
    if (arguments >= 1) {
        put(t, ", ");
        if (below(2) == 0) {
            PICK(t, replacers);
        } else {
            put_char(t, '[');
            uint32_t n = below(5);
            for (uint32_t i = 0; i < n; i++) {
                static const char *const junk[] = { "1", "2", "10", "true", "null", "[\"a\"]" };
                if (i > 0) put(t, ", ");
                if (below(4) == 0) {
                    PICK(t, junk);
                } else {
                    put_char(t, '"');
                    put(t, KEYS[below(sizeof KEYS / sizeof KEYS[0])]);
                    put_char(t, '"');
                }
            }
            put_char(t, ']');
        }
    }
    if (arguments == 2) {
        put(t, ", ");
        PICK(t, spaces);
    }
    put_char(t, ')');
}

/* ------------------------------------------------------------------------
 * Evaluation
 */

static long live = 0;

static void *counted_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    void *p = malloc(size);
    if (p != NULL) live++;
    return p;
}

static void counted_free(void *self, const sjs_allocator_free_params *params) {
    (void)self;
    if (params->ptr != NULL) live--;
    free(params->ptr);
}

static const sjs_allocator_vtable counted_vtable = { counted_alloc, counted_free };

typedef struct {
    sjs_context *context;
    sjs_vm *vm;
    sjs_program *round_trip;   /* JSON.stringify(JSON.parse(t)) */
} runner;

/* What an evaluation gave, as a line's last field. */
typedef enum { GAVE_TEXT, GAVE_UNDEFINED, GAVE_SYNTAX, GAVE_OTHER } gave_kind;

typedef struct {
    gave_kind kind;
    text text;
    char message[256];
} gave;

static void run_program(runner *r, sjs_program *program, const sjs_value *globals, gave *out) {
    out->kind = GAVE_OTHER;
    out->text.length = 0;
    out->message[0] = '\0';
    sjs_run_params params = { program, globals, globals == NULL ? 0u : 1u };
    if (sjs_vm_start(r->vm, &params) != SJS_OK) abort();
    sjs_run run = sjs_vm_run(r->vm, UINT64_MAX);
    if (run == SJS_RUN_FAULT) {
        sjs_error error;
        sjs_vm_error(r->vm, &error);
        snprintf(out->message, sizeof out->message, "%.*s [%.*s]", (int)error.message.length, error.message.ptr,
                 (int)error.subject.length, error.subject.ptr);
        out->kind = strstr(out->message, "SyntaxError") != NULL ? GAVE_SYNTAX : GAVE_OTHER;
        return;
    }
    sjs_value result;
    if (run != SJS_RUN_DONE || sjs_vm_take_result(r->vm, &result) != SJS_OK) abort();
    if (result.kind == SJS_STRING) {
        sjs_str s = sjs_string_text(result);
        out->kind = GAVE_TEXT;
        put_bytes(&out->text, s.ptr, s.length);
    } else if (result.kind == SJS_UNDEFINED) {
        out->kind = GAVE_UNDEFINED;
    } else {
        snprintf(out->message, sizeof out->message, "not a string: kind %d", result.kind);
    }
    sjs_release(r->context, result);
}

static void round_trip(runner *r, const text *document, gave *out) {
    sjs_value t;
    if (sjs_string_new(r->context, (sjs_str){ document->ptr, (uint32_t)document->length }, &t) != SJS_OK) abort();
    run_program(r, r->round_trip, &t, out);
    sjs_release(r->context, t);
}

static void stringify_call(runner *r, const text *source, gave *out) {
    sjs_program *program;
    sjs_error error;
    sjs_compile_params params = { { r->context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
                                  { source->ptr, (uint32_t)source->length }, &error };
    if (sjs_compile(&params, &program) != SJS_OK) {
        fprintf(stderr, "json_test: does not compile: %s\n  %.*s\n", source->ptr, (int)error.message.length, error.message.ptr);
        exit(1);
    }
    run_program(r, program, NULL, out);
    sjs_program_release(program);
}

static void runner_init(runner *r) {
    sjs_context_config config = { { NULL, &counted_vtable }, SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&r->context, &config) != SJS_OK) abort();
    if (sjs_vm_init(&r->vm, &(sjs_vm_config){ r->context, false, false }) != SJS_OK) abort();
    static const sjs_str globals[] = { { "t", 1 } };
    sjs_compile_params params = { { r->context, SJS_GOAL_EXPRESSION, globals, 1, { NULL, NULL }, false },
                                  SJS_STR("JSON.stringify(JSON.parse(t))"), NULL };
    if (sjs_compile(&params, &r->round_trip) != SJS_OK) abort();
}

static void runner_destroy(runner *r) {
    sjs_program_release(r->round_trip);
    sjs_vm_destroy(r->vm);
    sjs_context_destroy(r->context);
}

static void put_gave(text *line, const gave *g) {
    switch (g->kind) {
    case GAVE_TEXT: put_quoted(line, g->text.ptr == NULL ? "" : g->text.ptr, g->text.length); break;
    case GAVE_UNDEFINED: put(line, "u"); break;
    case GAVE_SYNTAX: put(line, "!S"); break;
    default: put(line, "!O"); break;
    }
}

static void print_line(char tag, const text *input, const gave *g) {
    text line = { 0 };
    put_char(&line, tag);
    put_char(&line, ' ');
    put_quoted(&line, input->ptr, input->length);
    put_char(&line, '\t');
    put_gave(&line, g);
    puts(line.ptr);
    free(line.ptr);
}

/* ------------------------------------------------------------------------
 * The checks
 */

static int failures = 0;

static void fail(const char *what, const text *input, const gave *g) {
    if (failures++ < 20) fprintf(stderr, "json_test: %s\n  input: %s\n  fault: %s\n", what, input->ptr, g->message);
}

static void check(long count, bool print) {
    runner r;
    runner_init(&r);
    text input = { 0 };
    gave g = { 0 };
    gave again = { 0 };
    for (long i = 0; i < count; i++) {
        input.length = 0;
        json_value(&input, 0);
        round_trip(&r, &input, &g);
        if (print) print_line('J', &input, &g);
        if (g.kind != GAVE_TEXT) {
            fail("a valid document faults", &input, &g);
            continue;
        }
        /* What it writes reads back to itself. */
        round_trip(&r, &g.text, &again);
        if (again.kind != GAVE_TEXT || again.text.length != g.text.length || memcmp(again.text.ptr, g.text.ptr, g.text.length) != 0) {
            fail("what stringify writes does not read back to itself", &g.text, &again);
        }
    }
    for (long i = 0; i < count; i++) {
        input.length = 0;
        json_value(&input, 0);
        mutate(&input);
        round_trip(&r, &input, &g);
        if (print) print_line('E', &input, &g);
        if (g.kind == GAVE_UNDEFINED) fail("a mutated document gives undefined", &input, &g);
    }
    for (long i = 0; i < count; i++) {
        input.length = 0;
        js_call(&input);
        stringify_call(&r, &input, &g);
        if (print) print_line('K', &input, &g);
        if (g.kind == GAVE_SYNTAX || g.kind == GAVE_OTHER) fail("a stringify call faults", &input, &g);
    }
    free(input.ptr);
    free(g.text.ptr);
    free(again.text.ptr);
    runner_destroy(&r);
    if (live != 0) {
        fprintf(stderr, "json_test: %ld allocations left behind\n", live);
        failures++;
    }
}

int main(int argc, char **argv) {
    bool print = argc == 3 && strcmp(argv[1], "--print") == 0;
    check(print ? atol(argv[2]) : 3000, print);
    if (failures != 0) {
        fprintf(stderr, "json_test: %d failure(s)\n", failures);
        return 1;
    }
    if (!print) printf("json_test: random documents read and write back, mutated ones read or fault, stringify calls run\n");
    return 0;
}
