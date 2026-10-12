/*
 * Values: immutable once shared, reference counted, freed through a worklist
 * so that releasing a deeply nested value never recurses. Nothing can form a
 * cycle, because nothing can change a value another value already holds.
 */
#include "internal.h"

sjs_value sjs_undefined(void) {
    sjs_value v = { 0 };
    v.kind = SJS_UNDEFINED;
    return v;
}

sjs_value sjs_null(void) {
    sjs_value v = { 0 };
    v.kind = SJS_NULL;
    return v;
}

sjs_value sjs_boolean(bool value) {
    sjs_value v = { 0 };
    v.kind = SJS_BOOLEAN;
    v.boolean = value ? 1u : 0u;
    return v;
}

sjs_value sjs_number(double value) {
    sjs_value v = { 0 };
    v.kind = SJS_NUMBER;
    v.as.number = value;
    return v;
}

void sjs_retain(sjs_value value) {
    sjs_ref(value);
}

static void intern_remove(sjs_context *context, const sjs_string *string);
__attribute__((always_inline)) static inline bool same_value_zero(sjs_value a, sjs_value b);

/* Drop one reference; a heap block whose last reference this was joins `work`.
 * An interned string leaves its table first: it needs its h.link for that. */
static void drop_heap(sjs_context *context, sjs_heap **work, sjs_heap *h) {
    if (h == NULL || h->refs == SJS_IMMORTAL) return;
    if (--h->refs != 0) return;
    if (h->type == SJS_HEAP_STRING && (h->flags & SJS_STRING_INTERNED) != 0) {
        intern_remove(context, (const sjs_string *)(const void *)h);
    }
    h->link = *work;
    *work = h;
}

static void drop(sjs_context *context, sjs_heap **work, sjs_value value) {
    if (!sjs_is_heap(value)) return;
    sjs_heap *h = value.as.heap;
    if (h->refs == SJS_IMMORTAL) return;
    drop_heap(context, work, h);
}

static void drop_program(sjs_context *context, sjs_heap **work, sjs_program *program) {
    if (program == NULL || --program->refs != 0) return;
    for (uint32_t i = 0; i < program->constant_count; i++) drop(context, work, program->constants[i]);
    sjs_mem_free(&program->context->counting, program, program->size);
}

static size_t lookup_size(uint32_t mask) {
    return sizeof(sjs_index) + ((size_t)mask + 1u) * sizeof(uint32_t);
}

static void lookup_free(sjs_context *context, sjs_index *index) {
    if (index != NULL) sjs_mem_free(&context->counting, index, lookup_size(index->mask));
}

static void drop_side(sjs_context *context, sjs_heap **work, sjs_array *a);

/* Free everything on the worklist, and what it alone held. */
static void drain(sjs_context *context, sjs_heap *work) {
    while (work != NULL) {
        sjs_heap *h = work;
        work = h->link;
        switch (h->type) {
        case SJS_HEAP_ARRAY:
        case SJS_HEAP_SET:
        case SJS_HEAP_MAP: {   /* a map's keys and values alike */
            sjs_array *a = (sjs_array *)(void *)h;
            for (uint32_t i = 0; i < a->length; i++) drop(context, &work, a->items[i]);
            drop_side(context, &work, a);
            break;
        }
        case SJS_HEAP_OBJECT: {
            sjs_object *o = (sjs_object *)(void *)h;
            for (uint32_t i = 0; i < o->count; i++) {
                drop_heap(context, &work, &o->entries[i].key->h);
                drop(context, &work, o->entries[i].value);
            }
            lookup_free(context, o->index);
            break;
        }
        case SJS_HEAP_ITERATOR: {
            sjs_iterator *it = (sjs_iterator *)(void *)h;
            drop(context, &work, it->source);
            drop(context, &work, it->items);
            break;
        }
        case SJS_HEAP_CLOSURE: {
            sjs_closure *c = (sjs_closure *)(void *)h;
            for (uint32_t i = 0; i < c->count; i++) drop(context, &work, c->captures[i]);
            drop_program(context, &work, c->program);
            break;
        }
        default:
            break;
        }
        sjs_mem_free(&context->counting, h, h->size);
    }
}

void sjs_release(sjs_context *context, sjs_value value) {
    sjs_heap *work = NULL;
    drop(context, &work, value);
    drain(context, work);
}

void sjs_program_retain(sjs_program *program) {
    if (program != NULL) program->refs++;
}

void sjs_program_release(sjs_program *program) {
    if (program == NULL) return;
    sjs_context *context = program->context;
    sjs_heap *work = NULL;
    drop_program(context, &work, program);
    drain(context, work);
}

static void *heap_alloc(sjs_context *context, size_t size, uint8_t type) {
    if (size > UINT32_MAX) return NULL;
    sjs_heap *h = sjs_mem_alloc(&context->counting, size);
    if (h == NULL) return NULL;
    sjs_zero(h, sizeof *h);
    h->refs = 1;
    h->type = type;
    h->size = (uint32_t)size;
    return h;
}

/* ------------------------------------------------------------------------
 * Strings
 */

static bool parse_index(sjs_str text, uint32_t *out) {
    if (text.length == 0 || text.length > 10) return false;
    if (text.length > 1 && text.ptr[0] == '0') return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < text.length; i++) {
        char c = text.ptr[i];
        if (c < '0' || c > '9') return false;
        v = v * 10u + (uint64_t)(c - '0');
    }
    if (v > 4294967294ull) return false;
    *out = (uint32_t)v;
    return true;
}

sjs_result sjs_string_make(sjs_context *context, sjs_str text, sjs_string **out) {
    if (text.length > context->limits.max_string_bytes) return SJS_ERR_LIMIT;
    sjs_string *s = heap_alloc(context, sizeof(sjs_string) + text.length + 1u, SJS_HEAP_STRING);
    if (s == NULL) return SJS_ERR_NOMEM;
    s->length = text.length;
    sjs_copy(s->bytes, text.ptr, text.length);
    s->bytes[text.length] = '\0';
    bool ascii = true;
    for (uint32_t i = 0; i < text.length; i++) {
        if ((uint8_t)text.ptr[i] >= 0x80u) ascii = false;
    }
    s->utf16_length = ascii ? text.length : sjs_utf16_length(text);
    if (ascii) s->h.flags |= SJS_STRING_ASCII;
    if (parse_index(text, &s->index)) s->h.flags |= SJS_STRING_INDEX;
    *out = s;
    return SJS_OK;
}

sjs_result sjs_string_new(sjs_context *context, sjs_str text, sjs_value *out) {
    if (context == NULL || out == NULL || (text.ptr == NULL && text.length != 0)) return SJS_ERR_INVALID;
    if (!sjs_utf8_valid(text)) return SJS_ERR_INVALID;
    sjs_string *s;
    sjs_result r = sjs_string_make(context, text, &s);
    if (r != SJS_OK) return r;
    *out = sjs_heap_value(SJS_STRING, &s->h);
    return SJS_OK;
}

sjs_str sjs_string_text(sjs_value string) {
    if (string.kind != SJS_STRING || string.internal != 0) return (sjs_str){ "", 0 };
    return sjs_string_view(SJS_AS_STRING(string));
}

sjs_result sjs_string_from_number(sjs_context *context, double value, sjs_string **out) {
    char text[32];
    uint32_t length = sjs_number_format(value, text);
    return sjs_string_make(context, (sjs_str){ text, length }, out);
}

/* ------------------------------------------------------------------------
 * Hash indexes of sets and objects
 */

static uint32_t hash_bytes(sjs_str text) {
    uint32_t h = 2166136261u;   /* FNV-1a */
    for (uint32_t i = 0; i < text.length; i++) h = (h ^ (uint8_t)text.ptr[i]) * 16777619u;
    return h;
}

/* ------------------------------------------------------------------------
 * Interned strings
 */

sjs_result sjs_string_intern(sjs_context *context, sjs_str text, sjs_string **out) {
    uint32_t bucket = hash_bytes(text) & (SJS_INTERN_BUCKETS - 1u);
    uint32_t chain = 0;
    for (sjs_string *s = context->interned[bucket]; s != NULL; s = (sjs_string *)(void *)s->h.link) {
        if (sjs_str_equal(sjs_string_view(s), text)) {
            if (s->h.refs != SJS_IMMORTAL) s->h.refs++;
            *out = s;
            return SJS_OK;
        }
        chain++;
    }
    sjs_result r = sjs_string_make(context, text, out);
    if (r != SJS_OK || chain >= SJS_INTERN_CHAIN_MAX) return r;
    (*out)->h.link = (sjs_heap *)(void *)context->interned[bucket];
    context->interned[bucket] = *out;
    (*out)->h.flags |= SJS_STRING_INTERNED;
    return SJS_OK;
}

/* Take a string being freed out of its chain. */
static void intern_remove(sjs_context *context, const sjs_string *string) {
    sjs_string **at = &context->interned[hash_bytes(sjs_string_view(string)) & (SJS_INTERN_BUCKETS - 1u)];
    while (*at != string) at = (sjs_string **)(void *)&(*at)->h.link;
    *at = (sjs_string *)(void *)string->h.link;
}

/* Equal under SameValueZero, equal hashes: every NaN hashes alike, and -0
 * as 0. */
static uint32_t hash_value(sjs_value v) {
    switch (v.kind) {
    case SJS_NUMBER: {
        union {
            double d;
            uint64_t u;
        } bits;
        bits.d = v.as.number;
        if (sjs_number_is_nan(bits.d)) return 0x7ff80000u;
        if (bits.d == 0) bits.u = 0;
        uint64_t u = bits.u;
        u ^= u >> 33;
        u *= 0xff51afd7ed558ccdull;
        u ^= u >> 33;
        return (uint32_t)u;
    }
    case SJS_STRING:
        if (v.internal != SJS_INTERNAL_NONE) return 0;
        return hash_bytes(sjs_string_view(SJS_AS_STRING(v)));
    case SJS_BOOLEAN:
        return 2u + (v.boolean != 0);
    default:
        /* A BigInt here, not as a case, keeps the switch as it was. */
        return v.kind == SJS_BIGINT ? sjs_bigint_hash(SJS_AS_BIGINT(v)) : v.kind;
    }
}

static sjs_index **lookup_field(sjs_heap *h) {
    if (h->type == SJS_HEAP_OBJECT) return &((sjs_object *)(void *)h)->index;
    return &((sjs_array *)(void *)h)->index;
}

/* The items an index is over: an object's keys, a set's elements, a map's
 * entries, whose keys are its even items. */
static uint32_t item_count(const sjs_heap *h) {
    if (h->type == SJS_HEAP_OBJECT) return ((const sjs_object *)(const void *)h)->count;
    uint32_t length = ((const sjs_array *)(const void *)h)->length;
    return h->type == SJS_HEAP_MAP ? length / 2u : length;
}

static uint32_t item_hash(const sjs_heap *h, uint32_t i) {
    if (h->type == SJS_HEAP_OBJECT) {
        return hash_bytes(sjs_string_view(((const sjs_object *)(const void *)h)->entries[i].key));
    }
    return hash_value(((const sjs_array *)(const void *)h)->items[h->type == SJS_HEAP_MAP ? 2u * i : i]);
}

static void lookup_put(sjs_index *index, uint32_t hash, uint32_t position) {
    uint32_t i = hash & index->mask;
    while (index->slots[i] != 0) i = (i + 1u) & index->mask;
    index->slots[i] = position + 1u;
}

/* Index every item of the container, in a table at most a quarter full.
 * When the table cannot be allocated, the container goes without one. */
static void lookup_build(sjs_context *context, sjs_heap *h) {
    sjs_index **field = lookup_field(h);
    uint32_t count = item_count(h);
    sjs_index *index = *field;
    uint32_t slots = 16u;
    while (slots / 4u < count && slots < 0x40000000u) slots *= 2u;
    if (slots / 2u < count) {
        /* More items than any table here holds at half load: scan. */
        lookup_free(context, index);
        *field = NULL;
        h->flags |= SJS_CONTAINER_UNINDEXED;
        return;
    }
    if (index == NULL || index->mask != slots - 1u) {
        lookup_free(context, index);
        *field = NULL;
        /* Only reachable where size_t is 32 bits (wasm32): there a large
           table's size could wrap. */
        index = (uint64_t)slots * sizeof(uint32_t) + sizeof(sjs_index) > (size_t)-1
                    ? NULL
                    : sjs_mem_alloc(&context->counting, lookup_size(slots - 1u));
        if (index == NULL) {
            h->flags |= SJS_CONTAINER_UNINDEXED;
            return;
        }
        index->mask = slots - 1u;
        *field = index;
    }
    sjs_zero(index->slots, (size_t)slots * sizeof(uint32_t));
    for (uint32_t i = 0; i < count; i++) lookup_put(index, item_hash(h, i), i);
}

/* The container's last item has just gone in at the end. */
static void lookup_appended(sjs_context *context, sjs_heap *h) {
    uint32_t count = item_count(h);
    sjs_index *index = *lookup_field(h);
    if (index == NULL) {
        if (count >= SJS_INDEX_FROM && (h->flags & SJS_CONTAINER_UNINDEXED) == 0) lookup_build(context, h);
        return;
    }
    if ((uint64_t)count * 2u > (uint64_t)index->mask + 1u) {
        lookup_build(context, h);
        return;
    }
    lookup_put(index, item_hash(h, count - 1u), count - 1u);
}

/* ------------------------------------------------------------------------
 * Arrays and sets
 */

/* An array or a set, empty, with room for `capacity` items. */
typedef struct {
    uint32_t capacity;
    uint8_t type;
} shape;

static sjs_result array_alloc(sjs_context *context, shape s, sjs_array **out) {
    uint32_t capacity = s.capacity;
    if (capacity > context->limits.max_value_items) return SJS_ERR_LIMIT;
    sjs_array *a = heap_alloc(context, sizeof(sjs_array) + (size_t)capacity * sizeof(sjs_value), s.type);
    if (a == NULL) return SJS_ERR_NOMEM;
    a->length = 0;
    a->capacity = capacity;
    a->index = NULL;
    *out = a;
    return SJS_OK;
}

sjs_result sjs_array_make(sjs_context *context, uint32_t capacity, sjs_array **out) {
    return array_alloc(context, (shape){ capacity, SJS_HEAP_ARRAY }, out);
}

sjs_result sjs_array_grow(sjs_context *context, sjs_array **array, uint32_t extra) {
    sjs_array *a = *array;
    if (extra > context->limits.max_value_items || a->length > context->limits.max_value_items - extra) return SJS_ERR_LIMIT;
    uint32_t need = a->length + extra;
    if (need <= a->capacity) return SJS_OK;
    uint32_t capacity = a->capacity < 4u ? 4u : a->capacity;
    while (capacity < need) capacity = capacity > UINT32_MAX / 2u ? need : capacity * 2u;
    if (capacity > context->limits.max_value_items) capacity = context->limits.max_value_items;
    sjs_array *grown;
    sjs_result r = array_alloc(context, (shape){ capacity, a->h.type }, &grown);
    if (r != SJS_OK) return r;
    grown->length = a->length;
    sjs_copy(grown->items, a->items, (size_t)a->length * sizeof(sjs_value));
    grown->h.flags = a->h.flags;
    grown->index = a->index;
    sjs_mem_free(&context->counting, a, a->h.size);
    *array = grown;
    return SJS_OK;
}

sjs_result sjs_array_new(sjs_context *context, uint32_t capacity, sjs_value *out) {
    if (context == NULL || out == NULL) return SJS_ERR_INVALID;
    sjs_array *a;
    sjs_result r = sjs_array_make(context, capacity, &a);
    if (r != SJS_OK) return r;
    *out = sjs_heap_value(SJS_ARRAY, &a->h);
    return SJS_OK;
}

sjs_result sjs_array_append(sjs_context *context, sjs_value *array, sjs_value element) {
    if (context == NULL || array == NULL || array->kind != SJS_ARRAY || array->internal != 0) return SJS_ERR_INVALID;
    if (element.internal != 0) return SJS_ERR_INVALID;
    sjs_array *a = SJS_AS_ARRAY(*array);
    if (a->h.refs != 1 || (a->h.flags & SJS_INTEGRITY) != 0) return SJS_ERR_INVALID;
    sjs_result r = sjs_array_grow(context, &a, 1);
    if (r != SJS_OK) return r;
    a->items[a->length++] = element;
    array->as.heap = &a->h;
    return SJS_OK;
}

/* Where an item is: its position (an entry's, in a map), and its slot in
 * the index, or UINT32_MAX for none (not found; no index). */
typedef struct {
    uint32_t position;
    uint32_t slot;
} lookup_hit;

/* Where `key` is among a set's elements (stride 1) or a map's keys (stride
 * 2: its even items), under SameValueZero. Inlined with its stride, so a
 * set's lookup compiles as it did before maps shared it. */
__attribute__((always_inline)) static inline lookup_hit keyed_hit(const sjs_array *a, sjs_value key, uint32_t stride) {
    lookup_hit hit = { UINT32_MAX, UINT32_MAX };
    const sjs_index *index = a->index;
    if (index != NULL) {
        for (uint32_t i = hash_value(key) & index->mask; index->slots[i] != 0; i = (i + 1u) & index->mask) {
            if (same_value_zero(a->items[(index->slots[i] - 1u) * stride], key)) {
                hit.position = index->slots[i] - 1u;
                hit.slot = i;
                return hit;
            }
        }
        return hit;
    }
    for (uint32_t i = 0; i < a->length; i += stride) {
        if (same_value_zero(a->items[i], key)) {
            hit.position = i / stride;
            return hit;
        }
    }
    return hit;
}

/* A set's own scan, as it was before keyed_hit: the same lookup, but it
 * answers a bool, which compiles to tighter code on the sets' hot paths. */
bool sjs_set_contains(const sjs_array *set, sjs_value element) {
    const sjs_index *index = set->index;
    if (index != NULL) {
        for (uint32_t i = hash_value(element) & index->mask; index->slots[i] != 0; i = (i + 1u) & index->mask) {
            if (same_value_zero(set->items[index->slots[i] - 1u], element)) return true;
        }
        return false;
    }
    for (uint32_t i = 0; i < set->length; i++) {
        if (same_value_zero(set->items[i], element)) return true;
    }
    return false;
}

sjs_result sjs_set_insert(sjs_context *context, sjs_array **set, sjs_value element) {
    if (!sjs_is_primitive(element)) return SJS_ERR_PROGRAM;
    if (element.kind == SJS_NUMBER && element.as.number == 0) element.as.number = 0.0;
    if (sjs_set_contains(*set, element)) {
        sjs_release(context, element);
        return SJS_OK;
    }
    sjs_result r = sjs_array_grow(context, set, 1);
    if (r != SJS_OK) return r;
    (*set)->items[(*set)->length++] = element;
    lookup_appended(context, &(*set)->h);
    return SJS_OK;
}

sjs_result sjs_set_new(sjs_context *context, uint32_t capacity, sjs_value *out) {
    if (context == NULL || out == NULL) return SJS_ERR_INVALID;
    sjs_array *a;
    sjs_result r = array_alloc(context, (shape){ capacity, SJS_HEAP_SET }, &a);
    if (r != SJS_OK) return r;
    *out = sjs_heap_value(SJS_SET, &a->h);
    return SJS_OK;
}

sjs_result sjs_set_add(sjs_context *context, sjs_value *set, sjs_value element) {
    if (context == NULL || set == NULL || set->kind != SJS_SET || set->internal != 0) return SJS_ERR_INVALID;
    sjs_array *a = SJS_AS_ARRAY(*set);
    if (a->h.refs != 1 || element.internal != 0) return SJS_ERR_INVALID;
    sjs_result r = sjs_set_insert(context, &a, element);
    if (r == SJS_ERR_PROGRAM) return SJS_ERR_INVALID;
    set->as.heap = &a->h;
    return r;
}

/* ------------------------------------------------------------------------
 * Objects
 */

static sjs_result object_alloc(sjs_context *context, uint32_t capacity, sjs_object **out) {
    if (capacity > context->limits.max_value_items) return SJS_ERR_LIMIT;
    sjs_object *o = heap_alloc(context, sizeof(sjs_object) + (size_t)capacity * sizeof(sjs_entry), SJS_HEAP_OBJECT);
    if (o == NULL) return SJS_ERR_NOMEM;
    o->count = 0;
    o->capacity = capacity;
    o->index = NULL;
    *out = o;
    return SJS_OK;
}

sjs_result sjs_object_make(sjs_context *context, uint32_t capacity, sjs_object **out) {
    return object_alloc(context, capacity, out);
}

sjs_result sjs_object_grow(sjs_context *context, sjs_object **object, uint32_t extra) {
    sjs_object *o = *object;
    if (extra > context->limits.max_value_items || o->count > context->limits.max_value_items - extra) return SJS_ERR_LIMIT;
    uint32_t need = o->count + extra;
    if (need <= o->capacity) return SJS_OK;
    uint32_t capacity = o->capacity < 4u ? 4u : o->capacity;
    while (capacity < need) capacity = capacity > UINT32_MAX / 2u ? need : capacity * 2u;
    if (capacity > context->limits.max_value_items) capacity = context->limits.max_value_items;
    sjs_object *grown;
    sjs_result r = object_alloc(context, capacity, &grown);
    if (r != SJS_OK) return r;
    grown->count = o->count;
    grown->index = o->index;
    grown->h.flags = o->h.flags;
    sjs_copy(grown->entries, o->entries, (size_t)o->count * sizeof(sjs_entry));
    sjs_mem_free(&context->counting, o, o->h.size);
    *object = grown;
    return SJS_OK;
}

const sjs_entry *sjs_object_find(const sjs_object *object, sjs_str key) {
    const sjs_index *index = object->index;
    if (index != NULL) {
        for (uint32_t i = hash_bytes(key) & index->mask; index->slots[i] != 0; i = (i + 1u) & index->mask) {
            const sjs_entry *e = &object->entries[index->slots[i] - 1u];
            if (sjs_str_equal(sjs_string_view(e->key), key)) return e;
        }
        return NULL;
    }
    for (uint32_t i = 0; i < object->count; i++) {
        if (sjs_str_equal(sjs_string_view(object->entries[i].key), key)) return &object->entries[i];
    }
    return NULL;
}

sjs_result sjs_object_put_key(sjs_context *context, sjs_object **object, sjs_put put) {
    sjs_object *o = *object;
    sjs_entry *found = (sjs_entry *)sjs_object_find(o, sjs_string_view(put.key));
    if (found != NULL) {
        sjs_value old = found->value;
        found->value = put.value;
        sjs_release(context, old);
        sjs_release(context, sjs_heap_value(SJS_STRING, &put.key->h));
        return SJS_OK;
    }
    sjs_result r = sjs_object_grow(context, object, 1);
    if (r != SJS_OK) return r;
    o = *object;
    uint32_t at = o->count;
    if ((put.key->h.flags & SJS_STRING_INDEX) != 0) {
        /* Array indices first, ascending. */
        at = 0;
        while (at < o->count && (o->entries[at].key->h.flags & SJS_STRING_INDEX) != 0 &&
               o->entries[at].key->index < put.key->index) {
            at++;
        }
        for (uint32_t i = o->count; i > at; i--) o->entries[i] = o->entries[i - 1];
    }
    o->entries[at].key = put.key;
    o->entries[at].value = put.value;
    o->count++;
    if (at == o->count - 1u) {
        lookup_appended(context, &o->h);
    } else if (o->index != NULL) {
        lookup_build(context, &o->h);   /* the entries after `at` moved */
    }
    return SJS_OK;
}

sjs_result sjs_object_literal(sjs_context *context, sjs_literal literal, sjs_value *out) {
    uint32_t n = literal.keys == NULL ? 0 : literal.keys->length;
    sjs_object *o;
    sjs_result r = object_alloc(context, literal.capacity, &o);
    if (r != SJS_OK) return r;
    for (uint32_t i = 0; i < n; i++) {
        sjs_string *key = SJS_AS_STRING(literal.keys->items[i]);
        sjs_ref(literal.keys->items[i]);
        o->entries[i].key = key;
        o->entries[i].value = literal.values[i];
    }
    o->count = n;
    if (n >= SJS_INDEX_FROM) lookup_build(context, &o->h);
    *out = sjs_heap_value(SJS_OBJECT, &o->h);
    return SJS_OK;
}

sjs_result sjs_object_new(sjs_context *context, uint32_t capacity, sjs_value *out) {
    if (context == NULL || out == NULL) return SJS_ERR_INVALID;
    sjs_object *o;
    sjs_result r = sjs_object_make(context, capacity, &o);
    if (r != SJS_OK) return r;
    *out = sjs_heap_value(SJS_OBJECT, &o->h);
    return SJS_OK;
}

sjs_result sjs_object_put(sjs_context *context, sjs_value *object, const sjs_property *property) {
    if (context == NULL || object == NULL || property == NULL || object->kind != SJS_OBJECT || object->internal != 0) {
        return SJS_ERR_INVALID;
    }
    sjs_object *o = SJS_AS_OBJECT(*object);
    if (o->h.refs != 1 || (o->h.flags & SJS_INTEGRITY) != 0 || property->value.internal != 0 || !sjs_utf8_valid(property->key)) {
        return SJS_ERR_INVALID;
    }
    if (sjs_str_equal(property->key, SJS_STR("__proto__"))) return SJS_ERR_INVALID;
    sjs_string *key;
    sjs_result r = sjs_string_intern(context, property->key, &key);
    if (r != SJS_OK) return r;
    r = sjs_object_put_key(context, &o, (sjs_put){ key, property->value });
    if (r != SJS_OK) {
        sjs_release(context, sjs_heap_value(SJS_STRING, &key->h));
        return r;
    }
    object->as.heap = &o->h;
    return SJS_OK;
}

uint32_t sjs_length(sjs_value value) {
    if (value.internal != 0) return 0;
    switch (value.kind) {
    case SJS_ARRAY:
    case SJS_SET:
        return SJS_AS_ARRAY(value)->length;
    case SJS_OBJECT:
        return SJS_AS_OBJECT(value)->count;
    case SJS_MAP:
        return SJS_AS_ARRAY(value)->length / 2u;
    default:
        return 0;
    }
}

sjs_value sjs_array_at(sjs_value array, uint32_t index) {
    if (array.kind != SJS_ARRAY || array.internal != 0 || index >= SJS_AS_ARRAY(array)->length) return sjs_undefined();
    return SJS_AS_ARRAY(array)->items[index];
}

sjs_value sjs_set_at(sjs_value set, uint32_t index) {
    if (set.kind != SJS_SET || set.internal != 0 || index >= SJS_AS_ARRAY(set)->length) return sjs_undefined();
    return SJS_AS_ARRAY(set)->items[index];
}

sjs_result sjs_object_at(sjs_value object, uint32_t index, sjs_property *out) {
    if (object.kind != SJS_OBJECT || object.internal != 0 || out == NULL) return SJS_ERR_INVALID;
    const sjs_object *o = SJS_AS_OBJECT(object);
    if (index >= o->count) return SJS_ERR_NOT_FOUND;
    out->key = sjs_string_view(o->entries[index].key);
    out->value = o->entries[index].value;
    return SJS_OK;
}

sjs_result sjs_object_get(sjs_value object, sjs_str key, sjs_value *out) {
    if (object.kind != SJS_OBJECT || object.internal != 0 || out == NULL) return SJS_ERR_INVALID;
    const sjs_entry *e = sjs_object_find(SJS_AS_OBJECT(object), key);
    if (e == NULL) return SJS_ERR_NOT_FOUND;
    *out = e->value;
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Functions
 */

sjs_result sjs_closure_make(sjs_context *context, uint32_t count, sjs_closure **out) {
    sjs_closure *c = heap_alloc(context, sizeof(sjs_closure) + (size_t)count * sizeof(sjs_value), SJS_HEAP_CLOSURE);
    if (c == NULL) return SJS_ERR_NOMEM;
    c->program = NULL;
    c->function = 0;
    c->count = 0;
    *out = c;
    return SJS_OK;
}

sjs_result sjs_function_native(sjs_context *context, sjs_native native, sjs_value *out) {
    if (context == NULL || out == NULL || native.v == NULL || native.v->call == NULL) return SJS_ERR_INVALID;
    sjs_native_function *f = heap_alloc(context, sizeof *f, SJS_HEAP_NATIVE);
    if (f == NULL) return SJS_ERR_NOMEM;
    f->native = native;
    *out = sjs_heap_value(SJS_FUNCTION, &f->h);
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * JavaScript's abstract operations, for the values staticjs has
 */

bool sjs_is_primitive(sjs_value v) {
    return v.kind <= SJS_BIGINT && v.internal == SJS_INTERNAL_NONE;
}

bool sjs_truthy(sjs_value v) {
    switch (v.kind) {
    case SJS_UNDEFINED:
    case SJS_NULL:
        return false;
    case SJS_BOOLEAN:
        return v.boolean != 0;
    case SJS_NUMBER:
        return v.as.number != 0 && !sjs_number_is_nan(v.as.number);
    case SJS_STRING:
        return SJS_AS_STRING(v)->length != 0;
    case SJS_BIGINT:
        return SJS_AS_BIGINT(v)->length != 0;
    default:
        return true;
    }
}

/* SameValueZero and strict equality, inlined where sets look an element up:
 * a BigInt compares by value, through a call, so that the compiler still
 * inlines the rest into the scans. */
__attribute__((always_inline)) static inline bool equal_primitive(sjs_value a, sjs_value b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case SJS_UNDEFINED:
    case SJS_NULL:
        return true;
    case SJS_BOOLEAN:
        return a.boolean == b.boolean;
    case SJS_NUMBER:
        return a.as.number == b.as.number;
    case SJS_STRING:
        return sjs_str_equal(sjs_string_view(SJS_AS_STRING(a)), sjs_string_view(SJS_AS_STRING(b)));
    default:
        return a.kind == SJS_BIGINT && sjs_bigint_equal(SJS_AS_BIGINT(a), SJS_AS_BIGINT(b));
    }
}

__attribute__((always_inline)) static inline bool same_value_zero(sjs_value a, sjs_value b) {
    if (a.kind == SJS_NUMBER && b.kind == SJS_NUMBER && sjs_number_is_nan(a.as.number) &&
        sjs_number_is_nan(b.as.number)) {
        return true;
    }
    return equal_primitive(a, b);
}

bool sjs_strict_equal_primitive(sjs_value a, sjs_value b) {
    return equal_primitive(a, b);
}

bool sjs_same_value_zero(sjs_value a, sjs_value b) {
    return same_value_zero(a, b);
}

sjs_str sjs_typeof(sjs_value v) {
    switch (v.kind) {
    case SJS_UNDEFINED:
        return SJS_STR("undefined");
    case SJS_BOOLEAN:
        return SJS_STR("boolean");
    case SJS_NUMBER:
        return SJS_STR("number");
    case SJS_STRING:
        return SJS_STR("string");
    case SJS_BIGINT:
        return SJS_STR("bigint");
    case SJS_FUNCTION:
        return SJS_STR("function");
    default:
        return SJS_STR("object");
    }
}

sjs_result sjs_to_string(sjs_context *context, sjs_value v, sjs_string **out) {
    switch (v.kind) {
    case SJS_UNDEFINED:
        return sjs_string_make(context, SJS_STR("undefined"), out);
    case SJS_NULL:
        return sjs_string_make(context, SJS_STR("null"), out);
    case SJS_BOOLEAN:
        return sjs_string_make(context, v.boolean ? SJS_STR("true") : SJS_STR("false"), out);
    case SJS_NUMBER:
        return sjs_string_from_number(context, v.as.number, out);
    case SJS_STRING:
        sjs_retain(v);
        *out = SJS_AS_STRING(v);
        return SJS_OK;
    case SJS_BIGINT:
        return sjs_bigint_to_string(context, (sjs_bigint_radix){ SJS_AS_BIGINT(v), 10u }, out);
    default:
        return SJS_ERR_PROGRAM;
    }
}

sjs_result sjs_to_key(sjs_context *context, sjs_value v, sjs_string **out) {
    return sjs_to_string(context, v, out);
}

/* ------------------------------------------------------------------------
 * MUTATION: removal from sets and objects, and shrinking (docs/mutation.md,
 * "Removal"). Removal compacts: the items after the one removed move down,
 * so a container's items stay dense and in order, and every reader of
 * them is unchanged; its index loses the removed item's slot by backward
 * shifting, which leaves no deletion marker, and the positions after it
 * go down by one. A container whose items or index have become mostly
 * empty is reallocated smaller. This section exists only for mutation.
 */

static lookup_hit object_hit(const sjs_object *o, sjs_str key) {
    lookup_hit hit = { UINT32_MAX, UINT32_MAX };
    const sjs_index *index = o->index;
    if (index != NULL) {
        for (uint32_t i = hash_bytes(key) & index->mask; index->slots[i] != 0; i = (i + 1u) & index->mask) {
            if (sjs_str_equal(sjs_string_view(o->entries[index->slots[i] - 1u].key), key)) {
                hit.position = index->slots[i] - 1u;
                hit.slot = i;
                return hit;
            }
        }
        return hit;
    }
    for (uint32_t i = 0; i < o->count; i++) {
        if (sjs_str_equal(sjs_string_view(o->entries[i].key), key)) {
            hit.position = i;
            return hit;
        }
    }
    return hit;
}

/* Take the item at `hit` out of the container's index, before the items
 * after it move down. Linear probing finds an item by walking from its
 * home slot to the first empty one, so each item after the hole in its
 * cluster that could no longer be found moves into the hole, which moves
 * on (backward-shift deletion): no slot is ever a deletion marker. */
static void lookup_remove(const sjs_heap *h, sjs_index *index, lookup_hit hit) {
    uint32_t mask = index->mask;
    uint32_t hole = hit.slot;
    for (uint32_t j = (hole + 1u) & mask; index->slots[j] != 0; j = (j + 1u) & mask) {
        uint32_t home = item_hash(h, index->slots[j] - 1u) & mask;
        /* It stays when its home is after the hole, up to j, cyclically. */
        bool stays = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
        if (stays) continue;
        index->slots[hole] = index->slots[j];
        hole = j;
    }
    index->slots[hole] = 0;
    if (hit.position + 1u == item_count(h)) return;
    for (uint32_t i = 0; i <= mask; i++) {
        if (index->slots[i] > hit.position + 1u) index->slots[i]--;
    }
}

/* After a removal: a container under half of SJS_INDEX_FROM items goes
 * without an index, as a small one is scanned, and may get one again as it
 * grows, even if one failed to allocate before; an index at most an eighth
 * full is rebuilt a quarter full, so that a table grows at half load and
 * shrinks at an eighth, and adding and removing around either does not
 * rebuild it each time. When the smaller table cannot be allocated, the
 * larger one stays: it is still correct. */
static void lookup_shrink(sjs_context *context, sjs_heap *h) {
    sjs_index **field = lookup_field(h);
    uint32_t count = item_count(h);
    sjs_index *index = *field;
    if (count < SJS_INDEX_FROM / 2u) {
        lookup_free(context, index);
        *field = NULL;
        h->flags &= (uint8_t)~SJS_CONTAINER_UNINDEXED;
        return;
    }
    if (index == NULL || index->mask < 16u || (uint64_t)count * 8u > (uint64_t)index->mask + 1u) return;
    uint32_t slots = 16u;
    while (slots / 4u < count) slots *= 2u;
    sjs_index *smaller = sjs_mem_alloc(&context->counting, lookup_size(slots - 1u));
    if (smaller == NULL) return;
    lookup_free(context, index);
    smaller->mask = slots - 1u;
    *field = smaller;
    sjs_zero(smaller->slots, (size_t)slots * sizeof(uint32_t));
    for (uint32_t i = 0; i < count; i++) lookup_put(smaller, item_hash(h, i), i);
}

/* A container's capacity shrinks once it reaches SJS_SHRINK_FROM and its
 * items fill under a quarter of it, to twice its items: it grows by
 * doubling when full, so adding and removing around the edge does not
 * reallocate each time. */
#define SJS_SHRINK_FROM 16u

static uint32_t shrunk_capacity(uint32_t capacity, uint32_t count) {
    if (capacity < SJS_SHRINK_FROM || (uint64_t)count * 4u >= capacity) return capacity;
    return count * 2u < 4u ? 4u : count * 2u;
}

void sjs_array_shrink(sjs_context *context, sjs_array **array) {
    sjs_array *a = *array;
    uint32_t capacity = shrunk_capacity(a->capacity, a->length);
    if (capacity == a->capacity) return;
    sjs_array *smaller;
    if (array_alloc(context, (shape){ capacity, a->h.type }, &smaller) != SJS_OK) return;
    smaller->length = a->length;
    sjs_copy(smaller->items, a->items, (size_t)a->length * sizeof(sjs_value));
    smaller->h.flags = a->h.flags;
    smaller->index = a->index;
    sjs_mem_free(&context->counting, a, a->h.size);
    *array = smaller;
}

static void object_shrink(sjs_context *context, sjs_object **object) {
    sjs_object *o = *object;
    uint32_t capacity = shrunk_capacity(o->capacity, o->count);
    if (capacity == o->capacity) return;
    sjs_object *smaller;
    if (object_alloc(context, capacity, &smaller) != SJS_OK) return;
    smaller->count = o->count;
    sjs_copy(smaller->entries, o->entries, (size_t)o->count * sizeof(sjs_entry));
    smaller->h.flags = o->h.flags;
    smaller->index = o->index;
    sjs_mem_free(&context->counting, o, o->h.size);
    *object = smaller;
}

/* The fuel of a removal: the lookup, one unit through the index or in a
 * small container, its length when it is scanned (as lookup_cost charges a
 * set's); and its length again when the item was not the last, as the
 * items after it move down and the index's positions are renumbered. */
static uint64_t removal_cost(const sjs_heap *h, const sjs_index *index, lookup_hit hit) {
    uint32_t count = item_count(h);
    uint64_t cost = index != NULL || count < SJS_INDEX_FROM ? 1u : count;
    if (hit.position != UINT32_MAX && hit.position + 1u < count) cost += count;
    return cost;
}

sjs_removal sjs_object_remove(sjs_context *context, sjs_object **object, sjs_str key) {
    sjs_object *o = *object;
    lookup_hit hit = object_hit(o, key);
    sjs_removal out = { hit.position != UINT32_MAX, removal_cost(&o->h, o->index, hit) };
    if (!out.removed) return out;
    if (o->index != NULL) lookup_remove(&o->h, o->index, hit);
    sjs_entry gone = o->entries[hit.position];
    for (uint32_t i = hit.position + 1u; i < o->count; i++) o->entries[i - 1u] = o->entries[i];
    o->count--;
    lookup_shrink(context, &o->h);
    object_shrink(context, object);
    sjs_release(context, sjs_heap_value(SJS_STRING, &gone.key->h));
    sjs_release(context, gone.value);
    return out;
}

/* A key among a set's elements (stride 1) or a map's keys (stride 2). */
typedef struct {
    sjs_value key;
    uint32_t stride;
} keyed_item;

/* A set's element, or a map's key and its value, removed: the items after
 * it move down by the stride. Inlined with its stride, so a set's removal
 * compiles as it did before maps shared it. */
__attribute__((always_inline)) static inline sjs_removal keyed_remove(sjs_context *context, sjs_array **keyed, keyed_item item) {
    sjs_value key = item.key;
    uint32_t stride = item.stride;
    sjs_array *a = *keyed;
    lookup_hit hit = keyed_hit(a, key, stride);
    sjs_removal out = { hit.position != UINT32_MAX, removal_cost(&a->h, a->index, hit) };
    if (!out.removed) return out;
    if (a->index != NULL) lookup_remove(&a->h, a->index, hit);
    uint32_t at = hit.position * stride;
    sjs_value gone = a->items[at];
    sjs_value value = stride == 2u ? a->items[at + 1u] : sjs_undefined();
    for (uint32_t i = at + stride; i < a->length; i++) a->items[i - stride] = a->items[i];
    a->length -= stride;
    lookup_shrink(context, &a->h);
    sjs_array_shrink(context, keyed);
    sjs_release(context, gone);
    sjs_release(context, value);
    return out;
}

sjs_removal sjs_set_remove(sjs_context *context, sjs_array **set, sjs_value element) {
    return keyed_remove(context, set, (keyed_item){ element, 1u });
}

/* MUTATION: Map.prototype.delete. A map's clear is a set's (sjs_set_clear),
 * which releases its items, keys and values alike. */
sjs_removal sjs_map_remove(sjs_context *context, sjs_array **map, sjs_value key) {
    return keyed_remove(context, map, (keyed_item){ key, 2u });
}

void sjs_set_clear(sjs_context *context, sjs_array **set) {
    sjs_array *a = *set;
    for (uint32_t i = 0; i < a->length; i++) sjs_release(context, a->items[i]);
    a->length = 0;
    lookup_shrink(context, &a->h);
    sjs_array_shrink(context, set);
}

/* ------------------------------------------------------------------------
 * STRING_APPEND: a string that one holder has, extended in place by + and a
 * template (README.md, "A value nothing else holds is updated in place").
 * Not mutation: no program can tell, as nothing else sees the string.
 */

/* Room for `extra` more bytes in a string the caller holds alone and that is
 * not interned. Its block grows to twice its length, or to what it needs if
 * that is more, so appending n bytes a few at a time copies O(n) bytes and
 * allocates O(log n) times; the slack is at most the string's length, and
 * stays when the string is shared or kept (README.md). The new block is
 * allocated before the old one is freed: out of memory, the string is as it
 * was. */
sjs_result sjs_string_reserve(sjs_context *context, sjs_string **string, uint32_t extra) {
    sjs_string *s = *string;
    uint64_t need = (uint64_t)s->length + extra;
    if (need > context->limits.max_string_bytes) return SJS_ERR_LIMIT;
    if (need + sizeof(sjs_string) + 1u <= s->h.size) return SJS_OK;
    uint64_t want = 2u * (uint64_t)s->length;
    if (want < need) want = need;
    if (want > context->limits.max_string_bytes) want = context->limits.max_string_bytes;
    uint64_t size = sizeof(sjs_string) + want + 1u;
    if (size > UINT32_MAX) return SJS_ERR_NOMEM;
    sjs_string *grown = sjs_mem_alloc(&context->counting, (size_t)size);
    if (grown == NULL) return SJS_ERR_NOMEM;
    sjs_copy(grown, s, sizeof(sjs_string) + s->length + 1u);
    grown->h.size = (uint32_t)size;
    sjs_mem_free(&context->counting, s, s->h.size);
    *string = grown;
    return SJS_OK;
}

/* Append `tail` to a string with room for it (sjs_string_reserve), and keep
 * what the string caches true of its text: both lengths, ASCII, and whether
 * it is an array index; its name's ID is looked up again when needed. */
void sjs_string_extend(sjs_string *s, sjs_str tail) {
    bool ascii = true;
    for (uint32_t i = 0; i < tail.length; i++) {
        if ((uint8_t)tail.ptr[i] >= 0x80u) ascii = false;
    }
    sjs_copy(s->bytes + s->length, tail.ptr, tail.length);
    s->length += tail.length;
    s->bytes[s->length] = '\0';
    s->utf16_length += ascii ? tail.length : sjs_utf16_length(tail);
    s->h.flags &= (uint8_t)~(SJS_STRING_INDEX | SJS_STRING_NAMED | (ascii ? 0u : SJS_STRING_ASCII));
    s->h.name_id = 0;
    if (parse_index(sjs_string_view(s), &s->index)) s->h.flags |= SJS_STRING_INDEX;
}

/* ------------------------------------------------------------------------
 * Maps: a set of keys with a value for each. A map is an sjs_array of
 * SJS_HEAP_MAP whose keys and values alternate in its items, so that it
 * grows, shrinks and is freed as a set is, and its keys are found through
 * the same index (keyed_hit with a stride of 2, item_count and item_hash
 * count and hash its entries by their keys).
 */

sjs_result sjs_map_make(sjs_context *context, uint32_t entries, sjs_array **out) {
    if (entries > context->limits.max_value_items / 2u) return SJS_ERR_LIMIT;
    return array_alloc(context, (shape){ entries * 2u, SJS_HEAP_MAP }, out);
}

const sjs_value *sjs_map_find(const sjs_array *map, sjs_value key) {
    lookup_hit hit = keyed_hit(map, key, 2u);
    return hit.position == UINT32_MAX ? NULL : &map->items[2u * hit.position + 1u];
}

sjs_result sjs_map_put(sjs_context *context, sjs_array **map, const sjs_map_entry *entry) {
    sjs_value key = entry->key;
    if (!sjs_is_primitive(key)) return SJS_ERR_PROGRAM;
    if (key.kind == SJS_NUMBER && key.as.number == 0) key.as.number = 0.0;
    sjs_value *found = (sjs_value *)(uintptr_t)sjs_map_find(*map, key);
    if (found != NULL) {
        sjs_value old = *found;
        *found = entry->value;
        sjs_release(context, old);
        sjs_release(context, key);
        return SJS_OK;
    }
    sjs_result r = sjs_array_grow(context, map, 2u);
    if (r != SJS_OK) return r;
    sjs_array *a = *map;
    a->items[a->length++] = key;
    a->items[a->length++] = entry->value;
    lookup_appended(context, &a->h);
    return SJS_OK;
}

sjs_result sjs_map_copy(sjs_context *context, const sjs_array *map, sjs_array **out) {
    sjs_result r = array_alloc(context, (shape){ map->length < 4u ? 4u : map->length, SJS_HEAP_MAP }, out);
    if (r != SJS_OK) return r;
    sjs_array *a = *out;
    for (uint32_t i = 0; i < map->length; i++) {
        sjs_ref(map->items[i]);
        a->items[i] = map->items[i];
    }
    a->length = map->length;
    if (a->length / 2u >= SJS_INDEX_FROM) lookup_build(context, &a->h);
    return SJS_OK;
}

sjs_result sjs_object_copy(sjs_context *context, const sjs_object *object, sjs_object **out) {
    const sjs_object *o = object;
    sjs_result r = object_alloc(context, o->count < 4u ? 4u : o->count, out);
    if (r != SJS_OK) return r;
    sjs_object *c = *out;
    for (uint32_t i = 0; i < o->count; i++) {
        sjs_ref(sjs_heap_value(SJS_STRING, &o->entries[i].key->h));
        sjs_ref(o->entries[i].value);
        c->entries[i] = o->entries[i];
    }
    c->count = o->count;
    if (c->count >= SJS_INDEX_FROM) lookup_build(context, &c->h);
    return SJS_OK;
}

sjs_result sjs_map_new(sjs_context *context, uint32_t capacity, sjs_value *out) {
    if (context == NULL || out == NULL) return SJS_ERR_INVALID;
    sjs_array *a;
    sjs_result r = sjs_map_make(context, capacity, &a);
    if (r != SJS_OK) return r;
    *out = sjs_heap_value(SJS_MAP, &a->h);
    return SJS_OK;
}

sjs_result sjs_map_set(sjs_context *context, sjs_value *map, const sjs_map_entry *entry) {
    if (context == NULL || map == NULL || entry == NULL || map->kind != SJS_MAP || map->internal != 0) return SJS_ERR_INVALID;
    sjs_array *a = SJS_AS_ARRAY(*map);
    if (a->h.refs != 1 || entry->key.internal != 0 || entry->value.internal != 0) return SJS_ERR_INVALID;
    sjs_result r = sjs_map_put(context, &a, entry);
    if (r == SJS_ERR_PROGRAM) return SJS_ERR_INVALID;
    map->as.heap = &a->h;
    return r;
}

uint32_t sjs_map_size(sjs_value map) {
    if (map.kind != SJS_MAP || map.internal != 0) return 0;
    return SJS_AS_ARRAY(map)->length / 2u;
}

sjs_result sjs_map_get(sjs_value map, sjs_value key, sjs_value *out) {
    if (map.kind != SJS_MAP || map.internal != 0 || key.internal != 0 || out == NULL) return SJS_ERR_INVALID;
    const sjs_value *found = sjs_map_find(SJS_AS_ARRAY(map), key);
    if (found == NULL) return SJS_ERR_NOT_FOUND;
    *out = *found;
    return SJS_OK;
}

sjs_result sjs_map_at(sjs_value map, uint32_t index, sjs_map_entry *out) {
    if (map.kind != SJS_MAP || map.internal != 0 || out == NULL) return SJS_ERR_INVALID;
    const sjs_array *a = SJS_AS_ARRAY(map);
    if (index >= a->length / 2u) return SJS_ERR_NOT_FOUND;
    out->key = a->items[2u * index];
    out->value = a->items[2u * index + 1u];
    return SJS_OK;
}

sjs_result sjs_iterator_make(sjs_context *context, sjs_iterator_spec spec, sjs_value *out) {
    sjs_iterator *it = heap_alloc(context, sizeof *it, SJS_HEAP_ITERATOR);
    if (it == NULL) return SJS_ERR_NOMEM;
    it->h.flags = spec.mode;
    it->source = spec.source;
    it->items = spec.items;
    it->position = 0; /* MUTATION */
    *out = sjs_heap_value(SJS_ITERATOR, &it->h);
    return SJS_OK;
}

uint32_t sjs_iterator_count(const sjs_iterator *it) {
    uint32_t n;
    if (it->items.kind == SJS_ARRAY) {
        n = SJS_AS_ARRAY(it->items)->length;
    } else {
        const sjs_array *source = SJS_AS_ARRAY(it->source);
        n = it->source.kind == SJS_MAP ? source->length / 2u : source->length;
    }
    return n - it->position; /* MUTATION: what next() took is gone. Restore: n. */
}

/* What an array, a set or a map holds beside its items: a set's or a map's
 * index, freed, or a match result's extra properties (SJS_ARRAY_EXTRA),
 * dropped. Out of drain and at the end of the file, so that the code before
 * it is laid out as it was. */
__attribute__((noinline)) static void drop_side(sjs_context *context, sjs_heap **work, sjs_array *a) {
    if (a->h.type == SJS_HEAP_ARRAY && (a->h.flags & SJS_ARRAY_EXTRA) != 0) drop_heap(context, work, a->extra);
    else lookup_free(context, a->index);
}

/* A Temporal value (docs/temporal.md): one block, nothing in it to release. */
sjs_result sjs_temporal_make(sjs_context *context, sjs_temporal_type type, sjs_temporal **out) {
    sjs_temporal *t = heap_alloc(context, sizeof *t, SJS_HEAP_TEMPORAL);
    if (t == NULL) return SJS_ERR_NOMEM;
    sjs_zero(&t->as, sizeof t->as);
    t->h.flags = (uint8_t)type;
    *out = t;
    return SJS_OK;
}
