/*
 * ta_envelope_pool: a system's pending inputs, fixed-size envelopes in an
 * index pool, with the run queue, outgoing lists, the admission watermark
 * and the macrostep reserve (src/envelope.h).
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Memory comes from the test
 * allocator (test_allocator.h), which can refuse any allocation and counts
 * the blocks that are live. Setting its `refuse` policy stands for a
 * macrostep: from then on every allocation would fail.
 *
 * Claims proven here, in order:
 *
 * 1. init refuses a bound below 8 or above 2^24, a watermark of 0% or
 *    100%, and a reserve of 0 or above the bound, with TA_ERR_INVALID,
 *    leaving the pool untouched; a valid init allocates nothing and its
 *    watermark is max(1, bound * percent / 100), computed without
 *    overflow at the largest bound; a pool with no page frees nothing
 *    when destroyed;
 * 2. take with no envelope free answers TA_ERR_FULL and changes nothing;
 *    after a refill every take answers a distinct, zeroed envelope in
 *    state FREE, the quota and the index pool count it alike, and give
 *    marks it free and returns it, so it is the next taken; at the bound
 *    take answers TA_ERR_FULL and changes nothing;
 * 3. refill makes min(reserve, bound - used) envelopes free: the reserve
 *    when the bound leaves room, what the bound leaves near it, nothing
 *    at it; a refill with enough free allocates nothing; trim gives back
 *    whole free pages while the reserve stays free;
 * 4. admission takes only while the envelopes in use stay at or below the
 *    watermark, ordinary takes included, and admitting says whether one
 *    more fits; at the watermark admit answers TA_ERR_FULL and changes
 *    nothing while an ordinary take still succeeds up to the bound;
 *    giving an envelope back lets one more in;
 * 5. the run queue is in arrival order: enqueue marks an envelope
 *    runnable with the next arrival number, dequeue answers the head
 *    marked taken, and an empty run queue dequeues 0 and changes nothing;
 * 6. post keeps a macrostep's mail on its outgoing list in send order,
 *    on no queue; deliver numbers each in order, marks it runnable and
 *    appends the whole list behind the run queue's earlier envelopes,
 *    leaving the outgoing list empty; delivering an empty list changes
 *    nothing; an abandoned macrostep's outgoing list is given back
 *    envelope by envelope;
 * 7. arrival numbers compare modulo 2^32: order holds across the wrap of
 *    the counter;
 * 8. an envelope on the run queue can be on an owner list too, through
 *    owner_next, and each list keeps its own order;
 * 9. take, admit, give, at, admitting, enqueue, dequeue, post and deliver
 *    never allocate, so they may run inside a macrostep; a refill that
 *    cannot allocate answers TA_ERR_NOMEM and changes nothing; an
 *    envelope's address and contents survive the refills that add pages
 *    after it; destroy frees every page.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

/* Envelopes per page. */
#define PAGE (1u << TA_ENVELOPE_PAGE_SHIFT)

/* A pool over `heap` with `bound`, `watermark` percent and `reserve`. */
static ta_envelope_pool pool_over(test_allocator *heap, const uint32_t shape[3]) {
    ta_envelope_pool pool;
    ta_result result = ta_envelope_pool_init(&pool, &(ta_envelope_pool_params){ .allocator = test_allocator_of(heap),
                                                                                 .bound = shape[0],
                                                                                 .watermark_percent = shape[1],
                                                                                 .reserve = shape[2] });
    CHECK(result == TA_OK);
    return pool;
}

/* Whether every byte of the envelope at `index` is zero but its state. */
static bool zeroed(const ta_envelope_pool *pool, uint32_t index) {
    ta_envelope blank;
    memset(&blank, 0, sizeof(blank));
    const ta_envelope *envelope = ta_envelope_pool_at(pool, index);
    return envelope != NULL && memcmp(envelope, &blank, sizeof(blank)) == 0;
}

/* Claim 1. */
static void test_init(void) {
    test_allocator heap = { 0 };
    static const uint32_t invalid[][3] = {
        { 7u, 50u, 1u }, { TA_ENVELOPE_POOL_MAX + 1u, 50u, 1u }, { 8u, 0u, 1u },
        { 8u, 100u, 1u }, { 8u, 50u, 0u }, { 8u, 50u, 9u },
    };
    for (size_t row = 0; row < sizeof invalid / sizeof invalid[0]; row += 1u) {
        ta_envelope_pool pool;
        memset(&pool, 0xEE, sizeof(pool));
        ta_envelope_pool before = pool;
        ta_result result = ta_envelope_pool_init(&pool, &(ta_envelope_pool_params){ .allocator = test_allocator_of(&heap),
                                                                                     .bound = invalid[row][0],
                                                                                     .watermark_percent = invalid[row][1],
                                                                                     .reserve = invalid[row][2] });
        CHECK(result == TA_ERR_INVALID);
        CHECK(memcmp(&pool, &before, sizeof(pool)) == 0);
    }
    /* bound, percent, expected watermark */
    static const uint32_t watermarks[][3] = {
        { 8u, 1u, 1u },       { 8u, 50u, 4u },       { 8u, 99u, 7u },
        { 4096u, 50u, 2048u }, { 4096u, 1u, 40u },   { 100u, 33u, 33u },
        { TA_ENVELOPE_POOL_MAX, 99u, 16609443u },     { TA_ENVELOPE_POOL_MAX, 1u, 167772u },
    };
    for (size_t row = 0; row < sizeof watermarks / sizeof watermarks[0]; row += 1u) {
        ta_envelope_pool pool = pool_over(&heap, (uint32_t[3]){ watermarks[row][0], watermarks[row][1], 1u });
        CHECK(pool.watermark == watermarks[row][2]);
        CHECK(pool.quota.capacity == watermarks[row][0]);
        CHECK(pool.quota.used == 0u && pool.items.free == 0u);
        CHECK(ta_list_empty(&pool.run_queue));
        ta_envelope_pool_destroy(&pool);
    }
    CHECK(heap.attempts == 0u && heap.frees == 0u);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 2. */
static void test_take_give(void) {
    test_allocator heap = { 0 };
    ta_envelope_pool pool = pool_over(&heap, (uint32_t[3]){ 8u, 50u, 8u });
    uint32_t index = 77u;
    CHECK(ta_envelope_pool_take(&pool, &index) == TA_ERR_FULL);
    CHECK(index == 77u && pool.quota.used == 0u && pool.items.used == 0u);

    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    CHECK(pool.items.free == 8u);
    uint32_t taken[8];
    for (uint32_t i = 0; i < 8u; i += 1u) {
        CHECK(ta_envelope_pool_take(&pool, &taken[i]) == TA_OK);
        CHECK(taken[i] == i + 1u);
        CHECK(zeroed(&pool, taken[i]));
        CHECK(ta_envelope_pool_at(&pool, taken[i])->state == TA_ENVELOPE_FREE);
        CHECK(pool.quota.used == i + 1u && pool.items.used == i + 1u);
        /* Scribble on it, so a later take must zero it again. */
        memset(ta_envelope_pool_at(&pool, taken[i]), 0xC3, sizeof(ta_envelope));
    }
    index = 77u;
    CHECK(ta_envelope_pool_take(&pool, &index) == TA_ERR_FULL);
    CHECK(index == 77u && pool.quota.used == 8u && pool.items.used == 8u);
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK); /* at the bound: nothing to make free */
    CHECK(pool.items.free == 0u);

    ta_envelope_pool_give(&pool, taken[4]);
    CHECK(ta_envelope_pool_at(&pool, taken[4])->state == TA_ENVELOPE_FREE);
    CHECK(pool.quota.used == 7u && pool.items.used == 7u && pool.items.free == 1u);
    CHECK(ta_envelope_pool_take(&pool, &index) == TA_OK);
    CHECK(index == taken[4]);
    CHECK(zeroed(&pool, index));
    for (uint32_t i = 0; i < 8u; i += 1u) {
        ta_envelope_pool_give(&pool, taken[i]);
    }
    CHECK(pool.quota.used == 0u && pool.items.used == 0u && pool.items.free == 8u);
    ta_envelope_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 3. */
static void test_refill_reserve(void) {
    test_allocator heap = { 0 };
    uint32_t bound = 3u * PAGE + 8u;
    uint32_t reserve = PAGE + 1u;
    ta_envelope_pool pool = pool_over(&heap, (uint32_t[3]){ bound, 50u, reserve });
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    CHECK(pool.items.free >= reserve);
    CHECK(pool.items.pages.count == 2u); /* the reserve fills two pages */
    uint32_t attempts = heap.attempts;
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    CHECK(heap.attempts == attempts); /* enough free: nothing allocated */

    /* Take until only 5 more fit under the bound, refilling as a safe point would. */
    uint32_t index = 0;
    while (pool.quota.used < bound - 5u) {
        if (pool.items.free == 0u) {
            CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
        }
        CHECK(ta_envelope_pool_take(&pool, &index) == TA_OK);
    }
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    CHECK(pool.items.free == 5u); /* min(reserve, bound - used) */
    while (pool.quota.used < bound) {
        CHECK(ta_envelope_pool_take(&pool, &index) == TA_OK);
    }
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    CHECK(pool.items.free == 0u);

    /* Give everything back, then trim: whole free pages go while the reserve stays free. */
    for (uint32_t i = 1u; i <= bound; i += 1u) {
        ta_envelope_pool_give(&pool, i);
    }
    CHECK(pool.items.pages.count == 4u);
    ta_envelope_pool_trim(&pool);
    CHECK(pool.items.free >= reserve);
    CHECK(pool.items.free < reserve + PAGE);
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    ta_envelope_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 4. */
static void test_watermark(void) {
    test_allocator heap = { 0 };
    ta_envelope_pool pool = pool_over(&heap, (uint32_t[3]){ 8u, 50u, 8u });
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    uint32_t admitted[4];
    for (uint32_t i = 0; i < 4u; i += 1u) {
        CHECK(ta_envelope_pool_admitting(&pool));
        CHECK(ta_envelope_pool_admit(&pool, &admitted[i]) == TA_OK);
        CHECK(zeroed(&pool, admitted[i]));
    }
    CHECK(!ta_envelope_pool_admitting(&pool));
    uint32_t index = 77u;
    CHECK(ta_envelope_pool_admit(&pool, &index) == TA_ERR_FULL);
    CHECK(index == 77u && pool.quota.used == 4u && pool.items.used == 4u && pool.items.free == 4u);
    /* The headroom above the watermark is for ordinary takes, up to the bound. */
    uint32_t sent[4];
    for (uint32_t i = 0; i < 4u; i += 1u) {
        CHECK(ta_envelope_pool_take(&pool, &sent[i]) == TA_OK);
    }
    CHECK(ta_envelope_pool_take(&pool, &index) == TA_ERR_FULL);
    /* Ordinary takes count against the watermark: with 7 in use, nothing is admitted. */
    ta_envelope_pool_give(&pool, admitted[0]);
    CHECK(!ta_envelope_pool_admitting(&pool));
    CHECK(ta_envelope_pool_admit(&pool, &index) == TA_ERR_FULL);
    for (uint32_t i = 0; i < 4u; i += 1u) {
        ta_envelope_pool_give(&pool, sent[i]);
    }
    /* 3 in use, one under the watermark of 4: exactly one more is admitted. */
    CHECK(ta_envelope_pool_admitting(&pool));
    CHECK(ta_envelope_pool_admit(&pool, &index) == TA_OK);
    CHECK(!ta_envelope_pool_admitting(&pool));
    CHECK(ta_envelope_pool_admit(&pool, &index) == TA_ERR_FULL);
    ta_envelope_pool_destroy(&pool);

    /* With no envelope free, admit answers FULL below the watermark too. */
    pool = pool_over(&heap, (uint32_t[3]){ 8u, 99u, 1u });
    CHECK(pool.watermark == 7u);
    CHECK(ta_envelope_pool_admitting(&pool));
    CHECK(ta_envelope_pool_admit(&pool, &index) == TA_ERR_FULL);
    CHECK(pool.quota.used == 0u);
    ta_envelope_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 5. */
static void test_run_queue(void) {
    test_allocator heap = { 0 };
    ta_envelope_pool pool = pool_over(&heap, (uint32_t[3]){ 16u, 50u, 16u });
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    CHECK(ta_envelope_pool_dequeue(&pool) == 0u);
    CHECK(ta_list_empty(&pool.run_queue) && pool.arrivals == 0u);

    uint32_t order[5];
    for (uint32_t i = 0; i < 5u; i += 1u) {
        CHECK(ta_envelope_pool_take(&pool, &order[i]) == TA_OK);
    }
    /* Enqueue in an order that is not the index order. */
    static const uint32_t shuffle[5] = { 3u, 0u, 4u, 1u, 2u };
    for (uint32_t i = 0; i < 5u; i += 1u) {
        uint32_t index = order[shuffle[i]];
        ta_envelope_pool_enqueue(&pool, index);
        ta_envelope *envelope = ta_envelope_pool_at(&pool, index);
        CHECK(envelope->state == TA_ENVELOPE_RUNNABLE);
        CHECK(envelope->heap_index == i);
    }
    CHECK(pool.arrivals == 5u);
    CHECK(ta_list_tail(&pool.run_queue) == order[shuffle[4]]);
    for (uint32_t i = 0; i < 5u; i += 1u) {
        uint32_t index = ta_envelope_pool_dequeue(&pool);
        CHECK(index == order[shuffle[i]]);
        CHECK(ta_envelope_pool_at(&pool, index)->state == TA_ENVELOPE_TAKEN);
        CHECK(ta_envelope_pool_at(&pool, index)->next == 0u);
    }
    CHECK(ta_envelope_pool_dequeue(&pool) == 0u);
    CHECK(ta_list_empty(&pool.run_queue) && pool.arrivals == 5u);
    ta_envelope_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 6. */
static void test_outgoing(void) {
    test_allocator heap = { 0 };
    ta_envelope_pool pool = pool_over(&heap, (uint32_t[3]){ 16u, 50u, 16u });
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    uint32_t earlier = 0;
    CHECK(ta_envelope_pool_admit(&pool, &earlier) == TA_OK);
    ta_envelope_pool_enqueue(&pool, earlier);

    ta_list outgoing = TA_LIST_EMPTY;
    ta_envelope_pool_deliver(&pool, &outgoing); /* empty: nothing changes */
    CHECK(pool.arrivals == 1u && ta_list_head(&pool.run_queue) == earlier && ta_list_tail(&pool.run_queue) == earlier);

    uint32_t mail[3];
    for (uint32_t i = 0; i < 3u; i += 1u) {
        CHECK(ta_envelope_pool_take(&pool, &mail[i]) == TA_OK);
        ta_envelope_pool_at(&pool, mail[i])->send_id = 100u + i;
        ta_envelope_pool_post(&pool, (ta_envelope_post){ .outgoing = &outgoing, .index = mail[i] });
        CHECK(ta_envelope_pool_at(&pool, mail[i])->state == TA_ENVELOPE_OUTGOING);
    }
    /* On no queue yet: the run queue holds only the earlier envelope. */
    CHECK(ta_list_head(&pool.run_queue) == earlier && ta_list_tail(&pool.run_queue) == earlier);
    ta_list_links links = ta_envelope_pool_links(&pool);
    uint32_t at = ta_list_head(&outgoing);
    for (uint32_t i = 0; i < 3u; i += 1u) {
        CHECK(at == mail[i]);
        at = ta_list_next(&links, at);
    }
    CHECK(at == 0u);

    ta_envelope_pool_deliver(&pool, &outgoing);
    CHECK(ta_list_empty(&outgoing));
    CHECK(ta_envelope_pool_dequeue(&pool) == earlier);
    for (uint32_t i = 0; i < 3u; i += 1u) {
        ta_envelope *envelope = ta_envelope_pool_at(&pool, mail[i]);
        CHECK(envelope->state == TA_ENVELOPE_RUNNABLE);
        CHECK(envelope->heap_index == 1u + i);
        CHECK(ta_envelope_pool_dequeue(&pool) == mail[i]);
        CHECK(envelope->send_id == 100u + i);
    }
    CHECK(pool.arrivals == 4u);

    /* An abandoned macrostep gives its outgoing list back. */
    for (uint32_t i = 0; i < 3u; i += 1u) {
        ta_envelope_pool_post(&pool, (ta_envelope_post){ .outgoing = &outgoing, .index = mail[i] });
    }
    for (uint32_t index = ta_list_pop_front(&outgoing, &links); index != 0u;
         index = ta_list_pop_front(&outgoing, &links)) {
        ta_envelope_pool_give(&pool, index);
    }
    CHECK(pool.quota.used == 1u && ta_list_empty(&pool.run_queue));
    ta_envelope_pool_give(&pool, earlier);
    CHECK(pool.quota.used == 0u && pool.items.free == 16u);
    ta_envelope_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 7. */
static void test_arrival_wrap(void) {
    CHECK(ta_envelope_arrived_before(0u, 1u));
    CHECK(!ta_envelope_arrived_before(1u, 0u));
    CHECK(!ta_envelope_arrived_before(5u, 5u));
    CHECK(ta_envelope_arrived_before(UINT32_MAX, 0u));
    CHECK(ta_envelope_arrived_before(UINT32_MAX - 3u, 2u));
    CHECK(!ta_envelope_arrived_before(2u, UINT32_MAX - 3u));
    CHECK(ta_envelope_arrived_before(0u, 0x7FFFFFFFu));

    test_allocator heap = { 0 };
    ta_envelope_pool pool = pool_over(&heap, (uint32_t[3]){ 8u, 50u, 8u });
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    pool.arrivals = UINT32_MAX - 1u;
    uint32_t index[4];
    for (uint32_t i = 0; i < 4u; i += 1u) {
        CHECK(ta_envelope_pool_take(&pool, &index[i]) == TA_OK);
        ta_envelope_pool_enqueue(&pool, index[i]);
    }
    CHECK(ta_envelope_pool_at(&pool, index[1])->heap_index == UINT32_MAX);
    CHECK(ta_envelope_pool_at(&pool, index[2])->heap_index == 0u);
    for (uint32_t i = 0; i + 1u < 4u; i += 1u) {
        CHECK(ta_envelope_arrived_before(ta_envelope_pool_at(&pool, index[i])->heap_index,
                                         ta_envelope_pool_at(&pool, index[i + 1u])->heap_index));
    }
    ta_envelope_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 8. */
static void test_owner_links(void) {
    test_allocator heap = { 0 };
    ta_envelope_pool pool = pool_over(&heap, (uint32_t[3]){ 8u, 50u, 8u });
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    uint32_t index[3];
    ta_list owner = TA_LIST_EMPTY;
    ta_list_links owner_links = ta_envelope_pool_owner_links(&pool);
    for (uint32_t i = 0; i < 3u; i += 1u) {
        CHECK(ta_envelope_pool_take(&pool, &index[i]) == TA_OK);
        ta_envelope_pool_enqueue(&pool, index[i]);
        ta_list_push_front(&owner, &owner_links, index[i]); /* the reverse order */
    }
    CHECK(ta_list_head(&owner) == index[2] && ta_list_tail(&owner) == index[0]);
    CHECK(ta_list_unlink(&owner, &owner_links, index[1]) == TA_OK);
    CHECK(ta_list_next(&owner_links, index[2]) == index[0]);
    for (uint32_t i = 0; i < 3u; i += 1u) {
        CHECK(ta_envelope_pool_dequeue(&pool) == index[i]);
    }
    CHECK(ta_list_head(&owner) == index[2] && ta_list_tail(&owner) == index[0]);
    ta_envelope_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0u);
}

/* Claim 9. */
static void test_no_allocation(void) {
    test_allocator heap = { 0 };
    ta_envelope_pool pool = pool_over(&heap, (uint32_t[3]){ 4u * PAGE, 50u, PAGE });
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    uint32_t first = 0;
    CHECK(ta_envelope_pool_admit(&pool, &first) == TA_OK);
    ta_envelope *kept = ta_envelope_pool_at(&pool, first);
    kept->source_seq = 0x1122334455667788u;
    kept->text = (ta_chunk_chain){ .first = 9u, .length = 33u };

    /* A macrostep: every allocation from here on would fail. */
    heap.policy.refuse = true;
    uint32_t attempts = heap.attempts;
    ta_list outgoing = TA_LIST_EMPTY;
    uint32_t index = 0;
    uint32_t sent = 0;
    while (ta_envelope_pool_take(&pool, &index) == TA_OK) {
        ta_envelope_pool_post(&pool, (ta_envelope_post){ .outgoing = &outgoing, .index = index });
        sent += 1u;
    }
    CHECK(sent == PAGE - 1u); /* what the last safe point left free, and no more */
    CHECK(ta_envelope_pool_admit(&pool, &index) == TA_ERR_FULL);
    CHECK(ta_envelope_pool_admitting(&pool)); /* below the watermark, though nothing is free */
    ta_envelope_pool_enqueue(&pool, first);
    CHECK(ta_envelope_pool_dequeue(&pool) == first);
    ta_envelope_pool_deliver(&pool, &outgoing);
    index = ta_envelope_pool_dequeue(&pool);
    ta_envelope_pool_give(&pool, index);
    CHECK(heap.attempts == attempts);

    /* After the macrostep, a refill that cannot allocate changes nothing. */
    uint32_t free_before = pool.items.free;
    uint32_t used_before = pool.quota.used;
    uint32_t pages_before = pool.items.pages.count;
    uint32_t live_before = heap.live;
    CHECK(ta_envelope_pool_refill(&pool) == TA_ERR_NOMEM);
    CHECK(pool.items.free == free_before && pool.quota.used == used_before);
    CHECK(pool.items.pages.count == pages_before && heap.live == live_before);

    /* With allocation back, refill adds pages; the first envelope did not move and kept its contents. */
    heap.policy.refuse = false;
    CHECK(ta_envelope_pool_refill(&pool) == TA_OK);
    CHECK(pool.items.free >= PAGE);
    CHECK(ta_envelope_pool_at(&pool, first) == kept);
    CHECK(kept->source_seq == 0x1122334455667788u && kept->text.first == 9u && kept->text.length == 33u);
    CHECK(kept->state == TA_ENVELOPE_TAKEN);
    ta_envelope_pool_destroy(&pool);
    CHECK(heap.live == 0u);
    CHECK(test_allocator_finish(&heap) == 0u);
}

int main(void) {
    test_init();
    test_take_give();
    test_refill_reserve();
    test_watermark();
    test_run_queue();
    test_outgoing();
    test_arrival_wrap();
    test_owner_links();
    test_no_allocation();
    if (failures != 0) {
        fprintf(stderr, "envelope_test: %d checks failed\n", failures);
        return 1;
    }
    printf("envelope_test: ok\n");
    return 0;
}
