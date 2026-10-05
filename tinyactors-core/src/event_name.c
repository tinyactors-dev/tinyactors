#include "event_name.h"
#include "memory.h"
#include "../include/tinyactors.h"
#include "vec.h"
#include "atom.h"
#include "hash.h"
#include "str.h"

static const ta_event_name_node *ta_event_name_node_at(const ta_event_name_table *table,
                                                       ta_event_name_id name) {
    return ta_vec_at(&table->nodes, name);
}

static uint32_t ta_event_name_hash(ta_event_name_id parent, ta_atom segment) {
    uint32_t key[2] = { parent, segment };
    return ta_hash_fnv1a32(TA_HASH_FNV1A32_BASIS ^ 0x9e3779b9u, key, sizeof(key));
}

static ta_result ta_event_name_rehash(ta_event_name_table *table, uint32_t bucket_count) {
    uint32_t *buckets = ta_mem_alloc(table->allocator, bucket_count * sizeof(uint32_t),
                                     _Alignof(uint32_t));
    if (buckets == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t index = 0; index < bucket_count; index += 1) {
        buckets[index] = UINT32_MAX;
    }
    /* Node 0 is the root and is never looked up as a child. */
    for (uint32_t name = 1; name < table->nodes.count; name += 1) {
        ta_event_name_node *node = ta_vec_at(&table->nodes, name);
        uint32_t bucket = ta_event_name_hash(node->parent, node->segment) & (bucket_count - 1u);
        node->hash_next = buckets[bucket];
        buckets[bucket] = name;
    }
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    table->buckets = buckets;
    table->bucket_count = bucket_count;
    return TA_OK;
}

ta_result ta_event_name_child(ta_event_name_table *table,
                              ta_event_name_id parent,
                              ta_atom segment,
                              ta_event_name_id *out) {
    if (!ta_event_name_valid(table, parent) || !ta_atom_valid(table->atoms, segment)) {
        return TA_ERR_INVALID;
    }
    uint32_t depth = ta_event_name_node_at(table, parent)->depth + 1u;
    if (depth > table->max_depth) {
        return TA_ERR_INVALID;
    }
    uint32_t hash = ta_event_name_hash(parent, segment);
    for (uint32_t name = table->buckets[hash & (table->bucket_count - 1u)]; name != UINT32_MAX;) {
        const ta_event_name_node *node = ta_event_name_node_at(table, name);
        if (node->parent == parent && node->segment == segment) {
            *out = name;
            return TA_OK;
        }
        name = node->hash_next;
    }
    uint32_t name = table->nodes.count;
    ta_event_name_node node = {
        .parent = parent,
        .segment = segment,
        .depth = (uint16_t)depth,
        .hash_next = table->buckets[hash & (table->bucket_count - 1u)],
    };
    ta_result result = ta_vec_push(&table->nodes, &node);
    if (result != TA_OK) {
        return result;
    }
    table->buckets[hash & (table->bucket_count - 1u)] = name;
    if (table->nodes.count > table->bucket_count) {
        (void)ta_event_name_rehash(table, table->bucket_count * 2u);
    }
    *out = name;
    return TA_OK;
}

static ta_result ta_event_name_builtin(ta_event_name_table *table, const char *parent_and_segment) {
    /* "P:segment" where P is the parent's built-in index digit. */
    ta_event_name_id parent = (ta_event_name_id)(parent_and_segment[0] - '0');
    ta_str text = ta_str_from_cstr(parent_and_segment + 2);
    ta_atom atom = 0;
    ta_event_name_id name = 0;
    ta_result result = ta_atom_intern(table->atoms, text, &atom);
    return result == TA_OK ? ta_event_name_child(table, parent, atom, &name) : result;
}

ta_result ta_event_name_table_init(ta_event_name_table *table,
                                   ta_allocator allocator,
                                   ta_atom_table *atoms) {
    *table = (ta_event_name_table){ .allocator = allocator, .atoms = atoms, .max_depth = TA_EVENT_NAME_MAX_DEPTH };
    ta_vec_init(&table->nodes, allocator, TA_VEC_LAYOUT(ta_event_name_node));
    ta_event_name_node root = { .parent = TA_EVENT_NAME_ROOT, .segment = TA_ATOM_NONE };
    ta_result result = ta_vec_push(&table->nodes, &root);
    if (result == TA_OK) {
        result = ta_event_name_rehash(table, 64);
    }
    /* Order matches the TA_EVENT_NAME_* constants. */
    static const char *const builtins[] = {
        "0:error", "1:execution", "1:communication", "1:platform",
        "0:done", "5:state", "5:invoke",
    };
    for (uint32_t index = 0; result == TA_OK && index < TA_ARRAY_COUNT(builtins); index += 1) {
        result = ta_event_name_builtin(table, builtins[index]);
    }
    if (result != TA_OK) {
        ta_event_name_table_destroy(table);
    }
    return result;
}

void ta_event_name_table_destroy(ta_event_name_table *table) {
    ta_vec_destroy(&table->nodes);
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    table->buckets = NULL;
    table->bucket_count = 0;
}

bool ta_event_name_valid(const ta_event_name_table *table, ta_event_name_id name) {
    return name < table->nodes.count;
}

ta_result ta_event_name_intern_atoms_internal(ta_event_name_table *table,
                                              const ta_atom_list *segments,
                                              ta_event_name_id *out) {
    if (segments == NULL || segments->count == 0 || segments->atoms == NULL ||
        segments->count > table->max_depth) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < segments->count; index += 1) {
        if (!ta_atom_valid(table->atoms, segments->atoms[index])) {
            return TA_ERR_INVALID;
        }
    }
    ta_event_name_id name = TA_EVENT_NAME_ROOT;
    for (uint32_t index = 0; index < segments->count; index += 1) {
        ta_result result = ta_event_name_child(table, name, segments->atoms[index], &name);
        if (result != TA_OK) {
            return result;
        }
    }
    *out = name;
    return TA_OK;
}

ta_result ta_event_name_intern_dotted(ta_event_name_table *table, ta_str dotted, ta_event_name_id *out) {
    if (dotted.ptr == NULL || dotted.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_event_name_id name = TA_EVENT_NAME_ROOT;
    uint32_t start = 0;
    for (uint32_t index = 0; index <= dotted.len; index += 1) {
        if (index < dotted.len && dotted.ptr[index] != '.') {
            continue;
        }
        if (index == start) {
            return TA_ERR_INVALID;
        }
        ta_atom atom = 0;
        ta_result result = ta_atom_intern(table->atoms,
                                          (ta_str){ .ptr = dotted.ptr + start, .len = index - start },
                                          &atom);
        if (result == TA_OK) {
            result = ta_event_name_child(table, name, atom, &name);
        }
        if (result != TA_OK) {
            return result;
        }
        start = index + 1u;
    }
    *out = name;
    return TA_OK;
}

bool ta_event_name_matches_internal(const ta_event_name_table *table,
                                    ta_event_name_id event,
                                    ta_event_name_id prefix) {
    if (!ta_event_name_valid(table, event) || !ta_event_name_valid(table, prefix)) {
        return false;
    }
    uint16_t depth = ta_event_name_node_at(table, prefix)->depth;
    while (ta_event_name_node_at(table, event)->depth > depth) {
        event = ta_event_name_node_at(table, event)->parent;
    }
    return event == prefix;
}

uint32_t ta_event_name_segments(const ta_event_name_table *table,
                                ta_event_name_id name,
                                ta_atom out[TA_EVENT_NAME_MAX_DEPTH]) {
    if (!ta_event_name_valid(table, name)) {
        return 0;
    }
    uint32_t depth = ta_event_name_node_at(table, name)->depth;
    for (uint32_t index = depth; index > 0; index -= 1) {
        const ta_event_name_node *node = ta_event_name_node_at(table, name);
        out[index - 1u] = node->segment;
        name = node->parent;
    }
    return depth;
}
