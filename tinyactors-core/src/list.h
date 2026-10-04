/**
 * @file list.h
 *
 * # Intrusive index list
 *
 * `ta_list` is a singly linked list of items addressed by `uint32_t`
 * index, `0` meaning none. It is only `{head, tail}`: the links live in
 * the items themselves, in a `uint32_t` field the caller chooses. One item
 * can sit on two lists at once when they link through different fields,
 * as an envelope does: `next` for the run queue, a deferred, waiting or
 * outgoing list or the pool's free list, and `owner_next` for its
 * sender's owner list of pending delayed sends.
 *
 * ## Reaching an item's link: `ta_list_links`
 *
 * Items live in an index pool's pages, which never move but are not
 * one array, so the list cannot compute an item's address from a base
 * and a stride. A `ta_list_links` names how to reach a link instead:
 *
 * - `item(items, index)` answers the address of item `index` (never 0) in
 *   the storage `items`. An index pool provides this function once;
 * - `offset` is the byte offset of the `uint32_t` link field inside the
 *   item (`offsetof(ta_envelope, next)` or `offsetof(ta_envelope,
 *   owner_next)`), so one storage serves lists over either field.
 *
 * The list keeps no pointer: it asks `item` again at every step and uses
 * the address only within that call. Storage may therefore grow between
 * calls; within a call it must not move, which holds because these
 * functions never allocate.
 *
 * Every operation that reads or writes a link takes the `ta_list_links`
 * of the field the list is linked through. Using different links for the
 * same list is a programmer error.
 *
 * ## Invariants
 *
 * - `head == 0` exactly when `tail == 0`: the list is empty.
 * - Following links from `head` reaches `tail` after each item once, and
 *   the tail's link is 0.
 * - An item is on at most one list per link field at a time. Pushing or
 *   inserting an item that is already on a list through the same field is
 *   a programmer error, not checked: it corrupts both lists.
 * - Removing an item (`pop_front`, `unlink`) sets its link to 0. A link of
 *   0 does not show that an item is unlinked, since the tail's is 0 too:
 *   the owner of the item tracks which list holds it (the envelope's
 *   state).
 *
 * ## Memory
 *
 * The list uses no memory of its own and never allocates: no function
 * here takes an allocator. A `ta_list` is 8 bytes the caller embeds where
 * it likes (an actor record, a slot record, a table value). The items and
 * their storage belong to the caller (the envelope pool), and the list
 * borrows them only for the length of a call. Every function here may run
 * inside a macrostep.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_list_push_back`, `ta_list_push_front`, `ta_list_pop_front` | O(1) |
 * | `ta_list_insert_after` | O(1) |
 * | `ta_list_splice` (append a whole list) | O(1) |
 * | `ta_list_remove_after` | O(1) |
 * | `ta_list_unlink(i)` | walks the list from its head to `i`: O(length), bounded by the pool |
 * | `ta_list_head`, `ta_list_tail`, `ta_list_empty`, `ta_list_next` | O(1) |
 *
 * Each step that touches an item costs one call to `links->item`.
 *
 * ## Failure
 *
 * Only `ta_list_unlink` can fail: it answers `TA_ERR_NOT_FOUND`, and
 * changes nothing, when the item is not on the list. Everything else is a
 * documented precondition and not checked at run time: an index of 0
 * where an item is required, an item already on a list through the same
 * field, an `after` that is not on the list.
 *
 * ## Users
 *
 * One block serves every list of the envelope pool:
 *
 * - the pool's **free list**;
 * - the **run queue**, and a macrostep's **outgoing list**, spliced onto
 *   the run queue at the macrostep's end;
 * - the **owner list** of a sender's pending delayed sends, walked by
 *   `<cancel>` and unloading;
 * - a session's **deferred list**, whose first envelope `insert_after`
 *   returns to the run queue at the position its arrival number gives it;
 * - the **waiting list** of an address that is being loaded, its head
 *   kept in a hash table keyed by the address.
 *
 * A walk that removes the item it chooses (filling a slot takes the first
 * envelope whose session is not active) keeps the item before it and
 * calls `ta_list_remove_after`, so it walks once.
 */
#pragma once

#include "types.h"

/** A list of item indices: `{0, 0}` is empty. */
typedef struct {
    uint32_t head;
    uint32_t tail;
} ta_list;

/** How to reach the link field of an item; see the file comment. */
typedef struct {
    /** The address of item `index` (never 0) in `items`, valid until the
     * storage next grows. */
    void *(*item)(void *items, uint32_t index);
    /** The storage `item` reads, borrowed for each call. */
    void *items;
    /** Byte offset of the item's `uint32_t` link field. */
    size_t offset;
} ta_list_links;

/** Where `ta_list_insert_after` puts `item`: after `after`, or at the
 * front when `after` is 0. */
typedef struct {
    uint32_t after;
    uint32_t item;
} ta_list_insert;

/** The empty list. */
#define TA_LIST_EMPTY ((ta_list){ .head = 0u, .tail = 0u })

/** The link field of item `index` (never 0). Valid within the calling
 * function only. */
static inline uint32_t *ta_list_link(const ta_list_links *links, uint32_t index) {
    return (uint32_t *)((unsigned char *)links->item(links->items, index) + links->offset);
}

/** Whether the list holds no item. */
static inline bool ta_list_empty(const ta_list *list) { return list->head == 0u; }

/** The first item, or 0 when the list is empty. */
static inline uint32_t ta_list_head(const ta_list *list) { return list->head; }

/** The last item, or 0 when the list is empty. */
static inline uint32_t ta_list_tail(const ta_list *list) { return list->tail; }

/**
 * The item after `index` (on a list), or 0 when `index` is the last. With
 * `ta_list_head` it walks a list in order:
 * `for (i = ta_list_head(l); i != 0; i = ta_list_next(links, i))`.
 * The walk must not remove the item it stands on before taking its next.
 * Never allocates; may run inside a macrostep.
 */
static inline uint32_t ta_list_next(const ta_list_links *links, uint32_t index) {
    return *ta_list_link(links, index);
}

/**
 * Appends `item`, which is on no list through this field, at the tail.
 * O(1). Never allocates; may run inside a macrostep. Cannot fail.
 */
static inline void ta_list_push_back(ta_list *list, const ta_list_links *links, uint32_t item) {
    *ta_list_link(links, item) = 0u;
    if (list->tail == 0u) {
        list->head = item;
    } else {
        *ta_list_link(links, list->tail) = item;
    }
    list->tail = item;
}

/**
 * Puts `item`, which is on no list through this field, at the head.
 * O(1). Never allocates; may run inside a macrostep. Cannot fail.
 */
static inline void ta_list_push_front(ta_list *list, const ta_list_links *links, uint32_t item) {
    *ta_list_link(links, item) = list->head;
    list->head = item;
    if (list->tail == 0u) {
        list->tail = item;
    }
}

/**
 * Removes the first item and answers it, with its link set to 0, or
 * answers 0 and changes nothing when the list is empty. O(1). Never
 * allocates; may run inside a macrostep.
 */
static inline uint32_t ta_list_pop_front(ta_list *list, const ta_list_links *links) {
    uint32_t item = list->head;
    if (item == 0u) {
        return 0u;
    }
    uint32_t *link = ta_list_link(links, item);
    list->head = *link;
    *link = 0u;
    if (list->head == 0u) {
        list->tail = 0u;
    }
    return item;
}

/**
 * Inserts `at.item`, which is on no list through this field, right after
 * `at.after`, which is on this list; when `at.after` is 0 it goes to the
 * front, as `ta_list_push_front`. This returns a deferred envelope to the
 * run queue at its arrival position: the caller finds the last envelope
 * that arrived before it (or none) and inserts after that. O(1). Never
 * allocates; may run inside a macrostep. Cannot fail.
 */
static inline void ta_list_insert_after(ta_list *list, const ta_list_links *links, ta_list_insert at) {
    if (at.after == 0u) {
        ta_list_push_front(list, links, at.item);
        return;
    }
    uint32_t *after_link = ta_list_link(links, at.after);
    *ta_list_link(links, at.item) = *after_link;
    *after_link = at.item;
    if (list->tail == at.after) {
        list->tail = at.item;
    }
}

/**
 * Appends every item of `source` to the tail of `list`, in order, and
 * leaves `source` empty. Both lists link through the same field, and
 * `source` is not `list`. A macrostep's outgoing list joins the run queue
 * this way at the macrostep's end. O(1): it touches only `list`'s tail
 * item. Never allocates; may run inside a macrostep. Cannot fail.
 */
static inline void ta_list_splice(ta_list *list, const ta_list_links *links, ta_list *source) {
    if (source->head == 0u) {
        return;
    }
    if (list->tail == 0u) {
        list->head = source->head;
    } else {
        *ta_list_link(links, list->tail) = source->head;
    }
    list->tail = source->tail;
    *source = TA_LIST_EMPTY;
}

/**
 * Removes the item after `previous`, which is on this list, or the head
 * when `previous` is 0, sets its link to 0 and answers it. Answers 0 and
 * changes nothing when there is no such item (`previous` is the tail, or
 * the list is empty). A walk that keeps the item before the one it stands
 * on removes that item this way without walking again. O(1). Never
 * allocates; may run inside a macrostep.
 */
static inline uint32_t ta_list_remove_after(ta_list *list, const ta_list_links *links, uint32_t previous) {
    if (previous == 0u) {
        return ta_list_pop_front(list, links);
    }
    uint32_t *previous_link = ta_list_link(links, previous);
    uint32_t item = *previous_link;
    if (item == 0u) {
        return 0u;
    }
    uint32_t *link = ta_list_link(links, item);
    *previous_link = *link;
    *link = 0u;
    if (list->tail == item) {
        list->tail = previous;
    }
    return item;
}

/**
 * Removes `item` from wherever it is on the list and sets its link to 0.
 * It walks the list from the head to find the item before it: O(length),
 * bounded by the pool, which `<cancel>` on an owner list relies on. Never
 * allocates; may run inside a macrostep.
 *
 * Answers `TA_OK`, or `TA_ERR_NOT_FOUND` and changes nothing when `item`
 * is not on the list (0 is never on a list).
 */
static inline ta_result ta_list_unlink(ta_list *list, const ta_list_links *links, uint32_t item) {
    uint32_t previous = 0u;
    uint32_t current = list->head;
    while (current != 0u && current != item) {
        previous = current;
        current = *ta_list_link(links, current);
    }
    if (current == 0u) {
        return TA_ERR_NOT_FOUND;
    }
    uint32_t *link = ta_list_link(links, item);
    if (previous == 0u) {
        list->head = *link;
    } else {
        *ta_list_link(links, previous) = *link;
    }
    if (list->tail == item) {
        list->tail = previous;
    }
    *link = 0u;
    return TA_OK;
}
