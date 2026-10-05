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

/* Drop one reference; a value whose last reference this was joins `work`. */
static void drop(sjs_heap **work, sjs_value value) {
    if (!sjs_is_heap(value)) return;
    sjs_heap *h = value.as.heap;
    if (h->refs == SJS_IMMORTAL) return;
    if (--h->refs != 0) return;
    h->link = *work;
    *work = h;
}

static void drop_heap(sjs_heap **work, sjs_heap *h) {
    if (h == NULL || h->refs == SJS_IMMORTAL) return;
    if (--h->refs != 0) return;
    h->link = *work;
    *work = h;
}

static void drop_program(sjs_heap **work, sjs_program *program) {
    if (program == NULL || --program->refs != 0) return;
    for (uint32_t i = 0; i < program->constant_count; i++) drop(work, program->constants[i]);
    sjs_mem_free(&program->context->counting, program, program->size);
}

static size_t lookup_size(uint32_t mask) {
    return sizeof(sjs_index) + ((size_t)mask + 1u) * sizeof(uint32_t);
}

static void lookup_free(sjs_context *context, sjs_index *index) {
    if (index != NULL) sjs_mem_free(&context->counting, index, lookup_size(index->mask));
}

static void intern_remove(sjs_context *context, const sjs_string *string);

/* Free everything on the worklist, and what it alone held. */
static void drain(sjs_context *context, sjs_heap *work) {
    while (work != NULL) {
        sjs_heap *h = work;
        work = h->link;
        switch (h->type) {
        case SJS_HEAP_ARRAY:
        case SJS_HEAP_SET: {
            sjs_array *a = (sjs_array *)(void *)h;
            for (uint32_t i = 0; i < a->length; i++) drop(&work, a->items[i]);
            lookup_free(context, a->index);
            break;
        }
        case SJS_HEAP_OBJECT: {
            sjs_object *o = (sjs_object *)(void *)h;
            for (uint32_t i = 0; i < o->count; i++) {
                drop_heap(&work, &o->entries[i].key->h);
                drop(&work, o->entries[i].value);
            }
            lookup_free(context, o->index);
            break;
        }
        case SJS_HEAP_CLOSURE: {
            sjs_closure *c = (sjs_closure *)(void *)h;
            for (uint32_t i = 0; i < c->count; i++) drop(&work, c->captures[i]);
            drop_program(&work, c->program);
            break;
        }
        case SJS_HEAP_STRING:
            if ((h->flags & SJS_STRING_INTERNED) != 0) intern_remove(context, (const sjs_string *)(const void *)h);
            break;
        default:
            break;
        }
        sjs_mem_free(&context->counting, h, h->size);
    }
}

void sjs_release(sjs_context *context, sjs_value value) {
    sjs_heap *work = NULL;
    drop(&work, value);
    drain(context, work);
}

void sjs_program_retain(sjs_program *program) {
    if (program != NULL) program->refs++;
}

void sjs_program_release(sjs_program *program) {
    if (program == NULL) return;
    sjs_context *context = program->context;
    sjs_heap *work = NULL;
    drop_program(&work, program);
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

#define INTERN_FIRST_SLOTS 64u

static size_t intern_size(uint32_t mask) {
    return ((size_t)mask + 1u) * sizeof(sjs_intern_slot);
}

static void intern_place(sjs_intern_slot *slots, uint32_t mask, sjs_intern_slot slot) {
    uint32_t i = slot.hash & mask;
    while (slots[i].string != NULL) i = (i + 1u) & mask;
    slots[i] = slot;
}

/* Room for one more string, at most half the slots full; false when the
 * table cannot grow. */
static bool intern_reserve(sjs_context *context) {
    uint32_t slots = context->interned == NULL ? 0 : context->intern_mask + 1u;
    if ((context->intern_count + 1u) * 2u <= slots) return true;
    /* Twice the slots; 32-bit size_t (wasm32) needs the guard. */
    uint32_t grown = slots == 0 ? INTERN_FIRST_SLOTS : slots * 2u;
    if (grown == 0 || (size_t)grown > SIZE_MAX / sizeof(sjs_intern_slot)) return false;
    sjs_intern_slot *table = sjs_mem_alloc(&context->counting, (size_t)grown * sizeof(sjs_intern_slot));
    if (table == NULL) return false;
    sjs_zero(table, (size_t)grown * sizeof(sjs_intern_slot));
    for (uint32_t i = 0; i < slots; i++) {
        if (context->interned[i].string != NULL) intern_place(table, grown - 1u, context->interned[i]);
    }
    if (context->interned != NULL) sjs_mem_free(&context->counting, context->interned, intern_size(context->intern_mask));
    context->interned = table;
    context->intern_mask = grown - 1u;
    return true;
}

sjs_result sjs_string_intern(sjs_context *context, sjs_str text, sjs_string **out) {
    uint32_t hash = hash_bytes(text);
    if (context->interned != NULL) {
        uint32_t mask = context->intern_mask;
        for (uint32_t i = hash & mask; context->interned[i].string != NULL; i = (i + 1u) & mask) {
            sjs_string *s = context->interned[i].string;
            if (context->interned[i].hash == hash && sjs_str_equal(sjs_string_view(s), text)) {
                if (s->h.refs != SJS_IMMORTAL) s->h.refs++;
                *out = s;
                return SJS_OK;
            }
        }
    }
    sjs_result r = sjs_string_make(context, text, out);
    if (r != SJS_OK || !intern_reserve(context)) return r;
    intern_place(context->interned, context->intern_mask, (sjs_intern_slot){ *out, hash });
    context->intern_count++;
    (*out)->h.flags |= SJS_STRING_INTERNED;
    return SJS_OK;
}

/* Take a string being freed out of the table, shifting back the strings
 * after it that probed past its slot, so that no probe meets a gap early. */
static void intern_remove(sjs_context *context, const sjs_string *string) {
    sjs_intern_slot *slots = context->interned;
    uint32_t mask = context->intern_mask;
    uint32_t i = hash_bytes(sjs_string_view(string)) & mask;
    while (slots[i].string != string) i = (i + 1u) & mask;
    for (uint32_t j = (i + 1u) & mask; slots[j].string != NULL; j = (j + 1u) & mask) {
        uint32_t home = slots[j].hash & mask;
        /* Slot j's string may move to i when its home is not in (i, j]. */
        bool stays = i <= j ? (home > i && home <= j) : (home > i || home <= j);
        if (stays) continue;
        slots[i] = slots[j];
        i = j;
    }
    slots[i].string = NULL;
    if (--context->intern_count == 0) {
        sjs_mem_free(&context->counting, slots, intern_size(mask));
        context->interned = NULL;
        context->intern_mask = 0;
    }
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
        return v.kind;
    }
}

static sjs_index **lookup_field(sjs_heap *h) {
    if (h->type == SJS_HEAP_OBJECT) return &((sjs_object *)(void *)h)->index;
    return &((sjs_array *)(void *)h)->index;
}

static uint32_t item_count(const sjs_heap *h) {
    if (h->type == SJS_HEAP_OBJECT) return ((const sjs_object *)(const void *)h)->count;
    return ((const sjs_array *)(const void *)h)->length;
}

static uint32_t item_hash(const sjs_heap *h, uint32_t i) {
    if (h->type == SJS_HEAP_OBJECT) {
        return hash_bytes(sjs_string_view(((const sjs_object *)(const void *)h)->entries[i].key));
    }
    return hash_value(((const sjs_array *)(const void *)h)->items[i]);
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
    if (a->h.refs != 1) return SJS_ERR_INVALID;
    sjs_result r = sjs_array_grow(context, &a, 1);
    if (r != SJS_OK) return r;
    a->items[a->length++] = element;
    array->as.heap = &a->h;
    return SJS_OK;
}

bool sjs_set_contains(const sjs_array *set, sjs_value element) {
    const sjs_index *index = set->index;
    if (index != NULL) {
        for (uint32_t i = hash_value(element) & index->mask; index->slots[i] != 0; i = (i + 1u) & index->mask) {
            if (sjs_same_value_zero(set->items[index->slots[i] - 1u], element)) return true;
        }
        return false;
    }
    for (uint32_t i = 0; i < set->length; i++) {
        if (sjs_same_value_zero(set->items[i], element)) return true;
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
    if (o->h.refs != 1 || property->value.internal != 0 || !sjs_utf8_valid(property->key)) return SJS_ERR_INVALID;
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
    return v.kind <= SJS_STRING && v.internal == SJS_INTERNAL_NONE;
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
    default:
        return true;
    }
}

bool sjs_strict_equal_primitive(sjs_value a, sjs_value b) {
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
        return false;
    }
}

bool sjs_same_value_zero(sjs_value a, sjs_value b) {
    if (a.kind == SJS_NUMBER && b.kind == SJS_NUMBER && sjs_number_is_nan(a.as.number) &&
        sjs_number_is_nan(b.as.number)) {
        return true;
    }
    return sjs_strict_equal_primitive(a, b);
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
    default:
        return SJS_ERR_PROGRAM;
    }
}

sjs_result sjs_to_key(sjs_context *context, sjs_value v, sjs_string **out) {
    return sjs_to_string(context, v, out);
}
