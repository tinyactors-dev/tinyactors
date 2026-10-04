#pragma once

#include "types.h"

/*
 * Package quota is the quota with reservations (building block C3 of
 * docs/proposals/ring-proto): the one place where "a mandatory record never
 * fails" is proved instead of hoped for.
 *
 * What it is for. A bounded store (the internal event queue, the mandatory
 * record area, the c2h ring, the park area) must answer two different
 * questions at two different times. At a safe point, where an error can be
 * reported, "is there room for everything this phase may add?" (reserve). Deep
 * inside the phase, where no error can be reported, "take what I was promised"
 * (consume). A quota counts capacity in whatever unit its owner chooses
 * (entries, bytes, records) and splits it three ways: used (consumed and not
 * yet released), reserved (promised to holders, not yet consumed) and the rest,
 * which is available.
 *
 * Two types. A ta_quota holds the totals. A ta_quota_claim holds one holder's
 * share of `reserved`. consume and cancel take the quota and the claim, so a
 * holder can only draw on what it reserved itself and never on another
 * holder's reservation. That is what makes the guarantee below provable.
 *
 * Lifetimes. A ta_quota is a value inside its owner and lives as long as the
 * owner does: system for the ring and queue quotas of the running workspace,
 * call for a nested interpreter's. Its capacity is fixed by ta_quota_init (the
 * owner sizes it at a safe point); nothing here resizes it. A ta_quota_claim
 * belongs to the holder and lives as long as the holder does: input for a
 * slot's claim (it survives parking, because a parked slot lasts until its
 * input completes and keeps what it reserved), macrostep for the reservation
 * of one macrostep, unit for one phase's. A holder must bring its claim back
 * to zero (consume what it used, ta_quota_cancel_all the rest) before it dies;
 * a claim that dies with held != 0 leaves `reserved` high forever. Nothing
 * here can detect that; the owner's invariant I2 below is the test.
 *
 * When it allocates. Never. Every function is O(1), takes no allocator, and
 * stores into the two structs it is given.
 *
 * Failure behaviour. Every operation that can be refused answers false and
 * changes nothing (no field of the quota or the claim). None asserts. Only
 * ta_quota_reserve can fail for lack of room; the others fail only when the
 * caller names more than it holds or has used, which is a bug in the owner.
 * All comparisons are written so that no sum is formed that could exceed
 * UINT64_MAX; the only subtractions are of values known to be smaller.
 *
 * Invariants.
 *   I1: used + reserved <= capacity, at all times (never computed as a sum).
 *   I2: reserved == the sum of held over all live claims on this quota. The
 *       owner's duty; every operation that changes `reserved` changes exactly
 *       one claim by the same amount, and tests check I2 after every step.
 *
 * Why mandatory records never fail. Claim: if reserve(n) answered true for a
 * claim, then consume(m) for any m <= claim->held answers true, whatever else
 * happened to the quota in between.
 *   1. I1 holds after init (0 + 0 <= capacity).
 *   2. reserve adds n to `reserved` only when n <= capacity - used - reserved,
 *      so I1 is kept. consume moves m from `reserved` to `used`: the sum is
 *      unchanged. cancel and release only decrease a field. So I1 always holds.
 *   3. I2 holds after init, and each operation that touches `reserved` touches
 *      one claim by the same amount, so I2 is kept.
 *   4. consume(m) succeeds iff m <= claim->held. It reads neither the capacity
 *      nor the other claims. After a successful reserve, held grew by n; only
 *      this claim's own consume and cancel ever lower it (other holders'
 *      calls take their own claim). So held >= n minus what this holder itself
 *      consumed or cancelled, and consuming within that cannot be refused.
 *   5. Other holders and ordinary users cannot make `used + reserved` exceed
 *      capacity at the expense of this claim: step 2 says they could only add
 *      to `reserved` or to `used` out of the available part, and a reservation
 *      has already left the available part.
 * Mandatory consumers use a quota of their own (the ordinary and the
 * mandatory queue quotas are two ta_quota values), so ordinary exhaustion
 * cannot touch the mandatory capacity.
 * What this does not prove, and cannot: that the capacity is large enough for
 * all claims that can be outstanding together. That is the compiler's
 * whole-execution bound (reference/macrostep.md, "Sizing mandatory storage",
 * still open). The guarantee therefore reduces to that sizing statement plus
 * one rule for owners: reserve a phase's maximum before the phase starts, at
 * a point where failure is reportable.
 *
 * Usage.
 *
 *	ta_quota queue;
 *	ta_quota_claim phase = { 0 };
 *	ta_quota_init(&queue, 64);
 *	if (!ta_quota_reserve(&queue, &phase, 3)) {
 *		return report_capacity_error();     // safe point: can fail
 *	}
 *	ta_quota_consume(&queue, &phase, 1);        // deep in the phase: cannot fail
 *	ta_quota_cancel_all(&queue, &phase);        // phase ends: unused 2 go back
 *	ta_quota_release(&queue, 1);                // the entry is popped later
 */

/* Totals. Readable by the owner and tests; written only by the functions below. */
typedef struct ta_quota {
    uint64_t capacity;
    uint64_t used;     /* consumed and not yet released */
    uint64_t reserved; /* promised to holders, not yet consumed or cancelled */
} ta_quota;

/* One holder's share of `reserved`. Zero-initialised means "holds nothing". */
typedef struct ta_quota_claim {
    uint64_t held;
} ta_quota_claim;

/*
 * ta_quota_init sets the capacity and clears used and reserved. It never
 * allocates and never fails. Claims on a quota must start at held == 0.
 */
void ta_quota_init(ta_quota *quota, uint64_t capacity);

/* ta_quota_available answers capacity - used - reserved; I1 keeps it from underflowing. */
uint64_t ta_quota_available(const ta_quota *quota);

/*
 * ta_quota_reserve adds n to the claim and to `reserved` and answers true,
 * if and only if n <= ta_quota_available. Otherwise it answers false and
 * changes nothing. n == 0 succeeds. It is the only operation that can fail
 * for lack of room, so call it at a safe point.
 */
bool ta_quota_reserve(ta_quota *quota, ta_quota_claim *claim, uint64_t n);

/*
 * ta_quota_consume turns n of the claim's reservation into use (held and
 * reserved fall by n, used rises by n) and answers true, if and only if
 * n <= claim->held. It never looks at the capacity or at other holders, so
 * it cannot fail for lack of room: this is the call that "cannot fail".
 * Otherwise it answers false and changes nothing.
 */
bool ta_quota_consume(ta_quota *quota, ta_quota_claim *claim, uint64_t n);

/*
 * ta_quota_cancel gives n of the claim's reservation back unused (held and
 * reserved fall by n) and answers true, if and only if n <= claim->held.
 * Otherwise it answers false and changes nothing.
 */
bool ta_quota_cancel(ta_quota *quota, ta_quota_claim *claim, uint64_t n);

/*
 * ta_quota_cancel_all gives back everything the claim holds and answers how
 * much that was. Use it when a phase or a slot ends. It cannot fail.
 */
uint64_t ta_quota_cancel_all(ta_quota *quota, ta_quota_claim *claim);

/*
 * ta_quota_release frees n of consumed capacity (an entry popped, a record
 * released; used falls by n) and answers true, if and only if n <= used.
 * Otherwise it answers false and changes nothing.
 */
bool ta_quota_release(ta_quota *quota, uint64_t n);
