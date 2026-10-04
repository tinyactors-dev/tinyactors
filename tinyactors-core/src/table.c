#include "table.h"

/* The package comment is in table.h. */

/* The largest `base` of a growing table: pages stay below 2^31, so every page
 * index, and 2 * base, fits in a u32. */
#define TA_TABLE_MAX_BASE (1u << 30)
/* A page of at most 2^20 slots keeps every product below in 64 bits. */
#define TA_TABLE_MAX_PAGE_SLOTS (1u << 20)
#define TA_TABLE_MIN_PAGE_SLOTS 16u
/* The most slots a page may be doubled to by prepare: 2^31, so a count and its double fit a u32. */
#define TA_TABLE_MAX_PAGE_SLOTS_TOTAL (1u << 31)
/* wants_grow triggers above this many sixteenths of the slots: 14/16 is the highest trigger that leaves headroom. */
#define TA_TABLE_MAX_GROW_SIXTEENTHS 14u

/* One page as the helpers see it: its slots, and how many there are. */
typedef struct ta_table_page {
    ta_table_slot *slots;
    uint32_t count;
} ta_table_page;

/*
 * The bytes of a page of `slots` slots, recomputed with a checked product
 * wherever a page is allocated or freed: a page's size is always its own
 * recorded slot count times the slot size, so the sized free matches the
 * allocation even when prepare has made pages of different sizes.
 */
static bool ta_table_page_bytes(uint32_t slots, size_t *bytes) {
    return ta_checked_mul(slots, sizeof(ta_table_slot), bytes);
}

/*
 * ta_table_home is the slot a hash starts probing at in a page of `slots`
 * slots. The multiplicative mix takes its answer from the top bits, which
 * depend on every bit of the hash, while the page was chosen by the low
 * bits: all entries of one page share those, and still spread evenly.
 * Multiplying by the slot count and shifting maps onto any count, a power
 * of two or not (FIXED pages are not).
 */
static uint32_t ta_table_home(uint32_t hash, uint32_t slots) {
    return (uint32_t)(((uint64_t)(hash * 0x9E3779B1u) * slots) >> 32);
}

static uint32_t ta_table_after(uint32_t position, uint32_t slots) {
    return position + 1u == slots ? 0u : position + 1u;
}

static void ta_table_page_clear(const ta_table_page *page) {
    for (uint32_t index = 0; index < page->count; index += 1u) {
        page->slots[index] = (ta_table_slot){ .hash = 0, .value = TA_TABLE_NONE };
    }
}

/* Puts `entry` in the first empty slot from its home; false when the page is full. */
static bool ta_table_page_insert(const ta_table_page *page, const ta_table_slot *entry) {
    uint32_t position = ta_table_home(entry->hash, page->count);
    for (uint32_t step = 0; step < page->count; step += 1u) {
        if (page->slots[position].value == TA_TABLE_NONE) {
            page->slots[position] = *entry;
            return true;
        }
        position = ta_table_after(position, page->count);
    }
    return false;
}

/* The position of exactly (hash, value), or TA_TABLE_NONE. */
static uint32_t ta_table_page_find(const ta_table_page *page, const ta_table_slot *entry) {
    uint32_t position = ta_table_home(entry->hash, page->count);
    for (uint32_t step = 0; step < page->count; step += 1u) {
        const ta_table_slot *slot = &page->slots[position];
        if (slot->value == TA_TABLE_NONE) {
            break;
        }
        if (slot->hash == entry->hash && slot->value == entry->value) {
            return position;
        }
        position = ta_table_after(position, page->count);
    }
    return TA_TABLE_NONE;
}

/*
 * Empties the slot at `hole` and closes the gap (Knuth, algorithm R): each
 * later entry of the run moves back into the hole unless its home lies
 * cyclically after the hole and not after the entry, in which case it
 * would end up before its home and be unreachable. The walk is bounded by
 * the page size so that a page without an empty slot ends too.
 */
static void ta_table_page_erase(const ta_table_page *page, uint32_t hole) {
    uint32_t next = hole;
    for (uint32_t step = 1u; step < page->count; step += 1u) {
        next = ta_table_after(next, page->count);
        if (page->slots[next].value == TA_TABLE_NONE) {
            break;
        }
        uint32_t home = ta_table_home(page->slots[next].hash, page->count);
        bool stays = hole <= next ? (hole < home && home <= next) : (hole < home || home <= next);
        if (!stays) {
            page->slots[hole] = page->slots[next];
            hole = next;
        }
    }
    page->slots[hole] = (ta_table_slot){ .hash = 0, .value = TA_TABLE_NONE };
}

static ta_table_page_entry *ta_table_entry_at(const ta_table *table, uint32_t index) {
    return ta_vec_at(&table->pages, index);
}

/* Linear hashing: pages before `split` are selected by one more bit. */
static uint32_t ta_table_page_index(const ta_table *table, uint32_t hash) {
    uint32_t index = hash & (table->base - 1u);
    return index < table->split ? hash & (2u * table->base - 1u) : index;
}

static ta_table_page_entry *ta_table_entry_of(const ta_table *table, uint32_t hash) {
    return ta_table_entry_at(table, ta_table_page_index(table, hash));
}

static ta_table_page ta_table_view(const ta_table_page_entry *entry) {
    return (ta_table_page){ .slots = entry->slots, .count = entry->count };
}

/* Frees a page of `slots` slots with the size it was allocated with. */
static void ta_table_page_free(const ta_table *table, ta_table_slot *slots, uint32_t count) {
    size_t bytes = 0;
    (void)ta_table_page_bytes(count, &bytes); /* it fitted when the page was allocated */
    ta_mem_free(table->allocator, slots, &(ta_allocator_free_params){ .size = bytes, .alignment = _Alignof(ta_table_slot) });
}

/* Allocates one cleared page of `count` slots; NULL when the size does not fit or memory is out. */
static ta_table_slot *ta_table_page_alloc(const ta_table *table, uint32_t count) {
    size_t bytes = 0;
    if (!ta_table_page_bytes(count, &bytes)) {
        return NULL;
    }
    ta_table_slot *slots = ta_mem_alloc(table->allocator, bytes, _Alignof(ta_table_slot));
    if (slots != NULL) {
        ta_table_page_clear(&(ta_table_page){ .slots = slots, .count = count });
    }
    return slots;
}

/* Does the work of ta_table_init; the caller zeroes *table when this fails. */
static ta_result ta_table_setup(ta_table *table, ta_allocator allocator, const ta_table_params *params) {
    *table = (ta_table){ .allocator = allocator, .mode = params->mode };
    uint64_t slots = 0;
    if (params->mode == TA_TABLE_FIXED) {
        if (params->max_entries == 0) {
            return TA_ERR_INVALID;
        }
        /* ceil(max * 4 / 3) + 1: never above three quarters full, always one empty slot. */
        slots = ((uint64_t)params->max_entries * 4u + 2u) / 3u + 1u;
        if (slots >= TA_TABLE_NONE) {
            return TA_ERR_NOMEM;
        }
        table->max_entries = params->max_entries;
    } else if (params->mode == TA_TABLE_GROWING) {
        if (params->page_slots < TA_TABLE_MIN_PAGE_SLOTS || params->page_slots > TA_TABLE_MAX_PAGE_SLOTS ||
            params->grow_sixteenths == 0 || params->grow_sixteenths > TA_TABLE_MAX_GROW_SIXTEENTHS) {
            return TA_ERR_INVALID;
        }
        slots = params->page_slots;
        table->grow_sixteenths = params->grow_sixteenths;
    } else {
        return TA_ERR_INVALID;
    }
    size_t bytes = 0;
    if (!ta_table_page_bytes((uint32_t)slots, &bytes)) {
        return TA_ERR_NOMEM;
    }
    table->page_slots = (uint32_t)slots;
    table->base = 1u;
    ta_vec_init(&table->pages, allocator, TA_VEC_LAYOUT(ta_table_page_entry));
    ta_table_slot *first = NULL;
    if (ta_vec_reserve(&table->pages, 1u) == TA_OK) {
        first = ta_table_page_alloc(table, table->page_slots);
    }
    if (first == NULL) {
        ta_vec_destroy(&table->pages);
        return TA_ERR_NOMEM;
    }
    (void)ta_vec_push(&table->pages, &(ta_table_page_entry){ .slots = first, .count = table->page_slots }); /* reserved above */
    table->slots = table->page_slots;
    return TA_OK;
}

ta_result ta_table_init(ta_table *table, ta_allocator allocator, const ta_table_params *params) {
    ta_result result = ta_table_setup(table, allocator, params);
    if (result != TA_OK) {
        *table = (ta_table){0};
    }
    return result;
}

void ta_table_destroy(ta_table *table) {
    for (uint32_t index = 0; index < table->pages.count; index += 1u) {
        const ta_table_page_entry *entry = ta_table_entry_at(table, index);
        ta_table_page_free(table, entry->slots, entry->count);
    }
    ta_vec_destroy(&table->pages);
    *table = (ta_table){0};
}

ta_result ta_table_insert(ta_table *table, uint32_t hash, uint32_t value) {
    if (value == TA_TABLE_NONE || table->pages.count == 0) {
        return TA_ERR_INVALID;
    }
    if (table->mode == TA_TABLE_FIXED && table->count >= table->max_entries) {
        return TA_ERR_NOMEM;
    }
    ta_table_slot entry = { .hash = hash, .value = value };
    ta_table_page_entry *home = ta_table_entry_of(table, hash);
    ta_table_page page = ta_table_view(home);
    if (!ta_table_page_insert(&page, &entry)) {
        return TA_ERR_NOMEM;
    }
    home->used += 1u;
    table->count += 1u;
    return TA_OK;
}

bool ta_table_remove(ta_table *table, uint32_t hash, uint32_t value) {
    if (table->pages.count == 0) {
        return false;
    }
    ta_table_slot entry = { .hash = hash, .value = value };
    ta_table_page_entry *home = ta_table_entry_of(table, hash);
    ta_table_page page = ta_table_view(home);
    uint32_t position = ta_table_page_find(&page, &entry);
    if (position == TA_TABLE_NONE) {
        return false;
    }
    ta_table_page_erase(&page, position);
    home->used -= 1u;
    table->count -= 1u;
    return true;
}

ta_result ta_table_prepare(ta_table *table, uint32_t hash) {
    if (table->pages.count == 0) {
        return TA_ERR_INVALID;
    }
    if (table->mode == TA_TABLE_FIXED) {
        return table->count >= table->max_entries ? TA_ERR_NOMEM : TA_OK;
    }
    ta_table_page_entry *home = ta_table_entry_of(table, hash);
    if (home->used < home->count) {
        return TA_OK;
    }
    if (home->count > TA_TABLE_MAX_PAGE_SLOTS_TOTAL / 2u) {
        return TA_ERR_NOMEM;
    }
    uint32_t doubled = home->count * 2u;
    ta_table_slot *slots = ta_table_page_alloc(table, doubled);
    if (slots == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_table_page to = { .slots = slots, .count = doubled };
    for (uint32_t index = 0; index < home->count; index += 1u) {
        (void)ta_table_page_insert(&to, &home->slots[index]); /* twice the slots of a page whose every slot is full */
    }
    ta_table_page_free(table, home->slots, home->count);
    table->slots += home->count;
    home->slots = slots;
    home->count = doubled;
    return TA_OK;
}

void ta_table_probe_start(const ta_table *table, uint32_t hash, ta_table_probe *probe) {
    if (table->pages.count == 0) {
        *probe = (ta_table_probe){0};
        return;
    }
    ta_table_page page = ta_table_view(ta_table_entry_of(table, hash));
    *probe = (ta_table_probe){
        .page = page.slots,
        .slots = page.count,
        .hash = hash,
        .position = ta_table_home(hash, page.count),
        .left = page.count,
    };
}

bool ta_table_probe_next(ta_table_probe *probe, uint32_t *value) {
    while (probe->left > 0) {
        const ta_table_slot *slot = &probe->page[probe->position];
        if (slot->value == TA_TABLE_NONE) {
            probe->left = 0;
            return false;
        }
        probe->position = ta_table_after(probe->position, probe->slots);
        probe->left -= 1u;
        if (slot->hash == probe->hash) {
            *value = slot->value;
            return true;
        }
    }
    return false;
}

bool ta_table_next(const ta_table *table, ta_table_cursor *cursor, uint32_t *hash, uint32_t *value) {
    while (cursor->page < table->pages.count) {
        const ta_table_page_entry *entry = ta_table_entry_at(table, cursor->page);
        while (cursor->position < entry->count) {
            const ta_table_slot *slot = &entry->slots[cursor->position];
            cursor->position += 1u;
            if (slot->value != TA_TABLE_NONE) {
                *hash = slot->hash;
                *value = slot->value;
                return true;
            }
        }
        cursor->page += 1u;
        cursor->position = 0;
    }
    return false;
}

bool ta_table_wants_grow(const ta_table *table) {
    if (table->mode != TA_TABLE_GROWING) {
        return false;
    }
    /* Both sides fit in 64 bits: slots < 2^50 and grow_sixteenths <= 14, count < 2^32. */
    return (uint64_t)table->count * 16u > table->slots * table->grow_sixteenths;
}

/*
 * Moves the entries of page `table->split` whose hash selects the new page
 * into it, in place, and answers how many moved. An entry that moves is
 * erased (which may pull a later entry of the run into its slot, so the slot
 * is looked at again) and inserted into the new page, which holds as many
 * slots as the old one and so cannot overflow.
 */
static uint32_t ta_table_split_page(const ta_table *table, const ta_table_page *from, const ta_table_page *to) {
    uint32_t wide = 2u * table->base - 1u;
    uint32_t moves = 0;
    for (uint32_t index = 0; index < from->count; index += 1u) {
        while (from->slots[index].value != TA_TABLE_NONE && (from->slots[index].hash & wide) != table->split) {
            ta_table_slot moved = from->slots[index];
            ta_table_page_erase(from, index);
            (void)ta_table_page_insert(to, &moved);
            moves += 1u;
        }
    }
    return moves;
}

ta_result ta_table_grow(ta_table *table) {
    if (table->mode != TA_TABLE_GROWING) {
        return TA_ERR_INVALID;
    }
    if (table->base >= TA_TABLE_MAX_BASE) {
        return TA_ERR_NOMEM;
    }
    ta_result result = ta_vec_reserve(&table->pages, table->base + table->split + 1u);
    if (result != TA_OK) {
        return result;
    }
    ta_table_page_entry *old = ta_table_entry_at(table, table->split);
    ta_table_slot *added = ta_table_page_alloc(table, old->count);
    if (added == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_table_page from = ta_table_view(old);
    ta_table_page to = { .slots = added, .count = old->count };
    uint32_t moved = ta_table_split_page(table, &from, &to);
    old->used -= moved;
    table->slots += to.count;
    /* reserved above; it becomes page base + split. `old` is dead after the push, which may not move but is not ours to assume. */
    (void)ta_vec_push(&table->pages, &(ta_table_page_entry){ .slots = added, .count = to.count, .used = moved });
    table->split += 1u;
    if (table->split == table->base) {
        table->base *= 2u;
        table->split = 0;
    }
    return TA_OK;
}

uint32_t ta_table_count(const ta_table *table) {
    return table->count;
}

size_t ta_table_bytes(const ta_table *table) {
    return (size_t)(table->slots * sizeof(ta_table_slot)) + ta_vec_bytes(&table->pages);
}
