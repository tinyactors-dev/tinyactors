#pragma once

#include "types.h"
#include "pool.h"
#include "ilist.h"
#include "quota.h"
#include "table.h"
#include "address.h"
#include "event.h"

/*
 * Package envelope is the pool of pending inputs (ADR 0099 sections 1 to 4;
 * the Envelope pool row of the catalog's "Lifetimes come first" table in
 * docs/proposals/ring-proto/reference/building-blocks.md). It is built from
 * four blocks and adds one rule of its own: A5 (the index pool), B1 (intrusive
 * lists), C3 (quotas) and B3 (the fixed hash table).
 *
 * What it is for. An input a session has not finished with (an external event
 * the host admitted, mail from another session, an invocation report, a
 * bootstrap) is one fixed-size record, an envelope, in one pool per system.
 * The pool answers three questions the host kernel used to answer with
 * mailboxes: how many inputs may wait (a bound, and a watermark below it for
 * the host's input), in what order they run (one run queue, arrival order),
 * and what a mail to a session that is not resident waits on (a waiting list
 * per address). An idle session holds no envelope and no queue: it costs
 * nothing here.
 *
 * Lifetimes. Each thing in this package has exactly one.
 *
 *   - system: the ta_envelopes control block (embedded in the owner, which
 *     must not move it after ta_envelopes_init), the pool's pages and
 *     directory, the quota totals, the list heads and the waiting table. The
 *     pool grows only in ta_envelopes_refill, up to a bound, and never
 *     shrinks until ta_envelopes_destroy. The waiting table is replaced at a
 *     refill, never grown in place, so it always has a slot for every
 *     envelope the pool can hold.
 *   - envelope: from ta_envelope_make until ta_envelope_release. It owns one
 *     reference to the event's data and origin, and, while it waits for a
 *     load, one reference to the interned address. Nothing else holds an
 *     index of an envelope except the one list it is on and, for a taken
 *     envelope, the slot that runs it. An index is valid until the envelope is
 *     released; the pool never moves envelopes, so a pointer from
 *     ta_envelope_at stays valid for the same time.
 *   - input: a ta_quota_claim made with ta_envelopes_reserve belongs to the
 *     input being run (the macrostep reserve). The runner takes it before the
 *     input starts and gives it back with ta_envelopes_unreserve when the
 *     input completes or is abandoned.
 *   - call: a ta_envelope_limits value (init reads it and keeps nothing).
 *
 * When it allocates. Only ta_envelopes_refill allocates, through the
 * allocator ta_envelopes_init was given, and it is a safe-point function: call
 * it where an out-of-memory answer can be reported and no unit of executable
 * content is running (admission, before an input is taken, after a macrostep
 * ends). ta_envelopes_init allocates nothing, so a system pays no byte for the
 * pool until its first input. Everything else, including ta_envelope_make,
 * ta_envelope_release and every list and waiting operation, never allocates:
 * an operation that finds no free envelope answers 0 and the owner refills at
 * its next safe point. Sizes are computed by the blocks underneath with
 * ta_layout and ta_checked_mul and freed with the size they were allocated
 * with.
 *
 * The bound. poolEnvelopes is the most envelopes that can exist. The pool is
 * made of pages of 256 envelopes (a smaller power of two when the bound is
 * under 256, so that a tiny pool still has a whole page), and the bound is
 * rounded down to whole pages; `bound` in the control block is the number
 * that results and the capacity of the quota. Two quantities are measured
 * against it. The admission watermark is a share of the bound: the host's
 * input is admitted only while `used * 100 < bound * watermark`
 * (ta_envelopes_admissible), so the sessions' own mail has headroom the host
 * cannot take. The macrostep reserve is a claim on the same quota: the
 * envelopes one input may take for the mail it sends, taken from the quota
 * before the input starts (ta_envelopes_reserve), so that a send inside the
 * input cannot fail for lack of an envelope that the pool had room for.
 * ta_envelope_make with a claim consumes the claim and cannot fail on the
 * quota; it fails only when the pool has no free item yet, which a refill
 * that covers the reserve prevents.
 *
 * Order. One run queue, in arrival order (ta_envelopes_run_push and
 * ta_envelopes_run_pop). A sender's outgoing list is a ta_ilist the sender
 * owns; ta_envelopes_run_splice appends all of it in O(1).
 *
 * Waiting lists. An envelope for an address with no resident session is put on
 * the waiting list of that address (ta_waiting_insert): a chain through `next`
 * whose first envelope is the entry the table stores and carries
 * `waiting_tail`. The first envelope of an address answers `first`, which is
 * the owner's cue to report that a load is needed once. ta_waiting_take_list
 * removes the whole list, in arrival order, for the owner to move to the run
 * queue (the address was loaded) or to recycle (it failed to load). Neither
 * allocates: the table has a slot for every envelope, so an insert cannot find
 * it full.
 *
 * Invariants.
 *   I1: quota.used == pool.used: every envelope in use consumed one unit, and
 *       release gives it back; quota.used + quota.reserved <= bound.
 *   I2: an envelope is on at most one list through `next`: the run queue, an
 *       outgoing list, a waiting list, or none (taken); its `state` says which
 *       the owner meant. Free envelopes have state FREE and no reference.
 *   I3: a WAITING envelope holds a reference to its address, and the table
 *       holds exactly one entry per address that has a non-empty list.
 *   I4: the waiting table's capacity is at least pool.capacity, so
 *       ta_waiting_insert has a free slot whenever an envelope is in use.
 *
 * Complexity. Make, release, push, pop, splice, reserve, admissible: O(1).
 * Waiting insert and take: O(1) expected (a hash probe). Refill: O(pages
 * added) for the pool plus O(waiting addresses) to rebuild the table, and
 * only when the pool grows. Destroy: O(extent).
 *
 * Failure. A refill is all or nothing: it builds the new waiting table first,
 * then grows the pool, and undoes the table when the pool refuses, so a
 * failure (TA_ERR_NOMEM) leaves the pool, the table and every counter as they
 * were. No other function fails except by answering 0 or false without
 * changing anything.
 *
 * Example (an admission, then a run):
 *
 *	ta_envelopes envelopes;
 *	ta_envelope_limits limits = { .pool_envelopes = 4096, .admission_watermark = 50, .macrostep_reserve = 1024 };
 *	ta_envelopes_init(&envelopes, allocator, &limits);        // allocates nothing
 *	// admission, at a safe point:
 *	if (!ta_envelopes_admissible(&envelopes)) return TA_ERR_FULL;
 *	if (ta_envelopes_refill(&envelopes, 1) != TA_OK) return TA_ERR_NOMEM;
 *	uint32_t index = ta_envelope_make(&envelopes, NULL, TA_ENVELOPE_RUNNABLE);
 *	ta_event_retain(event, &ta_envelope_at(&envelopes, index)->event);
 *	ta_envelopes_run_push(&envelopes, index);
 *	// the runner, later:
 *	index = ta_envelopes_run_pop(&envelopes);
 *	... run it ...
 *	ta_envelope_release(&envelopes, index);                    // gives the event's references back
 *	ta_envelopes_destroy(&envelopes);
 */

/*
 * ta_envelope_state says which holder the envelope belongs to. The list that
 * links it is the owner's business; the state is for checks and for release.
 */
typedef enum ta_envelope_state {
    TA_ENVELOPE_FREE = 0,     /* in the pool's free list; holds no reference */
    TA_ENVELOPE_RUNNABLE = 1, /* on the run queue or on a sender's outgoing list */
    TA_ENVELOPE_WAITING = 2,  /* on the waiting list of `payload.address` */
    TA_ENVELOPE_TAKEN = 3,    /* the input being run; on no list */
    TA_ENVELOPE_DECIDED = 4   /* a host input whose end is decided and waits for its notice, on the runner's refused list */
} ta_envelope_state;

/*
 * TA_ENVELOPE_WITHDRAWN marks (in ta_envelope.flags) a host input the host took
 * back with ta_input_withdraw while it waited on the run queue, an outgoing
 * list or a waiting list. It is the top bit, so it cannot meet the bits
 * package mail keeps in the same field (TA_MAIL_FLAG_*) or a refusal reason.
 * A withdrawn envelope is released unseen where it is next met; the session's
 * `queued` count and the host-input gauge were given back at the withdrawal.
 */
#define TA_ENVELOPE_WITHDRAWN 0x8000u

/*
 * ta_input_source (tinyactors.h) says where an input came from, which decides
 * how it is settled. An envelope's source is HOST, TRANSITION, MAIL or ERROR;
 * START is the source of a macrostep that has no envelope.
 */

/*
 * ta_envelope is one pending input, 168 bytes on a 64-bit host. The event is
 * kept whole, with its interned name, in this step: the name area that
 * shrinks the envelope to about 96 bytes is ADR 0100.
 *
 * Lifetime: envelope. `event` owns one reference to its data, to its origin's
 * address and to its origin's MESSAGE entry (ta_event_retain made them;
 * ta_envelope_release gives them back). `payload.address` is a reference to
 * an interned address, held only while the state is WAITING.
 */
typedef struct ta_envelope {
    uint32_t next;         /* the link of the one list that holds it; the pool's free link while free */
    uint32_t owner_next;   /* the owner's second list: the runner's pending load cues (run.h) while WAITING; ADR 0101's pending sends otherwise; the pool never writes it */
    uint32_t waiting_tail; /* first envelope of a waiting list: the list's last; 0 on the others */
    uint8_t state;         /* ta_envelope_state */
    uint8_t source;        /* ta_input_source */
    uint16_t flags;        /* the owner's bits; make clears them */
    uint64_t target;       /* the target session ID, or 0 while the target is the address in `payload` */
    uint64_t from;         /* the sending session ID, or 0 for the host */
    ta_time due;           /* the input time ADR 0094 validates; after take, the macrostep's logical time */
    uint64_t input;        /* the host's token for an admitted input (run.h: sequence << 25 | index), or 0 */
    union {
        ta_address *address;
    } payload;
    ta_event event;
    ta_trace_parent trace;
} ta_envelope;

_Static_assert(sizeof(ta_envelope) == 168, "ta_envelope is 168 bytes on a 64-bit host (ADR 0099 notes)");

/* ta_envelope_limits is the input of ta_envelopes_init: three numbers the system's parameters give. Call lifetime. */
typedef struct ta_envelope_limits {
    uint32_t pool_envelopes;      /* the most envelopes that may exist, before rounding to whole pages; at least 1 */
    uint32_t admission_watermark; /* percent of the bound above which the host's input is refused; 1 to 99 */
    uint32_t macrostep_reserve;   /* envelopes one input may take for its sends; 1 to pool_envelopes */
} ta_envelope_limits;

/*
 * ta_envelopes is the control block (lifetime: system). Its fields are for
 * envelope.c and the tests; use the functions. It holds pointers into
 * itself (the list links point at `pool`), so it must not move after
 * ta_envelopes_init.
 */
typedef struct ta_envelopes {
    ta_allocator allocator;
    ta_pool pool;
    ta_quota quota;        /* capacity == bound; used counts envelopes in use; reserved the live claims */
    ta_table waiting;      /* FIXED, hash of the address to the first envelope of its list; zeroed until the first refill */
    ta_ilink link;         /* the lists below, through ta_envelope.next */
    ta_ilink owner_link;   /* the owner's second list, through ta_envelope.owner_next */
    ta_ilist run;          /* the run queue */
    uint32_t bound;        /* pool_envelopes rounded down to whole pages */
    uint32_t admission_watermark;
    uint32_t macrostep_reserve;
} ta_envelopes;

/*
 * ta_envelopes_init validates `limits`, sets up the pool and the lists, and
 * allocates nothing: the pool is empty until the first ta_envelopes_refill.
 * It answers TA_ERR_INVALID for a null argument, a limit outside the ranges
 * on ta_envelope_limits, or a bound that rounds to zero, and TA_ERR_NOMEM if
 * a size overflows; *envelopes is then zeroed and ta_envelopes_destroy on it
 * is a no-op. O(1). The block must not move afterwards.
 */
ta_result ta_envelopes_init(ta_envelopes *envelopes, ta_allocator allocator, const ta_envelope_limits *limits);

/*
 * ta_envelopes_destroy gives back every reference an envelope in use still
 * holds (event data and origin, addresses of waiting ones), then the waiting
 * table and the pool's pages, with sized frees, and zeroes the block. Run it
 * before the address table is destroyed, because it releases addresses. It is
 * idempotent and safe on a block whose init failed. O(extent).
 */
void ta_envelopes_destroy(ta_envelopes *envelopes);

/*
 * ta_envelopes_refill makes `want_free` envelopes available to ta_envelope_make,
 * as far as the bound allows (the request is cut to bound - used), by adding
 * pages and, when the pool grew, replacing the waiting table by one with a
 * slot for every envelope. It allocates nothing when enough are free already,
 * and it is the only function in this package that allocates: call it at a
 * safe point only. TA_ERR_NOMEM when a size overflows or the allocator
 * refuses; nothing has changed then (the new table is built first, the pool
 * grown second, and the table undone if the pool fails). Reservations in the
 * quota are not counted: a caller that wants room for a reserve asks for it
 * in want_free.
 */
ta_result ta_envelopes_refill(ta_envelopes *envelopes, uint32_t want_free);

/*
 * ta_envelopes_admissible answers whether the host's input may be admitted:
 * the envelopes in use are below the watermark's share of the bound,
 * `used * 100 < bound * admission_watermark`, computed in 64 bits (both
 * factors are under 2^32 and the other under 100, so it cannot wrap). Reserved
 * envelopes are not "in use" until they are made, as the ADR says. It never
 * allocates. O(1).
 */
bool ta_envelopes_admissible(const ta_envelopes *envelopes);

/*
 * ta_envelope_make takes a free envelope, clears it (every field zero except
 * its state, which is `state`) and counts it against the quota, and returns
 * its index; it returns 0 and changes nothing when none is free (refill at
 * the next safe point) or the quota has no room. With a claim (from
 * ta_envelopes_reserve) the envelope is paid from the claim, which cannot be
 * refused for lack of quota; with NULL it is paid from what is neither used
 * nor reserved. It never allocates. O(1). The envelope belongs to the caller
 * and is on no list.
 */
uint32_t ta_envelope_make(ta_envelopes *envelopes, ta_quota_claim *claim, ta_envelope_state state);

/* ta_envelope_at returns envelope `index` (valid until it is released), or NULL for 0 or an unissued index. O(1). */
ta_envelope *ta_envelope_at(const ta_envelopes *envelopes, uint32_t index);

/*
 * ta_envelope_release gives an envelope back: it releases the event's data
 * and origin references and, if it is waiting, its address reference, marks
 * it FREE, returns it to the pool and the quota. The envelope must be on no
 * list (unlink or pop it first). Releasing 0 does nothing. It never
 * allocates. O(1) plus the release of what it held.
 */
void ta_envelope_release(ta_envelopes *envelopes, uint32_t index);

/*
 * ta_envelopes_reserve reserves min(macrostepReserve, quota available)
 * envelopes in `claim` for the input about to run, and answers how many. It
 * is "as far as the bounds allow": under load the claim can be smaller than
 * the reserve, and the sender sees a full pool exactly when the claim is
 * spent. The claim must be empty (held 0). Never allocates. O(1).
 */
uint32_t ta_envelopes_reserve(ta_envelopes *envelopes, ta_quota_claim *claim);

/* ta_envelopes_unreserve gives back what is left of `claim` (the input ended) and answers how much. O(1). */
uint32_t ta_envelopes_unreserve(ta_envelopes *envelopes, ta_quota_claim *claim);

/* ta_envelopes_run_push appends envelope `index` to the run queue and marks it RUNNABLE. O(1). */
void ta_envelopes_run_push(ta_envelopes *envelopes, uint32_t index);

/*
 * ta_envelopes_run_pop removes and returns the head of the run queue, or 0
 * when it is empty. The envelope is left in whatever state it had; the runner
 * marks it TAKEN. O(1).
 */
uint32_t ta_envelopes_run_pop(ta_envelopes *envelopes);

/*
 * ta_envelopes_run_splice appends every envelope of `outgoing`, in order, to
 * the run queue and empties `outgoing` (the end of a macrostep). The
 * envelopes were made RUNNABLE. No envelope is visited: O(1).
 */
void ta_envelopes_run_splice(ta_envelopes *envelopes, ta_ilist *outgoing);

/*
 * ta_envelopes_link answers the link descriptor of `next`, for an owner's own
 * list (an outgoing list, a deferred list) over these envelopes. Valid for
 * the life of the block.
 */
const ta_ilink *ta_envelopes_link(const ta_envelopes *envelopes);

/*
 * ta_envelopes_owner_link answers the link descriptor of `owner_next`, for a
 * second list over the same envelopes. An envelope may be on one list through
 * `next` and one through `owner_next` at the same time; the owner says which
 * states may do so (run.h: only the first envelope of a waiting list, while
 * the cue to load its address is pending). Valid for the life of the block.
 */
const ta_ilink *ta_envelopes_owner_link(const ta_envelopes *envelopes);

/*
 * ta_waiting_put is what ta_waiting_insert needs besides the envelope: the
 * address it waits for (a block of the caller, which only lends it for the
 * call) and where to say whether the envelope began the list.
 */
typedef struct {
    ta_address *address;
    bool *first;
} ta_waiting_put;

/*
 * ta_waiting_insert puts the made envelope `index` at the end of the waiting
 * list of `put->address`, marks it WAITING and gives it its own reference to
 * the address. *put->first is set to true when the list was empty, which is
 * when the owner reports that a load is needed. It never allocates and, after a
 * refill, cannot fail (I4); TA_ERR_INVALID is a caller bug (a null address or
 * an index that is not a made envelope). Nothing changes on failure. O(1)
 * expected.
 */
ta_result ta_waiting_insert(ta_envelopes *envelopes, uint32_t index, const ta_waiting_put *put);

/*
 * ta_waiting_length answers how many envelopes wait for `address`, 0 for none.
 * It walks the list: O(list), and the owner uses it for reports, not for
 * decisions on a hot path. It never allocates and changes nothing.
 */
uint32_t ta_waiting_length(const ta_envelopes *envelopes, const ta_address *address);

/*
 * ta_waiting_pending answers how many inputs still wait for `address`:
 * ta_waiting_length without the withdrawn ones (TA_ENVELOPE_WITHDRAWN), which
 * stay on the list, holding its place and its load cue, until the list
 * moves. It is what the host is told (ta_address_waiting); the owner decides
 * whether a list needs serving with ta_waiting_length, which counts them.
 * O(list). It never allocates and changes nothing.
 */
uint32_t ta_waiting_pending(const ta_envelopes *envelopes, const ta_address *address);

/*
 * ta_waiting_take_list removes the whole waiting list of `address` and
 * returns its first envelope, or 0 when nothing waits. The envelopes stay
 * chained through `next` in arrival order and keep state WAITING and their
 * address reference; the caller walks the chain with ta_ilist_next and moves
 * each to the run queue or releases it. The first envelope's `waiting_tail`
 * is cleared. It never allocates. O(1) expected.
 */
uint32_t ta_waiting_take_list(ta_envelopes *envelopes, const ta_address *address);
