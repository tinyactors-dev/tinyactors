#include "table.h"
#include "memory.h"

/* The most keys `slot_count` slots hold: three quarters of them. */
static uint32_t ta_table_load(uint32_t slot_count) {
    return slot_count - slot_count / 4u;
}

/* The fewest slots, a power of two of at least TA_TABLE_MIN_SLOTS, that
 * hold `keys` keys; `keys` is at most TA_TABLE_MAX_KEYS. */
static uint32_t ta_table_slots_for(uint32_t keys) {
    uint32_t slot_count = TA_TABLE_MIN_SLOTS;
    while (ta_table_load(slot_count) < keys) {
        slot_count *= 2u;
    }
    return slot_count;
}

/* Allocates `slot_count` empty slots, or answers NULL. */
static ta_table_slot *ta_table_slots_alloc(ta_allocator allocator, uint32_t slot_count) {
    if ((size_t)slot_count > SIZE_MAX / sizeof(ta_table_slot)) {
        return NULL;
    }
    size_t size = (size_t)slot_count * sizeof(ta_table_slot);
    ta_table_slot *slots = ta_mem_alloc(allocator, size, _Alignof(ta_table_slot));
    if (slots != NULL) {
        ta_mem_zero(slots, size);
    }
    return slots;
}

static void ta_table_slots_free(ta_allocator allocator, ta_table_slot *slots, uint32_t slot_count) {
    ta_mem_free(allocator, slots,
                &(ta_allocator_free_params){ .size = (size_t)slot_count * sizeof(ta_table_slot),
                                             .alignment = _Alignof(ta_table_slot) });
}

/* Puts (hash, value) in the first empty slot from its home; there is one. */
static void ta_table_place(ta_table_slot *slots, uint32_t slot_count, uint32_t hash, uint32_t value) {
    uint32_t mask = slot_count - 1u;
    uint32_t slot = hash & mask;
    while (slots[slot].value != 0) {
        slot = (slot + 1u) & mask;
    }
    slots[slot] = (ta_table_slot){ .hash = hash, .value = value };
}

/* The slot holding `key`, or slot_count when the table does not hold it. */
static uint32_t ta_table_slot_of(const ta_table *table, const ta_table_key *key) {
    if (table->slot_count == 0) {
        return 0;
    }
    uint32_t mask = table->slot_count - 1u;
    for (uint32_t slot = key->hash & mask; table->slots[slot].value != 0; slot = (slot + 1u) & mask) {
        const ta_table_slot *entry = &table->slots[slot];
        if (entry->hash == key->hash && key->equal(key->context, entry->value)) {
            return slot;
        }
    }
    return table->slot_count;
}

ta_result ta_table_init(ta_table *table, const ta_table_params *params) {
    *table = (ta_table){ .allocator = params->allocator, .mode = params->mode };
    if (params->mode != TA_TABLE_FIXED && params->mode != TA_TABLE_GROWING) {
        return TA_ERR_INVALID;
    }
    if (params->mode == TA_TABLE_FIXED && params->capacity == 0) {
        return TA_ERR_INVALID;
    }
    if (params->capacity > TA_TABLE_MAX_KEYS) {
        return TA_ERR_FULL;
    }
    if (params->capacity == 0) {
        return TA_OK;
    }
    uint32_t slot_count = ta_table_slots_for(params->capacity);
    ta_table_slot *slots = ta_table_slots_alloc(params->allocator, slot_count);
    if (slots == NULL) {
        return TA_ERR_NOMEM;
    }
    table->slots = slots;
    table->slot_count = slot_count;
    table->limit = params->mode == TA_TABLE_FIXED ? params->capacity : ta_table_load(slot_count);
    return TA_OK;
}

void ta_table_destroy(ta_table *table) {
    ta_table_slots_free(table->allocator, table->slots, table->slot_count);
    table->slots = NULL;
    table->slot_count = 0;
    table->count = 0;
    table->limit = 0;
}

ta_result ta_table_reserve(ta_table *table, uint32_t more) {
    if (more <= table->limit - table->count) {
        return TA_OK;
    }
    if (table->mode == TA_TABLE_FIXED || more > TA_TABLE_MAX_KEYS - table->count) {
        return TA_ERR_FULL;
    }
    uint32_t slot_count = ta_table_slots_for(table->count + more);
    ta_table_slot *slots = ta_table_slots_alloc(table->allocator, slot_count);
    if (slots == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t slot = 0; slot < table->slot_count; slot += 1u) {
        const ta_table_slot *entry = &table->slots[slot];
        if (entry->value != 0) {
            ta_table_place(slots, slot_count, entry->hash, entry->value);
        }
    }
    ta_table_slots_free(table->allocator, table->slots, table->slot_count);
    table->slots = slots;
    table->slot_count = slot_count;
    table->limit = ta_table_load(slot_count);
    return TA_OK;
}

ta_result ta_table_insert(ta_table *table, uint32_t hash, uint32_t value) {
    if (value == 0) {
        return TA_ERR_INVALID;
    }
    if (table->count >= table->limit) {
        return TA_ERR_FULL;
    }
    ta_table_place(table->slots, table->slot_count, hash, value);
    table->count += 1u;
    return TA_OK;
}

ta_result ta_table_find(const ta_table *table, const ta_table_key *key, uint32_t *value) {
    uint32_t slot = ta_table_slot_of(table, key);
    if (slot == table->slot_count) {
        return TA_ERR_NOT_FOUND;
    }
    *value = table->slots[slot].value;
    return TA_OK;
}

ta_result ta_table_remove(ta_table *table, const ta_table_key *key, uint32_t *value) {
    uint32_t hole = ta_table_slot_of(table, key);
    if (hole == table->slot_count) {
        return TA_ERR_NOT_FOUND;
    }
    if (value != NULL) {
        *value = table->slots[hole].value;
    }
    uint32_t mask = table->slot_count - 1u;
    for (uint32_t next = (hole + 1u) & mask; table->slots[next].value != 0; next = (next + 1u) & mask) {
        uint32_t home = table->slots[next].hash & mask;
        /* The key at `next` may move into the hole when the hole lies
         * between its home and its slot, cyclically. */
        if (((next - home) & mask) >= ((next - hole) & mask)) {
            table->slots[hole] = table->slots[next];
            hole = next;
        }
    }
    table->slots[hole] = (ta_table_slot){0};
    table->count -= 1u;
    return TA_OK;
}
