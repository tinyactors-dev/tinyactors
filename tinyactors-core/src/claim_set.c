#include "claim_set.h"

/* ta_claim_set_init: see claim_set.h. */
void ta_claim_set_init(ta_claim_set *set) {
    *set = (ta_claim_set){ .count = 0, .failed = NULL };
}

/* ta_claim_set_abort: see claim_set.h. Last to first, so the rollback undoes the adds in reverse order. */
void ta_claim_set_abort(ta_claim_set *set) {
    while (set->count > 0) {
        set->count -= 1;
        ta_claim_entry *entry = &set->entries[set->count];
        ta_quota_cancel_all(entry->quota, &entry->claim);
        *entry = (ta_claim_entry){ .quota = NULL, .claim = { 0 }, .resource = NULL };
    }
}

/*
 * ta_claim_set_add: see claim_set.h. The entry is stored only after the
 * reservation succeeded, so a refused add leaves the slot untouched and abort
 * never sees an entry without a quota.
 */
bool ta_claim_set_add(ta_claim_set *set, ta_claim_request request) {
    const char *failed = request.resource;
    if (set->count < TA_CLAIM_SET_MAX) {
        ta_claim_entry *entry = &set->entries[set->count];
        if (ta_quota_reserve(request.quota, &entry->claim, request.n)) {
            entry->quota = request.quota;
            entry->resource = request.resource;
            set->count += 1;
            return true;
        }
    } else {
        failed = NULL; /* a full set is a caller bug; no resource ran out */
    }
    ta_claim_set_abort(set);
    set->failed = failed;
    return false;
}

/*
 * ta_claim_set_commit: see claim_set.h. By C3 step 4 consume of exactly the
 * held amount cannot be refused, so the result is not examined: there is no
 * error to report here and nothing the set could do about one.
 */
void ta_claim_set_commit(ta_claim_set *set) {
    while (set->count > 0) {
        set->count -= 1;
        ta_claim_entry *entry = &set->entries[set->count];
        (void)ta_quota_consume(entry->quota, &entry->claim, entry->claim.held);
        *entry = (ta_claim_entry){ .quota = NULL, .claim = { 0 }, .resource = NULL };
    }
}

/* ta_claim_set_failed: see claim_set.h. */
const char *ta_claim_set_failed(const ta_claim_set *set) {
    return set->failed;
}

/* ta_claim_set_idle: see claim_set.h. */
bool ta_claim_set_idle(const ta_claim_set *set) {
    return set->count == 0;
}
