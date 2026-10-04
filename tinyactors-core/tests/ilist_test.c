/* test: standalone asan */
/*
 * Block B1 (src/ilist.h): the intrusive index list.
 *
 * Covers the descriptor (valid and invalid offsets, the compile-time field
 * check), each operation against a hand-written expectation, unlink of the
 * head, a middle item, the tail, the only item and an item that is not
 * there, splice of every combination of empty and non-empty lists, two lists
 * through two link fields on the same items (neither kind may touch the
 * other's field), the pool's own free-list field used as a list link, the
 * promise that no list operation allocates, and the bounded walk of unlink on
 * a cyclic list. A seeded model test compares three lists with a plain array
 * model after every step. It is fast, not exhaustive: about twenty thousand
 * steps over forty items, and there is no allocation to sweep.
 *
 * Includes the amalgamation, because the unit is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>

#include "test_allocator.h"

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

static ta_test_allocator heap = { .strict = true };

/* A 16-byte item with two link fields: the same item can be on a `next` list and an `owner_next` list. */
typedef struct {
    uint32_t next;
    uint32_t owner_next;
    uint32_t tag;
    uint32_t pad;
} item;

static ta_pool pool;

static void pool_open(uint32_t count) {
    ta_pool_params params = { .item_size = sizeof(item),
                              .item_alignment = _Alignof(item),
                              .link_offset = offsetof(item, next),
                              .page_items = 4 };
    CHECK(ta_pool_init(&pool, ta_test_allocator_get(&heap), &params) == TA_OK);
    CHECK(ta_pool_refill(&pool, count) == TA_OK);
    for (uint32_t expected = 1; expected <= count; expected += 1u) {
        uint32_t index = ta_pool_take(&pool);
        CHECK(index == expected);
        ((item *)ta_pool_at(&pool, index))->tag = index;
    }
}

static void pool_close(void) {
    ta_pool_destroy(&pool);
    CHECK(heap.stats.bad_frees == 0);
    CHECK(heap.stats.live_blocks == 0 && heap.stats.live_bytes == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
}

static item *at(uint32_t index) {
    return ta_pool_at(&pool, index);
}

/* walk copies the list into out[] by ta_ilist_next and returns its length (at most max, so a bug cannot hang). */
static uint32_t walk(const ta_ilist *list, const ta_ilink *link, uint32_t *out, uint32_t max) {
    uint32_t count = 0;
    for (uint32_t index = list->head; index != 0 && count < max; index = ta_ilist_next(link, index)) {
        out[count] = index;
        count += 1u;
    }
    return count;
}

/* expect_list checks the list holds exactly the given indexes in order, with a consistent head and tail. */
static void expect_list(const ta_ilist *list, const ta_ilink *link, const uint32_t *want, uint32_t count) {
    uint32_t got[64];
    CHECK(walk(list, link, got, 64) == count);
    for (uint32_t i = 0; i < count; i += 1u) {
        CHECK(got[i] == want[i]);
    }
    CHECK(list->head == (count == 0 ? 0u : want[0]));
    CHECK(list->tail == (count == 0 ? 0u : want[count - 1u]));
    CHECK(ta_ilist_is_empty(list) == (count == 0));
    if (count != 0) {
        CHECK(ta_ilist_next(link, want[count - 1u]) == 0);
    }
}

static void test_link_descriptor(void) {
    pool_open(4);
    ta_ilink link;
    CHECK(ta_ilink_init(&link, &pool, offsetof(item, owner_next)) == TA_OK);
    CHECK(link.pool == &pool && link.offset == offsetof(item, owner_next));
    CHECK(ta_ilink_init(&link, &pool, sizeof(item) - 4u) == TA_OK);
    CHECK(ta_ilink_init(&link, &pool, 0) == TA_OK);

    CHECK(ta_ilink_init(&link, &pool, 2) == TA_ERR_INVALID);
    CHECK(link.pool == NULL && link.offset == 0);
    CHECK(ta_ilink_init(&link, &pool, sizeof(item)) == TA_ERR_INVALID);
    CHECK(ta_ilink_init(&link, &pool, sizeof(item) - 3u) == TA_ERR_INVALID);
    CHECK(ta_ilink_init(&link, &pool, SIZE_MAX - 3u) == TA_ERR_INVALID);
    CHECK(ta_ilink_init(&link, NULL, 0) == TA_ERR_INVALID);
    CHECK(ta_ilink_init(NULL, &pool, 0) == TA_ERR_INVALID);
    ta_pool never;
    ta_mem_zero(&never, sizeof never);
    CHECK(ta_ilink_init(&link, &never, 0) == TA_ERR_INVALID);

    /* TA_ILINK: the type check compiles for a uint32_t field, and the offset is the struct's. */
    ta_ilink next = TA_ILINK(&pool, item, next);
    ta_ilink owner = TA_ILINK(&pool, item, owner_next);
    CHECK(next.pool == &pool && next.offset == 0);
    CHECK(owner.pool == &pool && owner.offset == 4u);
    CHECK(TA_ILINK(&pool, item, tag).offset == 8u);
    pool_close();
}

static void test_push_pop_order(void) {
    pool_open(6);
    ta_ilink link = TA_ILINK(&pool, item, next);
    ta_ilist list = { 0 };
    CHECK(ta_ilist_is_empty(&list));
    CHECK(ta_ilist_pop_front(&list, &link) == 0);
    expect_list(&list, &link, NULL, 0);

    ta_ilist_push_back(&list, &link, 1);
    CHECK(list.head == 1 && list.tail == 1);
    ta_ilist_push_back(&list, &link, 2);
    ta_ilist_push_back(&list, &link, 3);
    ta_ilist_push_front(&list, &link, 4);
    ta_ilist_push_front(&list, &link, 5);
    uint32_t want[] = { 5, 4, 1, 2, 3 };
    expect_list(&list, &link, want, 5);

    CHECK(ta_ilist_pop_front(&list, &link) == 5);
    CHECK(at(5)->next == 0);
    CHECK(ta_ilist_pop_front(&list, &link) == 4);
    CHECK(ta_ilist_pop_front(&list, &link) == 1);
    CHECK(ta_ilist_pop_front(&list, &link) == 2);
    uint32_t last[] = { 3 };
    expect_list(&list, &link, last, 1);
    CHECK(ta_ilist_pop_front(&list, &link) == 3);
    expect_list(&list, &link, NULL, 0);
    CHECK(ta_ilist_pop_front(&list, &link) == 0);

    /* a popped item can be pushed again at once; init discards a list without touching items */
    ta_ilist_push_back(&list, &link, 3);
    ta_ilist_push_back(&list, &link, 5);
    ta_ilist_init(&list);
    expect_list(&list, &link, NULL, 0);
    CHECK(at(3)->next == 5);
    at(3)->next = 0;
    pool_close();
}

static void fill(ta_ilist *list, const ta_ilink *link, const uint32_t *indexes, uint32_t count) {
    ta_ilist_init(list);
    for (uint32_t i = 0; i < count; i += 1u) {
        ta_ilist_push_back(list, link, indexes[i]);
    }
}

static void test_unlink(void) {
    pool_open(6);
    ta_ilink link = TA_ILINK(&pool, item, next);
    ta_ilist list;
    uint32_t all[] = { 1, 2, 3, 4 };

    fill(&list, &link, all, 4);
    CHECK(ta_ilist_unlink(&list, &link, 1));
    uint32_t no_head[] = { 2, 3, 4 };
    expect_list(&list, &link, no_head, 3);
    CHECK(at(1)->next == 0);

    CHECK(ta_ilist_unlink(&list, &link, 3));
    uint32_t no_middle[] = { 2, 4 };
    expect_list(&list, &link, no_middle, 2);
    CHECK(at(3)->next == 0);

    CHECK(ta_ilist_unlink(&list, &link, 4));
    uint32_t no_tail[] = { 2 };
    expect_list(&list, &link, no_tail, 1);

    CHECK(ta_ilist_unlink(&list, &link, 2));
    expect_list(&list, &link, NULL, 0);

    /* absent: unchanged, including the items' links; 0 and any index on an empty list */
    CHECK(!ta_ilist_unlink(&list, &link, 1));
    CHECK(!ta_ilist_unlink(&list, &link, 0));
    uint32_t some[] = { 2, 3, 4 };
    fill(&list, &link, some, 3);
    CHECK(!ta_ilist_unlink(&list, &link, 5));
    CHECK(!ta_ilist_unlink(&list, &link, 1));
    CHECK(!ta_ilist_unlink(&list, &link, 0));
    CHECK(!ta_ilist_unlink(&list, &link, 99));
    expect_list(&list, &link, some, 3);
    CHECK(at(5)->next == 0 && at(1)->next == 0);

    /* an unlinked item goes back on, and can be given to the pool */
    CHECK(ta_ilist_unlink(&list, &link, 3));
    ta_ilist_push_front(&list, &link, 3);
    uint32_t again[] = { 3, 2, 4 };
    expect_list(&list, &link, again, 3);
    while (ta_ilist_pop_front(&list, &link) != 0) {
    }
    pool_close();
}

static void test_splice(void) {
    pool_open(8);
    ta_ilink link = TA_ILINK(&pool, item, next);
    ta_ilist to, from;

    /* empty into empty */
    ta_ilist_init(&to);
    ta_ilist_init(&from);
    ta_ilist_splice(&to, &from, &link);
    expect_list(&to, &link, NULL, 0);
    expect_list(&from, &link, NULL, 0);

    /* empty into non-empty */
    uint32_t a[] = { 1, 2, 3 };
    fill(&to, &link, a, 3);
    ta_ilist_splice(&to, &from, &link);
    expect_list(&to, &link, a, 3);
    expect_list(&from, &link, NULL, 0);

    /* non-empty into non-empty: order is to, then from */
    uint32_t b[] = { 4, 5 };
    fill(&from, &link, b, 2);
    ta_ilist_splice(&to, &from, &link);
    uint32_t joined[] = { 1, 2, 3, 4, 5 };
    expect_list(&to, &link, joined, 5);
    expect_list(&from, &link, NULL, 0);
    CHECK(at(3)->next == 4);

    /* push_back after a splice lands after the spliced tail; push_front on the emptied list works */
    ta_ilist_push_back(&to, &link, 6);
    uint32_t more[] = { 1, 2, 3, 4, 5, 6 };
    expect_list(&to, &link, more, 6);
    ta_ilist_push_front(&from, &link, 7);
    uint32_t seven[] = { 7 };
    expect_list(&from, &link, seven, 1);

    /* non-empty into empty: the target takes the list over */
    ta_ilist empty;
    ta_ilist_init(&empty);
    ta_ilist_splice(&empty, &to, &link);
    expect_list(&empty, &link, more, 6);
    expect_list(&to, &link, NULL, 0);
    while (ta_ilist_pop_front(&empty, &link) != 0) {
    }
    while (ta_ilist_pop_front(&from, &link) != 0) {
    }
    pool_close();
}

static void test_two_kinds_on_the_same_items(void) {
    pool_open(6);
    ta_ilink next = TA_ILINK(&pool, item, next);
    ta_ilink owner = TA_ILINK(&pool, item, owner_next);
    ta_ilist queue = { 0 }, mine = { 0 };

    for (uint32_t index = 1; index <= 5; index += 1u) {
        ta_ilist_push_back(&queue, &next, index);
    }
    for (uint32_t index = 5; index >= 1; index -= 1u) {
        ta_ilist_push_back(&mine, &owner, index);
    }
    uint32_t forward[] = { 1, 2, 3, 4, 5 };
    uint32_t backward[] = { 5, 4, 3, 2, 1 };
    expect_list(&queue, &next, forward, 5);
    expect_list(&mine, &owner, backward, 5);

    /* operations on one kind leave the other's fields alone: check field by field */
    uint32_t owner_before[6], next_before[6];
    for (uint32_t index = 1; index <= 5; index += 1u) {
        owner_before[index] = at(index)->owner_next;
        next_before[index] = at(index)->next;
    }
    CHECK(ta_ilist_pop_front(&queue, &next) == 1);
    CHECK(ta_ilist_unlink(&queue, &next, 3));
    ta_ilist_push_front(&queue, &next, 3);
    for (uint32_t index = 1; index <= 5; index += 1u) {
        CHECK(at(index)->owner_next == owner_before[index]);
    }
    expect_list(&mine, &owner, backward, 5);

    /* unlink from the owner list while the item is still on the run queue */
    CHECK(ta_ilist_unlink(&mine, &owner, 4));
    uint32_t mine_now[] = { 5, 3, 2, 1 };
    expect_list(&mine, &owner, mine_now, 4);
    uint32_t queue_now[] = { 3, 2, 4, 5 };
    expect_list(&queue, &next, queue_now, 4);
    CHECK(at(4)->next == next_before[4]);
    CHECK(at(4)->owner_next == 0);

    /* an item can leave the queue and stay on the owner list */
    CHECK(ta_ilist_unlink(&queue, &next, 2));
    CHECK(at(2)->next == 0 && at(2)->owner_next == 1u);
    expect_list(&mine, &owner, mine_now, 4);

    ta_ilist_init(&queue);
    ta_ilist_init(&mine);
    pool_close();
}

static void test_pool_link_field_as_next(void) {
    pool_open(3);
    ta_ilink link = TA_ILINK(&pool, item, next);
    ta_ilist list = { 0 };
    uint32_t extra = ta_pool_take(&pool);
    CHECK(extra == 4);
    CHECK(at(extra)->next == 0);

    ta_pool_counts before, after;
    ta_pool_counts_get(&pool, &before);
    ta_ilist_push_back(&list, &link, 2);
    ta_ilist_push_back(&list, &link, extra);
    ta_ilist_push_back(&list, &link, 1);
    CHECK(ta_ilist_unlink(&list, &link, extra));
    CHECK(ta_ilist_pop_front(&list, &link) == 2);
    ta_pool_counts_get(&pool, &after);
    CHECK(before.used == after.used && before.free == after.free && before.pages == after.pages);

    /* give after unlink: the pool reuses the item and a take starts at link 0 again */
    CHECK(ta_ilist_unlink(&list, &link, 1));
    ta_pool_give(&pool, extra);
    CHECK(ta_pool_take(&pool) == extra);
    CHECK(at(extra)->next == 0);
    expect_list(&list, &link, NULL, 0);
    pool_close();
}

static void test_never_allocates(void) {
    pool_open(6);
    ta_ilink next = TA_ILINK(&pool, item, next);
    ta_ilink owner = TA_ILINK(&pool, item, owner_next);
    ta_ilist a = { 0 }, b = { 0 }, o = { 0 };
    ta_test_allocator_reset_counts(&heap);
    for (uint32_t index = 1; index <= 3; index += 1u) {
        ta_ilist_push_back(&a, &next, index);
        ta_ilist_push_front(&o, &owner, index);
    }
    ta_ilist_push_back(&b, &next, 4);
    ta_ilist_splice(&a, &b, &next);
    (void)ta_ilist_unlink(&a, &next, 2);
    (void)ta_ilist_unlink(&a, &next, 6);
    (void)ta_ilist_next(&next, 1);
    (void)ta_ilist_is_empty(&a);
    (void)ta_ilist_pop_front(&a, &next);
    (void)ta_ilink_init(&next, &pool, 0);
    ta_ilist_init(&a);
    CHECK(heap.stats.allocations == 0 && heap.stats.refused == 0);
    ta_ilist_init(&o);
    pool_close();
}

static void test_cyclic_unlink_terminates(void) {
    pool_open(4);
    ta_ilink link = TA_ILINK(&pool, item, next);
    ta_ilist list = { .head = 1, .tail = 3 };
    at(1)->next = 2;
    at(2)->next = 3;
    at(3)->next = 2; /* a cycle that never reaches 4 */
    CHECK(!ta_ilist_unlink(&list, &link, 4));
    CHECK(ta_ilist_unlink(&list, &link, 3));
    at(1)->next = 0;
    at(2)->next = 0;
    pool_close();
}

/* A seeded xorshift: the model test must not depend on the C library. */
static uint32_t next_random(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

#define MODEL_ITEMS 40u
#define MODEL_LISTS 3u /* lists 0 and 1 link through `next`, list 2 through `owner_next` */

typedef struct {
    uint32_t seq[MODEL_ITEMS];
    uint32_t len;
} model_list;

static uint32_t model_find(const model_list *list, uint32_t index) {
    for (uint32_t i = 0; i < list->len; i += 1u) {
        if (list->seq[i] == index) {
            return i;
        }
    }
    return UINT32_MAX;
}

static void model_remove_at(model_list *list, uint32_t position) {
    for (uint32_t i = position + 1u; i < list->len; i += 1u) {
        list->seq[i - 1u] = list->seq[i];
    }
    list->len -= 1u;
}

static void test_model(void) {
    pool_open(MODEL_ITEMS);
    ta_ilink links[MODEL_LISTS] = { TA_ILINK(&pool, item, next), TA_ILINK(&pool, item, next),
                                    TA_ILINK(&pool, item, owner_next) };
    ta_ilist lists[MODEL_LISTS] = { { 0 }, { 0 }, { 0 } };
    model_list model[MODEL_LISTS] = { { { 0 }, 0 }, { { 0 }, 0 }, { { 0 }, 0 } };
    uint32_t state = 0x9e3779b9u;

    ta_test_allocator_reset_counts(&heap);
    for (uint32_t step = 0; step < 20000u; step += 1u) {
        uint32_t which = next_random(&state) % MODEL_LISTS;
        uint32_t kind_other = which == 0 ? 1u : (which == 1 ? 0u : MODEL_LISTS);
        uint32_t operation = next_random(&state) % 8u;
        uint32_t index = 1u + next_random(&state) % MODEL_ITEMS;
        model_list *m = &model[which];
        /* is the item on a list of this kind already? */
        bool listed = model_find(m, index) != UINT32_MAX || (kind_other < MODEL_LISTS && model_find(&model[kind_other], index) != UINT32_MAX);
        if (operation <= 1 && !listed && m->len < MODEL_ITEMS) {
            ta_ilist_push_back(&lists[which], &links[which], index);
            m->seq[m->len] = index;
            m->len += 1u;
        } else if (operation <= 3 && !listed && m->len < MODEL_ITEMS) {
            ta_ilist_push_front(&lists[which], &links[which], index);
            for (uint32_t i = m->len; i > 0; i -= 1u) {
                m->seq[i] = m->seq[i - 1u];
            }
            m->seq[0] = index;
            m->len += 1u;
        } else if (operation == 4) {
            uint32_t got = ta_ilist_pop_front(&lists[which], &links[which]);
            CHECK(got == (m->len == 0 ? 0u : m->seq[0]));
            if (m->len != 0) {
                model_remove_at(m, 0);
            }
        } else if (operation <= 6) {
            uint32_t position = model_find(m, index);
            bool removed = ta_ilist_unlink(&lists[which], &links[which], index);
            CHECK(removed == (position != UINT32_MAX));
            if (position != UINT32_MAX) {
                model_remove_at(m, position);
            }
        } else if (which != 2) {
            /* splice list `kind_other` onto list `which`: the two `next` lists share the field */
            model_list *f = &model[kind_other];
            ta_ilist_splice(&lists[which], &lists[kind_other], &links[which]);
            for (uint32_t i = 0; i < f->len; i += 1u) {
                m->seq[m->len + i] = f->seq[i];
            }
            m->len += f->len;
            f->len = 0;
        }
        for (uint32_t l = 0; l < MODEL_LISTS; l += 1u) {
            expect_list(&lists[l], &links[l], model[l].seq, model[l].len);
        }
        for (uint32_t item_index = 1; item_index <= MODEL_ITEMS; item_index += 1u) {
            bool on_next = model_find(&model[0], item_index) != UINT32_MAX || model_find(&model[1], item_index) != UINT32_MAX;
            bool on_owner = model_find(&model[2], item_index) != UINT32_MAX;
            if (!on_next) {
                CHECK(at(item_index)->next == 0);
            }
            if (!on_owner) {
                CHECK(at(item_index)->owner_next == 0);
            }
        }
    }
    CHECK(heap.stats.allocations == 0);
    for (uint32_t l = 0; l < MODEL_LISTS; l += 1u) {
        ta_ilist_init(&lists[l]);
    }
    pool_close();
}

int main(void) {
    test_link_descriptor();
    test_push_pop_order();
    test_unlink();
    test_splice();
    test_two_kinds_on_the_same_items();
    test_pool_link_field_as_next();
    test_never_allocates();
    test_cyclic_unlink_terminates();
    test_model();
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "ilist_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("ilist_test: ok");
    return 0;
}
