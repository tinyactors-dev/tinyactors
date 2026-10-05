/*
 * The matcher (src/matcher.c, docs/matcher.md, section 5), driven by a
 * parser through a tee of this test's own, as the compiler drives it:
 *
 *   - reports: the exact shape, node, start and arguments for a list of
 *     sources, and the compiler records every one (none ignored);
 *   - every spec source: the matcher never stops, and holds no more records
 *     than its bound;
 *   - the deepest programs the limits allow, of each kind of nesting: the
 *     same;
 *   - width: a long array, a module of many consts and a long template hold
 *     a handful of records;
 *   - streams no parser sends: the matcher stops, frees what it holds and
 *     lets every later event pass;
 *   - out of memory: every allocation the matcher makes fails in turn, each
 *     run ends with SJS_ERR_NOMEM, and nothing leaks.
 *
 *   matcher_test tests/spec
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "matcher.h"

static int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            failures++;                                                         \
        }                                                                       \
    } while (0)

/* An allocator that counts, and fails from a chosen allocation on. */
typedef struct {
    long allocations;
    long fail_at;     /* 0: never */
    long live;
} counting;

static void *test_alloc(void *self, size_t size, size_t alignment) {
    counting *c = self;
    (void)alignment;
    c->allocations++;
    if (c->fail_at != 0 && c->allocations >= c->fail_at) return NULL;
    c->live++;
    return malloc(size);
}

static void test_free(void *self, const sjs_allocator_free_params *params) {
    counting *c = self;
    c->live--;
    free(params->ptr);
}

static const sjs_allocator_vtable test_vtable = { test_alloc, test_free };

/* ------------------------------------------------------------------------
 * The tee: each event goes to the matcher; reports are written down.
 */

typedef struct {
    sjs_matcher matcher;
    sjs_result result;     /* the matcher's first result other than SJS_OK */
    int reports;
    char text[4096];       /* SHAPE#node@offset(arguments) ... */
} tee;

static sjs_result tee_event(void *self, const sjs_event *e) {
    tee *t = self;
    sjs_result r = sjs_matcher_event(&t->matcher, e);
    if (r != SJS_OK && t->result == SJS_OK) t->result = r;
    return r;
}

static const sjs_sink_vtable tee_vtable = { tee_event };

static sjs_result tee_report(void *self, const sjs_shape_match *match) {
    tee *t = self;
    size_t used = strlen(t->text);
    used += (size_t)snprintf(t->text + used, sizeof t->text - used, "%s%s#%u@%u(", t->reports > 0 ? " " : "",
                             sjs_shape_name(match->shape), match->node, match->start.offset);
    for (uint32_t i = 0; i < SJS_SHAPE_ARGS && match->args[i].kind != SJS_SHAPE_ARG_NONE && used < sizeof t->text; i++) {
        const sjs_shape_value *arg = &match->args[i];
        const char *comma = i > 0 ? ", " : "";
        if (arg->kind == SJS_SHAPE_ARG_TEXT) {
            used += (size_t)snprintf(t->text + used, sizeof t->text - used, "%s%.*s", comma, (int)arg->text.length, arg->text.ptr);
        } else if (arg->kind == SJS_SHAPE_ARG_FLAG) {
            used += (size_t)snprintf(t->text + used, sizeof t->text - used, "%s%s", comma, arg->count ? "true" : "false");
        } else if (arg->kind == SJS_SHAPE_ARG_OPERATOR) {
            used += (size_t)snprintf(t->text + used, sizeof t->text - used, "%s%s", comma, sjs_shape_operator_name(arg->count));
        } else if (arg->kind == SJS_SHAPE_ARG_OPERAND) {
            static const char *const literals[] = { "", "", "", "true", "false", "null" };
            if (arg->operand == SJS_OPERAND_NUMBER) {
                char number[32];
                number[sjs_number_format(arg->number, number)] = '\0';
                used += (size_t)snprintf(t->text + used, sizeof t->text - used, "%s%s", comma, number);
            } else if (arg->operand == SJS_OPERAND_TEXT) {
                used += (size_t)snprintf(t->text + used, sizeof t->text - used, "%s\"%.*s\"", comma, (int)arg->text.length,
                                         arg->text.ptr);
            } else {
                used += (size_t)snprintf(t->text + used, sizeof t->text - used, "%s%s", comma, literals[arg->operand]);
            }
        } else {
            used += (size_t)snprintf(t->text + used, sizeof t->text - used, "%s%u", comma, arg->count);
        }
    }
    if (used < sizeof t->text) snprintf(t->text + used, sizeof t->text - used, ")");
    t->reports++;
    return SJS_OK;
}

static const sjs_shape_reporter_vtable report_vtable = { tee_report };

static void tee_init(tee *t, const sjs_allocator *allocator, uint32_t max_nesting) {
    memset(t, 0, sizeof *t);
    sjs_matcher_config config = { allocator, max_nesting, { t, &report_vtable } };
    sjs_matcher_init(&t->matcher, &config);
}

/* Parse `source` into the tee, `chunk` bytes at a time; the parser's result. */
typedef struct {
    const char *source;
    size_t length;
    sjs_goal goal;
    uint32_t chunk;
} parse_job;

static sjs_result parse_into(tee *t, parse_job job, sjs_limits limits) {
    counting c = { 0 };
    sjs_parser *parser;
    sjs_parser_config config = { { &c, &test_vtable }, limits, job.goal, { t, &tee_vtable } };
    if (sjs_parser_init(&parser, &config) != SJS_OK) return SJS_ERR_NOMEM;
    sjs_result r = SJS_OK;
    for (size_t at = 0; at < job.length && r == SJS_OK; at += job.chunk) {
        size_t n = job.length - at < job.chunk ? job.length - at : job.chunk;
        r = sjs_parser_push(parser, (sjs_bytes){ (const uint8_t *)job.source + at, (uint32_t)n });
    }
    if (r == SJS_OK) r = sjs_parser_finish(parser);
    sjs_parser_destroy(parser);
    CHECK(c.live == 0);
    return r;
}

static parse_job whole(const char *source, sjs_goal goal) {
    return (parse_job){ source, strlen(source), goal, 1u << 30 };
}

/* ------------------------------------------------------------------------
 * Reports
 */

/* The program's functions that carry a shape. */
static int tagged(const char *source, sjs_goal goal) {
    counting c = { 0 };
    sjs_context *context;
    sjs_context_config config = { { &c, &test_vtable }, SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&context, &config) != SJS_OK) return -1;
    sjs_program *program;
    sjs_compile_params params = { { context, goal, NULL, 0, { NULL, NULL }, false }, { source, (uint32_t)strlen(source) }, NULL };
    int n = -1;
    if (sjs_compile(&params, &program) == SJS_OK) {
        n = 0;
        for (uint32_t i = 0; i < program->function_count; i++) n += program->functions[i].shape != SJS_SHAPE_NONE;
        sjs_program_release(program);
    }
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
    return n;
}

/* `source`'s reports are `want`, whole and a byte at a time, and the
 * compiler records each one on a function. */
static void expect_reports(const char *source, sjs_goal goal, const char *want) {
    counting c = { 0 };
    sjs_allocator allocator = { &c, &test_vtable };
    static tee t;
    for (uint32_t chunk = 1; chunk != 0; chunk = chunk == 1 ? (1u << 30) : 0) {
        tee_init(&t, &allocator, SJS_LIMITS_DEFAULT.max_nesting);
        parse_job job = whole(source, goal);
        job.chunk = chunk;
        CHECK(parse_into(&t, job, SJS_LIMITS_DEFAULT) == SJS_OK);
        CHECK(!t.matcher.stopped);
        if (strcmp(t.text, want) != 0) fprintf(stderr, "%s\n  reports: %s\n  wanted:  %s\n", source, t.text, want);
        CHECK(strcmp(t.text, want) == 0);
        sjs_matcher_free(&t.matcher);
        CHECK(c.live == 0);
    }
    int n = tagged(source, goal);
    if (n != t.reports) fprintf(stderr, "%s\n  %d reports, %d functions tagged\n", source, t.reports, n);
    CHECK(n == t.reports);
}

static void test_reports(void) {
    expect_reports("x => x.k", SJS_GOAL_EXPRESSION, "PLUCK#1@0(k)");
    expect_reports("(row) => { return row.name; }", SJS_GOAL_EXPRESSION, "PLUCK#1@0(name)");
    expect_reports("(a, b) => a.score - b.score", SJS_GOAL_EXPRESSION, "COMPARE_BY_KEY#1@0(score, false)");
    expect_reports("(a, b) => { return b.score - a.score; }", SJS_GOAL_EXPRESSION, "COMPARE_BY_KEY#1@0(score, true)");
    expect_reports("(a, b) => a - b", SJS_GOAL_EXPRESSION, "COMPARE_NUMERIC#1@0(false)");
    expect_reports("(a, b) => { return b - a; }", SJS_GOAL_EXPRESSION, "COMPARE_NUMERIC#1@0(true)");
    /* Predicates: the operator, mirrored for a literal on the left, and the
     * literal of each kind. */
    expect_reports("x => x.priority === 2", SJS_GOAL_EXPRESSION, "KEY_COMPARE#1@0(priority, STRICT_EQUAL, 2)");
    expect_reports("x => 1.5 < x.kwh", SJS_GOAL_EXPRESSION, "KEY_COMPARE#1@0(kwh, GREATER, 1.5)");
    expect_reports("x => { return \"open\" !== x.state; }", SJS_GOAL_EXPRESSION,
                   "KEY_COMPARE#1@0(state, STRICT_NOT_EQUAL, \"open\")");
    expect_reports("x => x >= 0", SJS_GOAL_EXPRESSION, "ELEMENT_COMPARE#1@0(GREATER_EQUAL, 0)");
    expect_reports("x => \"m\" >= x", SJS_GOAL_EXPRESSION, "ELEMENT_COMPARE#1@0(LESS_EQUAL, \"m\")");
    expect_reports("[x => x === true, x => false !== x, x => x.k === null, x => x <= 1e21]", SJS_GOAL_EXPRESSION,
                   "ELEMENT_COMPARE#2@1(STRICT_EQUAL, true) ELEMENT_COMPARE#7@18(STRICT_NOT_EQUAL, false) "
                   "KEY_COMPARE#12@36(k, STRICT_EQUAL, null) ELEMENT_COMPARE#18@55(LESS_EQUAL, 1e+21)");
    /* An arrow's node and start in a call: the call is node 3, the arrow 4. */
    expect_reports("[].map(x => x.k)", SJS_GOAL_EXPRESSION, "PLUCK#4@7(k)");
    expect_reports("[].map(xs => xs.map(x => x.k))", SJS_GOAL_EXPRESSION, "PLUCK#9@20(k)");
    expect_reports("[r => r.a, (p, q) => q.b - p.b]", SJS_GOAL_EXPRESSION, "PLUCK#2@1(a) COMPARE_BY_KEY#6@11(b, true)");
    /* In a parameter's default, and in a held const pattern's default. */
    expect_reports("(f = x => x.k) => f", SJS_GOAL_EXPRESSION, "PLUCK#4@5(k)");
    expect_reports("const {f = x => x.k} = {};\nexport const g = y => y.j;", SJS_GOAL_MODULE,
                   "PLUCK#5@11(k) PLUCK#13@44(j)");
    /* Near misses report nothing. */
    expect_reports("y => [x => y.k]", SJS_GOAL_EXPRESSION, "");
    expect_reports("(a, b) => a.k - b.j", SJS_GOAL_EXPRESSION, "");
    expect_reports("(a, b) => a - a", SJS_GOAL_EXPRESSION, "");
    expect_reports("(a, b) => a.k - b", SJS_GOAL_EXPRESSION, "");
    expect_reports("(a, ...b) => a - b", SJS_GOAL_EXPRESSION, "");
    expect_reports("x => { const y = x; return y.k; }", SJS_GOAL_EXPRESSION, "");
    expect_reports("x => x?.k", SJS_GOAL_EXPRESSION, "");
    expect_reports("y => [x => x.k === y]", SJS_GOAL_EXPRESSION, "");
    expect_reports("x => x === -1", SJS_GOAL_EXPRESSION, "");
    expect_reports("x => x.k === `a${1}`", SJS_GOAL_EXPRESSION, "");
    expect_reports("x => x.k + 1", SJS_GOAL_EXPRESSION, "");
    expect_reports("x => \"k\" in x", SJS_GOAL_EXPRESSION, "");
    expect_reports("(x, i) => x === 1", SJS_GOAL_EXPRESSION, "");
    expect_reports("x => 1 === 2", SJS_GOAL_EXPRESSION, "");
    expect_reports("x => x === x", SJS_GOAL_EXPRESSION, "");
}

/* ------------------------------------------------------------------------
 * Every spec source: the matcher never stops and stays within its bound.
 */

typedef struct {
    int sources;
    uint32_t peak;
} spec_tally;

static void check_source(const char *source, sjs_goal goal, const char *where, spec_tally *tally) {
    counting c = { 0 };
    sjs_allocator allocator = { &c, &test_vtable };
    static tee t;
    tee_init(&t, &allocator, SJS_LIMITS_DEFAULT.max_nesting);
    parse_into(&t, whole(source, goal), SJS_LIMITS_DEFAULT);
    if (t.matcher.stopped || t.matcher.peak > t.matcher.bound || t.result != SJS_OK) {
        fprintf(stderr, "%s: the matcher %s (peak %u of %u)\n", where, t.matcher.stopped ? "stopped" : "failed",
                t.matcher.peak, t.matcher.bound);
        failures++;
    }
    if (t.matcher.peak > tally->peak) tally->peak = t.matcher.peak;
    tally->sources++;
    sjs_matcher_free(&t.matcher);
    CHECK(c.live == 0);
}

#define MAX_SOURCE 65536

static void check_spec_file(const char *path, spec_tally *tally) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        failures++;
        return;
    }
    static char line[MAX_SOURCE];
    static char source[MAX_SOURCE];
    char where[4200] = "";
    int in_source = 0;
    int module = 0;
    int number = 0;
    size_t used = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        number++;
        if (strncmp(line, "=== ", 4) == 0) {
            in_source = 1;
            module = 0;
            used = 0;
            source[0] = '\0';
            snprintf(where, sizeof where, "%s:%d", path, number);
            continue;
        }
        if (!in_source) continue;
        if (used == 0 && line[0] == '@') {
            module |= strncmp(line, "@module ", 8) == 0;
            continue;
        }
        if (strncmp(line, "--- ", 4) == 0) {
            in_source = 0;
            check_source(source, module ? SJS_GOAL_MODULE : SJS_GOAL_EXPRESSION, where, tally);
            continue;
        }
        size_t n = strlen(line);
        if (used + n + 1 < sizeof source) {
            memcpy(source + used, line, n + 1);
            used += n;
        }
    }
    fclose(f);
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void test_spec_sources(const char *directory) {
    spec_tally tally = { 0, 0 };
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        fprintf(stderr, "cannot open %s\n", directory);
        failures++;
        return;
    }
    char *names[256];
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < 256) {
        size_t n = strlen(entry->d_name);
        if (n > 5 && strcmp(entry->d_name + n - 5, ".spec") == 0) names[count++] = strdup(entry->d_name);
    }
    closedir(dir);
    qsort(names, (size_t)count, sizeof names[0], compare_names);
    for (int i = 0; i < count; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", directory, names[i]);
        check_spec_file(path, &tally);
        free(names[i]);
    }
    CHECK(tally.sources > 1000);
    printf("matcher_test: %d spec sources, at most %u records\n", tally.sources, tally.peak);
}

/* ------------------------------------------------------------------------
 * The deepest programs the limits allow, and wide ones
 */

/* A source nesting `open` ... `close` `depth` times around `middle`,
 * between `prefix` and `suffix`. */
typedef struct {
    const char *name;
    const char *prefix;
    const char *open;
    const char *middle;
    const char *close;
    const char *suffix;
    sjs_goal goal;
} nesting;

static char *repeated(const nesting *n, int depth) {
    size_t size = (strlen(n->open) + strlen(n->close)) * (size_t)depth + strlen(n->prefix) + strlen(n->middle) +
                  strlen(n->suffix) + 1;
    char *s = malloc(size);
    if (s == NULL) abort();
    size_t used = (size_t)sprintf(s, "%s", n->prefix);
    for (int i = 0; i < depth; i++) used += (size_t)sprintf(s + used, "%s", n->open);
    used += (size_t)sprintf(s + used, "%s", n->middle);
    for (int i = 0; i < depth; i++) used += (size_t)sprintf(s + used, "%s", n->close);
    sprintf(s + used, "%s", n->suffix);
    return s;
}

/* Each nesting at the deepest depth the parser accepts: the matcher never
 * stops and stays within 4 * max_nesting + 4 records. */
static void test_deepest(void) {
    static const nesting kinds[] = {
        { "parentheses and operators", "", "1 + (", "1", ")", "", SJS_GOAL_EXPRESSION },
        { "right operands", "", "a + b * (", "c", ")", "", SJS_GOAL_EXPRESSION },
        { "arrays", "", "[0, ", "1", "]", "", SJS_GOAL_EXPRESSION },
        { "objects", "", "{k: 0, j: ", "1", "}", "", SJS_GOAL_EXPRESSION },
        { "calls", "", "f(0, ", "1", ")", "", SJS_GOAL_EXPRESSION },
        { "members of calls", "", "f(", "x", ").k", "", SJS_GOAL_EXPRESSION },
        { "logical operators", "", "a && (b || ", "c", ")", "", SJS_GOAL_EXPRESSION },
        { "conditionals", "", "a ? b : (c ? ", "d", " : e)", "", SJS_GOAL_EXPRESSION },
        { "unary operators", "", "-(!", "a", ")", "", SJS_GOAL_EXPRESSION },
        { "templates", "", "`a${", "1", "}b`", "", SJS_GOAL_EXPRESSION },
        { "arrows", "", "x => (", "x.k", ")", "", SJS_GOAL_EXPRESSION },
        { "block bodies", "", "x => { const y = 1; return ", "x.k", "; }", "", SJS_GOAL_EXPRESSION },
        { "ifs", "x => { ", "if (x) { const y = x; ", "return 1;", " }", " }", SJS_GOAL_EXPRESSION },
        { "else-ifs", "x => { ", "if (x) return 0; else if (x) { ", "return 1;", " }", " }", SJS_GOAL_EXPRESSION },
        { "array patterns", "const ", "[a = 1, ", "b", "]", " = c;", SJS_GOAL_MODULE },
        { "object patterns", "const ", "{k: a, j: ", "b", "}", " = c;", SJS_GOAL_MODULE },
        { "defaults of parameters", "", "(a = ", "1", ") => a", "", SJS_GOAL_EXPRESSION },
        { "patterns of parameters", "", "(a, [b = (", "c", ")]) => a", "", SJS_GOAL_EXPRESSION },
    };
    counting c = { 0 };
    sjs_allocator allocator = { &c, &test_vtable };
    static tee t;
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        int deepest = 0;
        uint32_t peak = 0;
        for (int depth = 1; depth <= 600; depth++) {
            char *source = repeated(&kinds[k], depth);
            tee_init(&t, &allocator, SJS_LIMITS_DEFAULT.max_nesting);
            sjs_result r = parse_into(&t, whole(source, kinds[k].goal), SJS_LIMITS_DEFAULT);
            if (r == SJS_OK) {
                deepest = depth;
                peak = t.matcher.peak;
                if (t.matcher.stopped || t.matcher.peak > t.matcher.bound) {
                    fprintf(stderr, "%s, %d deep: the matcher stopped (peak %u of %u)\n", kinds[k].name, depth, t.matcher.peak,
                            t.matcher.bound);
                    failures++;
                }
            }
            sjs_matcher_free(&t.matcher);
            free(source);
            if (r != SJS_OK) break;
        }
        CHECK(c.live == 0);
        /* Every kind nests deep before a limit of the parser refuses it. */
        if (deepest < 10) fprintf(stderr, "%s: only %d deep\n", kinds[k].name, deepest);
        CHECK(deepest >= 10);
        printf("matcher_test: %-26s %3d deep, at most %4u records\n", kinds[k].name, deepest, peak);
    }
}

/* Width adds no records: separators, closes and items fold what they end. */
static void test_width(void) {
    counting c = { 0 };
    sjs_allocator allocator = { &c, &test_vtable };
    static tee t;
    enum { ELEMENTS = 100000, CONSTS = 2000, SUBSTITUTIONS = 1000 };
    size_t size = (size_t)ELEMENTS * 8u + 64u;
    char *source = malloc(size);
    if (source == NULL) abort();

    size_t used = (size_t)sprintf(source, "[");
    for (int i = 0; i < ELEMENTS; i++) used += (size_t)sprintf(source + used, "%sx.k", i > 0 ? "," : "");
    sprintf(source + used, "]");
    tee_init(&t, &allocator, SJS_LIMITS_DEFAULT.max_nesting);
    CHECK(parse_into(&t, whole(source, SJS_GOAL_EXPRESSION), SJS_LIMITS_DEFAULT) == SJS_OK);
    CHECK(!t.matcher.stopped && t.matcher.peak <= 6);
    printf("matcher_test: an array of %d members, at most %u records\n", ELEMENTS, t.matcher.peak);
    sjs_matcher_free(&t.matcher);

    used = 0;
    for (int i = 0; i < CONSTS; i++) used += (size_t)sprintf(source + used, "export const a%d = x => x.k;\n", i);
    tee_init(&t, &allocator, SJS_LIMITS_DEFAULT.max_nesting);
    CHECK(parse_into(&t, whole(source, SJS_GOAL_MODULE), SJS_LIMITS_DEFAULT) == SJS_OK);
    CHECK(!t.matcher.stopped && t.matcher.peak <= 6 && t.reports == CONSTS);
    printf("matcher_test: a module of %d consts, at most %u records\n", CONSTS, t.matcher.peak);
    sjs_matcher_free(&t.matcher);

    used = (size_t)sprintf(source, "`");
    for (int i = 0; i < SUBSTITUTIONS; i++) used += (size_t)sprintf(source + used, "a${x.k}");
    sprintf(source + used, "`");
    tee_init(&t, &allocator, SJS_LIMITS_DEFAULT.max_nesting);
    CHECK(parse_into(&t, whole(source, SJS_GOAL_EXPRESSION), SJS_LIMITS_DEFAULT) == SJS_OK);
    CHECK(!t.matcher.stopped && t.matcher.peak <= 6);
    printf("matcher_test: a template of %d substitutions, at most %u records\n", SUBSTITUTIONS, t.matcher.peak);
    sjs_matcher_free(&t.matcher);

    free(source);
    CHECK(c.live == 0);
}

/* ------------------------------------------------------------------------
 * Streams no parser sends
 */

static void expect_stop(const sjs_event *events, int count, int stops_at) {
    counting c = { 0 };
    sjs_allocator allocator = { &c, &test_vtable };
    static tee t;
    tee_init(&t, &allocator, SJS_LIMITS_DEFAULT.max_nesting);
    for (int i = 0; i < count; i++) {
        CHECK(sjs_matcher_event(&t.matcher, &events[i]) == SJS_OK);
        CHECK(t.matcher.stopped == (i >= stops_at));
    }
    /* Stopped, it holds nothing and lets more events pass. */
    CHECK(c.live == 0);
    sjs_event end = { .kind = SJS_EV_END };
    CHECK(sjs_matcher_event(&t.matcher, &end) == SJS_OK);
    sjs_matcher_free(&t.matcher);
    CHECK(c.live == 0);
}

static void test_unreadable(void) {
    /* A hand-built stream, as api_test's compile_events sends: node 0. */
    const sjs_event unnamed[] = { { .kind = SJS_EV_ARROW_BEGIN }, { .kind = SJS_EV_PARAMS_END } };
    expect_stop(unnamed, 2, 0);
    /* The rest is read where a rule could still match: in an arrow whose
     * rules are alive. (In a dead region the matcher keeps nothing and
     * checks nothing but the region's own end: its events, and those of the
     * brackets in it, are passed over.) */
    const sjs_event arrow[] = { { .kind = SJS_EV_ARROW_BEGIN, .node = 1, .count = 1 }, { .kind = SJS_EV_PARAM, .node = 1 },
                                { .kind = SJS_EV_BIND, .node = 2, .text = { "x", 1 } }, { .kind = SJS_EV_PARAMS_END, .node = 1 } };
    /* A close of a bracket that is not open. */
    const sjs_event stray[] = { arrow[0], arrow[1], arrow[2], arrow[3], { .kind = SJS_EV_ARRAY_END, .node = 3 } };
    expect_stop(stray, 5, 4);
    /* A close with the open bracket's id but of another kind. */
    const sjs_event mismatched[] = { arrow[0], { .kind = SJS_EV_OBJECT_END, .node = 1 } };
    expect_stop(mismatched, 2, 1);
    /* An operator with too few operands. */
    const sjs_event operands[] = { arrow[0], arrow[1], arrow[2], arrow[3], { .kind = SJS_EV_BINARY, .node = 3 } };
    expect_stop(operands, 5, 4);
    /* A separator of no bracket. */
    const sjs_event separator[] = { arrow[0], arrow[1], arrow[2], arrow[3], { .kind = SJS_EV_ELEMENT, .node = 3 } };
    expect_stop(separator, 5, 4);
    /* An event kind out of range. */
    const sjs_event unknown[] = { { .kind = SJS_EV_NUMBER, .node = 1 }, { .kind = (sjs_event_kind)200, .node = 2 } };
    expect_stop(unknown, 2, 1);
    /* More records than the bound: arrows that can still match, in arrows. */
    sjs_event many[16];
    for (int i = 0; i < 16; i++) many[i] = (sjs_event){ .kind = SJS_EV_ARROW_BEGIN, .node = (uint32_t)i + 1u, .count = 1 };
    counting c = { 0 };
    sjs_allocator allocator = { &c, &test_vtable };
    static tee t;
    tee_init(&t, &allocator, 2); /* a bound of 12 records */
    for (int i = 0; i < 16; i++) CHECK(sjs_matcher_event(&t.matcher, &many[i]) == SJS_OK);
    CHECK(t.matcher.stopped && t.matcher.peak == 12);
    sjs_matcher_free(&t.matcher);
    CHECK(c.live == 0);
}

/* ------------------------------------------------------------------------
 * Out of memory
 */

/* Fail each of the matcher's allocations of `source` in turn: each run
 * ends with SJS_ERR_NOMEM and frees what it took, until one gives
 * `reports`. Answers the allocations. */
static int sweep_out_of_memory(const char *source, int reports) {
    static tee t;
    int runs = 0;
    for (long fail_at = 1;; fail_at++) {
        counting c = { 0, fail_at, 0 };
        sjs_allocator allocator = { &c, &test_vtable };
        tee_init(&t, &allocator, SJS_LIMITS_DEFAULT.max_nesting);
        sjs_result r = parse_into(&t, whole(source, SJS_GOAL_EXPRESSION), SJS_LIMITS_DEFAULT);
        sjs_matcher_free(&t.matcher);
        CHECK(c.live == 0);
        runs++;
        if (r == SJS_OK) {
            CHECK(t.reports == reports);
            break;
        }
        CHECK(t.result == SJS_ERR_NOMEM);
        if (t.result != SJS_ERR_NOMEM || runs > 100) break;
    }
    return runs - 1;
}

static void test_out_of_memory(void) {
    /* Names long enough that the text outgrows the matcher's own bytes. */
    const char *names = "[{a_rather_long_name_for_a_key: \"a\", k: 1}]"
                        ".map(each_row_of_the_whole_table => each_row_of_the_whole_table.a_rather_long_name_for_a_key)"
                        ".concat([(left_hand_row, right_hand_row) => right_hand_row.k - left_hand_row.k, x => { return x.k; }])";
    /* The records, their growth, and the text past the matcher's own bytes. */
    CHECK(sweep_out_of_memory(names, 3) == 3);
    /* A string of 64 bytes, and its kind: an operand that outgrows them
     * alone. The records, their growth, and the text; a short string needs
     * no text of its own. */
    const char *operand = "x => x === \"a string literal of sixty-four bytes: with its kind, 65 bytes!!!\"";
    CHECK(sweep_out_of_memory(operand, 1) == 3);
    CHECK(sweep_out_of_memory("x => x === \"short\"", 1) == 2);
    printf("matcher_test: the matcher's allocations each failed in turn\n");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: matcher_test SPEC-DIRECTORY\n");
        return 2;
    }
    test_reports();
    test_spec_sources(argv[1]);
    test_deepest();
    test_width();
    test_unreadable();
    test_out_of_memory();
    if (failures > 0) {
        fprintf(stderr, "matcher_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("matcher_test: passed\n");
    return 0;
}
