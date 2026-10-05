/*
 * What the modules of staticjs share. Private: the public API is
 * ../include/staticjs.h.
 */
#pragma once

#include "staticjs.h"

#define SJS_UNUSED(x) ((void)(x))

/* ========================================================================
 * Memory
 */

void *sjs_mem_alloc(const sjs_allocator *allocator, size_t size);
void sjs_mem_free(const sjs_allocator *allocator, void *ptr, size_t size);
void sjs_copy(void *to, const void *from, size_t length);
void sjs_zero(void *to, size_t length);

static inline bool sjs_str_equal(sjs_str a, sjs_str b) {
    if (a.length != b.length) return false;
    for (uint32_t i = 0; i < a.length; i++) {
        if (a.ptr[i] != b.ptr[i]) return false;
    }
    return true;
}

/*
 * A growable array of bytes. It moves when it grows, so nothing keeps a
 * pointer into one across a call that may append to it.
 */
typedef struct {
    uint8_t *ptr;
    uint32_t length;
    uint32_t capacity;
} sjs_buf;

sjs_result sjs_buf_reserve(const sjs_allocator *allocator, sjs_buf *buf, uint32_t extra);
sjs_result sjs_buf_append(const sjs_allocator *allocator, sjs_buf *buf, sjs_bytes bytes);
void sjs_buf_free(const sjs_allocator *allocator, sjs_buf *buf);

/* A buffer of fixed-size records. */
#define SJS_BUF_AT(buf, type, index) (((type *)(void *)(buf).ptr)[index])
#define SJS_BUF_COUNT(buf, type) ((buf).length / (uint32_t)sizeof(type))

/* A slot of the context's table of interned strings (sjs_string_intern). */
typedef struct {
    struct sjs_string_ *string;   /* NULL: empty */
    uint32_t hash;
} sjs_intern_slot;

struct sjs_context {
    sjs_allocator backing;
    sjs_allocator counting;   /* what everything in the context allocates with */
    sjs_limits limits;
    size_t live;
    /* Interned strings, weakly: a string leaves when it is freed, and the
     * table goes when the last one does. Open-addressed, probed linearly. */
    sjs_intern_slot *interned;
    uint32_t intern_mask;     /* slots - 1 */
    uint32_t intern_count;
};

/* ========================================================================
 * Text
 */

/* The code point at `bytes[*index]`, advancing `*index`; -1 when the bytes
 * there are not well-formed UTF-8. */
int32_t sjs_utf8_next(sjs_str text, uint32_t *index);
bool sjs_utf8_valid(sjs_str text);
/* Bytes of the UTF-8 encoding of `point` written to `out` (room for 4). */
uint32_t sjs_utf8_encode(uint32_t point, uint8_t *out);
/* JavaScript's view: UTF-16 code units. */
uint32_t sjs_utf16_length(sjs_str text);
/* <0, 0, >0 as JavaScript compares the two strings: by UTF-16 code unit. */
int32_t sjs_utf16_compare(sjs_str a, sjs_str b);

/* ========================================================================
 * Numbers
 */

/* Number::toString(10): at most 25 bytes, written to `out`. */
uint32_t sjs_number_format(double value, char *out);
/* JavaScript's % (truncating remainder, exact). */
double sjs_number_remainder(double x, double y);
double sjs_number_trunc(double x);
double sjs_number_floor(double x);
double sjs_number_ceil(double x);
double sjs_number_round(double x);
bool sjs_number_is_nan(double x);
bool sjs_number_is_finite(double x);
bool sjs_number_is_negative_zero(double x);
bool sjs_number_is_integer(double x);
/* The decimal digits `digits` (no sign, no point) times 10^exponent, when
 * that is exact with one rounding (Clinger's fast path). */
bool sjs_number_from_decimal(sjs_str digits, int32_t exponent, double *out);

/* ========================================================================
 * Values
 */

enum {
    SJS_HEAP_STRING = 1,
    SJS_HEAP_ARRAY,
    SJS_HEAP_OBJECT,
    SJS_HEAP_SET,
    SJS_HEAP_CLOSURE,
    SJS_HEAP_NATIVE,
    SJS_HEAP_BUILTIN
};

/* refs of a value that is never freed: built-in functions. */
#define SJS_IMMORTAL UINT32_MAX

struct sjs_heap {
    uint32_t refs;
    uint8_t type;     /* SJS_HEAP_* */
    uint8_t flags;
    uint16_t name_id; /* a string with SJS_STRING_NAMED: its sjs_name_id */
    uint32_t size;    /* bytes allocated */
    sjs_heap *link;   /* the release worklist */
};

/* String flags. */
#define SJS_STRING_ASCII 1u
#define SJS_STRING_INDEX 2u   /* a canonical array index; `index` holds it */
#define SJS_STRING_NAMED 4u   /* `h.name_id` holds sjs_name_id of its text */
#define SJS_STRING_INTERNED 8u /* in its context's table: one string per text */

typedef struct sjs_string_ {
    sjs_heap h;
    uint32_t length;
    uint32_t utf16_length;
    uint32_t index;
    char bytes[];
} sjs_string;

/*
 * A hash index over a set's elements or an object's keys: an open-addressed
 * table of positions (plus one; 0 is empty), probed linearly. A container
 * gets one when an item goes in and it holds SJS_INDEX_FROM items or more,
 * so small ones keep the plain scan. Items only ever go in, into a container
 * one holder updates, so the index never goes stale; a container without one
 * (one copied whole, or one whose index could not be allocated) is scanned.
 */
typedef struct {
    uint32_t mask;      /* slots - 1; slots is a power of two */
    uint32_t slots[];
} sjs_index;

#define SJS_INDEX_FROM 8u

/* Container flags. */
#define SJS_CONTAINER_UNINDEXED 1u   /* its index failed to allocate: scan it */

typedef struct {
    sjs_heap h;
    uint32_t length;
    uint32_t capacity;
    sjs_index *index;   /* a set's, or NULL; an array has none */
    sjs_value items[];
} sjs_array;   /* also a set: elements in insertion order */

typedef struct {
    sjs_string *key;
    sjs_value value;
} sjs_entry;

typedef struct {
    sjs_heap h;
    uint32_t count;
    uint32_t capacity;
    sjs_index *index;   /* or NULL */
    sjs_entry entries[];
} sjs_object;

typedef struct {
    sjs_heap h;
    sjs_program *program;
    uint32_t function;
    uint32_t count;
    sjs_value captures[];
} sjs_closure;

typedef struct {
    sjs_heap h;
    sjs_native native;
} sjs_native_function;

typedef struct {
    sjs_heap h;
    uint16_t id;      /* sjs_builtin_id */
} sjs_builtin_function;

/* `internal` tags of values that live only on the evaluator's stack. */
enum {
    SJS_INTERNAL_NONE = 0,
    SJS_INTERNAL_SPREAD,   /* an argument to splice into a call */
    SJS_INTERNAL_METHOD    /* a built-in method; boolean holds its id */
};

#define SJS_AS_STRING(v) ((sjs_string *)(void *)(v).as.heap)
#define SJS_AS_ARRAY(v) ((sjs_array *)(void *)(v).as.heap)
#define SJS_AS_OBJECT(v) ((sjs_object *)(void *)(v).as.heap)
#define SJS_AS_CLOSURE(v) ((sjs_closure *)(void *)(v).as.heap)

static inline sjs_str sjs_string_view(const sjs_string *s) {
    return (sjs_str){ s->bytes, s->length };
}

static inline sjs_value sjs_heap_value(uint8_t kind, sjs_heap *heap) {
    sjs_value v = { 0 };
    v.kind = kind;
    v.as.heap = heap;
    return v;
}

static inline bool sjs_is_heap(sjs_value v) {
    return v.kind >= SJS_STRING && v.internal == SJS_INTERNAL_NONE;
}

/*
 * Is `v` a heap value with one reference? Every holder that can read a value
 * later counts in its refs: frame and loop slots, the stack, closures'
 * captures, a program's constants, and the embedder, for the globals and
 * modules it lends and the values natives keep. So when an instruction owns
 * the one reference, nothing else can see the value, and it may update the
 * value in place. Values are acyclic, so the update cannot make a cycle: the
 * value put in would need a reference to the container, which would count.
 * Immortal values never pass.
 */
static inline bool sjs_value_unique(sjs_value v) {
    return sjs_is_heap(v) && v.as.heap->refs == 1;
}

/* sjs_number, sjs_retain and sjs_release for the evaluator's hot paths,
 * inline. sjs_unref frees through sjs_release only when it drops the last
 * reference. */
static inline sjs_value sjs_num(double value) {
    sjs_value v = { 0 };
    v.kind = SJS_NUMBER;
    v.as.number = value;
    return v;
}

static inline void sjs_ref(sjs_value v) {
    if (sjs_is_heap(v) && v.as.heap->refs != SJS_IMMORTAL) v.as.heap->refs++;
}

static inline void sjs_unref(sjs_context *context, sjs_value v) {
    if (!sjs_is_heap(v)) return;
    uint32_t refs = v.as.heap->refs;
    if (refs == SJS_IMMORTAL) return;
    if (refs > 1) {
        v.as.heap->refs = refs - 1;
        return;
    }
    sjs_release(context, v);
}

sjs_result sjs_string_make(sjs_context *context, sjs_str text, sjs_string **out);
/* The context's one string with `text`, made and interned if there is none
 * yet; for property keys, so that two keys with the same text are most often
 * the same string. When the table cannot grow, the string is made but not
 * interned: interning only makes compares faster. */
sjs_result sjs_string_intern(sjs_context *context, sjs_str text, sjs_string **out);
/* The string for an array index, a number's text or a boolean's. */
sjs_result sjs_string_from_number(sjs_context *context, double value, sjs_string **out);
sjs_result sjs_array_make(sjs_context *context, uint32_t capacity, sjs_array **out);
sjs_result sjs_object_make(sjs_context *context, uint32_t capacity, sjs_object **out);
/* Grow a container the caller holds alone, which may move it. */
sjs_result sjs_array_grow(sjs_context *context, sjs_array **array, uint32_t extra);
sjs_result sjs_object_grow(sjs_context *context, sjs_object **object, uint32_t extra);
/* Put takes over the references to `key` and `value`. */
typedef struct {
    sjs_string *key;
    sjs_value value;
} sjs_put;
sjs_result sjs_object_put_key(sjs_context *context, sjs_object **object, sjs_put put);
/* The entry of `key`, or NULL. */
const sjs_entry *sjs_object_find(const sjs_object *object, sjs_str key);
/* Add to a set the caller holds alone; takes over the element's reference. */
sjs_result sjs_set_insert(sjs_context *context, sjs_array **set, sjs_value element);
bool sjs_set_contains(const sjs_array *set, sjs_value element);
sjs_result sjs_closure_make(sjs_context *context, uint32_t count, sjs_closure **out);

/* JavaScript's abstract operations. */
bool sjs_truthy(sjs_value v);
bool sjs_is_primitive(sjs_value v);
/* SameValueZero for primitives; false for any heap object. */
bool sjs_same_value_zero(sjs_value a, sjs_value b);
/* Strict equality for primitives. */
bool sjs_strict_equal_primitive(sjs_value a, sjs_value b);
/* typeof, as a static string. */
sjs_str sjs_typeof(sjs_value v);
/* ToString for primitives; SJS_ERR_PROGRAM for objects. */
sjs_result sjs_to_string(sjs_context *context, sjs_value v, sjs_string **out);
/* ToPropertyKey for primitives. */
sjs_result sjs_to_key(sjs_context *context, sjs_value v, sjs_string **out);

/* ========================================================================
 * Built-ins
 */

typedef enum {
    SJS_BUILTIN_NONE = 0,
    /* Functions. */
    SJS_BUILTIN_STRING,
    SJS_BUILTIN_BOOLEAN,
    SJS_BUILTIN_ARRAY_IS_ARRAY,
    SJS_BUILTIN_OBJECT_KEYS,
    SJS_BUILTIN_OBJECT_VALUES,
    SJS_BUILTIN_OBJECT_ENTRIES,
    SJS_BUILTIN_OBJECT_FROM_ENTRIES,
    SJS_BUILTIN_NUMBER_IS_INTEGER,
    SJS_BUILTIN_NUMBER_IS_SAFE_INTEGER,
    SJS_BUILTIN_NUMBER_IS_FINITE,
    SJS_BUILTIN_NUMBER_IS_NAN,
    SJS_BUILTIN_MATH_ABS,
    SJS_BUILTIN_MATH_MIN,
    SJS_BUILTIN_MATH_MAX,
    SJS_BUILTIN_MATH_FLOOR,
    SJS_BUILTIN_MATH_CEIL,
    SJS_BUILTIN_MATH_ROUND,
    SJS_BUILTIN_MATH_TRUNC,
    SJS_BUILTIN_MATH_SIGN,
    SJS_BUILTIN_FUNCTION_COUNT,
    /* Methods, called on a receiver. */
    SJS_METHOD_FIRST = 64,
    /* Arrays. */
    SJS_METHOD_ARRAY_AT = SJS_METHOD_FIRST,
    SJS_METHOD_ARRAY_CONCAT,
    SJS_METHOD_ARRAY_EVERY,
    SJS_METHOD_ARRAY_FILTER,
    SJS_METHOD_ARRAY_FIND,
    SJS_METHOD_ARRAY_FIND_INDEX,
    SJS_METHOD_ARRAY_FIND_LAST,
    SJS_METHOD_ARRAY_FLAT,
    SJS_METHOD_ARRAY_FLAT_MAP,
    SJS_METHOD_ARRAY_INCLUDES,
    SJS_METHOD_ARRAY_INDEX_OF,
    SJS_METHOD_ARRAY_JOIN,
    SJS_METHOD_ARRAY_MAP,
    SJS_METHOD_ARRAY_REDUCE,
    SJS_METHOD_ARRAY_SLICE,
    SJS_METHOD_ARRAY_SOME,
    SJS_METHOD_ARRAY_TO_REVERSED,
    SJS_METHOD_ARRAY_TO_SORTED,
    SJS_METHOD_ARRAY_WITH,
    /* Strings. */
    SJS_METHOD_STRING_ENDS_WITH,
    SJS_METHOD_STRING_INCLUDES,
    SJS_METHOD_STRING_INDEX_OF,
    SJS_METHOD_STRING_SLICE,
    SJS_METHOD_STRING_SPLIT,
    SJS_METHOD_STRING_STARTS_WITH,
    SJS_METHOD_STRING_TO_LOWER_CASE,
    SJS_METHOD_STRING_TO_UPPER_CASE,
    SJS_METHOD_STRING_TRIM,
    /* Sets. */
    SJS_METHOD_SET_DIFFERENCE,
    SJS_METHOD_SET_HAS,
    SJS_METHOD_SET_INTERSECTION,
    SJS_METHOD_SET_IS_SUBSET_OF,
    SJS_METHOD_SET_SYMMETRIC_DIFFERENCE,
    SJS_METHOD_SET_UNION
} sjs_builtin_id;

/* The built-in function `Namespace.name` or bare `name`; NONE if unknown. */
sjs_builtin_id sjs_builtin_lookup(sjs_str space, sjs_str name);

/*
 * A name JavaScript finds on a prototype, or that names a method that would
 * change its receiver: what a property read or a method call must know about
 * a key besides the receiver's own properties. A key constant has its entry
 * found once, at compile time (SJS_STRING_NAMED); any other key is looked up
 * when it is used.
 */
#define SJS_NAME_ON_ARRAY 1u   /* on Array.prototype (or Object.prototype) */
#define SJS_NAME_ON_STRING 2u  /* on String.prototype (or Object.prototype) */
#define SJS_NAME_ON_SET 4u     /* on Set.prototype (or Object.prototype) */
#define SJS_NAME_OBJECT 8u     /* on Object.prototype, which staticjs objects lack */
#define SJS_NAME_MUTATING 16u  /* a method that changes its receiver */

typedef struct {
    sjs_str name;
    uint8_t flags;      /* SJS_NAME_* */
    uint8_t methods[3]; /* the staticjs method on an array, a string, a set; 0 if none */
} sjs_name;

/* The entry of `text` plus one, or 0 when it has none. */
uint16_t sjs_name_id(sjs_str text);
/* Every entry, sorted by bytes, and how many (for names_test). */
const sjs_name *sjs_names(uint32_t *count);
/* The entry of a key, or NULL when it has none. */
const sjs_name *sjs_name_of(const sjs_string *key);
/* The method `name` of a receiver of kind `kind`; NONE if it has none. */
sjs_builtin_id sjs_method_of(const sjs_name *name, uint8_t kind);
/* The immortal function value of a built-in function. */
sjs_value sjs_builtin_value(sjs_builtin_id id);
bool sjs_builtin_loops(sjs_builtin_id id);

/* ========================================================================
 * Programs
 */

typedef enum {
    SJS_OP_UNDEFINED_ = 1,
    SJS_OP_NULL_,
    SJS_OP_TRUE_,
    SJS_OP_FALSE_,
    SJS_OP_NUMBER_,        /* u32 number */
    SJS_OP_CONSTANT,       /* u32 constant */
    SJS_OP_LOCAL,          /* u16 slot */
    SJS_OP_TAKE_LOCAL,     /* u16 slot: push it and leave the slot undefined, at its last read */
    SJS_OP_SET_LOCAL,      /* u16 slot: pop into it */
    SJS_OP_CAPTURE,        /* u16 capture */
    SJS_OP_GLOBAL,         /* u16 global */
    SJS_OP_POP,
    SJS_OP_GET_MEMBER,     /* u32 constant (string) */
    SJS_OP_GET_INDEX,
    SJS_OP_METHOD_MEMBER,  /* u32 constant: [receiver] -> [receiver, callee] */
    SJS_OP_METHOD_INDEX,   /* [receiver, key] -> [receiver, callee] */
    SJS_OP_CALL,           /* u16 argc, u8 flags */
    SJS_OP_NEW_SET,        /* u16 argc, u8 flags */
    SJS_OP_SPREAD,
    SJS_OP_JUMP,           /* u32 target */
    SJS_OP_JUMP_FALSY_KEEP,
    SJS_OP_JUMP_TRUTHY_KEEP,
    SJS_OP_JUMP_DEFINED_KEEP,
    SJS_OP_JUMP_FALSY,
    SJS_OP_JUMP_NULLISH,   /* replaces the value with undefined and jumps */
    SJS_OP_UNARY_,         /* u8 sjs_operator */
    SJS_OP_BINARY_,        /* u8 sjs_operator */
    SJS_OP_CONCAT,         /* u16 count */
    SJS_OP_ARRAY_NEW,
    SJS_OP_ARRAY_PUSH,
    SJS_OP_ARRAY_SPREAD,
    SJS_OP_OBJECT_NEW,
    SJS_OP_OBJECT_PUT,
    SJS_OP_OBJECT_SPREAD,
    SJS_OP_TO_KEY,
    SJS_OP_CLOSURE,        /* u32 function */
    SJS_OP_RETURN,
    SJS_OP_PATTERN_ARRAY,  /* checks the value can be destructured as an array */
    SJS_OP_PATTERN_OBJECT,
    SJS_OP_PATTERN_INDEX,  /* u32 index: [source] -> [source, element] */
    SJS_OP_PATTERN_KEY,    /* u32 constant: [source] -> [source, property] */
    SJS_OP_PATTERN_COMPUTED, /* u16 slot: [source, key] -> [source, property]; the key, as a string, in the slot */
    SJS_OP_PATTERN_REST,   /* u32 start: [source] -> [source, array of the elements from start] */
    SJS_OP_PATTERN_REST_OBJECT, /* u32 constant (array of keys, or UINT32_MAX), u16 n:
                                 * [source, n keys] -> [source, object of the other properties] */
    SJS_OP_DEFAULT,        /* u32 target: not undefined: jump, keeping it; else pop it */
    SJS_OP_DEFAULT_LOCAL   /* u16 slot, u32 target: the slot is not undefined: jump */
} sjs_opcode;

/* SJS_OP_CALL flags. */
#define SJS_CALL_METHOD 1u
#define SJS_CALL_SPREAD 2u

/* sjs_function_info flags. */
#define SJS_FUNCTION_REST 1u  /* the last parameter takes the rest of the arguments, as an array */

typedef struct {
    uint32_t code;          /* offset of the first instruction */
    uint32_t captures;      /* first entry in the capture table */
    uint16_t params;        /* a rest parameter included */
    uint16_t locals;        /* slots, the parameters included */
    uint16_t capture_count;
    uint16_t flags;         /* SJS_FUNCTION_* */
} sjs_function_info;

typedef struct {
    uint16_t from_capture;  /* 0: the creator's local slot; 1: its capture */
    uint16_t index;
} sjs_capture_info;

typedef struct {
    uint32_t pc;
    sjs_position at;
} sjs_line;

struct sjs_program {
    uint32_t refs;
    sjs_goal goal;
    sjs_context *context;
    size_t size;
    const uint8_t *code;
    uint32_t code_length;
    const double *numbers;
    uint32_t number_count;
    sjs_value *constants;   /* strings, and values imported */
    uint32_t constant_count;
    const sjs_function_info *functions;
    uint32_t function_count;
    const sjs_capture_info *captures;
    uint32_t capture_count;
    const sjs_line *lines;
    uint32_t line_count;
    uint32_t global_count;
};

/* Where the instruction at `pc` came from. */
sjs_position sjs_program_position(const sjs_program *program, uint32_t pc);

static inline uint16_t sjs_read16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t sjs_read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ========================================================================
 * Errors
 */

/* Room for an error's subject in the stage that reports it. */
#define SJS_SUBJECT_CAPACITY 64u

typedef struct {
    sjs_error error;
    char subject[SJS_SUBJECT_CAPACITY];
} sjs_error_slot;

/* What went wrong, before it is known where. */
typedef struct {
    sjs_error_code code;
    const char *message;
} sjs_problem;

void sjs_error_set(sjs_error_slot *slot, sjs_position at, sjs_problem problem);
void sjs_error_subject(sjs_error_slot *slot, sjs_str subject);
sjs_str sjs_cstr(const char *text);
