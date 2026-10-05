#pragma once

#include "types.h"
#include "quota.h"

/*
 * Package claim_set is the claim set (building block C4 of
 * docs/proposals/ring-proto): reservations from several quotas that either
 * all succeed or leave nothing behind.
 *
 * What it is for. One step of work often needs room in more than one bounded
 * store: a <send> needs an envelope, name chunks and record bytes; an invoke
 * needs a token and a queue slot. With ta_quota (C3) alone the caller reserves
 * from each in turn and, when the third refuses, must remember to cancel the
 * first two on every error path. A forgotten cancel leaves `reserved` high
 * forever (C3 invariant I2). A claim set owns that bookkeeping: add reserves,
 * and the first refusal rolls back everything the set holds, so a false answer
 * means "no quota holds anything for this set". The refusal also names the
 * resource that ran out, so every caller raises
 * error.execution.capacity.<resource> from one code path instead of one per
 * reservation.
 *
 * Lifetime: call. A ta_claim_set is a value on the caller's stack, started
 * by ta_claim_set_init at a safe point (admission, the end of a macrostep, the
 * start of a unit) and finished by exactly one ta_claim_set_commit or
 * ta_claim_set_abort before the frame returns. It must not be stored, passed
 * across a macrostep boundary or kept while parked: it holds raw pointers to
 * quotas that their owners (system lifetime, or call lifetime for a nested
 * interpreter) never promise to keep at that address that long. A reservation
 * that has to outlive the call (a parked slot's input) is a ta_quota_claim
 * used directly; that is C3 and the input lifetime, not this block. The
 * resource names are static strings, borrowed and never freed.
 *
 * When it allocates. Never. No function takes an allocator, calls the
 * allocator or uses the standard library. The set is fixed-size, so there is
 * no growth that could fail: a full set refuses the add like a quota would.
 *
 * Complexity. add is O(1) on success and O(TA_CLAIM_SET_MAX) on a refusal (the
 * rollback); commit and abort are O(TA_CLAIM_SET_MAX), a constant. Each makes
 * at most TA_CLAIM_SET_MAX calls into C3.
 *
 * Failure behaviour. Only add can fail, and a failure changes no quota
 * (compared with before the set's first add) and leaves the set empty.
 * commit and abort cannot fail. Arithmetic is inherited from C3: every
 * comparison is against `available`, no sum of caller values is formed.
 *
 * Invariants.
 *   S1: count <= TA_CLAIM_SET_MAX, and entries[0..count) are live; entries
 *       from count on are zero.
 *   S2: each live entry's claim.held is what it reserved from its quota, and
 *       nobody else touches that claim. Hence C3's I2 holds for the quotas.
 *   S3: after any failed add, commit or abort, count == 0.
 *   S4: `failed` changes only in a failed add (set to its resource) and in
 *       init (cleared). A later success does not clear it.
 *
 * Commit means consume: reserved becomes used. The caller releases that use
 * later with ta_quota_release when the thing it paid for goes away.
 *
 * Usage.
 *
 *	ta_claim_set claim;
 *	ta_claim_set_init(&claim);
 *	if (!ta_claim_set_add(&claim, (ta_claim_request){ &q->envelopes, 1, "envelope" }) ||
 *	    !ta_claim_set_add(&claim, (ta_claim_request){ &q->names, chunks, "name" }) ||
 *	    !ta_claim_set_add(&claim, (ta_claim_request){ &q->records, bytes, "record" })) {
 *		return capacity_error(ta_claim_set_failed(&claim));  // nothing is held
 *	}
 *	... build the envelope; this part cannot report errors ...
 *	ta_claim_set_commit(&claim);   // or ta_claim_set_abort(&claim) to back out
 */

#define TA_CLAIM_SET_MAX 4u /* envelope, name chunks, record bytes, and one spare */

/*
 * A request to ta_claim_set_add, a struct because core functions take at most
 * three parameters. `resource` must be a static string; it is borrowed.
 */
typedef struct ta_claim_request {
    ta_quota *quota;
    uint64_t n;
    const char *resource;
} ta_claim_request;

/* One reservation: where it was taken from, how much, and the resource's name. */
typedef struct ta_claim_entry {
    ta_quota *quota;      /* borrowed, not owned; outlives the set */
    ta_quota_claim claim; /* this entry's own share of quota->reserved */
    const char *resource; /* static string, e.g. "envelope"; reported on failure */
} ta_claim_entry;

/* Zero-initialised or ta_claim_set_init'ed means empty. A value on the caller's stack. */
typedef struct ta_claim_set {
    ta_claim_entry entries[TA_CLAIM_SET_MAX];
    uint32_t count;
    const char *failed; /* resource that ran out in the last failed add; NULL if none, or if the set was full */
} ta_claim_set;

/* ta_claim_set_init makes the set empty with no failure recorded. Never allocates. */
void ta_claim_set_init(ta_claim_set *set);

/*
 * ta_claim_set_add reserves request.n from request.quota into a new entry and answers true.
 * When the quota refuses (n > available), or the set already holds
 * TA_CLAIM_SET_MAX entries, it releases every earlier entry too (as abort
 * does), leaves the set empty, sets set->failed to request.resource (NULL for a
 * full set, which is a caller bug) and answers false. So a false answer means
 * no quota holds anything for this set. n == 0 succeeds and still takes an
 * entry. The same quota may appear in several entries. The resource must be a
 * static string. Call it at a safe point: it is the only call here that fails.
 */
bool ta_claim_set_add(ta_claim_set *set, ta_claim_request request);

/*
 * ta_claim_set_commit consumes every entry (reserved becomes used) and empties
 * the set. It cannot fail: consume succeeds for whatever a claim holds (C3
 * proof, step 4). At most TA_CLAIM_SET_MAX steps, no allocation. Committing an
 * empty set does nothing.
 */
void ta_claim_set_commit(ta_claim_set *set);

/*
 * ta_claim_set_abort cancels every entry and empties the set; each quota's
 * used and reserved return exactly to what they were before the adds. It
 * cannot fail and is safe on an empty set and when repeated.
 */
void ta_claim_set_abort(ta_claim_set *set);

/*
 * ta_claim_set_failed answers the resource named by the last failed add, or
 * NULL if none failed since init. A caller maps it to
 * error.execution.capacity.<resource> in one place.
 */
const char *ta_claim_set_failed(const ta_claim_set *set);

/* ta_claim_set_idle answers whether the set holds no reservation; owners assert it at the end of a call. */
bool ta_claim_set_idle(const ta_claim_set *set);
