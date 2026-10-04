#include "session.h"
#include "hash.h"
#include "system.h"

/* The package comment is in session.h. */

/*
 * A page of the node pool holds 1024 nodes of 24 bytes (24 KiB), and a page
 * of the index 4096 slots of 8 bytes (32 KiB), so the largest block the
 * table asks for stays flat as sessions come and go. The index grows at 7/16
 * of its slots: sessions are few, and a denser index costs less than the
 * longer probes just before a round of splits ends.
 */
#define TA_SESSION_PAGE_NODES 1024u
#define TA_SESSION_PAGE_SLOTS 4096u
#define TA_SESSION_GROW_SIXTEENTHS 7u

/* The free-list link lives in the low half of `actor`, which means nothing while the node is free. */
static const ta_pool_params TA_SESSION_POOL = {
    .item_size = sizeof(ta_session_node),
    .item_alignment = _Alignof(ta_session_node),
    .link_offset = offsetof(ta_session_node, actor),
    .page_items = TA_SESSION_PAGE_NODES,
};

static const ta_table_params TA_SESSION_INDEX = {
    .mode = TA_TABLE_GROWING,
    .page_slots = TA_SESSION_PAGE_SLOTS,
    .grow_sixteenths = TA_SESSION_GROW_SIXTEENTHS,
};

/* Runtime IDs differ mostly in their low slot bits, so the hash is the
 * 64-bit finalizer (D5) cut to 32 bits. */
static uint32_t ta_session_hash(uint64_t id) {
    return (uint32_t)ta_mix64(id);
}

void ta_session_table_init(ta_session_table *table, ta_allocator allocator) {
    *table = (ta_session_table){ .allocator = allocator };
    /* The parameters are constants that make a pool; init only validates them. */
    (void)ta_pool_init(&table->nodes, allocator, &TA_SESSION_POOL);
}

ta_session_node *ta_session_node_at(const ta_session_table *table, uint32_t index) {
    return ta_pool_at(&table->nodes, index);
}

/* The address of an unloaded node lets go of this node, then the reference is returned. */
static void ta_session_release_address(ta_session_node *node, uint32_t index) {
    ta_address *address = node->address;
    if (address != NULL) {
        if (address->unloaded == index) {
            address->unloaded = 0;
        }
        node->address = NULL;
        ta_address_release(address);
    }
}

void ta_session_table_destroy(ta_session_table *table) {
    ta_table_cursor cursor = {0};
    uint32_t hash = 0;
    uint32_t index = 0;
    while (ta_table_next(&table->index, &cursor, &hash, &index)) {
        ta_session_release_address(ta_session_node_at(table, index), index);
    }
    ta_table_destroy(&table->index);
    ta_pool_destroy(&table->nodes);
    *table = (ta_session_table){0};
}

ta_result ta_session_node_new(ta_session_table *table, const ta_session_node_params *params, uint32_t *out) {
    if (ta_pool_refill(&table->nodes, 1u) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    if (table->index.page_slots == 0) {
        ta_result result = ta_table_init(&table->index, table->allocator, &TA_SESSION_INDEX);
        if (result != TA_OK) {
            return result;
        }
    }
    uint32_t hash = ta_session_hash(params->id);
    if (ta_table_wants_grow(&table->index)) {
        (void)ta_table_grow(&table->index); /* a failure only postpones the split */
    }
    if (ta_table_prepare(&table->index, hash) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    uint32_t index = ta_pool_take(&table->nodes); /* refilled above, and nothing took it since */
    *ta_session_node_at(table, index) = (ta_session_node){ .id = params->id, .actor = params->actor };
    *out = index;
    return TA_OK;
}

void ta_session_insert(ta_session_table *table, uint32_t index) {
    /* Room was made by ta_session_node_new, and no insert came between. */
    (void)ta_table_insert(&table->index, ta_session_hash(ta_session_node_at(table, index)->id), index);
}

void ta_session_node_free(ta_session_table *table, uint32_t index) {
    ta_session_release_address(ta_session_node_at(table, index), index);
    ta_pool_give(&table->nodes, index);
}

void ta_session_drop(ta_session_table *table, uint32_t index) {
    (void)ta_table_remove(&table->index, ta_session_hash(ta_session_node_at(table, index)->id), index);
    ta_session_node_free(table, index);
}

void ta_session_drop_resident(ta_session_table *table, uint64_t id, const struct ta_actor *actor) {
    ta_table_probe probe;
    uint32_t index = 0;
    uint32_t found = 0;
    ta_table_probe_start(&table->index, ta_session_hash(id), &probe);
    while (found == 0 && ta_table_probe_next(&probe, &index)) {
        const ta_session_node *node = ta_session_node_at(table, index);
        found = node->id == id && node->actor == actor ? index : 0u;
    }
    if (found != 0) { /* the probe is dead once the index changes */
        ta_session_drop(table, found);
    }
}

void ta_session_drop_unloaded(ta_session_table *table, ta_address *address) {
    if (address->unloaded != 0) {
        ta_session_drop(table, address->unloaded);
    }
}

void ta_session_park(ta_session_table *table, uint32_t index, ta_address *address) {
    ta_address_retain(address); /* before the old node lets go of its own reference */
    ta_session_drop_unloaded(table, address);
    ta_session_node_at(table, index)->address = address;
    address->unloaded = index;
    ta_session_insert(table, index);
}

const ta_session_node *ta_session_find(const ta_session_table *table, uint64_t id) {
    const ta_session_node *unloaded = NULL;
    ta_table_probe probe;
    uint32_t index = 0;
    ta_table_probe_start(&table->index, ta_session_hash(id), &probe);
    while (ta_table_probe_next(&probe, &index)) {
        const ta_session_node *node = ta_session_node_at(table, index);
        if (node->id != id) {
            continue;
        }
        if (node->actor != NULL) {
            return node;
        }
        unloaded = node;
    }
    return unloaded;
}

bool ta_session_resident(const ta_system *system, uint64_t id) {
    const ta_actor *actor = ta_actor_lookup(system, id);
    if (actor != NULL && actor->session_id == id) {
        return true;
    }
    const ta_session_node *node = ta_session_find(&system->sessions, id);
    return node != NULL && node->actor != NULL;
}

uint64_t ta_session_id_of(const ta_system *system, ta_actor_id runtime) {
    const ta_actor *actor = ta_actor_lookup(system, runtime);
    return actor != NULL ? actor->session_id : runtime;
}

/* Whether `actor` is of the family `ref` names; a reference naming none matches any (ADR 0088). */
static bool ta_session_family_matches(const ta_actor_ref *ref, const ta_actor *actor) {
    return ref->family == 0 || actor->definition->family == ref->family;
}

bool ta_session_route(const ta_system *system, const ta_actor_ref *ref, ta_actor_ref *out) {
    *out = *ref;
    if (ref->kind != TA_ACTOR_REF_RUNTIME) {
        return false;
    }
    const ta_actor *resident = ta_actor_lookup(system, ref->as.runtime);
    if (resident != NULL) {
        /* Another family's actor holds the ID: the session named is gone. */
        if (!ta_session_family_matches(ref, resident)) {
            out->as.runtime = 0;
        }
        return false;
    }
    const ta_session_node *node = ta_session_find(&system->sessions, ref->as.runtime);
    if (node == NULL) {
        return false;
    }
    if (node->actor != NULL) {
        *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME,
                               .family = ref->family,
                               .as.runtime = ta_session_family_matches(ref, node->actor) ? node->actor->record.id : 0 };
        return false;
    }
    ta_address_retain(node->address);
    *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_NAMED, .as.named = node->address };
    return true;
}
