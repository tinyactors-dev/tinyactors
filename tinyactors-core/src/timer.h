#pragma once

#include "clock.h"
#include "event.h"

/*
 * Delayed sends (ADR 0026, ADR 0054).
 *
 * A send is stored with its owner: each owner has a ta_timer_list, one array
 * of public ta_delayed_send_info sorted by (due, timer id) plus a parallel
 * array of private fields. The system keeps a min-heap of the lists that hold
 * at least one send, keyed by each list's front. The heap's top list's front
 * is the globally earliest send, so dispatch order is exactly (due, id).
 *
 * A list's address must stay stable while it holds sends: the heap points at
 * it. Actors keep theirs in their sparse state; the sends whose session is
 * gone live in the heap's own `orphans` list.
 */

/*
 * An evaluated delayed send, the flat form used to schedule a send, to hand a
 * due send to dispatch, and to carry sends in a session image. For the
 * built-in SCXML processor `destination` is the resolved external target;
 * custom processors receive `target`.
 */
typedef struct {
    ta_timer_id id;
    ta_actor_id owner;
    ta_send_id send_id;
    ta_time due;
    ta_event_name_id event;
    ta_atom io_type;
    ta_actor_ref destination;
    ta_value target;
    ta_value data;
    /*
     * A send to a host I/O processor with literal <content>: the content's ID
     * in the owner's definition, not its bytes, so a session image restores
     * it against its exact revision (ADR 0053, ADR 0038). The processor gets
     * the bytes when the send is dispatched.
     */
    bool has_content;
    ta_content_id content;
    /*
     * A send to a host I/O processor with a literal `target`: its content ID
     * in the owner's definition, for the same reason. `target` is then
     * absent. Public, in ta_delayed_send_info, so inspection can name it.
     */
    bool has_target_text;
    ta_content_id target_text;
    /*
     * Already sent: the router accepted nothing and the send waits here to be
     * offered again. <cancel> and session termination leave it in place.
     */
    bool submitted;
    /* Submitted sends only: the envelope, which outlives the sending session. */
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
    /* The sending step (ADR 0067): a send that fires later still names it. */
    ta_trace_parent trace;
} ta_delayed_send;

/* The private fields of one stored send, parallel to ta_timer_list.sends. */
typedef struct {
    ta_timer_id id;
    ta_actor_id owner;
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
} ta_delayed_send_meta;

/* One owner's pending sends, sorted by (due, id). Zero-initialized is empty. */
typedef struct {
    ta_delayed_send_info *sends;
    ta_delayed_send_meta *meta;
    uint32_t count;
    uint32_t capacity;
    /* Position in the owner heap; meaningful only while `indexed`. */
    uint32_t heap_index;
    /* In the owner heap: true exactly when count != 0. */
    bool indexed;
} ta_timer_list;

/*
 * The owner heap's slots live in fixed pages of TA_TIMER_PAGE_OWNERS, found
 * through a page directory, as the actor table's do (ADR 0063). Growing adds
 * a page and never copies the heap; only the directory of page pointers
 * doubles. Pages live as long as the system.
 */
#define TA_TIMER_PAGE_OWNERS 8192u

typedef struct {
    ta_allocator allocator;
    /* A binary min-heap of non-empty lists, keyed by each list's front. */
    ta_vec pages; /* ta_timer_list **, one per page of owner slots */
    uint32_t count;
    ta_timer_id next_id;
    /* Submitted sends whose sending session is gone (ADR 0054). */
    ta_timer_list orphans;
    /* Set while the system is destroyed: orphaning releases instead of keeping. */
    bool closing;
} ta_timer_heap;

ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator);
/* Releases every send still indexed, the orphans included. */
void ta_timer_heap_destroy(ta_timer_heap *heap);
/* Makes the next `additional` schedules into list allocation-free. */
ta_result ta_timer_reserve(ta_timer_heap *heap, ta_timer_list *list, uint32_t additional);
/* Takes ownership of the send's values on TA_OK; assigns send->id when it is zero. */
ta_result ta_timer_schedule(ta_timer_heap *heap, ta_timer_list *list, const ta_delayed_send *send);
/* Removes and releases every not yet submitted send in list with send_id. */
ta_result ta_timer_cancel(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id);
/* Like ta_timer_cancel(), but also removes submitted sends. */
ta_result ta_timer_withdraw(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id);
/* Removes and releases every not yet submitted send in list (session termination). */
void ta_timer_cancel_unsubmitted(ta_timer_heap *heap, ta_timer_list *list);
/*
 * Makes ta_timer_orphan(list) allocation-free: reserves room in the orphans
 * for the list's submitted sends.
 */
ta_result ta_timer_reserve_orphans(ta_timer_heap *heap, const ta_timer_list *list);
/*
 * The sending session is gone: releases the list's unsubmitted sends and
 * moves its submitted ones to the orphans. Call ta_timer_reserve_orphans()
 * first. Leaves the list empty and unindexed; its storage stays allocated.
 */
void ta_timer_orphan(ta_timer_heap *heap, ta_timer_list *list);
/* Releases every send in list, unindexes it, and frees its storage. */
void ta_timer_list_destroy(ta_timer_heap *heap, ta_timer_list *list);
/* Bytes the list's storage occupies. */
size_t ta_timer_list_bytes(const ta_timer_list *list);
/*
 * Copies the list's not yet submitted sends in (due, id) order, and returns
 * how many there are; a null `out` only counts. These are the sends a
 * session belongs to — a submitted send outlives it and is the system's to
 * re-offer. The copies borrow their values; the caller takes its own
 * references.
 */
uint32_t ta_timer_copy_unsubmitted(const ta_timer_list *list, ta_delayed_send *out);
/* The earliest due time of any indexed send; false when there is none. */
bool ta_timer_next_due(const ta_timer_heap *heap, ta_time *out);
/* Removes the earliest send and transfers ownership of its values to *out. */
ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out);
