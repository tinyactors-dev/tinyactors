/*
 * ta_quota: the quota with reservations (src/quota.h), which counts the
 * c2h mandatory reserve and headroom, the pool reserve and admission
 * watermark, the internal queue's ordinary quota and mandatory floor,
 * and the park area's reserve.
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not.
 *
 * Claims proven here, in order:
 *
 * 1. make gives {capacity, 0, 0}, with all of it available; a ta_quota
 *    is 12 bytes and a claim 4; a quota of capacity 0 grants an amount
 *    of 0 and refuses 1 with TA_ERR_FULL;
 * 2. reserve grants exactly what fits, up to the whole capacity, adding
 *    the amount to the quota's reserved and to the claim; one more unit
 *    answers TA_ERR_FULL and changes neither;
 * 3. keep: reserve and take succeed only when amount + keep fits in the
 *    available room, at the exact boundary and not one beyond, and keep
 *    is not set aside; amounts and keeps near 2^32 - 1 do not overflow;
 * 4. consume moves an amount from reserved to used, and from the claim,
 *    in parts and as a whole; consuming more than the claim holds
 *    answers TA_ERR_INVALID and changes nothing, even when the quota's
 *    other holders have reserved enough;
 * 5. cancel returns part or all of a claim to the available room; more
 *    than the claim holds answers TA_ERR_INVALID and changes nothing;
 * 6. take uses room directly and never takes room a holder reserved: a
 *    take that would reach into another holder's reservation answers
 *    TA_ERR_FULL and changes nothing;
 * 7. release returns used room, whether taken or consumed; more than is
 *    used answers TA_ERR_INVALID and changes nothing;
 * 8. a holder that pauses keeps its claim: while it holds it, other
 *    holders and takes fill the rest and are refused, and the paused
 *    holder's consume still succeeds;
 * 9. an amount of 0 always succeeds and changes nothing, on a full quota
 *    too;
 * 10. the c2h shape: slots reserve their mandatory batches with the
 *     headroom of 2 records as keep; the slot that would leave less is
 *     refused, while 2 records of room stay free for an ordinary write
 *     that fits beside every reservation;
 * 11. the internal queue shape: a mandatory event takes the floor first,
 *     then a free ordinary entry, and an ordinary raise is refused at its
 *     quota and never uses the floor;
 * 12. a run of 10000 random reserves, consumes, cancels, takes and
 *     releases by four holders agrees with a model at every step, and
 *     keeps used + reserved <= capacity and reserved equal to the sum of
 *     the claims;
 * 13. resize keeps used, reserved and every claim; it grows freely and
 *     shrinks down to exactly used + reserved; below that it answers
 *     TA_ERR_FULL and changes nothing; room it adds is available at once.
 */
#include "../include/tinyactors.c"

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

static ta_quota_request ask(uint32_t amount) {
    return (ta_quota_request){ .amount = amount, .keep = 0u };
}

static ta_quota_request ask_keeping(uint32_t amount, uint32_t keep) {
    return (ta_quota_request){ .amount = amount, .keep = keep };
}

static bool quota_is(const ta_quota *quota, uint32_t capacity, uint32_t used, uint32_t reserved) {
    return quota->capacity == capacity && quota->used == used && quota->reserved == reserved;
}

/* Claim 1. */
static void test_make(void) {
    ta_quota quota = ta_quota_make(100u);
    CHECK(quota_is(&quota, 100u, 0u, 0u));
    CHECK(ta_quota_available(&quota) == 100u);
    CHECK(sizeof(ta_quota) == 12u);
    CHECK(sizeof(ta_quota_claim) == 4u);

    ta_quota empty = ta_quota_make(0u);
    ta_quota_claim claim = { 0u };
    CHECK(ta_quota_available(&empty) == 0u);
    CHECK(ta_quota_fits(&empty, ask(0u)));
    CHECK(!ta_quota_fits(&empty, ask(1u)));
    CHECK(ta_quota_reserve(&empty, &claim, ask(0u)) == TA_OK);
    CHECK(ta_quota_reserve(&empty, &claim, ask(1u)) == TA_ERR_FULL);
    CHECK(ta_quota_take(&empty, ask(1u)) == TA_ERR_FULL);
    CHECK(quota_is(&empty, 0u, 0u, 0u));
    CHECK(claim.reserved == 0u);
}

/* Claim 2. */
static void test_reserve(void) {
    ta_quota quota = ta_quota_make(10u);
    ta_quota_claim a = { 0u };
    ta_quota_claim b = { 0u };

    CHECK(ta_quota_reserve(&quota, &a, ask(4u)) == TA_OK);
    CHECK(quota_is(&quota, 10u, 0u, 4u));
    CHECK(a.reserved == 4u);
    CHECK(ta_quota_reserve(&quota, &a, ask(1u)) == TA_OK);
    CHECK(a.reserved == 5u);
    CHECK(ta_quota_reserve(&quota, &b, ask(5u)) == TA_OK);
    CHECK(quota_is(&quota, 10u, 0u, 10u));
    CHECK(b.reserved == 5u);
    CHECK(ta_quota_available(&quota) == 0u);

    CHECK(ta_quota_reserve(&quota, &b, ask(1u)) == TA_ERR_FULL);
    CHECK(quota_is(&quota, 10u, 0u, 10u));
    CHECK(a.reserved == 5u);
    CHECK(b.reserved == 5u);

    ta_quota whole = ta_quota_make(UINT32_MAX);
    ta_quota_claim c = { 0u };
    CHECK(ta_quota_reserve(&whole, &c, ask(UINT32_MAX)) == TA_OK);
    CHECK(quota_is(&whole, UINT32_MAX, 0u, UINT32_MAX));
    CHECK(ta_quota_reserve(&whole, &c, ask(1u)) == TA_ERR_FULL);
    CHECK(c.reserved == UINT32_MAX);
}

/* Claim 3. */
static void test_keep(void) {
    ta_quota quota = ta_quota_make(10u);
    ta_quota_claim claim = { 0u };

    CHECK(ta_quota_fits(&quota, ask_keeping(7u, 3u)));
    CHECK(!ta_quota_fits(&quota, ask_keeping(7u, 4u)));
    CHECK(!ta_quota_fits(&quota, ask_keeping(8u, 3u)));
    CHECK(ta_quota_reserve(&quota, &claim, ask_keeping(8u, 3u)) == TA_ERR_FULL);
    CHECK(quota_is(&quota, 10u, 0u, 0u));
    CHECK(claim.reserved == 0u);
    CHECK(ta_quota_reserve(&quota, &claim, ask_keeping(7u, 3u)) == TA_OK);
    CHECK(quota_is(&quota, 10u, 0u, 7u));
    CHECK(claim.reserved == 7u);
    /* keep was not set aside: the 3 are still there for a plain take. */
    CHECK(ta_quota_available(&quota) == 3u);
    CHECK(ta_quota_take(&quota, ask_keeping(1u, 3u)) == TA_ERR_FULL);
    CHECK(quota_is(&quota, 10u, 0u, 7u));
    CHECK(ta_quota_take(&quota, ask_keeping(1u, 2u)) == TA_OK);
    CHECK(quota_is(&quota, 10u, 1u, 7u));
    CHECK(ta_quota_take(&quota, ask(2u)) == TA_OK);
    CHECK(quota_is(&quota, 10u, 3u, 7u));

    /* A keep larger than the room, alone, refuses even 0. */
    ta_quota small = ta_quota_make(5u);
    CHECK(!ta_quota_fits(&small, ask_keeping(0u, 6u)));
    CHECK(ta_quota_take(&small, ask_keeping(0u, 6u)) == TA_ERR_FULL);
    CHECK(ta_quota_fits(&small, ask_keeping(0u, 5u)));

    /* No overflow of amount + keep near the top of uint32_t. */
    ta_quota big = ta_quota_make(UINT32_MAX);
    CHECK(!ta_quota_fits(&big, ask_keeping(UINT32_MAX, 1u)));
    CHECK(!ta_quota_fits(&big, ask_keeping(1u, UINT32_MAX)));
    CHECK(!ta_quota_fits(&big, ask_keeping(UINT32_MAX, UINT32_MAX)));
    CHECK(ta_quota_fits(&big, ask_keeping(UINT32_MAX - 1u, 1u)));
    CHECK(ta_quota_take(&big, ask_keeping(UINT32_MAX, 1u)) == TA_ERR_FULL);
    CHECK(quota_is(&big, UINT32_MAX, 0u, 0u));
}

/* Claim 4. */
static void test_consume(void) {
    ta_quota quota = ta_quota_make(10u);
    ta_quota_claim a = { 0u };
    ta_quota_claim b = { 0u };
    CHECK(ta_quota_reserve(&quota, &a, ask(4u)) == TA_OK);
    CHECK(ta_quota_reserve(&quota, &b, ask(5u)) == TA_OK);

    CHECK(ta_quota_consume(&quota, &a, 1u) == TA_OK);
    CHECK(quota_is(&quota, 10u, 1u, 8u));
    CHECK(a.reserved == 3u);
    CHECK(ta_quota_consume(&quota, &a, 3u) == TA_OK);
    CHECK(quota_is(&quota, 10u, 4u, 5u));
    CHECK(a.reserved == 0u);

    /* b's reservation does not cover a. */
    CHECK(ta_quota_consume(&quota, &a, 1u) == TA_ERR_INVALID);
    CHECK(quota_is(&quota, 10u, 4u, 5u));
    CHECK(a.reserved == 0u);
    CHECK(b.reserved == 5u);

    CHECK(ta_quota_consume(&quota, &b, 6u) == TA_ERR_INVALID);
    CHECK(quota_is(&quota, 10u, 4u, 5u));
    CHECK(b.reserved == 5u);
    CHECK(ta_quota_consume(&quota, &b, 5u) == TA_OK);
    CHECK(quota_is(&quota, 10u, 9u, 0u));
    CHECK(b.reserved == 0u);
}

/* Claim 5. */
static void test_cancel(void) {
    ta_quota quota = ta_quota_make(10u);
    ta_quota_claim a = { 0u };
    CHECK(ta_quota_reserve(&quota, &a, ask(6u)) == TA_OK);
    CHECK(ta_quota_consume(&quota, &a, 1u) == TA_OK);

    CHECK(ta_quota_cancel(&quota, &a, 6u) == TA_ERR_INVALID);
    CHECK(quota_is(&quota, 10u, 1u, 5u));
    CHECK(a.reserved == 5u);

    CHECK(ta_quota_cancel(&quota, &a, 2u) == TA_OK);
    CHECK(quota_is(&quota, 10u, 1u, 3u));
    CHECK(a.reserved == 3u);
    CHECK(ta_quota_available(&quota) == 6u);

    CHECK(ta_quota_cancel(&quota, &a, a.reserved) == TA_OK);
    CHECK(quota_is(&quota, 10u, 1u, 0u));
    CHECK(a.reserved == 0u);
    CHECK(ta_quota_available(&quota) == 9u);
}

/* Claim 6. */
static void test_take(void) {
    ta_quota quota = ta_quota_make(10u);
    ta_quota_claim a = { 0u };
    CHECK(ta_quota_reserve(&quota, &a, ask(6u)) == TA_OK);

    CHECK(ta_quota_take(&quota, ask(3u)) == TA_OK);
    CHECK(quota_is(&quota, 10u, 3u, 6u));
    CHECK(ta_quota_take(&quota, ask(2u)) == TA_ERR_FULL);
    CHECK(quota_is(&quota, 10u, 3u, 6u));
    CHECK(ta_quota_take(&quota, ask(1u)) == TA_OK);
    CHECK(quota_is(&quota, 10u, 4u, 6u));
    CHECK(ta_quota_take(&quota, ask(1u)) == TA_ERR_FULL);
    CHECK(quota_is(&quota, 10u, 4u, 6u));
    CHECK(a.reserved == 6u);

    /* The holder still gets all it reserved. */
    CHECK(ta_quota_consume(&quota, &a, 6u) == TA_OK);
    CHECK(quota_is(&quota, 10u, 10u, 0u));
}

/* Claim 7. */
static void test_release(void) {
    ta_quota quota = ta_quota_make(10u);
    ta_quota_claim a = { 0u };
    CHECK(ta_quota_take(&quota, ask(3u)) == TA_OK);
    CHECK(ta_quota_reserve(&quota, &a, ask(4u)) == TA_OK);
    CHECK(ta_quota_consume(&quota, &a, 4u) == TA_OK);
    CHECK(quota_is(&quota, 10u, 7u, 0u));

    CHECK(ta_quota_release(&quota, 8u) == TA_ERR_INVALID);
    CHECK(quota_is(&quota, 10u, 7u, 0u));
    CHECK(ta_quota_release(&quota, 4u) == TA_OK);
    CHECK(quota_is(&quota, 10u, 3u, 0u));
    CHECK(ta_quota_release(&quota, 3u) == TA_OK);
    CHECK(quota_is(&quota, 10u, 0u, 0u));
    CHECK(ta_quota_release(&quota, 1u) == TA_ERR_INVALID);
    CHECK(quota_is(&quota, 10u, 0u, 0u));
    CHECK(ta_quota_available(&quota) == 10u);
}

/* Claim 8. */
static void test_paused_holder(void) {
    ta_quota quota = ta_quota_make(12u);
    ta_quota_claim paused = { 0u };
    ta_quota_claim other = { 0u };
    CHECK(ta_quota_reserve(&quota, &paused, ask(5u)) == TA_OK);
    CHECK(ta_quota_consume(&quota, &paused, 1u) == TA_OK);

    /* While it is paused, the others fill everything else. */
    CHECK(ta_quota_reserve(&quota, &other, ask(5u)) == TA_OK);
    CHECK(ta_quota_take(&quota, ask(2u)) == TA_OK);
    CHECK(ta_quota_available(&quota) == 0u);
    CHECK(ta_quota_take(&quota, ask(1u)) == TA_ERR_FULL);
    CHECK(ta_quota_reserve(&quota, &other, ask(1u)) == TA_ERR_FULL);
    CHECK(ta_quota_consume(&quota, &other, 5u) == TA_OK);
    CHECK(ta_quota_take(&quota, ask(1u)) == TA_ERR_FULL);

    /* The claim moves with its holder: a copy is as good as the original. */
    ta_quota_claim moved;
    memcpy(&moved, &paused, sizeof(moved));
    memset(&paused, 0xab, sizeof(paused));
    CHECK(moved.reserved == 4u);
    CHECK(ta_quota_consume(&quota, &moved, 4u) == TA_OK);
    CHECK(quota_is(&quota, 12u, 12u, 0u));
}

/* Claim 9. */
static void test_zero(void) {
    ta_quota quota = ta_quota_make(3u);
    ta_quota_claim claim = { 0u };
    CHECK(ta_quota_take(&quota, ask(3u)) == TA_OK);

    CHECK(ta_quota_fits(&quota, ask(0u)));
    CHECK(ta_quota_reserve(&quota, &claim, ask(0u)) == TA_OK);
    CHECK(ta_quota_take(&quota, ask(0u)) == TA_OK);
    CHECK(ta_quota_consume(&quota, &claim, 0u) == TA_OK);
    CHECK(ta_quota_cancel(&quota, &claim, 0u) == TA_OK);
    CHECK(ta_quota_release(&quota, 0u) == TA_OK);
    CHECK(quota_is(&quota, 3u, 3u, 0u));
    CHECK(claim.reserved == 0u);
}

/* Claim 10. */
static void test_c2h_shape(void) {
    const uint32_t record = 4096u;  /* maxRecordBytes */
    const uint32_t pad = record - 8u;
    const uint32_t headroom = 2u * record;
    /* Smallest batch: input, macrostep_ended, input_completed, each with
     * its pad. */
    const uint32_t batch = 3u * (64u + pad);
    /* Room for three batches, but one byte short of the headroom beside
     * them. */
    ta_quota c2h = ta_quota_make(3u * batch + headroom - 1u);
    ta_quota_claim slots[3] = { { 0u }, { 0u }, { 0u } };

    CHECK(ta_quota_reserve(&c2h, &slots[0], ask_keeping(batch, headroom)) == TA_OK);
    CHECK(ta_quota_reserve(&c2h, &slots[1], ask_keeping(batch, headroom)) == TA_OK);
    /* A third batch would leave less than the headroom. */
    CHECK(ta_quota_available(&c2h) >= batch);
    CHECK(ta_quota_reserve(&c2h, &slots[2], ask_keeping(batch, headroom)) == TA_ERR_FULL);
    CHECK(slots[2].reserved == 0u);
    CHECK(c2h.reserved == 2u * batch);

    /* An ordinary record of the largest size and its pad still fits. */
    CHECK(ta_quota_fits(&c2h, ask(record + pad)));
    CHECK(ta_quota_take(&c2h, ask(record + pad)) == TA_OK);

    /* One input completes: its reserve comes back and the waiting slot fits. */
    CHECK(ta_quota_consume(&c2h, &slots[0], 64u) == TA_OK);
    CHECK(ta_quota_cancel(&c2h, &slots[0], slots[0].reserved) == TA_OK);
    CHECK(ta_quota_release(&c2h, 64u + record + pad) == TA_OK);
    CHECK(ta_quota_reserve(&c2h, &slots[2], ask_keeping(batch, headroom)) == TA_OK);
    CHECK(c2h.reserved == 2u * batch);
}

/* Claim 11. */
static ta_result mandatory_enqueue(ta_quota *floor, ta_quota *ordinary, int *where) {
    if (ta_quota_take(floor, ask(1u)) == TA_OK) {
        *where = 1;
        return TA_OK;
    }
    if (ta_quota_take(ordinary, ask(1u)) == TA_OK) {
        *where = 2;
        return TA_OK;
    }
    return TA_ERR_FULL;
}

static void test_internal_queue_shape(void) {
    ta_quota ordinary = ta_quota_make(3u);
    ta_quota floor = ta_quota_make(2u);
    int where = 0;

    CHECK(ta_quota_take(&ordinary, ask(1u)) == TA_OK);
    CHECK(ta_quota_take(&ordinary, ask(1u)) == TA_OK);
    CHECK(mandatory_enqueue(&floor, &ordinary, &where) == TA_OK && where == 1);
    CHECK(mandatory_enqueue(&floor, &ordinary, &where) == TA_OK && where == 1);
    CHECK(mandatory_enqueue(&floor, &ordinary, &where) == TA_OK && where == 2);
    CHECK(quota_is(&floor, 2u, 2u, 0u));
    CHECK(quota_is(&ordinary, 3u, 3u, 0u));

    /* The ordinary quota is full: a raise is refused, the floor is untouched. */
    CHECK(ta_quota_take(&ordinary, ask(1u)) == TA_ERR_FULL);
    CHECK(quota_is(&floor, 2u, 2u, 0u));
    /* A mandatory event beyond both is the runaway. */
    CHECK(mandatory_enqueue(&floor, &ordinary, &where) == TA_ERR_FULL);

    /* With the floor free and the ordinary quota full, a raise still fails. */
    CHECK(ta_quota_release(&floor, 2u) == TA_OK);
    CHECK(ta_quota_take(&ordinary, ask(1u)) == TA_ERR_FULL);
    CHECK(quota_is(&floor, 2u, 0u, 0u));
}

/* Claim 12. */
#define HOLDERS 4u

static uint32_t next_random(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state >> 8;
}

static void test_random(void) {
    const uint32_t capacity = 64u;
    ta_quota quota = ta_quota_make(capacity);
    ta_quota_claim claims[HOLDERS];
    memset(claims, 0, sizeof(claims));
    uint32_t model_used = 0u;
    uint32_t model_claims[HOLDERS] = { 0u };
    uint32_t seed = 12345u;

    for (uint32_t step = 0u; step < 10000u; step++) {
        uint32_t holder = next_random(&seed) % HOLDERS;
        uint32_t amount = next_random(&seed) % 20u;
        uint32_t keep = (next_random(&seed) % 3u == 0u) ? next_random(&seed) % 10u : 0u;
        uint32_t model_reserved = 0u;
        for (uint32_t i = 0u; i < HOLDERS; i++) {
            model_reserved += model_claims[i];
        }
        uint32_t model_available = capacity - model_used - model_reserved;
        bool fits = (uint64_t)amount + keep <= model_available;
        ta_result result;
        ta_result expected;
        switch (next_random(&seed) % 5u) {
        case 0:
            result = ta_quota_reserve(&quota, &claims[holder], ask_keeping(amount, keep));
            expected = fits ? TA_OK : TA_ERR_FULL;
            if (fits) {
                model_claims[holder] += amount;
            }
            break;
        case 1:
            result = ta_quota_consume(&quota, &claims[holder], amount);
            expected = amount <= model_claims[holder] ? TA_OK : TA_ERR_INVALID;
            if (expected == TA_OK) {
                model_claims[holder] -= amount;
                model_used += amount;
            }
            break;
        case 2:
            result = ta_quota_cancel(&quota, &claims[holder], amount);
            expected = amount <= model_claims[holder] ? TA_OK : TA_ERR_INVALID;
            if (expected == TA_OK) {
                model_claims[holder] -= amount;
            }
            break;
        case 3:
            result = ta_quota_take(&quota, ask_keeping(amount, keep));
            expected = fits ? TA_OK : TA_ERR_FULL;
            if (fits) {
                model_used += amount;
            }
            break;
        default:
            result = ta_quota_release(&quota, amount);
            expected = amount <= model_used ? TA_OK : TA_ERR_INVALID;
            if (expected == TA_OK) {
                model_used -= amount;
            }
            break;
        }
        CHECK(result == expected);
        model_reserved = 0u;
        uint32_t claimed = 0u;
        for (uint32_t i = 0u; i < HOLDERS; i++) {
            model_reserved += model_claims[i];
            claimed += claims[i].reserved;
            CHECK(claims[i].reserved == model_claims[i]);
        }
        CHECK(quota_is(&quota, capacity, model_used, model_reserved));
        CHECK(quota.reserved == claimed);
        CHECK(quota.used + quota.reserved <= quota.capacity);
        if (failures > 0) {
            fprintf(stderr, "random run diverged at step %u\n", step);
            return;
        }
    }
}

/* 13 */
static void test_resize(void) {
    ta_quota q = ta_quota_make(10u);
    ta_quota_claim slot = {0};
    CHECK(ta_quota_reserve(&q, &slot, (ta_quota_request){ .amount = 3u }) == TA_OK);
    CHECK(ta_quota_take(&q, (ta_quota_request){ .amount = 4u }) == TA_OK);
    CHECK(ta_quota_available(&q) == 3u);

    /* Grow: the new room is available at once; used, reserved and the
     * claim stay. */
    CHECK(ta_quota_resize(&q, 20u) == TA_OK);
    CHECK(q.capacity == 20u && q.used == 4u && q.reserved == 3u && slot.reserved == 3u);
    CHECK(ta_quota_available(&q) == 13u);

    /* Shrink to exactly used + reserved, then below it: refused. */
    CHECK(ta_quota_resize(&q, 7u) == TA_OK);
    CHECK(ta_quota_available(&q) == 0u);
    CHECK(ta_quota_resize(&q, 6u) == TA_ERR_FULL);
    CHECK(q.capacity == 7u && q.used == 4u && q.reserved == 3u && slot.reserved == 3u);
    CHECK(ta_quota_resize(&q, 0u) == TA_ERR_FULL);
    CHECK(q.capacity == 7u);

    /* The claim still works after the resizes. */
    CHECK(ta_quota_consume(&q, &slot, 3u) == TA_OK);
    CHECK(q.used == 7u && q.reserved == 0u && slot.reserved == 0u);
    CHECK(ta_quota_release(&q, 7u) == TA_OK);
    CHECK(ta_quota_resize(&q, 0u) == TA_OK);
    CHECK(ta_quota_available(&q) == 0u);
    CHECK(ta_quota_take(&q, (ta_quota_request){ .amount = 1u }) == TA_ERR_FULL);

    /* The largest capacity. */
    CHECK(ta_quota_resize(&q, UINT32_MAX) == TA_OK);
    CHECK(ta_quota_available(&q) == UINT32_MAX);
}

int main(void) {
    test_make();
    test_reserve();
    test_keep();
    test_consume();
    test_cancel();
    test_take();
    test_release();
    test_paused_holder();
    test_zero();
    test_c2h_shape();
    test_internal_queue_shape();
    test_random();
    test_resize();
    if (failures > 0) {
        fprintf(stderr, "quota_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("quota_test: ok\n");
    return 0;
}
