/**
 * @file envelope.h
 *
 * # The envelope pool
 *
 * An **envelope** is one input a session has not finished with yet: an
 * external event admitted from the host, mail from another local session,
 * a delayed send that is not yet due, a local invocation's completion or
 * stop token, and the event whose execution has not completed. Internal
 * events (`<raise>`, `done.state.*`, `error.*`) are not envelopes; they
 * keep interpreter storage of their own (ADR 0099 decision 1).
 *
 * `ta_envelope_pool` holds every envelope of one system: fixed-size
 * `ta_envelope` records in an index pool (`index_pool.h`), addressed by
 * `uint32_t` index, 0 meaning none. It also holds the system's **run
 * queue**, and counts its envelopes against its bound with a quota
 * (`quota.h`) that gives the **admission watermark**.
 *
 * ## The envelope
 *
 * An envelope has a fixed size, so the pool is an array of items in
 * pages. Its variable parts (the event name, an origin, a delayed send's
 * target, a trace parent) are length-prefixed text fields in the name
 * area (`name_area.h`), and `text` is the chain that holds them. See
 * `ta_envelope` for each field.
 *
 * - `next` links the envelope on the one list that holds it: the run
 *   queue, a macrostep's outgoing list, a session's deferred list, an
 *   address's waiting list, or, while it is free, the pool's free list.
 * - `owner_next` links a pending delayed send on its sender's owner list,
 *   so `<cancel>` and unloading find a session's timers.
 * - `heap_index` is the envelope's place in the deadline index while it
 *   is a timer, and its **arrival number** while it is runnable. The two
 *   never overlap: a runnable envelope is not in the deadline index.
 * - `state` says which list or role holds it (`ta_envelope_state`);
 *   the lists themselves do not record membership.
 *
 * ## The lists
 *
 * Every list is an intrusive `ta_list` (`list.h`) over the pool's items,
 * so no list owns memory. `ta_envelope_pool_links` reaches `next`, and
 * `ta_envelope_pool_owner_links` reaches `owner_next`.
 *
 * - **The run queue** is one list per system, in arrival order: the order
 *   in which envelopes became runnable, not one queue per target. Arrival
 *   order keeps every sender-to-receiver pair in order, which is what
 *   SCXML asks of one sender's events, and needs no scheduler choosing
 *   between targets. `ta_envelope_pool_enqueue` appends one envelope;
 *   `ta_envelope_pool_dequeue` takes the head.
 * - **An outgoing list** belongs to a running macrostep. It holds, in send
 *   order, the mail the macrostep sent, on no queue yet, so nothing can
 *   take it before the macrostep ends; `ta_envelope_pool_post` appends to
 *   it. At the macrostep's end `ta_envelope_pool_deliver` moves the whole
 *   list, in order, to the end of the run queue. A macrostep that is
 *   abandoned frees its outgoing list instead, envelope by envelope.
 *
 * **Arrival numbers.** The pool numbers each envelope as it joins the run
 * queue, from a `uint32_t` counter that wraps. Numbers are compared modulo
 * 2^32 (`ta_envelope_arrived_before`), which is exact while fewer than
 * 2^31 envelopes are runnable; the pool's bound is far below that. A
 * session's deferred mail goes back into the run queue at the position
 * its number gives it.
 *
 * ## Admission and the macrostep reserve
 *
 * The pool's quota has the pool's bound as its capacity and counts the
 * envelopes taken. Two takes use it:
 *
 * - **`ta_envelope_pool_take`** is the ordinary take of a `<send>`, a
 *   delayed send or an invocation token: it uses any free envelope that
 *   exists.
 * - **`ta_envelope_pool_admit`** is admission of external input: it takes
 *   an envelope only while the pool stays at or below the **admission
 *   watermark**, `max(1, bound × watermark_percent / 100)` envelopes in
 *   use, by asking the quota for one envelope with `bound - watermark`
 *   kept free. The rest is headroom for what sessions send each other, so
 *   external load alone cannot make the charts' sends fail.
 *   `ta_envelope_pool_admitting` says whether one more admission fits.
 *
 * The **macrostep reserve** is the number of free envelopes the pool
 * keeps allocated at each safe point: `ta_envelope_pool_refill` makes
 * `min(reserve, bound - used)` envelopes free, the quota's available room
 * capping it. A macrostep takes only envelopes that already exist, so the
 * reserve is the most one macrostep may take before its take answers
 * `TA_ERR_FULL`, unless an earlier safe point left more free. A host that
 * wants no allocation after start-up sets the reserve to the bound.
 *
 * ## Contract and invariants
 *
 * - `quota.used` and `items.used` are both the number of envelopes taken
 *   and not given back; `quota.capacity` and `items.ceiling` are both the
 *   bound.
 * - A taken envelope reads as all zero, its state `TA_ENVELOPE_FREE`,
 *   until its taker fills it and places it: on a list through
 *   `enqueue` or `post`, which set its state, or in a role (a timer, an
 *   invocation token) whose state the taker sets.
 * - An envelope is on at most one list through `next` and one through
 *   `owner_next`. The free list links through `next`, so a free envelope
 *   is on no other `next` list.
 * - Items never move: an index is stable from take to give. Users keep
 *   indices, never addresses, and look an address up again with
 *   `ta_envelope_pool_at` in each call, so a refill between calls cannot
 *   leave a pointer behind.
 *
 * ## Memory
 *
 * The pool owns its pages, allocated through the `ta_allocator` given to
 * `ta_envelope_pool_init`, which it borrows and which must outlive it.
 * It owns the envelopes, not what they hold: an envelope's `data` and
 * `content` are references its taker retained, and its `text` is a chain
 * of the name area. Whoever gives an envelope back releases those first;
 * the pool releases nothing, and neither does destroy.
 *
 * - **Never allocate, and may run inside a macrostep:** `take`, `admit`,
 *   `give`, `at`, `admitting`, the links, `enqueue`, `dequeue`, `post`,
 *   `deliver` and `ta_envelope_arrived_before`.
 * - **`ta_envelope_pool_refill` allocates** pages, up to the bound. It
 *   runs only at a safe point (during admission, after a macrostep ends,
 *   at system creation), never inside a macrostep.
 * - **`ta_envelope_pool_trim` and `ta_envelope_pool_destroy` only free.**
 *   Trim gives back whole free pages above the reserve, only when the host
 *   asks core to give memory back; core never trims on its own.
 *
 * On a 64-bit host an envelope is 104 bytes: 72 of links, state and
 * identities, plus two 16-byte value references.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `take`, `admit`, `give`, `at`, `admitting` | O(1) |
 * | `enqueue`, `dequeue`, `post` | O(1) |
 * | `deliver` | O(envelopes on the outgoing list): each gets its arrival number; the splice itself is O(1) |
 * | `refill` | O(1) when enough envelopes are free; otherwise one allocation per page added (`index_pool.h`) |
 * | `trim` | walks the pages and the free list once |
 *
 * ## Failure
 *
 * - `ta_envelope_pool_init` answers `TA_ERR_INVALID` for parameters that
 *   break a rule of `ta_envelope_pool_params`, and leaves `*pool`
 *   untouched.
 * - `ta_envelope_pool_take` answers `TA_ERR_FULL` when no envelope is
 *   free: the pool is at its bound, or the free envelopes the last refill
 *   left are used up. A `<send>` then fails as
 *   `error.execution.capacity.pool`.
 * - `ta_envelope_pool_admit` answers `TA_ERR_FULL` at the watermark as
 *   well, and the input is bounced.
 * - `ta_envelope_pool_refill` answers `TA_ERR_NOMEM` when an allocation
 *   fails, and changes nothing.
 * - A failed call changes nothing. Giving an envelope that is not taken,
 *   or one still on a list, and placing an envelope that is already on a
 *   list, are programmer errors, not checked.
 *
 * ## Users
 *
 * The system creates one pool (ADR 0099 decision 2), bounded by
 * `poolEnvelopes`. Sending to a local session, delaying a send, starting
 * a local invocation and admitting external input take an envelope from
 * it; raising an internal event does not. The run queue feeds the
 * macrostep; the outgoing list holds a macrostep's mail until it ends.
 * The deadline index, the owner lists, the deferred lists and the waiting
 * lists are built over the same envelopes and links.
 */
#pragma once

#include "../include/tinyactors.h"
#include "chunks.h"
#include "generated/ring_records.h"
#include "index_pool.h"
#include "list.h"
#include "quota.h"

/** The smallest bound a pool accepts (`poolEnvelopes`). */
#define TA_ENVELOPE_POOL_MIN 8u
/** The largest bound a pool accepts (`poolEnvelopes`): 2^24. */
#define TA_ENVELOPE_POOL_MAX 16777216u
/** Each page of the pool holds `1 << TA_ENVELOPE_PAGE_SHIFT` envelopes. */
#define TA_ENVELOPE_PAGE_SHIFT 6u

/** Which list or role holds an envelope. */
typedef enum {
    /** On the free list, or taken and not yet placed. */
    TA_ENVELOPE_FREE = 0,
    /** On a running macrostep's outgoing list, until the macrostep ends. */
    TA_ENVELOPE_OUTGOING = 1,
    /** On the run queue, or on its target's deferred list. */
    TA_ENVELOPE_RUNNABLE = 2,
    /** On the waiting list of an address that is not resident. */
    TA_ENVELOPE_WAITING = 3,
    /** Taken by a macrostep: `_event` names it until its input completes. */
    TA_ENVELOPE_TAKEN = 4,
    /** A delayed send, in the deadline index and its sender's owner list. */
    TA_ENVELOPE_TIMER = 5,
    /** Held by a live local invocation, on no list. */
    TA_ENVELOPE_INVOCATION_TOKEN = 6,
    /** An invocation token turned into the stop of a child session. */
    TA_ENVELOPE_CHILD_STOP = 7
} ta_envelope_state;

/** `target` is the index of a waiting entry, not a session. */
#define TA_ENVELOPE_FLAG_WAITING_TARGET ((uint8_t)0x01u)
/** `text` ends with a trace parent field (`name_area.h`). */
#define TA_ENVELOPE_FLAG_TRACED ((uint8_t)0x02u)

/** One pending input; see the file comment. */
typedef struct {
    /** The next envelope on the one list that holds it, 0 at the tail. */
    uint32_t next;
    /** The next pending delayed send of the same sender, 0 at the tail. */
    uint32_t owner_next;
    /** While a timer, its place in the deadline index; while runnable,
     * its arrival number. */
    uint32_t heap_index;
    /** A `ta_envelope_state`. */
    uint8_t state;
    /** `_event.type`: a `ta_event_type`. */
    uint8_t type;
    /** Where it came from: a `ta_record_input_source`. */
    uint8_t source;
    /** `TA_ENVELOPE_FLAG_*`. */
    uint8_t flags;
    /** The target session's ID (ADR 0066), or, with
     * `TA_ENVELOPE_FLAG_WAITING_TARGET`, the index of its address's
     * waiting entry; 0 for a timer whose target is an I/O processor's,
     * kept as text. */
    uint64_t target;
    /** The sending session's ID, or 0 for external input. */
    uint64_t from;
    /** A timer's deadline. */
    ta_time due;
    /** The invocation-instance token of a child event, completion or
     * stop, or 0. */
    uint64_t invoke_instance;
    /** A local send's ID, for `<cancel>` on the owner list; an external
     * send ID is text. */
    ta_send_id send_id;
    /** The host's sequence number of the input it was admitted from, or
     * 0. */
    uint64_t source_seq;
    /** Its text fields in the name area. */
    ta_chunk_chain text;
    /** `_event.data`, a reference the envelope holds. */
    ta_value data;
    /** A delayed send's evaluated content, held apart from its data, or
     * none. */
    ta_value content;
} ta_envelope;

/** What `ta_envelope_pool_init` needs. */
typedef struct {
    /** Where pages come from; borrowed, and must outlive the pool. */
    ta_allocator allocator;
    /** The most envelopes the pool holds: from `TA_ENVELOPE_POOL_MIN` to
     * `TA_ENVELOPE_POOL_MAX`. */
    uint32_t bound;
    /** The admission watermark, percent of `bound`: from 1 to 99. */
    uint32_t watermark_percent;
    /** Free envelopes refill keeps at a safe point: from 1 to `bound`. */
    uint32_t reserve;
} ta_envelope_pool_params;

/** The envelopes of one system; see the file comment. Fields are for
 * reading only. */
typedef struct {
    /** The envelopes, linked through `next` while free. */
    ta_index_pool items;
    /** The bound, and the envelopes in use. */
    ta_quota quota;
    /** Runnable envelopes in arrival order, linked through `next`. */
    ta_list run_queue;
    /** The arrival number the next runnable envelope gets. */
    uint32_t arrivals;
    /** The most envelopes in use that admission may leave:
     * `max(1, bound × watermark_percent / 100)`. */
    uint32_t watermark;
    /** Free envelopes refill keeps. */
    uint32_t reserve;
} ta_envelope_pool;

/**
 * Sets up an empty pool, with no page yet. Allocates nothing; may run
 * anywhere. Answers `TA_ERR_INVALID`, leaving `*pool` untouched, when a
 * parameter breaks its rule.
 */
ta_result ta_envelope_pool_init(ta_envelope_pool *pool, const ta_envelope_pool_params *params);

/**
 * Frees every page and leaves an empty pool. Every envelope is gone; the
 * references and text chains of envelopes still taken are not released,
 * so their owners release them first. Only frees; never fails; never
 * runs inside a macrostep.
 */
void ta_envelope_pool_destroy(ta_envelope_pool *pool);

/**
 * Makes `min(reserve, bound - used)` envelopes free, allocating pages
 * when fewer are. Runs only at a safe point, never inside a macrostep.
 * Answers `TA_OK`, or `TA_ERR_NOMEM` when an allocation fails, changing
 * nothing.
 */
ta_result ta_envelope_pool_refill(ta_envelope_pool *pool);

/**
 * Gives whole free pages back to the allocator while at least `reserve`
 * envelopes stay free. Only frees; never fails. Runs at a safe point,
 * only when the host asks core to give memory back.
 */
void ta_envelope_pool_trim(ta_envelope_pool *pool);

/**
 * Takes a free envelope for a `<send>`, a delayed send or an invocation
 * token, zeroes it, and answers its index in `*out`. O(1). Never
 * allocates; may run inside a macrostep. Answers `TA_ERR_FULL`, changing
 * nothing and leaving `*out` alone, when no envelope is free.
 */
ta_result ta_envelope_pool_take(ta_envelope_pool *pool, uint32_t *out);

/**
 * Takes a free envelope for admitted external input, as
 * `ta_envelope_pool_take` does, but only while the envelopes in use stay
 * at or below the watermark. O(1). Never allocates. Answers
 * `TA_ERR_FULL`, changing nothing, at the watermark or when no envelope
 * is free.
 */
ta_result ta_envelope_pool_admit(ta_envelope_pool *pool, uint32_t *out);

/**
 * Whether one more envelope may be admitted below the watermark: what
 * the control block's `admitting` reports. It does not say whether an
 * envelope is free. O(1). Never allocates.
 */
bool ta_envelope_pool_admitting(const ta_envelope_pool *pool);

/**
 * Gives back `index`, which is taken and on no list, to the free list,
 * and marks it free. Its references and text must be released already.
 * O(1). Never allocates; may run inside a macrostep. Cannot fail.
 */
void ta_envelope_pool_give(ta_envelope_pool *pool, uint32_t index);

/**
 * The envelope at `index`, or NULL for 0, an index past the bound, or
 * one whose page is not allocated. Valid within the calling function
 * only. O(1). Never allocates.
 */
static inline ta_envelope *ta_envelope_pool_at(const ta_envelope_pool *pool, uint32_t index) {
    return ta_index_pool_at(&pool->items, index);
}

/** The links of a list of envelopes through `next`. Never allocates. */
static inline ta_list_links ta_envelope_pool_links(ta_envelope_pool *pool) {
    return ta_index_pool_links(&pool->items, offsetof(ta_envelope, next));
}

/** The links of an owner list through `owner_next`. Never allocates. */
static inline ta_list_links ta_envelope_pool_owner_links(ta_envelope_pool *pool) {
    return ta_index_pool_links(&pool->items, offsetof(ta_envelope, owner_next));
}

/**
 * Appends `index`, which is on no list through `next`, to the run queue,
 * gives it the next arrival number and marks it runnable. O(1). Never
 * allocates; may run inside a macrostep. Cannot fail.
 */
void ta_envelope_pool_enqueue(ta_envelope_pool *pool, uint32_t index);

/**
 * Removes the head of the run queue, marks it taken, and answers its
 * index, or answers 0 and changes nothing when the run queue is empty.
 * O(1). Never allocates.
 */
uint32_t ta_envelope_pool_dequeue(ta_envelope_pool *pool);

/** Where `ta_envelope_pool_post` puts an envelope. */
typedef struct {
    /** The running macrostep's outgoing list. */
    ta_list *outgoing;
    /** The envelope, on no list through `next`. */
    uint32_t index;
} ta_envelope_post;

/**
 * Appends `post.index` to the outgoing list `post.outgoing`, in send
 * order, and marks it outgoing. O(1). Never allocates; may run inside a
 * macrostep. Cannot fail.
 */
void ta_envelope_pool_post(ta_envelope_pool *pool, ta_envelope_post post);

/**
 * At a macrostep's end: gives every envelope of `outgoing`, in order, the
 * next arrival number and marks it runnable, then appends the whole list
 * to the run queue and leaves `outgoing` empty. O(its length). Never
 * allocates. Cannot fail.
 */
void ta_envelope_pool_deliver(ta_envelope_pool *pool, ta_list *outgoing);

/**
 * Whether arrival number `a` came before `b`, modulo 2^32: exact while
 * fewer than 2^31 envelopes are runnable. O(1).
 */
static inline bool ta_envelope_arrived_before(uint32_t a, uint32_t b) {
    return (uint32_t)(a - b) >= 0x80000000u;
}
