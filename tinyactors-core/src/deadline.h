/**
 * @file deadline.h
 *
 * # Delayed sends: the deadline index and owner lists
 *
 * A delayed `<send>` is an envelope of the system's pool (`envelope.h`) in
 * state `TA_ENVELOPE_TIMER`, waiting for a time instead of a slot (ADR
 * 0101). `ta_deadline` holds the system's **deadline index**: a binary
 * min-heap (`heap.h`) of those envelopes, ordered by the envelope's `due`
 * time and then its `source_seq`, which for a timer is its send order.
 * Each session holds the **owner list** of its pending sends,
 * `ta_deadline_owner`, linked through the envelopes' `owner_next`.
 *
 * ## The envelope of a timer
 *
 * - `due` is the deadline and `source_seq` the order: the send's `id`,
 *   assigned from the system's counter when the send is scheduled with
 *   none, so one sender's sends due at the same time leave in send
 *   order. A send scheduled again with its `id` keeps its place.
 * - `send_id` is the send's ID, which `<cancel>` matches.
 * - `from` is the runtime ID of the session that made it. A submitted
 *   send whose session is gone is on no list, and that ID names no actor.
 * - `TA_ENVELOPE_FLAG_SUBMITTED` marks a send that was offered once and
 *   refused, and waits to be offered again. `<cancel>` and the end of its
 *   session leave it in place: it has left its owner (ADR 0054).
 * - `data` holds the send's data and `content` its evaluated `target` for a
 *   host I/O processor; the envelope holds a reference to each.
 * - The text holds the record: the event name, the I/O processor, the
 *   destination, the invocation, the literal content and target IDs, and
 *   for a submitted send its origin. A traced send's trace parent follows
 *   it as the trace field (`name_area.h`), with `TA_ENVELOPE_FLAG_TRACED`;
 *   an untraced send keeps none. The envelope owns the destination and
 *   origin handles. A record takes one 64-byte chunk, a traced one two.
 *
 * ## The owner list
 *
 * `ta_deadline_owner` is one envelope index, the list's head; new sends go
 * at the front, so the list is in reverse send order. What reads a
 * session's sends in order (inspection, export, session images) sorts
 * them by `(due, order)`.
 *
 * ## Dispatch, images and the end of a session
 *
 * - **Dispatch** takes due sends in `(due, order)` order. A send that falls
 *   due takes its owner off nothing but its list: it advances the owner's
 *   session epoch, as a withdrawal does, so an image captured before it is
 *   stale (ADR 0014). Its envelope goes back to the pool before the send
 *   is delivered, so a due send to a session never finds the pool full.
 * - **Submitted.** A send its host I/O processor refuses (out of memory)
 *   was sent: it keeps its origin, `<cancel>` no longer reaches it, and it
 *   waits, due, at its own place in the order, to be offered again.
 * - **The end of a session.** Destroy and unload take the owner's sends
 *   off its list: the unsubmitted ones are released, or for an unload
 *   carried in its image, and the submitted ones stay indexed with no
 *   list, until they are offered again or the system goes. Neither can
 *   fail for want of memory.
 * - **Images** hold a session's unsubmitted sends, in order, and a load
 *   schedules them again with their due times; a send already due fires at
 *   the next dispatch. Host values (target, data) are retained, not
 *   serialized.
 *
 * ## Memory
 *
 * The envelopes and their text are the pool's and the name area's. The
 * index keeps one `uint32_t` position per envelope the pool has, in the
 * heap's pages, grown by `ta_deadline_grow` at the pool's refill: a safe
 * point. `views` is the buffer inspection and export fill with a
 * session's sends, grown when they ask, outside any macrostep, and kept
 * until the system goes.
 *
 * - `ta_deadline_schedule`, `ta_deadline_cancel`, `ta_deadline_withdraw`,
 *   `ta_deadline_cancel_unsubmitted`, `ta_deadline_orphan`,
 *   `ta_deadline_owner_release`, `ta_deadline_copy`,
 *   `ta_deadline_next_due` and `ta_deadline_pop` never allocate; each may
 *   run inside a macrostep.
 * - `ta_deadline_grow`, `ta_deadline_reserve` and `ta_deadline_views`
 *   allocate, and run only at a safe point. The pool's refill grows
 *   `views` for the longest owner list seen, so `ta_deadline_views`
 *   allocates only when a list grew longer since the last refill.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | schedule, pop, remove one send | O(log n) in the index, n its length |
 * | cancel, withdraw, orphan, release | O(k + m log n), k the owner's sends, m those removed |
 * | copy, views | O(k log k): a walk and a sort |
 * | next due | O(1) |
 *
 * ## Failure
 *
 * A failed call changes nothing. `ta_deadline_schedule` answers
 * `TA_ERR_FULL` when the pool has no free envelope or the index no room,
 * and names the resource in `short_of`, or when the name area has no room
 * for the send's record. `ta_deadline_reserve` answers `TA_ERR_FULL` when
 * the pool's bound leaves fewer free envelopes than asked for, and
 * `TA_ERR_NOMEM` when its pages cannot be allocated. `ta_deadline_cancel`
 * answers `TA_ERR_NOT_FOUND` when it removed nothing.
 *
 * ## Users
 *
 * `<send>` with a delay and `<cancel>` (interpreter), a host's
 * `ta_actor_withdraw_send`, re-armed sends at spawn and restore, sends a
 * replaying session held until publish, session images, and the dispatch
 * of due sends. ADR 0101 is the decision; ADR 0054 and ADR 0026 the
 * contract of a pending send.
 */
#pragma once

#include "clock.h"
#include "event.h"
#include "generated/ring_records.h"
#include "heap.h"
#include "routing.h"
#include "vec.h"
#include "../include/tinyactors.h"

struct ta_system;
struct ta_actor;

/**
 * An evaluated delayed send, the flat form used to schedule a send, to hand
 * a due send to dispatch, and to carry sends in a session image. For the
 * built-in SCXML processor `destination` is the resolved external target;
 * custom processors receive `target`.
 */
typedef struct {
    /** Its order among sends due at the same time; 0 asks for the next. */
    ta_timer_id id;
    /** The runtime ID of the session that made it. */
    ta_actor_id owner;
    ta_send_id send_id;
    ta_time due;
    ta_event_name_id event;
    ta_atom io_type;
    ta_actor_ref destination;
    ta_value target;
    ta_value data;
    /**
     * A send to a host I/O processor with literal `<content>`: the
     * content's ID in the owner's definition, not its bytes, so a session
     * image restores it against its exact revision (ADR 0053, ADR 0038).
     * The processor gets the bytes when the send is dispatched.
     */
    bool has_content;
    ta_content_id content;
    /**
     * A send to a host I/O processor with a literal `target`: its content
     * ID in the owner's definition, for the same reason. `target` is then
     * absent.
     */
    bool has_target_text;
    ta_content_id target_text;
    /**
     * Already offered once and refused: it waits to be offered again.
     * `<cancel>` and the end of its session leave it in place.
     */
    bool submitted;
    /** Submitted sends only: the sending session, which it outlives. */
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
    /** The sending step (ADR 0067): a send that fires later still names it. */
    ta_trace_parent trace;
} ta_delayed_send;

/** One session's pending delayed sends: the head of its owner list, or 0,
 * and how many the list holds. */
typedef struct {
    uint32_t head;
    uint32_t count;
} ta_deadline_owner;

/** The system's deadline index; see the file comment. */
typedef struct {
    /** The timer envelopes, by `(due, order)`. */
    ta_heap heap;
    /** The order the next send without one is given. */
    ta_timer_id next_order;
    /** `ta_delayed_send_info`: what inspection and export last read. */
    ta_vec views;
    /** The most sends one owner's list has held: the refill keeps `views`
     * large enough for it, so reading a session's sends seldom allocates. */
    uint32_t most_per_owner;
} ta_deadline;

/** Room `views` has from system creation: a session with up to this many
 * pending sends is read without allocating. */
#define TA_DEADLINE_VIEWS_MIN 16u

/**
 * Sets up an empty index, with `views` room for `TA_DEADLINE_VIEWS_MIN`
 * sends. Allocates that room at system creation: `TA_ERR_NOMEM`, changing
 * nothing, when it cannot.
 */
ta_result ta_deadline_init(ta_deadline *deadline, ta_allocator allocator);

/**
 * Releases every pending send still indexed, submitted ones included, and
 * frees the index. Run at system teardown, before the pool goes.
 */
void ta_deadline_destroy(struct ta_system *system);

/**
 * Makes the index's room match the pool's envelopes, so that scheduling
 * never allocates. Runs at the pool's refill, a safe point. Answers
 * `TA_ERR_NOMEM`, changing nothing, when a page cannot be allocated.
 */
ta_result ta_deadline_grow(struct ta_system *system);

/**
 * Makes `count` envelopes free for sends scheduled before the next safe
 * point, and the index's room for them. Allocates; never inside a
 * macrostep. `TA_ERR_FULL` when the pool's bound leaves too few;
 * `TA_ERR_NOMEM` when pages cannot be allocated.
 */
ta_result ta_deadline_reserve(struct ta_system *system, uint32_t count);

/**
 * Stores `send` in an envelope, puts it in the index and, when `owner` is
 * not null, on `owner`'s list. Takes over the send's references on
 * `TA_OK`; assigns `send->id` from the system's counter when it is 0.
 * Never allocates; may run inside a macrostep. Answers `TA_ERR_FULL`,
 * changing nothing, with `*short_of` (when not null) naming the pool or
 * the name area.
 */
ta_result ta_deadline_schedule(struct ta_system *system,
                               ta_deadline_owner *owner,
                               const ta_delayed_send *send,
                               ta_record_capacity_resource *short_of);

/** How many sends `owner`'s list holds. O(1). Never allocates. */
uint32_t ta_deadline_count(const struct ta_system *system, const ta_deadline_owner *owner);

/**
 * Removes and releases every send on `owner`'s list with `send_id` that is
 * not submitted (`<cancel>`). Never allocates. `TA_ERR_NOT_FOUND` when it
 * removed none.
 */
ta_result ta_deadline_cancel(struct ta_system *system, ta_deadline_owner *owner, ta_send_id send_id);

/** Like ta_deadline_cancel(), but removes submitted sends too. */
ta_result ta_deadline_withdraw(struct ta_system *system, ta_deadline_owner *owner, ta_send_id send_id);

/** Whether `owner`'s list holds a submitted send with `send_id`. */
bool ta_deadline_has_submitted(const struct ta_system *system, const ta_deadline_owner *owner, ta_send_id send_id);

/** Removes and releases every send on `owner`'s list that is not submitted. */
void ta_deadline_cancel_unsubmitted(struct ta_system *system, ta_deadline_owner *owner);

/**
 * The owner goes: releases its sends that are not submitted, and takes its
 * submitted ones off its list, so they stay indexed with no owner (ADR
 * 0054). Never allocates; never fails.
 */
void ta_deadline_orphan(struct ta_system *system, ta_deadline_owner *owner);

/** Removes and releases every send on `owner`'s list, submitted ones too. */
void ta_deadline_owner_release(struct ta_system *system, ta_deadline_owner *owner);

/**
 * Copies `owner`'s sends that are not submitted, in `(due, order)` order,
 * into `out`, and answers how many there are; a null `out` only counts.
 * The copies borrow their references. Never allocates.
 */
uint32_t ta_deadline_copy(const struct ta_system *system, const ta_deadline_owner *owner, ta_delayed_send *out);

/**
 * Fills the system's view buffer with `owner`'s sends, submitted ones
 * included, as `ta_delayed_send_info` in `(due, order)` order, and points
 * `*out` at it. The view borrows the sends' references and stays valid
 * until the next call or until a send changes. Allocates when the buffer
 * is short, outside any macrostep: `TA_ERR_NOMEM` then.
 */
ta_result ta_deadline_views(struct ta_system *system,
                            const ta_deadline_owner *owner,
                            const ta_delayed_send_info **out,
                            uint32_t *count);

/**
 * Takes the **invocation token** of a local child: one envelope, in state
 * `TA_ENVELOPE_INVOCATION_TOKEN` on no list, whose text holds room for the
 * record of one traced timer. The child holds it from its spawn until its
 * completion is published; publishing gives it back first, so the
 * completion's mail, or the timer that keeps it while the parent's pool is
 * full, always finds an envelope and room for its record. Never allocates;
 * may run inside a macrostep. `TA_ERR_FULL`, changing nothing, with
 * `*short_of` (when not null) naming the pool or the name area.
 */
ta_result ta_deadline_token_take(struct ta_system *system, uint32_t *out, ta_record_capacity_resource *short_of);

/** Gives an invocation token back: its text and its envelope. 0 is none. */
void ta_deadline_token_release(struct ta_system *system, uint32_t token);

/** The earliest due time of any indexed send; false when there is none. */
bool ta_deadline_next_due(const struct ta_system *system, ta_time *out);

/**
 * Removes the earliest send from the index and from its owner's list, gives
 * its envelope back, and hands its references to `*out`. `TA_ERR_NOT_FOUND`
 * when the index is empty. Never allocates.
 */
ta_result ta_deadline_pop(struct ta_system *system, ta_delayed_send *out);
