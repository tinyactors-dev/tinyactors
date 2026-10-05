#include "session.h"
#include "system.h"
#include "memory.h"
#include "../include/tinyactors.h"
#include "vec.h"
#include "datamodel.h"
#include "actor.h"
#include "hash.h"

static uint32_t ta_session_hash(uint64_t id) {
    /* Mixed, since runtime IDs differ mostly in their low slot bits. */
    return (uint32_t)ta_hash_mix64(id);
}

static uint32_t ta_session_bucket_count(const ta_session_table *table) {
    return table->base + table->split;
}

static ta_session_node **ta_session_bucket(const ta_session_table *table, uint32_t index) {
    ta_session_node **page = *(ta_session_node ***)ta_vec_at(&table->pages, index / TA_SESSION_PAGE_BUCKETS);
    return &page[index % TA_SESSION_PAGE_BUCKETS];
}

static uint32_t ta_session_index(const ta_session_table *table, uint32_t hash) {
    uint32_t index = hash & (table->base - 1u);
    return index < table->split ? hash & (2u * table->base - 1u) : index;
}

static void ta_session_page_free(ta_allocator allocator, ta_session_node **page) {
    ta_mem_free(allocator, page,
                &(ta_allocator_free_params){ .size = TA_SESSION_PAGE_BUCKETS * sizeof(ta_session_node *),
                                             .alignment = _Alignof(ta_session_node *) });
}

/* Makes sure the page holding bucket `index` exists; false when it cannot. */
static bool ta_session_page_for(ta_session_table *table, uint32_t index) {
    if (index / TA_SESSION_PAGE_BUCKETS < table->pages.count) {
        return true;
    }
    ta_session_node **page = ta_mem_alloc(table->allocator, TA_SESSION_PAGE_BUCKETS * sizeof(ta_session_node *),
                                          _Alignof(ta_session_node *));
    if (page == NULL) {
        return false;
    }
    for (uint32_t bucket = 0; bucket < TA_SESSION_PAGE_BUCKETS; bucket += 1u) {
        page[bucket] = NULL;
    }
    if (ta_vec_push(&table->pages, &page) != TA_OK) {
        ta_session_page_free(table->allocator, page);
        return false;
    }
    return true;
}

/* Splits one bucket when the table holds more than one entry per bucket. */
static void ta_session_grow(ta_session_table *table) {
    uint32_t added = ta_session_bucket_count(table);
    if (table->count <= added || added == UINT32_MAX || !ta_session_page_for(table, added)) {
        return; /* a failed page only postpones the split */
    }
    uint32_t wide = 2u * table->base - 1u;
    ta_session_node **from = ta_session_bucket(table, table->split);
    ta_session_node **to = ta_session_bucket(table, added);
    ta_session_node *chain = *from;
    *from = NULL;
    while (chain != NULL) {
        ta_session_node *next = chain->next;
        ta_session_node **home = (ta_session_hash(chain->id) & wide) == table->split ? from : to;
        chain->next = *home;
        *home = chain;
        chain = next;
    }
    table->split += 1u;
    if (table->split == table->base) {
        table->base *= 2u;
        table->split = 0;
    }
}

ta_result ta_session_table_init(ta_session_table *table, ta_allocator allocator) {
    *table = (ta_session_table){ .allocator = allocator, .base = TA_SESSION_INITIAL_BUCKETS };
    ta_vec_init(&table->pages, allocator, TA_VEC_LAYOUT(ta_session_node **));
    return ta_session_page_for(table, 0) ? TA_OK : TA_ERR_NOMEM;
}

void ta_session_table_destroy(ta_session_table *table) {
    uint32_t buckets = table->pages.count == 0 ? 0u : ta_session_bucket_count(table);
    for (uint32_t index = 0; index < buckets; index += 1u) {
        ta_session_node *chain = *ta_session_bucket(table, index);
        while (chain != NULL) {
            ta_session_node *next = chain->next;
            ta_session_node_free(table, chain);
            chain = next;
        }
    }
    for (uint32_t page = 0; page < table->pages.count; page += 1u) {
        ta_session_page_free(table->allocator, *(ta_session_node ***)ta_vec_at(&table->pages, page));
    }
    ta_vec_destroy(&table->pages);
    *table = (ta_session_table){0};
}

ta_result ta_session_node_new(ta_session_table *table, ta_session_node **out) {
    ta_session_node *node = ta_mem_alloc(table->allocator, sizeof(ta_session_node), _Alignof(ta_session_node));
    if (node == NULL) {
        return TA_ERR_NOMEM;
    }
    *node = (ta_session_node){0};
    *out = node;
    return TA_OK;
}

void ta_session_node_free(ta_session_table *table, ta_session_node *node) {
    if (node == NULL) {
        return;
    }
    if (node->address != NULL) {
        if (node->address->unloaded == node) {
            node->address->unloaded = NULL;
        }
        ta_address_release(node->address);
    }
    ta_mem_free(table->allocator, node,
                &(ta_allocator_free_params){ .size = sizeof(ta_session_node), .alignment = _Alignof(ta_session_node) });
}

void ta_session_insert(ta_session_table *table, ta_session_node *node) {
    ta_session_node **bucket = ta_session_bucket(table, ta_session_index(table, ta_session_hash(node->id)));
    node->next = *bucket;
    *bucket = node;
    table->count += 1u;
    ta_session_grow(table);
}

void ta_session_remove(ta_session_table *table, ta_session_node *node) {
    for (ta_session_node **link = ta_session_bucket(table, ta_session_index(table, ta_session_hash(node->id)));
         *link != NULL; link = &(*link)->next) {
        if (*link == node) {
            *link = node->next;
            node->next = NULL;
            table->count -= 1u;
            return;
        }
    }
}

const ta_session_node *ta_session_find(const ta_session_table *table, uint64_t id) {
    if (table->pages.count == 0) {
        return NULL;
    }
    const ta_session_node *unloaded = NULL;
    for (const ta_session_node *node = *ta_session_bucket(table, ta_session_index(table, ta_session_hash(id)));
         node != NULL; node = node->next) {
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
