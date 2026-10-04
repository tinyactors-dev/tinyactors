#include "atom.h"
#include "hash.h"

/* Slots per index page: 2 KiB, so the atom index grows in small, flat steps. */
#define TA_ATOM_PAGE_SLOTS 256u

static uint32_t ta_atom_hash(ta_str text) {
    return ta_fnv1a32(TA_FNV1A32_BASIS, text.ptr, text.len);
}

/* The atom whose text is `text` among those stored under `hash`; false when there is none. */
static bool ta_atom_lookup(const ta_atom_table *table, ta_str text, uint32_t hash, ta_atom *out) {
    ta_table_probe probe;
    uint32_t atom = 0;
    ta_table_probe_start(&table->index, hash, &probe);
    while (ta_table_probe_next(&probe, &atom)) {
        const ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
        if (ta_str_equal(entry->text, text)) {
            *out = atom;
            return true;
        }
    }
    return false;
}

ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator) {
    *table = (ta_atom_table){ .allocator = allocator };
    ta_vec_init(&table->entries, allocator, TA_VEC_LAYOUT(ta_atom_entry));
    ta_result result = ta_arena_init(&table->strings, allocator, 4096);
    if (result == TA_OK) {
        result = ta_table_init(&table->index, allocator,
                               &(ta_table_params){ .mode = TA_TABLE_GROWING, .page_slots = TA_ATOM_PAGE_SLOTS });
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
    uint32_t hash = ta_atom_hash(text);
    if (ta_atom_lookup(table, text, hash, out)) {
        return TA_OK;
    }
    if (table->entries.count >= TA_ATOM_NONE - 1u) {
        return TA_ERR_NOMEM;
    }
    ta_atom atom = table->entries.count;
    ta_atom_entry entry;
    ta_result result = ta_vec_reserve(&table->entries, atom + 1u);
    if (result == TA_OK) {
        result = ta_arena_copy_str(&table->strings, text, &entry.text);
    }
    if (result != TA_OK) {
        return result;
    }
    /* Growth is asked for here, ahead of the insert, because interning still
     * runs inside macrosteps; Phase 4 (4.2) removes that, and growth then
     * happens only at the safe points that make atoms. A failed grow only
     * postpones: the pages fill a little more. */
    if (ta_table_wants_grow(&table->index)) {
        (void)ta_table_grow(&table->index);
    }
    result = ta_table_insert(&table->index, hash, atom);
    if (result != TA_OK) {
        return result;
    }
    (void)ta_vec_push(&table->entries, &entry); /* reserved above */
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
    return ta_atom_lookup(table, text, ta_atom_hash(text), out) ? TA_OK : TA_ERR_NOT_FOUND;
}
