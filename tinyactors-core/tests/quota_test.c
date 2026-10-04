/* test: standalone asan */
/*
 * Block C3 (src/quota.h): the quota with reservations.
 *
 * Covers the contract of each function, the overflow edges at capacity
 * UINT64_MAX, two holders that must not touch each other's reservation, an
 * exhaustive walk of every reachable state of a small system that checks the
 * invariants and the "a reservation can always be consumed" guarantee in each,
 * and a short seeded model test against plain variables. Fast, not
 * exhaustive: about twenty thousand random steps.
 *
 * Includes the amalgamation, because the unit is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

static bool same(const ta_quota *a, const ta_quota *b) {
    return a->capacity == b->capacity && a->used == b->used && a->reserved == b->reserved;
}

static void test_contract(void) {
    ta_quota q;
    ta_quota_claim c = { 0 };
    ta_quota_init(&q, 10);
    CHECK(q.capacity == 10 && q.used == 0 && q.reserved == 0 && ta_quota_available(&q) == 10);
    CHECK(ta_quota_reserve(&q, &c, 0) && c.held == 0 && q.reserved == 0);
    CHECK(ta_quota_reserve(&q, &c, 10) && c.held == 10 && q.reserved == 10 && ta_quota_available(&q) == 0);
    ta_quota before = q;
    ta_quota_claim other = { 0 };
    CHECK(!ta_quota_reserve(&q, &other, 1) && same(&q, &before) && other.held == 0);
    CHECK(ta_quota_reserve(&q, &other, 0));
    CHECK(!ta_quota_consume(&q, &c, 11) && c.held == 10 && same(&q, &before));
    CHECK(ta_quota_consume(&q, &c, 4) && c.held == 6 && q.reserved == 6 && q.used == 4);
    CHECK(!ta_quota_cancel(&q, &c, 7) && c.held == 6);
    CHECK(ta_quota_cancel(&q, &c, 2) && c.held == 4 && q.reserved == 4 && q.used == 4);
    CHECK(ta_quota_available(&q) == 2);
    CHECK(ta_quota_cancel_all(&q, &c) == 4 && c.held == 0 && q.reserved == 0);
    CHECK(!ta_quota_consume(&q, &c, 1) && !ta_quota_cancel(&q, &c, 1));
    CHECK(ta_quota_cancel_all(&q, &c) == 0);
    CHECK(!ta_quota_release(&q, 5) && q.used == 4);
    CHECK(ta_quota_release(&q, 4) && q.used == 0);
    CHECK(!ta_quota_release(&q, 1));
    CHECK(ta_quota_release(&q, 0));
}

static void test_overflow(void) {
    ta_quota q;
    ta_quota_claim c = { 0 };
    ta_quota_claim d = { 0 };
    ta_quota_init(&q, UINT64_MAX);
    CHECK(ta_quota_reserve(&q, &c, UINT64_MAX) && q.reserved == UINT64_MAX);
    CHECK(!ta_quota_reserve(&q, &d, 1));
    CHECK(ta_quota_consume(&q, &c, UINT64_MAX) && q.used == UINT64_MAX && q.reserved == 0);
    CHECK(!ta_quota_reserve(&q, &d, 1));
    CHECK(ta_quota_release(&q, UINT64_MAX) && q.used == 0);

    ta_quota_init(&q, UINT64_MAX);
    CHECK(ta_quota_reserve(&q, &d, 5) && ta_quota_consume(&q, &d, 5));
    ta_quota snapshot = q;
    ta_quota_claim e = { 0 };
    CHECK(!ta_quota_reserve(&q, &e, UINT64_MAX) && same(&q, &snapshot) && e.held == 0);
    CHECK(ta_quota_reserve(&q, &e, UINT64_MAX - 5) && ta_quota_available(&q) == 0);
}

static void test_two_holders(void) {
    ta_quota q;
    ta_quota_claim a = { 0 }, b = { 0 };
    ta_quota_init(&q, 100);
    CHECK(ta_quota_reserve(&q, &a, 60) && ta_quota_reserve(&q, &b, 3));
    ta_quota before = q;
    CHECK(!ta_quota_consume(&q, &b, 4) && !ta_quota_cancel(&q, &b, 4) && same(&q, &before) && a.held == 60);
    CHECK(ta_quota_consume(&q, &b, 3) && a.held == 60);
    CHECK(ta_quota_cancel_all(&q, &b) == 0);
    CHECK(ta_quota_consume(&q, &a, 60) && q.used == 63 && q.reserved == 0);
}

/* Exhaustive walk: capacity 4, two holders, every operation with n in 0..5. */
typedef struct {
    uint8_t used, a, b; /* reserved == a + b */
} state;

#define CAP 4u

static bool seen[CAP + 1][CAP + 1][CAP + 1];

static void load(const state *s, ta_quota *q, ta_quota_claim *claims) {
    ta_quota_init(q, CAP);
    q->used = s->used;
    q->reserved = (uint64_t)s->a + s->b;
    claims[0].held = s->a;
    claims[1].held = s->b;
}

static void test_state_space(void) {
    state queue[256];
    size_t head = 0, tail = 0, visited = 0;
    queue[tail++] = (state){ 0, 0, 0 };
    seen[0][0][0] = true;
    while (head < tail) {
        state s = queue[head++];
        visited += 1;
        for (unsigned op = 0; op < 5; op += 1) {
            for (uint64_t n = 0; n <= 5; n += 1) {
                for (unsigned who = 0; who < 2; who += 1) {
                    ta_quota q;
                    ta_quota_claim claims[2];
                    load(&s, &q, claims);
                    ta_quota before = q;
                    ta_quota_claim cbefore[2] = { claims[0], claims[1] };
                    bool ok = true;
                    switch (op) {
                    case 0: ok = ta_quota_reserve(&q, &claims[who], n); break;
                    case 1: ok = ta_quota_consume(&q, &claims[who], n); break;
                    case 2: ok = ta_quota_cancel(&q, &claims[who], n); break;
                    case 3: ok = ta_quota_release(&q, n); break;
                    default: (void)ta_quota_cancel_all(&q, &claims[who]); break;
                    }
                    if (!ok) {
                        CHECK(same(&q, &before) && claims[0].held == cbefore[0].held &&
                              claims[1].held == cbefore[1].held);
                    }
                    CHECK(q.used <= q.capacity && q.reserved <= q.capacity - q.used); /* I1 */
                    CHECK(q.reserved == claims[0].held + claims[1].held);             /* I2 */
                    /* The guarantee: whatever just happened, each holder can consume all it holds. */
                    for (unsigned h = 0; h < 2; h += 1) {
                        ta_quota copy = q;
                        ta_quota_claim cc = claims[h];
                        CHECK(ta_quota_consume(&copy, &cc, cc.held));
                    }
                    if (!seen[q.used][claims[0].held][claims[1].held]) {
                        seen[q.used][claims[0].held][claims[1].held] = true;
                        queue[tail++] = (state){ (uint8_t)q.used, (uint8_t)claims[0].held, (uint8_t)claims[1].held };
                    }
                }
            }
        }
    }
    /* Every (used, a, b) with used + a + b <= 4 is reachable: C(7,3) = 35 states. */
    CHECK(visited == 35u);
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void model_run(uint64_t capacity, uint64_t span) {
    ta_quota q;
    ta_quota_claim claims[3] = { { 0 }, { 0 }, { 0 } };
    uint64_t m_used = 0, m_held[3] = { 0, 0, 0 };
    ta_quota_init(&q, capacity);
    for (int step = 0; step < 5000 && failures == 0; step += 1) {
        unsigned h = (unsigned)(rng() % 3u);
        unsigned op = (unsigned)(rng() % 5u);
        uint64_t n = rng() % (span + 1u);
        if (rng() % 8u == 0) {
            n = UINT64_MAX - rng() % 3u;
        }
        uint64_t m_reserved = m_held[0] + m_held[1] + m_held[2];
        bool expect = false;
        bool ok = false;
        switch (op) {
        case 0:
            expect = n <= capacity - m_used - m_reserved;
            ok = ta_quota_reserve(&q, &claims[h], n);
            if (expect) m_held[h] += n;
            break;
        case 1:
            expect = n <= m_held[h];
            ok = ta_quota_consume(&q, &claims[h], n);
            if (expect) {
                m_held[h] -= n;
                m_used += n;
            }
            break;
        case 2:
            expect = n <= m_held[h];
            ok = ta_quota_cancel(&q, &claims[h], n);
            if (expect) m_held[h] -= n;
            break;
        case 3:
            expect = n <= m_used;
            ok = ta_quota_release(&q, n);
            if (expect) m_used -= n;
            break;
        default:
            expect = true;
            ok = ta_quota_cancel_all(&q, &claims[h]) == m_held[h];
            m_held[h] = 0;
            break;
        }
        CHECK(ok == expect);
        CHECK(q.used == m_used && claims[0].held == m_held[0] && claims[1].held == m_held[1] &&
              claims[2].held == m_held[2] && q.reserved == m_held[0] + m_held[1] + m_held[2] &&
              q.capacity == capacity);
    }
}

static void test_model(void) {
    model_run(10, 6);
    model_run(1000, 400);
    model_run(UINT64_MAX, UINT64_MAX / 4);
    model_run(UINT64_MAX - 1, 1000);
}

int main(void) {
    test_contract();
    test_overflow();
    test_two_holders();
    test_state_space();
    test_model();
    if (failures != 0) {
        fprintf(stderr, "quota_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("quota_test: ok");
    return 0;
}
