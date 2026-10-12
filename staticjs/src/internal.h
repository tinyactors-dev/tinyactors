/*
 * What the modules of staticjs share. Private: the public API is
 * ../include/staticjs.h.
 */
#pragma once

#include "staticjs.h"
#include "shapes.h"

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

/* Buckets of the context's table of interned strings (sjs_string_intern). */
#define SJS_INTERN_BUCKETS 256u
/* Interning stops for a text whose bucket holds this many already. */
#define SJS_INTERN_CHAIN_MAX 8u

struct sjs_context {
    sjs_allocator backing;
    sjs_allocator counting;   /* what everything in the context allocates with */
    sjs_limits limits;
    size_t live;
    /* Interned strings, weakly: a string leaves when it is freed. Each
     * bucket is a chain through the strings' h.link, which a string in use
     * does not need; a fixed array, so that the table never allocates. */
    struct sjs_string_ *interned[SJS_INTERN_BUCKETS];
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
/* WhiteSpace and LineTerminator: what trim removes and Number skips. */
static inline bool sjs_is_white_space(uint32_t p) {
    return p == 0x09u || p == 0x0Au || p == 0x0Bu || p == 0x0Cu || p == 0x0Du || p == 0x20u || p == 0xA0u ||
           p == 0x1680u || (p >= 0x2000u && p <= 0x200Au) || p == 0x2028u || p == 0x2029u || p == 0x202Fu ||
           p == 0x205Fu || p == 0x3000u || p == 0xFEFFu;
}

/* ========================================================================
 * Numbers
 */

/* Number::toString(10): at most 25 bytes, written to `out`. */
uint32_t sjs_number_format(double value, char *out);
/* Number.prototype.toFixed, toExponential and toPrecision of `value`, with
 * `digits` already in range (0 to 100; 1 to 100 for toPrecision), or -1 for
 * an undefined argument of toExponential or toPrecision: the decimal value
 * of the double exactly, rounded half up. At most SJS_NUMBER_DIGITS_MAX
 * bytes, written to `out`. */
#define SJS_NUMBER_DIGITS_MAX 128u
typedef enum {
    SJS_DIGITS_FIXED,
    SJS_DIGITS_EXPONENTIAL,
    SJS_DIGITS_PRECISION
} sjs_digits_mode;
typedef struct {
    double value;
    int32_t digits;
    sjs_digits_mode mode;
} sjs_number_digits;
uint32_t sjs_number_format_digits(sjs_number_digits d, char *out);
/* Number.prototype.toString(radix) of `value`, radix 2 to 36: String(value)
 * for 10, else the digits V8 writes (DoubleToRadixCString). `out` has
 * SJS_NUMBER_RADIX_MAX bytes, of which the text takes at most 2,100 (1,024
 * integer digits in radix 2, 1,074 of a fraction, a sign and a point). */
#define SJS_NUMBER_RADIX_MAX 2200u
uint32_t sjs_number_format_radix(double value, uint32_t radix, char *out);
/* A double's bits, and the double of bits (number.c, math.c). */
typedef union {
    double d;
    uint64_t u;
} sjs_bits64;
static inline uint64_t sjs_double_bits(double x) {
    sjs_bits64 b;
    b.d = x;
    return b.u;
}
static inline double sjs_bits_double(uint64_t u) {
    sjs_bits64 b;
    b.u = u;
    return b.d;
}
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
/* The decimal digits `digits` (no sign; a '.' among them is skipped) times
 * 10^exponent, rounded to the nearest double, ties to even: exact for any
 * number of digits. */
double sjs_number_from_decimal(sjs_str digits, int64_t exponent);
/* Digits of `radix` (2, 4, 8, 16 or 32; each a valid digit), rounded the
 * same way. */
double sjs_number_from_binary(sjs_str digits, uint32_t radix);
/* StringToNumber: Number(s). */
double sjs_number_parse(sjs_str s);
/* parseFloat(s), and parseInt(s, radix) with the radix already ToInt32. */
double sjs_number_parse_float(sjs_str s);
double sjs_number_parse_int(sjs_str s, int32_t radix);
/* ToUint32 (ToInt32 is the same bits, signed). */
uint32_t sjs_number_to_uint32(double x);
/* Correctly rounded square root; the nearest float32, widened. */
double sjs_number_sqrt(double x);
double sjs_number_fround(double x);
/* Math functions as V8 computes them (math.c, V8's fdlibm): x ** y and
 * Math.pow(x, y), Math.exp, expm1, log, log1p, log2 and log10, Math.sin,
 * cos, tan, asin, acos, atan and atan2, Math.sinh, cosh, tanh, asinh,
 * acosh, atanh and cbrt. */
double sjs_math_pow(double x, double y);
double sjs_math_exp(double x);
double sjs_math_expm1(double x);
double sjs_math_log(double x);
double sjs_math_log1p(double x);
double sjs_math_log2(double x);
double sjs_math_log10(double x);
double sjs_math_sin(double x);
double sjs_math_cos(double x);
double sjs_math_tan(double x);
double sjs_math_asin(double x);
double sjs_math_acos(double x);
double sjs_math_atan(double x);
double sjs_math_atan2(double y, double x);
double sjs_math_sinh(double x);
double sjs_math_cosh(double x);
double sjs_math_tanh(double x);
double sjs_math_asinh(double x);
double sjs_math_acosh(double x);
double sjs_math_atanh(double x);
double sjs_math_cbrt(double x);
/* Math.random: V8's generator (number.c), the public sjs_random and the
 * state whose first word gives the next value, which is sjs_random's
 * state stepped back 64 - left times. A state of zeros was never seeded. */
typedef struct {
    sjs_random v8;
    uint64_t cursor[2];
} sjs_random_generator;
void sjs_random_seed(sjs_random_generator *g, int64_t seed);
/* False, and the generator unchanged, for a state no seed gives. */
bool sjs_random_restore(sjs_random_generator *g, const sjs_random *state);
/* The next value of a seeded generator, in [0, 1). */
double sjs_random_next(sjs_random_generator *g);
/* A positive number q x 2^e2, q's top bit set, `sticky` when it is a little
 * more: the nearest double, ties to even (Number of a BigInt). */
typedef struct {
    uint64_t q;
    int32_t e2;
    bool sticky;
} sjs_number_top;
double sjs_number_from_top(sjs_number_top v);
/* Math.hypot, as V8 computes it: from a zeroed sjs_hypot, see every
 * argument; if that settles the result, it is that; otherwise add every
 * argument, and end. */
typedef struct {
    double max;
    double sum;
    double compensation;
    bool nan;
} sjs_hypot;
void sjs_hypot_see(sjs_hypot *h, double x);
bool sjs_hypot_settled(const sjs_hypot *h, double *out);
void sjs_hypot_add(sjs_hypot *h, double x);
double sjs_hypot_end(const sjs_hypot *h);

/* JSON's escape of `"`, `\\` or a control character below U+0020 (writer.c),
 * written to `out` (room for 6): its length. */
uint32_t sjs_json_escape(uint8_t c, char *out);

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
    SJS_HEAP_BUILTIN,
    SJS_HEAP_BIGINT,
    SJS_HEAP_MAP,
    SJS_HEAP_ITERATOR,
    SJS_HEAP_REGEXP,   /* a regular expression: one block holding no references (regex_program.h) */
    SJS_HEAP_TEMPORAL  /* a Temporal value (sjs_temporal): one block holding no references */
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
 * so small ones keep the plain scan. Items go in, into a container one
 * holder updates, so the index never goes stale; a container without one
 * (one copied whole, or one whose index could not be allocated) is scanned.
 * MUTATION: items also go out (sjs_object_remove, sjs_set_remove), which
 * keeps the index current and leaves no deletion marker in it.
 */
typedef struct {
    uint32_t mask;      /* slots - 1; slots is a power of two */
    uint32_t slots[];
} sjs_index;

#define SJS_INDEX_FROM 8u

/* Container flags. */
#define SJS_CONTAINER_UNINDEXED 1u   /* its index failed to allocate: scan it */

/* An array flag: `extra` holds the object of its properties besides its
 * elements and length: a match result's index, input, groups and indices
 * (docs/regex.md, 7.2). */
#define SJS_ARRAY_EXTRA 2u

/* An object's or an array's integrity (Object.seal, Object.freeze): sealed,
 * it takes no new keys or elements and loses none; frozen, it is sealed and
 * no key or element changes either, so freezing sets both. A value that
 * holds one is never taken over as another value's start (a spread, concat,
 * with, toSpliced, toSorted), which JavaScript makes a new, extensible
 * object of; the owned mutations of docs/mutation.md fault where JavaScript
 * throws a TypeError ("Frozen and sealed values"). */
#define SJS_INTEGRITY_SEALED 4u
#define SJS_INTEGRITY_FROZEN 8u
#define SJS_INTEGRITY (SJS_INTEGRITY_SEALED | SJS_INTEGRITY_FROZEN)

typedef struct {
    sjs_heap h;
    uint32_t length;
    uint32_t capacity;
    union {
        sjs_index *index;   /* a set's or a map's, or NULL; an array has none */
        sjs_heap *extra;    /* an array with SJS_ARRAY_EXTRA: an object (SJS_HEAP_OBJECT) */
    };
    sjs_value items[];
} sjs_array;

/* Does the array have properties besides its elements (SJS_ARRAY_EXTRA)? */
static inline bool sjs_array_extra(sjs_value v) {
    return v.kind == SJS_ARRAY && (v.as.heap->flags & SJS_ARRAY_EXTRA) != 0;
}   /* also a set: elements in insertion order; and a map (SJS_HEAP_MAP):
                * its keys and their values alternate, items[2i] and items[2i + 1],
                * in insertion order, and length and capacity count both; its
                * index is over the keys, by entry */

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
    SJS_INTERNAL_METHOD,   /* a built-in method; boolean holds its id */
    /* MUTATION: a reference to a variable's slot, the receiver of a method
     * that changes it (SJS_OP_RECEIVER); boolean holds the slot's index in
     * the stack. Its kind is undefined, so releasing it releases nothing. */
    SJS_INTERNAL_SLOT
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
 * the same string. When its bucket is full, the string is made but not
 * interned: interning only makes compares faster. */
sjs_result sjs_string_intern(sjs_context *context, sjs_str text, sjs_string **out);
/* The string for an array index, a number's text or a boolean's. */
sjs_result sjs_string_from_number(sjs_context *context, double value, sjs_string **out);
/* STRING_APPEND: room for `extra` more bytes in a string the caller holds
 * alone, not interned, which may move it; then `tail` appended in place. */
sjs_result sjs_string_reserve(sjs_context *context, sjs_string **string, uint32_t extra);
void sjs_string_extend(sjs_string *s, sjs_str tail);
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
/* An object literal whose keys are known when it compiles: distinct, none
 * an array index, so they go in in order. Takes over the values'
 * references when it succeeds. */
typedef struct {
    const sjs_array *keys;  /* NULL for none */
    const sjs_value *values;
    uint32_t capacity;      /* at least the keys' count */
} sjs_literal;
sjs_result sjs_object_literal(sjs_context *context, sjs_literal literal, sjs_value *out);
/* The entry of `key`, or NULL. */
const sjs_entry *sjs_object_find(const sjs_object *object, sjs_str key);
/* Add to a set the caller holds alone; takes over the element's reference. */
sjs_result sjs_set_insert(sjs_context *context, sjs_array **set, sjs_value element);
bool sjs_set_contains(const sjs_array *set, sjs_value element);
/*
 * An iterator (SJS_ITERATOR): an array's, a map's or a set's keys(),
 * values() or entries(), which yields from the array, the map or the set (an
 * array's keys are its indices and its entries [index, element]; a set's
 * keys are its values, and its entries [value, value]), or what an
 * iterator's map(), filter(), flatMap(), take() or drop() gave, which yields
 * `items`. It holds the array, the map or the set either way, so it cannot
 * change while the iterator lives, as a JavaScript iterator would see the
 * change. Immutable: a program reads one only where nothing else holds it
 * (sjs_iterator_items), so no program can read one twice.
 */
#define SJS_ITERATE_KEYS 0u
#define SJS_ITERATE_VALUES 1u
#define SJS_ITERATE_ENTRIES 2u
/* MUTATION: with SJS_ITERATE_VALUES, an iterator helper's result (take,
 * drop, map, filter, flatMap), which has return() as JavaScript's does. */
#define SJS_ITERATE_HELPER 4u

typedef struct {
    sjs_heap h;           /* h.flags: SJS_ITERATE_*, for an array's, a map's or a set's own */
    sjs_value source;     /* the array, the map or the set */
    sjs_value items;      /* an array of what it yields, or undefined for an array's, a map's or a set's own */
    uint32_t position;    /* MUTATION: how many values next() has taken (docs/mutation.md) */
} sjs_iterator;

#define SJS_AS_ITERATOR(v) ((sjs_iterator *)(void *)(v).as.heap)

/* An iterator over `source`, an array, a map or a set, taking over the
 * references to it and to `items` (undefined for its own, which yields as
 * `mode` says). */
typedef struct {
    sjs_value source;
    sjs_value items;
    uint8_t mode;
} sjs_iterator_spec;
sjs_result sjs_iterator_make(sjs_context *context, sjs_iterator_spec spec, sjs_value *out);
/* How many values an iterator yields (MUTATION: after its position). */
uint32_t sjs_iterator_count(const sjs_iterator *it);

/* A map with room for `entries` entries. */
sjs_result sjs_map_make(sjs_context *context, uint32_t entries, sjs_array **out);
/* The value of `key` in the map, or NULL: through the index, under
 * SameValueZero, so an object, which is never a key, finds nothing. */
const sjs_value *sjs_map_find(const sjs_array *map, sjs_value key);
/* Set in a map the caller holds alone, as Map.prototype.set does; takes over
 * the entry's references, but for SJS_ERR_PROGRAM (a key that is not a
 * primitive) and the errors of growing. */
sjs_result sjs_map_put(sjs_context *context, sjs_array **map, const sjs_map_entry *entry);
/* A copy of a map, its index rebuilt. */
sjs_result sjs_map_copy(sjs_context *context, const sjs_array *map, sjs_array **out);
/* A copy of an object, its index rebuilt. */
sjs_result sjs_object_copy(sjs_context *context, const sjs_object *object, sjs_object **out);
/* MUTATION: removal from a set or an object the caller holds alone, which
 * may move as it shrinks (value.c, "removal"): whether the item was there,
 * and the fuel the removal costs. */
typedef struct {
    bool removed;
    uint64_t cost;
} sjs_removal;
sjs_removal sjs_object_remove(sjs_context *context, sjs_object **object, sjs_str key);
sjs_removal sjs_set_remove(sjs_context *context, sjs_array **set, sjs_value element);
/* A set's or a map's items all released, and its capacity shrunk. */
void sjs_set_clear(sjs_context *context, sjs_array **set);
/* MUTATION: Map.prototype.delete: a map's key and its value. */
sjs_removal sjs_map_remove(sjs_context *context, sjs_array **map, sjs_value key);
/* MUTATION: an array or a set the caller holds alone, reallocated smaller
 * when its items fill under a quarter of its capacity. */
void sjs_array_shrink(sjs_context *context, sjs_array **array);
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
 * BigInts (bigint.c)
 */

#define SJS_BIGINT_NEGATIVE 1u   /* h.flags of a BigInt below 0n */

/* An integer of any size up to max_bigint_bits: a sign and a magnitude in
 * 32-bit limbs, least significant first. Immutable once shared, as every
 * value is; nothing else in it points anywhere, so releasing it frees it. */
typedef struct {
    sjs_heap h;
    uint32_t length;    /* limbs in use; the top one is not 0; 0 for 0n, which is never negative */
    uint32_t capacity;  /* limbs allocated */
    uint32_t limbs[];
} sjs_bigint;

#define SJS_AS_BIGINT(v) ((sjs_bigint *)(void *)(v).as.heap)

static inline bool sjs_bigint_negative(const sjs_bigint *b) {
    return (b->h.flags & SJS_BIGINT_NEGATIVE) != 0;
}

/* Digits of a BigInt in a radix (2, 8, 10 or 16), each a digit of it,
 * leading zeros allowed, and a sign. */
typedef struct {
    sjs_str digits;
    uint32_t radix;
    bool negative;
} sjs_bigint_digits;

/* StringToBigInt's syntax: white space around decimal digits with an
 * optional sign, or 0x, 0o or 0b and digits; empty or white space alone is
 * 0n. False for any other text (a SyntaxError in JavaScript). */
bool sjs_bigint_parse(sjs_str text, sjs_bigint_digits *out);
/* The BigInt of digits; SJS_ERR_LIMIT past max_bigint_bits. */
sjs_result sjs_bigint_from_digits(sjs_context *context, sjs_bigint_digits digits, sjs_value *out);
/* A BigInt's text in a radix from 2 to 36, as toString(radix) gives it. */
typedef struct {
    const sjs_bigint *value;
    uint32_t radix;
} sjs_bigint_radix;
/* Append the text to `out`, allocating from `allocator`. */
sjs_result sjs_bigint_format(const sjs_allocator *allocator, sjs_bigint_radix r, sjs_buf *out);
sjs_result sjs_bigint_to_string(sjs_context *context, sjs_bigint_radix r, sjs_string **out);
/* Fuel for reading or writing the text of a BigInt of `limbs` limbs in a
 * radix: linear in a power of two, quadratic in any other. */
uint64_t sjs_bigint_text_cost(uint64_t limbs, uint32_t radix);
bool sjs_bigint_equal(const sjs_bigint *a, const sjs_bigint *b);
/* An operator on BigInts: + - * / % ** of `a` and `b`, or unary - of `a`
 * (SJS_OP_NEGATE); both borrowed. SJS_OK with `result` owned and `cost` the
 * fuel of its limb work; SJS_ERR_PROGRAM with `fault` where JavaScript
 * throws a RangeError (division by 0n, a negative exponent);
 * SJS_ERR_LIMIT past max_bigint_bits; SJS_ERR_NOMEM. */
#define SJS_BIGINT_REUSE_A 1u
#define SJS_BIGINT_REUSE_B 2u
typedef struct {
    uint8_t op;           /* sjs_operator */
    /* BIGINT_REUSE: the operands the caller holds alone and gives up if the
     * result takes one over (SJS_BIGINT_REUSE_*), and the one it took, which
     * is the result: the caller's reference to it is now the result's. */
    uint8_t reuse;
    uint8_t took;
    bool negative;        /* the result's sign, while it is made */
    sjs_value a;
    sjs_value b;
    sjs_value result;
    uint64_t cost;
    const char *fault;
} sjs_bigint_op;
sjs_result sjs_bigint_binary(sjs_context *context, sjs_bigint_op *op);
/* <0, 0 or >0 as a is less than, equal to or greater than b; with a number,
 * exactly (1n < 1.5), and SJS_BIGINT_UNORDERED for NaN. */
#define SJS_BIGINT_UNORDERED 2
int sjs_bigint_compare(const sjs_bigint *a, const sjs_bigint *b);
int sjs_bigint_compare_number(const sjs_bigint *a, double d);
/* BigInt(d) of an integer d (finite, no fraction), exactly. */
sjs_result sjs_bigint_from_number(sjs_context *context, double d, sjs_value *out);
/* Number(b), rounded to the nearest double, ties to even. */
double sjs_bigint_to_number(const sjs_bigint *b);
/* BigInt.asIntN(bits, value) (is_signed) and BigInt.asUintN(bits, value):
 * value modulo 2^bits, as a two's complement number of `bits` bits when
 * signed. A value that is its own result is shared. */
typedef struct {
    sjs_value value;
    uint64_t bits;      /* at most 2^53 - 1 */
    bool is_signed;
} sjs_bigint_wrap_params;
sjs_result sjs_bigint_wrap(sjs_context *context, sjs_bigint_wrap_params w, sjs_value *out);
uint32_t sjs_bigint_hash(const sjs_bigint *b);

/* ========================================================================
 * Temporal values (temporal.c, docs/temporal.md)
 */

/* The type of a Temporal value, in its h.flags. */
typedef enum {
    SJS_TEMPORAL_PLAIN_DATE = 1,
    SJS_TEMPORAL_PLAIN_TIME,
    SJS_TEMPORAL_PLAIN_DATE_TIME,
    SJS_TEMPORAL_INSTANT,
    SJS_TEMPORAL_DURATION,
    /* Not Temporal's: a Date (date.c, docs/date.md), held as the same kind
     * because it is an object JavaScript compares by identity, as they are. */
    SJS_TEMPORAL_DATE
} sjs_temporal_type;

/* A date and a time of day in the ISO 8601 calendar: a PlainDate's date
 * (its time is zero), a PlainTime's time (its date is zero), a
 * PlainDateTime's both. */
typedef struct {
    int32_t year;           /* -271821 to 275760 */
    uint8_t month;          /* 1 to 12 */
    uint8_t day;            /* 1 to the month's days */
    uint8_t hour;           /* 0 to 23 */
    uint8_t minute;         /* 0 to 59 */
    uint8_t second;         /* 0 to 59 */
    uint16_t millisecond;   /* 0 to 999, and so the next two */
    uint16_t microsecond;
    uint16_t nanosecond;
} sjs_iso_date_time;

/*
 * A Temporal value. Immutable, as every value is once shared; it holds no
 * references, so releasing it frees it. Which member of `as` it uses
 * depends on its type (h.flags): `iso` for the plain types; `instant` for
 * an Instant, nanoseconds since the epoch as whole seconds (rounded toward
 * negative infinity) and the nanoseconds past them, which fits the range
 * (+-8.64e21 ns) where an int64 of nanoseconds would not; `duration` for a
 * Duration, its ten fields from years to nanoseconds, each an integer held
 * exactly in a double, all of one sign; `time` for a Date. A Date is the
 * one that may change (its setters, under owned mutation: docs/mutation.md).
 */
typedef struct {
    sjs_heap h;   /* h.flags: sjs_temporal_type */
    union {
        sjs_iso_date_time iso;
        struct {
            int64_t seconds;
            uint32_t nanoseconds;   /* 0 to 999,999,999 */
        } instant;
        double duration[10];
        double time;   /* a Date's time value: whole milliseconds since the
                        * epoch, within 8.64e15 either way, +0 not -0; NaN for
                        * an invalid date */
    } as;
} sjs_temporal;

#define SJS_AS_TEMPORAL(v) ((sjs_temporal *)(void *)(v).as.heap)

static inline sjs_temporal_type sjs_temporal_type_of(sjs_value v) {
    return (sjs_temporal_type)v.as.heap->flags;
}

/* A Temporal value of `type`, its fields zero, for the caller to fill
 * before anything else sees it. */
sjs_result sjs_temporal_make(sjs_context *context, sjs_temporal_type type, sjs_temporal **out);
/* The longest text sjs_temporal_text writes. */
#define SJS_TEMPORAL_TEXT_MAX 128u
/* What toString() with no options gives (and toJSON(), and JSON.stringify
 * quotes), written to `out`: its length. */
uint32_t sjs_temporal_text(const sjs_temporal *t, char *out);
/* A Date's time value as toISOString writes it (at most 27 bytes:
 * +275760-09-13T00:00:00.000Z), into `out`: its length; 0 for an invalid
 * date, which toISOString refuses. */
uint32_t sjs_date_iso_text(double time, char *out);
/* The type's name, as in Temporal.PlainDate. */
sjs_str sjs_temporal_type_name(sjs_temporal_type type);

/* A getter of a Temporal value: the name's `field` in NAMES. Which types
 * have each is temporal.c's to say. */
typedef enum {
    SJS_FIELD_NONE = 0,
    SJS_FIELD_BLANK,
    SJS_FIELD_CALENDAR_ID,
    SJS_FIELD_DAY,
    SJS_FIELD_DAY_OF_WEEK,
    SJS_FIELD_DAY_OF_YEAR,
    SJS_FIELD_DAYS,
    SJS_FIELD_DAYS_IN_MONTH,
    SJS_FIELD_DAYS_IN_WEEK,
    SJS_FIELD_DAYS_IN_YEAR,
    SJS_FIELD_EPOCH_MILLISECONDS,
    SJS_FIELD_EPOCH_NANOSECONDS,
    SJS_FIELD_ERA,
    SJS_FIELD_ERA_YEAR,
    SJS_FIELD_HOUR,
    SJS_FIELD_HOURS,
    SJS_FIELD_IN_LEAP_YEAR,
    SJS_FIELD_MICROSECOND,
    SJS_FIELD_MICROSECONDS,
    SJS_FIELD_MILLISECOND,
    SJS_FIELD_MILLISECONDS,
    SJS_FIELD_MINUTE,
    SJS_FIELD_MINUTES,
    SJS_FIELD_MONTH,
    SJS_FIELD_MONTH_CODE,
    SJS_FIELD_MONTHS,
    SJS_FIELD_MONTHS_IN_YEAR,
    SJS_FIELD_NANOSECOND,
    SJS_FIELD_NANOSECONDS,
    SJS_FIELD_SECOND,
    SJS_FIELD_SECONDS,
    SJS_FIELD_SIGN,
    SJS_FIELD_WEEK_OF_YEAR,
    SJS_FIELD_WEEKS,
    SJS_FIELD_YEAR,
    SJS_FIELD_YEAR_OF_WEEK,
    SJS_FIELD_YEARS,
    SJS_FIELD_COUNT
} sjs_temporal_field;

/* ========================================================================
 * Built-ins
 */

typedef enum {
    SJS_BUILTIN_NONE = 0,
    /* Functions. */
    SJS_BUILTIN_STRING,
    SJS_BUILTIN_STRING_FROM_CHAR_CODE,
    SJS_BUILTIN_STRING_FROM_CODE_POINT,
    SJS_BUILTIN_STRING_RAW,
    SJS_BUILTIN_BOOLEAN,
    SJS_BUILTIN_ARRAY_FROM,
    SJS_BUILTIN_ARRAY_IS_ARRAY,
    SJS_BUILTIN_ARRAY_OF,
    SJS_BUILTIN_OBJECT_KEYS,
    SJS_BUILTIN_OBJECT_VALUES,
    SJS_BUILTIN_OBJECT_ENTRIES,
    SJS_BUILTIN_OBJECT_FROM_ENTRIES,
    SJS_BUILTIN_OBJECT_GROUP_BY,
    SJS_BUILTIN_OBJECT_HAS_OWN,
    SJS_BUILTIN_OBJECT_ASSIGN,
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
    SJS_BUILTIN_NUMBER,
    SJS_BUILTIN_PARSE_INT,
    SJS_BUILTIN_PARSE_FLOAT,
    SJS_BUILTIN_IS_NAN,
    SJS_BUILTIN_IS_FINITE,
    SJS_BUILTIN_MATH_SQRT,
    SJS_BUILTIN_MATH_HYPOT,
    SJS_BUILTIN_MATH_FROUND,
    SJS_BUILTIN_MATH_CLZ32,
    SJS_BUILTIN_MATH_IMUL,
    SJS_BUILTIN_MATH_RANDOM,
    SJS_BUILTIN_MATH_POW,
    SJS_BUILTIN_MATH_EXP,
    SJS_BUILTIN_MATH_EXPM1,
    SJS_BUILTIN_MATH_LOG,
    SJS_BUILTIN_MATH_LOG1P,
    SJS_BUILTIN_MATH_LOG2,
    SJS_BUILTIN_MATH_LOG10,
    SJS_BUILTIN_MATH_SIN,
    SJS_BUILTIN_MATH_COS,
    SJS_BUILTIN_MATH_TAN,
    SJS_BUILTIN_MATH_ASIN,
    SJS_BUILTIN_MATH_ACOS,
    SJS_BUILTIN_MATH_ATAN,
    SJS_BUILTIN_MATH_ATAN2,
    SJS_BUILTIN_MATH_SINH,
    SJS_BUILTIN_MATH_COSH,
    SJS_BUILTIN_MATH_TANH,
    SJS_BUILTIN_MATH_ASINH,
    SJS_BUILTIN_MATH_ACOSH,
    SJS_BUILTIN_MATH_ATANH,
    SJS_BUILTIN_MATH_CBRT,
    SJS_BUILTIN_BIGINT,
    SJS_BUILTIN_BIGINT_AS_INT_N,
    SJS_BUILTIN_BIGINT_AS_UINT_N,
    SJS_BUILTIN_MAP_GROUP_BY,
    SJS_BUILTIN_JSON_PARSE,
    SJS_BUILTIN_JSON_STRINGIFY,
    SJS_BUILTIN_ENCODE_URI,
    SJS_BUILTIN_ENCODE_URI_COMPONENT,
    SJS_BUILTIN_DECODE_URI,
    SJS_BUILTIN_DECODE_URI_COMPONENT,
    SJS_BUILTIN_OBJECT_FREEZE,
    SJS_BUILTIN_OBJECT_IS_FROZEN,
    SJS_BUILTIN_OBJECT_SEAL,
    SJS_BUILTIN_OBJECT_IS_SEALED,
    /* Date's (date.c): `new Date(...)` (under the space "new", as Temporal's
     * constructors), Date called as a function, and its functions. */
    SJS_BUILTIN_DATE,
    SJS_BUILTIN_DATE_CALL,
    SJS_BUILTIN_DATE_NOW,
    SJS_BUILTIN_DATE_PARSE,
    SJS_BUILTIN_DATE_UTC,
    /* Temporal's (temporal.c): from SJS_BUILTIN_TEMPORAL_FIRST to the count.
     * A constructor is in the function table under the space "new", which
     * no name a program writes reaches: only `new Temporal.X(...)`. */
    SJS_BUILTIN_TEMPORAL_FIRST,
    SJS_BUILTIN_TEMPORAL_PLAIN_DATE = SJS_BUILTIN_TEMPORAL_FIRST,
    SJS_BUILTIN_TEMPORAL_PLAIN_DATE_FROM,
    SJS_BUILTIN_TEMPORAL_PLAIN_DATE_COMPARE,
    SJS_BUILTIN_TEMPORAL_PLAIN_TIME,
    SJS_BUILTIN_TEMPORAL_PLAIN_TIME_FROM,
    SJS_BUILTIN_TEMPORAL_PLAIN_TIME_COMPARE,
    SJS_BUILTIN_TEMPORAL_PLAIN_DATE_TIME,
    SJS_BUILTIN_TEMPORAL_PLAIN_DATE_TIME_FROM,
    SJS_BUILTIN_TEMPORAL_PLAIN_DATE_TIME_COMPARE,
    SJS_BUILTIN_TEMPORAL_DURATION,
    SJS_BUILTIN_TEMPORAL_DURATION_FROM,
    SJS_BUILTIN_TEMPORAL_DURATION_COMPARE,
    SJS_BUILTIN_TEMPORAL_INSTANT,
    SJS_BUILTIN_TEMPORAL_INSTANT_FROM,
    SJS_BUILTIN_TEMPORAL_INSTANT_COMPARE,
    SJS_BUILTIN_TEMPORAL_INSTANT_FROM_EPOCH_MILLISECONDS,
    SJS_BUILTIN_TEMPORAL_INSTANT_FROM_EPOCH_NANOSECONDS,
    SJS_BUILTIN_TEMPORAL_NOW_INSTANT,
    SJS_BUILTIN_TEMPORAL_NOW_PLAIN_DATE_ISO,
    SJS_BUILTIN_TEMPORAL_NOW_PLAIN_DATE_TIME_ISO,
    SJS_BUILTIN_TEMPORAL_NOW_PLAIN_TIME_ISO,
    SJS_BUILTIN_TEMPORAL_NOW_TIME_ZONE_ID,
    SJS_BUILTIN_TEMPORAL_NOW_ZONED_DATE_TIME_ISO,
    SJS_BUILTIN_FUNCTION_COUNT,
    /* Methods, called on a receiver: from 128, which leaves the functions
     * room to grow without moving it. */
    SJS_METHOD_FIRST = 128,
    /* Arrays. */
    SJS_METHOD_ARRAY_AT = SJS_METHOD_FIRST,
    SJS_METHOD_ARRAY_CONCAT,
    SJS_METHOD_ARRAY_EVERY,
    SJS_METHOD_ARRAY_FILTER,
    SJS_METHOD_ARRAY_FIND,
    SJS_METHOD_ARRAY_FIND_INDEX,
    SJS_METHOD_ARRAY_FIND_LAST,
    SJS_METHOD_ARRAY_FIND_LAST_INDEX,
    SJS_METHOD_ARRAY_FLAT,
    SJS_METHOD_ARRAY_FLAT_MAP,
    SJS_METHOD_ARRAY_INCLUDES,
    SJS_METHOD_ARRAY_INDEX_OF,
    SJS_METHOD_ARRAY_JOIN,
    SJS_METHOD_ARRAY_LAST_INDEX_OF,
    SJS_METHOD_ARRAY_MAP,
    SJS_METHOD_ARRAY_REDUCE,
    SJS_METHOD_ARRAY_REDUCE_RIGHT,
    SJS_METHOD_ARRAY_SLICE,
    SJS_METHOD_ARRAY_SOME,
    SJS_METHOD_ARRAY_TO_REVERSED,
    SJS_METHOD_ARRAY_TO_SORTED,
    SJS_METHOD_ARRAY_TO_SPLICED,
    SJS_METHOD_ARRAY_WITH,
    /* Strings. */
    SJS_METHOD_STRING_AT,
    SJS_METHOD_STRING_CHAR_AT,
    SJS_METHOD_STRING_CHAR_CODE_AT,
    SJS_METHOD_STRING_CODE_POINT_AT,
    SJS_METHOD_STRING_CONCAT,
    SJS_METHOD_STRING_ENDS_WITH,
    SJS_METHOD_STRING_INCLUDES,
    SJS_METHOD_STRING_INDEX_OF,
    SJS_METHOD_STRING_IS_WELL_FORMED,
    SJS_METHOD_STRING_PAD_END,
    SJS_METHOD_STRING_PAD_START,
    SJS_METHOD_STRING_REPEAT,
    SJS_METHOD_STRING_REPLACE_ALL,
    SJS_METHOD_STRING_SLICE,
    SJS_METHOD_STRING_SPLIT,
    SJS_METHOD_STRING_MATCH,
    SJS_METHOD_STRING_MATCH_ALL,
    SJS_METHOD_STRING_REPLACE,
    SJS_METHOD_STRING_SEARCH,
    SJS_METHOD_STRING_STARTS_WITH,
    SJS_METHOD_STRING_SUBSTR,
    SJS_METHOD_STRING_SUBSTRING,
    SJS_METHOD_STRING_TO_LOWER_CASE,
    SJS_METHOD_STRING_TO_UPPER_CASE,
    SJS_METHOD_STRING_TO_WELL_FORMED,
    SJS_METHOD_STRING_TRIM,
    SJS_METHOD_STRING_TRIM_END,
    SJS_METHOD_STRING_TRIM_START,
    /* Sets. */
    SJS_METHOD_SET_DIFFERENCE,
    SJS_METHOD_SET_HAS,
    SJS_METHOD_SET_INTERSECTION,
    SJS_METHOD_SET_IS_DISJOINT_FROM,
    SJS_METHOD_SET_IS_SUBSET_OF,
    SJS_METHOD_SET_IS_SUPERSET_OF,
    SJS_METHOD_SET_SYMMETRIC_DIFFERENCE,
    SJS_METHOD_SET_UNION,
    /* BigInts. */
    SJS_METHOD_BIGINT_TO_STRING,
    /* Numbers. */
    SJS_METHOD_NUMBER_TO_EXPONENTIAL,
    SJS_METHOD_NUMBER_TO_FIXED,
    SJS_METHOD_NUMBER_TO_PRECISION,
    SJS_METHOD_NUMBER_TO_STRING,
    /* Temporal values (temporal.c): one ID per name, for every type that
     * has a method of the name; the method checks its receiver's type. */
    SJS_METHOD_TEMPORAL_ABS,
    SJS_METHOD_TEMPORAL_ADD,
    SJS_METHOD_TEMPORAL_EQUALS,
    SJS_METHOD_TEMPORAL_NEGATED,
    SJS_METHOD_TEMPORAL_ROUND,
    SJS_METHOD_TEMPORAL_SINCE,
    SJS_METHOD_TEMPORAL_SUBTRACT,
    SJS_METHOD_TEMPORAL_TO_JSON,
    SJS_METHOD_TEMPORAL_TO_LOCALE_STRING,
    SJS_METHOD_TEMPORAL_TO_PLAIN_DATE,
    SJS_METHOD_TEMPORAL_TO_PLAIN_DATE_TIME,
    SJS_METHOD_TEMPORAL_TO_PLAIN_MONTH_DAY,
    SJS_METHOD_TEMPORAL_TO_PLAIN_TIME,
    SJS_METHOD_TEMPORAL_TO_PLAIN_YEAR_MONTH,
    SJS_METHOD_TEMPORAL_TO_STRING,
    SJS_METHOD_TEMPORAL_TO_ZONED_DATE_TIME,
    SJS_METHOD_TEMPORAL_TO_ZONED_DATE_TIME_ISO,
    SJS_METHOD_TEMPORAL_TOTAL,
    SJS_METHOD_TEMPORAL_UNTIL,
    SJS_METHOD_TEMPORAL_VALUE_OF,
    SJS_METHOD_TEMPORAL_WITH,
    SJS_METHOD_TEMPORAL_WITH_CALENDAR,
    /* A Date's (date.c), with toJSON, toLocaleString, toString and valueOf
     * above: getTime, the getUTC methods, toISOString, toUTCString (and
     * toGMTString); every name that reads local time is LOCAL, which faults
     * (the local setters too), and setTime and the UTC setters SETTER, which
     * faults on a Date called as any method is: on a variable that holds it
     * alone they are mutations (SJS_METHOD_DATE_SET_TIME on). */
    SJS_METHOD_TEMPORAL_DATE_GET_TIME,
    SJS_METHOD_TEMPORAL_DATE_GET_UTC_DATE,
    SJS_METHOD_TEMPORAL_DATE_GET_UTC_DAY,
    SJS_METHOD_TEMPORAL_DATE_GET_UTC_FULL_YEAR,
    SJS_METHOD_TEMPORAL_DATE_GET_UTC_HOURS,
    SJS_METHOD_TEMPORAL_DATE_GET_UTC_MILLISECONDS,
    SJS_METHOD_TEMPORAL_DATE_GET_UTC_MINUTES,
    SJS_METHOD_TEMPORAL_DATE_GET_UTC_MONTH,
    SJS_METHOD_TEMPORAL_DATE_GET_UTC_SECONDS,
    SJS_METHOD_TEMPORAL_DATE_TO_ISO_STRING,
    SJS_METHOD_TEMPORAL_DATE_TO_UTC_STRING,
    SJS_METHOD_TEMPORAL_DATE_LOCAL,
    SJS_METHOD_TEMPORAL_DATE_SETTER,
    SJS_METHOD_TEMPORAL_WITH_PLAIN_TIME,
    /* Regular expressions (regex_match.c). */
    SJS_METHOD_REGEXP_TEST,
    SJS_METHOD_REGEXP_EXEC,
    /* Maps. */
    SJS_METHOD_MAP_GET,
    SJS_METHOD_MAP_HAS,
    SJS_METHOD_MAP_KEYS,
    SJS_METHOD_MAP_VALUES,
    SJS_METHOD_MAP_ENTRIES,
    /* Iterators. */
    SJS_METHOD_ITERATOR_TO_ARRAY,
    SJS_METHOD_ITERATOR_TAKE,
    SJS_METHOD_ITERATOR_DROP,
    /* MUTATION: next() and return() of an iterator nothing else holds, which
     * no program sees again (a temporary): the value, not advanced. */
    SJS_METHOD_ITERATOR_NEXT,
    SJS_METHOD_ITERATOR_RETURN,
    /* Loops over maps and iterators, which sjs_loop_start hands to
     * late_start first: from SJS_METHOD_MAP_FOR_EACH to
     * SJS_METHOD_ITERATOR_FLAT_MAP. */
    SJS_METHOD_MAP_FOR_EACH,
    SJS_METHOD_ITERATOR_MAP,
    SJS_METHOD_ITERATOR_FILTER,
    SJS_METHOD_ITERATOR_REDUCE,
    SJS_METHOD_ITERATOR_SOME,
    SJS_METHOD_ITERATOR_EVERY,
    SJS_METHOD_ITERATOR_FIND,
    SJS_METHOD_ITERATOR_FOR_EACH,
    SJS_METHOD_ITERATOR_FLAT_MAP,
    /* MUTATION: methods that change their receiver, called only through a
     * reference to the variable that owns it (SJS_OP_MUTATE), never through
     * sjs_builtin_call. Arrays', then sets', then step 3's. */
    SJS_METHOD_MUTATION_FIRST,
    SJS_METHOD_ARRAY_FILL = SJS_METHOD_MUTATION_FIRST,
    SJS_METHOD_ARRAY_POP,
    SJS_METHOD_ARRAY_PUSH,
    SJS_METHOD_ARRAY_REVERSE,
    SJS_METHOD_ARRAY_SHIFT,
    SJS_METHOD_ARRAY_SORT,
    SJS_METHOD_ARRAY_UNSHIFT,
    SJS_METHOD_SET_ADD,
    /* MUTATION: removal (docs/mutation.md, "Removal"). */
    SJS_METHOD_SET_CLEAR,
    SJS_METHOD_SET_DELETE,
    SJS_METHOD_ARRAY_SPLICE,
    /* MUTATION: a map's set, delete and clear (docs/mutation.md). */
    SJS_METHOD_MAP_SET,
    SJS_METHOD_MAP_DELETE,
    SJS_METHOD_MAP_CLEAR,
    /* MUTATION: an iterator's next() and return() on a variable that holds
     * it alone, which advance it and close it (docs/mutation.md). */
    SJS_METHOD_ITERATOR_ADVANCE,
    SJS_METHOD_ITERATOR_CLOSE,
    /* MUTATION: a Date's setTime and UTC setters on a variable that holds
     * the Date alone, which write its time value (docs/date.md). */
    SJS_METHOD_DATE_SET_TIME,
    SJS_METHOD_DATE_SET_UTC_FULL_YEAR,
    SJS_METHOD_DATE_SET_UTC_MONTH,
    SJS_METHOD_DATE_SET_UTC_DATE,
    SJS_METHOD_DATE_SET_UTC_HOURS,
    SJS_METHOD_DATE_SET_UTC_MINUTES,
    SJS_METHOD_DATE_SET_UTC_SECONDS,
    SJS_METHOD_DATE_SET_UTC_MILLISECONDS,
    /* The loops of the regular expression methods: a method whose match is
     * too long for a leaf goes on as one (regex_match.c), the loop of
     * SJS_REGEX_LOOP_FIRST plus its sjs_regex_op. Last, so that
     * sjs_loop_start and sjs_loop_step tell them by one comparison. */
    SJS_REGEX_LOOP_FIRST
} sjs_builtin_id;

/* A method's ID is a uint16_t in NAMES (sjs_name.methods, .mutation), and a
 * regular expression loop's in sjs_frame.builtin. */
_Static_assert(SJS_METHOD_MUTATION_FIRST <= UINT16_MAX && SJS_REGEX_LOOP_FIRST <= UINT16_MAX,
               "a method's ID must fit a uint16_t of NAMES");
_Static_assert(SJS_BUILTIN_FUNCTION_COUNT <= SJS_METHOD_FIRST, "the functions' IDs run into the methods'");

/* The built-in function `Namespace.name` or bare `name`; NONE if unknown. */
sjs_builtin_id sjs_builtin_lookup(sjs_str space, sjs_str name);
/* One entry of the table sjs_builtin_lookup searches: the namespace ("" for
 * a global), the name, and the ID. */
typedef struct {
    const char *space;
    const char *name;
    uint16_t id;
} sjs_function_entry;
/* Every entry, in the table's order, and how many (for builtin-table). */
const sjs_function_entry *sjs_functions(uint32_t *count);

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
#define SJS_NAME_ON_MAP 32u    /* on Map.prototype (or Object.prototype) */
#define SJS_NAME_ON_ITERATOR 64u /* on a map's iterator's prototype or an iterator helper's, up to
                                  * Iterator.prototype (or Object.prototype) */
#define SJS_NAME_ON_REGEXP 128u /* on RegExp.prototype (or Object.prototype) */

typedef struct {
    sjs_str name;
    uint8_t flags;       /* SJS_NAME_* */
    uint16_t methods[9]; /* the staticjs method on an array, a string, a set, a BigInt, a map, an
                          * iterator, a regular expression, a number, a Temporal value; 0 if none */
    uint16_t mutation;   /* MUTATION: the method that changes its receiver, of the one kind
                          * that has it (SJS_METHOD_MUTATION_FIRST on); 0 if none */
    uint8_t field;       /* the getter of a Temporal value (sjs_temporal_field); 0 if none */
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
/* MUTATION: the method `name` names that changes `receiver`, an array, a
 * set, a map, an iterator or a Date (a Temporal value of SJS_TEMPORAL_DATE:
 * restore `uint8_t kind` without the Date); NONE if it names none. sjs_mutation_named:
 * the one of whichever kind has it, what the compiler knows from the name
 * (a set's delete and clear for a map's too, which return alike). */
sjs_builtin_id sjs_mutation_of(const sjs_name *name, sjs_value receiver);
sjs_builtin_id sjs_mutation_named(const sjs_name *name);
/* MUTATION: does the method return its receiver (sort, reverse, fill, add, set),
 * rather than something else (push and unshift a length, pop and shift an
 * element)? */
bool sjs_mutation_returns_receiver(sjs_builtin_id id);
/* MUTATION: does the name's method that changes its receiver share the name
 * with a method of another kind that changes nothing (today `add`, a set's
 * and a Temporal value's)? Then the compiler leaves to the run whether a
 * call of it changes a value (docs/mutation.md, "Shared names"). */
bool sjs_mutation_shared(const sjs_name *name);

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
    SJS_OP_OBJECT_LITERAL, /* u32 constant (array of keys, or UINT32_MAX), u8 SJS_LITERAL_*:
                            * [n values, above] -> [object, above], n the keys' count */
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
    SJS_OP_DEFAULT_LOCAL,  /* u16 slot, u32 target: the slot is not undefined: jump */
    /* MUTATION: a method that changes the variable it is called on. */
    SJS_OP_RECEIVER,       /* u16 slot: push a reference to the slot (SJS_INTERNAL_SLOT) */
    SJS_OP_METHOD_MUTATE,  /* u32 constant (string): [reference] -> [reference, callee] */
    SJS_OP_MUTATE,         /* u16 argc, u8 flags (SPREAD, CHAIN): call it, the slot's value
                            * held by the slot alone; a method that returns its receiver
                            * gives a copy of it, or with CHAIN the reference again */
    SJS_OP_SORTED,         /* u16 slot, u8 flags (CHAIN): after a MUTATE of sort, whose
                            * result it pops into the slot; then as MUTATE gives it */
    /* MUTATION: assignment to a property or an element of a variable. */
    SJS_OP_PATH_INDEX,     /* [key] -> [key]: a number stays one, for PATH to index an
                            * array with; anything else becomes a string, as TO_KEY makes it */
    SJS_OP_PATH,           /* u16 n, u8 SJS_PATH_*: [reference, n keys] the place of the
                            * variable's value they name, each a string or a number
                            * (PATH_INDEX): LOAD pushes its
                            * value; STORE pops the value above them into it, and them;
                            * DROP pops them and the value above them; DELETE removes
                            * the property they name, and pops them */
    /* MUTATION: a method that changes a container on a path from a variable
     * (docs/mutation.md, "Methods through a path"). */
    SJS_OP_PATH_METHOD,    /* u32 constant (string): [reference, n keys] -> [.., name, undefined],
                            * the method's name and the room its receiver takes */
    SJS_OP_PATH_CALL,      /* u16 n, u16 argc, u8 flags (SPREAD, CHAIN): [reference, n keys,
                            * name, room, arguments] call the method on the container the keys
                            * name, each container on the way held by its place alone; then
                            * as MUTATE, or with CHAIN [reference, n keys] for the next */
    SJS_OP_PATH_SORTED,    /* u16 n, u8 flags (CHAIN): after a PATH_CALL of sort, [reference,
                            * n keys, sorted] the sorted array back in its place; then as
                            * PATH_CALL gives it */
    /* MUTATION: Object.assign of a variable the function owns
     * (docs/mutation.md, "Object.assign"). */
    SJS_OP_OBJECT_ASSIGN,  /* u16 argc, u8 flags (SPREAD): [callee, reference, argc sources]
                            * put into the slot's object, held by the slot alone; then a
                            * copy of the slot's value */
    /* Not mutation: maps, which execute()'s default case reaches as it does
     * mutation's instructions, so that the evaluator's loop compiles as it
     * did. */
    SJS_OP_NEW_MAP,        /* u16 argc, u8 flags (SPREAD), as NEW_SET */
} sjs_opcode;

/* SJS_OP_CALL flags. */
#define SJS_CALL_METHOD 1u
#define SJS_CALL_SPREAD 2u
#define SJS_CALL_CHAIN 4u   /* MUTATION: SJS_OP_MUTATE: the receiver goes on to the next mutation */

/* MUTATION: SJS_OP_PATH modes. */
#define SJS_PATH_LOAD 0u
#define SJS_PATH_STORE 1u
#define SJS_PATH_DROP 2u
#define SJS_PATH_DELETE 3u   /* MUTATION: step 3: removes the property, pops the keys */
#define SJS_PATH_CALL 4u     /* MUTATION: a method's walk (PATH_CALL), never PATH's operand */

/* SJS_OP_OBJECT_LITERAL flags. */
#define SJS_LITERAL_ABOVE 1u  /* one value is above the literal's values: a spread's source or a computed key */
#define SJS_LITERAL_OPEN 2u   /* more properties follow, put one by one */

/* sjs_function_info flags. */
#define SJS_FUNCTION_REST 1u  /* the last parameter takes the rest of the arguments, as an array */
/* sjs_function_info.flags is 8 bits: every SJS_FUNCTION_* flag must fit. */
#define SJS_FUNCTION_FLAGS SJS_FUNCTION_REST
_Static_assert(SJS_FUNCTION_FLAGS <= UINT8_MAX, "SJS_FUNCTION_* flags must fit sjs_function_info.flags");

typedef struct {
    uint32_t code;          /* offset of the first instruction */
    uint32_t captures;      /* first entry in the capture table */
    uint16_t params;        /* a rest parameter included */
    uint16_t locals;        /* slots, the parameters included */
    uint16_t capture_count;
    uint8_t flags;          /* SJS_FUNCTION_* */
    /* The sjs_shape the matcher found the function computes, or
     * SJS_SHAPE_NONE (docs/matcher.md). It only tags: the code is the same. */
    uint8_t shape;
    /* The shape's arguments, by sjs_shape_arg_kind(shape, i): a text's string
     * constant (an index into the program's constants: interned, NAMED, the
     * same constant GET_MEMBER reads that key with), a flag, a count, an
     * sjs_operator, or an operand (sjs_shape_operand). */
    uint32_t shape_args[SJS_SHAPE_ARGS];
} sjs_function_info;

/* A shape's operand (docs/matcher.md, 11.2): a literal the callback compares
 * with, which the loop knows before it starts. The matcher keeps its kind
 * byte before the value; a function's shape_args keeps the kind in the low
 * 4 bits and, for a number or a string, the index of the constant the
 * literal's own instruction pushes (program->numbers or ->constants). */
enum { SJS_OPERAND_NUMBER = 1, SJS_OPERAND_TEXT, SJS_OPERAND_TRUE, SJS_OPERAND_FALSE, SJS_OPERAND_NULL };
#define SJS_SHAPE_OPERAND_INDICES (1u << 28)  /* an index at or past this is not recorded */
static inline uint32_t sjs_shape_operand(uint32_t kind, uint32_t index) {
    return kind | index << 4;
}
static inline uint32_t sjs_shape_operand_kind(uint32_t arg) {
    return arg & 15u;
}
static inline uint32_t sjs_shape_operand_index(uint32_t arg) {
    return arg >> 4;
}

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
