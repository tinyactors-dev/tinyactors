#include "envelope.h"
#include "../include/tinyactors.h"
#include "index_pool.h"
#include "list.h"
#include "memory.h"
#include "quota.h"

ta_result ta_envelope_pool_init(ta_envelope_pool *pool, const ta_envelope_pool_params *params) {
    if (params->bound < TA_ENVELOPE_POOL_MIN || params->bound > TA_ENVELOPE_POOL_MAX ||
        params->watermark_percent < 1u || params->watermark_percent > 99u || params->reserve < 1u ||
        params->reserve > params->bound) {
        return TA_ERR_INVALID;
    }
    ta_index_pool items;
    ta_result result = ta_index_pool_init(&items, &(ta_index_pool_params){
                                                      .allocator = params->allocator,
                                                      .item_size = sizeof(ta_envelope),
                                                      .item_alignment = _Alignof(ta_envelope),
                                                      .link_offset = offsetof(ta_envelope, next),
                                                      .page_shift = TA_ENVELOPE_PAGE_SHIFT,
                                                      .ceiling = params->bound,
                                                  });
    if (result != TA_OK) {
        return result;
    }
    uint32_t watermark = (uint32_t)((uint64_t)params->bound * params->watermark_percent / 100u);
    *pool = (ta_envelope_pool){
        .items = items,
        .quota = ta_quota_make(params->bound),
        .run_queue = TA_LIST_EMPTY,
        .arrivals = 0u,
        .watermark = watermark == 0u ? 1u : watermark,
        .reserve = params->reserve,
    };
    return TA_OK;
}

void ta_envelope_pool_destroy(ta_envelope_pool *pool) {
    ta_index_pool_destroy(&pool->items);
    pool->quota = ta_quota_make(pool->quota.capacity);
    pool->run_queue = TA_LIST_EMPTY;
}

ta_result ta_envelope_pool_refill(ta_envelope_pool *pool) {
    uint32_t available = ta_quota_available(&pool->quota);
    uint32_t min_free = pool->reserve < available ? pool->reserve : available;
    /* The ceiling is the quota's capacity, so `used + min_free` never passes it: only NOMEM remains. */
    return ta_index_pool_refill(&pool->items, min_free);
}

void ta_envelope_pool_trim(ta_envelope_pool *pool) {
    ta_index_pool_trim(&pool->items, pool->reserve);
}

/* Takes an envelope when the quota grants `request`; see take and admit. */
static ta_result ta_envelope_pool_take_within(ta_envelope_pool *pool, ta_quota_request request, uint32_t *out) {
    if (pool->items.free == 0u || !ta_quota_fits(&pool->quota, request)) {
        return TA_ERR_FULL;
    }
    uint32_t index = 0u;
    (void)ta_index_pool_take(&pool->items, &index); /* cannot fail: an envelope is free */
    (void)ta_quota_take(&pool->quota, request);     /* cannot fail: it fits */
    ta_mem_zero(ta_envelope_pool_at(pool, index), sizeof(ta_envelope));
    *out = index;
    return TA_OK;
}

ta_result ta_envelope_pool_take(ta_envelope_pool *pool, uint32_t *out) {
    return ta_envelope_pool_take_within(pool, (ta_quota_request){ .amount = 1u, .keep = 0u }, out);
}

/* The quota request of one admission: one envelope, keeping all room above the watermark. */
static ta_quota_request ta_envelope_pool_admission(const ta_envelope_pool *pool) {
    return (ta_quota_request){ .amount = 1u, .keep = pool->quota.capacity - pool->watermark };
}

ta_result ta_envelope_pool_admit(ta_envelope_pool *pool, uint32_t *out) {
    return ta_envelope_pool_take_within(pool, ta_envelope_pool_admission(pool), out);
}

bool ta_envelope_pool_admitting(const ta_envelope_pool *pool) {
    return ta_quota_fits(&pool->quota, ta_envelope_pool_admission(pool));
}

void ta_envelope_pool_give(ta_envelope_pool *pool, uint32_t index) {
    ta_envelope_pool_at(pool, index)->state = TA_ENVELOPE_FREE;
    ta_index_pool_give(&pool->items, index);
    (void)ta_quota_release(&pool->quota, 1u); /* cannot fail: the envelope was in use */
}

/* Gives `envelope` the next arrival number and marks it runnable. */
static void ta_envelope_pool_arrive(ta_envelope_pool *pool, ta_envelope *envelope) {
    envelope->heap_index = pool->arrivals;
    envelope->state = TA_ENVELOPE_RUNNABLE;
    pool->arrivals += 1u;
}

void ta_envelope_pool_enqueue(ta_envelope_pool *pool, uint32_t index) {
    ta_envelope_pool_arrive(pool, ta_envelope_pool_at(pool, index));
    ta_list_links links = ta_envelope_pool_links(pool);
    ta_list_push_back(&pool->run_queue, &links, index);
}

uint32_t ta_envelope_pool_dequeue(ta_envelope_pool *pool) {
    ta_list_links links = ta_envelope_pool_links(pool);
    uint32_t index = ta_list_pop_front(&pool->run_queue, &links);
    if (index != 0u) {
        ta_envelope_pool_at(pool, index)->state = TA_ENVELOPE_TAKEN;
    }
    return index;
}

void ta_envelope_pool_post(ta_envelope_pool *pool, ta_envelope_post post) {
    ta_envelope_pool_at(pool, post.index)->state = TA_ENVELOPE_OUTGOING;
    ta_list_links links = ta_envelope_pool_links(pool);
    ta_list_push_back(post.outgoing, &links, post.index);
}

void ta_envelope_pool_deliver(ta_envelope_pool *pool, ta_list *outgoing) {
    ta_list_links links = ta_envelope_pool_links(pool);
    for (uint32_t index = ta_list_head(outgoing); index != 0u; index = ta_list_next(&links, index)) {
        ta_envelope_pool_arrive(pool, ta_envelope_pool_at(pool, index));
    }
    ta_list_splice(&pool->run_queue, &links, outgoing);
}

/*
 * Inserts `index`, which keeps its arrival number, into `list`, ordered by
 * arrival: after the last envelope that arrived before it or with the same
 * number. Most inserts land at the tail, which is checked first.
 */
static void ta_envelope_pool_insert_arrived(ta_envelope_pool *pool, ta_list *list, uint32_t index) {
    ta_list_links links = ta_envelope_pool_links(pool);
    uint32_t arrival = ta_envelope_pool_at(pool, index)->heap_index;
    uint32_t tail = ta_list_tail(list);
    if (tail == 0u || !ta_envelope_arrived_before(arrival, ta_envelope_pool_at(pool, tail)->heap_index)) {
        ta_list_push_back(list, &links, index);
        return;
    }
    uint32_t after = 0u;
    for (uint32_t at = ta_list_head(list); at != 0u; at = ta_list_next(&links, at)) {
        if (ta_envelope_arrived_before(arrival, ta_envelope_pool_at(pool, at)->heap_index)) {
            break;
        }
        after = at;
    }
    ta_list_insert_after(list, &links, (ta_list_insert){ .after = after, .item = index });
}

void ta_envelope_pool_return(ta_envelope_pool *pool, uint32_t index) {
    ta_envelope_pool_at(pool, index)->state = TA_ENVELOPE_RUNNABLE;
    ta_envelope_pool_insert_arrived(pool, &pool->run_queue, index);
}

void ta_envelope_pool_enqueue_beside(ta_envelope_pool *pool, ta_envelope_beside beside) {
    ta_envelope *envelope = ta_envelope_pool_at(pool, beside.index);
    envelope->heap_index = ta_envelope_pool_at(pool, beside.beside)->heap_index;
    envelope->state = TA_ENVELOPE_RUNNABLE;
    ta_envelope_pool_insert_arrived(pool, &pool->run_queue, beside.index);
}

void ta_envelope_pool_defer(ta_envelope_pool *pool, ta_envelope_defer defer) {
    ta_envelope_pool_at(pool, defer.index)->state = TA_ENVELOPE_RUNNABLE;
    ta_envelope_pool_insert_arrived(pool, defer.deferred, defer.index);
}

void ta_envelope_pool_push_front(ta_envelope_pool *pool, uint32_t index) {
    ta_envelope_pool_at(pool, index)->state = TA_ENVELOPE_RUNNABLE;
    ta_list_links links = ta_envelope_pool_links(pool);
    ta_list_push_front(&pool->run_queue, &links, index);
}

uint32_t ta_envelope_pool_dequeue_after(ta_envelope_pool *pool, uint32_t previous) {
    ta_list_links links = ta_envelope_pool_links(pool);
    uint32_t index = ta_list_remove_after(&pool->run_queue, &links, previous);
    if (index != 0u) {
        ta_envelope_pool_at(pool, index)->state = TA_ENVELOPE_TAKEN;
    }
    return index;
}

ta_result ta_envelope_pool_unqueue(ta_envelope_pool *pool, uint32_t index) {
    ta_list_links links = ta_envelope_pool_links(pool);
    ta_result result = ta_list_unlink(&pool->run_queue, &links, index);
    if (result == TA_OK) {
        ta_envelope_pool_at(pool, index)->state = TA_ENVELOPE_TAKEN;
    }
    return result;
}
