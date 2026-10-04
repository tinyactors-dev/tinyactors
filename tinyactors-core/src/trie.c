#include "trie.h"
#include "hash.h"

/* The package comment is in trie.h. */

static const ta_trie_node *ta_trie_node_at(const ta_atom_trie *trie, uint32_t node) {
    return ta_vec_at(&trie->nodes, node);
}

static uint32_t ta_trie_hash(uint32_t parent, ta_atom segment) {
    uint32_t key[2] = { parent, segment };
    /* Offsetting the basis keeps these hashes apart from the atom table's. */
    return ta_fnv1a32(TA_FNV1A32_BASIS ^ 0x9e3779b9u, key, sizeof(key));
}

/* The node named by `key` (below key->parent, with key->segment); false when
 * there is none. The hash is computed here so the signature stays within
 * three parameters; a miss in ta_trie_child hashes the key a second time,
 * which costs less than the allocation that follows it. */
static bool ta_trie_lookup(const ta_atom_trie *trie, const ta_trie_key *key, uint32_t *out) {
    ta_table_probe probe;
    uint32_t node = 0;
    uint32_t parent = key->parent;
    ta_atom segment = key->segment;
    ta_table_probe_start(&trie->index, ta_trie_hash(parent, segment), &probe);
    while (ta_table_probe_next(&probe, &node)) {
        const ta_trie_node *candidate = ta_trie_node_at(trie, node);
        if (candidate->parent == parent && candidate->segment == segment) {
            *out = node;
            return true;
        }
    }
    return false;
}

ta_result ta_trie_init(ta_atom_trie *trie, ta_allocator allocator, uint32_t max_depth) {
    *trie = (ta_atom_trie){0};
    if (max_depth == 0 || max_depth > TA_TRIE_MAX_DEPTH) {
        return TA_ERR_INVALID;
    }
    trie->max_depth = max_depth;
    ta_vec_init(&trie->nodes, allocator, TA_VEC_LAYOUT(ta_trie_node));
    ta_trie_node root = { .parent = TA_TRIE_ROOT, .segment = TA_ATOM_NONE, .depth = 0 };
    ta_result result = ta_vec_push(&trie->nodes, &root);
    if (result == TA_OK) {
        result = ta_table_init(&trie->index, allocator,
                               &(ta_table_params){ .mode = TA_TABLE_GROWING, .page_slots = TA_TRIE_PAGE_SLOTS });
    }
    if (result != TA_OK) {
        ta_trie_destroy(trie);
    }
    return result;
}

void ta_trie_destroy(ta_atom_trie *trie) {
    ta_vec_destroy(&trie->nodes);
    ta_table_destroy(&trie->index);
    trie->max_depth = 0;
}

bool ta_trie_valid(const ta_atom_trie *trie, uint32_t node) {
    return node < trie->nodes.count;
}

ta_result ta_trie_find(const ta_atom_trie *trie, const ta_trie_key *key, uint32_t *out) {
    uint32_t parent = key->parent;
    ta_atom segment = key->segment;
    if (!ta_trie_valid(trie, parent) || segment == TA_ATOM_NONE) {
        return TA_ERR_INVALID;
    }
    return ta_trie_lookup(trie, key, out) ? TA_OK : TA_ERR_NOT_FOUND;
}

ta_result ta_trie_child(ta_atom_trie *trie, const ta_trie_key *key, uint32_t *out) {
    uint32_t parent = key->parent;
    ta_atom segment = key->segment;
    if (!ta_trie_valid(trie, parent) || segment == TA_ATOM_NONE) {
        return TA_ERR_INVALID;
    }
    uint32_t depth = ta_trie_node_at(trie, parent)->depth + 1u;
    if (depth > trie->max_depth) {
        return TA_ERR_INVALID;
    }
    if (ta_trie_lookup(trie, key, out)) {
        return TA_OK;
    }
    uint32_t node = trie->nodes.count;
    ta_result result = ta_vec_reserve(&trie->nodes, node + 1u);
    if (result != TA_OK) {
        return result;
    }
    /* Growth is asked for here, ahead of the insert, because invoke IDs still
     * make names inside macrosteps; Phase 4 (4.2) removes that, and growth
     * then happens only at the safe points that make names. A failed grow
     * only postpones: the pages fill a little more. */
    if (ta_table_wants_grow(&trie->index)) {
        (void)ta_table_grow(&trie->index);
    }
    result = ta_table_insert(&trie->index, ta_trie_hash(parent, segment), node);
    if (result != TA_OK) {
        return result;
    }
    ta_trie_node record = { .parent = parent, .segment = segment, .depth = depth };
    (void)ta_vec_push(&trie->nodes, &record); /* reserved above */
    *out = node;
    return TA_OK;
}

uint32_t ta_trie_segments(const ta_atom_trie *trie, uint32_t node, ta_atom out[TA_TRIE_MAX_DEPTH]) {
    if (!ta_trie_valid(trie, node)) {
        return 0;
    }
    uint32_t depth = ta_trie_node_at(trie, node)->depth;
    for (uint32_t index = depth; index > 0; index -= 1) {
        const ta_trie_node *step = ta_trie_node_at(trie, node);
        out[index - 1u] = step->segment;
        node = step->parent;
    }
    return depth;
}

bool ta_trie_under(const ta_atom_trie *trie, uint32_t node, uint32_t prefix) {
    if (!ta_trie_valid(trie, node) || !ta_trie_valid(trie, prefix)) {
        return false;
    }
    uint32_t depth = ta_trie_node_at(trie, prefix)->depth;
    while (ta_trie_node_at(trie, node)->depth > depth) {
        node = ta_trie_node_at(trie, node)->parent;
    }
    return node == prefix;
}
