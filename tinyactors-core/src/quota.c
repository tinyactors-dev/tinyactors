#include "quota.h"

/* ta_quota_init: see quota.h. */
void ta_quota_init(ta_quota *quota, uint64_t capacity) {
    *quota = (ta_quota){ .capacity = capacity, .used = 0, .reserved = 0 };
}

/* ta_quota_available: see quota.h. The subtraction is safe because of I1. */
uint64_t ta_quota_available(const ta_quota *quota) {
    return quota->capacity - quota->used - quota->reserved;
}

/* ta_quota_reserve: see quota.h. Compares against the available part; never adds n to anything first. */
bool ta_quota_reserve(ta_quota *quota, ta_quota_claim *claim, uint64_t n) {
    if (n > ta_quota_available(quota)) {
        return false;
    }
    /* held <= reserved <= capacity (I2, I1) and n <= available, so neither sum can wrap. */
    claim->held += n;
    quota->reserved += n;
    return true;
}

/* ta_quota_consume: see quota.h. held <= reserved by I2, so reserved - n cannot underflow. */
bool ta_quota_consume(ta_quota *quota, ta_quota_claim *claim, uint64_t n) {
    if (n > claim->held) {
        return false;
    }
    claim->held -= n;
    quota->reserved -= n;
    quota->used += n; /* used + reserved is unchanged, so I1 keeps this from wrapping */
    return true;
}

/* ta_quota_cancel: see quota.h. */
bool ta_quota_cancel(ta_quota *quota, ta_quota_claim *claim, uint64_t n) {
    if (n > claim->held) {
        return false;
    }
    claim->held -= n;
    quota->reserved -= n;
    return true;
}

/* ta_quota_cancel_all: see quota.h. */
uint64_t ta_quota_cancel_all(ta_quota *quota, ta_quota_claim *claim) {
    uint64_t held = claim->held;
    claim->held = 0;
    quota->reserved -= held;
    return held;
}

/* ta_quota_release: see quota.h. */
bool ta_quota_release(ta_quota *quota, uint64_t n) {
    if (n > quota->used) {
        return false;
    }
    quota->used -= n;
    return true;
}
