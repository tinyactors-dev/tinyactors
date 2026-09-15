#pragma once

#include "atom.h"

typedef struct {
    ta_event_name_id parent;
    ta_atom segment;
    uint16_t depth;
    uint16_t flags;
} ta_event_name_node;

typedef struct {
    ta_allocator allocator;
    ta_atom_table *atoms;
    ta_event_name_node *nodes;
    uint32_t node_count;
    uint32_t node_capacity;
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

ta_result ta_event_name_table_init(ta_event_name_table *table,
                                   ta_allocator allocator,
                                   ta_atom_table *atoms);
void ta_event_name_table_destroy(ta_event_name_table *table);
ta_result ta_event_name_intern_atoms_internal(ta_event_name_table *table,
                                              const ta_atom_list *segments,
                                              ta_event_name_id *out);
bool ta_event_name_matches_internal(const ta_event_name_table *table,
                                    ta_event_name_id event,
                                    ta_event_name_id prefix);
