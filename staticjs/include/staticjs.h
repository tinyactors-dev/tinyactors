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
#define SJS_VERSION_MINOR 9
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
     * expression's lookahead, a name that refers to itself. */
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
    uint32_t max_value_items;   /* elements of one array, object or set; a map's keys and values
                                 * count two for each entry */
    uint32_t max_string_bytes;  /* bytes of one string value */
    uint32_t max_bigint_bits;   /* bits of one BigInt's magnitude: 2^max_bigint_bits - 1 is the
                                 * largest; an operation whose result is larger faults */
    uint32_t max_regex_states;  /* what one regular expression's automaton may hold at once:
                                 * its instructions, times 2 to the depth of its loops that can
                                 * match empty, times the slots of each thread; a literal past
                                 * it is refused when compiled */
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
        .max_string_bytes = 16u * 1024u * 1024u, \
        .max_bigint_bits = 256u * 1024u,       \
        .max_regex_states = 1024u * 1024u })

#if defined(__wasm__)
/*
 * The WebAssembly module's own allocator (src/allocator_wasm.c, linked into
 * staticjs.wasm only): blocks from the module's linear memory, so a host that
 * loads the module, which imports nothing, supplies no memory management of
 * its own. A host passes sjs_wasm_allocator() as a context's allocator, places
 * the bytes it hands the library (a source, a config) with sjs_wasm_alloc(),
 * and gives a block back with sjs_wasm_free() and the size and alignment it
 * asked for. Alignments above 65536 are refused (NULL).
 */
SJS_API sjs_allocator sjs_wasm_allocator(void);
SJS_API void *sjs_wasm_alloc(size_t size, size_t alignment);
SJS_API void sjs_wasm_free(void *ptr, size_t size, size_t alignment);
#endif

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
    SJS_BIGINT,
    SJS_ARRAY,
    SJS_OBJECT,
    SJS_SET,
    SJS_MAP,
    SJS_ITERATOR,   /* an array's, a map's or a set's keys(), values() or entries(), or an iterator's map() or filter() */
    SJS_FUNCTION,
    SJS_REGEXP,     /* a regular expression literal: an object, immutable; its lastIndex is 0 */
    SJS_TEMPORAL    /* a Temporal value (a Temporal.PlainDate, ...): an object, immutable;
                     * the writer writes it as Temporal.PlainDate.from("2024-01-31"); or
                     * a Date (docs/date.md), written as new Date("2024-01-31T00:00:00.000Z") */
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
 * the item. An array or an object a program froze or sealed (Object.freeze,
 * Object.seal) is not added to: SJS_ERR_INVALID.
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

/* Elements of an array, properties of an object, elements of a set, entries
 * of a map. */
SJS_API uint32_t sjs_length(sjs_value value);
/* Borrowed: valid while the container is. */
SJS_API sjs_value sjs_array_at(sjs_value array, uint32_t index);
SJS_API sjs_value sjs_set_at(sjs_value set, uint32_t index);
SJS_API sjs_result sjs_object_at(sjs_value object, uint32_t index, sjs_property *out);
SJS_API sjs_result sjs_object_get(sjs_value object, sjs_str key, sjs_value *out);

/*
 * Maps: each key once, with its value, in insertion order. A key is any
 * primitive, compared as SameValueZero (NaN is NaN, -0 is 0, a BigInt by its
 * value); staticjs has no identity, so an object is never a key. A new map is
 * empty, with room for `capacity` entries, and can be set while the caller
 * holds its only reference.
 */
typedef struct {
    sjs_value key;
    sjs_value value;
} sjs_map_entry;

SJS_API sjs_result sjs_map_new(sjs_context *context, uint32_t capacity, sjs_value *out);
/* Set as Map.prototype.set would: a key the map has keeps its place and
 * takes the value; a new one goes last. Takes over the caller's references to
 * the key and the value; on an error (SJS_ERR_INVALID for a key that is not a
 * primitive) the caller keeps them. */
SJS_API sjs_result sjs_map_set(sjs_context *context, sjs_value *map, const sjs_map_entry *entry);
/* Entries of a map; 0 for any other value. */
SJS_API uint32_t sjs_map_size(sjs_value map);
/* The value of `key`, borrowed; SJS_ERR_NOT_FOUND when the map has none. */
SJS_API sjs_result sjs_map_get(sjs_value map, sjs_value key, sjs_value *out);
/* The entry at `index`, in insertion order, borrowed. */
SJS_API sjs_result sjs_map_at(sjs_value map, uint32_t index, sjs_map_entry *out);

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
 *
 * MUTATION: an assignment is a statement of its own, its target (a name, a
 * member or an index) first, then the value; ASSIGN's and ASSIGN_END's
 * count is the operator (an sjs_operator: ADD for `+=`, AND for `&&=`, ...),
 * 0 for `=`. ++ and -- are statements of their own, prefix or postfix,
 * UPDATE's count ADD for ++ and SUBTRACT for --; delete is a statement of
 * its own, its target a member or an index. `let` is CONST_BEGIN with
 * count 1, or 2 for a name without a value, which is undefined:
 *
 *   t op= v;                  <t> ASSIGN(op) <v> ASSIGN_END(op)
 *   t++;  ++t;                <t> UPDATE(ADD)
 *   delete t;                 <t> DELETE
 *   let p = v                 CONST_BEGIN(1) <v> <p> CONST_END
 *   let x                     CONST_BEGIN(2) BIND(x) CONST_END
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
    SJS_EV_ASSIGN,           /* MUTATION: after an assignment's target; count: its operator, 0 for = */
    SJS_EV_ASSIGN_END,       /* MUTATION: after its value; count: as ASSIGN's */
    SJS_EV_UPDATE,           /* MUTATION: after the target of ++ (count ADD) or -- (SUBTRACT) */
    SJS_EV_DELETE,           /* MUTATION: after the target of delete, a member or an index */
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
    SJS_EV_CONST_BEGIN,      /* MUTATION: count 1 for let, 2 for let without a value; 0 for const */
    SJS_EV_CONST_END,
    SJS_EV_EXPORT,           /* before CONST_BEGIN: its names are exported */
    SJS_EV_IMPORT,           /* text: the module; count: an sjs_import_kind */
    SJS_EV_EXPORT_DEFAULT,   /* after the value of `export default` */
    SJS_EV_EXPORT_NAME,      /* `export { alias as name }`; count: 1 when name was a string */
    SJS_EV_EXPORT_FROM,      /* a re-export; text: the module; count: an sjs_import_kind */
    SJS_EV_BIGINT,           /* a BigInt literal; text: its digits, without a prefix, a
                              * separator or the `n`; count: their radix, 2, 8, 10 or 16 */
    SJS_EV_REGEXP,           /* a regular expression literal; text: its body, between the
                              * slashes, as written; count: its flags, a set of bits in the
                              * order "dgimsuvy" (d is 1, y is 128) */
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
    SJS_OP_NOT, SJS_OP_NEGATE, SJS_OP_PLUS, SJS_OP_TYPEOF,
    SJS_OP_EXPONENT   /* ** */
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
    /* Run every regular expression match as a loop frame, which pauses
     * within it, never as one call when the match is short. A program gives
     * the same values, faults and fuel either way. For tests that compare
     * the two; false, which a designated initialiser gives, uses both. */
    bool no_regex_leaf;
    /* Run every match by the automaton itself, never from the cache of its
     * steps a long test() keeps (docs/regex.md, 12.3). A program gives the
     * same values, faults, fuel and pauses either way. For tests that
     * compare the two; false uses the cache where it applies. */
    bool no_regex_dfa;
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

/*
 * The host clock (docs/temporal.md, "Host clock"): the one source of the
 * current time a program reads (Temporal.Now, Date.now(), new Date()). It is a value the evaluator
 * holds and the host pushes: set it, and advance it, between runs. The
 * library never reads a clock of its own, and nothing calls back for one.
 * A program sees the value set when it reads it; two reads with no call
 * here between them agree, as a run calls nothing here. The value stays
 * across sjs_vm_start, sjs_vm_reset and pauses until set again. A program
 * that reads the clock of an evaluator whose clock was never set faults.
 *
 * An instant is whole seconds since 1970-01-01T00:00Z, rounded toward
 * negative infinity, and the nanoseconds past them: the 1 ns before the
 * epoch is { -1, 999999999 }. It reaches Temporal.Instant's limits, 8.64 x
 * 10^21 ns either way (an int64 of nanoseconds reaches only 292 years).
 */
typedef struct {
    int64_t seconds;
    uint32_t nanoseconds;  /* 0 to 999,999,999 */
} sjs_epoch;

/* SJS_ERR_INVALID when the nanoseconds are past 999,999,999 or the instant
 * is past Temporal's limits; the clock is unchanged then. */
SJS_API sjs_result sjs_vm_set_clock(sjs_vm *vm, const sjs_epoch *now);
/* Move a set clock forward. SJS_ERR_INVALID, the clock unchanged, when it
 * was never set or would pass Temporal's limits. */
SJS_API sjs_result sjs_vm_advance_clock(sjs_vm *vm, uint64_t nanoseconds);
/* The clock's value, for a host that keeps it (a paused evaluation saved
 * and resumed elsewhere); SJS_ERR_NOT_FOUND when it was never set. */
SJS_API sjs_result sjs_vm_clock(const sjs_vm *vm, sjs_epoch *out);

/*
 * The host seed (docs/temporal.md, "Host seed"): what Math.random draws
 * from, pushed as the clock is. The evaluator holds V8's generator
 * (xorshift128+, its state hashed from the seed by MurmurHash3's
 * finalizer, its doubles drawn from a cache of 64 from the last): a seed
 * gives the values `node --random-seed=<seed>` gives, for every seed that
 * flag takes (a nonzero int32; 0 there means "the machine's entropy").
 * Each draw advances the state; nothing else reads or changes it. It stays
 * across sjs_vm_start, sjs_vm_reset and pauses until set again, so a
 * second evaluation continues the first one's sequence; a host that wants
 * each evaluation to draw the same values sets the seed before each
 * sjs_vm_start. A program that calls Math.random on an evaluator whose seed
 * was never set faults. No call allocates.
 */
SJS_API sjs_result sjs_vm_set_seed(sjs_vm *vm, int64_t seed);

/* The generator's state between draws, for a host that saves a paused
 * evaluation and resumes it elsewhere: V8's xorshift128+ state as of its
 * last refill of the cache (or as seeded, before the first), and how many
 * of the cache's 64 values are left to draw. */
typedef struct {
    uint64_t state[2];
    uint32_t left;  /* 0 to 64 */
} sjs_random;

/* The generator's state; SJS_ERR_NOT_FOUND when the seed was never set. */
SJS_API sjs_result sjs_vm_random(const sjs_vm *vm, sjs_random *out);
/* Restore a state sjs_vm_random gave. SJS_ERR_INVALID, the generator
 * unchanged, when the state is all zero (no seed gives it) or more than 64
 * are left. */
SJS_API sjs_result sjs_vm_set_random(sjs_vm *vm, const sjs_random *state);

/* ========================================================================
 * Writer
 *
 * Writes a value as JavaScript source that evaluates to an equal value:
 * JSON where JSON can say it, and `undefined`, `NaN`, `Infinity`, `-0`, a
 * BigInt's `10n`, `new Set([...])`, `new Map([[key, value], ...])`, an
 * iterator's `[...].values()`, an iterator that yields the same values, and
 * a Temporal value's `Temporal.PlainDate.from("2024-01-31")`, a Date's
 * `new Date("2024-01-31T00:00:00.000Z")` (or `new Date(NaN)`), where it
 * cannot. Resumes when the buffer
 * is full; a BigInt's digits are made first, through the context.
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

/* ========================================================================
 * BigInts
 *
 * A BigInt is a heap value like a string: immutable, reference counted, its
 * magnitude at most max_bigint_bits bits. The writer writes one as `10n`.
 */

/* A new BigInt from text as BigInt(text) reads it: white space around
 * decimal digits with an optional sign, or 0x, 0o or 0b and digits; empty
 * text is 0n. SJS_ERR_INVALID for any other text, SJS_ERR_LIMIT for one
 * too large. */
SJS_API sjs_result sjs_bigint_new(sjs_context *context, sjs_str text, sjs_value *out);
/* Bytes the decimal text of a BigInt takes at most, its sign included. */
SJS_API uint32_t sjs_bigint_text_bound(sjs_value bigint);
/* The decimal text of a BigInt, as String(x) gives it ("-12", no `n`),
 * written into `buffer` from buffer->length on. SJS_NEED_SPACE, writing
 * nothing, when it does not fit; sjs_bigint_text_bound() bytes always do.
 * Converting allocates scratch from the context. */
SJS_API sjs_result sjs_bigint_text(sjs_context *context, sjs_value bigint, sjs_buffer *buffer);

#ifdef __cplusplus
}
#endif
