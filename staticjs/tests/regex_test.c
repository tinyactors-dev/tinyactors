/*
 * Regular expressions on random patterns (docs/regex.md, 11.3).
 *
 *   regex_test                 2,000 random literals, each compiled from its
 *                              source pushed whole and a byte at a time: both
 *                              must classify it alike, and nothing leaks
 *   regex_test --print COUNT   lines for diff-node to compare with Node, each
 *                              a tag, a space, then fields separated by tabs:
 *     X <pattern> <flags> - <class>
 *                              COUNT random literals /pattern/flags, the
 *                              pattern as a JSON string, and how staticjs
 *                              classifies the literal: exact, unsupported or
 *                              syntax. Node must refuse every syntax one with
 *                              a SyntaxError and read every other.
 *
 * Patterns come from a grammar over a small alphabet that reaches the edges
 * of the classification (docs/regex.md, section 8): Annex B forms, the
 * escapes of each mode, classes and ranges, groups, names and references,
 * lookaround, property escapes, quantifiers and their errors, every flag.
 * The inputs are deterministic (xorshift64). With REGEX_TEST_TRACE set in
 * the environment, each literal goes to stderr before it is compiled, to
 * find one that stops the program.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "staticjs.h"

static uint64_t state = 0x9E3779B97F4A7C15ull;

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

/* As a JSON string. */
static void put_quoted(text *t, const char *s, size_t n) {
    put(t, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char e[3] = { '\\', (char)c, 0 };
            put(t, e);
        } else if (c < 0x20) {
            char e[8];
            snprintf(e, sizeof e, "\\u%04x", c);
            put(t, e);
        } else {
            put_bytes(t, (const char *)&s[i], 1);
        }
    }
    put(t, "\"");
}

/* ------------------------------------------------------------------------
 * Patterns
 */

/* Atoms every mode reads, and then the edges. */
static const char *const COMMON[] = {
    "a", "b", "A", "é", "😀", "\\n", ".", "\\d", "\\w", "\\s", "\\D", "\\W", "\\S", "\\.", "\\/", "\\\\", "\\*",
    "[ab]", "[^a]", "[a-c]", "[é😀]", "[\\b]", "\\u0061", "\\x61", "\\0", "\\b", "\\B", "^", "$", "ſ", "K", "_", "1",
    " ", "\\cA", "[^\\W]", "[\\d]", "[^\\s]", "[_a-cA]", "\\u00e9",
};

static const char *const ATOMS[] = {
    "a", "b", "A", "é", "😀", "\\n", ".", "\\d", "\\w", "\\s", "\\D", "\\W", "\\S", "\\.", "\\/", "\\\\", "\\*",
    "[ab]", "[^a]", "[a-c]", "[é😀]", "[😀-😁]", "[\\d-z]", "[a-\\w]", "[]", "[^]", "[\\b]", "[\\-]", "[a\\-z]", "[z-a]",
    "[\\B]", "[\\k]", "[\\c1]", "[\\0]", "[\\00]", "[\\1]", "[^\\W]", "[[a]", "[a]]", "[(]", "[a&&b]", "[a--b]",
    "[\\q{ab}]", "[^\\q{ab}]", "[[a]--[b]]", "[a-z&&b]", "[!!]",
    "\\u0061", "\\u00e9", "\\uD83D\\uDE00", "\\uD83D", "\\x61", "\\x6", "\\u{61}", "\\u{1F600}", "\\u{110000}", "\\u",
    "\\0", "\\00", "\\08", "\\1", "\\2", "\\8", "\\k<n>", "\\k<q>", "\\k", "\\c", "\\cA", "\\ca", "\\c1", "\\-", "\\a",
    "\\p{L}", "\\P{Lu}", "\\p{Script=Latin}", "\\p{sc=Foo}", "\\p{Foo}", "\\p{RGI_Emoji}", "\\pL", "\\p",
    "{", "}", "]", "{1}", "{,2}", "\\b", "\\B", "^", "$", "(?=a)", "(?!a)", "(?<=a)", "(?<!a)", "(?i:a)", "(?a)",
    "\\q", "ſ", "K", "_", "1", " ",
};

static void pattern(text *t, uint32_t depth);

static void atom(text *t, uint32_t depth) {
    uint32_t k = below(depth > 3 ? 4 : 10);
    if (k < 6 || depth > 3) {
        if (below(6) == 0) put(t, ATOMS[below((uint32_t)(sizeof ATOMS / sizeof ATOMS[0]))]);
        else put(t, COMMON[below((uint32_t)(sizeof COMMON / sizeof COMMON[0]))]);
        return;
    }
    static const char *const opens[] = { "(", "(?:", "(?<n>", "(?<m>", "(", "(?:", "(?<n>", "(?=" };
    put(t, opens[below(8)]);
    pattern(t, depth + 1);
    if (below(60) != 0) put(t, ")");
}

static void quantifier(text *t) {
    static const char *const q[] = { "*", "+", "?", "{2}", "{1,2}", "{0,}", "{0,1}", "{3}", "*?", "+?", "??", "{1,2}?",
                                     "{2,1}", "**", "{2147483648,2147483647}", "{1" };
    put(t, q[below(below(8) == 0 ? (uint32_t)(sizeof q / sizeof q[0]) : 12u)]);
}

static void pattern(text *t, uint32_t depth) {
    uint32_t terms = below(4) + (depth == 0 ? 1 : 0);
    for (uint32_t i = 0; i < terms; i++) {
        atom(t, depth);
        if (below(3) == 0) quantifier(t);
        if (below(9) == 0) put(t, "|");
    }
}

static void flags(text *t) {
    static const char letters[] = "dgimsuvy";
    for (uint32_t k = 0; k < 8; k++) {
        if (below(k == 2 || k == 5 ? 3 : k == 6 ? 12 : 7) == 0) put_bytes(t, &letters[k], 1);
    }
    if (below(80) == 0) put(t, "g");
    if (below(120) == 0) put(t, "x");
}

/* A literal's body: not empty, and not one a JavaScript lexer reads as a
 * comment or ends early (a raw `/` is escaped by the atoms). */
static void body(text *t) {
    pattern(t, 0);
    if (t->length == 0 || t->ptr[0] == '*') {
        text fixed = { 0 };
        put(&fixed, "a");
        put_bytes(&fixed, t->ptr == NULL ? "" : t->ptr, t->length);
        t->length = 0;
        put_bytes(t, fixed.ptr, fixed.length);
        free(fixed.ptr);
    }
}

/* ------------------------------------------------------------------------
 * Classifying a literal by compiling it
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

static const char *code_name(sjs_error_code code) {
    switch (code) {
    case SJS_ERROR_NONE: return "exact";
    case SJS_ERROR_SYNTAX: return "syntax";
    case SJS_ERROR_UNSUPPORTED: return "unsupported";
    case SJS_ERROR_LIMIT: return "limit";
    case SJS_ERROR_MEMORY: return "memory";
    default: return "other";
    }
}

/* Compile `source`, pushed in chunks of `chunk` bytes (0: whole). */
static sjs_error_code classify(sjs_context *context, const text *source, uint32_t chunk) {
    sjs_compiler *compiler;
    sjs_compiler_config config = { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false };
    if (sjs_compiler_init(&compiler, &config) != SJS_OK) abort();
    sjs_parser *parser;
    sjs_parser_config pc = { { NULL, &counted_vtable }, SJS_LIMITS_DEFAULT, SJS_GOAL_EXPRESSION, sjs_compiler_sink(compiler) };
    if (sjs_parser_init(&parser, &pc) != SJS_OK) abort();
    sjs_result r = SJS_OK;
    size_t step = chunk == 0 ? source->length : chunk;
    for (size_t i = 0; r == SJS_OK && i < source->length; i += step) {
        size_t n = source->length - i < step ? source->length - i : step;
        r = sjs_parser_push(parser, (sjs_bytes){ (const uint8_t *)source->ptr + i, (uint32_t)n });
    }
    if (r == SJS_OK) r = sjs_parser_finish(parser);
    sjs_error_code code = SJS_ERROR_NONE;
    sjs_error error;
    if (r != SJS_OK) {
        if (sjs_parser_error(parser, &error) == SJS_OK) code = error.code;
        else if (sjs_compiler_error(compiler, &error) == SJS_OK) code = error.code;
        else code = SJS_ERROR_FAULT;
    } else {
        sjs_program *program;
        if (sjs_compiler_finish(compiler, &program) != SJS_OK) {
            code = sjs_compiler_error(compiler, &error) == SJS_OK ? error.code : SJS_ERROR_FAULT;
        } else {
            sjs_program_release(program);
        }
    }
    sjs_parser_destroy(parser);
    sjs_compiler_destroy(compiler);
    return code;
}

static int failures = 0;

/* ------------------------------------------------------------------------
 * Matching: the methods a pattern can be used with, on random inputs
 */

static const char *const PIECES[] = { "a", "b", "A", "B", "é", "😀", "\n", " ", "_", "1", "ſ", "K", "ab", "aa", "\r\n" };

static void input(text *t) {
    uint32_t n = below(8);
    for (uint32_t i = 0; i < n; i++) put(t, PIECES[below((uint32_t)(sizeof PIECES / sizeof PIECES[0]))]);
}

/* The program: each method on the global s, as JSON. The flags lose g and
 * y, which test, search and replace without g fault on or change, and the
 * global methods get g. */
static void methods_source(text *src, const text *pat, const text *flag) {
    text f = { 0 };
    for (size_t i = 0; i < flag->length; i++) {
        if (flag->ptr[i] != 'g' && flag->ptr[i] != 'y') put_bytes(&f, &flag->ptr[i], 1);
    }
    const char *fs = f.ptr == NULL ? "" : f.ptr;
    char head[128];
    put(src, "JSON.stringify([s.search(/");
    put_bytes(src, pat->ptr, pat->length);
    snprintf(head, sizeof head, "/%s), s.replace(/", fs);
    put(src, head);
    put_bytes(src, pat->ptr, pat->length);
    snprintf(head, sizeof head, "/%s, \"<$&|$1|$2>\"), s.split(/", fs);
    put(src, head);
    put_bytes(src, pat->ptr, pat->length);
    snprintf(head, sizeof head, "/%s), s.match(/", fs);
    put(src, head);
    put_bytes(src, pat->ptr, pat->length);
    snprintf(head, sizeof head, "/%sg), s.replace(/", fs);
    put(src, head);
    put_bytes(src, pat->ptr, pat->length);
    snprintf(head, sizeof head, "/%sg, \"<$&>\"), /", fs);
    put(src, head);
    put_bytes(src, pat->ptr, pat->length);
    snprintf(head, sizeof head, "/%s.test(s), (m => m && [m.index, [...m], m.groups, m.indices])(/", fs);
    put(src, head);
    put_bytes(src, pat->ptr, pat->length);
    snprintf(head, sizeof head, "/%s.exec(s)), [...s.matchAll(/", fs);
    put(src, head);
    put_bytes(src, pat->ptr, pat->length);
    snprintf(head, sizeof head, "/%sg)].map(m => [m.index, ...m]), s.replace(/", fs);
    put(src, head);
    put_bytes(src, pat->ptr, pat->length);
    snprintf(head, sizeof head, "/%sg, (...a) => JSON.stringify(a))])", fs);
    put(src, head);
    free(f.ptr);
}

/* What a run gave: the result's text, or a fault's message. */
typedef struct {
    bool fault;
    text out;
    uint64_t fuel;
    uint64_t pauses;    /* the hash of the fuel used at each pause */
} ran;

typedef struct {
    sjs_context *context;
    sjs_program *program;
    bool loops;         /* every match a loop frame */
    uint64_t fuel;      /* per sjs_vm_run */
    bool no_dfa;        /* every match by the automaton, not a test's cache of its steps */
} runner;

static void run(const runner *r, const text *s, ran *out) {
    out->out.length = 0;
    out->fault = false;
    sjs_vm *vm;
    sjs_vm_config config = { r->context, r->loops, r->no_dfa };
    if (sjs_vm_init(&vm, &config) != SJS_OK) abort();
    sjs_value global;
    if (sjs_string_new(r->context, (sjs_str){ s->ptr == NULL ? "" : s->ptr, (uint32_t)s->length }, &global) != SJS_OK) abort();
    sjs_run_params params = { r->program, &global, 1 };
    if (sjs_vm_start(vm, &params) != SJS_OK) abort();
    sjs_run state;
    out->pauses = 14695981039346656037ull;
    while ((state = sjs_vm_run(vm, r->fuel)) == SJS_RUN_PAUSED) {
        out->pauses = (out->pauses ^ sjs_vm_fuel_used(vm)) * 1099511628211ull;
    }
    if (state == SJS_RUN_DONE) {
        sjs_value result;
        if (sjs_vm_take_result(vm, &result) != SJS_OK || result.kind != SJS_STRING) abort();
        sjs_str t = sjs_string_text(result);
        put_bytes(&out->out, t.ptr, t.length);
        sjs_release(r->context, result);
    } else {
        sjs_error error;
        sjs_vm_error(vm, &error);
        out->fault = true;
        put(&out->out, "!");
        put_bytes(&out->out, error.message.ptr, error.message.length);
    }
    out->fuel = sjs_vm_fuel_used(vm);
    sjs_release(r->context, global);
    sjs_vm_destroy(vm);
}

static sjs_program *compile_methods(sjs_context *context, const text *source) {
    static const sjs_str names[] = { { "s", 1 } };
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, names, 1, { NULL, NULL }, false },
                                  { source->ptr, (uint32_t)source->length }, NULL };
    if (sjs_compile(&params, &program) != SJS_OK) return NULL;
    return program;
}

/* Each method on a few inputs: printed for diff-node, or run as a leaf, as
 * loops, and as loops paused after every unit of fuel, which must agree. */
static void match_inputs(sjs_context *context, const text *pat, const text *flag, bool print) {
    text source = { 0 };
    methods_source(&source, pat, flag);
    sjs_program *program = compile_methods(context, &source);
    if (program == NULL) {
        if (failures++ < 20) fprintf(stderr, "regex_test: does not compile: %s\n", source.ptr);
        free(source.ptr);
        return;
    }
    text s = { 0 };
    text line = { 0 };
    ran leaf = { 0 };
    ran loops = { 0 };
    ran paused = { 0 };
    ran uncached = { 0 };
    for (int k = 0; k < 3; k++) {
        s.length = 0;
        input(&s);
        run(&(runner){ context, program, false, UINT64_MAX, false }, &s, &leaf);
        if (print) {
            line.length = 0;
            put(&line, "X ");
            put_quoted(&line, pat->ptr, pat->length);
            put(&line, "\t");
            put_bytes(&line, flag->ptr == NULL ? "" : flag->ptr, flag->length);
            put(&line, "\t");
            put_quoted(&line, s.ptr == NULL ? "" : s.ptr, s.length);
            put(&line, "\t");
            if (leaf.fault) put_bytes(&line, leaf.out.ptr, leaf.out.length);
            else put_quoted(&line, leaf.out.ptr, leaf.out.length);
            puts(line.ptr);
            continue;
        }
        run(&(runner){ context, program, true, UINT64_MAX, false }, &s, &loops);
        run(&(runner){ context, program, true, 1, false }, &s, &paused);
        run(&(runner){ context, program, true, 1, true }, &s, &uncached);
        bool same = leaf.out.length == loops.out.length && memcmp(leaf.out.ptr, loops.out.ptr, leaf.out.length) == 0 &&
                    leaf.out.length == paused.out.length && memcmp(leaf.out.ptr, paused.out.ptr, leaf.out.length) == 0 &&
                    leaf.out.length == uncached.out.length && memcmp(leaf.out.ptr, uncached.out.ptr, leaf.out.length) == 0;
        /* Paused after each unit, a test's cache of the automaton's steps
         * pauses where the automaton does. */
        bool fuel = leaf.fuel == loops.fuel && leaf.fuel == paused.fuel && leaf.fuel == uncached.fuel && paused.pauses == uncached.pauses;
        if ((!same || !fuel) && failures++ < 20) {
            fprintf(stderr, "regex_test: %s on %s\n  leaf:     %s (fuel %llu)\n  loops:    %s (fuel %llu)\n  paused:   %s (fuel %llu)\n"
                    "  uncached: %s (fuel %llu, %s pauses)\n", source.ptr, s.ptr == NULL ? "" : s.ptr, leaf.out.ptr,
                    (unsigned long long)leaf.fuel, loops.out.ptr, (unsigned long long)loops.fuel, paused.out.ptr,
                    (unsigned long long)paused.fuel, uncached.out.ptr, (unsigned long long)uncached.fuel,
                    paused.pauses == uncached.pauses ? "the same" : "other");
        }
    }
    sjs_program_release(program);
    free(source.ptr);
    free(s.ptr);
    free(line.ptr);
    free(leaf.out.ptr);
    free(loops.out.ptr);
    free(paused.out.ptr);
    free(uncached.out.ptr);
}

static void check(long count, bool print) {
    sjs_context *context;
    sjs_context_config config = { { NULL, &counted_vtable }, SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&context, &config) != SJS_OK) abort();
    text pat = { 0 };
    text flag = { 0 };
    text source = { 0 };
    text line = { 0 };
    for (long i = 0; i < count; i++) {
        pat.length = 0;
        flag.length = 0;
        source.length = 0;
        body(&pat);
        flags(&flag);
        put(&source, "/");
        put_bytes(&source, pat.ptr, pat.length);
        put(&source, "/");
        put_bytes(&source, flag.ptr == NULL ? "" : flag.ptr, flag.length);
        if (getenv("REGEX_TEST_TRACE") != NULL) fprintf(stderr, "%s\n", source.ptr);
        sjs_error_code code = classify(context, &source, 0);
        if (code == SJS_ERROR_NONE) match_inputs(context, &pat, &flag, print);
        if (!print) {
            sjs_error_code bytes = classify(context, &source, 1);
            if (bytes != code) {
                if (failures++ < 20) fprintf(stderr, "regex_test: %s is %s whole and %s a byte at a time\n", source.ptr, code_name(code), code_name(bytes));
            }
            continue;
        }
        line.length = 0;
        put(&line, "X ");
        put_quoted(&line, pat.ptr, pat.length);
        put(&line, "\t");
        put_bytes(&line, flag.ptr == NULL ? "" : flag.ptr, flag.length);
        put(&line, "\t-\t");
        put(&line, code_name(code));
        puts(line.ptr);
    }
    free(pat.ptr);
    free(flag.ptr);
    free(source.ptr);
    free(line.ptr);
    sjs_context_destroy(context);
    if (live != 0) {
        fprintf(stderr, "regex_test: %ld allocations left behind\n", live);
        failures++;
    }
}

int main(int argc, char **argv) {
    bool print = argc == 3 && strcmp(argv[1], "--print") == 0;
    check(print ? atol(argv[2]) : 2000, print);
    if (failures != 0) {
        fprintf(stderr, "regex_test: %d failure(s)\n", failures);
        return 1;
    }
    if (!print) printf("regex_test: random literals classify alike, pushed whole and a byte at a time\n");
    return 0;
}
