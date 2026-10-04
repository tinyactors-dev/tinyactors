/**
 * @file quota.h
 *
 * # Quota with reservations
 *
 * `ta_quota` counts a bounded resource: `{capacity, used, reserved}`, in
 * whatever unit its owner chooses (bytes of a ring or of the name area,
 * envelopes of the pool, entries of the internal queue). It holds no
 * resource itself; it only decides whether a request fits, so that the
 * rule "mandatory work never fails for lack of room" is proved in one
 * place.
 *
 * - **Reserve** sets room aside before the work that needs it starts:
 *   `ta_quota_reserve` succeeds only if `capacity - used - reserved` is at
 *   least the amount asked for.
 * - **Consume** draws on a reservation when the work happens: the amount
 *   moves from `reserved` to `used`. It cannot fail for lack of room,
 *   since the room was set aside already.
 * - **Cancel** returns what is left of a reservation that will not be
 *   used.
 * - **Take** uses room directly, without a reservation, for ordinary work
 *   that may fail: it succeeds only if the amount fits beside every
 *   outstanding reservation.
 * - **Release** returns used room when the resource is freed (an entry
 *   popped, an envelope freed, ring bytes drained).
 *
 * ## Holders: `ta_quota_claim`
 *
 * A reservation belongs to a holder (a macrostep slot, a phase). The
 * holder keeps a `ta_quota_claim`, the amount it has reserved and not yet
 * consumed or cancelled, wherever it keeps its own state. Reserve,
 * consume and cancel change the quota and the claim together, so the
 * quota's `reserved` is always the sum of its holders' claims. A paused
 * or parked holder keeps its claim, and with it its room: no other holder
 * and no take can use that room meanwhile. A claim is a number, so it
 * stays valid when its holder moves.
 *
 * ## Keeping room free: `keep`
 *
 * A `ta_quota_request` asks for `amount` and may also ask that `keep` of
 * the quota stay unreserved and unused after it is granted. Reserve and
 * take succeed only if `amount + keep` fits in `capacity - used -
 * reserved`; `keep` itself is not set aside. This serves:
 *
 * - the **c2h headroom**: an input's mandatory reserve is granted only
 *   if 2 × `maxRecordBytes` of the ring stay unreserved beside every
 *   slot's reservation, so a waiting slot can always write one ordinary
 *   record and its pad once the host drains;
 * - the **admission watermark**: admission takes an envelope only while
 *   the pool keeps `capacity - watermark` free for what sessions send each
 *   other.
 *
 * ## Invariants
 *
 * - `used + reserved <= capacity` at all times.
 * - `reserved` is the sum of the outstanding claims of the quota's
 *   holders, and a claim is used with one quota only.
 * - A call that fails changes neither the quota nor the claim.
 *
 * ## Memory
 *
 * The quota uses no memory of its own and never allocates: no function
 * here takes an allocator. A `ta_quota` is 12 bytes and a
 * `ta_quota_claim` 4 bytes, embedded by their owners (a ring's control
 * state, the pool, a slot record, the running workspace). Every function
 * here may run inside a macrostep.
 *
 * ## Complexity
 *
 * Every operation is O(1): a few comparisons and additions.
 *
 * ## Failure
 *
 * - A reserve or take that does not fit, `keep` included, answers
 *   `TA_ERR_FULL` and changes nothing. Running out is a state that load
 *   causes, never a precondition.
 * - Consuming or cancelling more than the claim holds, or releasing more
 *   than is used, answers `TA_ERR_INVALID` and changes nothing. That is a
 *   bookkeeping error in the caller, but the check costs one comparison
 *   and keeps the counters from wrapping around.
 * - A resize below `used + reserved` answers `TA_ERR_FULL` and changes
 *   nothing: the room is in use or promised.
 * - An amount of 0 always succeeds and changes nothing.
 *
 * ## Users
 *
 * - the **c2h mandatory reserve**, in bytes: each reserved record at its
 *   aligned size plus a pad of up to `maxRecordBytes - 8`, held by the
 *   input's slot until its input completes, with the **c2h headroom** of
 *   2 × `maxRecordBytes` as the request's `keep`; an ordinary action
 *   checks with `ta_quota_fits` for room for the largest record beside
 *   every slot's reservation before it evaluates anything;
 * - the **pool reserve** for one macrostep, and the **admission
 *   watermark** over the envelope pool and the name area;
 * - the internal queue's **ordinary quota** (`internalQueueEvents`
 *   entries, which an ordinary raise takes) and its **mandatory floor**
 *   (sized by the compiler; a mandatory event takes the floor first, then
 *   a free ordinary entry), two quotas over one queue; registering a
 *   larger definition resizes the floor;
 * - the **park area's reserve** of one parked continuation.
 */
#pragma once

#include "types.h"

/** A bounded resource: `used + reserved <= capacity`. */
typedef struct {
    uint32_t capacity;
    uint32_t used;
    uint32_t reserved;
} ta_quota;

/** What one holder has reserved on a quota and not yet consumed or
 * cancelled. Starts at `{0}`. */
typedef struct {
    uint32_t reserved;
} ta_quota_claim;

/** A request for `amount`, granted only if `keep` more would still stay
 * unreserved and unused afterwards. */
typedef struct {
    uint32_t amount;
    uint32_t keep;
} ta_quota_request;

/** An empty quota of `capacity`: nothing used, nothing reserved. Never
 * allocates. */
static inline ta_quota ta_quota_make(uint32_t capacity) {
    return (ta_quota){ .capacity = capacity, .used = 0u, .reserved = 0u };
}

/**
 * Sets the capacity to `capacity`, keeping what is used and reserved and
 * every claim. Growing always succeeds; shrinking succeeds down to `used
 * + reserved`. The owner resizes at a safe point, when the resource it
 * counts has grown or shrunk (a larger definition's mandatory floor).
 * O(1). Never allocates.
 *
 * Answers `TA_OK`, or `TA_ERR_FULL` and changes nothing when `capacity`
 * is below `used + reserved`.
 */
static inline ta_result ta_quota_resize(ta_quota *quota, uint32_t capacity) {
    if (capacity < quota->used + quota->reserved) {
        return TA_ERR_FULL;
    }
    quota->capacity = capacity;
    return TA_OK;
}

/** The room neither used nor reserved: `capacity - used - reserved`.
 * O(1). Never allocates; may run inside a macrostep. */
static inline uint32_t ta_quota_available(const ta_quota *quota) {
    return quota->capacity - quota->used - quota->reserved;
}

/**
 * Whether `request.amount` fits with `request.keep` left over:
 * `amount + keep <= capacity - used - reserved`, computed without
 * overflow. Reserve and take grant exactly what this answers true for. An
 * ordinary action that may write c2h asks this, with the largest record
 * as the amount, before it evaluates anything. O(1). Never allocates; may
 * run inside a macrostep.
 */
static inline bool ta_quota_fits(const ta_quota *quota, ta_quota_request request) {
    uint32_t available = ta_quota_available(quota);
    return request.amount <= available && available - request.amount >= request.keep;
}

/**
 * Sets `request.amount` aside for the holder of `claim`, adding it to
 * both the quota's `reserved` and the claim. Room another holder has
 * reserved is never granted. O(1). Never allocates; may run inside a
 * macrostep.
 *
 * Answers `TA_OK`, or `TA_ERR_FULL` and changes nothing when the amount
 * does not fit with `request.keep` left over.
 */
static inline ta_result ta_quota_reserve(ta_quota *quota, ta_quota_claim *claim, ta_quota_request request) {
    if (!ta_quota_fits(quota, request)) {
        return TA_ERR_FULL;
    }
    quota->reserved += request.amount;
    claim->reserved += request.amount;
    return TA_OK;
}

/**
 * Draws `amount` from the holder's reservation: it moves from `reserved`
 * to `used` on the quota and leaves the claim. Mandatory work does this
 * when it writes what it reserved for, and cannot run out. O(1). Never
 * allocates; may run inside a macrostep.
 *
 * Answers `TA_OK`, or `TA_ERR_INVALID` and changes nothing when `amount`
 * is more than the claim holds.
 */
static inline ta_result ta_quota_consume(ta_quota *quota, ta_quota_claim *claim, uint32_t amount) {
    if (amount > claim->reserved) {
        return TA_ERR_INVALID;
    }
    claim->reserved -= amount;
    quota->reserved -= amount;
    quota->used += amount;
    return TA_OK;
}

/**
 * Returns `amount` of the holder's reservation to the quota unused, as
 * when an input completes and its reserve comes back; `claim->reserved`
 * as the amount returns all of it. O(1). Never allocates; may run inside
 * a macrostep.
 *
 * Answers `TA_OK`, or `TA_ERR_INVALID` and changes nothing when `amount`
 * is more than the claim holds.
 */
static inline ta_result ta_quota_cancel(ta_quota *quota, ta_quota_claim *claim, uint32_t amount) {
    if (amount > claim->reserved) {
        return TA_ERR_INVALID;
    }
    claim->reserved -= amount;
    quota->reserved -= amount;
    return TA_OK;
}

/**
 * Uses `request.amount` directly, with no reservation, as ordinary work
 * does (an ordinary raise taking an internal queue entry, admission
 * taking an envelope below the watermark). It never takes room a holder
 * has reserved. O(1). Never allocates; may run inside a macrostep.
 *
 * Answers `TA_OK`, or `TA_ERR_FULL` and changes nothing when the amount
 * does not fit with `request.keep` left over.
 */
static inline ta_result ta_quota_take(ta_quota *quota, ta_quota_request request) {
    if (!ta_quota_fits(quota, request)) {
        return TA_ERR_FULL;
    }
    quota->used += request.amount;
    return TA_OK;
}

/**
 * Returns `amount` of used room, whether it was taken or consumed from a
 * reservation, when the resource is freed. O(1). Never allocates; may
 * run inside a macrostep.
 *
 * Answers `TA_OK`, or `TA_ERR_INVALID` and changes nothing when `amount`
 * is more than is used.
 */
static inline ta_result ta_quota_release(ta_quota *quota, uint32_t amount) {
    if (amount > quota->used) {
        return TA_ERR_INVALID;
    }
    quota->used -= amount;
    return TA_OK;
}
