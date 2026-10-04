#pragma once

#include "arena.h"

/* Atom text lives in arena blocks, so a returned ta_str never moves. */
typedef struct {
    uint32_t hash;
    uint32_t next;
    ta_str text;
} ta_atom_entry;

typedef struct {
    ta_allocator allocator;
    ta_vec entries;
    uint32_t *buckets;
    uint32_t bucket_count;
    ta_arena strings;
} ta_atom_table;

#define TA_ATOM_NONE UINT32_MAX

ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator);
void ta_atom_table_destroy(ta_atom_table *table);
ta_result ta_atom_intern(ta_atom_table *table, ta_str text, ta_atom *out);
ta_str ta_atom_text(const ta_atom_table *table, ta_atom atom);
/* Looks text up without interning it; TA_ERR_NOT_FOUND when it is not an atom. */
ta_result ta_atom_find(const ta_atom_table *table, ta_str text, ta_atom *out);
bool ta_atom_valid(const ta_atom_table *table, ta_atom atom);
