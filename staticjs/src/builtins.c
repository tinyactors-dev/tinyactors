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

/*
 * Every name JavaScript finds on the prototype of an array, a string or a
 * set (those of Node 22, not only the methods staticjs has, so that reading
 * one faults instead of giving undefined), on Object.prototype, or that
 * names a method changing its receiver; with the staticjs method each kind
 * has by that name. Sorted by bytes, for sjs_name_id's binary search.
 */
#define NAME(text, flags, array, string, set) { { text, sizeof(text) - 1u }, flags, { array, string, set } }

static const sjs_name NAMES[] = {
    NAME("__defineGetter__", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("__defineSetter__", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("__lookupGetter__", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("__lookupSetter__", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("__proto__", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("add", SJS_NAME_ON_SET | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("anchor", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("at", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, SJS_METHOD_ARRAY_AT, 0, 0),
    NAME("big", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("blink", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("bold", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("charAt", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("charCodeAt", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("clear", SJS_NAME_ON_SET | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("codePointAt", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("concat", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, SJS_METHOD_ARRAY_CONCAT, 0, 0),
    NAME("constructor", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("copyWithin", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("delete", SJS_NAME_ON_SET | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("difference", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_DIFFERENCE),
    NAME("endsWith", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_ENDS_WITH, 0),
    NAME("entries", SJS_NAME_ON_ARRAY | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("every", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_EVERY, 0, 0),
    NAME("fill", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("filter", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_FILTER, 0, 0),
    NAME("find", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_FIND, 0, 0),
    NAME("findIndex", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_FIND_INDEX, 0, 0),
    NAME("findLast", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_FIND_LAST, 0, 0),
    NAME("findLastIndex", SJS_NAME_ON_ARRAY, 0, 0, 0),
    NAME("fixed", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("flat", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_FLAT, 0, 0),
    NAME("flatMap", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_FLAT_MAP, 0, 0),
    NAME("fontcolor", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("fontsize", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("forEach", SJS_NAME_ON_ARRAY | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("has", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_HAS),
    NAME("hasOwnProperty", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("includes", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, SJS_METHOD_ARRAY_INCLUDES, SJS_METHOD_STRING_INCLUDES, 0),
    NAME("indexOf", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, SJS_METHOD_ARRAY_INDEX_OF, SJS_METHOD_STRING_INDEX_OF, 0),
    NAME("intersection", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_INTERSECTION),
    NAME("isDisjointFrom", SJS_NAME_ON_SET, 0, 0, 0),
    NAME("isPrototypeOf", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("isSubsetOf", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_IS_SUBSET_OF),
    NAME("isSupersetOf", SJS_NAME_ON_SET, 0, 0, 0),
    NAME("isWellFormed", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("italics", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("join", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_JOIN, 0, 0),
    NAME("keys", SJS_NAME_ON_ARRAY | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("lastIndexOf", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("link", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("localeCompare", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("map", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_MAP, 0, 0),
    NAME("match", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("matchAll", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("normalize", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("padEnd", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("padStart", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("pop", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("propertyIsEnumerable", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("push", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("reduce", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_REDUCE, 0, 0),
    NAME("reduceRight", SJS_NAME_ON_ARRAY, 0, 0, 0),
    NAME("repeat", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("replace", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("replaceAll", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("reverse", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("search", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("set", SJS_NAME_MUTATING, 0, 0, 0),
    NAME("shift", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("slice", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, SJS_METHOD_ARRAY_SLICE, SJS_METHOD_STRING_SLICE, 0),
    NAME("small", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("some", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_SOME, 0, 0),
    NAME("sort", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("splice", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("split", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_SPLIT, 0),
    NAME("startsWith", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_STARTS_WITH, 0),
    NAME("strike", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("sub", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("substr", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("substring", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("sup", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("symmetricDifference", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_SYMMETRIC_DIFFERENCE),
    NAME("toLocaleLowerCase", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("toLocaleString", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("toLocaleUpperCase", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("toLowerCase", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_TO_LOWER_CASE, 0),
    NAME("toReversed", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_TO_REVERSED, 0, 0),
    NAME("toSorted", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_TO_SORTED, 0, 0),
    NAME("toSpliced", SJS_NAME_ON_ARRAY, 0, 0, 0),
    NAME("toString", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("toUpperCase", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_TO_UPPER_CASE, 0),
    NAME("toWellFormed", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("trim", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_TRIM, 0),
    NAME("trimEnd", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("trimLeft", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("trimRight", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("trimStart", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("union", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_UNION),
    NAME("unshift", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, 0, 0, 0),
    NAME("valueOf", SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("values", SJS_NAME_ON_ARRAY | SJS_NAME_ON_SET, 0, 0, 0),
    NAME("with", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_WITH, 0, 0)
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

/* Which of `a` and `b` sorts first by bytes: negative, 0 or positive. */
static int32_t name_compare(sjs_str a, sjs_str b) {
    uint32_t n = a.length < b.length ? a.length : b.length;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t x = (uint8_t)a.ptr[i];
        uint8_t y = (uint8_t)b.ptr[i];
        if (x != y) return x < y ? -1 : 1;
    }
    return a.length == b.length ? 0 : a.length < b.length ? -1 : 1;
}

uint16_t sjs_name_id(sjs_str text) {
    uint32_t low = 0;
    uint32_t high = sizeof NAMES / sizeof NAMES[0];
    while (low < high) {
        uint32_t mid = low + (high - low) / 2u;
        int32_t order = name_compare(text, NAMES[mid].name);
        if (order == 0) return (uint16_t)(mid + 1u);
        if (order < 0) high = mid;
        else low = mid + 1u;
    }
    return 0;
}

const sjs_name *sjs_names(uint32_t *count) {
    *count = sizeof NAMES / sizeof NAMES[0];
    return NAMES;
}

const sjs_name *sjs_name_of(const sjs_string *key) {
    uint16_t id = (key->h.flags & SJS_STRING_NAMED) != 0 ? key->h.name_id : sjs_name_id(sjs_string_view(key));
    return id == 0 ? NULL : &NAMES[id - 1u];
}

sjs_builtin_id sjs_method_of(const sjs_name *name, uint8_t kind) {
    if (name == NULL) return SJS_BUILTIN_NONE;
    switch (kind) {
    case SJS_ARRAY:
        return (sjs_builtin_id)name->methods[0];
    case SJS_STRING:
        return (sjs_builtin_id)name->methods[1];
    case SJS_SET:
        return (sjs_builtin_id)name->methods[2];
    default:
        return SJS_BUILTIN_NONE;
    }
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
        sjs_unref(vm->context, element);
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
            sjs_ref(key);
            value = e->value;
        } else {
            sjs_string *s;
            sjs_result r = sjs_string_from_number(vm->context, (double)i, &s);
            if (r != SJS_OK) return sjs_vm_fail(vm, r);
            key = sjs_heap_value(SJS_STRING, &s->h);
            value = SJS_AS_ARRAY(o)->items[i];
        }
        sjs_ref(value);
        sjs_value item;
        if (id == SJS_BUILTIN_OBJECT_KEYS) {
            sjs_unref(vm->context, value);
            item = key;
        } else if (id == SJS_BUILTIN_OBJECT_VALUES) {
            sjs_unref(vm->context, key);
            item = value;
        } else {
            sjs_array *pair;
            sjs_result r = sjs_array_make(vm->context, 2, &pair);
            if (r != SJS_OK) {
                sjs_unref(vm->context, key);
                sjs_unref(vm->context, value);
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
            sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &k->h));
            return sjs_vm_fault(vm, "staticjs objects have no prototype");
        }
        sjs_ref(value);
        r = sjs_object_put_key(vm->context, &o, (sjs_put){ k, value });
        call->result.as.heap = &o->h;
        if (r != SJS_OK) {
            sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &k->h));
            sjs_unref(vm->context, value);
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
        call->result = sjs_num(nan ? 0.0 / 0.0 : best);
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
    call->result = sjs_num(y);
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
        sjs_ref(s->items[i]);
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
        sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &s->h));
    }
    if (r == SJS_OK) r = new_string(vm, (sjs_str){ (const char *)text.ptr, text.length }, out);
    else if (r == SJS_ERR_PROGRAM) r = sjs_vm_fault(vm, "staticjs joins only primitives");
    else r = sjs_vm_fail(vm, r);
    sjs_buf_free(alloc, &text);
    return r;
}

/* Does the method get its receiver to itself? Only an array nothing else
 * holds: an argument that is the same array counts too. */
static bool takes_receiver(const sjs_leaf_call *call) {
    return call->receiver_slot != NULL && call->receiver.kind == SJS_ARRAY && sjs_value_unique(call->receiver);
}

/* Move the receiver out of its slot into the result, to change in place.
 * No element is copied, so the fuel array_method charges for copying the
 * receiver (`cost = n`, before its switch) goes: concat then charges only
 * for what it appends, and with() nothing beyond its own unit. */
static sjs_array *take_receiver(sjs_leaf_call *call) {
    *call->receiver_slot = sjs_undefined();
    call->result = call->receiver;
    call->cost = 0;
    return SJS_AS_ARRAY(call->receiver);
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
        sjs_ref(call->result);
        return SJS_OK;
    }
    case SJS_METHOD_ARRAY_CONCAT: {
        /* An array nothing else holds is appended to in place; `a` may move
         * then, so it is not read again. */
        sjs_array *out;
        if (takes_receiver(call)) {
            out = take_receiver(call);
        } else {
            if (new_array(vm, n, &out) != SJS_OK) return SJS_ERR_PROGRAM;
            call->result = sjs_heap_value(SJS_ARRAY, &out->h);
            for (uint32_t i = 0; i < n; i++) {
                sjs_ref(a->items[i]);
                out->items[out->length++] = a->items[i];
            }
        }
        for (uint32_t i = 0; i < call->count; i++) {
            sjs_value v = call->args[i];
            if (v.kind == SJS_ARRAY) {
                const sjs_array *b = SJS_AS_ARRAY(v);
                for (uint32_t j = 0; j < b->length; j++) {
                    sjs_ref(b->items[j]);
                    if (array_add(vm, &out, b->items[j]) != SJS_OK) return SJS_ERR_PROGRAM;
                    call->result.as.heap = &out->h;
                }
                call->cost += b->length;
            } else {
                sjs_ref(v);
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
                    sjs_ref(b->items[j]);
                    if (array_add(vm, &out, b->items[j]) != SJS_OK) return SJS_ERR_PROGRAM;
                    call->result.as.heap = &out->h;
                }
            } else {
                sjs_ref(v);
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
                call->result = includes ? sjs_boolean(true) : sjs_num((double)i);
                return SJS_OK;
            }
        }
        call->result = includes ? sjs_boolean(false) : sjs_num(-1);
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
            sjs_ref(out->items[i]);
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
        sjs_array *out;
        if (takes_receiver(call)) {
            out = take_receiver(call);
        } else {
            if (array_slice(vm, call, (range){ 0, n }) != SJS_OK) return SJS_ERR_PROGRAM;
            out = SJS_AS_ARRAY(call->result);
        }
        sjs_value v = arg(call, 1);
        sjs_ref(v);
        sjs_unref(vm->context, out->items[(uint32_t)k]);
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
            else call->result = sjs_num(at < 0 ? -1.0 : (double)byte_to_utf16(s, (uint32_t)at));
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
    sjs_ref(element);
    sjs_result r = sjs_set_insert(vm->context, set, element);
    if (r == SJS_OK) return SJS_OK;
    sjs_unref(vm->context, element);
    return sjs_vm_fail(vm, r);
}

/* The fuel of one lookup in a set: one through its index, or under
 * SJS_INDEX_FROM members; its length when its index failed to allocate and
 * a lookup scans it. */
static uint64_t lookup_cost(const sjs_array *set) {
    return set->index != NULL || set->length < SJS_INDEX_FROM ? 1u : set->length;
}

static sjs_result set_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    const sjs_array *a = SJS_AS_ARRAY(call->receiver);
    sjs_value x = arg(call, 0);
    if (id == SJS_METHOD_SET_HAS) {
        call->result = sjs_boolean(sjs_set_contains(a, x));
        call->cost = lookup_cost(a);
        return SJS_OK;
    }
    if (x.kind != SJS_SET) return sjs_vm_fault(vm, "staticjs combines a set only with another set");
    const sjs_array *b = SJS_AS_ARRAY(x);
    /* Each member of either set is looked up at most once in the other, or
     * in the result. */
    call->cost = (uint64_t)a->length * lookup_cost(b) + (uint64_t)b->length * lookup_cost(a);
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

/* ------------------------------------------------------------------------
 * Shapes: a C path for a callback (docs/matcher.md, sections 7 and 11).
 * The matcher tagged the callback's function with what it computes; while a
 * step's preconditions hold, the loop does that in C, and otherwise calls
 * the bytecode, which gives the value or the fault. Either way the same
 * value, the same fault, and no more fuel.
 */

/* The most a C path's bytecode pushes above the callback's slots: `a.k` and
 * `b.k` (or `a` and `b`) for a comparator, `x.k` for a pluck, `x.k` (or
 * `x`) and the literal, in either order, for a predicate, and the
 * accumulator and `x.k` (or `x`), in either order, for a sum. A C step needs
 * that much room, so that it succeeds only where the bytecode would not run
 * out of max_stack. More would only fall back more often; less would be
 * wrong. */
#define PLUCK_STACK 1u
#define COMPARE_STACK 2u
#define PREDICATE_STACK 2u
#define SUM_STACK 2u

/* The shapes each loop built-in runs in C (docs/matcher.md, 7 and 11.5). */
static const struct {
    uint16_t builtin;
    uint8_t shape;
} shape_runners[] = {
    { SJS_METHOD_ARRAY_MAP, SJS_SHAPE_PLUCK },
    { SJS_METHOD_ARRAY_TO_SORTED, SJS_SHAPE_COMPARE_BY_KEY },
    { SJS_METHOD_ARRAY_TO_SORTED, SJS_SHAPE_COMPARE_NUMERIC },
    { SJS_METHOD_ARRAY_FILTER, SJS_SHAPE_KEY_COMPARE },
    { SJS_METHOD_ARRAY_FILTER, SJS_SHAPE_ELEMENT_COMPARE },
    { SJS_METHOD_ARRAY_FIND, SJS_SHAPE_KEY_COMPARE },
    { SJS_METHOD_ARRAY_FIND, SJS_SHAPE_ELEMENT_COMPARE },
    { SJS_METHOD_ARRAY_FIND_INDEX, SJS_SHAPE_KEY_COMPARE },
    { SJS_METHOD_ARRAY_FIND_INDEX, SJS_SHAPE_ELEMENT_COMPARE },
    { SJS_METHOD_ARRAY_FIND_LAST, SJS_SHAPE_KEY_COMPARE },
    { SJS_METHOD_ARRAY_FIND_LAST, SJS_SHAPE_ELEMENT_COMPARE },
    { SJS_METHOD_ARRAY_SOME, SJS_SHAPE_KEY_COMPARE },
    { SJS_METHOD_ARRAY_SOME, SJS_SHAPE_ELEMENT_COMPARE },
    { SJS_METHOD_ARRAY_EVERY, SJS_SHAPE_KEY_COMPARE },
    { SJS_METHOD_ARRAY_EVERY, SJS_SHAPE_ELEMENT_COMPARE },
    { SJS_METHOD_ARRAY_REDUCE, SJS_SHAPE_SUM },
    { SJS_METHOD_ARRAY_REDUCE, SJS_SHAPE_SUM_KEY },
};

/* A key the C path reads as an object's own property: one an object lacks
 * reads as undefined, unless JavaScript would find it on Object.prototype,
 * where staticjs faults. */
static bool key_on_prototype(const sjs_string *key) {
    const sjs_name *n = sjs_name_of(key);
    return n != NULL && (n->flags & SJS_NAME_OBJECT) != 0;
}

/* A predicate's literal (sjs_shape_operand): the value the literal's own
 * instruction pushes, borrowed from the program. */
static sjs_value operand_value(const sjs_program *program, uint32_t operand) {
    uint32_t index = sjs_shape_operand_index(operand);
    switch (sjs_shape_operand_kind(operand)) {
    case SJS_OPERAND_NUMBER: return sjs_num(program->numbers[index]);
    case SJS_OPERAND_TEXT: return program->constants[index];
    case SJS_OPERAND_TRUE: return sjs_boolean(true);
    case SJS_OPERAND_FALSE: return sjs_boolean(false);
    default: return sjs_null();
    }
}

/* KEY_COMPARE's and ELEMENT_COMPARE's operator and literal, from argument
 * `at` on. False for an order (<, <=, >, >=) against true, false or null:
 * staticjs faults on every such comparison, so the bytecode runs the loop
 * and faults at its first element. */
static bool predicate_start(vm_frame *frame, const sjs_closure *closure, uint32_t at) {
    const sjs_function_info *fn = &closure->program->functions[closure->function];
    uint32_t op = fn->shape_args[at];
    uint32_t operand = fn->shape_args[at + 1u];
    uint32_t kind = sjs_shape_operand_kind(operand);
    bool equality = op == SJS_OP_STRICT_EQUAL || op == SJS_OP_STRICT_NOT_EQUAL;
    if (!equality && kind != SJS_OPERAND_NUMBER && kind != SJS_OPERAND_TEXT) return false;
    frame->shape_op = (uint8_t)op;
    frame->shape_value = operand_value(closure->program, operand);
    return true;
}

/* Note the callback's shape when this built-in runs it in C: a closure the
 * matcher tagged with a shape shape_runners pairs with the built-in. Its
 * key, for the shapes that have one; its flag: for PLUCK and KEY_COMPARE,
 * the key is on Object.prototype; for the comparators, descending; for the
 * sums, the element is the left operand; and a predicate's operator and
 * literal. */
static void shape_start(vm_frame *frame, sjs_value callback) {
    frame->shape = SJS_SHAPE_NONE;
    if (callback.as.heap->type != SJS_HEAP_CLOSURE) return;
    const sjs_closure *closure = SJS_AS_CLOSURE(callback);
    const sjs_function_info *fn = &closure->program->functions[closure->function];
    if (fn->shape == SJS_SHAPE_NONE) return;
    uint32_t count = (uint32_t)(sizeof shape_runners / sizeof shape_runners[0]);
    uint32_t i = 0;
    while (i < count && (shape_runners[i].builtin != frame->builtin || shape_runners[i].shape != fn->shape)) i++;
    if (i == count) return;
    frame->shape = (uint8_t)fn->shape;
    frame->shape_guess = 0;
    switch (fn->shape) {
    case SJS_SHAPE_PLUCK:
    case SJS_SHAPE_KEY_COMPARE:
        frame->shape_key = SJS_AS_STRING(closure->program->constants[fn->shape_args[0]]);
        frame->shape_flag = key_on_prototype(frame->shape_key);
        if (fn->shape == SJS_SHAPE_KEY_COMPARE && !predicate_start(frame, closure, 1)) frame->shape = SJS_SHAPE_NONE;
        break;
    case SJS_SHAPE_ELEMENT_COMPARE:
        if (!predicate_start(frame, closure, 0)) frame->shape = SJS_SHAPE_NONE;
        break;
    case SJS_SHAPE_COMPARE_BY_KEY:
    case SJS_SHAPE_SUM_KEY:
        frame->shape_key = SJS_AS_STRING(closure->program->constants[fn->shape_args[0]]);
        frame->shape_flag = fn->shape_args[1] != 0;
        break;
    default: /* SJS_SHAPE_COMPARE_NUMERIC, SJS_SHAPE_SUM */
        frame->shape_flag = fn->shape_args[0] != 0;
        break;
    }
}

/* The entry an object holds for the shape's key, found as GET_MEMBER finds
 * it: where the last object had it, then through the index or a scan,
 * comparing pointers before bytes. NULL when it has none. */
static const sjs_entry *own_entry(vm_frame *frame, const sjs_object *o) {
    const sjs_string *key = frame->shape_key;
    sjs_str name = sjs_string_view(key);
    uint32_t guess = frame->shape_guess;
    if (guess < o->count) {
        const sjs_string *k = o->entries[guess].key;
        if (k == key || sjs_str_equal(sjs_string_view(k), name)) return &o->entries[guess];
    }
    const sjs_entry *e = NULL;
    if (o->index != NULL) {
        e = sjs_object_find(o, name);
    } else {
        for (uint32_t i = 0; i < o->count; i++) {
            const sjs_string *k = o->entries[i].key;
            if (k == key || sjs_str_equal(sjs_string_view(k), name)) {
                e = &o->entries[i];
                break;
            }
        }
    }
    if (e != NULL) frame->shape_guess = (uint32_t)(e - o->entries);
    return e;
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
    frame->entered = false;
    shape_start(frame, callback);
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
            /* Moved, not copied: LOOP_SECOND is not read again. */
            sjs_value *second = slot(vm, frame, LOOP_SECOND);
            sjs_value initial = *second;
            *second = sjs_undefined();
            return set_output(vm, frame, initial);
        }
        if (a->length == 0) return sjs_vm_fault(vm, "reduce of an empty array needs an initial value");
        sjs_ref(a->items[0]);
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

/* Call the callback with the arguments JavaScript passes; the evaluator
 * passes a closure only those it has parameters for. */
static loop_step call_back(sjs_vm *vm, vm_frame *frame, sjs_values values) {
    frame->phase = 1;
    return sjs_vm_call_back(vm, frame, values) ? LOOP_CALL : LOOP_FAULT;
}

static loop_step call_element(sjs_vm *vm, vm_frame *frame, uint32_t i) {
    const sjs_array *a = receiver(vm, frame);
    sjs_value args[3] = { a->items[i], sjs_num((double)i), *slot(vm, frame, LOOP_RECEIVER) };
    return call_back(vm, frame, (sjs_values){ args, 3 });
}

static loop_step done(sjs_vm *vm, vm_frame *frame, sjs_value result) {
    set_output(vm, frame, result);
    return LOOP_DONE;
}

/* map's step for a PLUCK callback, `x => x.key`: an object's own property,
 * or undefined for a key it lacks that is not on Object.prototype, as
 * TAKE_LOCAL, GET_MEMBER and RETURN would give it, for one unit of fuel
 * where they cost three. Anything else calls the bytecode. */
static loop_step pluck_step(sjs_vm *vm, vm_frame *frame) {
    if (!frame->entered && !sjs_vm_enter_callback(vm, frame)) return LOOP_FAULT;
    sjs_value x = receiver(vm, frame)->items[frame->index];
    if (x.kind == SJS_OBJECT && vm->stack_capacity - vm->sp >= PLUCK_STACK) {
        const sjs_entry *e = own_entry(frame, SJS_AS_OBJECT(x));
        if (e != NULL || !frame->shape_flag) {
            sjs_value v = e != NULL ? e->value : (sjs_value){ .kind = SJS_UNDEFINED };
            sjs_ref(v);
            sjs_array *out = SJS_AS_ARRAY(*slot(vm, frame, LOOP_OUTPUT));
            out->items[out->length++] = v;
            frame->index++;
            vm->fuel_used += 1;
            return LOOP_AGAIN;
        }
    }
    return call_element(vm, frame, frame->index);
}

/* What filter, find, findIndex, findLast, some and every make of the
 * callback's answer for the element at frame->index (from the end, for
 * findLast), which it moves past: keep it, or finish with true, false, the
 * element or its index. LOOP_AGAIN to go on to the next element. The
 * bytecode's answer and the C path's go through here alike. */
static loop_step predicate_answer(sjs_vm *vm, vm_frame *frame, bool truthy) {
    const sjs_array *a = receiver(vm, frame);
    uint32_t i = frame->index;
    uint32_t element = frame->builtin == SJS_METHOD_ARRAY_FIND_LAST ? frame->length - 1u - i : i;
    frame->index++;
    if (!truthy) {
        return frame->builtin == SJS_METHOD_ARRAY_EVERY ? done(vm, frame, sjs_boolean(false)) : LOOP_AGAIN;
    }
    switch (frame->builtin) {
    case SJS_METHOD_ARRAY_FILTER: {
        sjs_array *out = SJS_AS_ARRAY(*slot(vm, frame, LOOP_OUTPUT));
        sjs_ref(a->items[i]);
        if (array_add(vm, &out, a->items[i]) != SJS_OK) return LOOP_FAULT;
        slot(vm, frame, LOOP_OUTPUT)->as.heap = &out->h;
        return LOOP_AGAIN;
    }
    case SJS_METHOD_ARRAY_SOME:
        return done(vm, frame, sjs_boolean(true));
    case SJS_METHOD_ARRAY_EVERY:
        return LOOP_AGAIN;
    case SJS_METHOD_ARRAY_FIND_INDEX:
        return done(vm, frame, sjs_num((double)element));
    default: /* FIND, FIND_LAST */
        sjs_ref(a->items[element]);
        return done(vm, frame, a->items[element]);
    }
}

/* A KEY_COMPARE or ELEMENT_COMPARE callback's answer for `v`, the element
 * or its key's value, against the literal: the bytecode's own comparison,
 * so 1 or 0; or SJS_VM_UNDECIDED where the bytecode faults. The rules
 * record `c OP x` as `x OP' c`, which is the same comparison (docs/matcher.md,
 * 11.7.2). */
static int predicate_compare(const vm_frame *frame, sjs_value v) {
    switch (frame->shape_op) {
    case SJS_OP_STRICT_EQUAL:
        return sjs_vm_strict_equal(v, frame->shape_value);
    case SJS_OP_STRICT_NOT_EQUAL: {
        /* The literal is a primitive, so this never fails. */
        int equal = sjs_vm_strict_equal(v, frame->shape_value);
        return equal == SJS_VM_UNDECIDED ? equal : !equal;
    }
    default:
        return sjs_vm_order(frame->shape_op, v, frame->shape_value);
    }
}

/* The step of a predicate loop whose callback is a KEY_COMPARE, `x => x.key
 * OP c`, or an ELEMENT_COMPARE, `x => x OP c`: the element decided in C, as
 * TAKE_LOCAL, GET_MEMBER, the literal, BINARY and RETURN would decide it,
 * for one unit of fuel and one per key looked up, where they cost five or
 * four more. An element that is not an object for a key, a key missing that
 * is on Object.prototype, and a comparison the bytecode faults on call the
 * bytecode for that element, which faults. */
static loop_step predicate_step(sjs_vm *vm, vm_frame *frame) {
    if (!frame->entered && !sjs_vm_enter_callback(vm, frame)) return LOOP_FAULT;
    uint32_t element = frame->builtin == SJS_METHOD_ARRAY_FIND_LAST ? frame->length - 1u - frame->index : frame->index;
    sjs_value v = receiver(vm, frame)->items[element];
    if (vm->stack_capacity - vm->sp < PREDICATE_STACK) return call_element(vm, frame, element);
    uint64_t lookups = 0;
    if (frame->shape == SJS_SHAPE_KEY_COMPARE) {
        if (v.kind != SJS_OBJECT) return call_element(vm, frame, element);
        const sjs_entry *e = own_entry(frame, SJS_AS_OBJECT(v));
        if (e == NULL && frame->shape_flag) return call_element(vm, frame, element);
        v = e != NULL ? e->value : sjs_undefined();
        lookups = 1;
    }
    int answer = predicate_compare(frame, v);
    if (answer == SJS_VM_UNDECIDED) return call_element(vm, frame, element);
    vm->fuel_used += lookups;
    return predicate_answer(vm, frame, answer != 0);
}

/* reduce's step for a SUM callback, `(a, x) => a + x` or `x + a`, or a
 * SUM_KEY, `(a, x) => a + x.key` or `x.key + a`, in C: when the accumulator
 * is a number and so is the element, or the element's own property, the
 * accumulator becomes the double the bytecode's addition gives, with the
 * operands in its order, in place (a number holds no reference). One unit of
 * fuel per key looked up beyond the step's, where the bytecode costs four
 * or five. False otherwise: the bytecode adds this element, and
 * concatenates or faults. */
static bool sum_step(sjs_vm *vm, vm_frame *frame) {
    sjs_value *output = slot(vm, frame, LOOP_OUTPUT);
    if (output->kind != SJS_NUMBER) return false;
    if (vm->stack_capacity - vm->sp < SUM_STACK) return false;
    sjs_value v = receiver(vm, frame)->items[frame->index];
    uint64_t lookups = 0;
    if (frame->shape == SJS_SHAPE_SUM_KEY) {
        if (v.kind != SJS_OBJECT) return false;
        /* A missing key falls back whether or not it is on Object.prototype:
         * the bytecode faults either way. */
        const sjs_entry *e = own_entry(frame, SJS_AS_OBJECT(v));
        if (e == NULL) return false;
        v = e->value;
        lookups = 1;
    }
    if (v.kind != SJS_NUMBER) return false;
    double acc = output->as.number;
    double x = v.as.number;
    *output = sjs_num(frame->shape_flag ? x + acc : acc + x);
    frame->index++;
    vm->fuel_used += lookups;
    return true;
}

/* Merge toSorted's next element: from the right run when the comparator
 * gave more than 0, else from the left (NaN and 0 included), which keeps
 * the sort stable. The bytecode's answer and the C path's go through here
 * alike. */
static void sort_take(vm_frame *frame, double d) {
    uint32_t n = frame->length;
    sjs_value *src = frame->scratch + (frame->index ? n : 0);
    sjs_value *dst = frame->scratch + (frame->index ? 0 : n);
    if (d > 0) dst[frame->k++] = src[frame->j++];
    else dst[frame->k++] = src[frame->i++];
}

/* toSorted's comparison for a COMPARE_BY_KEY callback, `(a, b) => a.key -
 * b.key` or `b.key - a.key`, in C: when both elements are objects that
 * hold the key, and both values are numbers, the difference is the double
 * the bytecode's subtraction gives, with the same operands in the same
 * order. Two units of fuel, where the bytecode costs six. False when a
 * precondition fails: the bytecode compares, and faults where it does. */
static bool sort_by_key(sjs_vm *vm, vm_frame *frame, const sjs_value *pair) {
    if (pair[0].kind != SJS_OBJECT || pair[1].kind != SJS_OBJECT) return false;
    if (vm->stack_capacity - vm->sp < COMPARE_STACK) return false;
    const sjs_entry *a = own_entry(frame, SJS_AS_OBJECT(pair[0]));
    if (a == NULL || a->value.kind != SJS_NUMBER) return false;
    const sjs_entry *b = own_entry(frame, SJS_AS_OBJECT(pair[1]));
    if (b == NULL || b->value.kind != SJS_NUMBER) return false;
    double x = a->value.as.number;
    double y = b->value.as.number;
    sort_take(frame, frame->shape_flag ? y - x : x - y);
    vm->fuel_used += 2;
    return true;
}

/* toSorted's comparison for a COMPARE_NUMERIC callback, `(a, b) => a - b`
 * or `b - a`, in C: when both elements are numbers, the difference is the
 * double the bytecode's subtraction gives, with the same operands in the
 * same order. No fuel beyond the step's, where the bytecode costs four.
 * False otherwise: the bytecode subtracts, and faults. */
static bool sort_numbers(sjs_vm *vm, vm_frame *frame, const sjs_value *pair) {
    if (pair[0].kind != SJS_NUMBER || pair[1].kind != SJS_NUMBER) return false;
    if (vm->stack_capacity - vm->sp < COMPARE_STACK) return false;
    double x = pair[0].as.number;
    double y = pair[1].as.number;
    sort_take(frame, frame->shape_flag ? y - x : x - y);
    return true;
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
            sort_take(frame, d);
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
                if (frame->shape != SJS_SHAPE_NONE) {
                    /* Entered where the bytecode's first call would enter
                     * it; one comparison per step, so fuel still pauses. */
                    if (!frame->entered && !sjs_vm_enter_callback(vm, frame)) return LOOP_FAULT;
                    bool fast = frame->shape == SJS_SHAPE_COMPARE_BY_KEY ? sort_by_key(vm, frame, args)
                                                                         : sort_numbers(vm, frame, args);
                    if (fast) return LOOP_AGAIN;
                }
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
            /* A receiver nothing else holds takes the order itself: the same
             * elements, so no count changes. The comparator never saw it. */
            sjs_value *held = slot(vm, frame, LOOP_RECEIVER);
            if (sjs_value_unique(*held)) {
                sjs_value sorted = *held;
                *held = sjs_undefined();
                sjs_copy(SJS_AS_ARRAY(sorted)->items, src, (size_t)n * sizeof(sjs_value));
                return done(vm, frame, sorted);
            }
            sjs_array *out;
            if (new_array(vm, n, &out) != SJS_OK) return LOOP_FAULT;
            for (uint32_t i = 0; i < n; i++) {
                out->items[i] = src[i];
                sjs_ref(src[i]);
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
        switch (frame->builtin) {
        case SJS_METHOD_ARRAY_MAP: {
            sjs_array *out = SJS_AS_ARRAY(*slot(vm, frame, LOOP_OUTPUT));
            out->items[out->length++] = r;
            frame->index++;
            break;
        }
        case SJS_METHOD_ARRAY_FLAT_MAP: {
            frame->index++;
            sjs_array *out = SJS_AS_ARRAY(*slot(vm, frame, LOOP_OUTPUT));
            if (r.kind == SJS_ARRAY) {
                const sjs_array *b = SJS_AS_ARRAY(r);
                for (uint32_t j = 0; j < b->length; j++) {
                    sjs_ref(b->items[j]);
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
            frame->index++;
            break;
        default: { /* the predicate loops */
            bool truthy = sjs_truthy(r);
            sjs_vm_release(vm, r);
            loop_step step = predicate_answer(vm, frame, truthy);
            if (step != LOOP_AGAIN) return step;
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
            return done(vm, frame, sjs_num(-1));
        case SJS_METHOD_ARRAY_FIND:
        case SJS_METHOD_ARRAY_FIND_LAST:
            return done(vm, frame, sjs_undefined());
        default:
            return LOOP_DONE;
        }
    }
    if (frame->builtin == SJS_METHOD_ARRAY_REDUCE) {
        if (frame->shape != SJS_SHAPE_NONE) {
            /* Entered where the bytecode's first call would enter it; one
             * element per step, so fuel still pauses. */
            if (!frame->entered && !sjs_vm_enter_callback(vm, frame)) return LOOP_FAULT;
            if (sum_step(vm, frame)) return LOOP_AGAIN;
        }
        /* The accumulator moves into the call, so that the callback can hold
         * it alone; its result comes back into LOOP_OUTPUT. If the call
         * cannot be pushed, the release below frees it. */
        sjs_value *output = slot(vm, frame, LOOP_OUTPUT);
        sjs_value acc = *output;
        *output = sjs_undefined();
        sjs_value args[4] = { acc, a->items[frame->index], sjs_num((double)frame->index), *slot(vm, frame, LOOP_RECEIVER) };
        loop_step step = call_back(vm, frame, (sjs_values){ args, 4 });
        sjs_vm_release(vm, acc);
        return step;
    }
    if (frame->shape == SJS_SHAPE_PLUCK) return pluck_step(vm, frame);
    if (frame->shape == SJS_SHAPE_KEY_COMPARE || frame->shape == SJS_SHAPE_ELEMENT_COMPARE) return predicate_step(vm, frame);
    uint32_t element = frame->builtin == SJS_METHOD_ARRAY_FIND_LAST ? frame->length - 1u - frame->index : frame->index;
    return call_element(vm, frame, element);
}
