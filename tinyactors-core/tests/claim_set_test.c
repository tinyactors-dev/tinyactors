/* test: standalone asan */
/*
 * Block C4 (src/claim_set.h): the claim set over C3 quotas.
 *
 * Covers: all adds succeed; each position failing (nothing kept, the right
 * resource named); zero-sized and duplicate-quota entries; overflow edges; the
 * full set; abort exactness; commit and the used counts; the stickiness of
 * `failed`; and a short seeded model test against plain integers. Fast, not
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

static const char *const names[5] = { "envelope", "name", "record", "token", "spare" };

static bool add(ta_claim_set *set, ta_quota *q, uint64_t n, unsigned name) {
    return ta_claim_set_add(set, (ta_claim_request){ .quota = q, .n = n, .resource = names[name] });
}

static void test_all_succeed(void) {
    ta_quota q[3];
    ta_claim_set set;
    for (int i = 0; i < 3; i += 1) {
        ta_quota_init(&q[i], 10);
    }
    ta_claim_set_init(&set);
    CHECK(ta_claim_set_idle(&set) && ta_claim_set_failed(&set) == NULL);
    CHECK(add(&set, &q[0], 1, 0) && add(&set, &q[1], 2, 1) && add(&set, &q[2], 3, 2));
    CHECK(set.count == 3 && !ta_claim_set_idle(&set) && ta_claim_set_failed(&set) == NULL);
    CHECK(q[0].reserved == 1 && q[1].reserved == 2 && q[2].reserved == 3);
    CHECK(q[0].used == 0 && q[1].used == 0 && q[2].used == 0);
    ta_claim_set_commit(&set);
    CHECK(ta_claim_set_idle(&set));
    CHECK(q[0].used == 1 && q[1].used == 2 && q[2].used == 3);
    CHECK(q[0].reserved == 0 && q[1].reserved == 0 && q[2].reserved == 0);
    CHECK(ta_quota_available(&q[0]) == 9 && ta_quota_available(&q[1]) == 8 && ta_quota_available(&q[2]) == 7);
}

/* Position i is the one that runs out, with several preparations of the quotas. */
static void test_each_position_fails(void) {
    for (unsigned kind = 0; kind < 3; kind += 1) {
        for (unsigned k = 1; k <= TA_CLAIM_SET_MAX; k += 1) {
            for (unsigned fail = 0; fail < k; fail += 1) {
                ta_quota q[TA_CLAIM_SET_MAX];
                ta_quota_claim other[TA_CLAIM_SET_MAX] = { { 0 } };
                for (unsigned i = 0; i < TA_CLAIM_SET_MAX; i += 1) {
                    ta_quota_init(&q[i], 10);
                    if (kind == 1) {
                        CHECK(ta_quota_reserve(&q[i], &other[i], 2) && ta_quota_consume(&q[i], &other[i], 2));
                    } else if (kind == 2) {
                        CHECK(ta_quota_reserve(&q[i], &other[i], 3));
                    }
                }
                ta_quota before[TA_CLAIM_SET_MAX];
                for (unsigned i = 0; i < TA_CLAIM_SET_MAX; i += 1) {
                    before[i] = q[i];
                }
                ta_claim_set set;
                ta_claim_set_init(&set);
                bool ok = true;
                for (unsigned i = 0; i < k && ok; i += 1) {
                    uint64_t want = i == fail ? ta_quota_available(&q[i]) + 1 : 1;
                    ok = add(&set, &q[i], want, i);
                    CHECK(ok == (i != fail));
                }
                CHECK(!ok);
                CHECK(ta_claim_set_failed(&set) == names[fail]);
                CHECK(set.count == 0 && ta_claim_set_idle(&set));
                for (unsigned i = 0; i < TA_CLAIM_SET_MAX; i += 1) {
                    CHECK(same(&q[i], &before[i]));
                    CHECK(other[i].held == (kind == 2 ? 3u : 0u));
                }
            }
        }
    }
}

static void test_zero_and_duplicate(void) {
    ta_quota q;
    ta_claim_set set;
    ta_quota_init(&q, 4);
    ta_quota_claim holder = { 0 };
    CHECK(ta_quota_reserve(&q, &holder, 4));
    ta_claim_set_init(&set);
    CHECK(add(&set, &q, 0, 0) && set.count == 1);
    CHECK(add(&set, &q, 0, 1) && set.count == 2);
    CHECK(q.reserved == 4);
    ta_claim_set_commit(&set);
    CHECK(q.used == 0 && q.reserved == 4);
    ta_quota_cancel_all(&q, &holder);

    /* The same quota twice: each fits alone, not together. */
    ta_claim_set_init(&set);
    CHECK(add(&set, &q, 3, 0));
    ta_quota before = q;
    CHECK(!add(&set, &q, 2, 1));
    CHECK(ta_claim_set_failed(&set) == names[1] && set.count == 0);
    CHECK(q.used == 0 && q.reserved == 0 && q.capacity == before.capacity);
    CHECK(add(&set, &q, 2, 0) && add(&set, &q, 2, 1));
    ta_claim_set_abort(&set);
    CHECK(q.used == 0 && q.reserved == 0);
}

static void test_overflow(void) {
    ta_quota big, small;
    ta_claim_set set;
    ta_quota_init(&big, UINT64_MAX);
    ta_quota_init(&small, UINT64_MAX - 1);
    ta_claim_set_init(&set);
    CHECK(add(&set, &big, UINT64_MAX, 0) && big.reserved == UINT64_MAX);
    CHECK(add(&set, &small, UINT64_MAX - 1, 1) && small.reserved == UINT64_MAX - 1);
    ta_claim_set_abort(&set);
    CHECK(big.reserved == 0);
    ta_claim_set_init(&set);
    CHECK(!add(&set, &small, UINT64_MAX, 2));
    CHECK(small.reserved == 0 && small.used == 0 && ta_claim_set_failed(&set) == names[2]);
    CHECK(add(&set, &big, 5, 0));
    CHECK(!add(&set, &big, UINT64_MAX, 1));
    CHECK(big.reserved == 0 && set.count == 0);
    CHECK(add(&set, &big, UINT64_MAX, 0));
    ta_claim_set_commit(&set);
    CHECK(big.used == UINT64_MAX && ta_quota_available(&big) == 0);
}

static void test_full_set(void) {
    ta_quota q;
    ta_claim_set set;
    ta_quota_init(&q, 100);
    ta_claim_set_init(&set);
    for (unsigned i = 0; i < TA_CLAIM_SET_MAX; i += 1) {
        CHECK(add(&set, &q, 1, i));
    }
    CHECK(q.reserved == TA_CLAIM_SET_MAX);
    CHECK(!add(&set, &q, 1, 4));
    CHECK(ta_claim_set_failed(&set) == NULL && set.count == 0);
    CHECK(q.reserved == 0 && q.used == 0);
}

static void test_abort_and_commit(void) {
    ta_quota q[2];
    ta_claim_set set;
    ta_quota_init(&q[0], 9);
    ta_quota_init(&q[1], 9);
    ta_quota_claim h = { 0 };
    CHECK(ta_quota_reserve(&q[0], &h, 2) && ta_quota_consume(&q[0], &h, 1));
    ta_quota before[2] = { q[0], q[1] };
    ta_claim_set_init(&set);
    CHECK(add(&set, &q[0], 3, 0) && add(&set, &q[1], 4, 1));
    ta_claim_set_abort(&set);
    CHECK(same(&q[0], &before[0]) && same(&q[1], &before[1]) && h.held == 1);
    ta_claim_set_abort(&set);
    CHECK(same(&q[0], &before[0]) && same(&q[1], &before[1]) && ta_claim_set_idle(&set));
    ta_claim_set_commit(&set);
    CHECK(same(&q[0], &before[0]) && same(&q[1], &before[1]));

    CHECK(add(&set, &q[0], 3, 0) && add(&set, &q[1], 4, 1));
    ta_claim_set_commit(&set);
    CHECK(q[0].used == 4 && q[1].used == 4 && q[0].reserved == 1 && q[1].reserved == 0);
    CHECK(ta_claim_set_idle(&set));
    CHECK(ta_quota_release(&q[0], 3) && ta_quota_release(&q[1], 4));
    CHECK(same(&q[0], &before[0]) && same(&q[1], &before[1]));
    CHECK(ta_quota_available(&q[1]) == 9);
}

static void test_failed_is_sticky(void) {
    ta_quota q;
    ta_claim_set set;
    ta_quota_init(&q, 2);
    ta_claim_set_init(&set);
    CHECK(!add(&set, &q, 3, 3));
    CHECK(add(&set, &q, 1, 0));
    CHECK(ta_claim_set_failed(&set) == names[3]);
    ta_claim_set_commit(&set);
    CHECK(ta_claim_set_failed(&set) == names[3]);
    CHECK(!add(&set, &q, 9, 2) && ta_claim_set_failed(&set) == names[2]);
    ta_claim_set_init(&set);
    CHECK(ta_claim_set_failed(&set) == NULL);
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_model(void) {
    enum { QUOTAS = 3 };
    const uint64_t capacity[QUOTAS] = { 6, 20, 1000 };
    ta_quota q[QUOTAS];
    uint64_t m_used[QUOTAS] = { 0, 0, 0 };
    for (int i = 0; i < QUOTAS; i += 1) {
        ta_quota_init(&q[i], capacity[i]);
    }
    for (int step = 0; step < 20000 && failures == 0; step += 1) {
        ta_claim_set set;
        ta_claim_set_init(&set);
        uint64_t m_held[QUOTAS] = { 0, 0, 0 };
        unsigned adds = 1u + (unsigned)(rng() % 4u);
        bool ok = true;
        unsigned failed_at = 0;
        for (unsigned a = 0; a < adds && ok; a += 1) {
            unsigned i = (unsigned)(rng() % QUOTAS);
            uint64_t n = rng() % 5u;
            if (rng() % 16u == 0) {
                n = UINT64_MAX - rng() % 3u;
            }
            bool expect = n <= capacity[i] - m_used[i] - m_held[i];
            ok = add(&set, &q[i], n, a);
            CHECK(ok == expect);
            if (ok) {
                m_held[i] += n;
            } else {
                failed_at = a;
            }
        }
        if (!ok) {
            CHECK(ta_claim_set_failed(&set) == names[failed_at] && ta_claim_set_idle(&set));
        } else if (rng() % 2u == 0) {
            ta_claim_set_commit(&set);
            for (int i = 0; i < QUOTAS; i += 1) {
                m_used[i] += m_held[i];
            }
        } else {
            ta_claim_set_abort(&set);
        }
        if (ok && !ta_claim_set_idle(&set)) {
            ta_claim_set_abort(&set);
        }
        unsigned r = (unsigned)(rng() % QUOTAS);
        uint64_t give = rng() % 4u;
        if (give <= m_used[r]) {
            CHECK(ta_quota_release(&q[r], give));
            m_used[r] -= give;
        }
        for (int i = 0; i < QUOTAS; i += 1) {
            CHECK(q[i].reserved == 0 && q[i].used == m_used[i]);
            CHECK(ta_quota_available(&q[i]) == capacity[i] - m_used[i]);
        }
    }
}

int main(void) {
    test_all_succeed();
    test_each_position_fails();
    test_zero_and_duplicate();
    test_overflow();
    test_full_set();
    test_abort_and_commit();
    test_failed_is_sticky();
    test_model();
    if (failures != 0) {
        fprintf(stderr, "claim_set_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("claim_set_test: ok\n");
    return 0;
}
