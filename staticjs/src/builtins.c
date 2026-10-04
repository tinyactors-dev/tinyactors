/*
 * The built-in functions and methods. A built-in that calls a function for
 * each element (map, reduce, toSorted, ...) is a loop frame: it asks the
 * evaluator to make each call and is stepped again with the result, so a
 * callback never runs on the C stack. Every other built-in finishes in one
 * call.
 */
#include "vm.h"

/* ------------------------------------------------------------------------
 * Tables
 */

typedef struct {
    const char *space;
    const char *name;
    uint16_t id;
} function_entry;

static const function_entry FUNCTION_TABLE[] = {
    { "", "String", SJS_BUILTIN_STRING },
    { "", "Boolean", SJS_BUILTIN_BOOLEAN },
    { "Array", "isArray", SJS_BUILTIN_ARRAY_IS_ARRAY },
    { "Object", "keys", SJS_BUILTIN_OBJECT_KEYS },
    { "Object", "values", SJS_BUILTIN_OBJECT_VALUES },
    { "Object", "entries", SJS_BUILTIN_OBJECT_ENTRIES },
    { "Object", "fromEntries", SJS_BUILTIN_OBJECT_FROM_ENTRIES },
    { "Number", "isInteger", SJS_BUILTIN_NUMBER_IS_INTEGER },
    { "Number", "isSafeInteger", SJS_BUILTIN_NUMBER_IS_SAFE_INTEGER },
    { "Number", "isFinite", SJS_BUILTIN_NUMBER_IS_FINITE },
    { "Number", "isNaN", SJS_BUILTIN_NUMBER_IS_NAN },
    { "Math", "abs", SJS_BUILTIN_MATH_ABS },
    { "Math", "min", SJS_BUILTIN_MATH_MIN },
    { "Math", "max", SJS_BUILTIN_MATH_MAX },
    { "Math", "floor", SJS_BUILTIN_MATH_FLOOR },
    { "Math", "ceil", SJS_BUILTIN_MATH_CEIL },
    { "Math", "round", SJS_BUILTIN_MATH_ROUND },
    { "Math", "trunc", SJS_BUILTIN_MATH_TRUNC },
    { "Math", "sign", SJS_BUILTIN_MATH_SIGN }
};

typedef struct {
    uint8_t kind;
    const char *name;
    uint16_t id;
} method_entry;

static const method_entry METHOD_TABLE[] = {
    { SJS_ARRAY, "at", SJS_METHOD_ARRAY_AT },
    { SJS_ARRAY, "concat", SJS_METHOD_ARRAY_CONCAT },
    { SJS_ARRAY, "every", SJS_METHOD_ARRAY_EVERY },
    { SJS_ARRAY, "filter", SJS_METHOD_ARRAY_FILTER },
    { SJS_ARRAY, "find", SJS_METHOD_ARRAY_FIND },
    { SJS_ARRAY, "findIndex", SJS_METHOD_ARRAY_FIND_INDEX },
    { SJS_ARRAY, "findLast", SJS_METHOD_ARRAY_FIND_LAST },
    { SJS_ARRAY, "flat", SJS_METHOD_ARRAY_FLAT },
    { SJS_ARRAY, "flatMap", SJS_METHOD_ARRAY_FLAT_MAP },
    { SJS_ARRAY, "includes", SJS_METHOD_ARRAY_INCLUDES },
    { SJS_ARRAY, "indexOf", SJS_METHOD_ARRAY_INDEX_OF },
    { SJS_ARRAY, "join", SJS_METHOD_ARRAY_JOIN },
    { SJS_ARRAY, "map", SJS_METHOD_ARRAY_MAP },
    { SJS_ARRAY, "reduce", SJS_METHOD_ARRAY_REDUCE },
    { SJS_ARRAY, "slice", SJS_METHOD_ARRAY_SLICE },
    { SJS_ARRAY, "some", SJS_METHOD_ARRAY_SOME },
    { SJS_ARRAY, "toReversed", SJS_METHOD_ARRAY_TO_REVERSED },
    { SJS_ARRAY, "toSorted", SJS_METHOD_ARRAY_TO_SORTED },
    { SJS_ARRAY, "with", SJS_METHOD_ARRAY_WITH },
    { SJS_STRING, "endsWith", SJS_METHOD_STRING_ENDS_WITH },
    { SJS_STRING, "includes", SJS_METHOD_STRING_INCLUDES },
    { SJS_STRING, "indexOf", SJS_METHOD_STRING_INDEX_OF },
    { SJS_STRING, "slice", SJS_METHOD_STRING_SLICE },
    { SJS_STRING, "split", SJS_METHOD_STRING_SPLIT },
    { SJS_STRING, "startsWith", SJS_METHOD_STRING_STARTS_WITH },
    { SJS_STRING, "toLowerCase", SJS_METHOD_STRING_TO_LOWER_CASE },
    { SJS_STRING, "toUpperCase", SJS_METHOD_STRING_TO_UPPER_CASE },
    { SJS_STRING, "trim", SJS_METHOD_STRING_TRIM },
    { SJS_SET, "difference", SJS_METHOD_SET_DIFFERENCE },
    { SJS_SET, "has", SJS_METHOD_SET_HAS },
    { SJS_SET, "intersection", SJS_METHOD_SET_INTERSECTION },
    { SJS_SET, "isSubsetOf", SJS_METHOD_SET_IS_SUBSET_OF },
    { SJS_SET, "symmetricDifference", SJS_METHOD_SET_SYMMETRIC_DIFFERENCE },
    { SJS_SET, "union", SJS_METHOD_SET_UNION }
};

#define FUNCTION_VALUE(id) [id] = { { SJS_IMMORTAL, SJS_HEAP_BUILTIN, 0, 0, sizeof(sjs_builtin_function), NULL }, id }

/* Immortal: their reference counts are never touched, so they are never
 * written. */
static const sjs_builtin_function FUNCTIONS[SJS_BUILTIN_FUNCTION_COUNT] = {
    FUNCTION_VALUE(SJS_BUILTIN_STRING),
    FUNCTION_VALUE(SJS_BUILTIN_BOOLEAN),
    FUNCTION_VALUE(SJS_BUILTIN_ARRAY_IS_ARRAY),
    FUNCTION_VALUE(SJS_BUILTIN_OBJECT_KEYS),
    FUNCTION_VALUE(SJS_BUILTIN_OBJECT_VALUES),
    FUNCTION_VALUE(SJS_BUILTIN_OBJECT_ENTRIES),
    FUNCTION_VALUE(SJS_BUILTIN_OBJECT_FROM_ENTRIES),
    FUNCTION_VALUE(SJS_BUILTIN_NUMBER_IS_INTEGER),
    FUNCTION_VALUE(SJS_BUILTIN_NUMBER_IS_SAFE_INTEGER),
    FUNCTION_VALUE(SJS_BUILTIN_NUMBER_IS_FINITE),
    FUNCTION_VALUE(SJS_BUILTIN_NUMBER_IS_NAN),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_ABS),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_MIN),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_MAX),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_FLOOR),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_CEIL),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_ROUND),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_TRUNC),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_SIGN)
};

sjs_builtin_id sjs_builtin_lookup(sjs_str space, sjs_str name) {
    for (uint32_t i = 0; i < sizeof FUNCTION_TABLE / sizeof FUNCTION_TABLE[0]; i++) {
        if (sjs_str_equal(space, sjs_cstr(FUNCTION_TABLE[i].space)) && sjs_str_equal(name, sjs_cstr(FUNCTION_TABLE[i].name))) {
            return (sjs_builtin_id)FUNCTION_TABLE[i].id;
        }
    }
    return SJS_BUILTIN_NONE;
}

sjs_builtin_id sjs_method_lookup(uint8_t kind, sjs_str name) {
    for (uint32_t i = 0; i < sizeof METHOD_TABLE / sizeof METHOD_TABLE[0]; i++) {
        if (METHOD_TABLE[i].kind == kind && sjs_str_equal(name, sjs_cstr(METHOD_TABLE[i].name))) {
            return (sjs_builtin_id)METHOD_TABLE[i].id;
        }
    }
    return SJS_BUILTIN_NONE;
}

sjs_value sjs_builtin_value(sjs_builtin_id id) {
    return sjs_heap_value(SJS_FUNCTION, (sjs_heap *)(uintptr_t)&FUNCTIONS[id].h);
}

bool sjs_builtin_loops(sjs_builtin_id id) {
    switch (id) {
    case SJS_METHOD_ARRAY_EVERY:
    case SJS_METHOD_ARRAY_FILTER:
    case SJS_METHOD_ARRAY_FIND:
    case SJS_METHOD_ARRAY_FIND_INDEX:
    case SJS_METHOD_ARRAY_FIND_LAST:
    case SJS_METHOD_ARRAY_FLAT_MAP:
    case SJS_METHOD_ARRAY_MAP:
    case SJS_METHOD_ARRAY_REDUCE:
    case SJS_METHOD_ARRAY_SOME:
    case SJS_METHOD_ARRAY_TO_SORTED:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------
 * Helpers
 */

static sjs_value arg(const sjs_leaf_call *call, uint32_t i) {
    return i < call->count ? call->args[i] : sjs_undefined();
}

static sjs_result new_string(sjs_vm *vm, sjs_str text, sjs_value *out) {
    sjs_string *s;
    sjs_result r = sjs_string_make(vm->context, text, &s);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    *out = sjs_heap_value(SJS_STRING, &s->h);
    return SJS_OK;
}

static sjs_result new_array(sjs_vm *vm, uint32_t capacity, sjs_array **out) {
    sjs_result r = sjs_array_make(vm->context, capacity, out);
    return r == SJS_OK ? SJS_OK : sjs_vm_fail(vm, r);
}

/* Append to an array the caller holds alone; takes over `element`. */
static sjs_result array_add(sjs_vm *vm, sjs_array **array, sjs_value element) {
    sjs_result r = sjs_array_grow(vm->context, array, 1);
    if (r != SJS_OK) {
        sjs_release(vm->context, element);
        return sjs_vm_fail(vm, r);
    }
    (*array)->items[(*array)->length++] = element;
    return SJS_OK;
}

sjs_result sjs_vm_to_number(sjs_vm *vm, sjs_value v, double *out) {
    switch (v.kind) {
    case SJS_NUMBER:
        *out = v.as.number;
        return SJS_OK;
    case SJS_UNDEFINED:
        *out = 0.0 / 0.0;
        return SJS_OK;
    case SJS_NULL:
        *out = 0;
        return SJS_OK;
    case SJS_BOOLEAN:
        *out = v.boolean ? 1 : 0;
        return SJS_OK;
    default:
        return sjs_vm_fault(vm, "staticjs converts only numbers, booleans, null and undefined to numbers");
    }
}

/* ToIntegerOrInfinity. */
static sjs_result to_integer(sjs_vm *vm, sjs_value v, double *out) {
    double d;
    if (sjs_vm_to_number(vm, v, &d) != SJS_OK) return SJS_ERR_PROGRAM;
    if (sjs_number_is_nan(d)) d = 0;
    *out = sjs_number_trunc(d) + 0.0;
    return SJS_OK;
}

/* A relative index, clamped to [0, length]. */
static uint32_t relative(double rel, uint32_t length) {
    if (rel < 0) {
        double at = (double)length + rel;
        return at < 0 ? 0 : (uint32_t)at;
    }
    return rel > (double)length ? length : (uint32_t)rel;
}

static bool is_callable(sjs_value v) {
    return v.kind == SJS_FUNCTION && v.internal == SJS_INTERNAL_NONE;
}

/* The byte offset of UTF-16 index `unit`; false when it splits a pair. */
static bool utf16_to_byte(sjs_str s, uint32_t unit, uint32_t *byte) {
    uint32_t units = 0;
    uint32_t i = 0;
    while (i < s.length && units < unit) {
        uint32_t before = i;
        int32_t point = sjs_utf8_next(s, &i);
        units += point >= 0x10000 ? 2u : 1u;
        if (units > unit) {
            *byte = before;
            return false;
        }
    }
    *byte = i;
    return true;
}

static uint32_t byte_to_utf16(sjs_str s, uint32_t byte) {
    return sjs_utf16_length((sjs_str){ s.ptr, byte });
}

static int64_t find_bytes(sjs_str haystack, sjs_str needle, uint32_t from) {
    if (needle.length > haystack.length) return -1;
    for (uint32_t i = from; i + needle.length <= haystack.length; i++) {
        if (sjs_str_equal((sjs_str){ haystack.ptr + i, needle.length }, needle)) return i;
    }
    return -1;
}

static sjs_result string_arg(sjs_vm *vm, sjs_value v, sjs_str *out) {
    if (v.kind != SJS_STRING) return sjs_vm_fault(vm, "staticjs string methods take strings");
    *out = sjs_string_view(SJS_AS_STRING(v));
    return SJS_OK;
}

static sjs_result no_position(sjs_vm *vm, const sjs_leaf_call *call, uint32_t allowed) {
    if (call->count > allowed && call->args[allowed].kind != SJS_UNDEFINED) {
        return sjs_vm_fault(vm, "staticjs string methods take no position");
    }
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Functions
 */

static sjs_result object_listing(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_value o = arg(call, 0);
    if (o.kind != SJS_OBJECT && o.kind != SJS_ARRAY) return sjs_vm_fault(vm, "Object.keys, values and entries take an object or an array");
    uint32_t n = sjs_length(o);
    sjs_array *out;
    if (new_array(vm, n, &out) != SJS_OK) return SJS_ERR_PROGRAM;
    call->result = sjs_heap_value(SJS_ARRAY, &out->h);
    call->cost = n;
    for (uint32_t i = 0; i < n; i++) {
        sjs_value key;
        sjs_value value;
        if (o.kind == SJS_OBJECT) {
            const sjs_entry *e = &SJS_AS_OBJECT(o)->entries[i];
            key = sjs_heap_value(SJS_STRING, &e->key->h);
            sjs_retain(key);
            value = e->value;
        } else {
            sjs_string *s;
            sjs_result r = sjs_string_from_number(vm->context, (double)i, &s);
            if (r != SJS_OK) return sjs_vm_fail(vm, r);
            key = sjs_heap_value(SJS_STRING, &s->h);
            value = SJS_AS_ARRAY(o)->items[i];
        }
        sjs_retain(value);
        sjs_value item;
        if (id == SJS_BUILTIN_OBJECT_KEYS) {
            sjs_release(vm->context, value);
            item = key;
        } else if (id == SJS_BUILTIN_OBJECT_VALUES) {
            sjs_release(vm->context, key);
            item = value;
        } else {
            sjs_array *pair;
            sjs_result r = sjs_array_make(vm->context, 2, &pair);
            if (r != SJS_OK) {
                sjs_release(vm->context, key);
                sjs_release(vm->context, value);
                return sjs_vm_fail(vm, r);
            }
            pair->items[0] = key;
            pair->items[1] = value;
            pair->length = 2;
            item = sjs_heap_value(SJS_ARRAY, &pair->h);
        }
        out->items[out->length++] = item;
    }
    return SJS_OK;
}

static sjs_result from_entries(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_value entries = arg(call, 0);
    if (entries.kind != SJS_ARRAY) return sjs_vm_fault(vm, "Object.fromEntries takes an array of [key, value] arrays");
    const sjs_array *a = SJS_AS_ARRAY(entries);
    sjs_object *o;
    sjs_result r = sjs_object_make(vm->context, a->length, &o);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    call->result = sjs_heap_value(SJS_OBJECT, &o->h);
    call->cost = a->length;
    for (uint32_t i = 0; i < a->length; i++) {
        sjs_value pair = a->items[i];
        if (pair.kind != SJS_ARRAY) return sjs_vm_fault(vm, "Object.fromEntries takes an array of [key, value] arrays");
        sjs_value key = sjs_array_at(pair, 0);
        sjs_value value = sjs_array_at(pair, 1);
        sjs_string *k;
        r = sjs_to_key(vm->context, key, &k);
        if (r == SJS_ERR_PROGRAM) return sjs_vm_fault(vm, "a key must be a primitive");
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        if (sjs_str_equal(sjs_string_view(k), SJS_STR("__proto__"))) {
            sjs_release(vm->context, sjs_heap_value(SJS_STRING, &k->h));
            return sjs_vm_fault(vm, "staticjs objects have no prototype");
        }
        sjs_retain(value);
        r = sjs_object_put_key(vm->context, &o, (sjs_put){ k, value });
        call->result.as.heap = &o->h;
        if (r != SJS_OK) {
            sjs_release(vm->context, sjs_heap_value(SJS_STRING, &k->h));
            sjs_release(vm->context, value);
            return sjs_vm_fail(vm, r);
        }
    }
    return SJS_OK;
}

static sjs_result math(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    if (id == SJS_BUILTIN_MATH_MIN || id == SJS_BUILTIN_MATH_MAX) {
        bool max = id == SJS_BUILTIN_MATH_MAX;
        double best = max ? -1.0 / 0.0 : 1.0 / 0.0;
        bool nan = false;
        for (uint32_t i = 0; i < call->count; i++) {
            double d;
            if (sjs_vm_to_number(vm, call->args[i], &d) != SJS_OK) return SJS_ERR_PROGRAM;
            if (sjs_number_is_nan(d)) {
                nan = true;
                continue;
            }
            if (d == best && d == 0) {
                /* +0 is larger than -0. */
                bool negative = sjs_number_is_negative_zero(d);
                if (max ? !negative : negative) best = d;
                continue;
            }
            if (max ? d > best : d < best) best = d;
        }
        call->cost = call->count;
        call->result = sjs_number(nan ? 0.0 / 0.0 : best);
        return SJS_OK;
    }
    double x;
    if (sjs_vm_to_number(vm, arg(call, 0), &x) != SJS_OK) return SJS_ERR_PROGRAM;
    double y = x;
    switch (id) {
    case SJS_BUILTIN_MATH_ABS:
        y = (x < 0 || sjs_number_is_negative_zero(x)) ? -x : x;
        break;
    case SJS_BUILTIN_MATH_FLOOR:
        y = sjs_number_floor(x);
        break;
    case SJS_BUILTIN_MATH_CEIL:
        y = sjs_number_ceil(x);
        break;
    case SJS_BUILTIN_MATH_ROUND:
        y = sjs_number_round(x);
        break;
    case SJS_BUILTIN_MATH_TRUNC:
        y = sjs_number_trunc(x);
        break;
    default: /* SIGN */
        y = x > 0 ? 1.0 : (x < 0 ? -1.0 : x);
        break;
    }
    call->result = sjs_number(y);
    return SJS_OK;
}

static sjs_result function_call(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_value x = arg(call, 0);
    switch (id) {
    case SJS_BUILTIN_STRING: {
        if (call->count == 0) return new_string(vm, SJS_STR(""), &call->result);
        sjs_string *s;
        sjs_result r = sjs_to_string(vm->context, x, &s);
        if (r == SJS_ERR_PROGRAM) return sjs_vm_fault(vm, "staticjs turns only primitives into strings");
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        call->result = sjs_heap_value(SJS_STRING, &s->h);
        return SJS_OK;
    }
    case SJS_BUILTIN_BOOLEAN:
        call->result = sjs_boolean(sjs_truthy(x));
        return SJS_OK;
    case SJS_BUILTIN_ARRAY_IS_ARRAY:
        call->result = sjs_boolean(x.kind == SJS_ARRAY);
        return SJS_OK;
    case SJS_BUILTIN_OBJECT_KEYS:
    case SJS_BUILTIN_OBJECT_VALUES:
    case SJS_BUILTIN_OBJECT_ENTRIES:
        return object_listing(vm, id, call);
    case SJS_BUILTIN_OBJECT_FROM_ENTRIES:
        return from_entries(vm, call);
    case SJS_BUILTIN_NUMBER_IS_INTEGER:
        call->result = sjs_boolean(x.kind == SJS_NUMBER && sjs_number_is_integer(x.as.number));
        return SJS_OK;
    case SJS_BUILTIN_NUMBER_IS_SAFE_INTEGER:
        call->result = sjs_boolean(x.kind == SJS_NUMBER && sjs_number_is_integer(x.as.number) &&
                                   x.as.number <= 9007199254740991.0 && x.as.number >= -9007199254740991.0);
        return SJS_OK;
    case SJS_BUILTIN_NUMBER_IS_FINITE:
        call->result = sjs_boolean(x.kind == SJS_NUMBER && sjs_number_is_finite(x.as.number));
        return SJS_OK;
    case SJS_BUILTIN_NUMBER_IS_NAN:
        call->result = sjs_boolean(x.kind == SJS_NUMBER && sjs_number_is_nan(x.as.number));
        return SJS_OK;
    default:
        return math(vm, id, call);
    }
}

/* ------------------------------------------------------------------------
 * Array methods that do not call back
 */

/* Elements [from, to) of an array. */
typedef struct {
    uint32_t from;
    uint32_t to;
} range;

static sjs_result array_slice(sjs_vm *vm, sjs_leaf_call *call, range span) {
    const sjs_array *a = SJS_AS_ARRAY(call->receiver);
    uint32_t from = span.from;
    uint32_t to = span.to;
    sjs_value *out = &call->result;
    uint32_t n = to > from ? to - from : 0;
    sjs_array *s;
    if (new_array(vm, n, &s) != SJS_OK) return SJS_ERR_PROGRAM;
    for (uint32_t i = 0; i < n; i++) {
        s->items[i] = a->items[from + i];
        sjs_retain(s->items[i]);
    }
    s->length = n;
    *out = sjs_heap_value(SJS_ARRAY, &s->h);
    return SJS_OK;
}

static sjs_result array_join(sjs_vm *vm, sjs_leaf_call *call) {
    const sjs_array *a = SJS_AS_ARRAY(call->receiver);
    sjs_value separator = arg(call, 0);
    sjs_value *out = &call->result;
    sjs_str sep = SJS_STR(",");
    if (separator.kind != SJS_UNDEFINED && string_arg(vm, separator, &sep) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_buf text = { 0 };
    const sjs_allocator *alloc = &vm->context->counting;
    sjs_result r = SJS_OK;
    for (uint32_t i = 0; i < a->length && r == SJS_OK; i++) {
        if (i > 0) r = sjs_buf_append(alloc, &text, (sjs_bytes){ (const uint8_t *)sep.ptr, sep.length });
        sjs_value v = a->items[i];
        if (r != SJS_OK || v.kind == SJS_UNDEFINED || v.kind == SJS_NULL) continue;
        sjs_string *s;
        r = sjs_to_string(vm->context, v, &s);
        if (r != SJS_OK) break;
        r = sjs_buf_append(alloc, &text, (sjs_bytes){ (const uint8_t *)s->bytes, s->length });
        sjs_release(vm->context, sjs_heap_value(SJS_STRING, &s->h));
    }
    if (r == SJS_OK) r = new_string(vm, (sjs_str){ (const char *)text.ptr, text.length }, out);
    else if (r == SJS_ERR_PROGRAM) r = sjs_vm_fault(vm, "staticjs joins only primitives");
    else r = sjs_vm_fail(vm, r);
    sjs_buf_free(alloc, &text);
    return r;
}

static sjs_result array_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    const sjs_array *a = SJS_AS_ARRAY(call->receiver);
    uint32_t n = a->length;
    call->cost = n;
    switch (id) {
    case SJS_METHOD_ARRAY_AT: {
        double rel;
        if (to_integer(vm, arg(call, 0), &rel) != SJS_OK) return SJS_ERR_PROGRAM;
        double k = rel < 0 ? (double)n + rel : rel;
        call->result = (k < 0 || k >= (double)n) ? sjs_undefined() : a->items[(uint32_t)k];
        sjs_retain(call->result);
        return SJS_OK;
    }
    case SJS_METHOD_ARRAY_CONCAT: {
        sjs_array *out;
        if (new_array(vm, n, &out) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_heap_value(SJS_ARRAY, &out->h);
        for (uint32_t i = 0; i < n; i++) {
            sjs_retain(a->items[i]);
            out->items[out->length++] = a->items[i];
        }
        for (uint32_t i = 0; i < call->count; i++) {
            sjs_value v = call->args[i];
            if (v.kind == SJS_ARRAY) {
                const sjs_array *b = SJS_AS_ARRAY(v);
                for (uint32_t j = 0; j < b->length; j++) {
                    sjs_retain(b->items[j]);
                    if (array_add(vm, &out, b->items[j]) != SJS_OK) return SJS_ERR_PROGRAM;
                    call->result.as.heap = &out->h;
                }
                call->cost += b->length;
            } else {
                sjs_retain(v);
                if (array_add(vm, &out, v) != SJS_OK) return SJS_ERR_PROGRAM;
                call->result.as.heap = &out->h;
            }
        }
        return SJS_OK;
    }
    case SJS_METHOD_ARRAY_FLAT: {
        sjs_value depth = arg(call, 0);
        if (!(depth.kind == SJS_UNDEFINED || (depth.kind == SJS_NUMBER && depth.as.number == 1))) {
            return sjs_vm_fault(vm, "staticjs flattens one level");
        }
        sjs_array *out;
        if (new_array(vm, n, &out) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_heap_value(SJS_ARRAY, &out->h);
        for (uint32_t i = 0; i < n; i++) {
            sjs_value v = a->items[i];
            if (v.kind == SJS_ARRAY) {
                const sjs_array *b = SJS_AS_ARRAY(v);
                for (uint32_t j = 0; j < b->length; j++) {
                    sjs_retain(b->items[j]);
                    if (array_add(vm, &out, b->items[j]) != SJS_OK) return SJS_ERR_PROGRAM;
                    call->result.as.heap = &out->h;
                }
            } else {
                sjs_retain(v);
                if (array_add(vm, &out, v) != SJS_OK) return SJS_ERR_PROGRAM;
                call->result.as.heap = &out->h;
            }
        }
        return SJS_OK;
    }
    case SJS_METHOD_ARRAY_INCLUDES:
    case SJS_METHOD_ARRAY_INDEX_OF: {
        sjs_value x = arg(call, 0);
        if (!sjs_is_primitive(x)) return sjs_vm_fault(vm, "staticjs does not compare objects by identity");
        if (call->count > 1) return sjs_vm_fault(vm, "staticjs searches the whole array");
        bool includes = id == SJS_METHOD_ARRAY_INCLUDES;
        for (uint32_t i = 0; i < n; i++) {
            bool equal = includes ? sjs_same_value_zero(a->items[i], x) : sjs_strict_equal_primitive(a->items[i], x);
            if (equal) {
                call->result = includes ? sjs_boolean(true) : sjs_number((double)i);
                return SJS_OK;
            }
        }
        call->result = includes ? sjs_boolean(false) : sjs_number(-1);
        return SJS_OK;
    }
    case SJS_METHOD_ARRAY_JOIN:
        return array_join(vm, call);
    case SJS_METHOD_ARRAY_SLICE: {
        double start = 0;
        double end = (double)n;
        if (to_integer(vm, arg(call, 0), &start) != SJS_OK) return SJS_ERR_PROGRAM;
        if (arg(call, 1).kind != SJS_UNDEFINED && to_integer(vm, arg(call, 1), &end) != SJS_OK) return SJS_ERR_PROGRAM;
        return array_slice(vm, call, (range){ relative(start, n), relative(end, n) });
    }
    case SJS_METHOD_ARRAY_TO_REVERSED: {
        sjs_array *out;
        if (new_array(vm, n, &out) != SJS_OK) return SJS_ERR_PROGRAM;
        for (uint32_t i = 0; i < n; i++) {
            out->items[i] = a->items[n - 1 - i];
            sjs_retain(out->items[i]);
        }
        out->length = n;
        call->result = sjs_heap_value(SJS_ARRAY, &out->h);
        return SJS_OK;
    }
    default: { /* WITH */
        double rel;
        if (to_integer(vm, arg(call, 0), &rel) != SJS_OK) return SJS_ERR_PROGRAM;
        double k = rel < 0 ? (double)n + rel : rel;
        if (k < 0 || k >= (double)n) return sjs_vm_fault(vm, "with() needs an index inside the array");
        if (array_slice(vm, call, (range){ 0, n }) != SJS_OK) return SJS_ERR_PROGRAM;
        sjs_array *out = SJS_AS_ARRAY(call->result);
        sjs_value v = arg(call, 1);
        sjs_retain(v);
        sjs_release(vm->context, out->items[(uint32_t)k]);
        out->items[(uint32_t)k] = v;
        return SJS_OK;
    }
    }
}

/* ------------------------------------------------------------------------
 * String methods
 */

static bool is_trim_space(uint32_t p) {
    return p == 0x09u || p == 0x0Au || p == 0x0Bu || p == 0x0Cu || p == 0x0Du || p == 0x20u || p == 0xA0u ||
           p == 0x1680u || (p >= 0x2000u && p <= 0x200Au) || p == 0x2028u || p == 0x2029u || p == 0x202Fu ||
           p == 0x205Fu || p == 0x3000u || p == 0xFEFFu;
}

static sjs_result string_split(sjs_vm *vm, sjs_str s, sjs_leaf_call *call) {
    sjs_value separator = arg(call, 0);
    if (call->count > 1 && call->args[1].kind != SJS_UNDEFINED) return sjs_vm_fault(vm, "staticjs splits without a limit");
    sjs_array *out;
    if (new_array(vm, 4, &out) != SJS_OK) return SJS_ERR_PROGRAM;
    call->result = sjs_heap_value(SJS_ARRAY, &out->h);
    call->cost = s.length;
    if (separator.kind == SJS_UNDEFINED) {
        sjs_value whole;
        if (new_string(vm, s, &whole) != SJS_OK) return SJS_ERR_PROGRAM;
        if (array_add(vm, &out, whole) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result.as.heap = &out->h;
        return SJS_OK;
    }
    sjs_str sep;
    if (string_arg(vm, separator, &sep) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t start = 0;
    if (sep.length == 0) {
        while (start < s.length) {
            uint32_t next = start;
            int32_t point = sjs_utf8_next(s, &next);
            if (point >= 0x10000) return sjs_vm_fault(vm, "splitting would cut a character outside the BMP in two");
            sjs_value piece;
            if (new_string(vm, (sjs_str){ s.ptr + start, next - start }, &piece) != SJS_OK) return SJS_ERR_PROGRAM;
            if (array_add(vm, &out, piece) != SJS_OK) return SJS_ERR_PROGRAM;
            call->result.as.heap = &out->h;
            start = next;
        }
        return SJS_OK;
    }
    for (;;) {
        int64_t at = find_bytes(s, sep, start);
        uint32_t end = at < 0 ? s.length : (uint32_t)at;
        sjs_value piece;
        if (new_string(vm, (sjs_str){ s.ptr + start, end - start }, &piece) != SJS_OK) return SJS_ERR_PROGRAM;
        if (array_add(vm, &out, piece) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result.as.heap = &out->h;
        if (at < 0) return SJS_OK;
        start = end + sep.length;
    }
}

static sjs_result string_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_str s = sjs_string_view(SJS_AS_STRING(call->receiver));
    call->cost = s.length / 16u;
    switch (id) {
    case SJS_METHOD_STRING_ENDS_WITH:
    case SJS_METHOD_STRING_STARTS_WITH:
    case SJS_METHOD_STRING_INCLUDES:
    case SJS_METHOD_STRING_INDEX_OF: {
        sjs_str needle;
        if (string_arg(vm, arg(call, 0), &needle) != SJS_OK) return SJS_ERR_PROGRAM;
        if (no_position(vm, call, 1) != SJS_OK) return SJS_ERR_PROGRAM;
        if (id == SJS_METHOD_STRING_STARTS_WITH) {
            call->result = sjs_boolean(needle.length <= s.length && sjs_str_equal((sjs_str){ s.ptr, needle.length }, needle));
        } else if (id == SJS_METHOD_STRING_ENDS_WITH) {
            call->result = sjs_boolean(needle.length <= s.length &&
                                       sjs_str_equal((sjs_str){ s.ptr + s.length - needle.length, needle.length }, needle));
        } else {
            int64_t at = find_bytes(s, needle, 0);
            if (id == SJS_METHOD_STRING_INCLUDES) call->result = sjs_boolean(at >= 0);
            else call->result = sjs_number(at < 0 ? -1.0 : (double)byte_to_utf16(s, (uint32_t)at));
        }
        return SJS_OK;
    }
    case SJS_METHOD_STRING_SLICE: {
        uint32_t units = SJS_AS_STRING(call->receiver)->utf16_length;
        double start = 0;
        double end = (double)units;
        if (to_integer(vm, arg(call, 0), &start) != SJS_OK) return SJS_ERR_PROGRAM;
        if (arg(call, 1).kind != SJS_UNDEFINED && to_integer(vm, arg(call, 1), &end) != SJS_OK) return SJS_ERR_PROGRAM;
        uint32_t from = relative(start, units);
        uint32_t to = relative(end, units);
        if (to < from) to = from;
        uint32_t a;
        uint32_t b;
        if (!utf16_to_byte(s, from, &a) || !utf16_to_byte(s, to, &b)) {
            return sjs_vm_fault(vm, "slicing would cut a character outside the BMP in two");
        }
        return new_string(vm, (sjs_str){ s.ptr + a, b - a }, &call->result);
    }
    case SJS_METHOD_STRING_SPLIT:
        return string_split(vm, s, call);
    case SJS_METHOD_STRING_TO_LOWER_CASE:
    case SJS_METHOD_STRING_TO_UPPER_CASE: {
        if (!(SJS_AS_STRING(call->receiver)->h.flags & SJS_STRING_ASCII)) {
            return sjs_vm_fault(vm, "staticjs changes the case of ASCII strings only");
        }
        if (new_string(vm, s, &call->result) != SJS_OK) return SJS_ERR_PROGRAM;
        sjs_string *t = SJS_AS_STRING(call->result);
        bool lower = id == SJS_METHOD_STRING_TO_LOWER_CASE;
        for (uint32_t i = 0; i < t->length; i++) {
            char c = t->bytes[i];
            if (lower && c >= 'A' && c <= 'Z') t->bytes[i] = (char)(c + 32);
            if (!lower && c >= 'a' && c <= 'z') t->bytes[i] = (char)(c - 32);
        }
        return SJS_OK;
    }
    default: { /* TRIM */
        uint32_t start = 0;
        while (start < s.length) {
            uint32_t next = start;
            if (!is_trim_space((uint32_t)sjs_utf8_next(s, &next))) break;
            start = next;
        }
        uint32_t end = start;
        uint32_t i = start;
        while (i < s.length) {
            int32_t point = sjs_utf8_next(s, &i);
            if (!is_trim_space((uint32_t)point)) end = i;
        }
        return new_string(vm, (sjs_str){ s.ptr + start, end - start }, &call->result);
    }
    }
}

/* ------------------------------------------------------------------------
 * Set methods
 */

static sjs_result set_make(sjs_vm *vm, uint32_t capacity, sjs_array **out) {
    sjs_value v;
    sjs_result r = sjs_set_new(vm->context, capacity, &v);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    *out = SJS_AS_ARRAY(v);
    return SJS_OK;
}

static sjs_result set_add(sjs_vm *vm, sjs_array **set, sjs_value element) {
    sjs_retain(element);
    sjs_result r = sjs_set_insert(vm->context, set, element);
    if (r == SJS_OK) return SJS_OK;
    sjs_release(vm->context, element);
    return sjs_vm_fail(vm, r);
}

static sjs_result set_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    const sjs_array *a = SJS_AS_ARRAY(call->receiver);
    sjs_value x = arg(call, 0);
    if (id == SJS_METHOD_SET_HAS) {
        call->result = sjs_boolean(sjs_set_contains(a, x));
        call->cost = a->length;
        return SJS_OK;
    }
    if (x.kind != SJS_SET) return sjs_vm_fault(vm, "staticjs combines a set only with another set");
    const sjs_array *b = SJS_AS_ARRAY(x);
    call->cost = a->length * (b->length + 1u);
    if (id == SJS_METHOD_SET_IS_SUBSET_OF) {
        bool subset = a->length <= b->length;
        for (uint32_t i = 0; subset && i < a->length; i++) subset = sjs_set_contains(b, a->items[i]);
        call->result = sjs_boolean(subset);
        return SJS_OK;
    }
    sjs_array *out;
    if (set_make(vm, a->length, &out) != SJS_OK) return SJS_ERR_PROGRAM;
    call->result = sjs_heap_value(SJS_SET, &out->h);
    sjs_result r = SJS_OK;
    switch (id) {
    case SJS_METHOD_SET_UNION:
        for (uint32_t i = 0; r == SJS_OK && i < a->length; i++) r = set_add(vm, &out, a->items[i]);
        for (uint32_t i = 0; r == SJS_OK && i < b->length; i++) r = set_add(vm, &out, b->items[i]);
        break;
    case SJS_METHOD_SET_INTERSECTION:
        /* In the order of the smaller set, as the specification walks it. */
        if (a->length <= b->length) {
            for (uint32_t i = 0; r == SJS_OK && i < a->length; i++) {
                if (sjs_set_contains(b, a->items[i])) r = set_add(vm, &out, a->items[i]);
            }
        } else {
            for (uint32_t i = 0; r == SJS_OK && i < b->length; i++) {
                if (sjs_set_contains(a, b->items[i])) r = set_add(vm, &out, b->items[i]);
            }
        }
        break;
    case SJS_METHOD_SET_DIFFERENCE:
        for (uint32_t i = 0; r == SJS_OK && i < a->length; i++) {
            if (!sjs_set_contains(b, a->items[i])) r = set_add(vm, &out, a->items[i]);
        }
        break;
    default: /* SYMMETRIC_DIFFERENCE */
        for (uint32_t i = 0; r == SJS_OK && i < a->length; i++) {
            if (!sjs_set_contains(b, a->items[i])) r = set_add(vm, &out, a->items[i]);
        }
        for (uint32_t i = 0; r == SJS_OK && i < b->length; i++) {
            if (!sjs_set_contains(a, b->items[i])) r = set_add(vm, &out, b->items[i]);
        }
        break;
    }
    call->result.as.heap = &out->h;
    return r;
}

sjs_result sjs_builtin_call(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    call->result = sjs_undefined();
    call->cost = 0;
    if (id < SJS_BUILTIN_FUNCTION_COUNT) return function_call(vm, id, call);
    if (id <= SJS_METHOD_ARRAY_WITH) return array_method(vm, id, call);
    if (id <= SJS_METHOD_STRING_TRIM) return string_method(vm, id, call);
    return set_method(vm, id, call);
}

/* ------------------------------------------------------------------------
 * Built-ins that call back
 */

static sjs_value *slot(sjs_vm *vm, const vm_frame *frame, uint32_t i) {
    return &vm->stack[frame->base + i];
}

static const sjs_array *receiver(sjs_vm *vm, const vm_frame *frame) {
    return SJS_AS_ARRAY(*slot(vm, frame, LOOP_RECEIVER));
}

static sjs_result set_output(sjs_vm *vm, vm_frame *frame, sjs_value v) {
    sjs_value *out = slot(vm, frame, LOOP_OUTPUT);
    sjs_vm_release(vm, *out);
    *out = v;
    return SJS_OK;
}

sjs_result sjs_loop_start(sjs_vm *vm, vm_frame *frame) {
    sjs_value callback = *slot(vm, frame, LOOP_CALLBACK);
    const sjs_array *a = receiver(vm, frame);
    frame->index = 0;
    frame->length = a->length;
    frame->phase = 0;
    if (!is_callable(callback)) {
        if (frame->builtin == SJS_METHOD_ARRAY_TO_SORTED && callback.kind == SJS_UNDEFINED) {
            return sjs_vm_fault(vm, "staticjs sorts with a compare function");
        }
        return sjs_vm_fault(vm, "the callback is not a function");
    }
    switch (frame->builtin) {
    case SJS_METHOD_ARRAY_MAP:
    case SJS_METHOD_ARRAY_FILTER:
    case SJS_METHOD_ARRAY_FLAT_MAP: {
        sjs_array *out;
        if (new_array(vm, frame->builtin == SJS_METHOD_ARRAY_MAP ? a->length : 4, &out) != SJS_OK) return SJS_ERR_PROGRAM;
        return set_output(vm, frame, sjs_heap_value(SJS_ARRAY, &out->h));
    }
    case SJS_METHOD_ARRAY_REDUCE:
        if (frame->argc >= 2) {
            sjs_value initial = *slot(vm, frame, LOOP_SECOND);
            sjs_retain(initial);
            return set_output(vm, frame, initial);
        }
        if (a->length == 0) return sjs_vm_fault(vm, "reduce of an empty array needs an initial value");
        sjs_retain(a->items[0]);
        frame->index = 1;
        return set_output(vm, frame, a->items[0]);
    case SJS_METHOD_ARRAY_TO_SORTED: {
        uint32_t n = a->length;
        if ((size_t)n * 2u > (size_t)UINT32_MAX / sizeof(sjs_value)) return sjs_vm_fail(vm, SJS_ERR_LIMIT);
        frame->scratch_count = n * 2u;
        frame->scratch = sjs_mem_alloc(&vm->context->counting, (size_t)frame->scratch_count * sizeof(sjs_value));
        if (frame->scratch == NULL) {
            frame->scratch_count = 0;
            return sjs_vm_fail(vm, SJS_ERR_NOMEM);
        }
        for (uint32_t i = 0; i < n; i++) frame->scratch[i] = a->items[i];
        frame->width = 1;
        frame->left = 0;
        frame->index = 0; /* which half of the scratch holds the source */
        return SJS_OK;
    }
    default:
        return SJS_OK;
    }
}

void sjs_loop_free(sjs_vm *vm, vm_frame *frame) {
    if (frame->scratch != NULL) {
        sjs_mem_free(&vm->context->counting, frame->scratch, (size_t)frame->scratch_count * sizeof(sjs_value));
        frame->scratch = NULL;
        frame->scratch_count = 0;
    }
}

/* Push the callback and its arguments. */
static loop_step call_back(sjs_vm *vm, vm_frame *frame, sjs_values values) {
    const sjs_value *args = values.ptr;
    uint32_t count = values.count;
    sjs_value callback = *slot(vm, frame, LOOP_CALLBACK);
    sjs_retain(callback);
    if (!sjs_vm_push(vm, callback)) return LOOP_FAULT;
    for (uint32_t i = 0; i < count; i++) {
        sjs_retain(args[i]);
        if (!sjs_vm_push(vm, args[i])) return LOOP_FAULT;
    }
    frame->argc = count;
    frame->phase = 1;
    return LOOP_CALL;
}

static loop_step call_element(sjs_vm *vm, vm_frame *frame, uint32_t i) {
    const sjs_array *a = receiver(vm, frame);
    sjs_value args[3] = { a->items[i], sjs_number((double)i), *slot(vm, frame, LOOP_RECEIVER) };
    return call_back(vm, frame, (sjs_values){ args, 3 });
}

static loop_step done(sjs_vm *vm, vm_frame *frame, sjs_value result) {
    set_output(vm, frame, result);
    return LOOP_DONE;
}

static loop_step sort_step(sjs_vm *vm, vm_frame *frame) {
    uint32_t n = frame->length;
    for (;;) {
        sjs_value *src = frame->scratch + (frame->index ? n : 0);
        sjs_value *dst = frame->scratch + (frame->index ? 0 : n);
        if (frame->phase == 1) {
            /* The comparator answered. */
            sjs_value r = sjs_vm_pop(vm);
            double d;
            sjs_result ok = sjs_vm_to_number(vm, r, &d);
            sjs_vm_release(vm, r);
            if (ok != SJS_OK) return LOOP_FAULT;
            if (d > 0) dst[frame->k++] = src[frame->j++];
            else dst[frame->k++] = src[frame->i++];
            frame->phase = 2;
        }
        if (frame->phase == 2) {
            if (frame->i < frame->mid && frame->j < frame->right) {
                sjs_value x = src[frame->i];
                sjs_value y = src[frame->j];
                /* undefined sorts last, and the comparator never sees it. */
                if (x.kind == SJS_UNDEFINED) {
                    if (y.kind == SJS_UNDEFINED) dst[frame->k++] = src[frame->i++];
                    else dst[frame->k++] = src[frame->j++];
                    continue;
                }
                if (y.kind == SJS_UNDEFINED) {
                    dst[frame->k++] = src[frame->i++];
                    continue;
                }
                sjs_value args[2] = { x, y };
                return call_back(vm, frame, (sjs_values){ args, 2 });
            }
            while (frame->i < frame->mid) dst[frame->k++] = src[frame->i++];
            while (frame->j < frame->right) dst[frame->k++] = src[frame->j++];
            frame->left += frame->width * 2u;
            frame->phase = 0;
            continue;
        }
        /* Choose the next pair of runs. */
        if (frame->width >= n) {
            sjs_array *out;
            if (new_array(vm, n, &out) != SJS_OK) return LOOP_FAULT;
            for (uint32_t i = 0; i < n; i++) {
                out->items[i] = src[i];
                sjs_retain(src[i]);
            }
            out->length = n;
            return done(vm, frame, sjs_heap_value(SJS_ARRAY, &out->h));
        }
        if (frame->left >= n) {
            frame->index ^= 1u;
            frame->width = frame->width > UINT32_MAX / 2u ? UINT32_MAX : frame->width * 2u;
            frame->left = 0;
            continue;
        }
        uint32_t left = frame->left;
        frame->mid = left + frame->width < n && left + frame->width > left ? left + frame->width : n;
        frame->right = frame->mid + frame->width < n && frame->mid + frame->width > frame->mid ? frame->mid + frame->width : n;
        frame->i = left;
        frame->j = frame->mid;
        frame->k = left;
        frame->phase = 2;
    }
}

loop_step sjs_loop_step(sjs_vm *vm, vm_frame *frame) {
    const sjs_array *a = receiver(vm, frame);
    if (frame->builtin == SJS_METHOD_ARRAY_TO_SORTED) return sort_step(vm, frame);
    if (frame->phase == 1) {
        sjs_value r = sjs_vm_pop(vm);
        frame->phase = 0;
        uint32_t i = frame->index;
        uint32_t element = frame->builtin == SJS_METHOD_ARRAY_FIND_LAST ? frame->length - 1u - i : i;
        frame->index++;
        switch (frame->builtin) {
        case SJS_METHOD_ARRAY_MAP: {
            sjs_array *out = SJS_AS_ARRAY(*slot(vm, frame, LOOP_OUTPUT));
            out->items[out->length++] = r;
            break;
        }
        case SJS_METHOD_ARRAY_FILTER: {
            bool keep = sjs_truthy(r);
            sjs_vm_release(vm, r);
            if (keep) {
                sjs_array *out = SJS_AS_ARRAY(*slot(vm, frame, LOOP_OUTPUT));
                sjs_retain(a->items[i]);
                if (array_add(vm, &out, a->items[i]) != SJS_OK) return LOOP_FAULT;
                slot(vm, frame, LOOP_OUTPUT)->as.heap = &out->h;
            }
            break;
        }
        case SJS_METHOD_ARRAY_FLAT_MAP: {
            sjs_array *out = SJS_AS_ARRAY(*slot(vm, frame, LOOP_OUTPUT));
            if (r.kind == SJS_ARRAY) {
                const sjs_array *b = SJS_AS_ARRAY(r);
                for (uint32_t j = 0; j < b->length; j++) {
                    sjs_retain(b->items[j]);
                    if (array_add(vm, &out, b->items[j]) != SJS_OK) {
                        sjs_vm_release(vm, r);
                        return LOOP_FAULT;
                    }
                    slot(vm, frame, LOOP_OUTPUT)->as.heap = &out->h;
                }
                sjs_vm_release(vm, r);
            } else {
                if (array_add(vm, &out, r) != SJS_OK) return LOOP_FAULT;
                slot(vm, frame, LOOP_OUTPUT)->as.heap = &out->h;
            }
            break;
        }
        case SJS_METHOD_ARRAY_REDUCE:
            set_output(vm, frame, r);
            break;
        default: {
            bool truthy = sjs_truthy(r);
            sjs_vm_release(vm, r);
            switch (frame->builtin) {
            case SJS_METHOD_ARRAY_SOME:
                if (truthy) return done(vm, frame, sjs_boolean(true));
                break;
            case SJS_METHOD_ARRAY_EVERY:
                if (!truthy) return done(vm, frame, sjs_boolean(false));
                break;
            case SJS_METHOD_ARRAY_FIND_INDEX:
                if (truthy) return done(vm, frame, sjs_number((double)element));
                break;
            default: /* FIND, FIND_LAST */
                if (truthy) {
                    sjs_retain(a->items[element]);
                    return done(vm, frame, a->items[element]);
                }
                break;
            }
            break;
        }
        }
    }
    if (frame->index >= frame->length) {
        switch (frame->builtin) {
        case SJS_METHOD_ARRAY_SOME:
            return done(vm, frame, sjs_boolean(false));
        case SJS_METHOD_ARRAY_EVERY:
            return done(vm, frame, sjs_boolean(true));
        case SJS_METHOD_ARRAY_FIND_INDEX:
            return done(vm, frame, sjs_number(-1));
        case SJS_METHOD_ARRAY_FIND:
        case SJS_METHOD_ARRAY_FIND_LAST:
            return done(vm, frame, sjs_undefined());
        default:
            return LOOP_DONE;
        }
    }
    if (frame->builtin == SJS_METHOD_ARRAY_REDUCE) {
        sjs_value args[4] = { *slot(vm, frame, LOOP_OUTPUT), a->items[frame->index], sjs_number((double)frame->index),
                              *slot(vm, frame, LOOP_RECEIVER) };
        return call_back(vm, frame, (sjs_values){ args, 4 });
    }
    uint32_t element = frame->builtin == SJS_METHOD_ARRAY_FIND_LAST ? frame->length - 1u - frame->index : frame->index;
    return call_element(vm, frame, element);
}
