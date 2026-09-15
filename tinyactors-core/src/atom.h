#pragma once

#include "types.h"

typedef struct {
    uint32_t hash;
    uint32_t next;
    ta_span32 text;
} ta_atom_entry;

typedef struct {
    ta_allocator allocator;
    ta_atom_entry *entries;
    uint32_t entry_count;
    uint32_t entry_capacity;
    char *strings;
    uint32_t strings_len;
    uint32_t strings_capacity;
} ta_atom_table;

ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator);
void ta_atom_table_destroy(ta_atom_table *table);
ta_atom ta_atom_intern(ta_atom_table *table, ta_str text);
ta_str ta_atom_text(const ta_atom_table *table, ta_atom atom);
