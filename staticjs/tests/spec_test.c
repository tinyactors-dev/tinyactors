/*
 * The data-driven suite: every case in the spec files of tests/spec, run three ways.
 *
 *   whole:  the source pushed in one chunk, the evaluation given all the fuel
 *   bytes:  the source pushed one byte at a time, one unit of fuel per run
 *   chunks: the source pushed 7 bytes at a time, 13 units of fuel per run
 *
 * All three must give the same answer, and the whole run the expected one.
 * After each case, every byte the case allocated must have been freed.
 *
 * With --full, each case is also run once for every allocation it makes,
 * with that allocation failing: each such run must end with an out-of-memory
 * error and free everything it took. The modules registered before the case
 * are evaluated again first, with no allocation failing.
 *
 * A spec file:
 *
 *   # a comment, between cases
 *   === the case's name
 *   @global name = expression      (optional: bind a global for the case)
 *   @module name                   (optional: the source is a module, named)
 *   source lines
 *   --- value
 *   what sjs_writer writes for the result
 *   === another case
 *   source
 *   --- fault words of the fault's message
 *   === another
 *   source
 *   --- error unsupported words of the message
 *
 * Modules a file defines can be imported by the cases after them. A module
 * registered under a name already taken replaces the earlier one.
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "staticjs.h"

static void *heap_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    return malloc(size);
}

static void heap_free(void *self, const sjs_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const sjs_allocator_vtable heap_vtable = { heap_alloc, heap_free };

/* An allocator that fails from a chosen allocation on. */
typedef struct {
    long allocations;
    long fail_at;
    long live;
} failing;

static void *failing_alloc(void *self, size_t size, size_t alignment) {
    failing *f = self;
    (void)alignment;
    if (++f->allocations >= f->fail_at) return NULL;
    f->live++;
    return malloc(size);
}

static void failing_free(void *self, const sjs_allocator_free_params *params) {
    failing *f = self;
    f->live--;
    free(params->ptr);
}

static const sjs_allocator_vtable failing_vtable = { failing_alloc, failing_free };

#define MAX_TEXT 65536
#define MAX_GLOBALS 8
#define MAX_MODULES 128

typedef struct {
    char name[256];
    int line;
    char source[MAX_TEXT];
    char expected[MAX_TEXT];
    char kind[16];          /* value, fault, error */
    char code[16];          /* error: syntax, unsupported, name, limit */
    char words[512];        /* fault or error: words of the message */
    int is_module;
    char module_name[64];
    int global_count;
    char global_names[MAX_GLOBALS][64];
    char global_sources[MAX_GLOBALS][1024];
} spec_case;

typedef struct {
    char name[64];
    char *source;           /* kept to evaluate it again for --full */
    sjs_value exports;
} module_entry;

typedef struct {
    module_entry entries[MAX_MODULES];
    int count;
} registry;

static module_entry *module_named(registry *r, const char *name) {
    for (int i = 0; i < r->count; i++) {
        if (strcmp(r->entries[i].name, name) == 0) return &r->entries[i];
    }
    return NULL;
}

/* Register exports under a name, replacing a module of the same name. Takes
 * over the reference to the exports, and a copy of the source. */
static int register_module(sjs_context *context, registry *r, const char *name, const char *source, sjs_value exports) {
    module_entry *m = module_named(r, name);
    if (m != NULL) {
        sjs_release(context, m->exports);
        free(m->source);
    } else if (r->count < MAX_MODULES) {
        m = &r->entries[r->count++];
        snprintf(m->name, sizeof m->name, "%s", name);
    } else {
        sjs_release(context, exports);
        return 0;
    }
    m->source = strdup(source);
    m->exports = exports;
    return 1;
}

static void release_modules(sjs_context *context, registry *r) {
    for (int i = 0; i < r->count; i++) {
        sjs_release(context, r->entries[i].exports);
        free(r->entries[i].source);
    }
    r->count = 0;
}

static sjs_result find_module(void *self, sjs_str name, sjs_value *exports) {
    registry *r = self;
    for (int i = 0; i < r->count; i++) {
        if (strlen(r->entries[i].name) == name.length && memcmp(r->entries[i].name, name.ptr, name.length) == 0) {
            *exports = r->entries[i].exports;
            return SJS_OK;
        }
    }
    return SJS_ERR_NOT_FOUND;
}

static const sjs_modules_vtable modules_vtable = { find_module };

/* The outcome of one run, as text: "value <text>", "fault <message>" or
 * "error <code> <line>:<column> <message>". */
typedef struct {
    char text[MAX_TEXT];
} outcome;

typedef struct {
    sjs_context *context;
    sjs_allocator allocator;   /* what the parser allocates with */
    registry *modules;
    const sjs_str *global_names;
    const sjs_value *globals;
    uint32_t global_count;
} environment;

typedef struct {
    uint32_t chunk;
    uint64_t fuel;
} mode;

static const char *code_name(sjs_error_code code) {
    switch (code) {
    case SJS_ERROR_SYNTAX: return "syntax";
    case SJS_ERROR_UNSUPPORTED: return "unsupported";
    case SJS_ERROR_NAME: return "name";
    case SJS_ERROR_LIMIT: return "limit";
    case SJS_ERROR_MEMORY: return "memory";
    case SJS_ERROR_FAULT: return "fault";
    default: return "none";
    }
}

static sjs_result write_value(sjs_context *context, sjs_value value, char *out, size_t capacity) {
    sjs_writer *writer;
    sjs_writer_config config = { context, value, { "<function>", 10 } };
    sjs_result r = sjs_writer_init(&writer, &config);
    if (r != SJS_OK) return r;
    sjs_buffer buffer = { (uint8_t *)out, (uint32_t)capacity - 1u, 0 };
    r = sjs_writer_write(writer, &buffer);
    out[buffer.length] = '\0';
    sjs_writer_destroy(writer);
    return r;
}

/* Parse and compile `source`, pushing it `chunk` bytes at a time. */
static sjs_result compile(const environment *env, const char *source, sjs_goal goal, uint32_t chunk, sjs_program **program,
                          sjs_error *error) {
    sjs_compiler *compiler;
    sjs_compiler_config config = { env->context, goal, env->global_names, env->global_count,
                                   { env->modules, &modules_vtable } };
    sjs_result r = sjs_compiler_init(&compiler, &config);
    if (r != SJS_OK) return r;
    sjs_parser *parser;
    sjs_parser_config parser_config = { env->allocator, SJS_LIMITS_DEFAULT, goal, sjs_compiler_sink(compiler) };
    r = sjs_parser_init(&parser, &parser_config);
    if (r != SJS_OK) {
        sjs_compiler_destroy(compiler);
        return r;
    }
    size_t length = strlen(source);
    for (size_t at = 0; at < length && r == SJS_OK; at += chunk) {
        size_t n = length - at < chunk ? length - at : chunk;
        r = sjs_parser_push(parser, (sjs_bytes){ (const uint8_t *)source + at, (uint32_t)n });
    }
    if (r == SJS_OK) r = sjs_parser_finish(parser);
    if (r == SJS_OK) r = sjs_compiler_finish(compiler, program);
    if (r == SJS_ERR_PROGRAM && sjs_parser_error(parser, error) != SJS_OK) sjs_compiler_error(compiler, error);
    sjs_parser_destroy(parser);
    sjs_compiler_destroy(compiler);
    return r;
}

/* Run a program to its end; the result is the caller's. */
static sjs_result evaluate(const environment *env, sjs_program *program, uint64_t fuel, sjs_value *result, sjs_error *error) {
    sjs_vm *vm;
    sjs_vm_config config = { env->context };
    sjs_result made = sjs_vm_init(&vm, &config);
    if (made != SJS_OK) return made;
    sjs_run_params params = { program, env->globals, env->global_count };
    sjs_result r = sjs_vm_start(vm, &params);
    if (r == SJS_OK) {
        sjs_run run;
        do {
            run = sjs_vm_run(vm, fuel);
        } while (run == SJS_RUN_PAUSED);
        if (run == SJS_RUN_DONE) {
            r = sjs_vm_take_result(vm, result);
        } else {
            sjs_vm_error(vm, error);
            r = SJS_ERR_PROGRAM;
        }
    }
    sjs_vm_destroy(vm);
    return r;
}

static void run_case(const environment *env, const spec_case *c, mode m, outcome *out) {
    sjs_program *program = NULL;
    sjs_error error = { 0 };
    sjs_result r = compile(env, c->source, c->is_module ? SJS_GOAL_MODULE : SJS_GOAL_EXPRESSION, m.chunk, &program, &error);
    if (r != SJS_OK) {
        if (r == SJS_ERR_PROGRAM) {
            snprintf(out->text, sizeof out->text, "error %s %u:%u %.*s", code_name(error.code), error.at.line,
                     error.at.column, (int)error.message.length, error.message.ptr);
        } else {
            snprintf(out->text, sizeof out->text, "error result %d", r);
        }
        return;
    }
    sjs_value value;
    r = evaluate(env, program, m.fuel, &value, &error);
    sjs_program_release(program);
    if (r != SJS_OK && r != SJS_ERR_PROGRAM) {
        snprintf(out->text, sizeof out->text, "error result %d", r);
        return;
    }
    if (r != SJS_OK) {
        snprintf(out->text, sizeof out->text, "fault %s %u:%u %.*s [%.*s]", code_name(error.code), error.at.line,
                 error.at.column, (int)error.message.length, error.message.ptr, (int)error.subject.length,
                 error.subject.ptr);
        return;
    }
    char text[MAX_TEXT];
    r = write_value(env->context, value, text, sizeof text);
    sjs_release(env->context, value);
    if (r != SJS_OK) snprintf(out->text, sizeof out->text, "error result %d", r);
    else snprintf(out->text, sizeof out->text, "value %s", text);
}

/* ------------------------------------------------------------------------
 * Reading spec files
 */

static void trim_end(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = '\0';
}

static void append_line(char *to, const char *line) {
    size_t used = strlen(to);
    size_t n = strlen(line);
    if (used + n + 2 >= MAX_TEXT) return;
    memcpy(to + used, line, n);
    to[used + n] = '\n';
    to[used + n + 1] = '\0';
}

typedef struct {
    int passed;
    int failed;
} tally;

static int contains_words(const char *text, const char *words) {
    return words[0] == '\0' || strstr(text, words) != NULL;
}

static int check(const spec_case *c, const outcome *whole, const char *file) {
    char want[MAX_TEXT + 64];
    if (strcmp(c->kind, "value") == 0) {
        snprintf(want, sizeof want, "value %s", c->expected);
        if (strcmp(whole->text, want) == 0) return 1;
    } else if (strcmp(c->kind, "fault") == 0) {
        if (strncmp(whole->text, "fault ", 6) == 0 && contains_words(whole->text, c->words)) return 1;
    } else {
        snprintf(want, sizeof want, "error %s ", c->code);
        if (strncmp(whole->text, want, strlen(want)) == 0 && contains_words(whole->text, c->words)) return 1;
        snprintf(want, sizeof want, "error %s ...%s", c->code, c->words);
    }
    if (strcmp(c->kind, "fault") == 0) snprintf(want, sizeof want, "fault ...%s", c->words);
    fprintf(stderr, "%s:%d: %s\n  expected: %s\n  got:      %s\n", file, c->line, c->name, want, whole->text);
    return 0;
}

static int full = 0;

static int is_memory_failure(const char *text) {
    return strncmp(text, "error memory", 12) == 0 || strncmp(text, "fault memory", 12) == 0 ||
           strncmp(text, "error result -2", 15) == 0;
}

/* Evaluate the modules registered so far again, in the order registered,
 * into `into`. */
static int load_modules(const environment *env, const registry *from, registry *into) {
    for (int i = 0; i < from->count; i++) {
        sjs_program *program;
        sjs_error error = { 0 };
        sjs_value exports;
        if (compile(env, from->entries[i].source, SJS_GOAL_MODULE, 4096, &program, &error) != SJS_OK) return 0;
        sjs_result r = evaluate(env, program, UINT64_MAX, &exports, &error);
        sjs_program_release(program);
        if (r != SJS_OK || !register_module(env->context, into, from->entries[i].name, from->entries[i].source, exports)) return 0;
    }
    return 1;
}

/* Run the case failing each allocation in turn, until one run needs no
 * failure; every run before it must fail cleanly. The modules it may import
 * are evaluated first, with nothing failing, and stay for every run. */
static int sweep(const spec_case *c, const outcome *expected, const char *file, const registry *modules) {
    static outcome got;
    failing f = { 0, 1L << 62, 0 };
    sjs_context *context;
    sjs_context_config config = { { &f, &failing_vtable }, SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&context, &config) != SJS_OK) return 0;
    static registry loaded;
    loaded.count = 0;
    environment env = { context, { &f, &failing_vtable }, &loaded, NULL, NULL, 0 };
    int result = load_modules(&env, modules, &loaded);
    if (!result) fprintf(stderr, "%s:%d: %s: the modules before it do not evaluate again\n", file, c->line, c->name);
    size_t base = sjs_context_live_bytes(context);
    long base_blocks = f.live;
    for (long fail_at = 1; result; fail_at++) {
        f.allocations = 0;
        f.fail_at = fail_at;
        sjs_str names[MAX_GLOBALS];
        sjs_value values[MAX_GLOBALS];
        int bound = 0;
        int ok = 1;
        for (int i = 0; i < c->global_count && ok; i++) {
            names[i] = (sjs_str){ c->global_names[i], (uint32_t)strlen(c->global_names[i]) };
            sjs_program *program;
            sjs_error error = { 0 };
            if (compile(&env, c->global_sources[i], SJS_GOAL_EXPRESSION, 4096, &program, &error) != SJS_OK) {
                ok = 0;
                break;
            }
            if (evaluate(&env, program, UINT64_MAX, &values[i], &error) != SJS_OK) ok = 0;
            else bound++;
            sjs_program_release(program);
        }
        int done = 0;
        if (ok) {
            environment with = env;
            with.global_names = names;
            with.globals = values;
            with.global_count = (uint32_t)c->global_count;
            run_case(&with, c, (mode){ 1u << 30, UINT64_MAX }, &got);
            if (strcmp(got.text, expected->text) == 0) {
                done = 1;
            } else if (!is_memory_failure(got.text)) {
                fprintf(stderr, "%s:%d: %s: with allocation %ld failing\n  got: %s\n", file, c->line, c->name, fail_at, got.text);
                result = 0;
            }
        }
        f.fail_at = 1L << 62;
        for (int i = 0; i < bound; i++) sjs_release(context, values[i]);
        size_t live = sjs_context_live_bytes(context);
        if (live != base || f.live != base_blocks) {
            fprintf(stderr, "%s:%d: %s: allocation %ld failing leaks %zu bytes\n", file, c->line, c->name, fail_at, live - base);
            result = 0;
        }
        if (done) break;
    }
    release_modules(context, &loaded);
    size_t live = sjs_context_live_bytes(context);
    sjs_context_destroy(context);
    if (live != 0 || f.live != 0) {
        fprintf(stderr, "%s:%d: %s: %zu bytes still live after the sweep\n", file, c->line, c->name, live);
        result = 0;
    }
    return result;
}

/* The case is done: run it, compare, and register a module. */
static void finish_case(environment *env, spec_case *c, const char *file, tally *t) {
    size_t before = sjs_context_live_bytes(env->context);
    /* Globals, evaluated first. */
    sjs_str names[MAX_GLOBALS];
    sjs_value values[MAX_GLOBALS];
    environment plain = *env;
    plain.global_names = NULL;
    plain.globals = NULL;
    plain.global_count = 0;
    for (int i = 0; i < c->global_count; i++) {
        names[i] = (sjs_str){ c->global_names[i], (uint32_t)strlen(c->global_names[i]) };
        sjs_program *program;
        sjs_error error = { 0 };
        if (compile(&plain, c->global_sources[i], SJS_GOAL_EXPRESSION, 4096, &program, &error) != SJS_OK ||
            evaluate(&plain, program, UINT64_MAX, &values[i], &error) != SJS_OK) {
            fprintf(stderr, "%s:%d: %s: @global %s does not evaluate\n", file, c->line, c->name, c->global_names[i]);
            t->failed++;
            return;
        }
        sjs_program_release(program);
    }
    environment with = *env;
    with.global_names = names;
    with.globals = values;
    with.global_count = (uint32_t)c->global_count;
    static outcome whole;
    static outcome bytes;
    static outcome chunks;
    run_case(&with, c, (mode){ 1u << 30, UINT64_MAX }, &whole);
    run_case(&with, c, (mode){ 1, 1 }, &bytes);
    run_case(&with, c, (mode){ 7, 13 }, &chunks);
    int ok = check(c, &whole, file);
    if (ok && strcmp(whole.text, bytes.text) != 0) {
        fprintf(stderr, "%s:%d: %s: pushed a byte at a time\n  whole: %s\n  bytes: %s\n", file, c->line, c->name, whole.text, bytes.text);
        ok = 0;
    }
    if (ok && strcmp(whole.text, chunks.text) != 0) {
        fprintf(stderr, "%s:%d: %s: pushed in chunks\n  whole:  %s\n  chunks: %s\n", file, c->line, c->name, whole.text, chunks.text);
        ok = 0;
    }
    for (int i = 0; i < c->global_count; i++) sjs_release(env->context, values[i]);
    if (ok && full && !sweep(c, &whole, file, env->modules)) ok = 0;
    if (ok && c->is_module && strncmp(whole.text, "value ", 6) == 0) {
        /* Evaluate once more, and keep the exports for the cases after. */
        sjs_program *program;
        sjs_error error = { 0 };
        sjs_value exports;
        if (compile(env, c->source, SJS_GOAL_MODULE, 4096, &program, &error) == SJS_OK) {
            if (evaluate(env, program, UINT64_MAX, &exports, &error) == SJS_OK &&
                register_module(env->context, env->modules, c->module_name, c->source, exports)) {
                /* registered */
            } else {
                ok = 0;
            }
            sjs_program_release(program);
        } else {
            ok = 0;
        }
        if (!ok) fprintf(stderr, "%s:%d: %s: the module could not be registered\n", file, c->line, c->name);
    } else if (ok && sjs_context_live_bytes(env->context) != before) {
        fprintf(stderr, "%s:%d: %s: %zu bytes were not freed\n", file, c->line, c->name,
                sjs_context_live_bytes(env->context) - before);
        ok = 0;
    }
    if (ok) t->passed++;
    else t->failed++;
}

static void run_file(sjs_context *context, const char *path, tally *t) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        t->failed++;
        return;
    }
    static registry modules;
    modules.count = 0;
    environment env = { context, { NULL, &heap_vtable }, &modules, NULL, NULL, 0 };
    static spec_case c;
    static char line[MAX_TEXT];
    enum { OUTSIDE, SOURCE, EXPECTED } where = OUTSIDE;
    int number = 0;
    int open = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        number++;
        if (strncmp(line, "=== ", 4) == 0) {
            if (open) {
                trim_end(c.expected);
                trim_end(c.source);
                finish_case(&env, &c, path, t);
            }
            memset(&c, 0, sizeof c);
            snprintf(c.name, sizeof c.name, "%s", line + 4);
            trim_end(c.name);
            c.line = number;
            open = 1;
            where = SOURCE;
            continue;
        }
        if (where != SOURCE && strncmp(line, "# ", 2) == 0) continue;
        if (where == OUTSIDE) continue;
        if (where == SOURCE && c.source[0] == '\0' && strncmp(line, "@global ", 8) == 0 && c.global_count < MAX_GLOBALS) {
            char *eq = strstr(line, " = ");
            if (eq != NULL) {
                *eq = '\0';
                snprintf(c.global_names[c.global_count], 64, "%s", line + 8);
                snprintf(c.global_sources[c.global_count], 1024, "%s", eq + 3);
                trim_end(c.global_sources[c.global_count]);
                c.global_count++;
            }
            continue;
        }
        if (where == SOURCE && c.source[0] == '\0' && strncmp(line, "@module ", 8) == 0) {
            c.is_module = 1;
            snprintf(c.module_name, sizeof c.module_name, "%s", line + 8);
            trim_end(c.module_name);
            continue;
        }
        if (where == SOURCE && strncmp(line, "--- ", 4) == 0) {
            char *rest = line + 4;
            trim_end(rest);
            char *space = strchr(rest, ' ');
            if (space != NULL) *space = '\0';
            snprintf(c.kind, sizeof c.kind, "%s", rest);
            char *words = space != NULL ? space + 1 : rest + strlen(rest);
            if (strcmp(c.kind, "error") == 0) {
                char *second = strchr(words, ' ');
                if (second != NULL) *second = '\0';
                snprintf(c.code, sizeof c.code, "%s", words);
                words = second != NULL ? second + 1 : words + strlen(words);
            }
            snprintf(c.words, sizeof c.words, "%s", words);
            where = EXPECTED;
            continue;
        }
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
        if (where == SOURCE) append_line(c.source, line);
        else append_line(c.expected, line);
    }
    if (open) {
        trim_end(c.expected);
        trim_end(c.source);
        finish_case(&env, &c, path, t);
    }
    fclose(f);
    release_modules(context, &modules);
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: spec_test [--full] DIRECTORY-or-FILE...\n");
        return 2;
    }
    sjs_context *context;
    sjs_context_config config = { { NULL, &heap_vtable }, SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&context, &config) != SJS_OK) return 2;
    tally t = { 0, 0 };
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--full") == 0) {
            full = 1;
            continue;
        }
        DIR *dir = opendir(argv[i]);
        if (dir == NULL) {
            run_file(context, argv[i], &t);
            continue;
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
        for (int k = 0; k < count; k++) {
            char path[4096];
            snprintf(path, sizeof path, "%s/%s", argv[i], names[k]);
            run_file(context, path, &t);
            free(names[k]);
        }
    }
    size_t live = sjs_context_live_bytes(context);
    sjs_context_destroy(context);
    if (live != 0) {
        fprintf(stderr, "spec_test: %zu bytes still live at the end\n", live);
        t.failed++;
    }
    printf("spec_test: %d cases passed, %d failed%s\n", t.passed, t.failed,
           full ? " (and each failing every allocation it makes)" : "");
    return t.failed == 0 ? 0 : 1;
}
