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

static const sjs_function_entry FUNCTION_TABLE[] = {
    { "", "String", SJS_BUILTIN_STRING },
    { "String", "fromCharCode", SJS_BUILTIN_STRING_FROM_CHAR_CODE },
    { "String", "fromCodePoint", SJS_BUILTIN_STRING_FROM_CODE_POINT },
    { "String", "raw", SJS_BUILTIN_STRING_RAW },
    { "", "Boolean", SJS_BUILTIN_BOOLEAN },
    { "Array", "from", SJS_BUILTIN_ARRAY_FROM },
    { "Array", "isArray", SJS_BUILTIN_ARRAY_IS_ARRAY },
    { "Array", "of", SJS_BUILTIN_ARRAY_OF },
    { "Object", "keys", SJS_BUILTIN_OBJECT_KEYS },
    { "Object", "values", SJS_BUILTIN_OBJECT_VALUES },
    { "Object", "entries", SJS_BUILTIN_OBJECT_ENTRIES },
    { "Object", "fromEntries", SJS_BUILTIN_OBJECT_FROM_ENTRIES },
    { "Object", "groupBy", SJS_BUILTIN_OBJECT_GROUP_BY },
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
    { "Math", "sign", SJS_BUILTIN_MATH_SIGN },
    { "", "Number", SJS_BUILTIN_NUMBER },
    { "", "parseInt", SJS_BUILTIN_PARSE_INT },
    { "", "parseFloat", SJS_BUILTIN_PARSE_FLOAT },
    { "Number", "parseInt", SJS_BUILTIN_PARSE_INT },
    { "Number", "parseFloat", SJS_BUILTIN_PARSE_FLOAT },
    { "", "isNaN", SJS_BUILTIN_IS_NAN },
    { "", "isFinite", SJS_BUILTIN_IS_FINITE },
    { "Math", "sqrt", SJS_BUILTIN_MATH_SQRT },
    { "Math", "hypot", SJS_BUILTIN_MATH_HYPOT },
    { "Math", "fround", SJS_BUILTIN_MATH_FROUND },
    { "Math", "clz32", SJS_BUILTIN_MATH_CLZ32 },
    { "Math", "imul", SJS_BUILTIN_MATH_IMUL },
    { "", "BigInt", SJS_BUILTIN_BIGINT },
    { "BigInt", "asIntN", SJS_BUILTIN_BIGINT_AS_INT_N },
    { "BigInt", "asUintN", SJS_BUILTIN_BIGINT_AS_UINT_N },
    { "Map", "groupBy", SJS_BUILTIN_MAP_GROUP_BY },
    { "JSON", "parse", SJS_BUILTIN_JSON_PARSE },
    { "JSON", "stringify", SJS_BUILTIN_JSON_STRINGIFY },
    { "", "encodeURI", SJS_BUILTIN_ENCODE_URI },
    { "", "encodeURIComponent", SJS_BUILTIN_ENCODE_URI_COMPONENT },
    { "", "decodeURI", SJS_BUILTIN_DECODE_URI },
    { "", "decodeURIComponent", SJS_BUILTIN_DECODE_URI_COMPONENT }
};

/*
 * Every name JavaScript finds on the prototype of an array, a string or a
 * set (those of Node 22, not only the methods staticjs has, so that reading
 * one faults instead of giving undefined), on Object.prototype, or that
 * names a method changing its receiver; with the staticjs method each kind
 * has by that name. Sorted by bytes, for sjs_name_id's binary search.
 */
#define NAME(text, flags, array, string, set) { { text, sizeof(text) - 1u }, flags, { array, string, set, 0, 0, 0, 0 }, 0 }
/* A name with a BigInt method too: toString. */
#define NAME_BIGINT(text, flags, bigint) { { text, sizeof(text) - 1u }, flags, { 0, 0, 0, bigint, 0, 0, 0 }, 0 }
/* A name with a map's method: on a set too, and maps'. */
#define NAME_MAP(text, flags, set, map) { { text, sizeof(text) - 1u }, flags, { 0, 0, set, 0, map, 0, 0 }, 0 }
/* A name with an iterator's method: on an array too, and iterators'. */
#define NAME_ITERATOR(text, flags, array, iterator) { { text, sizeof(text) - 1u }, flags, { array, 0, 0, 0, 0, iterator, 0 }, 0 }
/* A name with a map's method and an iterator's: forEach. */
#define NAME_MAP_ITERATOR(text, flags, map, iterator) { { text, sizeof(text) - 1u }, flags, { 0, 0, 0, 0, map, iterator, 0 }, 0 }
/* A name on RegExp.prototype, with the regular expressions' method of the
 * name (docs/regex.md), or 0. */
#define NAME_REGEXP(text, regexp) { { text, sizeof(text) - 1u }, SJS_NAME_ON_REGEXP, { 0, 0, 0, 0, 0, 0, regexp }, 0 }
/* MUTATION: a name with a method that changes its receiver. Restore: NAME(text, flags, 0, 0, 0). */
#define NAME_MUTATION(text, flags, mutation) { { text, sizeof(text) - 1u }, flags, { 0, 0, 0, 0, 0, 0, 0 }, mutation }
/* A name of Object.prototype, which every kind of value's prototype holds. */
#define SJS_NAME_EVERYWHERE (SJS_NAME_OBJECT | SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING | SJS_NAME_ON_SET | SJS_NAME_ON_MAP | SJS_NAME_ON_ITERATOR | SJS_NAME_ON_REGEXP)

static const sjs_name NAMES[] = {
    NAME("__defineGetter__", SJS_NAME_EVERYWHERE, 0, 0, 0),
    NAME("__defineSetter__", SJS_NAME_EVERYWHERE, 0, 0, 0),
    NAME("__lookupGetter__", SJS_NAME_EVERYWHERE, 0, 0, 0),
    NAME("__lookupSetter__", SJS_NAME_EVERYWHERE, 0, 0, 0),
    NAME("__proto__", SJS_NAME_EVERYWHERE, 0, 0, 0),
    NAME_MUTATION("add", SJS_NAME_ON_SET | SJS_NAME_MUTATING, SJS_METHOD_SET_ADD), /* MUTATION */
    NAME("anchor", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("at", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, SJS_METHOD_ARRAY_AT, SJS_METHOD_STRING_AT, 0),
    NAME("big", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("blink", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("bold", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("charAt", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_CHAR_AT, 0),
    NAME("charCodeAt", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_CHAR_CODE_AT, 0),
    NAME_MUTATION("clear", SJS_NAME_ON_SET | SJS_NAME_ON_MAP | SJS_NAME_MUTATING, SJS_METHOD_SET_CLEAR), /* MUTATION */
    NAME("codePointAt", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_CODE_POINT_AT, 0),
    NAME_REGEXP("compile", 0),
    NAME("concat", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, SJS_METHOD_ARRAY_CONCAT, SJS_METHOD_STRING_CONCAT, 0),
    NAME("constructor", SJS_NAME_EVERYWHERE, 0, 0, 0),
    NAME("copyWithin", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, 0, 0, 0),
    NAME_MUTATION("delete", SJS_NAME_ON_SET | SJS_NAME_ON_MAP | SJS_NAME_MUTATING, SJS_METHOD_SET_DELETE), /* MUTATION */
    NAME("difference", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_DIFFERENCE),
    NAME_REGEXP("dotAll", 0),
    NAME_ITERATOR("drop", SJS_NAME_ON_ITERATOR, 0, SJS_METHOD_ITERATOR_DROP),
    NAME("endsWith", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_ENDS_WITH, 0),
    NAME_MAP("entries", SJS_NAME_ON_ARRAY | SJS_NAME_ON_SET | SJS_NAME_ON_MAP, 0, SJS_METHOD_MAP_ENTRIES),
    NAME_ITERATOR("every", SJS_NAME_ON_ARRAY | SJS_NAME_ON_ITERATOR, SJS_METHOD_ARRAY_EVERY, SJS_METHOD_ITERATOR_EVERY),
    NAME_REGEXP("exec", SJS_METHOD_REGEXP_EXEC),
    NAME_MUTATION("fill", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, SJS_METHOD_ARRAY_FILL), /* MUTATION */
    NAME_ITERATOR("filter", SJS_NAME_ON_ARRAY | SJS_NAME_ON_ITERATOR, SJS_METHOD_ARRAY_FILTER, SJS_METHOD_ITERATOR_FILTER),
    NAME_ITERATOR("find", SJS_NAME_ON_ARRAY | SJS_NAME_ON_ITERATOR, SJS_METHOD_ARRAY_FIND, SJS_METHOD_ITERATOR_FIND),
    NAME("findIndex", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_FIND_INDEX, 0, 0),
    NAME("findLast", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_FIND_LAST, 0, 0),
    NAME("findLastIndex", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_FIND_LAST_INDEX, 0, 0),
    NAME("fixed", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME_REGEXP("flags", 0),
    NAME("flat", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_FLAT, 0, 0),
    NAME_ITERATOR("flatMap", SJS_NAME_ON_ARRAY | SJS_NAME_ON_ITERATOR, SJS_METHOD_ARRAY_FLAT_MAP, SJS_METHOD_ITERATOR_FLAT_MAP),
    NAME("fontcolor", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("fontsize", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME_MAP_ITERATOR("forEach", SJS_NAME_ON_ARRAY | SJS_NAME_ON_SET | SJS_NAME_ON_MAP | SJS_NAME_ON_ITERATOR, SJS_METHOD_MAP_FOR_EACH, SJS_METHOD_ITERATOR_FOR_EACH),
    NAME_MAP("get", SJS_NAME_ON_MAP, 0, SJS_METHOD_MAP_GET),
    NAME_REGEXP("global", 0),
    NAME_MAP("has", SJS_NAME_ON_SET | SJS_NAME_ON_MAP, SJS_METHOD_SET_HAS, SJS_METHOD_MAP_HAS),
    NAME_REGEXP("hasIndices", 0),
    NAME("hasOwnProperty", SJS_NAME_EVERYWHERE, 0, 0, 0),
    NAME_REGEXP("ignoreCase", 0),
    NAME("includes", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, SJS_METHOD_ARRAY_INCLUDES, SJS_METHOD_STRING_INCLUDES, 0),
    NAME("indexOf", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, SJS_METHOD_ARRAY_INDEX_OF, SJS_METHOD_STRING_INDEX_OF, 0),
    NAME("intersection", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_INTERSECTION),
    NAME("isDisjointFrom", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_IS_DISJOINT_FROM),
    NAME("isPrototypeOf", SJS_NAME_EVERYWHERE, 0, 0, 0),
    NAME("isSubsetOf", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_IS_SUBSET_OF),
    NAME("isSupersetOf", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_IS_SUPERSET_OF),
    NAME("isWellFormed", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_IS_WELL_FORMED, 0),
    NAME("italics", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("join", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_JOIN, 0, 0),
    NAME_MAP("keys", SJS_NAME_ON_ARRAY | SJS_NAME_ON_SET | SJS_NAME_ON_MAP, 0, SJS_METHOD_MAP_KEYS),
    NAME("lastIndexOf", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, SJS_METHOD_ARRAY_LAST_INDEX_OF, 0, 0),
    NAME("link", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("localeCompare", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME_ITERATOR("map", SJS_NAME_ON_ARRAY | SJS_NAME_ON_ITERATOR, SJS_METHOD_ARRAY_MAP, SJS_METHOD_ITERATOR_MAP),
    NAME("match", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_MATCH, 0),
    NAME("matchAll", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_MATCH_ALL, 0),
    NAME_REGEXP("multiline", 0),
    NAME("next", SJS_NAME_ON_ITERATOR, 0, 0, 0),
    NAME("normalize", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("padEnd", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_PAD_END, 0),
    NAME("padStart", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_PAD_START, 0),
    NAME_MUTATION("pop", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, SJS_METHOD_ARRAY_POP), /* MUTATION */
    NAME("propertyIsEnumerable", SJS_NAME_EVERYWHERE, 0, 0, 0),
    NAME_MUTATION("push", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, SJS_METHOD_ARRAY_PUSH), /* MUTATION */
    NAME_ITERATOR("reduce", SJS_NAME_ON_ARRAY | SJS_NAME_ON_ITERATOR, SJS_METHOD_ARRAY_REDUCE, SJS_METHOD_ITERATOR_REDUCE),
    NAME("reduceRight", SJS_NAME_ON_ARRAY, 0, 0, 0),
    NAME("repeat", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_REPEAT, 0),
    NAME("replace", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_REPLACE, 0),
    NAME("replaceAll", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_REPLACE_ALL, 0),
    NAME("return", SJS_NAME_ON_ITERATOR, 0, 0, 0),
    NAME_MUTATION("reverse", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, SJS_METHOD_ARRAY_REVERSE), /* MUTATION */
    NAME("search", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_SEARCH, 0),
    NAME_MUTATION("set", SJS_NAME_ON_MAP | SJS_NAME_MUTATING, SJS_METHOD_MAP_SET), /* MUTATION */
    NAME_MUTATION("shift", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, SJS_METHOD_ARRAY_SHIFT), /* MUTATION */
    NAME("slice", SJS_NAME_ON_ARRAY | SJS_NAME_ON_STRING, SJS_METHOD_ARRAY_SLICE, SJS_METHOD_STRING_SLICE, 0),
    NAME("small", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME_ITERATOR("some", SJS_NAME_ON_ARRAY | SJS_NAME_ON_ITERATOR, SJS_METHOD_ARRAY_SOME, SJS_METHOD_ITERATOR_SOME),
    NAME_MUTATION("sort", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, SJS_METHOD_ARRAY_SORT), /* MUTATION */
    NAME_REGEXP("source", 0),
    NAME_MUTATION("splice", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, SJS_METHOD_ARRAY_SPLICE), /* MUTATION */
    NAME("split", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_SPLIT, 0),
    NAME("startsWith", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_STARTS_WITH, 0),
    NAME_REGEXP("sticky", 0),
    NAME("strike", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("sub", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("substr", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_SUBSTR, 0),
    NAME("substring", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_SUBSTRING, 0),
    NAME("sup", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("symmetricDifference", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_SYMMETRIC_DIFFERENCE),
    NAME_ITERATOR("take", SJS_NAME_ON_ITERATOR, 0, SJS_METHOD_ITERATOR_TAKE),
    NAME_REGEXP("test", SJS_METHOD_REGEXP_TEST),
    NAME_ITERATOR("toArray", SJS_NAME_ON_ITERATOR, 0, SJS_METHOD_ITERATOR_TO_ARRAY),
    NAME("toLocaleLowerCase", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("toLocaleString", SJS_NAME_EVERYWHERE, 0, 0, 0),
    NAME("toLocaleUpperCase", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("toLowerCase", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_TO_LOWER_CASE, 0),
    NAME("toReversed", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_TO_REVERSED, 0, 0),
    NAME("toSorted", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_TO_SORTED, 0, 0),
    NAME("toSpliced", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_TO_SPLICED, 0, 0),
    NAME_BIGINT("toString", SJS_NAME_EVERYWHERE, SJS_METHOD_BIGINT_TO_STRING),
    NAME("toUpperCase", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_TO_UPPER_CASE, 0),
    NAME("toWellFormed", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_TO_WELL_FORMED, 0),
    NAME("trim", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_TRIM, 0),
    NAME("trimEnd", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_TRIM_END, 0),
    NAME("trimLeft", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("trimRight", SJS_NAME_ON_STRING, 0, 0, 0),
    NAME("trimStart", SJS_NAME_ON_STRING, 0, SJS_METHOD_STRING_TRIM_START, 0),
    NAME_REGEXP("unicode", 0),
    NAME_REGEXP("unicodeSets", 0),
    NAME("union", SJS_NAME_ON_SET, 0, 0, SJS_METHOD_SET_UNION),
    NAME_MUTATION("unshift", SJS_NAME_ON_ARRAY | SJS_NAME_MUTATING, SJS_METHOD_ARRAY_UNSHIFT), /* MUTATION */
    NAME("valueOf", SJS_NAME_EVERYWHERE, 0, 0, 0),
    NAME_MAP("values", SJS_NAME_ON_ARRAY | SJS_NAME_ON_SET | SJS_NAME_ON_MAP, 0, SJS_METHOD_MAP_VALUES),
    NAME("with", SJS_NAME_ON_ARRAY, SJS_METHOD_ARRAY_WITH, 0, 0)
};

#define FUNCTION_VALUE(id) [id] = { { SJS_IMMORTAL, SJS_HEAP_BUILTIN, 0, 0, sizeof(sjs_builtin_function), NULL }, id }

/* Immortal: their reference counts are never touched, so they are never
 * written. */
static const sjs_builtin_function FUNCTIONS[SJS_BUILTIN_FUNCTION_COUNT] = {
    FUNCTION_VALUE(SJS_BUILTIN_STRING),
    FUNCTION_VALUE(SJS_BUILTIN_STRING_FROM_CHAR_CODE),
    FUNCTION_VALUE(SJS_BUILTIN_STRING_FROM_CODE_POINT),
    FUNCTION_VALUE(SJS_BUILTIN_STRING_RAW),
    FUNCTION_VALUE(SJS_BUILTIN_BOOLEAN),
    FUNCTION_VALUE(SJS_BUILTIN_ARRAY_FROM),
    FUNCTION_VALUE(SJS_BUILTIN_ARRAY_IS_ARRAY),
    FUNCTION_VALUE(SJS_BUILTIN_ARRAY_OF),
    FUNCTION_VALUE(SJS_BUILTIN_OBJECT_KEYS),
    FUNCTION_VALUE(SJS_BUILTIN_OBJECT_VALUES),
    FUNCTION_VALUE(SJS_BUILTIN_OBJECT_ENTRIES),
    FUNCTION_VALUE(SJS_BUILTIN_OBJECT_FROM_ENTRIES),
    FUNCTION_VALUE(SJS_BUILTIN_OBJECT_GROUP_BY),
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
    FUNCTION_VALUE(SJS_BUILTIN_MATH_SIGN),
    FUNCTION_VALUE(SJS_BUILTIN_NUMBER),
    FUNCTION_VALUE(SJS_BUILTIN_PARSE_INT),
    FUNCTION_VALUE(SJS_BUILTIN_PARSE_FLOAT),
    FUNCTION_VALUE(SJS_BUILTIN_IS_NAN),
    FUNCTION_VALUE(SJS_BUILTIN_IS_FINITE),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_SQRT),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_HYPOT),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_FROUND),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_CLZ32),
    FUNCTION_VALUE(SJS_BUILTIN_MATH_IMUL),
    FUNCTION_VALUE(SJS_BUILTIN_BIGINT),
    FUNCTION_VALUE(SJS_BUILTIN_BIGINT_AS_INT_N),
    FUNCTION_VALUE(SJS_BUILTIN_BIGINT_AS_UINT_N),
    FUNCTION_VALUE(SJS_BUILTIN_MAP_GROUP_BY),
    FUNCTION_VALUE(SJS_BUILTIN_JSON_PARSE),
    FUNCTION_VALUE(SJS_BUILTIN_JSON_STRINGIFY),
    FUNCTION_VALUE(SJS_BUILTIN_ENCODE_URI),
    FUNCTION_VALUE(SJS_BUILTIN_ENCODE_URI_COMPONENT),
    FUNCTION_VALUE(SJS_BUILTIN_DECODE_URI),
    FUNCTION_VALUE(SJS_BUILTIN_DECODE_URI_COMPONENT)
};

sjs_builtin_id sjs_builtin_lookup(sjs_str space, sjs_str name) {
    for (uint32_t i = 0; i < sizeof FUNCTION_TABLE / sizeof FUNCTION_TABLE[0]; i++) {
        if (sjs_str_equal(space, sjs_cstr(FUNCTION_TABLE[i].space)) && sjs_str_equal(name, sjs_cstr(FUNCTION_TABLE[i].name))) {
            return (sjs_builtin_id)FUNCTION_TABLE[i].id;
        }
    }
    return SJS_BUILTIN_NONE;
}

const sjs_function_entry *sjs_functions(uint32_t *count) {
    *count = sizeof FUNCTION_TABLE / sizeof FUNCTION_TABLE[0];
    return FUNCTION_TABLE;
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
    case SJS_BIGINT:
        return (sjs_builtin_id)name->methods[3];
    case SJS_MAP:
        return (sjs_builtin_id)name->methods[4];
    case SJS_ITERATOR:
        return (sjs_builtin_id)name->methods[5];
    case SJS_REGEXP:
        return (sjs_builtin_id)name->methods[6];
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
    case SJS_METHOD_ARRAY_FIND_LAST_INDEX:
    case SJS_METHOD_ARRAY_FLAT_MAP:
    case SJS_METHOD_ARRAY_MAP:
    case SJS_METHOD_ARRAY_REDUCE:
    case SJS_METHOD_ARRAY_SOME:
    case SJS_METHOD_ARRAY_TO_SORTED:
    case SJS_BUILTIN_OBJECT_GROUP_BY:
    case SJS_BUILTIN_ARRAY_FROM:
    case SJS_BUILTIN_MAP_GROUP_BY:
    case SJS_METHOD_MAP_FOR_EACH:
    case SJS_METHOD_ITERATOR_MAP:
    case SJS_METHOD_ITERATOR_FILTER:
    case SJS_METHOD_ITERATOR_REDUCE:
    case SJS_METHOD_ITERATOR_SOME:
    case SJS_METHOD_ITERATOR_EVERY:
    case SJS_METHOD_ITERATOR_FIND:
    case SJS_METHOD_ITERATOR_FOR_EACH:
    case SJS_METHOD_ITERATOR_FLAT_MAP:
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

/* A primitive whose text sjs_primitive_text gives, for an argument a method
 * turns into a string: a BigInt's digits are made only by String, toString,
 * + and a template. */
static sjs_result text_primitive(sjs_vm *vm, sjs_value v) {
    if (!sjs_is_primitive(v)) return sjs_vm_fault(vm, "staticjs turns only primitives into strings");
    if (v.kind == SJS_BIGINT) return sjs_vm_fault(vm, "staticjs turns a BigInt into a string only with String, toString, + or a template");
    return SJS_OK;
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

/* Append to an array the caller holds alone; takes over `element`.
 * Inlined, as clang did by itself until Map.groupBy's step called it too. */
__attribute__((always_inline)) static inline sjs_result array_add(sjs_vm *vm, sjs_array **array, sjs_value element) {
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
        return sjs_vm_fault(vm, v.kind == SJS_BIGINT ? "JavaScript throws a TypeError where a BigInt would become a number"
                                                     : "staticjs converts only numbers, booleans, null and undefined to numbers");
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

/* Maps' methods, and Object.fromEntries of a map: at the end of the file. */
static sjs_result map_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
static sjs_result entries_other(sjs_vm *vm, sjs_leaf_call *call);

static sjs_result object_listing(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_value o = arg(call, 0);
    if (o.kind != SJS_OBJECT && o.kind != SJS_ARRAY) return sjs_vm_fault(vm, "Object.keys, values and entries take an object or an array");
    if (sjs_array_extra(o)) return sjs_array_extra_fault(vm);
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

/* One entry of Object.fromEntries, put into the object in call->result. The
 * key and the value are borrowed. */
static sjs_result from_entry(sjs_vm *vm, sjs_leaf_call *call, const sjs_map_entry *entry) {
    sjs_value value = entry->value;
    sjs_string *k;
    sjs_result r = sjs_to_key(vm->context, entry->key, &k);
    if (r == SJS_ERR_PROGRAM) return sjs_vm_fault(vm, "a key must be a primitive");
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    if (sjs_str_equal(sjs_string_view(k), SJS_STR("__proto__"))) {
        sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &k->h));
        return sjs_vm_fault(vm, "staticjs objects have no prototype");
    }
    sjs_object *o = SJS_AS_OBJECT(call->result);
    sjs_ref(value);
    r = sjs_object_put_key(vm->context, &o, (sjs_put){ k, value });
    call->result.as.heap = &o->h;
    if (r != SJS_OK) {
        sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &k->h));
        sjs_unref(vm->context, value);
        return sjs_vm_fail(vm, r);
    }
    return SJS_OK;
}

static sjs_result from_entries(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_value entries = arg(call, 0);
    if (entries.kind != SJS_ARRAY) return entries_other(vm, call);
    const sjs_array *a = SJS_AS_ARRAY(entries);
    sjs_object *o;
    sjs_result r = sjs_object_make(vm->context, a->length, &o);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    call->result = sjs_heap_value(SJS_OBJECT, &o->h);
    call->cost = a->length;
    for (uint32_t i = 0; i < a->length; i++) {
        sjs_value pair = a->items[i];
        if (pair.kind != SJS_ARRAY) return sjs_vm_fault(vm, "Object.fromEntries takes an array of [key, value] arrays");
        if (from_entry(vm, call, &(sjs_map_entry){ sjs_array_at(pair, 0), sjs_array_at(pair, 1) }) != SJS_OK) return SJS_ERR_PROGRAM;
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

/* String.fromCharCode and String.fromCodePoint, at the end of the file. */
static sjs_result string_from(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
/* String.raw, encodeURI, encodeURIComponent, decodeURI and
 * decodeURIComponent, at the end of the file. */
static sjs_result string_raw(sjs_vm *vm, sjs_leaf_call *call);
static sjs_result uri_encode(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
static sjs_result uri_decode(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
/* Number, parseInt, parseFloat, isNaN and isFinite, at the end of the file. */
static sjs_result number_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
/* Math.sqrt, hypot, fround, clz32 and imul, at the end of the file. */
static sjs_result math_exact(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
/* BigInt, BigInt.asIntN and asUintN, a BigInt's toString, and Number,
 * parseInt and parseFloat of a BigInt: at the end of the file. */
static sjs_result bigint_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
static sjs_result bigint_method(sjs_vm *vm, sjs_leaf_call *call);
static sjs_result bigint_number(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);

static sjs_result function_call(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_value x = arg(call, 0);
    switch (id) {
    case SJS_BUILTIN_STRING_FROM_CHAR_CODE:
    case SJS_BUILTIN_STRING_FROM_CODE_POINT:
        return string_from(vm, id, call);
    case SJS_BUILTIN_STRING_RAW:
        return string_raw(vm, call);
    case SJS_BUILTIN_ENCODE_URI:
    case SJS_BUILTIN_ENCODE_URI_COMPONENT:
        return uri_encode(vm, id, call);
    case SJS_BUILTIN_DECODE_URI:
    case SJS_BUILTIN_DECODE_URI_COMPONENT:
        return uri_decode(vm, id, call);
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
    case SJS_BUILTIN_ARRAY_OF: {
        sjs_array *a;
        if (new_array(vm, call->count, &a) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_heap_value(SJS_ARRAY, &a->h);
        for (uint32_t i = 0; i < call->count; i++) {
            sjs_ref(call->args[i]);
            a->items[i] = call->args[i];
            a->length = i + 1;
        }
        call->cost = call->count;
        return SJS_OK;
    }
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
    case SJS_BUILTIN_NUMBER:
    case SJS_BUILTIN_PARSE_INT:
    case SJS_BUILTIN_PARSE_FLOAT:
    case SJS_BUILTIN_IS_NAN:
    case SJS_BUILTIN_IS_FINITE:
        return number_function(vm, id, call);
    case SJS_BUILTIN_MATH_SQRT:
    case SJS_BUILTIN_MATH_HYPOT:
    case SJS_BUILTIN_MATH_FROUND:
    case SJS_BUILTIN_MATH_CLZ32:
    case SJS_BUILTIN_MATH_IMUL:
        return math_exact(vm, id, call);
    case SJS_BUILTIN_BIGINT:
    case SJS_BUILTIN_BIGINT_AS_INT_N:
    case SJS_BUILTIN_BIGINT_AS_UINT_N:
        return bigint_function(vm, id, call);
    case SJS_BUILTIN_JSON_PARSE:
        return sjs_json_parse(vm, call);
    case SJS_BUILTIN_JSON_STRINGIFY:
        return sjs_json_stringify(vm, call);
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
 * holds: an argument that is the same array counts too. Not a match result,
 * whose copy JavaScript gives without its index, input and groups. */
__attribute__((always_inline)) static inline bool takes_receiver(const sjs_leaf_call *call) {
    return call->receiver_slot != NULL && call->receiver.kind == SJS_ARRAY && sjs_value_unique(call->receiver) &&
           (call->receiver.as.heap->flags & SJS_ARRAY_EXTRA) == 0;
}

/* Move the receiver out of its slot into the result, to change in place.
 * No element is copied, so the fuel array_method charges for copying the
 * receiver (`cost = n`, before its switch) goes: concat then charges only
 * for what it appends, with() nothing beyond its own unit, and toSpliced
 * for what it removes, shifts and inserts. */
static sjs_array *take_receiver(sjs_leaf_call *call) {
    *call->receiver_slot = sjs_undefined();
    call->result = call->receiver;
    call->cost = 0;
    return SJS_AS_ARRAY(call->receiver);
}

/* toSpliced's and splice's arguments (start, skipCount, ...items), over
 * `length` elements: remove `skip` at `start` and insert the `added` items
 * after them. start and skipCount are relative and clamped as slice takes
 * them; with one argument everything from `start` goes, with none nothing.
 * Inlined, so that toSpliced compiles as it did when it read them itself. */
typedef struct {
    uint32_t length;
    uint32_t start;
    uint32_t skip;
    uint32_t added;
} splice_range;

__attribute__((always_inline)) static inline sjs_result splice_arguments(sjs_vm *vm, const sjs_leaf_call *call, splice_range *span) {
    uint32_t n = span->length;
    double rel = 0;
    if (call->count > 0 && to_integer(vm, arg(call, 0), &rel) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t start = relative(rel, n);
    uint32_t skip = n - start;
    if (call->count == 0) {
        skip = 0;
    } else if (call->count > 1) {
        double count;
        if (to_integer(vm, arg(call, 1), &count) != SJS_OK) return SJS_ERR_PROGRAM;
        skip = count <= 0 ? 0 : (count > (double)(n - start) ? n - start : (uint32_t)count);
    }
    span->start = start;
    span->skip = skip;
    span->added = call->count > 2 ? call->count - 2 : 0;
    return SJS_OK;
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
    case SJS_METHOD_ARRAY_LAST_INDEX_OF: {
        sjs_value x = arg(call, 0);
        if (!sjs_is_primitive(x)) return sjs_vm_fault(vm, "staticjs does not compare objects by identity");
        call->result = sjs_num(-1);
        if (n == 0) return SJS_OK;
        double from = (double)n - 1;
        if (call->count > 1) {
            double rel;
            if (to_integer(vm, arg(call, 1), &rel) != SJS_OK) return SJS_ERR_PROGRAM;
            from = rel < 0 ? (double)n + rel : (rel > from ? from : rel);
        }
        if (from < 0) return SJS_OK;
        for (uint32_t i = (uint32_t)from + 1; i-- > 0;) {
            call->cost++;
            if (sjs_strict_equal_primitive(a->items[i], x)) {
                call->result = sjs_num((double)i);
                return SJS_OK;
            }
        }
        return SJS_OK;
    }
    case SJS_METHOD_ARRAY_SLICE: {
        double start = 0;
        double end = (double)n;
        if (to_integer(vm, arg(call, 0), &start) != SJS_OK) return SJS_ERR_PROGRAM;
        if (arg(call, 1).kind != SJS_UNDEFINED && to_integer(vm, arg(call, 1), &end) != SJS_OK) return SJS_ERR_PROGRAM;
        return array_slice(vm, call, (range){ relative(start, n), relative(end, n) });
    }
    case SJS_METHOD_ARRAY_TO_SPLICED: {
        splice_range span = { n, 0, 0, 0 };
        if (splice_arguments(vm, call, &span) != SJS_OK) return SJS_ERR_PROGRAM;
        uint32_t start = span.start;
        uint32_t skip = span.skip;
        uint32_t added = span.added;
        uint32_t tail = n - start - skip;
        sjs_array *out;
        if (takes_receiver(call)) {
            out = take_receiver(call);
            if (added > skip) {
                sjs_result grown = sjs_array_grow(vm->context, &out, added - skip);
                call->result.as.heap = &out->h;
                if (grown != SJS_OK) return sjs_vm_fail(vm, grown);
            }
            for (uint32_t i = 0; i < skip; i++) sjs_unref(vm->context, out->items[start + i]);
            if (added != skip) {
                /* Move the tail by hand: the regions overlap. */
                if (added < skip) {
                    for (uint32_t i = 0; i < tail; i++) out->items[start + added + i] = out->items[start + skip + i];
                } else {
                    for (uint32_t i = tail; i-- > 0;) out->items[start + added + i] = out->items[start + skip + i];
                }
            }
            out->length = n - skip + added;
            call->cost = (uint64_t)skip + tail + added;
        } else {
            if (new_array(vm, n - skip + added, &out) != SJS_OK) return SJS_ERR_PROGRAM;
            call->result = sjs_heap_value(SJS_ARRAY, &out->h);
            for (uint32_t i = 0; i < start; i++) out->items[i] = a->items[i];
            for (uint32_t i = 0; i < tail; i++) out->items[start + added + i] = a->items[start + skip + i];
            for (uint32_t i = 0; i < start; i++) sjs_ref(out->items[i]);
            for (uint32_t i = 0; i < tail; i++) sjs_ref(out->items[start + added + i]);
            out->length = n - skip + added;
            call->cost = (uint64_t)n + added;
        }
        for (uint32_t i = 0; i < added; i++) {
            sjs_ref(call->args[2 + i]);
            out->items[start + i] = call->args[2 + i];
        }
        return SJS_OK;
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
    return sjs_is_white_space(p);
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

/* trim and the string methods added after these, at the end of the file. */
static sjs_result string_method_tail(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
/* The methods that match a regular expression (regex_match.c), and replace
 * with a string pattern: at the end of the file. */
static sjs_result regex_method(sjs_vm *vm, uint8_t op, sjs_leaf_call *call);

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
        if (arg(call, 0).kind == SJS_REGEXP) return regex_method(vm, SJS_REGEX_OP_SPLIT, call);
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
    default:
        return string_method_tail(vm, id, call);
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
    if (id == SJS_METHOD_SET_IS_SUBSET_OF || id == SJS_METHOD_SET_IS_SUPERSET_OF) {
        /* Every member of the set that should be the smaller one is in the
         * other: the receiver for isSubsetOf, the argument for isSupersetOf. */
        const sjs_array *inner = id == SJS_METHOD_SET_IS_SUBSET_OF ? a : b;
        const sjs_array *outer = inner == a ? b : a;
        bool holds = inner->length <= outer->length;
        for (uint32_t i = 0; holds && i < inner->length; i++) holds = sjs_set_contains(outer, inner->items[i]);
        call->result = sjs_boolean(holds);
        return SJS_OK;
    }
    if (id == SJS_METHOD_SET_IS_DISJOINT_FROM) {
        /* The smaller set's members, each looked up in the other. */
        const sjs_array *smaller = a->length <= b->length ? a : b;
        const sjs_array *larger = smaller == a ? b : a;
        bool disjoint = true;
        for (uint32_t i = 0; disjoint && i < smaller->length; i++) disjoint = !sjs_set_contains(larger, smaller->items[i]);
        call->result = sjs_boolean(disjoint);
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
    if (id <= SJS_METHOD_STRING_TRIM_START) return string_method(vm, id, call);
    if (id <= SJS_METHOD_SET_UNION) return set_method(vm, id, call);
    if (id == SJS_METHOD_BIGINT_TO_STRING) return bigint_method(vm, call);
    if (id == SJS_METHOD_REGEXP_TEST || id == SJS_METHOD_REGEXP_EXEC) {
        return regex_method(vm, id == SJS_METHOD_REGEXP_TEST ? SJS_REGEX_OP_TEST : SJS_REGEX_OP_EXEC, call);
    }
    return map_method(vm, id, call);
}

/* ------------------------------------------------------------------------
 * Built-ins that call back
 */

static sjs_value *slot(sjs_vm *vm, const vm_frame *frame, uint32_t i) {
    return &vm->stack[frame->base + i];
}

/* The array a loop walks: the receiver of a method, and for Object.groupBy
 * the first argument, which call() moved into the receiver's slot. */
static const sjs_array *receiver(sjs_vm *vm, const vm_frame *frame) {
    return SJS_AS_ARRAY(*slot(vm, frame, LOOP_RECEIVER));
}

/* Array.from's items as an array held in the receiver's slot: an array as
 * it is, a set's members, a string's code points, or n undefined values for
 * { length: n }. Charges what it copies. With no mapper the list is the
 * result, and the loop ends before its first step. */
static sjs_result from_list(sjs_vm *vm, sjs_value items, sjs_value *list) {
    sjs_array *a;
    if (items.kind == SJS_ARRAY && !sjs_array_extra(items)) {
        sjs_ref(items);
        *list = items;
        return SJS_OK;
    }
    /* A match result's elements, copied: JavaScript's array has no index,
     * input or groups. */
    if (items.kind == SJS_SET || items.kind == SJS_ARRAY) {
        const sjs_array *s = SJS_AS_ARRAY(items);
        if (new_array(vm, s->length, &a) != SJS_OK) return SJS_ERR_PROGRAM;
        for (uint32_t i = 0; i < s->length; i++) {
            sjs_ref(s->items[i]);
            a->items[i] = s->items[i];
            a->length = i + 1;
        }
        vm->fuel_used += s->length;
    } else if (items.kind == SJS_STRING) {
        sjs_str text = sjs_string_view(SJS_AS_STRING(items));
        if (new_array(vm, 4, &a) != SJS_OK) return SJS_ERR_PROGRAM;
        uint32_t i = 0;
        while (i < text.length) {
            uint32_t start = i;
            sjs_utf8_next(text, &i);
            sjs_string *c;
            sjs_result r = sjs_string_make(vm->context, (sjs_str){ text.ptr + start, i - start }, &c);
            if (r != SJS_OK) {
                sjs_unref(vm->context, sjs_heap_value(SJS_ARRAY, &a->h));
                return sjs_vm_fail(vm, r);
            }
            if (array_add(vm, &a, sjs_heap_value(SJS_STRING, &c->h)) != SJS_OK) {
                sjs_unref(vm->context, sjs_heap_value(SJS_ARRAY, &a->h));
                return SJS_ERR_PROGRAM;
            }
        }
        vm->fuel_used += a->length;
    } else if (items.kind == SJS_MAP) {
        if (sjs_map_entries(vm, SJS_AS_ARRAY(items), &a) != SJS_OK) return SJS_ERR_PROGRAM;
    } else if (items.kind == SJS_ITERATOR) {
        if (sjs_iterator_items(vm, items, &a) != SJS_OK) return SJS_ERR_PROGRAM;
    } else {
        const sjs_entry *e = NULL;
        if (items.kind == SJS_OBJECT && SJS_AS_OBJECT(items)->count == 1) e = sjs_object_find(SJS_AS_OBJECT(items), SJS_STR("length"));
        if (e == NULL) return sjs_vm_fault(vm, "Array.from takes an array, a set, a map, an iterator, a string or an object with only a length");
        double n = e->value.kind == SJS_NUMBER ? e->value.as.number : -1.0;
        if (!sjs_number_is_integer(n) || n < 0) return sjs_vm_fault(vm, "Array.from takes a length that is a non-negative integer");
        if (n > (double)vm->context->limits.max_value_items) return sjs_vm_fault(vm, "Array.from length is past the limit of an array");
        if (new_array(vm, (uint32_t)n, &a) != SJS_OK) return SJS_ERR_PROGRAM;
        for (uint32_t i = 0; i < (uint32_t)n; i++) a->items[i] = sjs_undefined();
        a->length = (uint32_t)n;
        vm->fuel_used += a->length;
    }
    *list = sjs_heap_value(SJS_ARRAY, &a->h);
    return SJS_OK;
}

static sjs_result set_output(sjs_vm *vm, vm_frame *frame, sjs_value v);

/* The loop's state for Array.from: its items become the receiver, and with
 * no mapper they are also the result. */
static sjs_result from_start(sjs_vm *vm, vm_frame *frame, bool mapping) {
    sjs_value *held = slot(vm, frame, LOOP_RECEIVER);
    sjs_value list;
    if (from_list(vm, *held, &list) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_vm_release(vm, *held);
    *held = list;
    if (mapping) return SJS_OK;
    sjs_ref(list);
    frame->index = 0;
    frame->length = 0;
    frame->phase = 0;
    frame->entered = false;
    frame->shape = SJS_SHAPE_NONE;
    return set_output(vm, frame, list);
}

/* Object.groupBy's step for the key the callback gave for the element at
 * `i`: ToPropertyKey of a primitive, and the element appended to the array
 * the key holds in the output object, which is made when the key is first
 * seen. The key is borrowed. The object holds each array alone, so it grows
 * in place. One lookup, through the object's index when it has one. */
static bool group_key_ok(sjs_value key) {
    if (key.kind == SJS_STRING) return !sjs_str_equal(sjs_string_view(SJS_AS_STRING(key)), SJS_STR("__proto__"));
    return key.kind == SJS_UNDEFINED || key.kind == SJS_NULL || key.kind == SJS_BOOLEAN || key.kind == SJS_NUMBER;
}

static bool map_group_add(sjs_vm *vm, vm_frame *frame, sjs_value key);

static bool group_add(sjs_vm *vm, vm_frame *frame, sjs_value key) {
    if (frame->builtin == SJS_BUILTIN_MAP_GROUP_BY) return map_group_add(vm, frame, key);
    sjs_string *k;
    sjs_result r = sjs_to_key(vm->context, key, &k);
    if (r == SJS_ERR_PROGRAM) {
        sjs_vm_fault(vm, "a key must be a primitive");
        return false;
    }
    if (r != SJS_OK) {
        sjs_vm_fail(vm, r);
        return false;
    }
    sjs_value *output = slot(vm, frame, LOOP_OUTPUT);
    sjs_object *o = SJS_AS_OBJECT(*output);
    sjs_value item = receiver(vm, frame)->items[frame->index];
    if (sjs_str_equal(sjs_string_view(k), SJS_STR("__proto__"))) {
        sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &k->h));
        sjs_vm_fault(vm, "staticjs objects have no prototype");
        return false;
    }
    const sjs_entry *e = sjs_object_find(o, sjs_string_view(k));
    if (e != NULL) {
        sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &k->h));
        sjs_entry *entry = (sjs_entry *)(uintptr_t)e;
        sjs_array *group = SJS_AS_ARRAY(entry->value);
        sjs_ref(item);
        if (array_add(vm, &group, item) != SJS_OK) return false;
        entry->value.as.heap = &group->h;
        return true;
    }
    sjs_array *group;
    if (new_array(vm, 4, &group) != SJS_OK) {
        sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &k->h));
        return false;
    }
    sjs_ref(item);
    group->items[group->length++] = item;
    r = sjs_object_put_key(vm->context, &o, (sjs_put){ k, sjs_heap_value(SJS_ARRAY, &group->h) });
    output->as.heap = &o->h;
    if (r != SJS_OK) {
        sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &k->h));
        sjs_unref(vm->context, sjs_heap_value(SJS_ARRAY, &group->h));
        sjs_vm_fail(vm, r);
        return false;
    }
    return true;
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
    { SJS_BUILTIN_OBJECT_GROUP_BY, SJS_SHAPE_PLUCK },
    { SJS_BUILTIN_MAP_GROUP_BY, SJS_SHAPE_PLUCK },
    { SJS_BUILTIN_ARRAY_FROM, SJS_SHAPE_PLUCK },
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
    { SJS_METHOD_ARRAY_FIND_LAST_INDEX, SJS_SHAPE_KEY_COMPARE },
    { SJS_METHOD_ARRAY_FIND_LAST_INDEX, SJS_SHAPE_ELEMENT_COMPARE },
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

/* Map.groupBy, a map's forEach, and an iterator's map, filter and reduce:
 * at the end of the file. A forEach's step is sjs_loop_step's, which calls
 * for_each_call where an element would be called back. */
static sjs_result late_start(sjs_vm *vm, vm_frame *frame);
static loop_step late_done(sjs_vm *vm, vm_frame *frame);
static loop_step for_each_call(sjs_vm *vm, vm_frame *frame);
static bool flatten_iterated(sjs_vm *vm, vm_frame *frame, sjs_value r);

sjs_result sjs_loop_start(sjs_vm *vm, vm_frame *frame) {
    if (frame->builtin >= SJS_REGEX_LOOP_FIRST) return sjs_regex_loop_start(vm, frame);
    if (frame->builtin == SJS_BUILTIN_MAP_GROUP_BY || (frame->builtin >= SJS_METHOD_MAP_FOR_EACH && frame->builtin <= SJS_METHOD_ITERATOR_FLAT_MAP)) {
        sjs_result started = late_start(vm, frame);
        if (started != SJS_OK || frame->builtin == SJS_METHOD_MAP_FOR_EACH) return started;
    }
    sjs_value callback = *slot(vm, frame, LOOP_CALLBACK);
    if (frame->builtin == SJS_BUILTIN_ARRAY_FROM) {
        bool mapping = callback.kind != SJS_UNDEFINED;
        if (mapping && !is_callable(callback)) return sjs_vm_fault(vm, "Array.from maps with a function");
        sjs_result started = from_start(vm, frame, mapping);
        if (started != SJS_OK || !mapping) return started;
    }
    if (frame->builtin == SJS_BUILTIN_OBJECT_GROUP_BY && slot(vm, frame, LOOP_RECEIVER)->kind != SJS_ARRAY) {
        return sjs_vm_fault(vm, "Object.groupBy takes an array");
    }
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
    case SJS_METHOD_ARRAY_FLAT_MAP:
    case SJS_METHOD_ITERATOR_FLAT_MAP:
    case SJS_BUILTIN_ARRAY_FROM: {
        sjs_array *out;
        if (new_array(vm, frame->builtin == SJS_METHOD_ARRAY_MAP || frame->builtin == SJS_BUILTIN_ARRAY_FROM ? a->length : 4, &out) != SJS_OK) return SJS_ERR_PROGRAM;
        return set_output(vm, frame, sjs_heap_value(SJS_ARRAY, &out->h));
    }
    case SJS_BUILTIN_OBJECT_GROUP_BY: {
        sjs_object *out;
        sjs_result r = sjs_object_make(vm->context, 4, &out);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        return set_output(vm, frame, sjs_heap_value(SJS_OBJECT, &out->h));
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

/* Inlined, with pluck_step, into sjs_loop_step, as clang did by itself
 * until the step's test for forEach tipped its inliner: map's pluck and
 * findIndex measured 10 to 14% slower with them out of line. */
__attribute__((always_inline)) static inline loop_step call_element(sjs_vm *vm, vm_frame *frame, uint32_t i) {
    const sjs_array *a = receiver(vm, frame);
    sjs_value args[3] = { a->items[i], sjs_num((double)i), *slot(vm, frame, LOOP_RECEIVER) };
    /* Object.groupBy, Map.groupBy and Array.from, the functions that loop,
     * and an iterator's helpers call back with the element and its index
     * (the iterator's counter) only. */
    bool two = frame->builtin < SJS_BUILTIN_FUNCTION_COUNT || frame->iterated;
    return call_back(vm, frame, (sjs_values){ args, two ? 2u : 3u });
}

static loop_step done(sjs_vm *vm, vm_frame *frame, sjs_value result) {
    set_output(vm, frame, result);
    return LOOP_DONE;
}

/* map's step for a PLUCK callback, `x => x.key`: an object's own property,
 * or undefined for a key it lacks that is not on Object.prototype, as
 * TAKE_LOCAL, GET_MEMBER and RETURN would give it, for one unit of fuel
 * where they cost three. Anything else calls the bytecode. */
__attribute__((always_inline)) static inline loop_step pluck_step(sjs_vm *vm, vm_frame *frame) {
    if (!frame->entered && !sjs_vm_enter_callback(vm, frame)) return LOOP_FAULT;
    sjs_value x = receiver(vm, frame)->items[frame->index];
    if (x.kind == SJS_OBJECT && vm->stack_capacity - vm->sp >= PLUCK_STACK) {
        const sjs_entry *e = own_entry(frame, SJS_AS_OBJECT(x));
        if (e != NULL || !frame->shape_flag) {
            sjs_value v = e != NULL ? e->value : (sjs_value){ .kind = SJS_UNDEFINED };
            if (frame->builtin == SJS_BUILTIN_OBJECT_GROUP_BY || frame->builtin == SJS_BUILTIN_MAP_GROUP_BY) {
                /* A key that faults (an object, or __proto__) runs in the
                 * bytecode, so the fault has the callback's position; as
                 * does a map's __proto__ or BigInt key, which is fine. */
                if (!group_key_ok(v)) return call_element(vm, frame, frame->index);
                if (!group_add(vm, frame, v)) return LOOP_FAULT;
                frame->index++;
                vm->fuel_used += 1;
                return LOOP_AGAIN;
            }
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

/* findLast and findLastIndex visit the elements from the end. */
static bool walks_from_end(sjs_builtin_id id) {
    return id == SJS_METHOD_ARRAY_FIND_LAST || id == SJS_METHOD_ARRAY_FIND_LAST_INDEX;
}

/* What filter, find, findIndex, findLast, findLastIndex, some and every make
 * of the callback's answer for the element at frame->index (from the end, for
 * findLast and findLastIndex), which it moves past: keep it, or finish with true, false, the
 * element or its index. LOOP_AGAIN to go on to the next element. The
 * bytecode's answer and the C path's go through here alike. */
static loop_step predicate_answer(sjs_vm *vm, vm_frame *frame, bool truthy) {
    const sjs_array *a = receiver(vm, frame);
    uint32_t i = frame->index;
    uint32_t element = walks_from_end(frame->builtin) ? frame->length - 1u - i : i;
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
    case SJS_METHOD_ARRAY_FIND_LAST_INDEX:
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
    uint32_t element = walks_from_end(frame->builtin) ? frame->length - 1u - frame->index : frame->index;
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
            if (sjs_value_unique(*held) && !sjs_array_extra(*held)) {
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
        case SJS_METHOD_ARRAY_MAP:
        case SJS_BUILTIN_ARRAY_FROM: {
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
        case SJS_BUILTIN_OBJECT_GROUP_BY:
        case SJS_BUILTIN_MAP_GROUP_BY: {
            bool added = group_add(vm, frame, r);
            sjs_vm_release(vm, r);
            if (!added) return LOOP_FAULT;
            frame->index++;
            break;
        }
        case SJS_METHOD_ARRAY_REDUCE:
            set_output(vm, frame, r);
            frame->index++;
            break;
        case SJS_METHOD_MAP_FOR_EACH:
            sjs_vm_release(vm, r);
            frame->index++;
            break;
        case SJS_METHOD_ITERATOR_FLAT_MAP:
            if (!flatten_iterated(vm, frame, r)) return LOOP_FAULT;
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
        case SJS_METHOD_ARRAY_FIND_LAST_INDEX:
            return done(vm, frame, sjs_num(-1));
        case SJS_METHOD_ARRAY_FIND:
        case SJS_METHOD_ARRAY_FIND_LAST:
            return done(vm, frame, sjs_undefined());
        default:
            /* A regular expression's loop is always here (its index and
             * length stay 0), off the path of the array loops' steps. */
            if (frame->builtin >= SJS_REGEX_LOOP_FIRST) return sjs_regex_loop_step(vm, frame);
            return frame->iterated ? late_done(vm, frame) : LOOP_DONE;
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
        /* An iterator's reduce passes no receiver. */
        loop_step step = call_back(vm, frame, (sjs_values){ args, frame->iterated ? 3u : 4u });
        sjs_vm_release(vm, acc);
        return step;
    }
    if (frame->shape == SJS_SHAPE_PLUCK) return pluck_step(vm, frame);
    if (frame->shape == SJS_SHAPE_KEY_COMPARE || frame->shape == SJS_SHAPE_ELEMENT_COMPARE) return predicate_step(vm, frame);
    /* Past the shapes' C paths, so that only a loop that calls back pays
     * for the test. */
    if (frame->builtin == SJS_METHOD_MAP_FOR_EACH) return for_each_call(vm, frame);
    uint32_t element = walks_from_end(frame->builtin) ? frame->length - 1u - frame->index : frame->index;
    return call_element(vm, frame, element);
}

/* ------------------------------------------------------------------------
 * MUTATION: methods that change their receiver (docs/mutation.md), called
 * by SJS_OP_MUTATE with the slot of the variable that holds the receiver
 * alone. Each updates the slot when the receiver moves as it grows. sort is
 * toSorted's loop, which writes back (vm.c, mutate). This section exists
 * only for mutation.
 */

/* Room for `extra` more elements in the receiver, which may move. */
static sjs_result receiver_grow(sjs_vm *vm, sjs_leaf_call *call, uint32_t extra) {
    sjs_array *a = SJS_AS_ARRAY(*call->receiver_slot);
    sjs_result r = sjs_array_grow(vm->context, &a, extra);
    call->receiver_slot->as.heap = &a->h;
    return r == SJS_OK ? SJS_OK : sjs_vm_fail(vm, r);
}

/* push and unshift: the arguments go in at the end or at the start; the
 * new length is the result. One unit per element added or moved. */
static sjs_result array_insert(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    uint32_t n = SJS_AS_ARRAY(*call->receiver_slot)->length;
    uint32_t count = call->count;
    if (receiver_grow(vm, call, count) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_array *a = SJS_AS_ARRAY(*call->receiver_slot);
    uint32_t at = n;
    call->cost = count;
    if (id == SJS_METHOD_ARRAY_UNSHIFT) {
        for (uint32_t i = n; i-- > 0;) a->items[i + count] = a->items[i];
        at = 0;
        call->cost += n;
    }
    for (uint32_t i = 0; i < count; i++) {
        sjs_ref(call->args[i]);
        a->items[at + i] = call->args[i];
    }
    a->length = n + count;
    call->result = sjs_num((double)a->length);
    return SJS_OK;
}

/* pop and shift: the element taken out is the result, its reference moved
 * from the array. shift moves the rest down, one unit each. The array
 * shrinks when it has become mostly empty (sjs_array_shrink). */
static sjs_result array_remove(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_array *a = SJS_AS_ARRAY(*call->receiver_slot);
    uint32_t n = a->length;
    if (n == 0) return SJS_OK;
    if (id == SJS_METHOD_ARRAY_POP) {
        call->result = a->items[n - 1u];
    } else {
        call->result = a->items[0];
        for (uint32_t i = 1; i < n; i++) a->items[i - 1u] = a->items[i];
        call->cost = n - 1u;
    }
    a->length = n - 1u;
    sjs_array_shrink(vm->context, &a);
    call->receiver_slot->as.heap = &a->h;
    return SJS_OK;
}

/* fill(value, start, end): start and end relative, as slice takes them. */
static sjs_result array_fill(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_array *a = SJS_AS_ARRAY(*call->receiver_slot);
    uint32_t n = a->length;
    double start = 0;
    double end = (double)n;
    if (to_integer(vm, arg(call, 1), &start) != SJS_OK) return SJS_ERR_PROGRAM;
    if (arg(call, 2).kind != SJS_UNDEFINED && to_integer(vm, arg(call, 2), &end) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t from = relative(start, n);
    uint32_t to = relative(end, n);
    sjs_value v = arg(call, 0);
    for (uint32_t i = from; i < to; i++) {
        sjs_ref(v);
        sjs_unref(vm->context, a->items[i]);
        a->items[i] = v;
    }
    call->cost = to > from ? to - from : 0;
    return SJS_OK;
}

/* splice(start, skipCount, ...items): toSpliced's arguments
 * (splice_arguments), done in place; the elements removed are the result,
 * a new array that their references move to. The array grows first, so
 * that one it cannot grow is left as it was, and shrinks when it has
 * become mostly empty. One unit per element removed, moved or added. */
static sjs_result array_splice(sjs_vm *vm, sjs_leaf_call *call) {
    uint32_t n = SJS_AS_ARRAY(*call->receiver_slot)->length;
    splice_range span = { n, 0, 0, 0 };
    if (splice_arguments(vm, call, &span) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t start = span.start;
    uint32_t skip = span.skip;
    uint32_t added = span.added;
    uint32_t tail = n - start - skip;
    sjs_array *removed;
    if (new_array(vm, skip, &removed) != SJS_OK) return SJS_ERR_PROGRAM;
    call->result = sjs_heap_value(SJS_ARRAY, &removed->h);
    if (added > skip && receiver_grow(vm, call, added - skip) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_array *a = SJS_AS_ARRAY(*call->receiver_slot);
    sjs_copy(removed->items, &a->items[start], (size_t)skip * sizeof(sjs_value));
    removed->length = skip;
    if (added < skip) {
        for (uint32_t i = 0; i < tail; i++) a->items[start + added + i] = a->items[start + skip + i];
    } else if (added > skip) {
        for (uint32_t i = tail; i-- > 0;) a->items[start + added + i] = a->items[start + skip + i];
    }
    for (uint32_t i = 0; i < added; i++) {
        sjs_ref(call->args[2 + i]);
        a->items[start + i] = call->args[2 + i];
    }
    a->length = n - skip + added;
    call->cost = (uint64_t)skip + tail + added;
    sjs_array_shrink(vm->context, &a);
    call->receiver_slot->as.heap = &a->h;
    return SJS_OK;
}

/* delete(value): whether the set had it, which it no longer has; an
 * object is never a member, so it gives false. clear(): undefined, one
 * unit for each member released. The set shrinks as it empties. */
static sjs_result set_remove(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_array *set = SJS_AS_ARRAY(*call->receiver_slot);
    if (id == SJS_METHOD_SET_CLEAR) {
        call->cost = set->length;
        sjs_set_clear(vm->context, &set);
    } else {
        sjs_removal removal = sjs_set_remove(vm->context, &set, arg(call, 0));
        call->cost = removal.cost;
        call->result = sjs_boolean(removal.removed);
    }
    call->receiver_slot->as.heap = &set->h;
    return SJS_OK;
}

sjs_builtin_id sjs_mutation_named(const sjs_name *name) {
    return name == NULL ? SJS_BUILTIN_NONE : (sjs_builtin_id)name->mutation;
}

/* The kind of receiver a method that changes it is a method of. */
static uint8_t mutation_kind(sjs_builtin_id id) {
    if (id >= SJS_METHOD_MAP_SET) return SJS_MAP; /* MUTATION: a map's set, delete and clear */
    return id == SJS_METHOD_SET_ADD || id == SJS_METHOD_SET_CLEAR || id == SJS_METHOD_SET_DELETE ? SJS_SET : SJS_ARRAY;
}

sjs_builtin_id sjs_mutation_of(const sjs_name *name, uint8_t kind) {
    sjs_builtin_id id = sjs_mutation_named(name);
    if (id == SJS_BUILTIN_NONE) return id;
    /* MUTATION: delete and clear name a set's method and a map's; the
     * name's column holds the set's. */
    if (kind == SJS_MAP && id == SJS_METHOD_SET_DELETE) return SJS_METHOD_MAP_DELETE;
    if (kind == SJS_MAP && id == SJS_METHOD_SET_CLEAR) return SJS_METHOD_MAP_CLEAR;
    return kind == mutation_kind(id) ? id : SJS_BUILTIN_NONE;
}

bool sjs_mutation_returns_receiver(sjs_builtin_id id) {
    return id == SJS_METHOD_ARRAY_FILL || id == SJS_METHOD_ARRAY_REVERSE || id == SJS_METHOD_ARRAY_SORT || id == SJS_METHOD_SET_ADD ||
           id == SJS_METHOD_MAP_SET; /* MUTATION */
}

/* MUTATION: a map's set, delete and clear, at the end of the file. */
static sjs_result map_change(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);

sjs_result sjs_mutation_call(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    call->result = sjs_undefined();
    call->cost = 0;
    switch (id) {
    case SJS_METHOD_ARRAY_PUSH:
    case SJS_METHOD_ARRAY_UNSHIFT:
        return array_insert(vm, id, call);
    case SJS_METHOD_ARRAY_POP:
    case SJS_METHOD_ARRAY_SHIFT:
        return array_remove(vm, id, call);
    case SJS_METHOD_ARRAY_FILL:
        return array_fill(vm, call);
    case SJS_METHOD_ARRAY_REVERSE: {
        sjs_array *a = SJS_AS_ARRAY(*call->receiver_slot);
        for (uint32_t i = 0, j = a->length; i + 1u < j; i++, j--) {
            sjs_value swap = a->items[i];
            a->items[i] = a->items[j - 1u];
            a->items[j - 1u] = swap;
        }
        call->cost = a->length;
        return SJS_OK;
    }
    case SJS_METHOD_SET_CLEAR:
    case SJS_METHOD_SET_DELETE:
        return set_remove(vm, id, call);
    case SJS_METHOD_ARRAY_SPLICE:
        return array_splice(vm, call);
    case SJS_METHOD_MAP_SET:     /* MUTATION */
    case SJS_METHOD_MAP_DELETE:
    case SJS_METHOD_MAP_CLEAR:
        return map_change(vm, id, call);
    default: { /* SET_ADD: appends, as a set keeps insertion order */
        sjs_value x = arg(call, 0);
        if (!sjs_is_primitive(x)) return sjs_vm_fault(vm, "staticjs sets hold primitives only");
        sjs_array *set = SJS_AS_ARRAY(*call->receiver_slot);
        call->cost = lookup_cost(set);
        sjs_result r = set_add(vm, &set, x);
        call->receiver_slot->as.heap = &set->h;
        return r;
    }
    }
}

/* ------------------------------------------------------------------------
 * String methods added later: at the end of the file, so that the code
 * before them is laid out as it was (docs/builtins.md, section 8). None
 * changes its receiver: JavaScript's strings are immutable.
 */

/* The receiver, as the result: a string is a value, so giving the same one
 * back cannot be told from a copy. */
static sjs_result same_string(sjs_leaf_call *call) {
    sjs_ref(call->receiver);
    call->result = call->receiver;
    return SJS_OK;
}

/* An empty string with room for `bytes` bytes, as call->result: the text is
 * appended with sjs_string_extend, which keeps what the string caches true.
 * Charges a unit per 16 bytes, as the other string methods do. */
static sjs_result string_with_room(sjs_vm *vm, sjs_leaf_call *call, uint64_t bytes) {
    if (bytes > vm->context->limits.max_string_bytes) return sjs_vm_fail(vm, SJS_ERR_LIMIT);
    sjs_string *s;
    sjs_result r = sjs_string_make(vm->context, SJS_STR(""), &s);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    call->result = sjs_heap_value(SJS_STRING, &s->h);
    call->cost += bytes / 16u;
    r = sjs_string_reserve(vm->context, &s, (uint32_t)bytes);
    call->result.as.heap = &s->h;
    return r == SJS_OK ? SJS_OK : sjs_vm_fail(vm, r);
}

/* Append `times` copies of `piece` to a string with room for them, doubling
 * what is appended, so that a short piece costs O(log times) calls. */
static void append_copies(sjs_string *out, sjs_str piece, uint32_t times) {
    if (times == 0 || piece.length == 0) return;
    uint32_t start = out->length;
    sjs_string_extend(out, piece);
    uint32_t done = 1;
    while (done < times) {
        uint32_t more = done <= times - done ? done : times - done;
        sjs_string_extend(out, (sjs_str){ out->bytes + start, more * piece.length });
        done += more;
    }
}

/* padStart and padEnd: the receiver filled to `targetLength` UTF-16 code
 * units with copies of the pad string, the last one cut short. As in
 * JavaScript, the pad string is converted only when there is padding to
 * make; as String converts it, so an object faults. */
static sjs_result string_pad(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    const sjs_string *s = SJS_AS_STRING(call->receiver);
    double target;
    if (to_integer(vm, arg(call, 0), &target) != SJS_OK) return SJS_ERR_PROGRAM;
    if (!(target > (double)s->utf16_length)) return same_string(call);
    sjs_value fill = arg(call, 1);
    char room[32];
    sjs_str filler = SJS_STR(" ");
    if (fill.kind != SJS_UNDEFINED) {
        if (text_primitive(vm, fill) != SJS_OK) return SJS_ERR_PROGRAM;
        filler = sjs_primitive_text(fill, room);
    }
    if (filler.length == 0) return same_string(call);
    /* Each code unit takes at least a byte. */
    double units = target - (double)s->utf16_length;
    if (units > (double)vm->context->limits.max_string_bytes) return sjs_vm_fail(vm, SJS_ERR_LIMIT);
    uint32_t filler_units = sjs_utf16_length(filler);
    uint32_t whole = (uint32_t)units / filler_units;
    uint32_t cut;
    if (!utf16_to_byte(filler, (uint32_t)units % filler_units, &cut)) {
        return sjs_vm_fault(vm, "staticjs strings cannot hold a lone surrogate");
    }
    uint64_t bytes = (uint64_t)whole * filler.length + cut + s->length;
    if (string_with_room(vm, call, bytes) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_string *out = SJS_AS_STRING(call->result);
    if (id == SJS_METHOD_STRING_PAD_END) sjs_string_extend(out, sjs_string_view(s));
    append_copies(out, filler, whole);
    sjs_string_extend(out, (sjs_str){ filler.ptr, cut });
    if (id == SJS_METHOD_STRING_PAD_START) sjs_string_extend(out, sjs_string_view(s));
    return SJS_OK;
}

static sjs_result string_repeat(sjs_vm *vm, sjs_leaf_call *call) {
    const sjs_string *s = SJS_AS_STRING(call->receiver);
    double count;
    if (to_integer(vm, arg(call, 0), &count) != SJS_OK) return SJS_ERR_PROGRAM;
    /* A RangeError in JavaScript. */
    if (count < 0 || !sjs_number_is_finite(count)) return sjs_vm_fault(vm, "repeat takes a count that is finite and not negative");
    if (s->length == 0) return same_string(call);
    if (count * (double)s->length > (double)vm->context->limits.max_string_bytes) return sjs_vm_fail(vm, SJS_ERR_LIMIT);
    uint32_t times = (uint32_t)count;
    if (string_with_room(vm, call, (uint64_t)times * s->length) != SJS_OK) return SJS_ERR_PROGRAM;
    append_copies(SJS_AS_STRING(call->result), sjs_string_view(s), times);
    return SJS_OK;
}

/* The UTF-16 code unit at an index in range, and the code point it is part
 * of: its bytes are [start, end); `second` when the unit is the second half
 * of a pair. */
typedef struct {
    uint32_t point;
    uint32_t unit;
    uint32_t start;
    uint32_t end;
    bool second;
} code_unit;

static code_unit unit_at(const sjs_string *s, uint32_t index) {
    code_unit u = { 0 };
    if (s->h.flags & SJS_STRING_ASCII) {
        u.point = u.unit = (uint8_t)s->bytes[index];
        u.start = index;
        u.end = index + 1u;
        return u;
    }
    sjs_str text = sjs_string_view(s);
    u.second = !utf16_to_byte(text, index, &u.start);
    u.end = u.start;
    u.point = (uint32_t)sjs_utf8_next(text, &u.end);
    if (u.point < 0x10000u) u.unit = u.point;
    else if (u.second) u.unit = 0xDC00u + ((u.point - 0x10000u) & 0x3FFu);
    else u.unit = 0xD800u + ((u.point - 0x10000u) >> 10);
    return u;
}

/* at, charAt, charCodeAt and codePointAt: by UTF-16 index. at counts a
 * negative index from the end; the others give "", NaN or undefined
 * outside the string. A unit that is half of a pair is a number for
 * charCodeAt and codePointAt, but cannot be a staticjs string. */
static sjs_result string_index(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    const sjs_string *s = SJS_AS_STRING(call->receiver);
    double index;
    if (to_integer(vm, arg(call, 0), &index) != SJS_OK) return SJS_ERR_PROGRAM;
    if (id == SJS_METHOD_STRING_AT && index < 0) index += (double)s->utf16_length;
    if (!(index >= 0 && index < (double)s->utf16_length)) {
        if (id == SJS_METHOD_STRING_CHAR_AT) return new_string(vm, SJS_STR(""), &call->result);
        if (id == SJS_METHOD_STRING_CHAR_CODE_AT) call->result = sjs_num(0.0 / 0.0);
        return SJS_OK;
    }
    code_unit u = unit_at(s, (uint32_t)index);
    switch (id) {
    case SJS_METHOD_STRING_CHAR_CODE_AT:
        call->result = sjs_num((double)u.unit);
        return SJS_OK;
    case SJS_METHOD_STRING_CODE_POINT_AT:
        call->result = sjs_num((double)(u.second ? u.unit : u.point));
        return SJS_OK;
    default: /* AT, CHAR_AT */
        if (u.point >= 0x10000u) return sjs_vm_fault(vm, "this index is half of a character outside the BMP");
        return new_string(vm, (sjs_str){ s->bytes + u.start, u.end - u.start }, &call->result);
    }
}

/* trim, trimStart and trimEnd: what is_trim_space matches, off both ends
 * or one. A string with nothing to trim is given back as it is. */
static sjs_result string_trim(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_str s = sjs_string_view(SJS_AS_STRING(call->receiver));
    uint32_t start = 0;
    if (id != SJS_METHOD_STRING_TRIM_END) {
        while (start < s.length) {
            uint32_t next = start;
            if (!is_trim_space((uint32_t)sjs_utf8_next(s, &next))) break;
            start = next;
        }
    }
    uint32_t end = s.length;
    if (id != SJS_METHOD_STRING_TRIM_START) {
        end = start;
        uint32_t i = start;
        while (i < s.length) {
            int32_t point = sjs_utf8_next(s, &i);
            if (!is_trim_space((uint32_t)point)) end = i;
        }
    }
    if (start == 0 && end == s.length) return same_string(call);
    return new_string(vm, (sjs_str){ s.ptr + start, end - start }, &call->result);
}

/* concat: the receiver and each argument as String turns it into a string,
 * joined. STRING_APPEND: a receiver nothing else holds, and not interned,
 * is extended in place, as + extends its left operand (vm.c's
 * string_append); otherwise the text is copied into a new string. */
static sjs_result string_concat(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_string *s = SJS_AS_STRING(call->receiver);
    char room[32];
    uint64_t extra = 0;
    for (uint32_t i = 0; i < call->count; i++) {
        if (text_primitive(vm, call->args[i]) != SJS_OK) return SJS_ERR_PROGRAM;
        extra += sjs_primitive_text(call->args[i], room).length;
    }
    if (s->length + extra > vm->context->limits.max_string_bytes) return sjs_vm_fail(vm, SJS_ERR_LIMIT);
    if (call->receiver_slot != NULL && sjs_value_unique(call->receiver) && (s->h.flags & SJS_STRING_INTERNED) == 0) {
        *call->receiver_slot = sjs_undefined();
        call->result = call->receiver;
        call->cost = extra / 16u;
        sjs_result r = sjs_string_reserve(vm->context, &s, (uint32_t)extra);
        call->result.as.heap = &s->h;
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
    } else {
        if (string_with_room(vm, call, s->length + extra) != SJS_OK) return SJS_ERR_PROGRAM;
        sjs_string_extend(SJS_AS_STRING(call->result), sjs_string_view(s));
        s = SJS_AS_STRING(call->result);
    }
    for (uint32_t i = 0; i < call->count; i++) sjs_string_extend(s, sjs_primitive_text(call->args[i], room));
    return SJS_OK;
}

/* Text built a piece at a time into call->result, a string made by
 * string_with_room, growing as sjs_string_reserve grows it. A code unit
 * that is half of a pair waits in `high` for its second half: a staticjs
 * string cannot hold a lone surrogate, so a half that stays alone faults. */
typedef struct {
    sjs_leaf_call *call;
    uint32_t high;
} text_builder;

static sjs_result lone_surrogate(sjs_vm *vm) {
    return sjs_vm_fault(vm, "staticjs strings cannot hold a lone surrogate");
}

static sjs_result build_text(sjs_vm *vm, text_builder *b, sjs_str text) {
    if (text.length == 0) return SJS_OK;
    if (b->high != 0) return lone_surrogate(vm);
    sjs_string *s = SJS_AS_STRING(b->call->result);
    sjs_result r = sjs_string_reserve(vm->context, &s, text.length);
    b->call->result.as.heap = &s->h;
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    sjs_string_extend(s, text);
    return SJS_OK;
}

/* One UTF-16 code unit: a first half waits, a second half joins it. */
static sjs_result build_unit(sjs_vm *vm, text_builder *b, uint32_t unit) {
    uint32_t point = unit;
    if (unit >= 0xD800u && unit <= 0xDBFFu) {
        if (b->high != 0) return lone_surrogate(vm);
        b->high = unit;
        return SJS_OK;
    }
    if (unit >= 0xDC00u && unit <= 0xDFFFu) {
        if (b->high == 0) return lone_surrogate(vm);
        point = 0x10000u + ((b->high - 0xD800u) << 10) + (unit - 0xDC00u);
        b->high = 0;
    }
    uint8_t bytes[4];
    uint32_t n = sjs_utf8_encode(point, bytes);
    return build_text(vm, b, (sjs_str){ (const char *)bytes, n });
}

/* The end: a first half still waiting is alone. Charges a unit per 16
 * bytes built. */
static sjs_result build_end(sjs_vm *vm, text_builder *b) {
    if (b->high != 0) return lone_surrogate(vm);
    b->call->cost += SJS_AS_STRING(b->call->result)->length / 16u;
    return SJS_OK;
}

/* A match of replaceAll: the text before it and after it, each with the
 * half of a pair it ends or starts with when an empty pattern matched
 * between the halves (0 when none). */
typedef struct {
    sjs_str replacement;
    sjs_str before;
    uint32_t before_half;
    sjs_str matched;
    uint32_t after_half;
    sjs_str after;
} replace_match;

/* GetSubstitution with a string pattern, which has no captures: $$ is $,
 * $& the match, $` the text before it and $' the text after it; any other
 * $ ($1, $<name>, a $ at the end) stays as written. */
static sjs_result substitute(sjs_vm *vm, text_builder *b, const replace_match *m) {
    sjs_str r = m->replacement;
    uint32_t plain = 0;
    uint32_t i = 0;
    while (i + 1u < r.length) {
        char c = r.ptr[i + 1u];
        if (r.ptr[i] != '$' || (c != '$' && c != '&' && c != '`' && c != '\'')) {
            i++;
            continue;
        }
        sjs_result res = build_text(vm, b, (sjs_str){ r.ptr + plain, i - plain });
        if (res == SJS_OK && c == '$') res = build_text(vm, b, SJS_STR("$"));
        if (res == SJS_OK && c == '&') res = build_text(vm, b, m->matched);
        if (res == SJS_OK && c == '`') {
            res = build_text(vm, b, m->before);
            if (res == SJS_OK && m->before_half != 0) res = build_unit(vm, b, m->before_half);
        }
        if (res == SJS_OK && c == '\'') {
            if (m->after_half != 0) res = build_unit(vm, b, m->after_half);
            if (res == SJS_OK) res = build_text(vm, b, m->after);
        }
        if (res != SJS_OK) return res;
        i += 2u;
        plain = i;
    }
    return build_text(vm, b, (sjs_str){ r.ptr + plain, r.length - plain });
}

/* replaceAll with an empty pattern: the replacement before every UTF-16
 * code unit and at the end, so between the halves of a pair too. */
static sjs_result replace_empty(sjs_vm *vm, text_builder *b, replace_match *m) {
    sjs_str s = m->after;
    uint32_t i = 0;
    while (i < s.length) {
        uint32_t next = i;
        uint32_t point = (uint32_t)sjs_utf8_next(s, &next);
        m->before = (sjs_str){ s.ptr, i };
        m->after = (sjs_str){ s.ptr + i, s.length - i };
        if (substitute(vm, b, m) != SJS_OK) return SJS_ERR_PROGRAM;
        if (point < 0x10000u) {
            if (build_text(vm, b, (sjs_str){ s.ptr + i, next - i }) != SJS_OK) return SJS_ERR_PROGRAM;
        } else {
            uint32_t high = 0xD800u + ((point - 0x10000u) >> 10);
            uint32_t low = 0xDC00u + ((point - 0x10000u) & 0x3FFu);
            if (build_unit(vm, b, high) != SJS_OK) return SJS_ERR_PROGRAM;
            m->before_half = high;
            m->after_half = low;
            m->after = (sjs_str){ s.ptr + next, s.length - next };
            sjs_result r = substitute(vm, b, m);
            m->before_half = 0;
            m->after_half = 0;
            if (r != SJS_OK || build_unit(vm, b, low) != SJS_OK) return SJS_ERR_PROGRAM;
        }
        i = next;
    }
    m->before = s;
    m->after = (sjs_str){ s.ptr + s.length, 0 };
    return substitute(vm, b, m);
}

/* replaceAll(pattern, replacement) with a string pattern: every match, left
 * to right and not overlapping, replaced. A non-empty pattern is matched by
 * bytes, which finds what UTF-16 finds: a valid string cannot start with a
 * second half or end with a first half. */
static sjs_result string_replace_all(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_str s = sjs_string_view(SJS_AS_STRING(call->receiver));
    sjs_str pattern;
    if (string_arg(vm, arg(call, 0), &pattern) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_value with = arg(call, 1);
    if (with.kind == SJS_FUNCTION) return sjs_vm_fault(vm, "staticjs replaces with a string, not with a function's result");
    if (text_primitive(vm, with) != SJS_OK) return SJS_ERR_PROGRAM;
    char room[32];
    replace_match m = { 0 };
    m.replacement = sjs_primitive_text(with, room);
    m.matched = pattern;
    text_builder b = { call, 0 };
    if (string_with_room(vm, call, s.length) != SJS_OK) return SJS_ERR_PROGRAM;
    if (pattern.length == 0) {
        m.after = s;
        if (replace_empty(vm, &b, &m) != SJS_OK) return SJS_ERR_PROGRAM;
        return build_end(vm, &b);
    }
    uint32_t last = 0;
    for (;;) {
        int64_t at = find_bytes(s, pattern, last);
        if (at < 0) break;
        uint32_t end = (uint32_t)at + pattern.length;
        m.before = (sjs_str){ s.ptr, (uint32_t)at };
        m.after = (sjs_str){ s.ptr + end, s.length - end };
        if (build_text(vm, &b, (sjs_str){ s.ptr + last, (uint32_t)at - last }) != SJS_OK) return SJS_ERR_PROGRAM;
        if (substitute(vm, &b, &m) != SJS_OK) return SJS_ERR_PROGRAM;
        last = end;
    }
    if (build_text(vm, &b, (sjs_str){ s.ptr + last, s.length - last }) != SJS_OK) return SJS_ERR_PROGRAM;
    return build_end(vm, &b);
}

/* replace(pattern, replacement) with a string pattern: its first match
 * replaced, or the string itself when there is none. */
static sjs_result string_replace(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_str s = sjs_string_view(SJS_AS_STRING(call->receiver));
    sjs_str pattern;
    if (string_arg(vm, arg(call, 0), &pattern) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_value with = arg(call, 1);
    if (with.kind == SJS_FUNCTION) return sjs_vm_fault(vm, "staticjs replaces with a string, not with a function's result");
    if (text_primitive(vm, with) != SJS_OK) return SJS_ERR_PROGRAM;
    int64_t at = find_bytes(s, pattern, 0);
    if (at < 0) {
        call->result = call->receiver;
        sjs_ref(call->result);
        return SJS_OK;
    }
    char room[32];
    replace_match m = { 0 };
    m.replacement = sjs_primitive_text(with, room);
    m.matched = pattern;
    uint32_t end = (uint32_t)at + pattern.length;
    m.before = (sjs_str){ s.ptr, (uint32_t)at };
    m.after = (sjs_str){ s.ptr + end, s.length - end };
    text_builder b = { call, 0 };
    if (string_with_room(vm, call, s.length) != SJS_OK) return SJS_ERR_PROGRAM;
    if (build_text(vm, &b, m.before) != SJS_OK || substitute(vm, &b, &m) != SJS_OK || build_text(vm, &b, m.after) != SJS_OK) {
        return SJS_ERR_PROGRAM;
    }
    return build_end(vm, &b);
}

/* substring(start, end): both ToIntegerOrInfinity, clamped to [0, length],
 * and swapped when start is past end. substr(start, length): start counts
 * from the end when negative, an undefined length runs to the end. Both by
 * UTF-16 code unit, converting their arguments as slice does. A cut
 * between the halves of a pair faults, as staticjs strings cannot hold a
 * lone surrogate. */
static sjs_result string_substring(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    const sjs_string *string = SJS_AS_STRING(call->receiver);
    uint32_t units = string->utf16_length;
    double start = 0;
    double second = (double)units;
    if (to_integer(vm, arg(call, 0), &start) != SJS_OK) return SJS_ERR_PROGRAM;
    if (arg(call, 1).kind != SJS_UNDEFINED && to_integer(vm, arg(call, 1), &second) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t from;
    uint32_t to;
    if (id == SJS_METHOD_STRING_SUBSTRING) {
        from = start < 0 ? 0 : start > (double)units ? units : (uint32_t)start;
        to = second < 0 ? 0 : second > (double)units ? units : (uint32_t)second;
        if (to < from) {
            uint32_t swap = from;
            from = to;
            to = swap;
        }
    } else {
        from = relative(start, units);
        double end = (double)from + second;
        to = end < (double)from ? from : end > (double)units ? units : (uint32_t)end;
    }
    sjs_str s = sjs_string_view(string);
    uint32_t a = from;
    uint32_t b = to;
    if (!(string->h.flags & SJS_STRING_ASCII) && (!utf16_to_byte(s, from, &a) || !utf16_to_byte(s, to, &b))) {
        return sjs_vm_fault(vm, "this cut would leave half of a character outside the BMP: a lone surrogate");
    }
    return new_string(vm, (sjs_str){ s.ptr + a, b - a }, &call->result);
}

__attribute__((noinline)) static sjs_result string_method_tail(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    switch (id) {
    case SJS_METHOD_STRING_REPLACE_ALL:
        if (arg(call, 0).kind == SJS_REGEXP) {
            call->cost = 0;
            sjs_regex_call_params params = { call, SJS_REGEX_OP_REPLACE, true };
            return sjs_regex_call(vm, &params);
        }
        return string_replace_all(vm, call);
    case SJS_METHOD_STRING_REPLACE:
        if (arg(call, 0).kind == SJS_REGEXP) return regex_method(vm, SJS_REGEX_OP_REPLACE, call);
        return string_replace(vm, call);
    case SJS_METHOD_STRING_MATCH:
        return regex_method(vm, SJS_REGEX_OP_MATCH, call);
    case SJS_METHOD_STRING_MATCH_ALL:
        return regex_method(vm, SJS_REGEX_OP_MATCH_ALL, call);
    case SJS_METHOD_STRING_SEARCH:
        return regex_method(vm, SJS_REGEX_OP_SEARCH, call);
    case SJS_METHOD_STRING_TRIM:
    case SJS_METHOD_STRING_TRIM_END:
    case SJS_METHOD_STRING_TRIM_START:
        return string_trim(vm, id, call);
    case SJS_METHOD_STRING_CONCAT:
        return string_concat(vm, call);
    case SJS_METHOD_STRING_AT:
    case SJS_METHOD_STRING_CHAR_AT:
    case SJS_METHOD_STRING_CHAR_CODE_AT:
    case SJS_METHOD_STRING_CODE_POINT_AT:
        return string_index(vm, id, call);
    case SJS_METHOD_STRING_PAD_END:
    case SJS_METHOD_STRING_PAD_START:
        return string_pad(vm, id, call);
    case SJS_METHOD_STRING_SUBSTR:
    case SJS_METHOD_STRING_SUBSTRING:
        return string_substring(vm, id, call);
    case SJS_METHOD_STRING_IS_WELL_FORMED:
        /* A staticjs string cannot hold a lone surrogate. */
        call->cost = 0;
        call->result = sjs_boolean(true);
        return SJS_OK;
    case SJS_METHOD_STRING_TO_WELL_FORMED:
        call->cost = 0;
        return same_string(call);
    default: /* REPEAT */
        return string_repeat(vm, call);
    }
}

/* String.fromCharCode(...codes): each code ToUint16, as a UTF-16 code unit.
 * String.fromCodePoint(...points): each an integer from 0 to 0x10FFFF, or a
 * RangeError in JavaScript. A pair of halves makes a character either way;
 * a half left alone faults (text_builder). */
__attribute__((noinline)) static sjs_result string_from(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    /* Each code unit takes at least a byte. */
    if (string_with_room(vm, call, call->count) != SJS_OK) return SJS_ERR_PROGRAM;
    call->cost += call->count;
    text_builder b = { call, 0 };
    for (uint32_t i = 0; i < call->count; i++) {
        double d;
        if (sjs_vm_to_number(vm, call->args[i], &d) != SJS_OK) return SJS_ERR_PROGRAM;
        uint32_t point;
        if (id == SJS_BUILTIN_STRING_FROM_CHAR_CODE) {
            double t = sjs_number_is_finite(d) ? sjs_number_trunc(d) : 0.0;
            point = (uint32_t)(t - 65536.0 * sjs_number_floor(t / 65536.0));
        } else {
            if (!sjs_number_is_integer(d) || d < 0 || d > 1114111.0) {
                return sjs_vm_fault(vm, "a code point is an integer from 0 to 0x10FFFF");
            }
            point = (uint32_t)d;
        }
        if (build_unit(vm, &b, point) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    return build_end(vm, &b);
}

/* ------------------------------------------------------------------------
 * Number, parseInt, parseFloat, isNaN and isFinite
 */

/* Explicit conversions: Number reads a string (StringToNumber), as
 * parseInt and parseFloat do; the global isNaN and isFinite convert as the
 * Math functions do, so a string faults there. Reading a string costs a
 * unit of fuel per 16 bytes. */
static sjs_result number_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_value x = arg(call, 0);
    char room[32];
    sjs_str text = { 0 };
    double d;
    if (x.kind == SJS_BIGINT && id != SJS_BUILTIN_IS_NAN && id != SJS_BUILTIN_IS_FINITE) return bigint_number(vm, id, call);
    if (id == SJS_BUILTIN_PARSE_INT || id == SJS_BUILTIN_PARSE_FLOAT) {
        /* The string they read: a primitive's as String gives it. An
         * object faults, as String does. */
        if (text_primitive(vm, x) != SJS_OK) return SJS_ERR_PROGRAM;
        text = sjs_primitive_text(x, room);
        call->cost = text.length / 16u;
    }
    switch (id) {
    case SJS_BUILTIN_NUMBER:
        if (call->count == 0) {
            d = 0;
        } else if (x.kind == SJS_STRING) {
            text = sjs_string_view(SJS_AS_STRING(x));
            call->cost = text.length / 16u;
            d = sjs_number_parse(text);
        } else if (!sjs_is_primitive(x)) {
            return sjs_vm_fault(vm, "staticjs converts only primitives to numbers");
        } else if (sjs_vm_to_number(vm, x, &d) != SJS_OK) {
            return SJS_ERR_PROGRAM;
        }
        break;
    case SJS_BUILTIN_PARSE_FLOAT:
        d = sjs_number_parse_float(text);
        break;
    case SJS_BUILTIN_PARSE_INT: {
        double radix;
        if (sjs_vm_to_number(vm, arg(call, 1), &radix) != SJS_OK) return SJS_ERR_PROGRAM;
        d = sjs_number_parse_int(text, (int32_t)sjs_number_to_uint32(radix));
        break;
    }
    default: /* IS_NAN, IS_FINITE */
        if (sjs_vm_to_number(vm, x, &d) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_boolean(id == SJS_BUILTIN_IS_NAN ? sjs_number_is_nan(d) : sjs_number_is_finite(d));
        return SJS_OK;
    }
    call->result = sjs_num(d);
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Math functions IEEE 754 makes exact, the same bits on every target:
 * sqrt and fround round once; clz32 and imul are integer operations; hypot
 * is V8's own sum (number.c). Each converts its arguments as the other
 * Math functions do.
 */

/* ToInt32's bits as a number. */
static double int32_number(uint32_t bits) {
    return bits >= 0x80000000u ? (double)bits - 4294967296.0 : (double)bits;
}

static sjs_result math_hypot(sjs_vm *vm, sjs_leaf_call *call) {
    /* Every argument is converted and seen first, as V8 does. */
    sjs_hypot h = { 0, 0, 0, false };
    for (uint32_t i = 0; i < call->count; i++) {
        double d;
        if (sjs_vm_to_number(vm, call->args[i], &d) != SJS_OK) return SJS_ERR_PROGRAM;
        sjs_hypot_see(&h, d);
    }
    call->cost = call->count;
    double result;
    if (!sjs_hypot_settled(&h, &result)) {
        for (uint32_t i = 0; i < call->count; i++) {
            double d;
            if (sjs_vm_to_number(vm, call->args[i], &d) != SJS_OK) return SJS_ERR_PROGRAM;
            sjs_hypot_add(&h, d);
        }
        result = sjs_hypot_end(&h);
    }
    call->result = sjs_num(result);
    return SJS_OK;
}

static sjs_result math_exact(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    if (id == SJS_BUILTIN_MATH_HYPOT) return math_hypot(vm, call);
    double x;
    if (sjs_vm_to_number(vm, arg(call, 0), &x) != SJS_OK) return SJS_ERR_PROGRAM;
    double y;
    switch (id) {
    case SJS_BUILTIN_MATH_SQRT:
        y = sjs_number_sqrt(x);
        break;
    case SJS_BUILTIN_MATH_FROUND:
        y = sjs_number_fround(x);
        break;
    case SJS_BUILTIN_MATH_CLZ32: {
        uint32_t bits = sjs_number_to_uint32(x);
        y = bits == 0 ? 32 : (double)__builtin_clz(bits);
        break;
    }
    default: { /* IMUL */
        double b;
        if (sjs_vm_to_number(vm, arg(call, 1), &b) != SJS_OK) return SJS_ERR_PROGRAM;
        y = int32_number(sjs_number_to_uint32(x) * sjs_number_to_uint32(b));
        break;
    }
    }
    call->result = sjs_num(y);
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * BigInts: the BigInt function, BigInt.asIntN and asUintN, toString(radix),
 * and Number, parseInt and parseFloat of a BigInt.
 */

/* BigInt(x): ToBigInt, but a number converts when it is an integer. */
static sjs_result bigint_of(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_value x = arg(call, 0);
    sjs_result r;
    switch (x.kind) {
    case SJS_BIGINT:
        sjs_ref(x);
        call->result = x;
        return SJS_OK;
    case SJS_BOOLEAN:
        r = sjs_bigint_from_number(vm->context, x.boolean ? 1.0 : 0.0, &call->result);
        break;
    case SJS_NUMBER:
        if (!sjs_number_is_integer(x.as.number)) {
            return sjs_vm_fault(vm, "JavaScript throws a RangeError for BigInt of a number that is not an integer");
        }
        r = sjs_bigint_from_number(vm->context, x.as.number, &call->result);
        call->cost = 2u;
        break;
    case SJS_STRING: {
        sjs_str text = sjs_string_view(SJS_AS_STRING(x));
        sjs_bigint_digits digits;
        if (!sjs_bigint_parse(text, &digits)) return sjs_vm_fault(vm, "JavaScript throws a SyntaxError for BigInt of this string");
        call->cost = text.length / 16u + sjs_bigint_text_cost((uint64_t)digits.digits.length / 8u + 1u, digits.radix);
        r = sjs_bigint_from_digits(vm->context, digits, &call->result);
        break;
    }
    case SJS_UNDEFINED:
    case SJS_NULL:
        return sjs_vm_fault(vm, "JavaScript throws a TypeError for BigInt of undefined or null");
    default:
        return sjs_vm_fault(vm, "staticjs converts only primitives to BigInts");
    }
    return r == SJS_OK ? SJS_OK : sjs_vm_fail(vm, r);
}

/* asIntN and asUintN: ToIndex of the bits, then ToBigInt of the value; a
 * string, which ToBigInt reads, faults, as a conversion on the way to
 * something else. */
static sjs_result bigint_as_n(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    double bits;
    if (to_integer(vm, arg(call, 0), &bits) != SJS_OK) return SJS_ERR_PROGRAM;
    if (bits < 0 || bits > 9007199254740991.0) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a number of bits outside 0 to 2^53 - 1");
    sjs_value x = arg(call, 1);
    sjs_value held = sjs_undefined();
    if (x.kind == SJS_BOOLEAN) {
        sjs_result r = sjs_bigint_from_number(vm->context, x.boolean ? 1.0 : 0.0, &held);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        x = held;
    } else if (x.kind == SJS_STRING) {
        return sjs_vm_fault(vm, "staticjs reads a string as a BigInt only in BigInt()");
    } else if (x.kind != SJS_BIGINT) {
        return sjs_vm_fault(vm, "JavaScript throws a TypeError where this value would become a BigInt");
    }
    sjs_bigint_wrap_params w = { x, (uint64_t)bits, id == SJS_BUILTIN_BIGINT_AS_INT_N };
    uint64_t width = w.bits / 32u + 1u;
    call->cost = (width < SJS_AS_BIGINT(x)->length ? width : SJS_AS_BIGINT(x)->length) / 16u;
    sjs_result r = sjs_bigint_wrap(vm->context, w, &call->result);
    sjs_unref(vm->context, held);
    if (r == SJS_OK && call->result.kind == SJS_BIGINT) call->cost += SJS_AS_BIGINT(call->result)->length / 16u;
    return r == SJS_OK ? SJS_OK : sjs_vm_fail(vm, r);
}

__attribute__((noinline)) static sjs_result bigint_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    if (id == SJS_BUILTIN_BIGINT) return bigint_of(vm, call);
    return bigint_as_n(vm, id, call);
}

/* x.toString(radix): radix 10 when undefined, else 2 to 36. */
__attribute__((noinline)) static sjs_result bigint_method(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_value radix_arg = arg(call, 0);
    double radix = 10;
    if (radix_arg.kind != SJS_UNDEFINED && to_integer(vm, radix_arg, &radix) != SJS_OK) return SJS_ERR_PROGRAM;
    if (radix < 2 || radix > 36) return sjs_vm_fault(vm, "JavaScript throws a RangeError for a radix outside 2 to 36");
    const sjs_bigint *b = SJS_AS_BIGINT(call->receiver);
    call->cost = sjs_bigint_text_cost(b->length, (uint32_t)radix);
    sjs_string *s;
    sjs_result r = sjs_bigint_to_string(vm->context, (sjs_bigint_radix){ b, (uint32_t)radix }, &s);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    call->result = sjs_heap_value(SJS_STRING, &s->h);
    return SJS_OK;
}

/* Number(x), correctly rounded; parseFloat(x) reads String(x), whose
 * digits round alike; parseInt(x, radix) reads String(x) in the radix. */
__attribute__((noinline)) static sjs_result bigint_number(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    const sjs_bigint *b = SJS_AS_BIGINT(arg(call, 0));
    if (id != SJS_BUILTIN_PARSE_INT) {
        call->cost = b->length / 16u;
        call->result = sjs_num(sjs_bigint_to_number(b));
        return SJS_OK;
    }
    double radix;
    if (sjs_vm_to_number(vm, arg(call, 1), &radix) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_buf text = { 0 };
    sjs_result r = sjs_bigint_format(&vm->context->counting, (sjs_bigint_radix){ b, 10u }, &text);
    if (r != SJS_OK) {
        sjs_buf_free(&vm->context->counting, &text);
        return sjs_vm_fail(vm, r);
    }
    call->cost = sjs_bigint_text_cost(b->length, 10u) + text.length / 16u;
    call->result = sjs_num(sjs_number_parse_int((sjs_str){ (const char *)text.ptr, text.length }, (int32_t)sjs_number_to_uint32(radix)));
    sjs_buf_free(&vm->context->counting, &text);
    return SJS_OK;
}


/* ------------------------------------------------------------------------
 * Maps (README.md, "Maps"): get, has, a map's entries as arrays, and
 * Object.fromEntries of a map. At the end of the file and out of line, so
 * that the code before it is laid out, and inlined, as it was.
 */

/* The fuel of one lookup in a map: one through its index, or in a map under
 * SJS_INDEX_FROM entries; its entries when its index failed to allocate and
 * a lookup scans it (lookup_cost, by entries). */
static uint64_t map_lookup_cost(const sjs_array *map) {
    uint32_t entries = map->length / 2u;
    return map->index != NULL || entries < SJS_INDEX_FROM ? 1u : entries;
}

/* take and drop: at the end of the file. */
static sjs_result iterator_limit(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);

/* keys(), values() and entries(): an iterator that holds the map, one unit;
 * an iterator's toArray(): what it yields, a unit each. */
static sjs_result iterator_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    if (id == SJS_METHOD_ITERATOR_TAKE || id == SJS_METHOD_ITERATOR_DROP) return iterator_limit(vm, id, call);
    if (id == SJS_METHOD_ITERATOR_TO_ARRAY) {
        sjs_array *a;
        if (sjs_iterator_items(vm, call->receiver, &a) != SJS_OK) return SJS_ERR_PROGRAM;
        call->result = sjs_heap_value(SJS_ARRAY, &a->h);
        return SJS_OK;
    }
    uint8_t mode = id == SJS_METHOD_MAP_KEYS ? SJS_ITERATE_KEYS : id == SJS_METHOD_MAP_VALUES ? SJS_ITERATE_VALUES : SJS_ITERATE_ENTRIES;
    sjs_ref(call->receiver);
    sjs_result r = sjs_iterator_make(vm->context, (sjs_iterator_spec){ call->receiver, sjs_undefined(), mode }, &call->result);
    if (r == SJS_OK) return SJS_OK;
    sjs_unref(vm->context, call->receiver);
    return sjs_vm_fail(vm, r);
}

/* get(key): its value, or undefined; has(key). An object is never a key, so
 * either finds nothing for one, as JavaScript finds nothing for an object
 * the map does not hold. One lookup through the index. Then the iterators'
 * methods. */
__attribute__((noinline)) static sjs_result map_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    if (id != SJS_METHOD_MAP_GET && id != SJS_METHOD_MAP_HAS) return iterator_method(vm, id, call);
    const sjs_array *m = SJS_AS_ARRAY(call->receiver);
    const sjs_value *found = sjs_map_find(m, arg(call, 0));
    call->cost = map_lookup_cost(m);
    if (id == SJS_METHOD_MAP_HAS) {
        call->result = sjs_boolean(found != NULL);
        return SJS_OK;
    }
    if (found != NULL) {
        call->result = *found;
        sjs_ref(call->result);
    }
    return SJS_OK;
}

/* Entries [from, to) of a map, as [key, value] arrays: what map_pairs
 * makes. */
typedef struct {
    const sjs_array *map;
    uint32_t from;
    uint32_t to;
} map_slice;

/* The entries of a map_slice as a new array of new [key, value] arrays, a
 * unit of fuel each. */
static sjs_result map_pairs(sjs_vm *vm, map_slice slice, sjs_array **out) {
    const sjs_array *map = slice.map;
    sjs_array *a;
    if (new_array(vm, slice.to - slice.from, &a) != SJS_OK) return SJS_ERR_PROGRAM;
    for (uint32_t i = slice.from; i < slice.to; i++) {
        sjs_array *pair;
        sjs_result r = sjs_array_make(vm->context, 2, &pair);
        if (r != SJS_OK) {
            sjs_unref(vm->context, sjs_heap_value(SJS_ARRAY, &a->h));
            return sjs_vm_fail(vm, r);
        }
        pair->items[0] = map->items[2u * i];
        pair->items[1] = map->items[2u * i + 1u];
        sjs_ref(pair->items[0]);
        sjs_ref(pair->items[1]);
        pair->length = 2;
        a->items[a->length++] = sjs_heap_value(SJS_ARRAY, &pair->h);
    }
    vm->fuel_used += slice.to - slice.from;
    *out = a;
    return SJS_OK;
}

__attribute__((noinline)) sjs_result sjs_map_entries(sjs_vm *vm, const sjs_array *map, sjs_array **out) {
    return map_pairs(vm, (map_slice){ map, 0, map->length / 2u }, out);
}

/* Object.fromEntries of what an iterator yields: each a [key, value] array,
 * as an array's are. */
static sjs_result entries_iterated(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_array *list;
    if (sjs_iterator_items(vm, arg(call, 0), &list) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_object *o;
    sjs_result r = sjs_object_make(vm->context, list->length, &o);
    if (r != SJS_OK) r = sjs_vm_fail(vm, r);
    else call->result = sjs_heap_value(SJS_OBJECT, &o->h);
    call->cost = list->length;
    for (uint32_t i = 0; r == SJS_OK && i < list->length; i++) {
        sjs_value pair = list->items[i];
        if (pair.kind != SJS_ARRAY) r = sjs_vm_fault(vm, "Object.fromEntries takes [key, value] arrays");
        else r = from_entry(vm, call, &(sjs_map_entry){ sjs_array_at(pair, 0), sjs_array_at(pair, 1) });
    }
    sjs_unref(vm->context, sjs_heap_value(SJS_ARRAY, &list->h));
    return r;
}

/* Object.fromEntries of anything but an array: a map's entries, each key
 * ToPropertyKey as an array's pair's is, or what an iterator yields. */
__attribute__((noinline)) static sjs_result entries_other(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_value entries = arg(call, 0);
    if (entries.kind == SJS_ITERATOR) return entries_iterated(vm, call);
    if (entries.kind != SJS_MAP) return sjs_vm_fault(vm, "Object.fromEntries takes an array of [key, value] arrays, a map, or an iterator of them");
    const sjs_array *m = SJS_AS_ARRAY(entries);
    uint32_t n = m->length / 2u;
    sjs_object *o;
    sjs_result r = sjs_object_make(vm->context, n, &o);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    call->result = sjs_heap_value(SJS_OBJECT, &o->h);
    call->cost = n;
    for (uint32_t i = 0; i < n; i++) {
        if (from_entry(vm, call, &(sjs_map_entry){ m->items[2u * i], m->items[2u * i + 1u] }) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    return SJS_OK;
}

/* The values [from, to) of what an iterator yields, `to` clamped to their
 * count and `from` to `to`. */
typedef struct {
    sjs_value iterator;
    uint32_t from;
    uint32_t to;
} item_range;

/* What an iterator yields in `range`, as an array: its own when the range
 * covers it, or a new one, a unit of fuel for each value copied; a map's own
 * iterator copies only the range. Faults unless nothing but the caller's
 * stack slot holds the iterator, as sjs_iterator_items says. */
static sjs_result iterator_range(sjs_vm *vm, item_range range, sjs_array **out) {
    if (!sjs_value_unique(range.iterator)) {
        return sjs_vm_fault(vm, "an iterator yields its values once: staticjs reads one only where nothing else holds it");
    }
    const sjs_iterator *it = SJS_AS_ITERATOR(range.iterator);
    uint32_t n = sjs_iterator_count(it);
    uint32_t to = range.to < n ? range.to : n;
    uint32_t from = range.from < to ? range.from : to;
    const sjs_array *m = SJS_AS_ARRAY(it->source);
    if (it->items.kind == SJS_ARRAY) {
        if (from == 0 && to == n) {
            sjs_ref(it->items);
            *out = SJS_AS_ARRAY(it->items);
            return SJS_OK;
        }
        m = SJS_AS_ARRAY(it->items);
    } else if (it->h.flags == SJS_ITERATE_ENTRIES) {
        return map_pairs(vm, (map_slice){ m, from, to }, out);
    }
    /* A list's values one apart; a map's keys or values two apart. */
    uint32_t step = it->items.kind == SJS_ARRAY ? 1u : 2u;
    uint32_t value = it->items.kind != SJS_ARRAY && it->h.flags == SJS_ITERATE_VALUES ? 1u : 0u;
    sjs_array *a;
    if (new_array(vm, to - from, &a) != SJS_OK) return SJS_ERR_PROGRAM;
    for (uint32_t i = from; i < to; i++) {
        sjs_value v = m->items[step * i + value];
        sjs_ref(v);
        a->items[a->length++] = v;
    }
    vm->fuel_used += to - from;
    *out = a;
    return SJS_OK;
}

__attribute__((noinline)) sjs_result sjs_iterator_items(sjs_vm *vm, sjs_value iterator, sjs_array **out) {
    return iterator_range(vm, (item_range){ iterator, 0, UINT32_MAX }, out);
}

/* ------------------------------------------------------------------------
 * Loops over maps and iterators: Map.groupBy, a map's forEach, and an
 * iterator's map, filter, reduce, some, every, find, forEach and flatMap.
 * sjs_loop_start hands them to late_start, and sjs_loop_step a forEach's
 * call to for_each_call. An iterator's helper runs as the array's loop of
 * its name over what the iterator yields (`iterated`: its callback gets the
 * value and a counter, and the result of map, filter and flatMap becomes an
 * iterator in late_done), so it has the same shapes' C paths; forEach runs
 * as a map's, and flatMap has its own step (flatten_iterated).
 *
 * JavaScript's helpers are lazy: `m.values().map(f).take(1)` calls f once,
 * and `.map(f).some(g)` calls f only up to the first element g accepts.
 * staticjs's map, filter and flatMap run to the end when they are called,
 * so f runs on values JavaScript never asks it for. That can give no other
 * value: a callback changes nothing a program can see (it cannot assign to
 * a variable it captures, and values never change once shared), and every
 * value JavaScript computes, staticjs computes the same. The extra calls
 * can only fault, or use fuel, where JavaScript would have given a value
 * (map.spec, "laziness"). some, every and find stop at the first element
 * that decides them, and take copies no more than its limit. The result
 * holds the map, which cannot change while it lives.
 */

/* Map.groupBy's step: the element at frame->index appended to the array of
 * `key` in the output map, made when the key is first seen. The key is
 * borrowed, and is any primitive, by SameValueZero; an object faults, as
 * JavaScript keys it by its identity. One lookup through the index. */
__attribute__((noinline)) static bool map_group_add(sjs_vm *vm, vm_frame *frame, sjs_value key) {
    if (!sjs_is_primitive(key)) {
        sjs_vm_fault(vm, "staticjs keys a map by primitives: JavaScript keys an object by its identity");
        return false;
    }
    sjs_value *output = slot(vm, frame, LOOP_OUTPUT);
    sjs_array *m = SJS_AS_ARRAY(*output);
    sjs_value item = receiver(vm, frame)->items[frame->index];
    sjs_value *found = (sjs_value *)(uintptr_t)sjs_map_find(m, key);
    if (found != NULL) {
        sjs_array *group = SJS_AS_ARRAY(*found);
        sjs_ref(item);
        if (array_add(vm, &group, item) != SJS_OK) return false;
        found->as.heap = &group->h;
        return true;
    }
    sjs_array *group;
    if (new_array(vm, 4, &group) != SJS_OK) return false;
    sjs_ref(item);
    group->items[group->length++] = item;
    sjs_ref(key);
    sjs_map_entry entry = { key, sjs_heap_value(SJS_ARRAY, &group->h) };
    sjs_result r = sjs_map_put(vm->context, &m, &entry);
    output->as.heap = &m->h;
    if (r != SJS_OK) {
        sjs_unref(vm->context, key);
        sjs_unref(vm->context, entry.value);
        sjs_vm_fail(vm, r);
        return false;
    }
    return true;
}

/* A forEach's loop, of a map's `length` entries or of an iterator's list:
 * its result is the output it starts with, undefined. */
static sjs_result for_each_start(sjs_vm *vm, vm_frame *frame, uint32_t length) {
    if (!is_callable(*slot(vm, frame, LOOP_CALLBACK))) return sjs_vm_fault(vm, "the callback is not a function");
    frame->index = 0;
    frame->length = length;
    frame->phase = 0;
    frame->entered = false;
    frame->shape = SJS_SHAPE_NONE;
    return SJS_OK;
}

__attribute__((noinline)) static sjs_result late_start(sjs_vm *vm, vm_frame *frame) {
    sjs_value *held = slot(vm, frame, LOOP_RECEIVER);
    if (frame->builtin == SJS_BUILTIN_MAP_GROUP_BY) {
        if (held->kind != SJS_ARRAY) return sjs_vm_fault(vm, "Map.groupBy takes an array");
        sjs_array *out;
        sjs_result r = sjs_map_make(vm->context, 4, &out);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        return set_output(vm, frame, sjs_heap_value(SJS_MAP, &out->h));
    }
    if (frame->builtin == SJS_METHOD_MAP_FOR_EACH) return for_each_start(vm, frame, SJS_AS_ARRAY(*held)->length / 2u);
    /* An iterator's helper: what it yields becomes the receiver, and the
     * slot of the method, which holds nothing, holds its map until the
     * result does (late_done). */
    sjs_array *list;
    if (sjs_iterator_items(vm, *held, &list) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_value source = SJS_AS_ITERATOR(*held)->source;
    sjs_ref(source);
    vm->stack[frame->base + 1u] = source;
    sjs_vm_release(vm, *held);
    *held = sjs_heap_value(SJS_ARRAY, &list->h);
    frame->iterated = true;
    switch (frame->builtin) {
    case SJS_METHOD_ITERATOR_REDUCE:
        if (frame->argc < 2 && list->length == 0) return sjs_vm_fault(vm, "reduce of an iterator that yields nothing needs an initial value");
        frame->builtin = SJS_METHOD_ARRAY_REDUCE;
        return SJS_OK;
    case SJS_METHOD_ITERATOR_MAP:
        frame->builtin = SJS_METHOD_ARRAY_MAP;
        return SJS_OK;
    case SJS_METHOD_ITERATOR_FILTER:
        frame->builtin = SJS_METHOD_ARRAY_FILTER;
        return SJS_OK;
    case SJS_METHOD_ITERATOR_SOME:
        frame->builtin = SJS_METHOD_ARRAY_SOME;
        return SJS_OK;
    case SJS_METHOD_ITERATOR_EVERY:
        frame->builtin = SJS_METHOD_ARRAY_EVERY;
        return SJS_OK;
    case SJS_METHOD_ITERATOR_FIND:
        frame->builtin = SJS_METHOD_ARRAY_FIND;
        return SJS_OK;
    case SJS_METHOD_ITERATOR_FOR_EACH:
        /* A map's forEach's loop, which calls back with the value and the
         * counter (for_each_call) over the list. */
        frame->builtin = SJS_METHOD_MAP_FOR_EACH;
        return for_each_start(vm, frame, list->length);
    default: /* SJS_METHOD_ITERATOR_FLAT_MAP, which keeps its own step */
        return SJS_OK;
    }
}

/* The end of an iterator's helper: map's and filter's array becomes the
 * iterator they give, which holds the map; reduce gives its accumulator. */
__attribute__((noinline)) static loop_step late_done(sjs_vm *vm, vm_frame *frame) {
    if (frame->builtin == SJS_METHOD_ARRAY_REDUCE || frame->builtin == SJS_METHOD_MAP_FOR_EACH) return LOOP_DONE;
    sjs_value *source = &vm->stack[frame->base + 1u];
    sjs_value *output = slot(vm, frame, LOOP_OUTPUT);
    sjs_value it;
    sjs_result r = sjs_iterator_make(vm->context, (sjs_iterator_spec){ *source, *output, SJS_ITERATE_VALUES }, &it);
    if (r != SJS_OK) {
        sjs_vm_fail(vm, r);
        return LOOP_FAULT;
    }
    *source = sjs_undefined();
    *output = it;
    return LOOP_DONE;
}

/* A map's forEach: the callback with (value, key, map) for the entry at
 * frame->index, whose result sjs_loop_step drops; undefined at the end,
 * the output it starts with. The loop holds the map, so the callback cannot
 * change it. */
__attribute__((noinline)) static loop_step for_each_call(sjs_vm *vm, vm_frame *frame) {
    /* An iterator's forEach: the value and the counter, over its list. */
    if (frame->iterated) return call_element(vm, frame, frame->index);
    sjs_value map = *slot(vm, frame, LOOP_RECEIVER);
    const sjs_array *m = SJS_AS_ARRAY(map);
    sjs_value args[3] = { m->items[2u * frame->index + 1u], m->items[2u * frame->index], map };
    return call_back(vm, frame, (sjs_values){ args, 3 });
}

/* ------------------------------------------------------------------------
 * MUTATION: a map's set, delete and clear (docs/mutation.md), called by
 * SJS_OP_MUTATE or SJS_OP_PATH_CALL with the place of the map, which holds
 * it alone. This section exists only for mutation.
 */

/* set(key, value): the key keeps its place and takes the value, or goes in
 * last; the call gives the map (sjs_mutation_returns_receiver). A key that
 * is not a primitive faults, as JavaScript keys an object by its identity;
 * one lookup. delete(key): whether the map had it, which it no longer has;
 * an object is never a key, so it gives false; the lookup, and the map's
 * entries when one moves down (sjs_map_remove). clear(): undefined, a unit
 * for each entry released. The map shrinks as it empties. */
__attribute__((noinline)) static sjs_result map_change(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    sjs_array *m = SJS_AS_ARRAY(*call->receiver_slot);
    sjs_result r = SJS_OK;
    if (id == SJS_METHOD_MAP_CLEAR) {
        call->cost = m->length / 2u;
        sjs_set_clear(vm->context, &m);
    } else if (id == SJS_METHOD_MAP_DELETE) {
        sjs_removal removal = sjs_map_remove(vm->context, &m, arg(call, 0));
        call->cost = removal.cost;
        call->result = sjs_boolean(removal.removed);
    } else {
        sjs_map_entry entry = { arg(call, 0), arg(call, 1) };
        if (!sjs_is_primitive(entry.key)) return sjs_vm_fault(vm, "staticjs keys a map by primitives: JavaScript keys an object by its identity");
        call->cost = map_lookup_cost(m);
        sjs_ref(entry.key);
        sjs_ref(entry.value);
        r = sjs_map_put(vm->context, &m, &entry);
        if (r != SJS_OK) {
            sjs_unref(vm->context, entry.key);
            sjs_unref(vm->context, entry.value);
            r = sjs_vm_fail(vm, r);
        }
    }
    call->receiver_slot->as.heap = &m->h;
    return r;
}

/* ------------------------------------------------------------------------
 * String.raw, encodeURI, encodeURIComponent, decodeURI and
 * decodeURIComponent: leaves over text.
 */

/* A value's text as String gives it, for an argument these built-ins turn
 * into a string: a primitive's own, or a BigInt's digits made into `made`,
 * which the caller releases (text_release). An object faults, as String
 * faults on one. */
typedef struct {
    sjs_str text;
    sjs_string *made;   /* NULL when nothing was made */
    char room[32];
} value_text;

static sjs_result text_of(sjs_vm *vm, sjs_value v, value_text *t) {
    t->made = NULL;
    if (!sjs_is_primitive(v)) return sjs_vm_fault(vm, "staticjs turns only primitives into strings");
    if (v.kind != SJS_BIGINT) {
        t->text = sjs_primitive_text(v, t->room);
        return SJS_OK;
    }
    sjs_result r = sjs_to_string(vm->context, v, &t->made);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    t->text = sjs_string_view(t->made);
    return SJS_OK;
}

static void text_release(sjs_vm *vm, value_text *t) {
    if (t->made != NULL) sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &t->made->h));
    t->made = NULL;
}

/* Append a value's text to the text being built. */
static sjs_result build_value(sjs_vm *vm, text_builder *b, sjs_value v) {
    value_text t;
    if (text_of(vm, v, &t) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_result r = build_text(vm, b, t.text);
    text_release(vm, &t);
    return r;
}

/* The code units of a string's text, one at a time: a character past
 * 0xFFFF gives its two halves in turn. */
typedef struct {
    sjs_str text;
    uint32_t at;     /* the byte the next character starts at */
    uint32_t low;    /* the second half still to give, or 0 */
} unit_cursor;

static uint32_t next_unit(unit_cursor *u) {
    if (u->low != 0) {
        uint32_t low = u->low;
        u->low = 0;
        return low;
    }
    /* A staticjs string is valid UTF-8, so this never fails. */
    uint32_t point = (uint32_t)sjs_utf8_next(u->text, &u->at);
    if (point < 0x10000u) return point;
    u->low = 0xDC00u + ((point - 0x10000u) & 0x3FFu);
    return 0xD800u + ((point - 0x10000u) >> 10);
}

/* String.raw(template, ...substitutions): template.raw's strings, with
 * each substitution between two of them. raw is an array of primitives,
 * or a string, whose code units are the strings; a substitution between
 * the halves of a pair gives a lone surrogate in JavaScript, which faults
 * (text_builder). A template that is not an object, or has no raw, or a
 * null or undefined one, is a TypeError in JavaScript and faults; a
 * number, boolean or BigInt as raw has no length, which JavaScript reads
 * as none. An object or a function as raw faults: JavaScript reads it by
 * its length and indexes. Substitutions past the last gap are never
 * converted, as in JavaScript. Fuel: a unit per raw string and
 * substitution, and per 16 bytes built. */
__attribute__((noinline)) static sjs_result string_raw(sjs_vm *vm, sjs_leaf_call *call) {
    sjs_value t = arg(call, 0);
    const sjs_entry *e = t.kind == SJS_OBJECT ? sjs_object_find(SJS_AS_OBJECT(t), SJS_STR("raw")) : NULL;
    sjs_value raw = e != NULL ? e->value : sjs_undefined();
    if (raw.kind == SJS_UNDEFINED || raw.kind == SJS_NULL) {
        return sjs_vm_fault(vm, "String.raw: the template has no raw strings (a TypeError in JavaScript)");
    }
    uint32_t count;
    unit_cursor u = { { NULL, 0 }, 0, 0 };
    if (raw.kind == SJS_ARRAY) {
        count = SJS_AS_ARRAY(raw)->length;
    } else if (raw.kind == SJS_STRING) {
        u.text = sjs_string_view(SJS_AS_STRING(raw));
        count = SJS_AS_STRING(raw)->utf16_length;
    } else if (sjs_is_primitive(raw)) {
        count = 0;
    } else {
        return sjs_vm_fault(vm, "String.raw: staticjs reads raw strings only from an array or a string");
    }
    if (string_with_room(vm, call, 0) != SJS_OK) return SJS_ERR_PROGRAM;
    text_builder b = { call, 0 };
    for (uint32_t i = 0; i < count; i++) {
        call->cost += 1u;
        sjs_result r = raw.kind == SJS_ARRAY ? build_value(vm, &b, SJS_AS_ARRAY(raw)->items[i]) : build_unit(vm, &b, next_unit(&u));
        if (r != SJS_OK) return SJS_ERR_PROGRAM;
        if (i + 1u == count || i + 1u >= call->count) continue;
        call->cost += 1u;
        if (build_value(vm, &b, call->args[i + 1u]) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    return build_end(vm, &b);
}

/* Whether an encoder keeps an ASCII byte as it is: the unreserved
 * characters, and for encodeURI (`whole`) the reserved ones and `#`. */
static bool uri_keeps(uint8_t c, bool whole) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    switch (c) {
    case '-': case '_': case '.': case '!': case '~': case '*': case '\'': case '(': case ')':
        return true;
    case ';': case '/': case '?': case ':': case '@': case '&': case '=': case '+': case '$': case ',': case '#':
        return whole;
    default:
        return false;
    }
}

/* encodeURI(text) and encodeURIComponent(text): each byte of the text's
 * UTF-8 that the encoder does not keep, as %XX in upper-case hex. That is
 * JavaScript's escape of each character's UTF-8; a lone surrogate, a
 * URIError in JavaScript, cannot be in a staticjs string. A string with
 * nothing to escape is given back as it is. Fuel: a unit per 16 bytes
 * read, and per 16 bytes written. */
__attribute__((noinline)) static sjs_result uri_encode(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    static const char hex[] = "0123456789ABCDEF";
    bool whole = id == SJS_BUILTIN_ENCODE_URI;
    sjs_value x = arg(call, 0);
    value_text t;
    if (text_of(vm, x, &t) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_str s = t.text;
    call->cost += s.length / 16u;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < s.length; i++) bytes += uri_keeps((uint8_t)s.ptr[i], whole) ? 1u : 3u;
    sjs_result r = SJS_OK;
    if (bytes == s.length && x.kind == SJS_STRING) {
        sjs_ref(x);
        call->result = x;
    } else if (string_with_room(vm, call, bytes) != SJS_OK) {
        r = SJS_ERR_PROGRAM;
    } else {
        sjs_string *out = SJS_AS_STRING(call->result);
        uint32_t run = 0;
        for (uint32_t i = 0; i < s.length; i++) {
            uint8_t c = (uint8_t)s.ptr[i];
            if (uri_keeps(c, whole)) continue;
            sjs_string_extend(out, (sjs_str){ s.ptr + run, i - run });
            char escape[3] = { '%', hex[c >> 4], hex[c & 15u] };
            sjs_string_extend(out, (sjs_str){ escape, 3 });
            run = i + 1u;
        }
        sjs_string_extend(out, (sjs_str){ s.ptr + run, s.length - run });
    }
    text_release(vm, &t);
    return r;
}

static int32_t hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The byte the escape %XX at `at` stands for, or -1 if there is none. */
static int32_t escaped_byte(sjs_str s, uint32_t at) {
    if (at >= s.length || s.length - at < 3u || s.ptr[at] != '%') return -1;
    int32_t high = hex_digit(s.ptr[at + 1u]);
    int32_t low = hex_digit(s.ptr[at + 2u]);
    return high < 0 || low < 0 ? -1 : high * 16 + low;
}

/* One character's escapes: its UTF-8 bytes and how many, each escaped in
 * three bytes of the text. */
typedef struct {
    uint8_t bytes[4];
    uint32_t count;
} uri_character;

/* The character whose escapes start at `at`, as valid UTF-8 (no overlong
 * form, no surrogate, nothing past 0x10FFFF); false for a malformed
 * sequence. */
static bool uri_character_at(sjs_str s, uint32_t at, uri_character *c) {
    int32_t first = escaped_byte(s, at);
    if (first < 0) return false;
    uint32_t n = first < 0x80 ? 1u : first >= 0xC0 && first < 0xE0 ? 2u : first >= 0xE0 && first < 0xF0 ? 3u : first >= 0xF0 && first < 0xF8 ? 4u : 0u;
    if (n == 0) return false;
    c->bytes[0] = (uint8_t)first;
    for (uint32_t k = 1; k < n; k++) {
        int32_t next = escaped_byte(s, at + 3u * k);
        if (next < 0 || (next & 0xC0) != 0x80) return false;
        c->bytes[k] = (uint8_t)next;
    }
    uint32_t end = 0;
    if (sjs_utf8_next((sjs_str){ (const char *)c->bytes, n }, &end) < 0 || end != n) return false;
    c->count = n;
    return true;
}

/* decodeURI(text) and decodeURIComponent(text): each run of %XX escapes
 * of a character's UTF-8, in either case, as the character. decodeURI
 * keeps the escape of a reserved character or `#` as written. A malformed
 * sequence, a URIError in JavaScript, faults. The result is never longer
 * than the text. Fuel: a unit per 16 bytes read, and per 16 bytes of room
 * for the result. */
__attribute__((noinline)) static sjs_result uri_decode(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    bool whole = id == SJS_BUILTIN_DECODE_URI;
    value_text t;
    if (text_of(vm, arg(call, 0), &t) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_str s = t.text;
    call->cost += s.length / 16u;
    sjs_result r = string_with_room(vm, call, s.length);
    uint32_t run = 0;
    uint32_t i = 0;
    while (r == SJS_OK && i < s.length) {
        if (s.ptr[i] != '%') {
            i++;
            continue;
        }
        sjs_string *out = SJS_AS_STRING(call->result);
        sjs_string_extend(out, (sjs_str){ s.ptr + run, i - run });
        uri_character c;
        if (!uri_character_at(s, i, &c)) {
            r = sjs_vm_fault(vm, "a malformed URI sequence (a URIError in JavaScript)");
            break;
        }
        /* The reserved characters are what encodeURI keeps and
         * encodeURIComponent escapes. */
        bool keep = whole && c.count == 1u && uri_keeps(c.bytes[0], true) && !uri_keeps(c.bytes[0], false);
        sjs_string_extend(out, keep ? (sjs_str){ s.ptr + i, 3u } : (sjs_str){ (const char *)c.bytes, c.count });
        i += 3u * c.count;
        run = i;
    }
    if (r == SJS_OK) sjs_string_extend(SJS_AS_STRING(call->result), (sjs_str){ s.ptr + run, s.length - run });
    text_release(vm, &t);
    return r == SJS_OK ? SJS_OK : SJS_ERR_PROGRAM;
}

/* ------------------------------------------------------------------------
 * Regular expressions' methods (regex_match.c): a leaf, which hands a long
 * match to a loop of its own (SJS_LEAF_LOOP).
 */

__attribute__((noinline)) static sjs_result regex_method(sjs_vm *vm, uint8_t op, sjs_leaf_call *call) {
    /* The fuel is the match's, alike as a leaf and as a loop. */
    call->cost = 0;
    sjs_regex_call_params params = { call, op, false };
    return sjs_regex_call(vm, &params);
}

/* ------------------------------------------------------------------------
 * An iterator's flatMap's step and its take and drop, out of line at the end
 * of the file, so that the code before is laid out as it was.
 */

/* flatMap's answer for one value, appended to the output: what JavaScript
 * iterates of it, an array's elements, a set's members, a map's entries
 * (new [key, value] arrays, a unit each) or what an iterator yields (read
 * once, sjs_iterator_items). JavaScript throws a TypeError for a primitive,
 * a string included, and for an object with no iterator; staticjs faults
 * on both, and on any other object, whose iterator would be a user's
 * (README.md: user iterables are refused). Takes over `r`. */
__attribute__((noinline)) static bool flatten_iterated(sjs_vm *vm, vm_frame *frame, sjs_value r) {
    sjs_array *list = NULL;
    sjs_result got = SJS_OK;
    if (r.kind == SJS_ARRAY || r.kind == SJS_SET) {
        sjs_ref(r);
        list = SJS_AS_ARRAY(r);
    } else if (r.kind == SJS_MAP) {
        got = sjs_map_entries(vm, SJS_AS_ARRAY(r), &list);
    } else if (r.kind == SJS_ITERATOR) {
        got = sjs_iterator_items(vm, r, &list);
    } else {
        got = sjs_vm_fault(vm, "flatMap's callback returns an array, a set, a map or an iterator: JavaScript throws a TypeError for a primitive, a string included, or a plain object");
    }
    sjs_vm_release(vm, r);
    if (got != SJS_OK) return false;
    sjs_value *output = slot(vm, frame, LOOP_OUTPUT);
    sjs_array *out = SJS_AS_ARRAY(*output);
    sjs_result added = SJS_OK;
    for (uint32_t i = 0; added == SJS_OK && i < list->length; i++) {
        sjs_ref(list->items[i]);
        added = array_add(vm, &out, list->items[i]);
        output->as.heap = &out->h;
    }
    sjs_unref(vm->context, sjs_heap_value(SJS_ARRAY, &list->h));
    return added == SJS_OK;
}

/* take(limit) and drop(limit): an iterator of the first `limit` values the
 * receiver yields, or of the rest, which holds its map. The limit is
 * ToNumber'd as JavaScript does it, so null is 0 and true is 1, and a
 * string or an object faults (sjs_vm_to_number); NaN, which undefined
 * becomes, and a negative limit fault where JavaScript throws a
 * RangeError. A fraction is cut toward zero (-0.5 is 0), and Infinity takes
 * or drops everything. A unit of fuel for each value copied
 * (iterator_range): JavaScript's take reads no further than the limit, and
 * a map's own iterator copies no more. */
__attribute__((noinline)) static sjs_result iterator_limit(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call) {
    double d;
    if (sjs_vm_to_number(vm, arg(call, 0), &d) != SJS_OK) return SJS_ERR_PROGRAM;
    if (sjs_number_is_nan(d)) return sjs_vm_fault(vm, "take and drop take a limit that is a number, not NaN: JavaScript throws a RangeError");
    d = sjs_number_trunc(d);
    if (d < 0) return sjs_vm_fault(vm, "take and drop take a limit that is not negative: JavaScript throws a RangeError");
    uint32_t limit = d >= (double)UINT32_MAX ? UINT32_MAX : (uint32_t)d;
    item_range range = id == SJS_METHOD_ITERATOR_TAKE ? (item_range){ call->receiver, 0, limit } : (item_range){ call->receiver, limit, UINT32_MAX };
    sjs_array *a;
    if (iterator_range(vm, range, &a) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_value source = SJS_AS_ITERATOR(call->receiver)->source;
    sjs_value items = sjs_heap_value(SJS_ARRAY, &a->h);
    sjs_ref(source);
    sjs_result r = sjs_iterator_make(vm->context, (sjs_iterator_spec){ source, items, SJS_ITERATE_VALUES }, &call->result);
    if (r == SJS_OK) return SJS_OK;
    sjs_unref(vm->context, source);
    sjs_unref(vm->context, items);
    return sjs_vm_fail(vm, r);
}
