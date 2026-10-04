/*
 * ta_heap: the indexed min-heap (src/heap.h) that is the deadline index
 * over envelopes, keyed by (due, send order).
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Items live in small pages that
 * are not one array, as an index pool's do, and carry a due time, an
 * order and a position field among other fields. Memory for the heap
 * comes from a counting allocator local to this test, which can fail
 * from a given allocation on.
 *
 * Claims proven here, in order:
 *
 * 1. init refuses a page shift above 31 with TA_ERR_INVALID and leaves
 *    the heap untouched; a valid init allocates nothing and gives an empty
 *    heap of capacity 0, whose head and pop answer 0, whose remove answers
 *    TA_ERR_NOT_FOUND, and whose shrink and destroy free nothing;
 * 2. grow rounds up to whole pages, a grow within the capacity allocates
 *    nothing, and a grow that fails at any of its allocations answers
 *    TA_ERR_NOMEM and changes nothing: the same capacity, items, positions
 *    and position fields, and no block leaked;
 * 3. one item: insert puts it at position 0 and records that in its
 *    position field, head answers it and leaves it there, and pop answers
 *    it and empties the heap;
 * 4. pop answers items in (due, order) order, whatever order they were
 *    inserted in: the smaller due first, and for the same due the smaller
 *    order; after every insert and pop the heap property holds and every
 *    item's position field names its position;
 * 5. remove takes out the head, the last position, an item whose
 *    replacement sinks and one whose replacement rises, keeping the heap
 *    property and the position fields; it answers TA_ERR_NOT_FOUND and
 *    changes nothing for 0, for an item never inserted (with a position
 *    field naming another item's position, or one at or above the
 *    length) and for an item already removed;
 * 6. the heap writes only the position field of an item, and only while
 *    the item is in it: due, order and the item's other bytes stay as they
 *    were, and a field the owner writes after pop or remove (an arrival
 *    number) is left alone by later operations;
 * 7. the heap keeps no pointer into the items' storage: moving the items
 *    to new memory between calls leaves the heap intact;
 * 8. insert, pop, remove, head, length and capacity never allocate;
 * 9. shrink frees the pages wholly above the larger of keep and the
 *    length, keeps the items in the heap, and with an empty heap and keep
 *    0 frees every page; the heap then grows again;
 * 10. destroy frees every page and leaves an empty heap of capacity 0;
 * 11. a random mix of inserts, pops and removes over 64 items agrees with
 *    a plain model at every step;
 * 12. an insert into a full heap answers TA_ERR_FULL and changes nothing:
 *    the length, the order of the items and the new item's position field;
 *    after a pop there is room again.
 */
#include "../include/tinyactors.c"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

/* ------------------------------------------------------------------------
 * An allocator that counts and can fail from a given attempt on.
 */

#define MAX_BLOCKS 256u

typedef struct {
    void *ptrs[MAX_BLOCKS];
    size_t sizes[MAX_BLOCKS];
    uint32_t live;
    uint32_t attempts;
    /* The attempt, counting from 1, from which every allocation fails; 0
     * never fails. */
    uint32_t fail_from;
} counting_allocator;

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    counting_allocator *heap = self;
    heap->attempts += 1u;
    if ((heap->fail_from != 0 && heap->attempts >= heap->fail_from) || heap->live == MAX_BLOCKS) {
        return NULL;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size_t rounded = (size + alignment - 1u) / alignment * alignment;
    void *ptr = aligned_alloc(alignment, rounded);
    if (ptr != NULL) {
        memset(ptr, 0xA5, rounded);
        heap->sizes[heap->live] = size;
        heap->ptrs[heap->live++] = ptr;
    }
    return ptr;
}

static void counting_free(void *self, const ta_allocator_free_params *params) {
    counting_allocator *heap = self;
    for (uint32_t index = 0; index < heap->live; index += 1u) {
        if (heap->ptrs[index] == params->ptr) {
            if (heap->sizes[index] != params->size) {
                fprintf(stderr, "free of %zu bytes, allocated as %zu\n", params->size, heap->sizes[index]);
                failures += 1;
            }
            heap->live -= 1u;
            heap->ptrs[index] = heap->ptrs[heap->live];
            heap->sizes[index] = heap->sizes[heap->live];
            free(params->ptr);
            return;
        }
    }
    fprintf(stderr, "free of %p, which this allocator never handed out\n", params->ptr);
    failures += 1;
}

static const ta_allocator_vtable counting_vtable = { .alloc = counting_alloc, .free = counting_free };

static ta_allocator allocator_of(counting_allocator *heap) {
    return (ta_allocator){ .self = heap, .v = &counting_vtable };
}

/* ------------------------------------------------------------------------
 * Items in pages of eight, with a directory of page pointers, so items
 * are not contiguous. Index 0 is never an item.
 */

typedef struct {
    uint32_t next;
    uint32_t heap_index;
    uint64_t due;
    uint64_t order;
    uint64_t payload;
} item;

#define PAGE_ITEMS 8u
#define PAGES 8u
#define ITEMS (PAGE_ITEMS * PAGES)

typedef struct {
    item *pages[PAGES];
} storage;

static void *item_at(void *items, uint32_t index) {
    storage *s = items;
    if (index == 0u || index > ITEMS) {
        fprintf(stderr, "item_at: bad index %u\n", index);
        abort();
    }
    uint32_t at = index - 1u;
    return &s->pages[at / PAGE_ITEMS][at % PAGE_ITEMS];
}

static item *item_of(storage *s, uint32_t index) { return item_at(s, index); }

static void storage_init(storage *s) {
    for (uint32_t p = 0; p < PAGES; p++) {
        s->pages[p] = calloc(PAGE_ITEMS, sizeof(item));
        for (uint32_t i = 0; i < PAGE_ITEMS; i++) {
            uint32_t index = p * PAGE_ITEMS + i + 1u;
            /* Garbage positions: an insert must not depend on a clean field. */
            s->pages[p][i] = (item){ .next = 0xdeadbeefu, .heap_index = 0xdeadbeefu, .payload = 0xFACE0000u + index };
        }
    }
}

/* Moves every page to new memory and poisons the old: a pool that grew. */
static void storage_move(storage *s) {
    for (uint32_t p = 0; p < PAGES; p++) {
        item *moved = malloc(PAGE_ITEMS * sizeof(item));
        memcpy(moved, s->pages[p], PAGE_ITEMS * sizeof(item));
        memset(s->pages[p], 0xa5, PAGE_ITEMS * sizeof(item));
        free(s->pages[p]);
        s->pages[p] = moved;
    }
}

static void storage_destroy(storage *s) {
    for (uint32_t p = 0; p < PAGES; p++) {
        free(s->pages[p]);
    }
}

static ta_heap_items items_of(storage *s) {
    return (ta_heap_items){
        .item = item_at,
        .items = s,
        .due_offset = offsetof(item, due),
        .order_offset = offsetof(item, order),
        .position_offset = offsetof(item, heap_index),
    };
}

static void set_key(storage *s, uint32_t index, uint64_t due, uint64_t order) {
    item_of(s, index)->due = due;
    item_of(s, index)->order = order;
}

static bool key_less(const item *left, const item *right) {
    return left->due < right->due || (left->due == right->due && left->order < right->order);
}

static uint32_t at_position(const ta_heap *heap, uint32_t position) {
    uint32_t value;
    ta_paged_array_get(&heap->positions, position, &value);
    return value;
}

/* Whether the heap property holds and every item's position field names
 * its position. */
static bool well_formed(const ta_heap *heap, storage *s) {
    for (uint32_t position = 0; position < heap->length; position += 1u) {
        uint32_t index = at_position(heap, position);
        if (index == 0u || index > ITEMS || item_of(s, index)->heap_index != position) {
            return false;
        }
        if (position > 0u && key_less(item_of(s, index), item_of(s, at_position(heap, (position - 1u) / 2u)))) {
            return false;
        }
    }
    return true;
}

/* A heap over the test's allocator with pages of four positions. */
static void heap_setup(ta_heap *heap, counting_allocator *memory) {
    CHECK(ta_heap_init(heap, &(ta_heap_params){ .allocator = allocator_of(memory), .page_shift = 2 }) == TA_OK);
}

/* 1 */
static void test_init(void) {
    counting_allocator memory = {0};
    storage s;
    storage_init(&s);
    ta_heap_items items = items_of(&s);

    ta_heap heap;
    memset(&heap, 0x5A, sizeof(heap));
    ta_heap before;
    memcpy(&before, &heap, sizeof(heap));
    CHECK(ta_heap_init(&heap, &(ta_heap_params){ .allocator = allocator_of(&memory), .page_shift = 32 }) ==
          TA_ERR_INVALID);
    CHECK(memcmp(&heap, &before, sizeof(heap)) == 0);

    heap_setup(&heap, &memory);
    CHECK(memory.attempts == 0);
    CHECK(ta_heap_length(&heap) == 0 && ta_heap_capacity(&heap) == 0);
    CHECK(ta_heap_head(&heap) == 0);
    CHECK(ta_heap_pop(&heap, &items) == 0);
    CHECK(ta_heap_remove(&heap, &items, 0) == TA_ERR_NOT_FOUND);
    item_of(&s, 3)->heap_index = 0;
    CHECK(ta_heap_remove(&heap, &items, 3) == TA_ERR_NOT_FOUND);
    CHECK(ta_heap_length(&heap) == 0);
    ta_heap_shrink(&heap, 0);
    ta_heap_destroy(&heap);
    CHECK(memory.attempts == 0 && memory.live == 0);

    /* The widest page shift is accepted. */
    CHECK(ta_heap_init(&heap, &(ta_heap_params){ .allocator = allocator_of(&memory), .page_shift = 31 }) == TA_OK);
    CHECK(memory.attempts == 0);
    storage_destroy(&s);
}

/* 2 */
static void test_grow(void) {
    counting_allocator memory = {0};
    ta_heap heap;
    heap_setup(&heap, &memory);
    CHECK(ta_heap_grow(&heap, 0) == TA_OK && memory.attempts == 0);
    CHECK(ta_heap_grow(&heap, 1) == TA_OK && ta_heap_capacity(&heap) == 4);
    uint32_t attempts = memory.attempts;
    CHECK(ta_heap_grow(&heap, 4) == TA_OK && ta_heap_grow(&heap, 2) == TA_OK);
    CHECK(memory.attempts == attempts && ta_heap_capacity(&heap) == 4);
    CHECK(ta_heap_grow(&heap, 5) == TA_OK && ta_heap_capacity(&heap) == 8);
    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);

    /* A heap of 6 items grown to 40 positions, failing each allocation in
     * turn. */
    for (uint32_t failing = 1;; failing += 1u) {
        counting_allocator failing_memory = {0};
        storage s;
        storage_init(&s);
        ta_heap_items items = items_of(&s);
        heap_setup(&heap, &failing_memory);
        CHECK(ta_heap_grow(&heap, 6) == TA_OK);
        for (uint32_t index = 1; index <= 6; index += 1u) {
            set_key(&s, index, 100u - index, index);
            ta_heap_insert(&heap, &items, index);
        }
        ta_heap before;
        memcpy(&before, &heap, sizeof(heap));
        uint32_t order[6];
        uint32_t fields[6];
        for (uint32_t position = 0; position < 6; position += 1u) {
            order[position] = at_position(&heap, position);
            fields[position] = item_of(&s, position + 1u)->heap_index;
        }
        uint32_t live = failing_memory.live;

        failing_memory.fail_from = failing_memory.attempts + failing;
        ta_result result = ta_heap_grow(&heap, 40);
        failing_memory.fail_from = 0;
        if (result == TA_OK) {
            CHECK(failing > 1u);
            CHECK(ta_heap_capacity(&heap) == 40 && ta_heap_length(&heap) == 6);
            CHECK(well_formed(&heap, &s));
            ta_heap_destroy(&heap);
            CHECK(failing_memory.live == 0);
            storage_destroy(&s);
            break;
        }
        CHECK(result == TA_ERR_NOMEM);
        CHECK(memcmp(&heap, &before, sizeof(heap)) == 0);
        CHECK(failing_memory.live == live);
        for (uint32_t position = 0; position < 6; position += 1u) {
            CHECK(at_position(&heap, position) == order[position]);
            CHECK(item_of(&s, position + 1u)->heap_index == fields[position]);
        }
        CHECK(well_formed(&heap, &s));
        ta_heap_destroy(&heap);
        CHECK(failing_memory.live == 0);
        storage_destroy(&s);
    }
}

/* 3 */
static void test_one_item(void) {
    counting_allocator memory = {0};
    storage s;
    storage_init(&s);
    ta_heap_items items = items_of(&s);
    ta_heap heap;
    heap_setup(&heap, &memory);
    CHECK(ta_heap_grow(&heap, 1) == TA_OK);

    set_key(&s, 5, 42, 7);
    ta_heap_insert(&heap, &items, 5);
    CHECK(ta_heap_length(&heap) == 1);
    CHECK(item_of(&s, 5)->heap_index == 0);
    CHECK(ta_heap_head(&heap) == 5 && ta_heap_head(&heap) == 5);
    CHECK(ta_heap_length(&heap) == 1);
    CHECK(ta_heap_pop(&heap, &items) == 5);
    CHECK(ta_heap_length(&heap) == 0 && ta_heap_head(&heap) == 0);
    CHECK(ta_heap_pop(&heap, &items) == 0);

    /* Remove of the only item empties the heap too. */
    ta_heap_insert(&heap, &items, 5);
    CHECK(ta_heap_remove(&heap, &items, 5) == TA_OK);
    CHECK(ta_heap_length(&heap) == 0 && ta_heap_head(&heap) == 0);

    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);
    storage_destroy(&s);
}

/* 4 */
static void test_order(void) {
    counting_allocator memory = {0};
    storage s;
    storage_init(&s);
    ta_heap_items items = items_of(&s);
    ta_heap heap;
    heap_setup(&heap, &memory);
    CHECK(ta_heap_grow(&heap, ITEMS) == TA_OK);

    /* Item i is due at (i * 37) % 11 and ordered by 1000 - i, so many
     * share a due and the order breaks the tie against index order. */
    for (uint32_t index = 1; index <= ITEMS; index += 1u) {
        set_key(&s, index, (index * 37u) % 11u, 1000u - index);
    }
    /* Inserted in a scrambled order. */
    for (uint32_t step = 0; step < ITEMS; step += 1u) {
        uint32_t index = (step * 29u) % ITEMS + 1u;
        ta_heap_insert(&heap, &items, index);
        CHECK(ta_heap_length(&heap) == step + 1u);
        CHECK(well_formed(&heap, &s));
    }
    item previous = { .due = 0, .order = 0 };
    for (uint32_t step = 0; step < ITEMS; step += 1u) {
        uint32_t head = ta_heap_head(&heap);
        uint32_t index = ta_heap_pop(&heap, &items);
        CHECK(index == head && index != 0u);
        if (step > 0u) {
            CHECK(key_less(&previous, item_of(&s, index)));
        }
        previous = *item_of(&s, index);
        CHECK(well_formed(&heap, &s));
    }
    CHECK(ta_heap_length(&heap) == 0 && ta_heap_pop(&heap, &items) == 0);

    /* Equal due: order decides; equal order: due decides. */
    set_key(&s, 1, 10, 3);
    set_key(&s, 2, 10, 2);
    set_key(&s, 3, 9, 50);
    set_key(&s, 4, 10, 1);
    for (uint32_t index = 1; index <= 4; index += 1u) {
        ta_heap_insert(&heap, &items, index);
    }
    CHECK(ta_heap_pop(&heap, &items) == 3);
    CHECK(ta_heap_pop(&heap, &items) == 4);
    CHECK(ta_heap_pop(&heap, &items) == 2);
    CHECK(ta_heap_pop(&heap, &items) == 1);

    /* The largest due and order sort last. */
    set_key(&s, 1, UINT64_MAX, UINT64_MAX);
    set_key(&s, 2, UINT64_MAX, 0);
    set_key(&s, 3, 0, UINT64_MAX);
    for (uint32_t index = 1; index <= 3; index += 1u) {
        ta_heap_insert(&heap, &items, index);
    }
    CHECK(ta_heap_pop(&heap, &items) == 3);
    CHECK(ta_heap_pop(&heap, &items) == 2);
    CHECK(ta_heap_pop(&heap, &items) == 1);

    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);
    storage_destroy(&s);
}

/* Fills the heap with items 1..7 keyed 1, 10, 2, 11, 12, 3, 4, inserted
 * in that order, which lays them out exactly so:
 * positions 0..6 hold the keys [1, 10, 2, 11, 12, 3, 4]. */
static void fill_seven(ta_heap *heap, storage *s, const ta_heap_items *items) {
    static const uint64_t dues[] = { 1, 10, 2, 11, 12, 3, 4 };
    for (uint32_t index = 1; index <= 7; index += 1u) {
        set_key(s, index, dues[index - 1u], 0);
        ta_heap_insert(heap, items, index);
    }
    for (uint32_t position = 0; position < 7; position += 1u) {
        CHECK(at_position(heap, position) == position + 1u);
    }
}

static void drain_in_order(ta_heap *heap, storage *s, const ta_heap_items *items) {
    uint64_t last = 0;
    while (ta_heap_length(heap) > 0u) {
        uint32_t index = ta_heap_pop(heap, items);
        CHECK(item_of(s, index)->due >= last);
        last = item_of(s, index)->due;
        CHECK(well_formed(heap, s));
    }
}

/* 5 */
static void test_remove(void) {
    counting_allocator memory = {0};
    storage s;
    storage_init(&s);
    ta_heap_items items = items_of(&s);
    ta_heap heap;
    heap_setup(&heap, &memory);
    CHECK(ta_heap_grow(&heap, 16) == TA_OK);

    /* The head. */
    fill_seven(&heap, &s, &items);
    CHECK(ta_heap_remove(&heap, &items, 1) == TA_OK);
    CHECK(ta_heap_length(&heap) == 6 && well_formed(&heap, &s));
    CHECK(ta_heap_head(&heap) == 3); /* due 2 */
    drain_in_order(&heap, &s, &items);

    /* The last position: nothing moves. */
    fill_seven(&heap, &s, &items);
    CHECK(ta_heap_remove(&heap, &items, 7) == TA_OK);
    CHECK(ta_heap_length(&heap) == 6 && well_formed(&heap, &s));
    for (uint32_t position = 0; position < 6; position += 1u) {
        CHECK(at_position(&heap, position) == position + 1u);
    }
    drain_in_order(&heap, &s, &items);

    /* Item 2 (due 10, position 1): its replacement, item 7 (due 4), is
     * not smaller than the root and not larger than its new children (11
     * and 12), so it stays at position 1. */
    fill_seven(&heap, &s, &items);
    CHECK(ta_heap_remove(&heap, &items, 2) == TA_OK);
    CHECK(ta_heap_length(&heap) == 6 && well_formed(&heap, &s));
    CHECK(at_position(&heap, 1) == 7 && item_of(&s, 7)->heap_index == 1);
    drain_in_order(&heap, &s, &items);

    /* Sinks: keys [1, 2, 3, 4, 5, 6, 7, 8, 9]; remove due 2 at position 1;
     * the last, due 9, sinks below 4 and then 8. */
    for (uint32_t index = 1; index <= 9; index += 1u) {
        set_key(&s, index, index, 0);
        ta_heap_insert(&heap, &items, index);
    }
    CHECK(ta_heap_remove(&heap, &items, 2) == TA_OK);
    CHECK(well_formed(&heap, &s));
    CHECK(at_position(&heap, 1) == 4 && at_position(&heap, 3) == 8 && at_position(&heap, 7) == 9);
    drain_in_order(&heap, &s, &items);

    /* Rises: remove item 4 (due 11, position 3); the last, due 4 from
     * position 6, is smaller than its new parent, due 10. */
    fill_seven(&heap, &s, &items);
    CHECK(ta_heap_remove(&heap, &items, 4) == TA_OK);
    CHECK(well_formed(&heap, &s));
    CHECK(at_position(&heap, 1) == 7 && at_position(&heap, 3) == 2);
    drain_in_order(&heap, &s, &items);

    /* Not in the heap. */
    fill_seven(&heap, &s, &items);
    ta_heap before;
    memcpy(&before, &heap, sizeof(heap));
    uint32_t fields[8];
    for (uint32_t index = 1; index <= 7; index += 1u) {
        fields[index] = item_of(&s, index)->heap_index;
    }
    CHECK(ta_heap_remove(&heap, &items, 0) == TA_ERR_NOT_FOUND);
    item_of(&s, 20)->heap_index = 3; /* names item 4's position */
    CHECK(ta_heap_remove(&heap, &items, 20) == TA_ERR_NOT_FOUND);
    item_of(&s, 20)->heap_index = 7; /* the length */
    CHECK(ta_heap_remove(&heap, &items, 20) == TA_ERR_NOT_FOUND);
    item_of(&s, 20)->heap_index = 0xdeadbeefu;
    CHECK(ta_heap_remove(&heap, &items, 20) == TA_ERR_NOT_FOUND);
    CHECK(memcmp(&heap, &before, sizeof(heap)) == 0);
    for (uint32_t index = 1; index <= 7; index += 1u) {
        CHECK(item_of(&s, index)->heap_index == fields[index]);
        CHECK(at_position(&heap, index - 1u) == index);
    }
    /* Already removed: its field still names a position, which now holds
     * another item. */
    CHECK(ta_heap_remove(&heap, &items, 2) == TA_OK);
    CHECK(item_of(&s, 2)->heap_index == 1);
    CHECK(ta_heap_remove(&heap, &items, 2) == TA_ERR_NOT_FOUND);
    CHECK(ta_heap_pop(&heap, &items) == 1);
    CHECK(ta_heap_remove(&heap, &items, 1) == TA_ERR_NOT_FOUND);
    CHECK(ta_heap_length(&heap) == 5 && well_formed(&heap, &s));

    /* Removing every item, from the tail, then from the middle. */
    while (ta_heap_length(&heap) > 0u) {
        uint32_t index = at_position(&heap, ta_heap_length(&heap) / 2u);
        CHECK(ta_heap_remove(&heap, &items, index) == TA_OK);
        CHECK(well_formed(&heap, &s));
    }

    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);
    storage_destroy(&s);
}

/* 6 */
static void test_writes_only_position(void) {
    counting_allocator memory = {0};
    storage s;
    storage_init(&s);
    ta_heap_items items = items_of(&s);
    ta_heap heap;
    heap_setup(&heap, &memory);
    CHECK(ta_heap_grow(&heap, ITEMS) == TA_OK);

    for (uint32_t index = 1; index <= 20; index += 1u) {
        set_key(&s, index, (index * 7u) % 5u, index);
    }
    item saved[21];
    for (uint32_t index = 1; index <= 20; index += 1u) {
        saved[index] = *item_of(&s, index);
        ta_heap_insert(&heap, &items, index);
    }
    /* Remove three and pop three; their owner writes an arrival number. */
    CHECK(ta_heap_remove(&heap, &items, 9) == TA_OK);
    item_of(&s, 9)->heap_index = 0xA0000009u;
    CHECK(ta_heap_remove(&heap, &items, 20) == TA_OK);
    item_of(&s, 20)->heap_index = 0xA0000014u;
    /* Due 0 are items 5, 10, 15 and 20; 20 is gone, so these pop first. */
    uint32_t popped[3];
    for (uint32_t step = 0; step < 3; step += 1u) {
        popped[step] = ta_heap_pop(&heap, &items);
        CHECK(popped[step] == (step + 1u) * 5u);
        item_of(&s, popped[step])->heap_index = 0xB0000000u + step;
    }
    CHECK(ta_heap_remove(&heap, &items, 13) == TA_OK);
    item_of(&s, 13)->heap_index = 0xA000000Du;
    drain_in_order(&heap, &s, &items);

    for (uint32_t index = 1; index <= 20; index += 1u) {
        item now = *item_of(&s, index);
        CHECK(now.next == saved[index].next);
        CHECK(now.due == saved[index].due && now.order == saved[index].order);
        CHECK(now.payload == saved[index].payload);
    }
    CHECK(item_of(&s, 9)->heap_index == 0xA0000009u);
    CHECK(item_of(&s, 20)->heap_index == 0xA0000014u);
    CHECK(item_of(&s, 13)->heap_index == 0xA000000Du);
    for (uint32_t step = 0; step < 3; step += 1u) {
        CHECK(item_of(&s, popped[step])->heap_index == 0xB0000000u + step);
    }
    /* Items never inserted are untouched entirely. */
    for (uint32_t index = 21; index <= ITEMS; index += 1u) {
        CHECK(item_of(&s, index)->heap_index == 0xdeadbeefu);
    }

    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);
    storage_destroy(&s);
}

/* 7 */
static void test_storage_moves(void) {
    counting_allocator memory = {0};
    storage s;
    storage_init(&s);
    ta_heap_items items = items_of(&s);
    ta_heap heap;
    heap_setup(&heap, &memory);
    CHECK(ta_heap_grow(&heap, ITEMS) == TA_OK);

    for (uint32_t index = 1; index <= ITEMS; index += 1u) {
        set_key(&s, index, (index * 13u) % 17u, index);
        ta_heap_insert(&heap, &items, index);
        if (index % 8u == 0u) {
            storage_move(&s);
            CHECK(well_formed(&heap, &s));
        }
    }
    for (uint32_t index = 3; index <= ITEMS; index += 5u) {
        CHECK(ta_heap_remove(&heap, &items, index) == TA_OK);
        storage_move(&s);
        CHECK(well_formed(&heap, &s));
    }
    uint64_t last_due = 0;
    uint64_t last_order = 0;
    while (ta_heap_length(&heap) > 0u) {
        uint32_t index = ta_heap_pop(&heap, &items);
        storage_move(&s);
        CHECK(item_of(&s, index)->due > last_due ||
              (item_of(&s, index)->due == last_due && item_of(&s, index)->order > last_order));
        last_due = item_of(&s, index)->due;
        last_order = item_of(&s, index)->order;
        CHECK(well_formed(&heap, &s));
    }

    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);
    storage_destroy(&s);
}

/* 8 */
static void test_never_allocates(void) {
    counting_allocator memory = {0};
    storage s;
    storage_init(&s);
    ta_heap_items items = items_of(&s);
    ta_heap heap;
    heap_setup(&heap, &memory);
    CHECK(ta_heap_grow(&heap, 24) == TA_OK);
    uint32_t attempts = memory.attempts;
    uint32_t live = memory.live;
    memory.fail_from = attempts + 1u; /* any allocation from here on fails */

    for (uint32_t index = 1; index <= 24; index += 1u) {
        set_key(&s, index, 30u - index, 0);
        ta_heap_insert(&heap, &items, index);
    }
    CHECK(ta_heap_length(&heap) == 24 && ta_heap_capacity(&heap) == 24);
    CHECK(ta_heap_head(&heap) == 24);
    CHECK(ta_heap_remove(&heap, &items, 10) == TA_OK);
    CHECK(ta_heap_remove(&heap, &items, 10) == TA_ERR_NOT_FOUND);
    drain_in_order(&heap, &s, &items);
    CHECK(memory.attempts == attempts && memory.live == live);

    memory.fail_from = 0;
    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);
    storage_destroy(&s);
}

/* 9 */
static void test_shrink(void) {
    counting_allocator memory = {0};
    storage s;
    storage_init(&s);
    ta_heap_items items = items_of(&s);
    ta_heap heap;
    heap_setup(&heap, &memory);
    CHECK(ta_heap_grow(&heap, 32) == TA_OK);
    CHECK(ta_heap_capacity(&heap) == 32 && memory.live == 9);

    for (uint32_t index = 1; index <= 10; index += 1u) {
        set_key(&s, index, index % 4u, index);
        ta_heap_insert(&heap, &items, index);
    }

    /* keep above the capacity: nothing changes. */
    ta_heap_shrink(&heap, 40);
    CHECK(ta_heap_capacity(&heap) == 32 && memory.live == 9);

    /* keep 20: positions 0..19 stay, five pages. */
    ta_heap_shrink(&heap, 20);
    CHECK(ta_heap_capacity(&heap) == 20 && memory.live == 6);
    CHECK(well_formed(&heap, &s));

    /* keep below the length: the length wins, three pages hold 10. */
    ta_heap_shrink(&heap, 0);
    CHECK(ta_heap_capacity(&heap) == 12 && memory.live == 4);
    CHECK(ta_heap_length(&heap) == 10 && well_formed(&heap, &s));

    /* Items pop in order after the shrink. */
    for (uint32_t step = 0; step < 6; step += 1u) {
        ta_heap_pop(&heap, &items);
        CHECK(well_formed(&heap, &s));
    }
    ta_heap_shrink(&heap, 0); /* four items: one page */
    CHECK(ta_heap_capacity(&heap) == 4 && memory.live == 2);
    drain_in_order(&heap, &s, &items);

    /* Empty, keep 0: every page and the directory go. */
    ta_heap_shrink(&heap, 0);
    CHECK(ta_heap_capacity(&heap) == 0 && memory.live == 0);

    /* And it grows again. */
    CHECK(ta_heap_grow(&heap, 3) == TA_OK);
    for (uint32_t index = 1; index <= 3; index += 1u) {
        ta_heap_insert(&heap, &items, index);
    }
    CHECK(well_formed(&heap, &s));
    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);
    storage_destroy(&s);
}

/* 10 */
static void test_destroy(void) {
    counting_allocator memory = {0};
    storage s;
    storage_init(&s);
    ta_heap_items items = items_of(&s);
    ta_heap heap;
    heap_setup(&heap, &memory);
    CHECK(ta_heap_grow(&heap, 30) == TA_OK);
    for (uint32_t index = 1; index <= 30; index += 1u) {
        set_key(&s, index, index, 0);
        ta_heap_insert(&heap, &items, index);
    }
    CHECK(memory.live == 9);
    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);
    CHECK(ta_heap_length(&heap) == 0 && ta_heap_capacity(&heap) == 0 && ta_heap_head(&heap) == 0);
    for (uint32_t index = 1; index <= 30; index += 1u) {
        CHECK(item_of(&s, index)->due == index && item_of(&s, index)->payload == 0xFACE0000u + index);
    }
    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);
    storage_destroy(&s);
}

/* 11 */
static uint32_t next_random(uint64_t *state) {
    *state = *state * 6364136223846793005u + 1442695040888963407u;
    return (uint32_t)(*state >> 33);
}

static void test_random(void) {
    counting_allocator memory = {0};
    storage s;
    storage_init(&s);
    ta_heap_items items = items_of(&s);
    ta_heap heap;
    heap_setup(&heap, &memory);
    CHECK(ta_heap_grow(&heap, ITEMS) == TA_OK);

    bool in_heap[ITEMS + 1u] = {0};
    uint32_t count = 0;
    uint64_t sequence = 0;
    uint64_t state = 12345;
    for (uint32_t step = 0; step < 2000u; step += 1u) {
        uint32_t index = next_random(&state) % ITEMS + 1u;
        uint32_t action = next_random(&state) % 3u;
        if (action == 0u && !in_heap[index]) {
            sequence += 1u;
            set_key(&s, index, next_random(&state) % 16u, sequence);
            ta_heap_insert(&heap, &items, index);
            in_heap[index] = true;
            count += 1u;
        } else if (action == 1u) {
            /* The model's smallest. */
            uint32_t expected = 0;
            for (uint32_t candidate = 1; candidate <= ITEMS; candidate += 1u) {
                if (in_heap[candidate] &&
                    (expected == 0u || key_less(item_of(&s, candidate), item_of(&s, expected)))) {
                    expected = candidate;
                }
            }
            CHECK(ta_heap_head(&heap) == expected);
            CHECK(ta_heap_pop(&heap, &items) == expected);
            if (expected != 0u) {
                in_heap[expected] = false;
                item_of(&s, expected)->heap_index = next_random(&state); /* an arrival number */
                count -= 1u;
            }
        } else {
            ta_result result = ta_heap_remove(&heap, &items, index);
            CHECK(result == (in_heap[index] ? TA_OK : TA_ERR_NOT_FOUND));
            if (in_heap[index]) {
                in_heap[index] = false;
                item_of(&s, index)->heap_index = next_random(&state);
                count -= 1u;
            }
        }
        CHECK(ta_heap_length(&heap) == count);
        CHECK(well_formed(&heap, &s));
    }

    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);
    storage_destroy(&s);
}

/* 12 */
static void test_full(void) {
    counting_allocator memory = {0};
    storage s;
    storage_init(&s);
    ta_heap_items items = items_of(&s);
    ta_heap heap;
    heap_setup(&heap, &memory);

    /* No capacity at all: full from the start. */
    set_key(&s, 1, 10, 1);
    item_of(&s, 1)->heap_index = 0xdeadbeefu;
    CHECK(ta_heap_insert(&heap, &items, 1) == TA_ERR_FULL);
    CHECK(ta_heap_length(&heap) == 0 && ta_heap_head(&heap) == 0);
    CHECK(item_of(&s, 1)->heap_index == 0xdeadbeefu);

    CHECK(ta_heap_grow(&heap, 1) == TA_OK);
    uint64_t capacity = ta_heap_capacity(&heap);
    for (uint32_t index = 1; index <= capacity; index++) {
        set_key(&s, index, 100u - index, index);
        CHECK(ta_heap_insert(&heap, &items, index) == TA_OK);
    }
    CHECK(ta_heap_length(&heap) == capacity);

    uint32_t extra = (uint32_t)capacity + 1u;
    set_key(&s, extra, 0, 0); /* would become the new head */
    item_of(&s, extra)->heap_index = 0xdeadbeefu;
    uint32_t head = ta_heap_head(&heap);
    uint32_t attempts = memory.attempts;
    CHECK(ta_heap_insert(&heap, &items, extra) == TA_ERR_FULL);
    CHECK(memory.attempts == attempts);
    CHECK(ta_heap_length(&heap) == capacity);
    CHECK(ta_heap_head(&heap) == head);
    CHECK(item_of(&s, extra)->heap_index == 0xdeadbeefu);
    CHECK(well_formed(&heap, &s));

    /* After a pop there is room again. */
    CHECK(ta_heap_pop(&heap, &items) == head);
    CHECK(ta_heap_insert(&heap, &items, extra) == TA_OK);
    CHECK(ta_heap_head(&heap) == extra);
    CHECK(well_formed(&heap, &s));

    ta_heap_destroy(&heap);
    CHECK(memory.live == 0);
    storage_destroy(&s);
}

int main(void) {
    test_init();
    test_grow();
    test_one_item();
    test_order();
    test_remove();
    test_writes_only_position();
    test_storage_moves();
    test_never_allocates();
    test_shrink();
    test_destroy();
    test_random();
    test_full();
    if (failures != 0) {
        fprintf(stderr, "heap_test: %d checks failed\n", failures);
        return 1;
    }
    printf("heap_test: ok\n");
    return 0;
}
