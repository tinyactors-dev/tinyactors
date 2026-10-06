/*
 * staticjs public API.
 *
 * staticjs is a pure, total subset of JavaScript (docs/proposals/staticjs.md
 * in the repository). A program it accepts is a valid JavaScript program, and
 * evaluating it gives the same result JavaScript gives, or a fault; never a
 * different value.
 *
 * Every stage is pushed, never pulls, and never recurses:
 *
 *   bytes --push--> parser --events--> sink (the compiler, or any other)
 *   program + fuel --push--> evaluator --> value --> writer --> bytes
 *
 * The library performs no I/O, uses no C library, and allocates only through
 * the embedder's allocator. This file is the whole public interface.
 *
 * Every public function is declared with SJS_API. Define SJS_API before
 * including this header to add export attributes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef SJS_API
#define SJS_API
#endif

#define SJS_VERSION_MAJOR 0
#define SJS_VERSION_MINOR 3
#define SJS_VERSION_PATCH 0
/* (major << 16) | (minor << 8) | patch; sjs_version() answers it for the library linked. */
#define SJS_VERSION ((SJS_VERSION_MAJOR << 16) | (SJS_VERSION_MINOR << 8) | SJS_VERSION_PATCH)

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Results, text and positions
 */

typedef int32_t sjs_result;

enum {
    /* The call succeeded. */
    SJS_OK = 0,
    /* Writer only: the buffer is full. Take what it holds and call again. */
    SJS_NEED_SPACE = 1,
    /* API misuse: a null handle, a call in the wrong order, a value from
     * another context. Never caused by a program. */
    SJS_ERR_INVALID = -1,
    /* The allocator refused a request. */
    SJS_ERR_NOMEM = -2,
    /* An sjs_limits value was reached. */
    SJS_ERR_LIMIT = -3,
    /* The name, key or index does not exist. */
    SJS_ERR_NOT_FOUND = -4,
    /* The program is not staticjs, or its evaluation faulted. Read the
     * sjs_error of the stage that returned it. */
    SJS_ERR_PROGRAM = -5
};

/* Borrowed views. Lengths count bytes. Text is UTF-8. */
typedef struct { const char *ptr; uint32_t length; } sjs_str;
typedef struct { const uint8_t *ptr; uint32_t length; } sjs_bytes;

/* A string literal as an sjs_str. The trailing NUL is not part of the text. */
#define SJS_STR(literal) ((sjs_str){ (literal), (uint32_t)(sizeof(literal) - 1u) })

/* A place in the source. `offset` counts bytes from the first byte pushed;
 * `line` and `column` count from 1, and `column` counts bytes. */
typedef struct {
    uint32_t offset;
    uint32_t line;
    uint32_t column;
} sjs_position;

typedef enum {
    SJS_ERROR_NONE = 0,
    /* Not JavaScript. */
    SJS_ERROR_SYNTAX,
    /* JavaScript, but outside staticjs: a loop, an assignment, a regular
     * expression, a name that refers to itself. */
    SJS_ERROR_UNSUPPORTED,
    /* A name that is not bound anywhere, or bound twice. */
    SJS_ERROR_NAME,
    /* An sjs_limits value was reached. */
    SJS_ERROR_LIMIT,
    /* The allocator refused. */
    SJS_ERROR_MEMORY,
    /* Evaluation stopped where staticjs declines to follow JavaScript, or
     * where JavaScript throws: a property of undefined, `===` on two objects,
     * a function that is already running. */
    SJS_ERROR_FAULT
} sjs_error_code;

/*
 * What went wrong, and where. `message` is a fixed English phrase owned by the
 * library. `subject` names the token or name concerned, when there is one; it
 * points into the stage that reported the error and is valid until that stage
 * is called again or destroyed.
 */
typedef struct {
    sjs_error_code code;
    sjs_position at;
    sjs_str message;
    sjs_str subject;
} sjs_error;

/* ========================================================================
 * Allocation and limits
 */

typedef struct { void *ptr; size_t size; size_t alignment; } sjs_allocator_free_params;

typedef struct sjs_allocator_vtable {
    /* Return `size` bytes aligned to `alignment`, or NULL. */
    void *(*alloc)(void *self, size_t size, size_t alignment);
    /* Release a block from alloc(); `params` repeats its size and alignment. */
    void (*free)(void *self, const sjs_allocator_free_params *params);
} sjs_allocator_vtable;

typedef struct { void *self; const sjs_allocator_vtable *v; } sjs_allocator;

/* Every construct a program controls has a number here. */
typedef struct {
    uint32_t max_source_bytes;  /* bytes pushed to one parser */
    uint32_t max_token_bytes;   /* one identifier, number, string or template piece */
    uint32_t max_nesting;       /* open brackets, operators, functions, blocks and ifs at once;
                                 * an else-if chain counts once */
    uint32_t max_lookahead;     /* tokens held to tell `(a, b = 1) =>` from `(a, b)`: an arrow
                                 * function's parameter list, defaults included, must fit */
    uint32_t max_held_events;   /* events the parser holds to send in evaluation order: a const's
                                 * pattern, its default values included, waits for its value */
    uint32_t max_stack;         /* evaluator value slots */
    uint32_t max_frames;        /* evaluator frames: calls and looping built-ins */
    uint32_t max_value_items;   /* elements of one array, object or set */
    uint32_t max_string_bytes;  /* bytes of one string value */
} sjs_limits;

#define SJS_LIMITS_DEFAULT                     \
    ((sjs_limits){                             \
        .max_source_bytes = 1024u * 1024u,     \
        .max_token_bytes = 64u * 1024u,        \
        .max_nesting = 256u,                   \
        .max_lookahead = 256u,                 \
        .max_held_events = 64u * 1024u,        \
        .max_stack = 4096u,                    \
        .max_frames = 256u,                    \
        .max_value_items = 1024u * 1024u,      \
        .max_string_bytes = 16u * 1024u * 1024u })

/* The library's version, SJS_VERSION of the header it was built from. */
SJS_API uint32_t sjs_version(void);

/* ========================================================================
 * Context and values
 *
 * A context owns the allocator values are made with. Values are immutable
 * and reference counted; a value of one context must not meet another
 * context. A context is used by one thread at a time.
 */

typedef struct sjs_context sjs_context;

typedef struct {
    sjs_allocator allocator;
    sjs_limits limits;
} sjs_context_config;

SJS_API sjs_result sjs_context_init(sjs_context **out, const sjs_context_config *config);
/* Every value, program and stage made with the context must be gone first. */
SJS_API void sjs_context_destroy(sjs_context *context);
/* Bytes the context's values, programs and stages hold now. */
SJS_API size_t sjs_context_live_bytes(const sjs_context *context);

typedef enum {
    SJS_UNDEFINED = 0,
    SJS_NULL,
    SJS_BOOLEAN,
    SJS_NUMBER,
    SJS_STRING,
    SJS_ARRAY,
    SJS_OBJECT,
    SJS_SET,
    SJS_FUNCTION
} sjs_kind;

typedef struct sjs_heap sjs_heap;

/*
 * A value. Copying the struct does not retain it. `internal` is zero in every
 * value the library hands out and must be zero in every value given to it.
 */
typedef struct {
    uint8_t kind;      /* sjs_kind */
    uint8_t internal;
    uint16_t reserved;
    uint32_t boolean;  /* SJS_BOOLEAN */
    union {
        double number; /* SJS_NUMBER */
        sjs_heap *heap;/* SJS_STRING and the kinds after it */
    } as;
} sjs_value;

SJS_API sjs_value sjs_undefined(void);
SJS_API sjs_value sjs_null(void);
SJS_API sjs_value sjs_boolean(bool value);
SJS_API sjs_value sjs_number(double value);

/* Retain adds a reference; release drops one and frees the value with its
 * last. Neither recurses, however deeply a value nests. */
SJS_API void sjs_retain(sjs_value value);
SJS_API void sjs_release(sjs_context *context, sjs_value value);

/* A new string with a copy of `text`, which must be well-formed UTF-8. */
SJS_API sjs_result sjs_string_new(sjs_context *context, sjs_str text, sjs_value *out);
/* The bytes of a string. Valid while the string is. */
SJS_API sjs_str sjs_string_text(sjs_value string);

/*
 * Building arrays, objects and sets. A new one is empty, with room for
 * `capacity` items; it can be added to while the caller holds its only
 * reference, and grows when full. Adding takes over the caller's reference to
 * the item.
 */
SJS_API sjs_result sjs_array_new(sjs_context *context, uint32_t capacity, sjs_value *out);
SJS_API sjs_result sjs_array_append(sjs_context *context, sjs_value *array, sjs_value element);
SJS_API sjs_result sjs_object_new(sjs_context *context, uint32_t capacity, sjs_value *out);

typedef struct {
    sjs_str key;
    sjs_value value;
} sjs_property;

/* Set a property as JavaScript's [[Set]] on a fresh object would: a key that
 * is already there keeps its place, and array-index keys come first. */
SJS_API sjs_result sjs_object_put(sjs_context *context, sjs_value *object, const sjs_property *property);
SJS_API sjs_result sjs_set_new(sjs_context *context, uint32_t capacity, sjs_value *out);
/* Add as Set.prototype.add would; only primitives may be elements. On an
 * error the caller keeps its reference to the element. */
SJS_API sjs_result sjs_set_add(sjs_context *context, sjs_value *set, sjs_value element);

/* Elements of an array, properties of an object, elements of a set. */
SJS_API uint32_t sjs_length(sjs_value value);
/* Borrowed: valid while the container is. */
SJS_API sjs_value sjs_array_at(sjs_value array, uint32_t index);
SJS_API sjs_value sjs_set_at(sjs_value set, uint32_t index);
SJS_API sjs_result sjs_object_at(sjs_value object, uint32_t index, sjs_property *out);
SJS_API sjs_result sjs_object_get(sjs_value object, sjs_str key, sjs_value *out);

/*
 * Functions the embedder provides, such as SCXML's In(). A native is called
 * synchronously, must finish quickly and must not change anything a program
 * can see: it is the one way into a program from outside, and the library
 * cannot check that it keeps the rule.
 */
typedef struct {
    sjs_context *context;
    const sjs_value *arguments; /* borrowed: never to be added to, even one with a single reference */
    uint32_t count;
    sjs_value result;           /* set by the native; the program takes its reference */
    sjs_str fault;              /* set by a native that returns SJS_ERR_PROGRAM */
} sjs_native_call;

typedef struct sjs_native_vtable {
    /* SJS_OK with `result` set, or SJS_ERR_PROGRAM with `fault` set. */
    sjs_result (*call)(void *self, sjs_native_call *call);
} sjs_native_vtable;

typedef struct { void *self; const sjs_native_vtable *v; } sjs_native;

SJS_API sjs_result sjs_function_native(sjs_context *context, sjs_native native, sjs_value *out);

/* ========================================================================
 * Syntax events
 *
 * The parser emits a program as a stream of events in evaluation order: the
 * operands of an operator come before it, and markers stand where `&&`, `?:`,
 * `??` and `?.` branch. A sink sees each event once, as the parser reaches
 * it. The compiler is a sink; a formatter or a linter can be another.
 *
 * Where the source puts a pattern before what it takes, the events put it
 * after, as it is evaluated:
 *
 *   const p = v               CONST_BEGIN <v> <p> CONST_END
 *   (a, b = d, ...r) => ...   ARROW_BEGIN(3) PARAM(0) <a> PARAM(1) DEFAULT <d>
 *                             DEFAULT_END <b> REST_PARAM(2) <r> PARAMS_END ...
 *   [x = d, ...r]             ARRAY_PATTERN_BEGIN PATTERN_INDEX(0) DEFAULT <d>
 *                             DEFAULT_END <x> PATTERN_REST(1) <r> ARRAY_PATTERN_END
 *   {k: x = d, [e]: y, ...r}  OBJECT_PATTERN_BEGIN PATTERN_KEY(k) DEFAULT <d>
 *                             DEFAULT_END <x> <e> PATTERN_COMPUTED_KEY <y>
 *                             PATTERN_REST BIND(r) OBJECT_PATTERN_END
 *
 * Statements in a function's block body come in source order; an if's test
 * before its IF, as the test of ?: comes before CONDITIONAL:
 *
 *   if (t) s1                 <t> IF <s1> IF_END
 *   if (t) s1 else s2         <t> IF <s1> ELSE(0) <s2> IF_END
 *   if (t) a else if (u) b else c
 *                             <t> IF <a> ELSE(1) <u> IF <b> ELSE(0) <c> IF_END
 *   { s }                     BLOCK_STATEMENT_BEGIN <s> BLOCK_STATEMENT_END
 *   ;                         (nothing)
 *
 * ELSE comes even when the alternative is `;`. A branch in braces is a
 * BLOCK_STATEMENT, with a scope of its own; a branch without braces has none.
 *
 * MUTATION: an expression statement's value comes before
 * EXPRESSION_STATEMENT, sent only for a statement whose expression, as a
 * whole, is a call (a method that changes its receiver, docs/mutation.md);
 * any other is refused:
 *
 *   x.m(a);                   <x.m(a)> EXPRESSION_STATEMENT
 */

typedef enum {
    /* Values. */
    SJS_EV_NUMBER = 1,       /* number */
    SJS_EV_STRING,           /* text: the cooked string */
    SJS_EV_TRUE,
    SJS_EV_FALSE,
    SJS_EV_NULL,
    SJS_EV_NAME,             /* text: a name read */
    SJS_EV_TEMPLATE_BEGIN,   /* a template with substitutions: strings and */
    SJS_EV_TEMPLATE_END,     /* values alternate; count: how many */
    SJS_EV_ARRAY_BEGIN,
    SJS_EV_ELEMENT,          /* after each element's value */
    SJS_EV_SPREAD_ELEMENT,   /* after `...value` */
    SJS_EV_ARRAY_END,
    SJS_EV_OBJECT_BEGIN,
    SJS_EV_KEY,              /* text: a static key, before its value */
    SJS_EV_COMPUTED_KEY,     /* after the key's value, before the property's */
    SJS_EV_PROPERTY,         /* after a property's value */
    SJS_EV_SPREAD_PROPERTY,  /* after `...value` */
    SJS_EV_OBJECT_END,
    /* Members and calls. */
    SJS_EV_MEMBER,           /* text: `.name` of the value before */
    SJS_EV_INDEX,            /* after `[key]`: the object, then the key */
    SJS_EV_OPTIONAL,         /* `?.`; count: the chain's depth */
    SJS_EV_CHAIN_END,        /* count: the depth of the chain that ends */
    SJS_EV_CALL_BEGIN,       /* after the callee */
    SJS_EV_ARGUMENT,         /* after each argument */
    SJS_EV_SPREAD_ARGUMENT,  /* after `...argument` */
    SJS_EV_CALL_END,         /* count: arguments */
    SJS_EV_NEW_BEGIN,        /* text: the constructor; only `Set` */
    SJS_EV_NEW_END,          /* count: arguments */
    /* Operators; count: an sjs_operator. */
    SJS_EV_UNARY,
    SJS_EV_BINARY,
    SJS_EV_LOGICAL,          /* after the left operand of && || ?? */
    SJS_EV_LOGICAL_END,      /* after the right one */
    SJS_EV_CONDITIONAL,      /* after the test of ?: */
    SJS_EV_CONDITIONAL_ELSE, /* after the consequent */
    SJS_EV_CONDITIONAL_END,  /* after the alternative */
    /* Functions. */
    SJS_EV_ARROW_BEGIN,      /* count: its parameters, a rest parameter included */
    SJS_EV_PARAM,            /* count: the parameter's position; a default value
                              * (DEFAULT ... DEFAULT_END) may follow, then a pattern */
    SJS_EV_REST_PARAM,       /* `...`; count: its position, the last; a pattern follows */
    SJS_EV_PARAMS_END,
    SJS_EV_ARROW_END,
    SJS_EV_BLOCK_BEGIN,      /* a function's block body: its statements */
    SJS_EV_RETURN,           /* after the returned value; count 0: `return;`, anywhere in a body */
    SJS_EV_BLOCK_END,
    /* Statements. */
    SJS_EV_BLOCK_STATEMENT_BEGIN, /* `{` of a block statement: a scope of its own */
    SJS_EV_BLOCK_STATEMENT_END,
    SJS_EV_IF,               /* after an if's test */
    SJS_EV_ELSE,             /* after the consequent; count 1: else if, whose test and IF
                              * follow and continue the chain, which one IF_END ends */
    SJS_EV_IF_END,           /* after the last branch of an if or an else-if chain */
    SJS_EV_EXPRESSION_STATEMENT, /* MUTATION: after the value of a statement that is a call */
    /* Patterns: each one takes the value before it. A target's default
     * value comes before its pattern, as it is evaluated first. */
    SJS_EV_BIND,             /* text: the name bound */
    SJS_EV_ARRAY_PATTERN_BEGIN,
    SJS_EV_PATTERN_INDEX,    /* count: an element's index; a default value may
                              * follow, then a pattern */
    SJS_EV_ARRAY_PATTERN_END,
    SJS_EV_OBJECT_PATTERN_BEGIN,
    SJS_EV_PATTERN_KEY,      /* text: a property's key (a number's in canonical
                              * form); a default value may follow, then a pattern */
    SJS_EV_PATTERN_COMPUTED_KEY, /* after the value of `[key]`; as PATTERN_KEY */
    SJS_EV_PATTERN_REST,     /* `...`, the last item. In an array pattern, count:
                              * the first index it takes, and a pattern follows;
                              * in an object pattern, count 0 and a BIND follows */
    SJS_EV_OBJECT_PATTERN_END,
    SJS_EV_DEFAULT,          /* a default value's events follow: taken when the */
    SJS_EV_DEFAULT_END,      /* value is undefined */
    /* Declarations: the value, then its pattern. */
    SJS_EV_CONST_BEGIN,
    SJS_EV_CONST_END,
    SJS_EV_EXPORT,           /* before CONST_BEGIN: its names are exported */
    SJS_EV_IMPORT,           /* text: the module; count: an sjs_import_kind */
    SJS_EV_EXPORT_DEFAULT,   /* after the value of `export default` */
    SJS_EV_EXPORT_NAME,      /* `export { alias as name }`; count: 1 when name was a string */
    SJS_EV_EXPORT_FROM,      /* a re-export; text: the module; count: an sjs_import_kind */
    /* The end of the program. */
    SJS_EV_END
} sjs_event_kind;

/*
 * What an SJS_EV_IMPORT or SJS_EV_EXPORT_FROM takes from its module. One
 * import or re-export statement gives one event per specifier, in source
 * order, when the statement ends; a statement that binds nothing gives one
 * SJS_IMPORT_MODULE event.
 */
typedef enum {
    SJS_IMPORT_NAME = 0,   /* alias: one export ("default" for a default import) */
    SJS_IMPORT_NAMESPACE,  /* `* as name`: the namespace object; alias is empty */
    SJS_IMPORT_STAR,       /* `export * from`: every export but default (EXPORT_FROM only) */
    SJS_IMPORT_MODULE      /* binds nothing; the module must exist */
} sjs_import_kind;

typedef enum {
    SJS_OP_ADD = 1, SJS_OP_SUBTRACT, SJS_OP_MULTIPLY, SJS_OP_DIVIDE, SJS_OP_REMAINDER,
    SJS_OP_LESS, SJS_OP_LESS_EQUAL, SJS_OP_GREATER, SJS_OP_GREATER_EQUAL,
    SJS_OP_STRICT_EQUAL, SJS_OP_STRICT_NOT_EQUAL, SJS_OP_IN,
    SJS_OP_AND, SJS_OP_OR, SJS_OP_COALESCE,
    SJS_OP_NOT, SJS_OP_NEGATE, SJS_OP_PLUS, SJS_OP_TYPEOF
} sjs_operator;

typedef struct {
    sjs_event_kind kind;
    uint32_t count;
    double number;
    sjs_str text;   /* valid during the call only */
    /* SJS_EV_IMPORT, SJS_EV_EXPORT_FROM, SJS_EV_EXPORT_NAME: */
    sjs_str alias;  /* the name before `as` */
    sjs_str name;   /* the name after `as`, or the same name */
    sjs_position at;       /* this event's own token */
    /*
     * The syntax node the event belongs to, so that a sink can tell which
     * subtree an event closes without counting events. Every event of one
     * node carries the same `node`: a bracketed node's BEGIN, its separators
     * (ELEMENT, ARGUMENT, KEY, PARAM, PARAMS_END, ...) and its END; an
     * operator's markers (LOGICAL, CONDITIONAL, CONDITIONAL_ELSE) and its last
     * event; an arrow function's parameter list and body (BLOCK_BEGIN,
     * BLOCK_END); every IF, ELSE and the IF_END of an else-if chain; a
     * template's STRING pieces. Every other event is a node of its own.
     * The ids are unique within one parse, made as the parser reads, so they
     * need not increase in the events' order (a const's pattern is read
     * before its value and sent after it). 0 is the program, whose only event
     * is END. A sink that does not need them ignores both fields.
     *
     * `start` is where the node's source begins: its first token, which for
     * an operator, a member, an index, a call or an optional chain is its
     * first operand's. Parentheses around an operand are not part of it.
     * `start` is at or before `at` for every event but CHAIN_END, whose `at`
     * is the bracket the chain ends at and may come first: in `(a?.b).c`
     * CHAIN_END's `at` is the `(` and its `start` is the `a`.
     */
    uint32_t node;
    sjs_position start;
} sjs_event;

typedef struct sjs_sink_vtable {
    /* SJS_OK to go on. Any other result stops the parser, which returns it. */
    sjs_result (*event)(void *self, const sjs_event *event);
} sjs_sink_vtable;

typedef struct { void *self; const sjs_sink_vtable *v; } sjs_sink;

/* ========================================================================
 * Parser
 *
 * Pushed the source in chunks of any size; a token may span chunks. Events
 * reach the sink while the source arrives.
 */

typedef enum {
    /* One expression, read as JavaScript reads `(` source `)`. */
    SJS_GOAL_EXPRESSION = 0,
    /* A module: imports, const declarations and exports, in any order. */
    SJS_GOAL_MODULE
} sjs_goal;

typedef struct sjs_parser sjs_parser;

typedef struct {
    sjs_allocator allocator;
    sjs_limits limits;
    sjs_goal goal;
    sjs_sink sink;
} sjs_parser_config;

SJS_API sjs_result sjs_parser_init(sjs_parser **out, const sjs_parser_config *config);
/* Read a chunk. SJS_ERR_PROGRAM: the source is not staticjs, or the sink
 * refused an event; sjs_parser_error() says which. */
SJS_API sjs_result sjs_parser_push(sjs_parser *parser, sjs_bytes chunk);
/* The source is complete. On success the sink has seen SJS_EV_END. */
SJS_API sjs_result sjs_parser_finish(sjs_parser *parser);
/* The error that stopped the parser. SJS_ERR_NOT_FOUND while there is none. */
SJS_API sjs_result sjs_parser_error(const sjs_parser *parser, sjs_error *out);
SJS_API void sjs_parser_destroy(sjs_parser *parser);

/* ========================================================================
 * Compiler and programs
 */

typedef struct sjs_compiler sjs_compiler;
typedef struct sjs_program sjs_program;

/*
 * Modules a program can import, by name. find() answers a module's exports:
 * an object from evaluating the module's program, borrowed for as long as the
 * compiler runs (the program retains what it imports). A module can only be
 * imported once it has been compiled and evaluated, so imports cannot form a
 * cycle. staticjs gives a module's exports as its namespace: keys sorted by
 * UTF-16 code units; an embedder's object may be in any order.
 */
typedef struct sjs_modules_vtable {
    sjs_result (*find)(void *self, sjs_str name, sjs_value *exports);
} sjs_modules_vtable;

typedef struct { void *self; const sjs_modules_vtable *v; } sjs_modules;

typedef struct {
    sjs_context *context;
    sjs_goal goal;
    /* Names the embedder binds for each evaluation, in sjs_run_params order. */
    const sjs_str *globals;
    uint32_t global_count;
    sjs_modules modules;   /* optional: v may be NULL */
    /*
     * Compile without matching shapes: no function is tagged with the shape
     * of what it computes (a key read, a comparison by a key), so no built-in
     * can run a callback in C instead of its code. A program gives the same
     * values and faults either way, and never uses more fuel with shapes.
     * Matching allocates (its records, from the context's allocator), so a
     * compile at the edge of an allocator's budget can fail only with shapes.
     * For tests that compare the two, and for an embedder ruling the fast
     * paths out. false, which a designated initialiser that leaves it out
     * gives, matches (docs/matcher.md).
     */
    bool no_shapes;
} sjs_compiler_config;

SJS_API sjs_result sjs_compiler_init(sjs_compiler **out, const sjs_compiler_config *config);
/* The sink to give the parser. Valid while the compiler is. */
SJS_API sjs_sink sjs_compiler_sink(sjs_compiler *compiler);
/* The program, once the sink has seen SJS_EV_END. */
SJS_API sjs_result sjs_compiler_finish(sjs_compiler *compiler, sjs_program **out);
SJS_API sjs_result sjs_compiler_error(const sjs_compiler *compiler, sjs_error *out);
SJS_API void sjs_compiler_destroy(sjs_compiler *compiler);

/* Parse and compile a source held in memory: a parser feeding a compiler. */
typedef struct {
    sjs_compiler_config compiler;
    sjs_str source;
    sjs_error *error;      /* optional: filled when the result is SJS_ERR_PROGRAM */
} sjs_compile_params;

SJS_API sjs_result sjs_compile(const sjs_compile_params *params, sjs_program **out);

SJS_API void sjs_program_retain(sjs_program *program);
SJS_API void sjs_program_release(sjs_program *program);

/* ========================================================================
 * Evaluator
 *
 * Runs a program in steps on pushed fuel: one unit per instruction, and per
 * element for the built-ins that loop. A built-in's C path for a callback's
 * shape charges what the callback would have used in one step, so a run can
 * use up to 2 units more than the fuel pushed. A callback of map() or reduce() is a
 * frame on the evaluator's own stack, so an evaluation can stop and resume
 * anywhere, a callback included.
 */

typedef struct sjs_vm sjs_vm;

typedef struct {
    sjs_context *context;
} sjs_vm_config;

typedef struct {
    sjs_program *program;
    /* Values for the compiler's globals, in order; borrowed until the
     * evaluation ends. */
    const sjs_value *globals;
    uint32_t global_count;
} sjs_run_params;

typedef enum {
    SJS_RUN_DONE = 0,  /* the result is ready: sjs_vm_take_result() */
    SJS_RUN_PAUSED,    /* the fuel is spent; push more */
    SJS_RUN_FAULT      /* sjs_vm_error() */
} sjs_run;

SJS_API sjs_result sjs_vm_init(sjs_vm **out, const sjs_vm_config *config);
SJS_API sjs_result sjs_vm_start(sjs_vm *vm, const sjs_run_params *params);
SJS_API sjs_run sjs_vm_run(sjs_vm *vm, uint64_t fuel);
/* The result, which the caller now owns. */
SJS_API sjs_result sjs_vm_take_result(sjs_vm *vm, sjs_value *out);
SJS_API sjs_result sjs_vm_error(const sjs_vm *vm, sjs_error *out);
/* Fuel used by the evaluation so far. */
SJS_API uint64_t sjs_vm_fuel_used(const sjs_vm *vm);
/* Abandon an evaluation and release what it holds. */
SJS_API void sjs_vm_reset(sjs_vm *vm);
SJS_API void sjs_vm_destroy(sjs_vm *vm);

/* ========================================================================
 * Writer
 *
 * Writes a value as JavaScript source that evaluates to an equal value:
 * JSON where JSON can say it, and `undefined`, `NaN`, `Infinity`, `-0` and
 * `new Set([...])` where it cannot. Resumes when the buffer is full.
 */

typedef struct sjs_writer sjs_writer;

typedef struct {
    sjs_context *context;
    sjs_value value;       /* borrowed until the writer is done */
    /* Written for a function. Empty: a function is SJS_ERR_INVALID, as it
     * has no source to write. A log or a test can say "<function>". */
    sjs_str function_text;
} sjs_writer_config;

typedef struct {
    uint8_t *ptr;
    uint32_t capacity;
    uint32_t length;       /* bytes the writer has put in, from 0 */
} sjs_buffer;

SJS_API sjs_result sjs_writer_init(sjs_writer **out, const sjs_writer_config *config);
/* Fill `buffer` from buffer->length on. SJS_OK when the value is written,
 * SJS_NEED_SPACE when the buffer is full, SJS_ERR_INVALID for a function
 * when the config gives no function_text. */
SJS_API sjs_result sjs_writer_write(sjs_writer *writer, sjs_buffer *buffer);
SJS_API void sjs_writer_destroy(sjs_writer *writer);

#ifdef __cplusplus
}
#endif
