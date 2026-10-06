/*
 * Implementation of the envelope pool. The contract, the lifetimes, when it
 * allocates and the invariants (I1 to I4) are in envelope.h; this comment
 * records what the code relies on and why it is ordered as it is.
 *
 * Allocation. This file never calls the allocator. Every byte comes from the
 * blocks underneath (ta_pool for pages and its directory, ta_table for the
 * waiting table), which size with ta_layout and free with the sizes they
 * stored. The one function that reaches them is ta_envelopes_refill.
 *
 * Why the table is built before the pool grows. A refill must be all or
 * nothing. The pool can be undone only by trimming, which may also give back
 * older free pages, so the pool is the last step: the new table is built
 * first, for the capacity the pool will have (the pool's own page arithmetic,
 * repeated in ta_envelopes_target), and thrown away if the pool refuses. Once
 * the pool has grown nothing below can fail: the old table is destroyed and
 * the new one installed.
 *
 * Why the table is fixed and re-created rather than grown. A fixed table never
 * allocates at insert, which keeps routing out of the allocator
 * (invariant I4). Its slots are sized for the pool's capacity, so even if
 * every envelope waited for a different address there would be a slot. The
 * cost is one table of about 2 slots (8 bytes each) per envelope, paid when
 * the pool grows, which is the ADR's "about 8 bytes per envelope".
 *
 * The waiting list layout. The table stores, for each address with waiters,
 * the first envelope of its list. The first carries `waiting_tail`, so an
 * append is O(1) without a list header per address; the others are chained
 * through `next`. The key is the interned address pointer (one record per
 * address text), compared after the table's candidate probe, so two addresses
 * with equal hashes stay distinct.
 */

#include "internal.h"

/* The largest page of envelopes: 256 of them are 43 KiB, one allocation. */
#define TA_ENVELOPE_PAGE_ITEMS 256u

/* ta_envelopes_page_items answers the largest power of two that is at most min(256, bound). bound >= 1. */
static uint32_t ta_envelopes_page_items(uint32_t pool_envelopes) {
    uint32_t page = 1u;
    while (page < TA_ENVELOPE_PAGE_ITEMS && page <= pool_envelopes / 2u) {
        page *= 2u;
    }
    return page;
}

/*
 * ta_envelopes_init sets up the envelope pool of one system (lifetime:
 * system; the block lives inside ta_system). It needs `envelopes` to be
 * storage the caller will not move afterwards, because the block holds
 * pointers into itself (the pool and both link views name each other), and
 * valid `limits`: a pool of at least one envelope, a watermark of 1..99
 * percent, and a macrostep reserve of 1..pool_envelopes.
 *
 * The pool bound is `pool_envelopes` rounded down to a whole number of pages
 * (all arithmetic is checked inside ta_pool_init). Init allocates nothing: the
 * pool and the waiting table are empty until ta_envelopes_refill runs at a
 * safe point.
 *
 * Answers TA_ERR_INVALID for a NULL argument or limits outside the ranges
 * above, and whatever ta_pool_init or ta_ilink_init answers for a size that
 * overflows. On every failure `*envelopes` is left zeroed and nothing is
 * held, so a failed init needs no destroy.
 */
ta_result ta_envelopes_init(ta_envelopes *envelopes, ta_allocator allocator, const ta_envelope_limits *limits) {
    if (envelopes == NULL) {
        return TA_ERR_INVALID;
    }
    *envelopes = (ta_envelopes){0};
    if (limits == NULL || limits->pool_envelopes == 0 || limits->admission_watermark == 0 ||
        limits->admission_watermark > 99u || limits->macrostep_reserve == 0 ||
        limits->macrostep_reserve > limits->pool_envelopes) {
        return TA_ERR_INVALID;
    }
    uint32_t page = ta_envelopes_page_items(limits->pool_envelopes);
    uint32_t bound = limits->pool_envelopes / page * page;
    ta_pool_params params = { .item_size = sizeof(ta_envelope),
                              .item_alignment = _Alignof(ta_envelope),
                              .link_offset = offsetof(ta_envelope, next),
                              .page_items = page,
                              .max_items = bound };
    ta_result result = ta_pool_init(&envelopes->pool, allocator, &params);
    if (result != TA_OK) {
        *envelopes = (ta_envelopes){0};
        return result;
    }
    result = ta_ilink_init(&envelopes->link, &envelopes->pool, offsetof(ta_envelope, next));
    if (result == TA_OK) {
        result = ta_ilink_init(&envelopes->owner_link, &envelopes->pool, offsetof(ta_envelope, owner_next));
    }
    if (result != TA_OK) {
        ta_pool_destroy(&envelopes->pool);
        *envelopes = (ta_envelopes){0};
        return result;
    }
    envelopes->allocator = allocator;
    envelopes->bound = bound;
    envelopes->admission_watermark = limits->admission_watermark;
    envelopes->macrostep_reserve = limits->macrostep_reserve;
    ta_quota_init(&envelopes->quota, bound);
    ta_ilist_init(&envelopes->run);
    return TA_OK;
}

/*
 * ta_envelopes_destroy ends the pool's lifetime. Every envelope still in use
 * gives back what it owns: its event data, its origin, and the address of one
 * that waits or of a refused host input that keeps its reference for its
 * notice. Then the waiting table and the pool's pages are freed, each with
 * the size it was allocated with, and the block is zeroed.
 *
 * It must run while the address table is alive (an address is a core handle
 * and releasing it touches the table), so ta_system_destroy calls it before
 * the address table goes. Calling it on a zeroed block, or twice, does
 * nothing. It never allocates and is O(pool extent).
 */
void ta_envelopes_destroy(ta_envelopes *envelopes) {
    uint32_t extent = ta_pool_extent(&envelopes->pool);
    for (uint32_t index = 1; index <= extent; index += 1u) {
        ta_envelope *envelope = ta_envelope_at(envelopes, index);
        if (envelope->state == TA_ENVELOPE_FREE) {
            continue;
        }
        ta_event_release(&envelope->event);
        if (envelope->payload.address != NULL) {
            /* Whatever the state: a refused host input keeps its reference for its notice. */
            ta_address_release(envelope->payload.address);
        }
    }
    ta_table_destroy(&envelopes->waiting);
    ta_pool_destroy(&envelopes->pool);
    *envelopes = (ta_envelopes){0};
}

/*
 * ta_envelopes_target answers the capacity the pool will have after a refill
 * that leaves `want` envelopes free, which is ta_pool_refill's arithmetic: whole
 * pages, enough that capacity - used >= want. `want` is already cut to
 * bound - used, so the result is at most the bound (a multiple of the page).
 */
static uint32_t ta_envelopes_target(const ta_envelopes *envelopes, uint32_t want) {
    uint64_t page = (uint64_t)envelopes->pool.page_mask + 1u;
    uint64_t free_items = (uint64_t)envelopes->pool.capacity - envelopes->pool.used;
    uint64_t added = ((uint64_t)want - free_items + page - 1u) / page;
    return (uint32_t)((uint64_t)envelopes->pool.capacity + added * page);
}

/*
 * ta_envelopes_table_build makes a waiting table for `capacity` envelopes and
 * moves every entry of the current one into it. It changes nothing that
 * exists: the caller installs the result after the pool has grown, or
 * destroys it.
 */
static ta_result ta_envelopes_table_build(const ta_envelopes *envelopes, uint32_t capacity, ta_table *out) {
    ta_table_params params = { .mode = TA_TABLE_FIXED, .max_entries = capacity };
    ta_result result = ta_table_init(out, envelopes->allocator, &params);
    if (result != TA_OK) {
        return result;
    }
    ta_table_cursor cursor = {0};
    uint32_t hash = 0;
    uint32_t value = 0;
    while (ta_table_next(&envelopes->waiting, &cursor, &hash, &value)) {
        result = ta_table_insert(out, hash, value); /* cannot fail: capacity >= the old capacity (I4) */
        if (result != TA_OK) {
            ta_table_destroy(out);
            return result;
        }
    }
    return TA_OK;
}

/*
 * ta_envelopes_refill grows the pool, at a safe point, so that `want_free`
 * envelopes are free, and replaces the waiting table with one that can hold
 * an entry for every envelope the pool can then have. That is what lets a
 * later ta_mail_post_waiting never allocate (envelope.h I4).
 *
 * `want_free` is first cut to bound - used: the pool never exceeds its
 * bound, so asking for more than can exist is not an error, it just asks for
 * all of it. Reservations are not counted in `used`: a claim is backed by
 * items that exist (see ta_run_fill), and the callers add what they promise
 * to `want_free` themselves. If enough items are free already the call
 * returns TA_OK without allocating.
 *
 * Failure leaves no partial state: the new table is built first (nothing
 * existing changes), the pool grows second, and if that fails the new table
 * is destroyed and the old one stays. Only after both succeed is the old
 * table destroyed and the new installed. Answers TA_ERR_NOMEM (or the
 * allocator's answer) on failure. Cost is O(waiting entries) when it grows.
 */
ta_result ta_envelopes_refill(ta_envelopes *envelopes, uint32_t want_free) {
    uint32_t room = envelopes->bound - envelopes->pool.used;
    uint32_t want = want_free < room ? want_free : room;
    if (want <= envelopes->pool.capacity - envelopes->pool.used) {
        return TA_OK;
    }
    ta_table table;
    ta_result result = ta_envelopes_table_build(envelopes, ta_envelopes_target(envelopes, want), &table);
    if (result != TA_OK) {
        return result;
    }
    result = ta_pool_refill(&envelopes->pool, want);
    if (result != TA_OK) {
        ta_table_destroy(&table);
        return result;
    }
    ta_table_destroy(&envelopes->waiting);
    envelopes->waiting = table;
    return TA_OK;
}

/* ta_envelopes_admissible: see envelope.h. */
bool ta_envelopes_admissible(const ta_envelopes *envelopes) {
    return (uint64_t)envelopes->pool.used * 100u < (uint64_t)envelopes->bound * envelopes->admission_watermark;
}

/* ta_envelope_make: see envelope.h. */
uint32_t ta_envelope_make(ta_envelopes *envelopes, ta_quota_claim *claim, ta_envelope_state state) {
    uint32_t index = ta_pool_take(&envelopes->pool);
    if (index == 0) {
        return 0;
    }
    ta_quota_claim one = {0};
    ta_quota_claim *payer = claim != NULL ? claim : &one;
    if (claim == NULL && !ta_quota_reserve(&envelopes->quota, &one, 1u)) {
        ta_pool_give(&envelopes->pool, index);
        return 0;
    }
    if (!ta_quota_consume(&envelopes->quota, payer, 1u)) {
        ta_pool_give(&envelopes->pool, index);
        return 0;
    }
    ta_envelope *envelope = ta_envelope_at(envelopes, index);
    *envelope = (ta_envelope){0};
    envelope->state = (uint8_t)state;
    return index;
}

/* ta_envelope_at: see envelope.h. */
ta_envelope *ta_envelope_at(const ta_envelopes *envelopes, uint32_t index) {
    return (ta_envelope *)ta_pool_at(&envelopes->pool, index);
}

/* ta_envelope_release: see envelope.h. */
void ta_envelope_release(ta_envelopes *envelopes, uint32_t index) {
    ta_envelope *envelope = ta_envelope_at(envelopes, index);
    if (envelope == NULL || envelope->state == TA_ENVELOPE_FREE) {
        return;
    }
    ta_event_release(&envelope->event);
    if (envelope->payload.address != NULL) {
        /* A waiting envelope's reference, or the one a refused host input keeps for its notice. */
        ta_address_release(envelope->payload.address);
    }
    *envelope = (ta_envelope){0}; /* state FREE; the free link is written by give */
    (void)ta_quota_release(&envelopes->quota, 1u);
    ta_pool_give(&envelopes->pool, index);
}

/* ta_envelopes_reserve: see envelope.h. */
uint32_t ta_envelopes_reserve(ta_envelopes *envelopes, ta_quota_claim *claim) {
    uint64_t available = ta_quota_available(&envelopes->quota);
    uint32_t n = available < envelopes->macrostep_reserve ? (uint32_t)available : envelopes->macrostep_reserve;
    (void)ta_quota_reserve(&envelopes->quota, claim, n); /* n <= available, so it succeeds */
    return n;
}

/* ta_envelopes_unreserve: see envelope.h. */
uint32_t ta_envelopes_unreserve(ta_envelopes *envelopes, ta_quota_claim *claim) {
    return (uint32_t)ta_quota_cancel_all(&envelopes->quota, claim);
}

/* ta_envelopes_run_push: see envelope.h. */
void ta_envelopes_run_push(ta_envelopes *envelopes, uint32_t index) {
    ta_envelope_at(envelopes, index)->state = TA_ENVELOPE_RUNNABLE;
    ta_ilist_push_back(&envelopes->run, &envelopes->link, index);
}

/* ta_envelopes_run_pop: see envelope.h. */
uint32_t ta_envelopes_run_pop(ta_envelopes *envelopes) {
    return ta_ilist_pop_front(&envelopes->run, &envelopes->link);
}

/* ta_envelopes_run_splice: see envelope.h. */
void ta_envelopes_run_splice(ta_envelopes *envelopes, ta_ilist *outgoing) {
    ta_ilist_splice(&envelopes->run, outgoing, &envelopes->link);
}

/* ta_envelopes_link: see envelope.h. */
const ta_ilink *ta_envelopes_link(const ta_envelopes *envelopes) {
    return &envelopes->link;
}

/* ta_envelopes_owner_link: see envelope.h. */
const ta_ilink *ta_envelopes_owner_link(const ta_envelopes *envelopes) {
    return &envelopes->owner_link;
}

/*
 * ta_waiting_find answers the first envelope of the waiting list of `address`,
 * or 0. The table yields every entry with the address's hash; the interned
 * pointer decides which is the address.
 */
static uint32_t ta_waiting_find(const ta_envelopes *envelopes, const ta_address *address, uint32_t hash) {
    ta_table_probe probe;
    uint32_t head = 0;
    ta_table_probe_start(&envelopes->waiting, hash, &probe);
    while (ta_table_probe_next(&probe, &head)) {
        const ta_envelope *envelope = ta_envelope_at(envelopes, head);
        if (envelope->payload.address == address) {
            return head;
        }
    }
    return 0;
}

/* ta_waiting_insert: see envelope.h. */
ta_result ta_waiting_insert(ta_envelopes *envelopes, uint32_t index, const ta_waiting_put *put) {
    ta_envelope *envelope = ta_envelope_at(envelopes, index);
    if (put == NULL) {
        return TA_ERR_INVALID;
    }
    ta_address *address = put->address;
    bool *first = put->first;
    if (envelope == NULL || address == NULL || first == NULL || envelope->state == TA_ENVELOPE_FREE ||
        envelope->state == TA_ENVELOPE_WAITING) {
        return TA_ERR_INVALID;
    }
    uint32_t hash = ta_address_record_hash(address);
    uint32_t head = ta_waiting_find(envelopes, address, hash);
    if (head == 0) {
        ta_result result = ta_table_insert(&envelopes->waiting, hash, index);
        if (result != TA_OK) {
            return result;
        }
    }
    ta_address_retain(address);
    envelope->state = TA_ENVELOPE_WAITING;
    envelope->payload.address = address;
    envelope->next = 0;
    if (head == 0) {
        envelope->waiting_tail = index;
        *first = true;
        return TA_OK;
    }
    ta_envelope *first_envelope = ta_envelope_at(envelopes, head);
    ta_envelope_at(envelopes, first_envelope->waiting_tail)->next = index;
    first_envelope->waiting_tail = index;
    *first = false;
    return TA_OK;
}

/* ta_waiting_length: see envelope.h. */
uint32_t ta_waiting_length(const ta_envelopes *envelopes, const ta_address *address) {
    if (address == NULL) {
        return 0;
    }
    uint32_t length = 0;
    uint32_t bound = ta_pool_extent(&envelopes->pool);
    for (uint32_t index = ta_waiting_find(envelopes, address, ta_address_record_hash(address));
         index != 0 && length < bound; index = ta_ilist_next(&envelopes->link, index)) {
        length += 1u;
    }
    return length;
}

/* ta_waiting_pending: see envelope.h. */
uint32_t ta_waiting_pending(const ta_envelopes *envelopes, const ta_address *address) {
    if (address == NULL) {
        return 0;
    }
    uint32_t count = 0;
    uint32_t steps = 0;
    uint32_t bound = ta_pool_extent(&envelopes->pool);
    for (uint32_t index = ta_waiting_find(envelopes, address, ta_address_record_hash(address));
         index != 0 && steps < bound; index = ta_ilist_next(&envelopes->link, index)) {
        count += (ta_envelope_at(envelopes, index)->flags & TA_ENVELOPE_WITHDRAWN) == 0 ? 1u : 0u;
        steps += 1u;
    }
    return count;
}

/* ta_waiting_take_list: see envelope.h. */
uint32_t ta_waiting_take_list(ta_envelopes *envelopes, const ta_address *address) {
    if (address == NULL) {
        return 0;
    }
    uint32_t hash = ta_address_record_hash(address);
    uint32_t head = ta_waiting_find(envelopes, address, hash);
    if (head == 0) {
        return 0;
    }
    (void)ta_table_remove(&envelopes->waiting, hash, head);
    ta_envelope_at(envelopes, head)->waiting_tail = 0;
    return head;
}
