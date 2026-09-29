#pragma once

#include "atom.h"

typedef struct {
    ta_event_name_id parent;
    ta_atom segment;
    uint16_t depth;
    uint16_t flags;
    uint32_t hash_next;
} ta_event_name_node;

typedef struct {
    ta_allocator allocator;
    ta_atom_table *atoms;
    ta_vec nodes;
    uint32_t *buckets;
    uint32_t bucket_count;
    /* Segments a name may have: the system's eventNameDepth (ADR 0092),
     * at most TA_EVENT_NAME_MAX_DEPTH, which sizes callers' arrays. */
    uint32_t max_depth;
} ta_event_name_table;

enum {
    TA_EVENT_NAME_ROOT = 0u,
    TA_EVENT_NAME_ERROR = 1u,
    TA_EVENT_NAME_ERROR_EXECUTION = 2u,
    TA_EVENT_NAME_ERROR_COMMUNICATION = 3u,
    TA_EVENT_NAME_ERROR_PLATFORM = 4u,
    TA_EVENT_NAME_DONE = 5u,
    TA_EVENT_NAME_DONE_STATE = 6u,
    TA_EVENT_NAME_DONE_INVOKE = 7u
};

/* The ceiling of eventNameDepth (ADR 0092): it sizes segment arrays. */
#define TA_EVENT_NAME_MAX_DEPTH 64u

ta_result ta_event_name_table_init(ta_event_name_table *table,
                                   ta_allocator allocator,
                                   ta_atom_table *atoms);
void ta_event_name_table_destroy(ta_event_name_table *table);
ta_result ta_event_name_intern_atoms_internal(ta_event_name_table *table,
                                              const ta_atom_list *segments,
                                              ta_event_name_id *out);
/* Interns one more segment below `parent`. */
ta_result ta_event_name_child(ta_event_name_table *table,
                              ta_event_name_id parent,
                              ta_atom segment,
                              ta_event_name_id *out);
/* Interns a dotted name; used by the compiler for authored names only. */
ta_result ta_event_name_intern_dotted(ta_event_name_table *table, ta_str dotted, ta_event_name_id *out);
bool ta_event_name_matches_internal(const ta_event_name_table *table,
                                    ta_event_name_id event,
                                    ta_event_name_id prefix);
bool ta_event_name_valid(const ta_event_name_table *table, ta_event_name_id name);
/* Writes the name's segments root-first; returns the segment count. */
uint32_t ta_event_name_segments(const ta_event_name_table *table,
                                ta_event_name_id name,
                                ta_atom out[TA_EVENT_NAME_MAX_DEPTH]);
