#include "compile/compile_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *default_alloc(void *context, size_t size, size_t align) {
    (void)context;
    (void)align; /* malloc alignment suffices (<= 16) */
    return malloc(size ? size : 1);
}
static void default_free(void *context, void *p, size_t size) {
    (void)context;
    (void)size;
    free(p);
}

ta_allocator ta_compile_default_allocator(void) {
    ta_allocator allocator = {NULL, default_alloc, default_free};
    return allocator;
}

struct ta_chunk_arena_chunk {
    ta_chunk_arena_chunk *next;
    size_t size, used;
    /* data follows (8-aligned) */
};

#define CHUNK_HEADER ((sizeof(ta_chunk_arena_chunk) + 7u) & ~(size_t)7u)

void ta_chunk_arena_init(ta_chunk_arena *arena, ta_allocator *allocator) {
    arena->allocator = allocator;
    arena->head = NULL;
    arena->out_of_memory = 0;
}

void *ta_chunk_arena_alloc(ta_chunk_arena *arena, size_t size) {
    size = (size + 7u) & ~(size_t)7u;
    if (!arena->head || arena->head->size - arena->head->used < size) {
        size_t csz = size > 32768 ? size : 32768;
        ta_chunk_arena_chunk *chunk = (ta_chunk_arena_chunk *)arena->allocator->alloc(
            arena->allocator->context, CHUNK_HEADER + csz, 8);
        if (!chunk) {
            arena->out_of_memory = 1;
            return NULL;
        }
        chunk->next = arena->head;
        chunk->size = csz;
        chunk->used = 0;
        arena->head = chunk;
    }
    void *p = (unsigned char *)arena->head + CHUNK_HEADER + arena->head->used;
    arena->head->used += size;
    memset(p, 0, size);
    return p;
}

char *ta_chunk_arena_strdup(ta_chunk_arena *arena, const char *s, size_t length) {
    char *p = (char *)ta_chunk_arena_alloc(arena, length + 1);
    if (p) {
        if (length) memcpy(p, s, length);
        p[length] = 0;
    }
    return p;
}

void ta_chunk_arena_free(ta_chunk_arena *arena) {
    ta_chunk_arena_chunk *chunk = arena->head;
    while (chunk) {
        ta_chunk_arena_chunk *new_chunk = chunk->next;
        arena->allocator->free(arena->allocator->context, chunk, CHUNK_HEADER + chunk->size);
        chunk = new_chunk;
    }
    arena->head = NULL;
}

void ta_vector_init(ta_vector *vector, ta_allocator *allocator, uint32_t elem) {
    vector->allocator = allocator;
    vector->data = NULL;
    vector->length = vector->capacity = 0;
    vector->element_size = elem;
    vector->out_of_memory = 0;
}

void *ta_vector_push_many(ta_vector *vector, uint32_t n) {
    if (vector->length + n > vector->capacity) {
        uint32_t ncap = vector->capacity ? vector->capacity * 2 : 16;
        while (ncap < vector->length + n)
            ncap *= 2;
        unsigned char *nd = (unsigned char *)vector->allocator->alloc(
            vector->allocator->context, (size_t)ncap * vector->element_size, 8);
        if (!nd) {
            vector->out_of_memory = 1;
            return NULL;
        }
        if (vector->data) {
            memcpy(nd, vector->data, (size_t)vector->length * vector->element_size);
            vector->allocator->free(vector->allocator->context, vector->data,
                                    (size_t)vector->capacity * vector->element_size);
        }
        vector->data = nd;
        vector->capacity = ncap;
    }
    void *p = vector->data + (size_t)vector->length * vector->element_size;
    memset(p, 0, (size_t)n * vector->element_size);
    vector->length += n;
    return p;
}

void *ta_vector_push(ta_vector *vector) {
    return ta_vector_push_many(vector, 1);
}

uint32_t ta_vector_push_u32(ta_vector *vector, uint32_t x) {
    uint32_t *p = (uint32_t *)ta_vector_push(vector);
    if (p) *p = x;
    return vector->length - 1;
}

void ta_vector_free(ta_vector *vector) {
    if (vector->data)
        vector->allocator->free(vector->allocator->context, vector->data,
                                (size_t)vector->capacity * vector->element_size);
    vector->data = NULL;
    vector->length = vector->capacity = 0;
}

static uint32_t hash_str(const char *s, uint32_t length) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < length; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

void ta_string_map_init(ta_string_map *map, ta_allocator *allocator) {
    map->allocator = allocator;
    map->slots = NULL;
    map->capacity = map->count = 0;
    map->out_of_memory = 0;
}

uint32_t ta_string_map_get(const ta_string_map *map, const char *key, uint32_t length) {
    if (!map->capacity) return TA_NIL;
    uint32_t i = hash_str(key, length) & (map->capacity - 1);
    while (map->slots[i].key) {
        if (map->slots[i].length == length && memcmp(map->slots[i].key, key, length) == 0)
            return map->slots[i].value;
        i = (i + 1) & (map->capacity - 1);
    }
    return TA_NIL;
}

static int string_map_insert(ta_string_map_entry *slots, uint32_t capacity, const char *key,
                             uint32_t length, uint32_t value) {
    uint32_t i = hash_str(key, length) & (capacity - 1);
    while (slots[i].key) {
        if (slots[i].length == length && memcmp(slots[i].key, key, length) == 0) {
            slots[i].value = value;
            return 0;
        }
        i = (i + 1) & (capacity - 1);
    }
    slots[i].key = key;
    slots[i].length = length;
    slots[i].value = value;
    return 1;
}

int ta_string_map_put(ta_string_map *map, const char *key, uint32_t length, uint32_t value) {
    if ((map->count + 1) * 2 > map->capacity) {
        uint32_t ncap = map->capacity ? map->capacity * 2 : 64;
        ta_string_map_entry *ns = (ta_string_map_entry *)map->allocator->alloc(
            map->allocator->context, sizeof(ta_string_map_entry) * ncap, 8);
        if (!ns) {
            map->out_of_memory = 1;
            return 0;
        }
        memset(ns, 0, sizeof(ta_string_map_entry) * ncap);
        for (uint32_t i = 0; i < map->capacity; i++)
            if (map->slots[i].key)
                string_map_insert(ns, ncap, map->slots[i].key, map->slots[i].length,
                                  map->slots[i].value);
        if (map->slots)
            map->allocator->free(map->allocator->context, map->slots,
                                 sizeof(ta_string_map_entry) * map->capacity);
        map->slots = ns;
        map->capacity = ncap;
    }
    map->count += (uint32_t)string_map_insert(map->slots, map->capacity, key, length, value);
    return 1;
}

void ta_string_map_free(ta_string_map *map) {
    if (map->slots)
        map->allocator->free(map->allocator->context, map->slots,
                             sizeof(ta_string_map_entry) * map->capacity);
    map->slots = NULL;
    map->capacity = map->count = 0;
}

void ta_diagnostics_add(ta_diagnostics *d, uint32_t severity, const char *file, uint32_t line,
                        uint32_t col, const char *format, ...) {
    if (severity == TA_DIAGNOSTIC_ERROR) d->errors++;
    if (severity == TA_DIAGNOSTIC_WARNING) d->warnings++;
    if (d->list.length >= TA_DIAGNOSTICS_MAX) return;
    char buffer[512];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(buffer, sizeof buffer, format, ap);
    va_end(ap);
    if (d->list.length == TA_DIAGNOSTICS_MAX - 1) { /* a document with an error per line */
        severity = TA_DIAGNOSTIC_NOTE;
        n = snprintf(buffer, sizeof buffer, "too many diagnostics; the rest are not reported");
        file = NULL;
        line = col = 0;
    }
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof buffer) n = (int)sizeof buffer - 1;
    ta_diagnostic *diagnostic = (ta_diagnostic *)ta_vector_push(&d->list);
    if (!diagnostic) return;
    diagnostic->severity = severity;
    diagnostic->line = line;
    diagnostic->column = col;
    diagnostic->file.pointer = file ? ta_chunk_arena_strdup(d->arena, file, strlen(file)) : NULL;
    diagnostic->file.length = file ? (uint32_t)strlen(file) : 0;
    diagnostic->message.pointer = ta_chunk_arena_strdup(d->arena, buffer, (size_t)n);
    diagnostic->message.length = (uint32_t)n;
}

int ta_c_string_equal(const char *a, uint32_t alen, const char *b) {
    size_t bl = strlen(b);
    return alen == bl && memcmp(a, b, bl) == 0;
}
