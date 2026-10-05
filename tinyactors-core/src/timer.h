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
 * it. Actors keep theirs in their sparse state. A send never outlives its
 * owner: once it fires it is an envelope (package mail) or a call of a host
 * I/O processor, and when the owner goes its sends go with it.
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
    /* The sending step (ADR 0067): a send that fires later still names it. */
    ta_trace_parent trace;
} ta_delayed_send;

/* The private fields of one stored send, parallel to ta_timer_list.sends. */
typedef struct {
    ta_timer_id id;
    ta_actor_id owner;
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
} ta_timer_heap;

ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator);
/* Releases every send still indexed. */
void ta_timer_heap_destroy(ta_timer_heap *heap);
/* Makes the next `additional` schedules into list allocation-free. */
ta_result ta_timer_reserve(ta_timer_heap *heap, ta_timer_list *list, uint32_t additional);
/* Takes ownership of the send's values on TA_OK; assigns send->id when it is zero. */
ta_result ta_timer_schedule(ta_timer_heap *heap, ta_timer_list *list, const ta_delayed_send *send);
/* Removes and releases every send in list with send_id (<cancel>, an operator's withdrawal). */
ta_result ta_timer_cancel(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id);
/*
 * Removes and releases every send in list (the session ended, or unloaded:
 * its sends travel in its image). Leaves the list empty and unindexed; its
 * storage stays allocated. Never allocates.
 */
void ta_timer_cancel_all(ta_timer_heap *heap, ta_timer_list *list);
/* Releases every send in list, unindexes it, and frees its storage. */
void ta_timer_list_destroy(ta_timer_heap *heap, ta_timer_list *list);
/* Bytes the list's storage occupies. */
size_t ta_timer_list_bytes(const ta_timer_list *list);
/*
 * Copies the list's sends in (due, id) order, and returns how many there are;
 * a null `out` only counts. The copies borrow their values; the caller takes
 * its own references.
 */
uint32_t ta_timer_copy(const ta_timer_list *list, ta_delayed_send *out);
/* The earliest due time of any indexed send; false when there is none. */
bool ta_timer_next_due(const ta_timer_heap *heap, ta_time *out);
/* Removes the earliest send and transfers ownership of its values to *out. */
ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out);
