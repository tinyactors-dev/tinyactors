#include "address.h"
#include "system.h"

static uint32_t ta_address_hash(const ta_actor_address *address) {
    uint32_t hash = ta_hash_bytes(0, address->family.name.ptr, address->family.name.len);
    const char separator = 0;
    hash = ta_hash_bytes(hash, &separator, 1);
    return ta_hash_bytes(hash, address->name.ptr, address->name.len);
}

static uint32_t ta_address_bucket_count(const ta_address_table *table) {
    return table->base + table->split;
}

static ta_address **ta_address_bucket(const ta_address_table *table, uint32_t index) {
    ta_address **page = *(ta_address ***)ta_vec_at(&table->pages, index / TA_ADDRESS_PAGE_BUCKETS);
    return &page[index % TA_ADDRESS_PAGE_BUCKETS];
}

static uint32_t ta_address_index(const ta_address_table *table, uint32_t hash) {
    uint32_t index = hash & (table->base - 1u);
    return index < table->split ? hash & (2u * table->base - 1u) : index;
}

static size_t ta_address_bytes(uint32_t family_len, uint32_t name_len) {
    return sizeof(ta_address) + (size_t)family_len + (size_t)name_len + 2u;
}

/* Makes sure the page holding bucket `index` exists; false when it cannot. */
static bool ta_address_page_for(ta_address_table *table, uint32_t index) {
    uint32_t page_index = index / TA_ADDRESS_PAGE_BUCKETS;
    if (page_index < table->pages.count) {
        return true;
    }
    ta_address **page = ta_mem_alloc(table->allocator, TA_ADDRESS_PAGE_BUCKETS * sizeof(ta_address *),
                                     _Alignof(ta_address *));
    if (page == NULL) {
        return false;
    }
    for (uint32_t bucket = 0; bucket < TA_ADDRESS_PAGE_BUCKETS; bucket += 1u) {
        page[bucket] = NULL;
    }
    if (ta_vec_push(&table->pages, &page) != TA_OK) {
        ta_mem_free(table->allocator, page,
                    &(ta_allocator_free_params){ .size = TA_ADDRESS_PAGE_BUCKETS * sizeof(ta_address *),
                                                 .alignment = _Alignof(ta_address *) });
        return false;
    }
    return true;
}

/* Splits one bucket when the table is fuller than one record per bucket. */
static void ta_address_grow(ta_address_table *table) {
    uint32_t added = ta_address_bucket_count(table);
    if (table->count <= added || added == UINT32_MAX || !ta_address_page_for(table, added)) {
        return; /* a failed page only postpones the split */
    }
    uint32_t wide = 2u * table->base - 1u;
    ta_address **from = ta_address_bucket(table, table->split);
    ta_address **to = ta_address_bucket(table, added);
    ta_address *chain = *from;
    *from = NULL;
    while (chain != NULL) {
        ta_address *next = chain->next;
        ta_address **home = (chain->hash & wide) == table->split ? from : to;
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

void ta_address_table_init(ta_address_table *table, ta_allocator allocator) {
    *table = (ta_address_table){ .allocator = allocator, .base = TA_ADDRESS_INITIAL_BUCKETS };
    ta_vec_init(&table->pages, allocator, TA_VEC_LAYOUT(ta_address **));
}

static void ta_address_free(ta_address_table *table, ta_address *address) {
    ta_mem_free(table->allocator, address,
                &(ta_allocator_free_params){ .size = ta_address_bytes(address->family_len, address->name_len),
                                             .alignment = _Alignof(ta_address) });
}

void ta_address_table_destroy(ta_address_table *table) {
    uint32_t buckets = table->pages.count == 0 ? 0u : ta_address_bucket_count(table);
    for (uint32_t index = 0; index < buckets; index += 1u) {
        ta_address *chain = *ta_address_bucket(table, index);
        while (chain != NULL) {
            ta_address *next = chain->next;
            ta_address_free(table, chain);
            chain = next;
        }
    }
    for (uint32_t page = 0; page < table->pages.count; page += 1u) {
        ta_mem_free(table->allocator, *(ta_address ***)ta_vec_at(&table->pages, page),
                    &(ta_allocator_free_params){ .size = TA_ADDRESS_PAGE_BUCKETS * sizeof(ta_address *),
                                                 .alignment = _Alignof(ta_address *) });
    }
    ta_vec_destroy(&table->pages);
    *table = (ta_address_table){0};
}

static bool ta_address_matches(const ta_address *record, const ta_actor_address *address, uint32_t hash) {
    ta_str family = { .ptr = record->text, .len = record->family_len };
    ta_str name = { .ptr = record->text + record->family_len + 1u, .len = record->name_len };
    return record->hash == hash && ta_str_equal(family, address->family.name) && ta_str_equal(name, address->name);
}

static ta_address *ta_address_record(ta_allocator allocator, const ta_actor_address *address, uint32_t hash) {
    uint32_t family_len = address->family.name.len;
    uint32_t name_len = address->name.len;
    ta_address *record = ta_mem_alloc(allocator, ta_address_bytes(family_len, name_len), _Alignof(ta_address));
    if (record == NULL) {
        return NULL;
    }
    record->table = NULL;
    record->next = NULL;
    record->references = 0;
    record->hash = hash;
    record->family_len = family_len;
    record->name_len = name_len;
    if (family_len != 0) {
        ta_mem_copy(record->text, address->family.name.ptr, family_len);
    }
    record->text[family_len] = 0;
    if (name_len != 0) {
        ta_mem_copy(record->text + family_len + 1u, address->name.ptr, name_len);
    }
    record->text[family_len + 1u + name_len] = 0;
    return record;
}

ta_result ta_address_table_intern(ta_address_table *table, const ta_actor_address *address,
                                  ta_address **out) {
    uint32_t hash = ta_address_hash(address);
    if (table->pages.count != 0) {
        for (ta_address *record = *ta_address_bucket(table, ta_address_index(table, hash)); record != NULL;
             record = record->next) {
            if (ta_address_matches(record, address, hash)) {
                record->references += 1u;
                *out = record;
                return TA_OK;
            }
        }
    } else if (!ta_address_page_for(table, 0)) {
        return TA_ERR_NOMEM;
    }
    ta_address *record = ta_address_record(table->allocator, address, hash);
    if (record == NULL) {
        return TA_ERR_NOMEM;
    }
    record->table = table;
    record->references = 1;
    ta_address **bucket = ta_address_bucket(table, ta_address_index(table, hash));
    record->next = *bucket;
    *bucket = record;
    table->count += 1u;
    ta_address_grow(table);
    *out = record;
    return TA_OK;
}

ta_result ta_address_detach(ta_arena *arena, const ta_address *address, ta_address **out) {
    size_t bytes = ta_address_bytes(address->family_len, address->name_len);
    ta_address *copy = ta_arena_alloc(arena, bytes, _Alignof(ta_address));
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, address, bytes);
    copy->table = NULL;
    copy->next = NULL;
    copy->references = 0;
    *out = copy;
    return TA_OK;
}

bool ta_address_in(const ta_address *address, const ta_address_table *table) {
    return address != NULL && address->table == table;
}

/* ------------------------------------------------------------------------
 * Public
 */

ta_result ta_address_intern(ta_system *system, const ta_actor_address *address, ta_address **out) {
    if (system == NULL || address == NULL || out == NULL ||
        (address->family.name.ptr == NULL && address->family.name.len != 0) ||
        (address->name.ptr == NULL && address->name.len != 0)) {
        return TA_ERR_INVALID;
    }
    return ta_address_table_intern(&system->addresses, address, out);
}

void ta_address_retain(ta_address *address) {
    if (address != NULL && address->table != NULL) {
        address->references += 1u;
    }
}

void ta_address_release(ta_address *address) {
    if (address == NULL || address->table == NULL) {
        return;
    }
    ta_address *record = address;
    record->references -= 1u;
    if (record->references != 0) {
        return;
    }
    ta_address_table *table = record->table;
    for (ta_address **link = ta_address_bucket(table, ta_address_index(table, record->hash)); *link != NULL;
         link = &(*link)->next) {
        if (*link == record) {
            *link = record->next;
            break;
        }
    }
    table->count -= 1u;
    ta_address_free(table, record);
}

ta_actor_address ta_address_view(const ta_address *address) {
    if (address == NULL) {
        return (ta_actor_address){0};
    }
    return (ta_actor_address){
        .family = { .name = { .ptr = address->text, .len = address->family_len } },
        .name = { .ptr = address->text + address->family_len + 1u, .len = address->name_len },
    };
}
