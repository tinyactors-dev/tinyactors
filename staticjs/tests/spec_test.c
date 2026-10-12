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
 * Every compile passes the parser's events through a check of their nodes
 * (node_check, below), and the three runs must send the same nodes and tag
 * the same functions with the same shapes.
 *
 * Each case also runs once more whole with matching off (no_shapes,
 * docs/matcher.md): it must compile to the same program but for the shapes,
 * give the same outcome, tag no function, and use no less fuel than the run
 * with shapes. A case whose source holds a `/` runs once more whole with
 * every regular expression match a loop frame (no_regex_leaf): the same
 * outcome, and the same fuel; and twice more so, on one unit of fuel per
 * run, with a test's cache of the automaton's steps and without it
 * (no_regex_dfa): the same outcome and fuel, and the same pauses.
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
 *   @shapes PLUCK(k) - ...         (optional: each function's shape, in function
 *                                   index order, the main program excluded;
 *                                   `-` for none)
 *   @clock 1704067200123456789     (optional: the host clock, in nanoseconds
 *                                   since 1970-01-01T00:00Z, an integer of
 *                                   any sign; sjs_vm_set_clock before every
 *                                   evaluation of the case. Without it the
 *                                   clock is never set, and Temporal.Now
 *                                   faults. diff-node gives the polyfill's
 *                                   Temporal.Now the same instant)
 *   @seed 42                       (optional: Math.random's seed, an integer
 *                                   of any sign; sjs_vm_set_seed before every
 *                                   evaluation of the case. Without it the
 *                                   seed is never set, and Math.random
 *                                   faults. diff-node runs the case in a
 *                                   `node --random-seed=<seed>` of its own,
 *                                   so the seed is a nonzero int32)
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

#include "internal.h"

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
    int has_shapes;
    char shapes[1024];
    int has_clock;
    sjs_epoch clock;
    int has_seed;
    int64_t seed;
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
    uint64_t events;        /* the hash of the compile's events */
    char shapes[4096];      /* each function's shape, as @shapes writes them */
    uint64_t fuel;          /* the evaluation's */
    uint64_t pauses;        /* the hash of the fuel used at each pause */
    uint64_t program;       /* the hash of the program, its functions' shapes left out */
} outcome;

typedef struct {
    sjs_context *context;
    sjs_allocator allocator;   /* what the parser allocates with */
    registry *modules;
    const sjs_str *global_names;
    const sjs_value *globals;
    uint32_t global_count;
    const sjs_epoch *clock;    /* the host clock, or NULL: never set */
    const int64_t *seed;       /* Math.random's seed, or NULL: never set */
} environment;

typedef struct {
    uint32_t chunk;
    uint64_t fuel;
    bool no_shapes;
    bool no_regex_leaf;
    bool no_regex_dfa;
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

/* ------------------------------------------------------------------------
 * Node identity: a tee between the parser and the compiler that checks each
 * event's node and start (docs/matcher.md, section 1), with a table of its
 * own of which events open, separate and close a node:
 *
 *   - an event starts no later than its own token (but CHAIN_END, whose
 *     `at` is the bracket around its chain);
 *   - a bracket's BEGIN, separators and END, and an operator's markers and
 *     its last event, share one id and one start, which no other node uses;
 *   - every other event is a node of its own, with an id never seen before;
 *   - ids are never 0 but on END, which closes every bracket.
 */

enum { NODE_OWN = 0, NODE_OPEN, NODE_INNER, NODE_CLOSE };

/* What an event is to its node; for NODE_INNER and NODE_CLOSE, the kinds
 * that open the node it belongs to (two at most). */
typedef struct {
    uint8_t role;
    uint8_t owner;
    uint8_t other_owner;
} node_role;

static node_role role_of(sjs_event_kind kind) {
    switch (kind) {
    case SJS_EV_TEMPLATE_BEGIN:
    case SJS_EV_ARRAY_BEGIN:
    case SJS_EV_OBJECT_BEGIN:
    case SJS_EV_CALL_BEGIN:
    case SJS_EV_NEW_BEGIN:
    case SJS_EV_LOGICAL:
    case SJS_EV_CONDITIONAL:
    case SJS_EV_ARROW_BEGIN:
    case SJS_EV_BLOCK_STATEMENT_BEGIN:
    case SJS_EV_IF:
    case SJS_EV_ARRAY_PATTERN_BEGIN:
    case SJS_EV_OBJECT_PATTERN_BEGIN:
    case SJS_EV_DEFAULT:
    case SJS_EV_CONST_BEGIN:
    case SJS_EV_ASSIGN: /* MUTATION */
        return (node_role){ NODE_OPEN, 0, 0 };
    case SJS_EV_TEMPLATE_END: return (node_role){ NODE_CLOSE, SJS_EV_TEMPLATE_BEGIN, 0 };
    case SJS_EV_ELEMENT:
    case SJS_EV_SPREAD_ELEMENT: return (node_role){ NODE_INNER, SJS_EV_ARRAY_BEGIN, 0 };
    case SJS_EV_ARRAY_END: return (node_role){ NODE_CLOSE, SJS_EV_ARRAY_BEGIN, 0 };
    case SJS_EV_KEY:
    case SJS_EV_COMPUTED_KEY:
    case SJS_EV_PROPERTY:
    case SJS_EV_SPREAD_PROPERTY: return (node_role){ NODE_INNER, SJS_EV_OBJECT_BEGIN, 0 };
    case SJS_EV_OBJECT_END: return (node_role){ NODE_CLOSE, SJS_EV_OBJECT_BEGIN, 0 };
    case SJS_EV_ARGUMENT:
    case SJS_EV_SPREAD_ARGUMENT: return (node_role){ NODE_INNER, SJS_EV_CALL_BEGIN, SJS_EV_NEW_BEGIN };
    case SJS_EV_CALL_END: return (node_role){ NODE_CLOSE, SJS_EV_CALL_BEGIN, 0 };
    case SJS_EV_NEW_END: return (node_role){ NODE_CLOSE, SJS_EV_NEW_BEGIN, 0 };
    case SJS_EV_LOGICAL_END: return (node_role){ NODE_CLOSE, SJS_EV_LOGICAL, 0 };
    case SJS_EV_CONDITIONAL_ELSE: return (node_role){ NODE_INNER, SJS_EV_CONDITIONAL, 0 };
    case SJS_EV_CONDITIONAL_END: return (node_role){ NODE_CLOSE, SJS_EV_CONDITIONAL, 0 };
    case SJS_EV_PARAM:
    case SJS_EV_REST_PARAM:
    case SJS_EV_PARAMS_END:
    case SJS_EV_BLOCK_BEGIN:
    case SJS_EV_BLOCK_END: return (node_role){ NODE_INNER, SJS_EV_ARROW_BEGIN, 0 };
    case SJS_EV_ARROW_END: return (node_role){ NODE_CLOSE, SJS_EV_ARROW_BEGIN, 0 };
    case SJS_EV_BLOCK_STATEMENT_END: return (node_role){ NODE_CLOSE, SJS_EV_BLOCK_STATEMENT_BEGIN, 0 };
    case SJS_EV_ELSE: return (node_role){ NODE_INNER, SJS_EV_IF, 0 };
    case SJS_EV_IF_END: return (node_role){ NODE_CLOSE, SJS_EV_IF, 0 };
    case SJS_EV_PATTERN_INDEX: return (node_role){ NODE_INNER, SJS_EV_ARRAY_PATTERN_BEGIN, 0 };
    case SJS_EV_ARRAY_PATTERN_END: return (node_role){ NODE_CLOSE, SJS_EV_ARRAY_PATTERN_BEGIN, 0 };
    case SJS_EV_PATTERN_KEY:
    case SJS_EV_PATTERN_COMPUTED_KEY: return (node_role){ NODE_INNER, SJS_EV_OBJECT_PATTERN_BEGIN, 0 };
    case SJS_EV_PATTERN_REST: return (node_role){ NODE_INNER, SJS_EV_ARRAY_PATTERN_BEGIN, SJS_EV_OBJECT_PATTERN_BEGIN };
    case SJS_EV_OBJECT_PATTERN_END: return (node_role){ NODE_CLOSE, SJS_EV_OBJECT_PATTERN_BEGIN, 0 };
    case SJS_EV_DEFAULT_END: return (node_role){ NODE_CLOSE, SJS_EV_DEFAULT, 0 };
    case SJS_EV_CONST_END: return (node_role){ NODE_CLOSE, SJS_EV_CONST_BEGIN, 0 };
    case SJS_EV_ASSIGN_END: return (node_role){ NODE_CLOSE, SJS_EV_ASSIGN, 0 }; /* MUTATION */
    default: return (node_role){ NODE_OWN, 0, 0 };
    }
}

#define MAX_OPEN_NODES 4096

typedef struct {
    sjs_sink downstream;
    struct { uint32_t node; uint8_t kind; uint32_t start; } open[MAX_OPEN_NODES];
    uint32_t depth;
    uint8_t *seen;          /* by id: 1 when a node has used it */
    uint32_t seen_capacity;
    uint64_t hash;          /* of every event's kind, node and start, to compare push modes */
    char problem[256];      /* the first one, or empty */
} node_check;

static void node_problem(node_check *check, const sjs_event *e, const char *what) {
    if (check->problem[0] != '\0') return;
    snprintf(check->problem, sizeof check->problem, "event %d (node %u, start %u, at %u:%u): %s", (int)e->kind, e->node,
             e->start.offset, e->at.line, e->at.column, what);
}

/* Note that a node uses `id`; false when one already did. */
static int claim_node(node_check *check, uint32_t id) {
    if (id >= check->seen_capacity) {
        uint32_t capacity = check->seen_capacity == 0 ? 1024u : check->seen_capacity;
        while (capacity <= id) capacity *= 2u;
        uint8_t *seen = realloc(check->seen, capacity);
        if (seen == NULL) abort();
        memset(seen + check->seen_capacity, 0, capacity - check->seen_capacity);
        check->seen = seen;
        check->seen_capacity = capacity;
    }
    if (check->seen[id]) return 0;
    check->seen[id] = 1;
    return 1;
}

static void check_node(node_check *check, const sjs_event *e) {
    uint32_t values[5] = { (uint32_t)e->kind, e->node, e->start.offset, e->start.line, e->start.column };
    for (int i = 0; i < 5; i++) check->hash = (check->hash ^ values[i]) * 1099511628211ull;
    if (e->kind != SJS_EV_CHAIN_END && e->start.offset > e->at.offset) node_problem(check, e, "starts after its own token");
    if (e->kind == SJS_EV_END) {
        if (e->node != 0 || e->start.offset != 0 || e->start.line != 1 || e->start.column != 1) {
            node_problem(check, e, "END is node 0, from the start");
        }
        if (check->depth != 0) node_problem(check, e, "END while a node is open");
        return;
    }
    if (e->node == 0) {
        node_problem(check, e, "node 0 is the program's, whose only event is END");
        return;
    }
    node_role role = role_of(e->kind);
    uint32_t top = check->depth;
    bool of_top = top > 0 && check->open[top - 1].node == e->node;
    if (of_top) {
        uint8_t owner = check->open[top - 1].kind;
        /* A template's pieces and an else-if's IF are their node's own events. */
        bool inner = (role.role == NODE_INNER || role.role == NODE_CLOSE) && (owner == role.owner || owner == role.other_owner);
        if ((e->kind == SJS_EV_STRING && owner == SJS_EV_TEMPLATE_BEGIN) || (e->kind == SJS_EV_IF && owner == SJS_EV_IF)) inner = true;
        if (!inner) node_problem(check, e, "has the id of the open node, of another kind");
        if (e->start.offset != check->open[top - 1].start) node_problem(check, e, "starts elsewhere than its node");
        if (role.role == NODE_CLOSE) check->depth--;
        return;
    }
    if (role.role == NODE_INNER || role.role == NODE_CLOSE) {
        node_problem(check, e, "does not have the id of the node open around it");
        return;
    }
    if (!claim_node(check, e->node)) node_problem(check, e, "a new node with an id already used");
    if (role.role == NODE_OPEN) {
        if (check->depth == MAX_OPEN_NODES) {
            node_problem(check, e, "too many nodes open for the check");
            return;
        }
        check->open[check->depth].node = e->node;
        check->open[check->depth].kind = (uint8_t)e->kind;
        check->open[check->depth].start = e->start.offset;
        check->depth++;
    }
}

static sjs_result node_check_event(void *self, const sjs_event *e) {
    node_check *check = self;
    check_node(check, e);
    return check->downstream.v->event(check->downstream.self, e);
}

static const sjs_sink_vtable node_check_vtable = { node_check_event };

/* The hash of the events of the last compile, for comparing push modes. */
static uint64_t last_events_hash = 0;

/* Parse and compile `source`, pushing it `chunk` bytes at a time. A stream
 * whose nodes break the rules above is SJS_ERR_INVALID, after a message. */
static sjs_result compile(const environment *env, const char *source, sjs_goal goal, mode m, sjs_program **program,
                          sjs_error *error) {
    uint32_t chunk = m.chunk;
    sjs_compiler *compiler;
    sjs_compiler_config config = { env->context, goal, env->global_names, env->global_count,
                                   { env->modules, &modules_vtable }, m.no_shapes };
    sjs_result r = sjs_compiler_init(&compiler, &config);
    if (r != SJS_OK) return r;
    static node_check check;
    check.downstream = sjs_compiler_sink(compiler);
    check.depth = 0;
    check.hash = 14695981039346656037ull;
    check.problem[0] = '\0';
    if (check.seen != NULL) memset(check.seen, 0, check.seen_capacity);
    sjs_parser *parser;
    sjs_parser_config parser_config = { env->allocator, SJS_LIMITS_DEFAULT, goal, { &check, &node_check_vtable } };
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
    last_events_hash = check.hash;
    if (check.problem[0] != '\0') {
        fprintf(stderr, "the events' nodes: %s\n", check.problem);
        if (r == SJS_OK) sjs_program_release(*program);
        return SJS_ERR_INVALID;
    }
    return r;
}

static uint64_t hash_bytes(uint64_t hash, const void *bytes, size_t length) {
    const uint8_t *p = bytes;
    for (size_t i = 0; i < length; i++) hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
}

/* The fuel the last evaluation used, and the hash of what it had used at
 * each pause. */
static uint64_t last_fuel = 0;
static uint64_t last_pauses = 0;
/* The next evaluation runs every regular expression match as a loop, and
 * every match by the automaton itself, not its cache of steps. */
static bool evaluate_no_regex_leaf = false;
static bool evaluate_no_regex_dfa = false;

/* An error's subject, kept past the VM it points into. */
static char fault_subject[1024];

/* Run a program to its end; the result is the caller's. An error's subject
 * is copied out of the VM before it is destroyed (staticjs.h: it is valid
 * until the stage that reported it is destroyed). */
static sjs_result evaluate(const environment *env, sjs_program *program, uint64_t fuel, sjs_value *result, sjs_error *error) {
    sjs_vm *vm;
    sjs_vm_config config = { env->context, evaluate_no_regex_leaf, evaluate_no_regex_dfa };
    sjs_result made = sjs_vm_init(&vm, &config);
    if (made != SJS_OK) return made;
    if ((env->clock != NULL && sjs_vm_set_clock(vm, env->clock) != SJS_OK) ||
        (env->seed != NULL && sjs_vm_set_seed(vm, *env->seed) != SJS_OK)) {
        sjs_vm_destroy(vm);
        return SJS_ERR_INVALID;
    }
    sjs_run_params params = { program, env->globals, env->global_count };
    sjs_result r = sjs_vm_start(vm, &params);
    last_pauses = 14695981039346656037ull;
    if (r == SJS_OK) {
        sjs_run run;
        while ((run = sjs_vm_run(vm, fuel)) == SJS_RUN_PAUSED) {
            uint64_t used = sjs_vm_fuel_used(vm);
            last_pauses = hash_bytes(last_pauses, &used, sizeof used);
        }
        if (run == SJS_RUN_DONE) {
            r = sjs_vm_take_result(vm, result);
        } else {
            sjs_vm_error(vm, error);
            uint32_t length = error->subject.length < sizeof fault_subject ? error->subject.length
                                                                            : (uint32_t)sizeof fault_subject;
            if (length) memcpy(fault_subject, error->subject.ptr, length);
            error->subject = (sjs_str){ fault_subject, length };
            r = SJS_ERR_PROGRAM;
        }
    }
    last_fuel = sjs_vm_fuel_used(vm);
    sjs_vm_destroy(vm);
    return r;
}

/* The program's code, numbers, constants' kinds, functions but for their
 * shapes, captures and lines: what a match must not change. */
static uint64_t hash_program(const sjs_program *program) {
    uint64_t h = 14695981039346656037ull;
    h = hash_bytes(h, program->code, program->code_length);
    h = hash_bytes(h, program->numbers, program->number_count * sizeof(double));
    for (uint32_t i = 0; i < program->constant_count; i++) {
        sjs_value k = program->constants[i];
        h = hash_bytes(h, &k.kind, 1);
        if (k.kind == SJS_STRING) h = hash_bytes(h, SJS_AS_STRING(k)->bytes, SJS_AS_STRING(k)->length);
        else if (k.kind == SJS_NUMBER) h = hash_bytes(h, &k.as.number, sizeof k.as.number);
    }
    for (uint32_t i = 0; i < program->function_count; i++) {
        const sjs_function_info *f = &program->functions[i];
        uint32_t fields[6] = { f->code, f->captures, f->params, f->locals, f->capture_count, f->flags };
        h = hash_bytes(h, fields, sizeof fields);
    }
    h = hash_bytes(h, program->captures, program->capture_count * sizeof(sjs_capture_info));
    h = hash_bytes(h, program->lines, program->line_count * sizeof(sjs_line));
    return h;
}

/* An operand as JavaScript writes the literal: 2, "open", true, null. */
static void describe_operand(const sjs_program *program, uint32_t operand, char out[256]) {
    uint32_t index = sjs_shape_operand_index(operand);
    switch (sjs_shape_operand_kind(operand)) {
    case SJS_OPERAND_NUMBER: {
        uint32_t n = sjs_number_format(program->numbers[index], out);
        out[n] = '\0';
        break;
    }
    case SJS_OPERAND_TEXT: {
        const sjs_string *text = SJS_AS_STRING(program->constants[index]);
        snprintf(out, 256, "\"%.*s\"", (int)text->length, text->bytes);
        break;
    }
    case SJS_OPERAND_TRUE:
        snprintf(out, 256, "true");
        break;
    case SJS_OPERAND_FALSE:
        snprintf(out, 256, "false");
        break;
    default:
        snprintf(out, 256, "null");
        break;
    }
}

/* Each function's shape, as @shapes writes them: `-`, or the shape's name
 * and its arguments, a text as the constant's text, a flag as true or
 * false, an operator by its name and an operand as its literal. */
static void describe_shapes(const sjs_program *program, char *out, size_t capacity) {
    size_t used = 0;
    out[0] = '\0';
    for (uint32_t i = 1; i < program->function_count && used < capacity; i++) {
        const sjs_function_info *f = &program->functions[i];
        used += (size_t)snprintf(out + used, capacity - used, "%s", i > 1 ? " " : "");
        if (used >= capacity) break;
        if (f->shape == SJS_SHAPE_NONE) {
            used += (size_t)snprintf(out + used, capacity - used, "-");
            continue;
        }
        used += (size_t)snprintf(out + used, capacity - used, "%s(", sjs_shape_name(f->shape));
        for (uint32_t a = 0; a < SJS_SHAPE_ARGS && sjs_shape_arg_kind(f->shape, a) != SJS_SHAPE_ARG_NONE && used < capacity; a++) {
            const char *comma = a > 0 ? ", " : "";
            switch (sjs_shape_arg_kind(f->shape, a)) {
            case SJS_SHAPE_ARG_TEXT: {
                const sjs_string *key = SJS_AS_STRING(program->constants[f->shape_args[a]]);
                used += (size_t)snprintf(out + used, capacity - used, "%s%.*s", comma, (int)key->length, key->bytes);
                break;
            }
            case SJS_SHAPE_ARG_FLAG:
                used += (size_t)snprintf(out + used, capacity - used, "%s%s", comma, f->shape_args[a] ? "true" : "false");
                break;
            case SJS_SHAPE_ARG_OPERATOR:
                used += (size_t)snprintf(out + used, capacity - used, "%s%s", comma, sjs_shape_operator_name(f->shape_args[a]));
                break;
            case SJS_SHAPE_ARG_OPERAND: {
                char operand[256];
                describe_operand(program, f->shape_args[a], operand);
                used += (size_t)snprintf(out + used, capacity - used, "%s%s", comma, operand);
                break;
            }
            default:
                used += (size_t)snprintf(out + used, capacity - used, "%s%u", comma, f->shape_args[a]);
                break;
            }
        }
        if (used < capacity) used += (size_t)snprintf(out + used, capacity - used, ")");
    }
}

static void run_case(const environment *env, const spec_case *c, mode m, outcome *out) {
    sjs_program *program = NULL;
    sjs_error error = { 0 };
    sjs_result r = compile(env, c->source, c->is_module ? SJS_GOAL_MODULE : SJS_GOAL_EXPRESSION, m, &program, &error);
    out->events = last_events_hash;
    out->shapes[0] = '\0';
    out->fuel = 0;
    out->program = 0;
    if (r != SJS_OK) {
        if (r == SJS_ERR_PROGRAM) {
            snprintf(out->text, sizeof out->text, "error %s %u:%u %.*s", code_name(error.code), error.at.line,
                     error.at.column, (int)error.message.length, error.message.ptr);
        } else {
            snprintf(out->text, sizeof out->text, "error result %d", r);
        }
        return;
    }
    describe_shapes(program, out->shapes, sizeof out->shapes);
    out->program = hash_program(program);
    sjs_value value;
    evaluate_no_regex_leaf = m.no_regex_leaf;
    evaluate_no_regex_dfa = m.no_regex_dfa;
    r = evaluate(env, program, m.fuel, &value, &error);
    evaluate_no_regex_leaf = false;
    evaluate_no_regex_dfa = false;
    out->fuel = last_fuel;
    out->pauses = last_pauses;
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
        if (compile(env, from->entries[i].source, SJS_GOAL_MODULE, (mode){ 4096, 0, false, false, false }, &program, &error) != SJS_OK) return 0;
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
    environment env = { context, { &f, &failing_vtable }, &loaded, NULL, NULL, 0, NULL, NULL };
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
            if (compile(&env, c->global_sources[i], SJS_GOAL_EXPRESSION, (mode){ 4096, 0, false, false, false }, &program, &error) != SJS_OK) {
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
            with.clock = c->has_clock ? &c->clock : NULL;
            with.seed = c->has_seed ? &c->seed : NULL;
            run_case(&with, c, (mode){ 1u << 30, UINT64_MAX, false, false, false }, &got);
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
        if (compile(&plain, c->global_sources[i], SJS_GOAL_EXPRESSION, (mode){ 4096, 0, false, false, false }, &program, &error) != SJS_OK ||
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
    with.clock = c->has_clock ? &c->clock : NULL;
    with.seed = c->has_seed ? &c->seed : NULL;
    static outcome whole;
    static outcome bytes;
    static outcome chunks;
    static outcome plain_run;
    run_case(&with, c, (mode){ 1u << 30, UINT64_MAX, false, false, false }, &whole);
    run_case(&with, c, (mode){ 1, 1, false, false, false }, &bytes);
    run_case(&with, c, (mode){ 7, 13, false, false, false }, &chunks);
    run_case(&with, c, (mode){ 1u << 30, UINT64_MAX, true, false, false }, &plain_run);
    /* A source with a slash may match a regular expression: once more with
     * every match a loop, which must give the same outcome and fuel; and
     * with every match a loop on one unit of fuel per run, by the cache of
     * a test's steps and by the automaton alone, which must give the same
     * outcome and fuel and pause at the same points. */
    static outcome loops;
    static outcome cached;
    static outcome uncached;
    bool slashed = strchr(c->source, '/') != NULL;
    if (slashed) {
        run_case(&with, c, (mode){ 1u << 30, UINT64_MAX, false, true, false }, &loops);
        run_case(&with, c, (mode){ 1u << 30, 1, false, true, false }, &cached);
        run_case(&with, c, (mode){ 1u << 30, 1, false, true, true }, &uncached);
    }
    int ok = check(c, &whole, file);
    if (ok && slashed && (strcmp(whole.text, loops.text) != 0 || whole.fuel != loops.fuel)) {
        fprintf(stderr, "%s:%d: %s: with every match a loop\n  leaf:  %s (fuel %llu)\n  loops: %s (fuel %llu)\n", file, c->line,
                c->name, whole.text, (unsigned long long)whole.fuel, loops.text, (unsigned long long)loops.fuel);
        ok = 0;
    }
    if (ok && slashed &&
        (strcmp(whole.text, cached.text) != 0 || strcmp(whole.text, uncached.text) != 0 || cached.fuel != whole.fuel ||
         uncached.fuel != whole.fuel || cached.pauses != uncached.pauses)) {
        fprintf(stderr, "%s:%d: %s: every match a loop, paused after each unit\n  cached:   %s (fuel %llu, pauses %016llx)\n"
                "  uncached: %s (fuel %llu, pauses %016llx)\n", file, c->line, c->name, cached.text, (unsigned long long)cached.fuel,
                (unsigned long long)cached.pauses, uncached.text, (unsigned long long)uncached.fuel, (unsigned long long)uncached.pauses);
        ok = 0;
    }
    if (ok && c->has_shapes && strcmp(whole.shapes, c->shapes) != 0) {
        fprintf(stderr, "%s:%d: %s: shapes\n  expected: %s\n  got:      %s\n", file, c->line, c->name, c->shapes, whole.shapes);
        ok = 0;
    }
    if (ok && (strcmp(whole.shapes, bytes.shapes) != 0 || strcmp(whole.shapes, chunks.shapes) != 0)) {
        fprintf(stderr, "%s:%d: %s: the shapes differ between push modes\n  whole:  %s\n  bytes:  %s\n  chunks: %s\n", file,
                c->line, c->name, whole.shapes, bytes.shapes, chunks.shapes);
        ok = 0;
    }
    if (ok && whole.program != plain_run.program) {
        fprintf(stderr, "%s:%d: %s: compiles to another program without shapes\n", file, c->line, c->name);
        ok = 0;
    }
    if (ok && strcmp(whole.text, plain_run.text) != 0) {
        fprintf(stderr, "%s:%d: %s: without shapes\n  with:    %s\n  without: %s\n", file, c->line, c->name, whole.text,
                plain_run.text);
        ok = 0;
    }
    if (ok && strspn(plain_run.shapes, "- ") != strlen(plain_run.shapes)) {
        fprintf(stderr, "%s:%d: %s: tagged without shapes: %s\n", file, c->line, c->name, plain_run.shapes);
        ok = 0;
    }
    if (ok && whole.fuel > plain_run.fuel) {
        fprintf(stderr, "%s:%d: %s: more fuel with shapes (%llu) than without (%llu)\n", file, c->line, c->name,
                (unsigned long long)whole.fuel, (unsigned long long)plain_run.fuel);
        ok = 0;
    }
    if (ok && strcmp(whole.text, bytes.text) != 0) {
        fprintf(stderr, "%s:%d: %s: pushed a byte at a time\n  whole: %s\n  bytes: %s\n", file, c->line, c->name, whole.text, bytes.text);
        ok = 0;
    }
    if (ok && strcmp(whole.text, chunks.text) != 0) {
        fprintf(stderr, "%s:%d: %s: pushed in chunks\n  whole:  %s\n  chunks: %s\n", file, c->line, c->name, whole.text, chunks.text);
        ok = 0;
    }
    if (ok && (whole.events != bytes.events || whole.events != chunks.events)) {
        fprintf(stderr, "%s:%d: %s: the events' nodes or starts differ between push modes\n", file, c->line, c->name);
        ok = 0;
    }
    for (int i = 0; i < c->global_count; i++) sjs_release(env->context, values[i]);
    if (ok && full && !sweep(c, &whole, file, env->modules)) ok = 0;
    if (ok && c->is_module && strncmp(whole.text, "value ", 6) == 0) {
        /* Evaluate once more, and keep the exports for the cases after. */
        sjs_program *program;
        sjs_error error = { 0 };
        sjs_value exports;
        environment clocked = *env;
        clocked.clock = c->has_clock ? &c->clock : NULL;
        clocked.seed = c->has_seed ? &c->seed : NULL;
        if (compile(env, c->source, SJS_GOAL_MODULE, (mode){ 4096, 0, false, false, false }, &program, &error) == SJS_OK) {
            if (evaluate(&clocked, program, UINT64_MAX, &exports, &error) == SJS_OK &&
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

/* @clock's nanoseconds, an integer of any sign and up to 22 digits, as
 * seconds (toward negative infinity) and nanoseconds. */
static int parse_clock(const char *text, sjs_epoch *out) {
    int negative = *text == '-';
    if (negative) text++;
    size_t n = strspn(text, "0123456789");
    if (n == 0 || n > 22 || (text[n] != '\0' && text[n] != '\n' && text[n] != '\r')) return 0;
    int64_t seconds = 0;
    uint32_t nanoseconds = 0;
    for (size_t i = 0; i < n; i++) {
        if (i + 9 < n) seconds = seconds * 10 + (text[i] - '0');
        else nanoseconds = nanoseconds * 10u + (uint32_t)(text[i] - '0');
    }
    if (negative && nanoseconds != 0) {
        seconds = -seconds - 1;
        nanoseconds = 1000000000u - nanoseconds;
    } else if (negative) {
        seconds = -seconds;
    }
    *out = (sjs_epoch){ seconds, nanoseconds };
    return 1;
}

/* @seed's integer, of any sign, within an int64. */
static int parse_seed(const char *text, int64_t *out) {
    int negative = *text == '-';
    if (negative) text++;
    size_t n = strspn(text, "0123456789");
    if (n == 0 || n > 19 || (text[n] != '\0' && text[n] != '\n' && text[n] != '\r')) return 0;
    uint64_t magnitude = 0;
    for (size_t i = 0; i < n; i++) magnitude = magnitude * 10u + (uint64_t)(text[i] - '0');
    if (magnitude > (negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX)) return 0;
    *out = negative ? (int64_t)(0u - magnitude) : (int64_t)magnitude;
    return 1;
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
    environment env = { context, { NULL, &heap_vtable }, &modules, NULL, NULL, 0, NULL, NULL };
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
        if (where == SOURCE && c.source[0] == '\0' && strncmp(line, "@clock ", 7) == 0) {
            if (!parse_clock(line + 7, &c.clock)) {
                fprintf(stderr, "%s:%d: @clock takes an integer of nanoseconds\n", path, number);
                t->failed++;
            }
            c.has_clock = 1;
            continue;
        }
        if (where == SOURCE && c.source[0] == '\0' && strncmp(line, "@seed ", 6) == 0) {
            if (!parse_seed(line + 6, &c.seed)) {
                fprintf(stderr, "%s:%d: @seed takes an integer\n", path, number);
                t->failed++;
            }
            c.has_seed = 1;
            continue;
        }
        if (where == SOURCE && c.source[0] == '\0' && strncmp(line, "@shapes ", 8) == 0) {
            c.has_shapes = 1;
            snprintf(c.shapes, sizeof c.shapes, "%s", line + 8);
            trim_end(c.shapes);
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
