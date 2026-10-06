#include "trie.h"
#include "atom.h"
#include "hash.h"
#include "table.h"
#include "vec.h"
#include "../include/tinyactors.h"

/* The nodes the hash table has room for when the trie is made. */
#define TA_TRIE_INITIAL_CAPACITY 48u

/* What the hash table's comparison needs: the trie and the edge looked for. */
typedef struct {
    const ta_trie *trie;
    ta_trie_edge edge;
} ta_trie_key;

static const ta_trie_node *ta_trie_node_at(const ta_trie *trie, ta_trie_id id) {
    return ta_vec_at(&trie->nodes, id);
}

/* The hash of (parent, segment): FNV-1a 32 of both words, from a seeded basis. */
static uint32_t ta_trie_hash(ta_trie_id parent, ta_atom segment) {
    uint32_t key[2] = { parent, segment };
    return ta_hash_fnv1a32(TA_HASH_FNV1A32_BASIS ^ 0x9e3779b9u, key, sizeof(key));
}

/* Whether node `value` has the parent and segment in `context`, a ta_trie_key. */
static bool ta_trie_key_equal(const void *context, uint32_t value) {
    const ta_trie_key *key = context;
    const ta_trie_node *node = ta_trie_node_at(key->trie, value);
    return node->parent == key->edge.parent && node->segment == key->edge.segment;
}

/* Answers TA_OK when the edge's parent is a node and its segment an atom. */
static ta_result ta_trie_check_edge(const ta_trie *trie, ta_trie_edge edge) {
    return ta_trie_valid(trie, edge.parent) && ta_atom_valid(trie->atoms, edge.segment) ? TA_OK : TA_ERR_INVALID;
}

/* Makes room for `more` nodes, so as many adds that follow cannot fail. */
static ta_result ta_trie_reserve(ta_trie *trie, uint32_t more) {
    if (more > TA_TRIE_MAX_NODES - trie->nodes.count) {
        return TA_ERR_FULL;
    }
    ta_result result = ta_table_reserve(&trie->index, more);
    return result == TA_OK ? ta_vec_reserve(&trie->nodes, trie->nodes.count + more) : result;
}

/* Finds or adds the node of `edge`, a checked edge, with room for one more node made. */
static ta_trie_id ta_trie_add(ta_trie *trie, ta_trie_edge edge) {
    uint32_t hash = ta_trie_hash(edge.parent, edge.segment);
    ta_trie_key key = { .trie = trie, .edge = edge };
    uint32_t id = 0;
    if (ta_table_find(&trie->index, &(ta_table_key){ .hash = hash, .equal = ta_trie_key_equal, .context = &key },
                      &id) == TA_OK) {
        return id;
    }
    id = trie->nodes.count;
    ta_trie_node node = { .parent = edge.parent,
                          .segment = edge.segment,
                          .depth = ta_trie_node_at(trie, edge.parent)->depth + 1u };
    /* Both have room: the caller reserved it. */
    (void)ta_vec_push(&trie->nodes, &node);
    (void)ta_table_insert(&trie->index, hash, id);
    return id;
}

ta_result ta_trie_init(ta_trie *trie, const ta_trie_params *params) {
    *trie = (ta_trie){ .allocator = params->allocator, .atoms = params->atoms, .max_depth = params->max_depth };
    ta_vec_init(&trie->nodes, params->allocator, TA_VEC_LAYOUT(ta_trie_node));
    if (params->max_depth == 0 || params->max_depth > TA_TRIE_MAX_DEPTH) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_table_init(&trie->index, &(ta_table_params){ .allocator = params->allocator,
                                                                       .mode = TA_TABLE_GROWING,
                                                                       .capacity = TA_TRIE_INITIAL_CAPACITY });
    ta_trie_node root = { .parent = TA_TRIE_ROOT, .segment = TA_ATOM_NONE, .depth = 0 };
    if (result == TA_OK) {
        result = ta_vec_push(&trie->nodes, &root);
    }
    if (result != TA_OK) {
        ta_trie_destroy(trie);
    }
    return result;
}

void ta_trie_destroy(ta_trie *trie) {
    ta_vec_destroy(&trie->nodes);
    ta_table_destroy(&trie->index);
}

bool ta_trie_valid(const ta_trie *trie, ta_trie_id id) {
    return id < trie->nodes.count;
}

ta_result ta_trie_find(const ta_trie *trie, ta_trie_edge edge, ta_trie_id *out) {
    ta_result result = ta_trie_check_edge(trie, edge);
    if (result != TA_OK) {
        return result;
    }
    ta_trie_key key = { .trie = trie, .edge = edge };
    return ta_table_find(&trie->index,
                         &(ta_table_key){ .hash = ta_trie_hash(edge.parent, edge.segment),
                                          .equal = ta_trie_key_equal,
                                          .context = &key },
                         out);
}

ta_result ta_trie_child(ta_trie *trie, ta_trie_edge edge, ta_trie_id *out) {
    ta_result result = ta_trie_check_edge(trie, edge);
    if (result != TA_OK) {
        return result;
    }
    if (ta_trie_find(trie, edge, out) == TA_OK) {
        return TA_OK;
    }
    if (ta_trie_node_at(trie, edge.parent)->depth + 1u > trie->max_depth) {
        return TA_ERR_INVALID;
    }
    result = ta_trie_reserve(trie, 1);
    if (result == TA_OK) {
        *out = ta_trie_add(trie, edge);
    }
    return result;
}

ta_result ta_trie_intern_path(ta_trie *trie, const ta_atom_list *segments, ta_trie_id *out) {
    if (segments == NULL || segments->count == 0 || segments->atoms == NULL ||
        segments->count > trie->max_depth) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < segments->count; index += 1) {
        if (!ta_atom_valid(trie->atoms, segments->atoms[index])) {
            return TA_ERR_INVALID;
        }
    }
    /* The nodes below the longest prefix the trie holds are all new: room
     * for exactly those, so a path the trie holds never grows it. */
    ta_trie_id id = TA_TRIE_ROOT;
    uint32_t held = 0;
    while (held < segments->count &&
           ta_trie_find(trie, (ta_trie_edge){ .parent = id, .segment = segments->atoms[held] }, &id) == TA_OK) {
        held += 1u;
    }
    if (held < segments->count) {
        ta_result result = ta_trie_reserve(trie, segments->count - held);
        if (result != TA_OK) {
            return result;
        }
    }
    for (uint32_t index = held; index < segments->count; index += 1) {
        id = ta_trie_add(trie, (ta_trie_edge){ .parent = id, .segment = segments->atoms[index] });
    }
    *out = id;
    return TA_OK;
}

ta_result ta_trie_intern_dotted(ta_trie *trie, ta_str dotted, ta_trie_id *out) {
    if (dotted.ptr == NULL || dotted.len == 0) {
        return TA_ERR_INVALID;
    }
    /* Checks the segments before it interns any. */
    uint32_t count = 1;
    for (uint32_t index = 0; index <= dotted.len; index += 1) {
        bool end = index == dotted.len || dotted.ptr[index] == '.';
        bool empty = index == 0 || dotted.ptr[index - 1u] == '.';
        if (end && empty) {
            return TA_ERR_INVALID;
        }
        count += index < dotted.len && dotted.ptr[index] == '.' ? 1u : 0u;
    }
    if (count > trie->max_depth) {
        return TA_ERR_INVALID;
    }
    ta_atom atoms[TA_TRIE_MAX_DEPTH];
    uint32_t segment = 0;
    uint32_t start = 0;
    for (uint32_t index = 0; index <= dotted.len; index += 1) {
        if (index < dotted.len && dotted.ptr[index] != '.') {
            continue;
        }
        ta_result result = ta_atom_intern(trie->atoms, (ta_str){ .ptr = dotted.ptr + start, .len = index - start },
                                          &atoms[segment]);
        if (result != TA_OK) {
            return result;
        }
        segment += 1u;
        start = index + 1u;
    }
    return ta_trie_intern_path(trie, &(ta_atom_list){ .atoms = atoms, .count = count }, out);
}

bool ta_trie_has_prefix(const ta_trie *trie, ta_trie_id id, ta_trie_id prefix) {
    if (!ta_trie_valid(trie, id) || !ta_trie_valid(trie, prefix)) {
        return false;
    }
    uint32_t depth = ta_trie_node_at(trie, prefix)->depth;
    while (ta_trie_node_at(trie, id)->depth > depth) {
        id = ta_trie_node_at(trie, id)->parent;
    }
    return id == prefix;
}

uint32_t ta_trie_path(const ta_trie *trie, ta_trie_id id, ta_atom out[TA_TRIE_MAX_DEPTH]) {
    if (!ta_trie_valid(trie, id)) {
        return 0;
    }
    uint32_t depth = ta_trie_node_at(trie, id)->depth;
    for (uint32_t index = depth; index > 0; index -= 1) {
        const ta_trie_node *node = ta_trie_node_at(trie, id);
        out[index - 1u] = node->segment;
        id = node->parent;
    }
    return depth;
}
