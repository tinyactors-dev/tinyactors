#include "atom.h"
#include "memory.h"
#include "str.h"
#include "../include/tinyactors.h"
#include "vec.h"
#include "arena.h"
#include "hash.h"
#include "table.h"

/* The atoms the hash table has room for when the table is made. */
#define TA_ATOM_INITIAL_CAPACITY 48u

/* What the hash table's comparison needs: the atoms and the text looked for. */
typedef struct {
    const ta_atom_table *table;
    ta_str text;
} ta_atom_key;

/* Whether atom `value - 1` has the text in `context`, a ta_atom_key. */
static bool ta_atom_key_equal(const void *context, uint32_t value) {
    const ta_atom_key *key = context;
    const ta_atom_entry *entry = ta_vec_at(&key->table->entries, value - 1u);
    return ta_str_equal(entry->text, key->text);
}

/* Looks `text`, whose hash is `hash`, up in the hash table. */
static ta_result ta_atom_lookup(const ta_atom_table *table, ta_atom_key key, uint32_t hash, ta_atom *out) {
    uint32_t value = 0;
    ta_result result = ta_table_find(&table->index,
                                     &(ta_table_key){ .hash = hash, .equal = ta_atom_key_equal, .context = &key },
                                     &value);
    if (result == TA_OK) {
        *out = value - 1u;
    }
    return result;
}

ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator) {
    *table = (ta_atom_table){ .allocator = allocator };
    ta_vec_init(&table->entries, allocator, TA_VEC_LAYOUT(ta_atom_entry));
    ta_result result = ta_table_init(&table->index, &(ta_table_params){ .allocator = allocator,
                                                                        .mode = TA_TABLE_GROWING,
                                                                        .capacity = TA_ATOM_INITIAL_CAPACITY });
    if (result == TA_OK) {
        result = ta_arena_init(&table->strings, allocator, 4096);
    }
    /* Atom 0 is the empty text, which no family is, so a zeroed family
     * atom names none (ADR 0088). */
    ta_atom empty = TA_ATOM_NONE;
    if (result == TA_OK) {
        result = ta_atom_intern(table, (ta_str){ .ptr = "", .len = 0 }, &empty);
    }
    if (result != TA_OK) {
        ta_atom_table_destroy(table);
    }
    return result;
}

void ta_atom_table_destroy(ta_atom_table *table) {
    ta_vec_destroy(&table->entries);
    ta_table_destroy(&table->index);
    ta_arena_destroy(&table->strings);
}

ta_result ta_atom_intern(ta_atom_table *table, ta_str text, ta_atom *out) {
    if (text.ptr == NULL && text.len != 0) {
        return TA_ERR_INVALID;
    }
    uint32_t hash = ta_hash_fnv1a32(TA_HASH_FNV1A32_BASIS, text.ptr, text.len);
    if (ta_atom_lookup(table, (ta_atom_key){ .table = table, .text = text }, hash, out) == TA_OK) {
        return TA_OK;
    }
    if (table->entries.count >= TA_ATOM_NONE - 1u) {
        return TA_ERR_NOMEM;
    }
    /* Room in the hash table first, so the insert below cannot fail; a full
     * table is out of memory to the atom table's callers. */
    ta_result result = ta_table_reserve(&table->index, 1);
    if (result != TA_OK) {
        return TA_ERR_NOMEM;
    }
    ta_atom_entry entry = { .hash = hash };
    result = ta_arena_copy_str(&table->strings, text, &entry.text);
    if (result != TA_OK) {
        return result;
    }
    uint32_t atom = table->entries.count;
    result = ta_vec_push(&table->entries, &entry);
    if (result != TA_OK) {
        return result;
    }
    result = ta_table_insert(&table->index, hash, atom + 1u);
    if (result != TA_OK) {
        table->entries.count -= 1u;
        return TA_ERR_NOMEM;
    }
    *out = atom;
    return TA_OK;
}

bool ta_atom_valid(const ta_atom_table *table, ta_atom atom) {
    return atom < table->entries.count;
}

ta_str ta_atom_text(const ta_atom_table *table, ta_atom atom) {
    if (!ta_atom_valid(table, atom)) {
        return (ta_str){0};
    }
    const ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
    return entry->text;
}

ta_result ta_atom_find(const ta_atom_table *table, ta_str text, ta_atom *out) {
    if (text.ptr == NULL && text.len != 0) {
        return TA_ERR_INVALID;
    }
    uint32_t hash = ta_hash_fnv1a32(TA_HASH_FNV1A32_BASIS, text.ptr, text.len);
    return ta_atom_lookup(table, (ta_atom_key){ .table = table, .text = text }, hash, out);
}
