#include "waiting.h"
#include "../include/tinyactors.h"
#include "address.h"
#include "hash.h"
#include "index_pool.h"
#include "list.h"
#include "routing.h"
#include "table.h"

ta_result ta_waiting_init(ta_waiting *waiting, const ta_waiting_params *params) {
    if (params->bound == 0u || params->reserve == 0u || params->reserve > params->bound) {
        return TA_ERR_INVALID;
    }
    ta_index_pool items;
    ta_result result = ta_index_pool_init(&items, &(ta_index_pool_params){
                                                      .allocator = params->allocator,
                                                      .item_size = sizeof(ta_waiting_entry),
                                                      .item_alignment = _Alignof(ta_waiting_entry),
                                                      .link_offset = offsetof(ta_waiting_entry, next),
                                                      .page_shift = TA_WAITING_PAGE_SHIFT,
                                                      .ceiling = params->bound,
                                                  });
    if (result != TA_OK) {
        return result;
    }
    ta_table table;
    result = ta_table_init(&table, &(ta_table_params){ .allocator = params->allocator,
                                                       .mode = TA_TABLE_GROWING,
                                                       .capacity = 0u });
    if (result != TA_OK) {
        ta_index_pool_destroy(&items);
        return result;
    }
    *waiting = (ta_waiting){ .items = items, .table = table, .owed = TA_LIST_EMPTY, .reserve = params->reserve };
    return TA_OK;
}

void ta_waiting_destroy(ta_waiting *waiting) {
    for (uint32_t index = 1u; index <= waiting->items.high_water; index += 1u) {
        ta_waiting_entry *entry = ta_waiting_at(waiting, index);
        if (entry != NULL && (entry->flags & TA_WAITING_LIVE) != 0u) {
            ta_actor_ref_release(&entry->destination);
            entry->flags = 0u;
        }
    }
    ta_table_destroy(&waiting->table);
    ta_index_pool_destroy(&waiting->items);
    waiting->owed = TA_LIST_EMPTY;
}

ta_result ta_waiting_refill(ta_waiting *waiting) {
    ta_index_pool *items = &waiting->items;
    uint32_t room = items->ceiling - items->used;
    uint32_t want = waiting->reserve < room ? waiting->reserve : room;
    ta_result result = want == 0u ? TA_OK : ta_index_pool_refill(items, want);
    /* Room for every entry that could be shared without another refill:
     * those in use and those free. */
    uint32_t keys = items->used + items->free;
    if (keys > waiting->table.count) {
        ta_result reserved = ta_table_reserve(&waiting->table, keys - waiting->table.count);
        if (result == TA_OK) {
            result = reserved;
        }
    }
    return result;
}

/* The hash of a destination: a named address's own, or a mix of a runtime
 * ID and its family. */
static uint32_t ta_waiting_hash(const ta_actor_ref *destination) {
    if (destination->kind == TA_ACTOR_REF_NAMED) {
        return destination->as.named->hash;
    }
    return (uint32_t)ta_hash_mix64(destination->as.runtime ^ ((uint64_t)destination->family << 40));
}

typedef struct {
    const ta_waiting *waiting;
    const ta_actor_ref *destination;
} ta_waiting_lookup;

static bool ta_waiting_equal(const void *context, uint32_t value) {
    const ta_waiting_lookup *lookup = context;
    const ta_actor_ref *wanted = lookup->destination;
    const ta_actor_ref *held = &ta_waiting_at(lookup->waiting, value)->destination;
    if (held->kind != wanted->kind) {
        return false;
    }
    if (held->kind == TA_ACTOR_REF_NAMED) {
        return held->as.named == wanted->as.named;
    }
    return held->as.runtime == wanted->as.runtime && held->family == wanted->family;
}

/* Whether mail for `destination` may share an entry: not a runtime ID of
 * 0, which names no one session. */
static bool ta_waiting_shareable(const ta_actor_ref *destination) {
    return destination->kind == TA_ACTOR_REF_NAMED || destination->as.runtime != 0u;
}

ta_result ta_waiting_take(ta_waiting *waiting, const ta_waiting_key *key, uint32_t *out) {
    const ta_actor_ref *destination = key->destination;
    bool shared = ta_waiting_shareable(destination);
    uint32_t hash = ta_waiting_hash(destination);
    if (shared) {
        ta_waiting_lookup lookup = { .waiting = waiting, .destination = destination };
        uint32_t found = 0u;
        if (ta_table_find(&waiting->table,
                          &(ta_table_key){ .hash = hash, .equal = ta_waiting_equal, .context = &lookup },
                          &found) == TA_OK) {
            *out = found;
            return TA_OK;
        }
        if (waiting->table.count >= waiting->table.limit) {
            return TA_ERR_FULL;
        }
    }
    uint32_t index = 0u;
    ta_result result = ta_index_pool_take(&waiting->items, &index);
    if (result != TA_OK) {
        return result;
    }
    if (shared) {
        result = ta_table_insert(&waiting->table, hash, index);
        if (result != TA_OK) {
            ta_index_pool_give(&waiting->items, index);
            return result;
        }
    }
    ta_waiting_entry *entry = ta_waiting_at(waiting, index);
    uint32_t generation = entry->generation;
    *entry = (ta_waiting_entry){ .generation = generation,
                                 .list = TA_LIST_EMPTY,
                                 .destination = *destination,
                                 .hash = hash,
                                 .flags = (uint8_t)(TA_WAITING_LIVE | (shared ? TA_WAITING_SHARED : 0u)),
                                 .hint = key->hint };
    ta_actor_ref_retain(&entry->destination);
    *out = index;
    return TA_OK;
}

static ta_list_links ta_waiting_links(ta_waiting *waiting) {
    return ta_index_pool_links(&waiting->items, offsetof(ta_waiting_entry, next));
}

void ta_waiting_owe(ta_waiting *waiting, uint32_t index) {
    ta_waiting_entry *entry = ta_waiting_at(waiting, index);
    if ((entry->flags & (TA_WAITING_OWED | TA_WAITING_REPORTED | TA_WAITING_RETIRED)) != 0u) {
        return;
    }
    entry->flags |= TA_WAITING_OWED;
    ta_list_links links = ta_waiting_links(waiting);
    ta_list_push_back(&waiting->owed, &links, index);
}

uint32_t ta_waiting_next_owed(ta_waiting *waiting) {
    ta_list_links links = ta_waiting_links(waiting);
    uint32_t index = ta_list_pop_front(&waiting->owed, &links);
    if (index != 0u) {
        ta_waiting_entry *entry = ta_waiting_at(waiting, index);
        entry->flags = (uint8_t)((entry->flags & ~TA_WAITING_OWED) | TA_WAITING_REPORTED);
    }
    return index;
}

uint32_t ta_waiting_find_request(const ta_waiting *waiting, uint64_t token) {
    uint32_t index = (uint32_t)token;
    if (index == 0u || index > waiting->items.high_water) {
        return 0u;
    }
    const ta_waiting_entry *entry = ta_waiting_at(waiting, index);
    if (entry == NULL || (entry->flags & TA_WAITING_LIVE) == 0u || (entry->flags & TA_WAITING_RETIRED) != 0u ||
        (entry->flags & TA_WAITING_REPORTED) == 0u || entry->generation != (uint32_t)(token >> 32)) {
        return 0u;
    }
    return index;
}

/* Takes entry `index` out of the table and the owed list, wherever it is. */
static void ta_waiting_detach(ta_waiting *waiting, uint32_t index) {
    ta_waiting_entry *entry = ta_waiting_at(waiting, index);
    if ((entry->flags & TA_WAITING_SHARED) != 0u) {
        ta_waiting_lookup lookup = { .waiting = waiting, .destination = &entry->destination };
        (void)ta_table_remove(&waiting->table,
                              &(ta_table_key){ .hash = entry->hash, .equal = ta_waiting_equal, .context = &lookup },
                              NULL);
        entry->flags &= (uint8_t)~TA_WAITING_SHARED;
    }
    if ((entry->flags & TA_WAITING_OWED) != 0u) {
        ta_list_links links = ta_waiting_links(waiting);
        (void)ta_list_unlink(&waiting->owed, &links, index);
        entry->flags &= (uint8_t)~TA_WAITING_OWED;
    }
}

void ta_waiting_retire(ta_waiting *waiting, uint32_t index, ta_actor_id settled_to, ta_result failed_code) {
    ta_waiting_detach(waiting, index);
    ta_waiting_entry *entry = ta_waiting_at(waiting, index);
    entry->flags |= TA_WAITING_RETIRED;
    entry->settled_to = settled_to;
    entry->failed_code = failed_code;
}

void ta_waiting_free(ta_waiting *waiting, uint32_t index) {
    ta_waiting_detach(waiting, index);
    ta_waiting_entry *entry = ta_waiting_at(waiting, index);
    ta_actor_ref_release(&entry->destination);
    entry->destination = (ta_actor_ref){ 0 };
    entry->flags = 0u;
    entry->count = 0u;
    entry->list = TA_LIST_EMPTY;
    entry->generation += 1u;
    ta_index_pool_give(&waiting->items, index);
}
