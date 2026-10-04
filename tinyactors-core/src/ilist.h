#pragma once

#include "types.h"
#include "pool.h"

/*
 * Package ilist is the intrusive index list (building block B1 of
 * docs/proposals/ring-proto): a singly linked list of items of an A5 index
 * pool, linked through one 32-bit field of the item itself, with indexes in
 * place of pointers and 0 meaning "none".
 *
 * What it is for. Later phases of the plan keep many short lists of the same
 * records: the run queue of actors, an actor's deferred and waiting lists,
 * the outgoing list of an input, the owner list of an actor's timers, and
 * the free list of a pool. Each wants the same few operations at O(1), no
 * allocation of its own (a list that allocates cannot be used where
 * allocation is forbidden), and a link that costs four bytes per item and
 * per kind of list, not a node. This unit is that one list, written once.
 *
 * Shape. A ta_ilist is {head, tail}: two indexes, zeroed when empty. Which
 * field of the items carries the links is not stored in the list but named
 * by a ta_ilink, a descriptor {pool, byte offset of a uint32_t}, built once
 * per kind of list (TA_ILINK checks the field's type at compile time and
 * takes the offset from offsetof, so it cannot drift from the struct). The
 * same item may sit on two lists at once if the lists use two different
 * link fields (an actor on the run queue through `next` and on its owner's
 * list through `owner_next`); the operations on one kind never read or write
 * the other kind's field. A list has no length counter: an owner that needs
 * one keeps it beside the list.
 *
 * Lifetimes. The list owns nothing: not the items, not any memory.
 *   - A ta_ilist value has the lifetime of its owner, never its own. It is
 *     embedded by value in the owner: system (the run queue), session (an
 *     actor's lists), input (the outgoing list of a slot record).
 *   - A ta_ilink is a call-lifetime value (a compound literal at the use
 *     site) or system-lifetime when the owner stores it next to the pool it
 *     names. It points at the ta_pool, so it must not outlive it, and the
 *     pool must not move after init (it is embedded in the heap-allocated
 *     system).
 *   - A link field lives and dies with its item, from ta_pool_take to
 *     ta_pool_give (envelope, input, parked: whatever the pool's item is).
 *     It costs 4 bytes per kind of list and nothing else.
 *   - An index is held by a list only between push and pop/unlink. Before
 *     an item is given back to the pool it must be unlinked from every list;
 *     before a pool is trimmed or destroyed every list over it must be empty
 *     or discarded (ta_ilist_init), because an index left on a list would
 *     name freed memory.
 *
 * When it allocates. Never. No function here calls an allocator, so every one
 * may run anywhere, including where an out-of-memory answer cannot be
 * reported. There is no failure result except the validation of a descriptor.
 *
 * Invariants. head == 0 if and only if tail == 0. Walking from head by
 * ta_ilist_next reaches tail and then 0, visits each index once, and visits
 * only taken items. The link of the tail is 0. An item that is on no list of
 * a kind has 0 in that kind's field: ta_pool_take zeroes the pool's own
 * link_offset field, the owner's fresh template keeps its other link fields
 * 0, and pop and unlink leave the item they remove with link 0, so it can be
 * pushed again at once. Splice removes no item: it relinks the tail of one
 * list to the head of the other and keeps the links of every item it moves.
 *
 * Complexity. push_back, push_front, pop_front, splice, is_empty and next
 * are O(1). unlink walks from the head and is O(n) for a list of n items,
 * bounded by ta_pool_extent; the proposal accepts the walk in exchange for
 * not carrying a previous link in every item.
 *
 * Failure behaviour. A bad index is the caller's bug and is not detected,
 * because detecting it would cost O(n) (see below). unlink is the one
 * operation that can be asked about an item that is not there, and it
 * answers false and changes nothing; its walk takes at most ta_pool_extent
 * steps, so even a corrupted, cyclic list ends instead of looping forever.
 *
 * Misuse that is not checked: pushing 0, an index that was never issued, an
 * item that was given, or an item already on a list of this kind (it would
 * corrupt both lists silently); splicing a list into itself; two lists of
 * the same kind that share items; using a ta_ilink whose pool was destroyed
 * or moved.
 *
 * Usage, a run queue of actors that also sit on their owner's list:
 *
 *	typedef struct { uint32_t next; uint32_t owner_next; ... } slot;
 *	// pool: a ta_pool of slot; its link_offset may be offsetof(slot, next)
 *	// because an item is never both free and listed.
 *	ta_ilink run = TA_ILINK(&pool, slot, next);
 *	ta_ilink owned = TA_ILINK(&pool, slot, owner_next);
 *	ta_ilist queue = { 0 }, mine = { 0 };       // embedded in their owners
 *	uint32_t i = ta_pool_take(&pool);
 *	ta_ilist_push_back(&queue, &run, i);        // O(1), no allocation
 *	ta_ilist_push_back(&mine, &owned, i);       // the same item, a second list
 *	...
 *	i = ta_ilist_pop_front(&queue, &run);       // 0 when empty
 *	if (ta_ilist_unlink(&mine, &owned, i)) {    // off the other list too
 *		ta_pool_give(&pool, i);                 // only now may it be given
 *	}
 */

/* ta_ilist is one list: the first and the last index, both 0 when empty. A zeroed value is an empty list.
 * Lifetime: that of its owner, in whose struct it is embedded by value. */
typedef struct ta_ilist {
    uint32_t head; /* first index, 0 when empty */
    uint32_t tail; /* last index, 0 when empty; head == 0 iff tail == 0 */
} ta_ilist;

/*
 * ta_ilink names one link field of the items of one pool: the lens through
 * which a list reaches the next index of an item. Build it with TA_ILINK (or
 * ta_ilink_init when the pool and offset are only known at run time).
 * Lifetime: call, or system when stored next to the pool; it must not outlive
 * the pool it points at.
 */
typedef struct ta_ilink {
    const ta_pool *pool;
    size_t offset; /* byte offset of a uint32_t in the item; a multiple of 4 with offset + 4 <= item size */
} ta_ilink;

/*
 * TA_ILINK(pool, type, field) is a ta_ilink value for `field` of `type`, in
 * the items of `pool` (a const ta_pool *). It is an lvalue (a compound
 * literal), so `&TA_ILINK(...)` can be passed to the functions below. `field`
 * must be a uint32_t member of `type`: any other type is a compile-time error
 * (the _Generic selects a negative array size). The offset is offsetof, so it
 * follows the struct. C cannot check that `type` is what the pool stores;
 * ta_ilink_init checks the offset against the pool's item size at run time.
 */
#define TA_ILINK(pool, type, field) \
    ((ta_ilink){ (pool), \
                 offsetof(type, field) + 0 * sizeof(char[_Generic(((type *)0)->field, uint32_t : 1, default : -1)]) })

/*
 * ta_ilink_init fills *link with a descriptor for the uint32_t at byte
 * `offset` of the items of `pool`, after checking it: the pool is
 * initialised, the offset is a multiple of 4, and offset + 4 fits the item
 * size (written as offset <= item_size - 4, which cannot overflow). It
 * answers TA_ERR_INVALID for a NULL argument, a pool that is not initialised
 * or a bad offset, and then leaves *link zeroed. It never allocates. It reads
 * the pool's item_size directly, a field pool.h reserves for pool.c, because
 * the pool has no accessor for it.
 */
ta_result ta_ilink_init(ta_ilink *link, const ta_pool *pool, size_t offset);

/* ta_ilist_init makes *list empty. It is idempotent, and it is how an owner discards a list (without touching
 * its items) before the pool the list is over dies. It never allocates. */
void ta_ilist_init(ta_ilist *list);

/* ta_ilist_is_empty reports whether the list has no item. It never allocates. O(1). */
bool ta_ilist_is_empty(const ta_ilist *list);

/*
 * ta_ilist_push_back makes the taken item `index` the last of the list. The
 * item must not be on a list of this link kind. Only the link of the old tail
 * and of the item are written. It never allocates. O(1).
 */
void ta_ilist_push_back(ta_ilist *list, const ta_ilink *link, uint32_t index);

/*
 * ta_ilist_push_front makes the taken item `index` the first of the list,
 * under the same rules as ta_ilist_push_back. It never allocates. O(1).
 */
void ta_ilist_push_front(ta_ilist *list, const ta_ilink *link, uint32_t index);

/*
 * ta_ilist_pop_front removes the first item and returns its index, with its
 * link cleared to 0 so it can be pushed again; it returns 0 and changes
 * nothing for an empty list. It never allocates. O(1).
 */
uint32_t ta_ilist_pop_front(ta_ilist *list, const ta_ilink *link);

/*
 * ta_ilist_splice appends every item of `from` to the end of `to`, in order,
 * and leaves `from` empty. Both lists must use `link` and must be different
 * lists. An empty `from` is a no-op; an empty `to` takes over `from`. No item
 * is visited, only the two ends are written. It never allocates. O(1).
 */
void ta_ilist_splice(ta_ilist *to, ta_ilist *from, const ta_ilink *link);

/*
 * ta_ilist_unlink removes `index` from the list wherever it is (head,
 * middle, tail, only item) and returns true, clearing its link to 0; when
 * the index is not on the list it returns false and changes nothing. It walks
 * from the head and stops after ta_pool_extent steps, so a corrupted cyclic
 * list ends instead of looping forever. It never allocates. O(n) in the
 * length of the list, which is at most the pool's extent.
 */
bool ta_ilist_unlink(ta_ilist *list, const ta_ilink *link, uint32_t index);

/*
 * ta_ilist_next returns the index after `index` in its list, 0 at the end
 * (and 0 for index 0). A walker that may unlink the item it stands on reads
 * next first. The item must be taken. It never allocates. O(1).
 */
uint32_t ta_ilist_next(const ta_ilink *link, uint32_t index);
