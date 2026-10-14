/* compile_support.h — compile-time utilities: arena storage, growable vectors,
 * string map, diagnostics. Hosted code; all memory via ta_allocator. */
#ifndef TINYACTORS_COMPILE_COMPILE_SUPPORT_H
#define TINYACTORS_COMPILE_COMPILE_SUPPORT_H

#include <stdarg.h>

#include "base/config.h"
#include <tinyactors/compile.h>
#include <tinyactors/arena.h>

/* The C library's malloc, for callers that pass no allocator. */
ta_allocator ta_compile_default_allocator(void);

/* Compiler objects require zeroed, 8-aligned storage and a sticky failure
 * flag. Chunk ownership and lifetime belong to the shared arena; these
 * helpers add only initialization and compiler error bookkeeping. */
void *ta_compile_alloc(ta_growable_arena *arena, size_t size, int *out_of_memory);
char *ta_compile_copy(ta_growable_arena *arena, ta_string_view text, int *out_of_memory);

/* ---- growable vector ------------------------------------------------------ */
typedef struct ta_vector {
    ta_allocator *allocator;
    unsigned char *data;
    uint32_t length, capacity, element_size;
    int out_of_memory;
} ta_vector;

void ta_vector_init(ta_vector *v, ta_allocator *a, uint32_t elem);
void *ta_vector_push(ta_vector *v); /* zeroed new element or NULL */
void *ta_vector_push_many(ta_vector *v, uint32_t n);
void ta_vector_free(ta_vector *v);
#define TA_VECTOR_AT(v, type, i) (((type *)(void *)(v)->data)[i])
uint32_t ta_vector_push_u32(ta_vector *v, uint32_t x); /* returns index */

/* ---- string -> u32 map (open addressing; keys are not copied) ------------- */
typedef struct ta_string_map_entry {
    const char *key;
    uint32_t length;
    uint32_t value;
} ta_string_map_entry;
typedef struct ta_string_map {
    ta_allocator *allocator;
    ta_string_map_entry *slots;
    uint32_t capacity, count;
    int out_of_memory;
} ta_string_map;

void ta_string_map_init(ta_string_map *m, ta_allocator *a);
uint32_t ta_string_map_get(const ta_string_map *m, const char *key,
                           uint32_t length); /* TA_NIL if absent */
int ta_string_map_put(ta_string_map *m, const char *key, uint32_t length, uint32_t value);
void ta_string_map_free(ta_string_map *m);

/* ---- diagnostics ---------------------------------------------------------- */
/* At most TA_DIAGNOSTICS_MAX diagnostics are kept (the last one says so); all
 * are counted. */
#define TA_DIAGNOSTICS_MAX 200u
typedef struct ta_diagnostics {
    ta_vector list; /* ta_diagnostic */
    ta_growable_arena *arena;
    int out_of_memory;
    uint32_t errors, warnings;
} ta_diagnostics;

typedef struct ta_diagnostic_input {
    uint32_t severity;
    const char *file;
    uint32_t line, column;
    const char *format;
} ta_diagnostic_input;

void ta_diagnostics_add(ta_diagnostics *d, const ta_diagnostic_input *input, ...);

int ta_c_string_equal(const char *a, uint32_t alen, const char *b);

#endif /* TINYACTORS_COMPILE_COMPILE_SUPPORT_H */
