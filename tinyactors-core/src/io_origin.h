#pragma once

#include "types.h"
#include "arena.h"

/*
 * Stored URIs of I/O origins (ADR 0068): SCOPED origins name a record in the
 * system's scope table by index and generation; MESSAGE origins point at a
 * reference-counted record in the entry table. Records are fixed-size, live
 * in pages that never move, and are reused through a free list, so a URI of
 * up to TA_IO_ORIGIN_INLINE bytes costs no allocation after warm-up.
 */
#define TA_IO_ORIGIN_INLINE 48u
#define TA_IO_ORIGIN_PAGE 256u

typedef struct ta_io_origin_table ta_io_origin_table;

struct ta_io_origin_entry {
    /* MESSAGE: references held by events. SCOPED: the record's own index. */
    uint32_t refs;
    /* SCOPED: incremented when the scope closes, so stale handles miss. */
    uint32_t generation;
    /* The owning table; null for a detached copy in a session image. */
    ta_io_origin_table *table;
    /* The processor's registry index; unused in a detached copy. */
    uint32_t processor;
    /* The URI's length; UINT32_MAX while the record is free. */
    uint32_t length;
    /* A URI longer than the inline buffer; the next free record while free. */
    char *spill;
    /* The URI when it fits; a detached copy keeps its processor type text here. */
    char text[TA_IO_ORIGIN_INLINE];
};

struct ta_io_origin_table {
    ta_allocator allocator;
    /* A scope table keeps each record's index in `refs`; an entry table counts references there. */
    bool scoped;
    ta_vec pages; /* ta_io_origin_entry *, TA_IO_ORIGIN_PAGE records each */
    uint32_t issued;
    uint32_t live;
    ta_io_origin_entry *free;
};

void ta_io_origin_table_init(ta_io_origin_table *table, ta_allocator allocator, bool scoped);
void ta_io_origin_table_destroy(ta_io_origin_table *table);

/* A live record holding a copy of `uri`, with its index in `*index`; null when out of memory. */
typedef struct {
    uint32_t processor;
    ta_str uri;
    uint32_t *index;
} ta_io_origin_take_params;
ta_io_origin_entry *ta_io_origin_take(ta_io_origin_table *table, const ta_io_origin_take_params *params);
/* Frees a record's spill and returns it to its table's free list. */
void ta_io_origin_give(ta_io_origin_entry *entry);
/* The live record at `index`, or null. */
ta_io_origin_entry *ta_io_origin_at(const ta_io_origin_table *table, uint32_t index);

/* MESSAGE references; both ignore null and detached records. */
void ta_io_origin_entry_retain(ta_io_origin_entry *entry);
void ta_io_origin_entry_release(ta_io_origin_entry *entry);

/* The stored URI. */
ta_str ta_io_origin_entry_uri(const ta_io_origin_entry *entry);

/* A detached copy for a session image: the processor type text and the URI, both in `arena`. */
typedef struct {
    ta_str type;
    ta_str uri;
    ta_io_origin_entry **out;
} ta_io_origin_detach_params;
ta_result ta_io_origin_detach(ta_arena *arena, const ta_io_origin_detach_params *params);
bool ta_io_origin_entry_detached(const ta_io_origin_entry *entry);
/* The processor type text of a detached copy. */
ta_str ta_io_origin_detached_type(const ta_io_origin_entry *entry);
