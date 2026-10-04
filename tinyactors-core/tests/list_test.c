/*
 * ta_list: the intrusive index list (src/list.h), which holds the
 * envelope pool's free list, the run queue, and the outgoing, deferred,
 * waiting and owner lists.
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Items live in small pages that
 * are not one array, as an index pool's do, and each carries two link
 * fields, `next` and `owner_next`.
 *
 * Claims proven here, in order:
 *
 * 1. an empty list is {0, 0}: head, tail and pop_front answer 0, and
 *    pop_front on it changes nothing; a ta_list is 8 bytes;
 * 2. push_back and push_front keep order and the head/tail invariants
 *    from the empty and the one-item list on, and the tail's link is 0;
 * 3. pop_front removes in order, clears the removed item's link, and
 *    empties the list after the last item;
 * 4. insert_after puts an item after the head, in the middle and after
 *    the tail (moving the tail), and at the front when `after` is 0,
 *    including into an empty list;
 * 5. splice appends a whole list in O(1) (one link written), leaves the
 *    source empty, and works with an empty source or an empty target;
 * 6. unlink removes the head, a middle item, the tail and the only item,
 *    fixing head and tail; an item not on the list (or 0) answers
 *    TA_ERR_NOT_FOUND and changes nothing;
 * 7. remove_after removes the head (previous 0), a middle item and the
 *    tail (moving the tail back) in one lookup of the previous item and
 *    one of the removed item, clears the removed link, and answers 0 and
 *    changes nothing after the tail or on an empty list; a walk that
 *    keeps its previous item removes what it finds without walking again;
 * 8. one item sits on two lists at once through different fields, and
 *    operations on one leave the other intact;
 * 9. the list keeps no pointer into storage: moving the items to new
 *    memory between calls (an index pool growing at a safe point) leaves
 *    every list intact;
 * 10. the list never allocates: no function takes an allocator, and the
 *    list operations above complete without one.
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
 * Items in pages of four, with a directory of page pointers, so items are
 * not contiguous. Index 0 is never an item.
 */

typedef struct {
    uint32_t next;
    uint32_t owner_next;
    uint32_t value;
} item;

#define PAGE_ITEMS 4u
#define PAGES 4u

typedef struct {
    item *pages[PAGES];
    uint32_t lookups;
} storage;

static void *item_at(void *items, uint32_t index) {
    storage *s = items;
    s->lookups += 1u;
    if (index == 0u || index > PAGES * PAGE_ITEMS) {
        fprintf(stderr, "item_at: bad index %u\n", index);
        abort();
    }
    uint32_t at = index - 1u;
    return &s->pages[at / PAGE_ITEMS][at % PAGE_ITEMS];
}

static void storage_init(storage *s) {
    s->lookups = 0u;
    for (uint32_t p = 0; p < PAGES; p++) {
        s->pages[p] = calloc(PAGE_ITEMS, sizeof(item));
        for (uint32_t i = 0; i < PAGE_ITEMS; i++) {
            /* Garbage links: a push must not depend on a clean field. */
            s->pages[p][i].next = 0xdeadbeefu;
            s->pages[p][i].owner_next = 0xdeadbeefu;
            s->pages[p][i].value = p * PAGE_ITEMS + i + 1u;
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

static ta_list_links next_links(storage *s) {
    return (ta_list_links){ .item = item_at, .items = s, .offset = offsetof(item, next) };
}

static ta_list_links owner_links(storage *s) {
    return (ta_list_links){ .item = item_at, .items = s, .offset = offsetof(item, owner_next) };
}

/* Whether `list` holds exactly `expected[0..count)`, in order, with a
 * consistent head, tail and terminating link. */
static bool holds(const ta_list *list, const ta_list_links *links, const uint32_t *expected, uint32_t count) {
    if (count == 0u) {
        return list->head == 0u && list->tail == 0u && ta_list_empty(list);
    }
    if (ta_list_empty(list) || ta_list_head(list) != expected[0] || ta_list_tail(list) != expected[count - 1u]) {
        return false;
    }
    uint32_t at = ta_list_head(list);
    for (uint32_t i = 0; i < count; i++) {
        if (at != expected[i]) {
            return false;
        }
        at = ta_list_next(links, at);
    }
    return at == 0u;
}

#define HOLDS(list, links, ...) \
    holds((list), (links), (const uint32_t[]){ __VA_ARGS__ }, \
          (uint32_t)(sizeof((const uint32_t[]){ __VA_ARGS__ }) / sizeof(uint32_t)))

static uint32_t link_of(storage *s, uint32_t index) { return ((item *)item_at(s, index))->next; }

/* 1 */
static void test_empty(void) {
    storage s;
    storage_init(&s);
    ta_list_links links = next_links(&s);
    ta_list list = TA_LIST_EMPTY;
    CHECK(sizeof(ta_list) == 8u);
    CHECK(holds(&list, &links, NULL, 0u));
    CHECK(ta_list_head(&list) == 0u);
    CHECK(ta_list_tail(&list) == 0u);
    CHECK(ta_list_pop_front(&list, &links) == 0u);
    CHECK(holds(&list, &links, NULL, 0u));
    CHECK(s.lookups == 0u);
    storage_destroy(&s);
}

/* 2 */
static void test_push(void) {
    storage s;
    storage_init(&s);
    ta_list_links links = next_links(&s);
    ta_list list = TA_LIST_EMPTY;

    ta_list_push_back(&list, &links, 5u);
    CHECK(HOLDS(&list, &links, 5u));
    ta_list_push_back(&list, &links, 9u);
    CHECK(HOLDS(&list, &links, 5u, 9u));
    ta_list_push_front(&list, &links, 2u);
    CHECK(HOLDS(&list, &links, 2u, 5u, 9u));
    ta_list_push_back(&list, &links, 16u);
    CHECK(HOLDS(&list, &links, 2u, 5u, 9u, 16u));
    CHECK(link_of(&s, 16u) == 0u);

    ta_list front = TA_LIST_EMPTY;
    ta_list_push_front(&front, &links, 3u);
    CHECK(HOLDS(&front, &links, 3u));
    CHECK(link_of(&s, 3u) == 0u);
    ta_list_push_front(&front, &links, 1u);
    CHECK(HOLDS(&front, &links, 1u, 3u));
    storage_destroy(&s);
}

/* 3 */
static void test_pop_front(void) {
    storage s;
    storage_init(&s);
    ta_list_links links = next_links(&s);
    ta_list list = TA_LIST_EMPTY;
    ta_list_push_back(&list, &links, 4u);
    ta_list_push_back(&list, &links, 8u);
    ta_list_push_back(&list, &links, 1u);

    CHECK(ta_list_pop_front(&list, &links) == 4u);
    CHECK(link_of(&s, 4u) == 0u);
    CHECK(HOLDS(&list, &links, 8u, 1u));
    CHECK(ta_list_pop_front(&list, &links) == 8u);
    CHECK(link_of(&s, 8u) == 0u);
    CHECK(HOLDS(&list, &links, 1u));
    CHECK(ta_list_pop_front(&list, &links) == 1u);
    CHECK(holds(&list, &links, NULL, 0u));
    CHECK(ta_list_pop_front(&list, &links) == 0u);

    /* A popped item can go back: the list is reusable once empty. */
    ta_list_push_back(&list, &links, 8u);
    CHECK(HOLDS(&list, &links, 8u));
    storage_destroy(&s);
}

/* 4 */
static void test_insert_after(void) {
    storage s;
    storage_init(&s);
    ta_list_links links = next_links(&s);
    ta_list list = TA_LIST_EMPTY;

    ta_list_insert_after(&list, &links, (ta_list_insert){ .after = 0u, .item = 7u });
    CHECK(HOLDS(&list, &links, 7u));
    ta_list_insert_after(&list, &links, (ta_list_insert){ .after = 7u, .item = 9u });
    CHECK(HOLDS(&list, &links, 7u, 9u));
    ta_list_insert_after(&list, &links, (ta_list_insert){ .after = 7u, .item = 8u });
    CHECK(HOLDS(&list, &links, 7u, 8u, 9u));
    ta_list_insert_after(&list, &links, (ta_list_insert){ .after = 0u, .item = 1u });
    CHECK(HOLDS(&list, &links, 1u, 7u, 8u, 9u));
    ta_list_insert_after(&list, &links, (ta_list_insert){ .after = 9u, .item = 12u });
    CHECK(HOLDS(&list, &links, 1u, 7u, 8u, 9u, 12u));
    CHECK(link_of(&s, 12u) == 0u);
    /* The new tail is where push_back appends. */
    ta_list_push_back(&list, &links, 13u);
    CHECK(HOLDS(&list, &links, 1u, 7u, 8u, 9u, 12u, 13u));
    storage_destroy(&s);
}

/* 5 */
static void test_splice(void) {
    storage s;
    storage_init(&s);
    ta_list_links links = next_links(&s);
    ta_list run = TA_LIST_EMPTY;
    ta_list outgoing = TA_LIST_EMPTY;

    /* Empty into empty. */
    ta_list_splice(&run, &links, &outgoing);
    CHECK(holds(&run, &links, NULL, 0u));
    CHECK(holds(&outgoing, &links, NULL, 0u));

    /* Into an empty target. */
    ta_list_push_back(&outgoing, &links, 3u);
    ta_list_push_back(&outgoing, &links, 6u);
    ta_list_splice(&run, &links, &outgoing);
    CHECK(HOLDS(&run, &links, 3u, 6u));
    CHECK(holds(&outgoing, &links, NULL, 0u));

    /* An empty source changes nothing. */
    s.lookups = 0u;
    ta_list_splice(&run, &links, &outgoing);
    CHECK(s.lookups == 0u);
    CHECK(HOLDS(&run, &links, 3u, 6u));

    /* Onto a non-empty target: one lookup, however long the source. */
    for (uint32_t i = 7u; i <= 16u; i++) {
        ta_list_push_back(&outgoing, &links, i);
    }
    s.lookups = 0u;
    ta_list_splice(&run, &links, &outgoing);
    CHECK(s.lookups == 1u);
    CHECK(HOLDS(&run, &links, 3u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u));
    CHECK(holds(&outgoing, &links, NULL, 0u));
    storage_destroy(&s);
}

/* 6 */
static void test_unlink(void) {
    storage s;
    storage_init(&s);
    ta_list_links links = next_links(&s);
    ta_list list = TA_LIST_EMPTY;
    for (uint32_t i = 1u; i <= 5u; i++) {
        ta_list_push_back(&list, &links, i);
    }

    /* Not on the list, and 0: nothing changes. */
    CHECK(ta_list_unlink(&list, &links, 9u) == TA_ERR_NOT_FOUND);
    CHECK(ta_list_unlink(&list, &links, 0u) == TA_ERR_NOT_FOUND);
    CHECK(HOLDS(&list, &links, 1u, 2u, 3u, 4u, 5u));

    CHECK(ta_list_unlink(&list, &links, 3u) == TA_OK);
    CHECK(link_of(&s, 3u) == 0u);
    CHECK(HOLDS(&list, &links, 1u, 2u, 4u, 5u));
    CHECK(ta_list_unlink(&list, &links, 3u) == TA_ERR_NOT_FOUND);
    CHECK(ta_list_unlink(&list, &links, 1u) == TA_OK);
    CHECK(HOLDS(&list, &links, 2u, 4u, 5u));
    CHECK(ta_list_unlink(&list, &links, 5u) == TA_OK);
    CHECK(link_of(&s, 5u) == 0u);
    CHECK(HOLDS(&list, &links, 2u, 4u));
    /* The tail moved back: push_back appends after 4. */
    ta_list_push_back(&list, &links, 6u);
    CHECK(HOLDS(&list, &links, 2u, 4u, 6u));
    CHECK(ta_list_unlink(&list, &links, 2u) == TA_OK);
    CHECK(ta_list_unlink(&list, &links, 6u) == TA_OK);
    CHECK(HOLDS(&list, &links, 4u));
    CHECK(ta_list_unlink(&list, &links, 4u) == TA_OK);
    CHECK(holds(&list, &links, NULL, 0u));
    CHECK(ta_list_unlink(&list, &links, 4u) == TA_ERR_NOT_FOUND);
    CHECK(holds(&list, &links, NULL, 0u));
    storage_destroy(&s);
}

/* 7 */
static void test_remove_after(void) {
    storage s;
    storage_init(&s);
    ta_list_links links = next_links(&s);
    ta_list list = TA_LIST_EMPTY;

    /* Empty list: nothing to remove, nothing changes. */
    CHECK(ta_list_remove_after(&list, &links, 0u) == 0u);
    CHECK(holds(&list, &links, NULL, 0u));

    for (uint32_t i = 1u; i <= 5u; i++) {
        ta_list_push_back(&list, &links, i);
    }
    /* The head, through previous 0. */
    CHECK(ta_list_remove_after(&list, &links, 0u) == 1u);
    CHECK(link_of(&s, 1u) == 0u);
    CHECK(HOLDS(&list, &links, 2u, 3u, 4u, 5u));

    /* A middle item: one lookup for the previous item, one for the removed. */
    s.lookups = 0u;
    CHECK(ta_list_remove_after(&list, &links, 2u) == 3u);
    CHECK(s.lookups == 2u);
    CHECK(link_of(&s, 3u) == 0u);
    CHECK(HOLDS(&list, &links, 2u, 4u, 5u));

    /* The tail: the tail moves back, and push_back follows it. */
    CHECK(ta_list_remove_after(&list, &links, 4u) == 5u);
    CHECK(HOLDS(&list, &links, 2u, 4u));
    ta_list_push_back(&list, &links, 6u);
    CHECK(HOLDS(&list, &links, 2u, 4u, 6u));

    /* After the tail there is nothing: 0, and nothing changes. */
    CHECK(ta_list_remove_after(&list, &links, 6u) == 0u);
    CHECK(HOLDS(&list, &links, 2u, 4u, 6u));

    /* A walk that keeps its previous item takes the first even item in one
     * pass, as filling a slot takes the first runnable envelope. */
    ta_list walk = TA_LIST_EMPTY;
    ta_list_links owner = owner_links(&s);
    uint32_t order[] = { 7u, 9u, 8u, 11u, 10u };
    for (uint32_t i = 0; i < 5u; i++) {
        ta_list_push_back(&walk, &owner, order[i]);
    }
    uint32_t previous = 0u;
    uint32_t at = ta_list_head(&walk);
    while (at != 0u && at % 2u != 0u) {
        previous = at;
        at = ta_list_next(&owner, at);
    }
    CHECK(at == 8u);
    CHECK(ta_list_remove_after(&walk, &owner, previous) == 8u);
    CHECK(HOLDS(&walk, &owner, 7u, 9u, 11u, 10u));
    /* The other field's list is untouched. */
    CHECK(HOLDS(&list, &links, 2u, 4u, 6u));
    storage_destroy(&s);
}

/* 8 */
static void test_two_fields(void) {
    storage s;
    storage_init(&s);
    ta_list_links next = next_links(&s);
    ta_list_links owner = owner_links(&s);
    ta_list run = TA_LIST_EMPTY;
    ta_list owned = TA_LIST_EMPTY;

    for (uint32_t i = 1u; i <= 6u; i++) {
        ta_list_push_back(&run, &next, i);
    }
    ta_list_push_back(&owned, &owner, 5u);
    ta_list_push_back(&owned, &owner, 2u);
    ta_list_push_back(&owned, &owner, 4u);
    CHECK(HOLDS(&run, &next, 1u, 2u, 3u, 4u, 5u, 6u));
    CHECK(HOLDS(&owned, &owner, 5u, 2u, 4u));

    CHECK(ta_list_unlink(&owned, &owner, 2u) == TA_OK);
    CHECK(HOLDS(&owned, &owner, 5u, 4u));
    CHECK(HOLDS(&run, &next, 1u, 2u, 3u, 4u, 5u, 6u));

    CHECK(ta_list_pop_front(&run, &next) == 1u);
    CHECK(ta_list_unlink(&run, &next, 4u) == TA_OK);
    CHECK(HOLDS(&run, &next, 2u, 3u, 5u, 6u));
    CHECK(HOLDS(&owned, &owner, 5u, 4u));
    storage_destroy(&s);
}

/* 9 */
static void test_storage_moves(void) {
    storage s;
    storage_init(&s);
    ta_list_links links = next_links(&s);
    ta_list a = TA_LIST_EMPTY;
    ta_list b = TA_LIST_EMPTY;

    ta_list_push_back(&a, &links, 1u);
    ta_list_push_back(&a, &links, 10u);
    storage_move(&s);
    ta_list_push_back(&a, &links, 5u);
    ta_list_push_back(&b, &links, 16u);
    storage_move(&s);
    ta_list_insert_after(&a, &links, (ta_list_insert){ .after = 10u, .item = 2u });
    storage_move(&s);
    ta_list_splice(&a, &links, &b);
    storage_move(&s);
    CHECK(HOLDS(&a, &links, 1u, 10u, 2u, 5u, 16u));
    CHECK(ta_list_unlink(&a, &links, 5u) == TA_OK);
    storage_move(&s);
    CHECK(HOLDS(&a, &links, 1u, 10u, 2u, 16u));
    CHECK(((item *)item_at(&s, 10u))->value == 10u);
    storage_destroy(&s);
}

int main(void) {
    test_empty();
    test_push();
    test_pop_front();
    test_insert_after();
    test_splice();
    test_unlink();
    test_remove_after();
    test_two_fields();
    test_storage_moves();
    /* 10 holds by construction: ta_list_links carries no allocator, and
     * every test above ran without one. */
    if (failures != 0) {
        fprintf(stderr, "list_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("list_test: ok\n");
    return 0;
}
