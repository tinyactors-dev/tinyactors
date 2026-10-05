#include "address.h"
#include "hash.h"
#include "system.h"

/* The package comment is in address.h. */

/*
 * A page of the record directory holds 1024 pointers (8 KiB), and a full page
 * of the index 4096 slots (32 KiB), so the largest block the table asks for
 * stays flat as names are added: at four million names the pool's page
 * directory is 64 KiB, as the actor pool's is. The index starts at 64 slots
 * (512 bytes) and ta_table_grow doubles that page to 4096 before it adds one,
 * so a system with a few named actors pays about 9 KiB for its first address
 * and not 64 KiB. The index grows at 7/16 of its slots, which is about 18
 * bytes of slots per name.
 */
#define TA_ADDRESS_PAGE_ITEMS 1024u
#define TA_ADDRESS_FIRST_SLOTS 64u
#define TA_ADDRESS_PAGE_SLOTS 4096u
#define TA_ADDRESS_GROW_SIXTEENTHS 7u

/* The free-list link lives in the low half of the pointer, which means nothing while the item is free. */
static const ta_pool_params TA_ADDRESS_POOL = {
    .item_size = sizeof(ta_address *),
    .item_alignment = _Alignof(ta_address *),
    .link_offset = 0,
    .page_items = TA_ADDRESS_PAGE_ITEMS,
};

static const ta_table_params TA_ADDRESS_INDEX = {
    .mode = TA_TABLE_GROWING,
    .page_slots = TA_ADDRESS_PAGE_SLOTS,
    .first_slots = TA_ADDRESS_FIRST_SLOTS,
    .grow_sixteenths = TA_ADDRESS_GROW_SIXTEENTHS,
};

/* The hash chains family, a NUL separator and name through one running FNV-1a
 * state, so ("a", "bc") and ("ab", "c") differ. */
static uint32_t ta_address_hash(const ta_actor_address *address) {
    const char separator = 0;
    uint32_t hash = ta_fnv1a32(TA_FNV1A32_BASIS, address->family.name.ptr, address->family.name.len);
    hash = ta_fnv1a32(hash, &separator, 1);
    return ta_fnv1a32(hash, address->name.ptr, address->name.len);
}

/* ta_address_record_hash: see address.h. */
uint32_t ta_address_record_hash(const ta_address *record) {
    return ta_fnv1a32(TA_FNV1A32_BASIS, record->text, (size_t)record->family_len + 1u + record->name_len);
}

ta_result ta_address_record_bytes(uint32_t family_len, uint32_t name_len, size_t *bytes) {
    ta_layout layout;
    ta_layout_init(&layout, 0);
    ta_layout_add(&layout, &(ta_layout_entry){ .size = offsetof(ta_address, text), .alignment = _Alignof(ta_address), .count = 1u });
    ta_layout_add(&layout, &(ta_layout_entry){ .size = 1u, .alignment = 1u, .count = family_len });
    ta_layout_add(&layout, &(ta_layout_entry){ .size = 1u, .alignment = 1u, .count = 2u });
    ta_layout_add(&layout, &(ta_layout_entry){ .size = 1u, .alignment = 1u, .count = name_len });
    return ta_layout_finish(&layout, bytes, NULL) == TA_OK ? TA_OK : TA_ERR_NOMEM;
}

size_t ta_address_bytes(const ta_address *address) {
    size_t bytes = 0;
    /* A record that exists was allocated with this size, so the sum fits. */
    (void)ta_address_record_bytes(address->family_len, address->name_len, &bytes);
    return bytes;
}

void ta_address_detached_init(ta_address *copy) {
    copy->table = NULL;
    copy->unloaded = 0;
    copy->references = 0;
    copy->index = 0;
}

void ta_address_table_init(ta_address_table *table, ta_allocator allocator) {
    *table = (ta_address_table){ .allocator = allocator };
    /* The parameters are constants that make a pool; init only validates them. */
    (void)ta_pool_init(&table->records, allocator, &TA_ADDRESS_POOL);
}

static ta_address *ta_address_record_at(const ta_address_table *table, uint32_t index) {
    return *(ta_address **)ta_pool_at(&table->records, index);
}

static void ta_address_free(const ta_address_table *table, ta_address *address) {
    ta_mem_free(table->allocator, address,
                &(ta_allocator_free_params){ .size = ta_address_bytes(address), .alignment = _Alignof(ta_address) });
}

void ta_address_table_destroy(ta_address_table *table) {
    ta_table_cursor cursor = {0};
    uint32_t hash = 0;
    uint32_t index = 0;
    while (ta_table_next(&table->index, &cursor, &hash, &index)) {
        ta_address_free(table, ta_address_record_at(table, index));
    }
    ta_table_destroy(&table->index);
    ta_pool_destroy(&table->records);
    *table = (ta_address_table){0};
}

/* The index only offers records whose hash is equal, so the text is all that is left to compare. */
static bool ta_address_matches(const ta_address *record, const ta_actor_address *address) {
    ta_str family = { .ptr = record->text, .len = record->family_len };
    ta_str name = { .ptr = record->text + record->family_len + 1u, .len = record->name_len };
    return ta_str_equal(family, address->family.name) && ta_str_equal(name, address->name);
}

/* The record of `address`, found through the index, or NULL. */
static ta_address *ta_address_find(const ta_address_table *table, const ta_actor_address *address, uint32_t hash) {
    ta_table_probe probe;
    uint32_t index = 0;
    ta_table_probe_start(&table->index, hash, &probe);
    while (ta_table_probe_next(&probe, &index)) {
        ta_address *record = ta_address_record_at(table, index);
        if (ta_address_matches(record, address)) {
            return record;
        }
    }
    return NULL;
}

/* A new record with one reference's worth of fields filled in; not yet in any table. NULL when memory is out. */
static ta_address *ta_address_record(ta_allocator allocator, const ta_actor_address *address) {
    uint32_t family_len = address->family.name.len;
    uint32_t name_len = address->name.len;
    size_t bytes = 0;
    if (ta_address_record_bytes(family_len, name_len, &bytes) != TA_OK) {
        return NULL;
    }
    ta_address *record = ta_mem_alloc(allocator, bytes, _Alignof(ta_address));
    if (record == NULL) {
        return NULL;
    }
    record->table = NULL;
    record->unloaded = 0;
    record->references = 0;
    record->index = 0;
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

/*
 * Makes room for one more record: a free pool item, the index (created at
 * the first intern), and a free slot in the page `hash` selects, growing the
 * index first when it is due. A failed grow only postpones; a failed prepare,
 * refill or first page is TA_ERR_NOMEM. Nothing is taken from the pool.
 */
static ta_result ta_address_reserve(ta_address_table *table, uint32_t hash) {
    ta_result result = ta_pool_refill(&table->records, 1u);
    if (result == TA_OK && table->index.page_slots == 0) {
        result = ta_table_init(&table->index, table->allocator, &TA_ADDRESS_INDEX);
    }
    if (result != TA_OK) {
        return result;
    }
    if (ta_table_wants_grow(&table->index)) {
        (void)ta_table_grow(&table->index);
    }
    return ta_table_prepare(&table->index, hash);
}

ta_result ta_address_table_intern(ta_address_table *table, const ta_actor_address *address, ta_address **out) {
    uint32_t hash = ta_address_hash(address);
    ta_address *record = ta_address_find(table, address, hash);
    if (record != NULL) {
        record->references += 1u;
        *out = record;
        return TA_OK;
    }
    record = ta_address_record(table->allocator, address);
    if (record == NULL) {
        return TA_ERR_NOMEM;
    }
    if (ta_address_reserve(table, hash) != TA_OK) {
        ta_address_free(table, record);
        return TA_ERR_NOMEM;
    }
    uint32_t index = ta_pool_take(&table->records); /* reserved above */
    *(ta_address **)ta_pool_at(&table->records, index) = record;
    record->table = table;
    record->references = 1;
    record->index = index;
    (void)ta_table_insert(&table->index, hash, index); /* room was prepared, and nothing inserted since */
    *out = record;
    return TA_OK;
}

ta_result ta_address_detach(ta_arena *arena, const ta_address *address, ta_address **out) {
    size_t bytes = ta_address_bytes(address);
    ta_address *copy = ta_arena_alloc(arena, bytes, _Alignof(ta_address));
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, address, bytes);
    ta_address_detached_init(copy);
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
    (void)ta_table_remove(&table->index, ta_address_record_hash(record), record->index);
    ta_pool_give(&table->records, record->index);
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
