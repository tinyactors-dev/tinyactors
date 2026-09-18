#pragma once

#include "clock.h"
#include "event.h"

/*
 * An evaluated delayed send. For the built-in SCXML processor `destination`
 * is the resolved external target; custom processors receive `target`.
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
    uint32_t heap_index;
    bool active;
    /*
     * Already sent: the router accepted nothing and the send waits here to be
     * offered again. <cancel> and session termination leave it in place.
     */
    bool submitted;
    /* Submitted sends only: the envelope, which outlives the sending session. */
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
} ta_delayed_send;

/* A binary min-heap ordered by (due, id); ids increase in scheduling order. */
typedef struct {
    ta_allocator allocator;
    ta_delayed_send *items;
    uint32_t count;
    uint32_t capacity;
    ta_timer_id next_id;
} ta_timer_heap;

ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator);
void ta_timer_heap_destroy(ta_timer_heap *heap);
/* Takes ownership of the send's values; assigns send->id when it is zero. */
ta_result ta_timer_schedule(ta_timer_heap *heap, const ta_delayed_send *send);
/* Removes and releases every pending, not yet submitted send of owner with send_id. */
ta_result ta_timer_cancel(ta_timer_heap *heap,
                          ta_actor_id owner,
                          ta_send_id send_id);
/* Like ta_timer_cancel(), but also removes submitted sends. */
ta_result ta_timer_withdraw(ta_timer_heap *heap,
                            ta_actor_id owner,
                            ta_send_id send_id);
/* Removes and releases every not yet submitted send of owner (session termination). */
void ta_timer_cancel_unsubmitted(ta_timer_heap *heap, ta_actor_id owner);
/* Removes and releases every pending send of owner. */
void ta_timer_cancel_owner(ta_timer_heap *heap, ta_actor_id owner);
uint32_t ta_timer_count_owner(const ta_timer_heap *heap, ta_actor_id owner);
const ta_delayed_send *ta_timer_peek(const ta_timer_heap *heap);
/* Transfers ownership of the earliest send's values to *out. */
ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out);
