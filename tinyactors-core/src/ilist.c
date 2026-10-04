#include "internal.h"

/* ta_ilist_slot returns the link field of item `index`. `index` must be a taken item (never 0). */
static uint32_t *ta_ilist_slot(const ta_ilink *link, uint32_t index) {
    return (uint32_t *)((unsigned char *)ta_pool_at(link->pool, index) + link->offset);
}

/* ta_ilink_init: see ilist.h. */
ta_result ta_ilink_init(ta_ilink *link, const ta_pool *pool, size_t offset) {
    if (link == NULL) {
        return TA_ERR_INVALID;
    }
    link->pool = NULL;
    link->offset = 0;
    if (pool == NULL || pool->item_size < sizeof(uint32_t) || (offset & (sizeof(uint32_t) - 1u)) != 0 ||
        offset > pool->item_size - sizeof(uint32_t)) {
        return TA_ERR_INVALID;
    }
    link->pool = pool;
    link->offset = offset;
    return TA_OK;
}

/* ta_ilist_init: see ilist.h. */
void ta_ilist_init(ta_ilist *list) {
    list->head = 0;
    list->tail = 0;
}

/* ta_ilist_is_empty: see ilist.h. */
bool ta_ilist_is_empty(const ta_ilist *list) {
    return list->head == 0;
}

/* ta_ilist_push_back: see ilist.h. The item's link is cleared so a stale value cannot extend the list. */
void ta_ilist_push_back(ta_ilist *list, const ta_ilink *link, uint32_t index) {
    *ta_ilist_slot(link, index) = 0;
    if (list->tail == 0) {
        list->head = index;
    } else {
        *ta_ilist_slot(link, list->tail) = index;
    }
    list->tail = index;
}

/* ta_ilist_push_front: see ilist.h. */
void ta_ilist_push_front(ta_ilist *list, const ta_ilink *link, uint32_t index) {
    *ta_ilist_slot(link, index) = list->head;
    if (list->head == 0) {
        list->tail = index;
    }
    list->head = index;
}

/* ta_ilist_pop_front: see ilist.h. */
uint32_t ta_ilist_pop_front(ta_ilist *list, const ta_ilink *link) {
    uint32_t index = list->head;
    if (index == 0) {
        return 0;
    }
    uint32_t *slot = ta_ilist_slot(link, index);
    list->head = *slot;
    if (list->head == 0) {
        list->tail = 0;
    }
    *slot = 0;
    return index;
}

/* ta_ilist_splice: see ilist.h. Only the two ends are touched, whatever the lengths. */
void ta_ilist_splice(ta_ilist *to, ta_ilist *from, const ta_ilink *link) {
    if (from->head == 0) {
        return;
    }
    if (to->tail == 0) {
        to->head = from->head;
    } else {
        *ta_ilist_slot(link, to->tail) = from->head;
    }
    to->tail = from->tail;
    ta_ilist_init(from);
}

/* ta_ilist_unlink: see ilist.h. The step bound is the pool's extent: a list cannot hold more items than were issued. */
bool ta_ilist_unlink(ta_ilist *list, const ta_ilink *link, uint32_t index) {
    if (index == 0) {
        return false;
    }
    uint32_t bound = ta_pool_extent(link->pool);
    uint32_t previous = 0;
    uint32_t current = list->head;
    for (uint32_t steps = 0; current != 0 && steps < bound; steps += 1u) {
        uint32_t *slot = ta_ilist_slot(link, current);
        if (current == index) {
            if (previous == 0) {
                list->head = *slot;
            } else {
                *ta_ilist_slot(link, previous) = *slot;
            }
            if (list->tail == index) {
                list->tail = previous;
            }
            *slot = 0;
            return true;
        }
        previous = current;
        current = *slot;
    }
    return false;
}

/* ta_ilist_next: see ilist.h. */
uint32_t ta_ilist_next(const ta_ilink *link, uint32_t index) {
    return index == 0 ? 0 : *ta_ilist_slot(link, index);
}
