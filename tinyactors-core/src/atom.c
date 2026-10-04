#include "atom.h"
#include "memory.h"
#include "str.h"
#include "../include/tinyactors.h"
#include "vec.h"
#include "arena.h"

uint32_t ta_hash_bytes(uint32_t seed, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    uint32_t hash = seed ^ 2166136261u;
    for (size_t index = 0; index < size; index += 1) {
        hash ^= in[index];
        hash *= 16777619u;
    }
    return hash;
}

static ta_result ta_atom_rehash(ta_atom_table *table, uint32_t bucket_count) {
    uint32_t *buckets = ta_mem_alloc(table->allocator, bucket_count * sizeof(uint32_t),
                                     _Alignof(uint32_t));
    if (buckets == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t index = 0; index < bucket_count; index += 1) {
        buckets[index] = TA_ATOM_NONE;
    }
    for (uint32_t atom = 0; atom < table->entries.count; atom += 1) {
        ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
        uint32_t bucket = entry->hash & (bucket_count - 1u);
        entry->next = buckets[bucket];
        buckets[bucket] = atom;
    }
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    table->buckets = buckets;
    table->bucket_count = bucket_count;
    return TA_OK;
}

ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator) {
    *table = (ta_atom_table){ .allocator = allocator };
    ta_vec_init(&table->entries, allocator, TA_VEC_LAYOUT(ta_atom_entry));
    ta_result result = ta_arena_init(&table->strings, allocator, 4096);
    if (result == TA_OK) {
        result = ta_atom_rehash(table, 64);
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
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    ta_arena_destroy(&table->strings);
    table->buckets = NULL;
    table->bucket_count = 0;
}

ta_result ta_atom_intern(ta_atom_table *table, ta_str text, ta_atom *out) {
    if (text.ptr == NULL && text.len != 0) {
        return TA_ERR_INVALID;
    }
    uint32_t hash = ta_hash_bytes(0, text.ptr, text.len);
    for (uint32_t atom = table->buckets[hash & (table->bucket_count - 1u)]; atom != TA_ATOM_NONE;) {
        const ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
        if (entry->hash == hash && ta_str_equal(entry->text, text)) {
            *out = atom;
            return TA_OK;
        }
        atom = entry->next;
    }
    if (table->entries.count >= TA_ATOM_NONE - 1u) {
        return TA_ERR_NOMEM;
    }
    ta_atom_entry entry = { .hash = hash };
    ta_result result = ta_arena_copy_str(&table->strings, text, &entry.text);
    if (result != TA_OK) {
        return result;
    }
    uint32_t atom = table->entries.count;
    uint32_t bucket = hash & (table->bucket_count - 1u);
    entry.next = table->buckets[bucket];
    result = ta_vec_push(&table->entries, &entry);
    if (result != TA_OK) {
        return result;
    }
    table->buckets[bucket] = atom;
    if (table->entries.count > table->bucket_count) {
        /* A failed rehash leaves the old, still-correct buckets. */
        (void)ta_atom_rehash(table, table->bucket_count * 2u);
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
    uint32_t hash = ta_hash_bytes(0, text.ptr, text.len);
    for (uint32_t atom = table->buckets[hash & (table->bucket_count - 1u)]; atom != TA_ATOM_NONE;) {
        const ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
        if (entry->hash == hash && ta_str_equal(entry->text, text)) {
            *out = atom;
            return TA_OK;
        }
        atom = entry->next;
    }
    return TA_ERR_NOT_FOUND;
}
