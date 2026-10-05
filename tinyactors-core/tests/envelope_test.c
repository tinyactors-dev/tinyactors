/* test: standalone asan */
/*
 * The envelope pool (src/envelope.h), on its own: no system, no actor.
 *
 *   1. Creation: init allocates nothing, validates its limits, and rounds the
 *      bound down to whole pages (a tiny pool has a tiny page); the envelope
 *      is 168 bytes.
 *   2. Refill and the ceiling: make answers 0 until a refill, a refill is cut
 *      to the bound, grows by whole pages, allocates nothing when enough are
 *      free, and keeps pointers stable.
 *   3. Make and release: every field is clear after make, release gives back
 *      the event's and the address's references, and the counters (pool,
 *      quota) agree after every step.
 *   4. The watermark: the arithmetic at 1, 50 and 99 percent and at a bound
 *      of 8.
 *   5. The reserve: a claim is min(macrostepReserve, available), pays for
 *      envelopes without being refused, and is spent exactly at its size;
 *      unreserve gives back the rest.
 *   6. The run queue and outgoing lists: arrival order, and a splice is one
 *      step that keeps the order.
 *   7. Waiting lists: the first insert says "first", the next do not, a take
 *      returns the whole list in arrival order, two addresses with ONE 32-bit
 *      hash stay apart, and a refill that re-creates the table keeps every
 *      list.
 *   8. Failure: every allocation of a refill refused in turn leaves the pool,
 *      the table, the lists and the counters as they were, and the retry
 *      works (a strict allocator checks every free's size).
 *   9. Destroy releases the references of envelopes still in use.
 *
 * Fast, not exhaustive: the largest pool here has 1,024 envelopes.
 * Includes the amalgamation, because the unit is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* open makes an envelope pool over the strict allocator with the given limits. */
static ta_result open_pool(ta_envelopes *envelopes, uint32_t pool, uint32_t watermark, uint32_t reserve) {
    ta_envelope_limits limits = { .pool_envelopes = pool, .admission_watermark = watermark, .macrostep_reserve = reserve };
    return ta_envelopes_init(envelopes, ta_test_allocator_get(&heap), &limits);
}

/* consistent checks I1 and the pool counters: what must hold between any two calls. */
static void consistent(const ta_envelopes *envelopes) {
    ta_pool_counts counts;
    ta_pool_counts_get(&envelopes->pool, &counts);
    CHECK(envelopes->quota.used == counts.used);
    CHECK(envelopes->quota.used + envelopes->quota.reserved <= envelopes->quota.capacity);
    CHECK(counts.capacity <= envelopes->bound);
    CHECK(envelopes->quota.capacity == envelopes->bound);
}

static void test_creation(void) {
    CHECK(sizeof(ta_envelope) == 168u);
    ta_test_allocator_reset_counts(&heap);
    ta_envelopes envelopes;
    CHECK(open_pool(&envelopes, 4096u, 50u, 1024u) == TA_OK);
    CHECK(heap.stats.allocations == 0 && heap.stats.live_blocks == 0); /* an empty system pays nothing */
    CHECK(envelopes.bound == 4096u && envelopes.pool.page_mask == 255u);
    CHECK(ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE) == 0); /* nothing until a refill */
    CHECK(ta_envelopes_run_pop(&envelopes) == 0);
    consistent(&envelopes);
    ta_envelopes_destroy(&envelopes);
    ta_envelopes_destroy(&envelopes); /* idempotent */

    /* Rounding: whole pages of 256, or the largest power of two under a small bound. */
    static const struct { uint32_t pool, page, bound; } cases[] = {
        { 1u, 1u, 1u },     { 8u, 8u, 8u },     { 9u, 8u, 8u },     { 100u, 64u, 64u },
        { 255u, 128u, 128u }, { 256u, 256u, 256u }, { 1000u, 256u, 768u }, { 4096u, 256u, 4096u },
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index += 1u) {
        CHECK(open_pool(&envelopes, cases[index].pool, 50u, 1u) == TA_OK);
        CHECK(envelopes.pool.page_mask + 1u == cases[index].page && envelopes.bound == cases[index].bound);
        ta_envelopes_destroy(&envelopes);
    }

    /* Limits outside their ranges, and a null block, are rejected and leave a destroyable block. */
    CHECK(open_pool(&envelopes, 0u, 50u, 1u) == TA_ERR_INVALID);
    CHECK(open_pool(&envelopes, 8u, 0u, 1u) == TA_ERR_INVALID);
    CHECK(open_pool(&envelopes, 8u, 100u, 1u) == TA_ERR_INVALID);
    CHECK(open_pool(&envelopes, 8u, 50u, 0u) == TA_ERR_INVALID);
    CHECK(open_pool(&envelopes, 8u, 50u, 9u) == TA_ERR_INVALID);
    CHECK(ta_envelopes_init(NULL, ta_test_allocator_get(&heap), &(ta_envelope_limits){ 8u, 50u, 1u }) == TA_ERR_INVALID);
    CHECK(ta_envelopes_init(&envelopes, ta_test_allocator_get(&heap), NULL) == TA_ERR_INVALID);
    ta_envelopes_destroy(&envelopes);
    CHECK(heap.stats.live_blocks == 0);
}

static void test_refill_and_ceiling(void) {
    ta_envelopes envelopes;
    CHECK(open_pool(&envelopes, 512u, 50u, 16u) == TA_OK);
    CHECK(ta_envelopes_refill(&envelopes, 0) == TA_OK && heap.stats.live_blocks == 0); /* nothing wanted */
    ta_test_allocator_reset_counts(&heap);
    CHECK(ta_envelopes_refill(&envelopes, 1u) == TA_OK);
    CHECK(envelopes.pool.capacity == 256u && envelopes.pool.pages == 1u);
    uint64_t after_first = heap.stats.allocations;
    CHECK(after_first >= 3u); /* directory, page, waiting table (page + directory) */
    CHECK(ta_envelopes_refill(&envelopes, 200u) == TA_OK && heap.stats.allocations == after_first); /* enough free: no call */
    uint32_t first = ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE);
    CHECK(first == 1u);
    ta_envelope *pointer = ta_envelope_at(&envelopes, first);
    /* Take everything that is there, then more: the second page is added, the first does not move. */
    for (uint32_t index = 1; index < 256u; index += 1u) {
        CHECK(ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE) != 0);
    }
    CHECK(ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE) == 0); /* full until refilled */
    CHECK(ta_envelopes_refill(&envelopes, 1u) == TA_OK && envelopes.pool.capacity == 512u);
    CHECK(ta_envelope_at(&envelopes, first) == pointer);
    consistent(&envelopes);
    /* The ceiling: wanting more than the bound allows is cut, not an error, and then nothing more comes. */
    CHECK(ta_envelopes_refill(&envelopes, 100000u) == TA_OK && envelopes.pool.capacity == 512u);
    for (uint32_t index = 0; index < 256u; index += 1u) {
        CHECK(ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE) != 0);
    }
    CHECK(ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE) == 0);
    CHECK(ta_envelopes_refill(&envelopes, 1u) == TA_OK && envelopes.pool.capacity == 512u); /* at the bound: no growth */
    CHECK(envelopes.quota.used == 512u);
    consistent(&envelopes);
    for (uint32_t index = 1; index <= 512u; index += 1u) {
        ta_envelope_release(&envelopes, index);
    }
    CHECK(envelopes.quota.used == 0 && envelopes.pool.used == 0);
    consistent(&envelopes);
    ta_envelopes_destroy(&envelopes);
    CHECK(heap.stats.live_blocks == 0);
}

static void test_make_and_release(void) {
    ta_envelopes envelopes;
    CHECK(open_pool(&envelopes, 16u, 50u, 4u) == TA_OK);
    CHECK(ta_envelopes_refill(&envelopes, 4u) == TA_OK);
    uint32_t index = ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE);
    ta_envelope *envelope = ta_envelope_at(&envelopes, index);
    CHECK(envelope->state == TA_ENVELOPE_RUNNABLE && envelope->next == 0 && envelope->input == 0 && envelope->target == 0);
    envelope->input = 77u;
    envelope->flags = 5u;
    envelope->event.name = 3u;
    ta_envelope_release(&envelopes, index);
    CHECK(envelope->state == TA_ENVELOPE_FREE && envelopes.pool.used == 0 && envelopes.quota.used == 0);
    ta_envelope_release(&envelopes, index); /* releasing a free envelope or 0 does nothing */
    ta_envelope_release(&envelopes, 0);
    CHECK(envelopes.pool.used == 0 && envelopes.quota.used == 0);
    /* The most recently given envelope is the next made, and it comes back clear. */
    CHECK(ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_TAKEN) == index);
    CHECK(envelope->input == 0 && envelope->flags == 0 && envelope->event.name == 0 && envelope->state == TA_ENVELOPE_TAKEN);
    ta_envelope_release(&envelopes, index);
    CHECK(ta_envelope_at(&envelopes, 0) == NULL && ta_envelope_at(&envelopes, 1000u) == NULL);
    consistent(&envelopes);
    ta_envelopes_destroy(&envelopes);
    CHECK(heap.stats.live_blocks == 0);
}

static void test_watermark(void) {
    static const struct { uint32_t pool, percent; } cases[] = { { 8u, 1u }, { 8u, 50u }, { 8u, 99u }, { 4096u, 1u },
                                                                { 4096u, 50u }, { 4096u, 99u }, { 1024u, 50u } };
    for (size_t row = 0; row < sizeof(cases) / sizeof(cases[0]); row += 1u) {
        ta_envelopes envelopes;
        CHECK(open_pool(&envelopes, cases[row].pool, cases[row].percent, 1u) == TA_OK);
        CHECK(ta_envelopes_refill(&envelopes, envelopes.bound) == TA_OK);
        /* Admission stops at the first used for which used * 100 >= bound * percent. */
        uint32_t expected = (uint32_t)(((uint64_t)envelopes.bound * cases[row].percent + 99u) / 100u);
        uint32_t admitted = 0;
        while (ta_envelopes_admissible(&envelopes)) {
            CHECK(ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE) != 0);
            admitted += 1u;
        }
        CHECK(admitted == expected);
        /* The headroom above the watermark is still there for mail (a make with no admission check). */
        if (admitted < envelopes.bound) {
            CHECK(ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE) != 0);
        }
        ta_envelopes_destroy(&envelopes);
    }
    CHECK(heap.stats.live_blocks == 0);
}

static void test_reserve(void) {
    ta_envelopes envelopes;
    CHECK(open_pool(&envelopes, 256u, 50u, 100u) == TA_OK);
    CHECK(ta_envelopes_refill(&envelopes, 100u) == TA_OK);
    ta_quota_claim claim = {0};
    CHECK(ta_envelopes_reserve(&envelopes, &claim) == 100u && claim.held == 100u && envelopes.quota.reserved == 100u);
    consistent(&envelopes);
    /* Reserved envelopes are not "in use": the watermark does not count them. */
    CHECK(envelopes.pool.used == 0 && ta_envelopes_admissible(&envelopes));
    uint32_t made[100];
    for (uint32_t index = 0; index < 100u; index += 1u) {
        made[index] = ta_envelope_make(&envelopes, &claim, TA_ENVELOPE_RUNNABLE);
        CHECK(made[index] != 0);
    }
    CHECK(claim.held == 0 && envelopes.quota.reserved == 0 && envelopes.quota.used == 100u);
    CHECK(ta_envelope_make(&envelopes, &claim, TA_ENVELOPE_RUNNABLE) == 0); /* the claim is spent */
    CHECK(envelopes.quota.used == 100u && envelopes.pool.used == 100u);       /* and nothing was taken */
    consistent(&envelopes);
    /* A second claim is cut to what is left of the quota: bound 256, 100 used, 156 available, reserve 100. */
    ta_quota_claim second = {0};
    CHECK(ta_envelopes_reserve(&envelopes, &second) == 100u);
    ta_quota_claim third = {0};
    CHECK(ta_envelopes_reserve(&envelopes, &third) == 56u);
    ta_quota_claim fourth = {0};
    CHECK(ta_envelopes_reserve(&envelopes, &fourth) == 0u);
    /* Ordinary make cannot dip into what is reserved. */
    CHECK(ta_envelopes_refill(&envelopes, 156u) == TA_OK);
    CHECK(ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE) == 0);
    CHECK(ta_envelopes_unreserve(&envelopes, &second) == 100u && ta_envelopes_unreserve(&envelopes, &third) == 56u);
    CHECK(envelopes.quota.reserved == 0);
    CHECK(ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE) != 0);
    consistent(&envelopes);
    ta_envelopes_destroy(&envelopes);
    CHECK(heap.stats.live_blocks == 0);
}

static void test_lists(void) {
    ta_envelopes envelopes;
    CHECK(open_pool(&envelopes, 64u, 50u, 8u) == TA_OK);
    CHECK(ta_envelopes_refill(&envelopes, 16u) == TA_OK);
    uint32_t queued[3];
    for (int index = 0; index < 3; index += 1) {
        queued[index] = ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE);
        ta_envelopes_run_push(&envelopes, queued[index]);
    }
    /* A sender's outgoing list is invisible until the splice, which keeps its order and is one step. */
    ta_ilist outgoing;
    ta_ilist_init(&outgoing);
    uint32_t sent[3];
    for (int index = 0; index < 3; index += 1) {
        sent[index] = ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE);
        ta_ilist_push_back(&outgoing, ta_envelopes_link(&envelopes), sent[index]);
    }
    CHECK(ta_envelopes_run_pop(&envelopes) == queued[0]);
    CHECK(ta_envelope_at(&envelopes, queued[0])->next == 0);
    ta_envelopes_run_splice(&envelopes, &outgoing);
    CHECK(outgoing.head == 0 && outgoing.tail == 0);
    CHECK(ta_envelopes_run_pop(&envelopes) == queued[1]);
    CHECK(ta_envelopes_run_pop(&envelopes) == queued[2]);
    CHECK(ta_envelopes_run_pop(&envelopes) == sent[0]);
    CHECK(ta_envelopes_run_pop(&envelopes) == sent[1]);
    CHECK(ta_envelopes_run_pop(&envelopes) == sent[2]);
    CHECK(ta_envelopes_run_pop(&envelopes) == 0);
    ta_envelopes_run_splice(&envelopes, &outgoing); /* splicing nothing is a no-op */
    CHECK(ta_envelopes_run_pop(&envelopes) == 0);
    ta_envelopes_destroy(&envelopes);
    CHECK(heap.stats.live_blocks == 0);
}

/* A name that denotes an address under the family "svc". */
typedef struct {
    char text[16];
    uint32_t length;
} name;

static ta_actor_address address_of(const name *entry) {
    return (ta_actor_address){ .family = { .name = { .ptr = "svc", .len = 3u } },
                               .name = { .ptr = entry->text, .len = entry->length } };
}

static name numbered(uint32_t number) {
    name entry = {0};
    entry.length = (uint32_t)snprintf(entry.text, sizeof(entry.text), "n%u", number);
    return entry;
}

/* scrambled answers a name of eight letters derived from `number` by a mixing function, so the names behave like random ones. */
static name scrambled(uint32_t number) {
    name entry = {0};
    uint64_t state = ta_mix64((uint64_t)number + 1u);
    for (uint32_t position = 0; position < 8u; position += 1u) {
        entry.text[position] = (char)('a' + (state >> (position * 5u)) % 26u);
    }
    entry.length = 8u;
    return entry;
}

/*
 * find_collision searches scrambled names for two whose address hash is the
 * same 32-bit value (a birthday search over 400,000 names: about 18 pairs are
 * expected, and the search is deterministic) and stores the first it finds.
 * FNV-1a over names that differ only in a counter does not collide, which is
 * why the names are scrambled.
 */
static bool find_collision(name *left, name *right) {
    enum { COUNT = 400000, SLOTS = 1 << 20 };
    uint32_t *seen = calloc(SLOTS, sizeof(*seen));
    uint32_t *hashes = calloc(COUNT, sizeof(*hashes));
    bool found = false;
    for (uint32_t number = 0; number < COUNT && !found; number += 1u) {
        name entry = scrambled(number);
        ta_actor_address address = address_of(&entry);
        hashes[number] = ta_address_hash(&address);
        uint32_t slot = hashes[number] & (SLOTS - 1u);
        while (seen[slot] != 0) {
            if (hashes[seen[slot] - 1u] == hashes[number]) {
                *left = scrambled(seen[slot] - 1u);
                *right = entry;
                found = true;
                break;
            }
            slot = (slot + 1u) & (SLOTS - 1u);
        }
        seen[slot] = number + 1u;
    }
    free(seen);
    free(hashes);
    return found;
}

/* wait_on makes an envelope for `address` and puts it on the waiting list; *first says whether it began the list. */
static uint32_t wait_on(ta_envelopes *envelopes, ta_address *address, bool *first) {
    uint32_t index = ta_envelope_make(envelopes, NULL, TA_ENVELOPE_RUNNABLE);
    CHECK(index != 0);
    CHECK(ta_waiting_insert(envelopes, index, address, first) == TA_OK);
    return index;
}

static void test_waiting(void) {
    ta_address_table addresses;
    ta_address_table_init(&addresses, ta_test_allocator_get(&heap));
    name left, right;
    CHECK(find_collision(&left, &right));
    name plain = numbered(7u);
    ta_address *a = NULL, *b = NULL, *c = NULL;
    ta_actor_address address_a = address_of(&left), address_b = address_of(&right), address_c = address_of(&plain);
    CHECK(ta_address_table_intern(&addresses, &address_a, &a) == TA_OK);
    CHECK(ta_address_table_intern(&addresses, &address_b, &b) == TA_OK);
    CHECK(ta_address_table_intern(&addresses, &address_c, &c) == TA_OK);
    CHECK(a != b && ta_address_record_hash(a) == ta_address_record_hash(b)); /* one hash, two addresses */
    uint32_t references = a->references;

    ta_envelopes envelopes;
    CHECK(open_pool(&envelopes, 1024u, 50u, 8u) == TA_OK);
    CHECK(ta_envelopes_refill(&envelopes, 8u) == TA_OK);
    bool first = false;
    CHECK(ta_waiting_take_list(&envelopes, a) == 0); /* nothing waits yet */
    uint32_t a1 = wait_on(&envelopes, a, &first);
    CHECK(first && a->references == references + 1u);
    uint32_t b1 = wait_on(&envelopes, b, &first);
    CHECK(first); /* the equal hash is not the same address */
    uint32_t a2 = wait_on(&envelopes, a, &first);
    CHECK(!first);
    uint32_t c1 = wait_on(&envelopes, c, &first);
    CHECK(first);
    uint32_t a3 = wait_on(&envelopes, a, &first);
    CHECK(!first && ta_envelope_at(&envelopes, a1)->waiting_tail == a3);
    CHECK(ta_envelope_at(&envelopes, a2)->state == TA_ENVELOPE_WAITING && ta_envelope_at(&envelopes, a2)->payload.address == a);
    CHECK(ta_table_count(&envelopes.waiting) == 3u);

    /* A refill that grows the pool re-creates the table and every list survives. */
    ta_pool_counts before;
    ta_pool_counts_get(&envelopes.pool, &before);
    CHECK(ta_envelopes_refill(&envelopes, before.capacity + 1u) == TA_OK);
    ta_pool_counts after;
    ta_pool_counts_get(&envelopes.pool, &after);
    CHECK(after.capacity > before.capacity && ta_table_count(&envelopes.waiting) == 3u);

    /* Taking a list gives the first envelope, chained in arrival order, and forgets the address. */
    uint32_t head = ta_waiting_take_list(&envelopes, a);
    CHECK(head == a1 && ta_envelope_at(&envelopes, head)->waiting_tail == 0);
    CHECK(ta_ilist_next(ta_envelopes_link(&envelopes), a1) == a2);
    CHECK(ta_ilist_next(ta_envelopes_link(&envelopes), a2) == a3);
    CHECK(ta_ilist_next(ta_envelopes_link(&envelopes), a3) == 0);
    CHECK(ta_waiting_take_list(&envelopes, a) == 0);
    CHECK(ta_table_count(&envelopes.waiting) == 2u);
    /* A new envelope for the address begins a new list, and says so. */
    uint32_t a4 = wait_on(&envelopes, a, &first);
    CHECK(first && ta_waiting_take_list(&envelopes, a) == a4);
    ta_envelope_release(&envelopes, a4);
    /* The caller moves the taken chain to the run queue; its references go with the envelopes. */
    for (uint32_t index = head; index != 0;) {
        uint32_t next = ta_ilist_next(ta_envelopes_link(&envelopes), index);
        ta_envelope_release(&envelopes, index);
        index = next;
    }
    CHECK(a->references == references);
    CHECK(ta_waiting_take_list(&envelopes, b) == b1);
    ta_envelope_release(&envelopes, b1);
    /* Invalid inserts change nothing. */
    uint32_t spare = ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE);
    CHECK(ta_waiting_insert(&envelopes, spare, NULL, &first) == TA_ERR_INVALID);
    CHECK(ta_waiting_insert(&envelopes, 0, c, &first) == TA_ERR_INVALID);
    CHECK(ta_envelope_at(&envelopes, spare)->state == TA_ENVELOPE_RUNNABLE);
    ta_envelope_release(&envelopes, spare);

    /* Destroy gives the references of what still waits back (c1). */
    CHECK(c->references == references + 1u);
    ta_envelopes_destroy(&envelopes);
    CHECK(c->references == references);
    ta_address_release(a);
    ta_address_release(b);
    ta_address_release(c);
    ta_address_table_destroy(&addresses);
    CHECK(heap.stats.live_blocks == 0);
    (void)c1;
}

/* A refill that fails at its n-th allocation changes nothing; the same refill afterwards works. */
static void test_refill_failure(void) {
    ta_address_table addresses;
    ta_address_table_init(&addresses, ta_test_allocator_get(&heap));
    name entry = numbered(1u);
    ta_actor_address address = address_of(&entry);
    ta_address *a = NULL;
    CHECK(ta_address_table_intern(&addresses, &address, &a) == TA_OK);
    uint64_t baseline = heap.stats.live_blocks;
    uint64_t total = 0;
    for (uint64_t nth = 0; nth <= total; nth += 1u) {
        ta_envelopes envelopes;
        CHECK(open_pool(&envelopes, 1024u, 50u, 8u) == TA_OK);
        CHECK(ta_envelopes_refill(&envelopes, 4u) == TA_OK);
        bool first = false;
        uint32_t waiting = wait_on(&envelopes, a, &first);
        ta_pool_counts counts;
        ta_pool_counts_get(&envelopes.pool, &counts);
        size_t table_bytes = ta_table_bytes(&envelopes.waiting);
        ta_test_allocator_reset_counts(&heap);
        if (nth != 0) {
            ta_test_allocator_fail_nth(&heap, nth);
        }
        ta_result result = ta_envelopes_refill(&envelopes, 300u); /* needs a second page */
        ta_test_allocator_disarm(&heap);
        if (nth == 0) {
            CHECK(result == TA_OK);
            total = heap.stats.attempts;
            CHECK(total >= 3u);
        } else {
            CHECK(result == TA_ERR_NOMEM);
            ta_pool_counts now;
            ta_pool_counts_get(&envelopes.pool, &now);
            CHECK(now.capacity == counts.capacity && now.pages == counts.pages && now.used == counts.used);
            CHECK(ta_table_bytes(&envelopes.waiting) == table_bytes && ta_table_count(&envelopes.waiting) == 1u);
            CHECK(ta_waiting_take_list(&envelopes, a) == waiting); /* the list is intact */
            ta_envelope_release(&envelopes, waiting);
            CHECK(ta_envelopes_refill(&envelopes, 300u) == TA_OK && envelopes.pool.capacity >= 304u);
        }
        consistent(&envelopes);
        ta_envelopes_destroy(&envelopes);
        CHECK(heap.stats.live_blocks == baseline); /* only the address table's blocks remain */
    }
    ta_address_release(a);
    ta_address_table_destroy(&addresses);
    CHECK(heap.stats.live_blocks == 0);
    CHECK(total >= 3u);
}

int main(void) {
    test_creation();
    test_refill_and_ceiling();
    test_make_and_release();
    test_watermark();
    test_reserve();
    test_lists();
    test_waiting();
    test_refill_failure();
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "envelope_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("envelope_test: ok");
    return 0;
}
