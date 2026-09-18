#pragma once

#include "clock.h"
#include "event.h"

typedef struct {
    ta_timer_id id;
    ta_actor_id owner;
    ta_send_id send_id;
    ta_time due;
    ta_event_name_id event;
    ta_atom io_type;
    ta_value target;
    ta_value data;
    uint32_t heap_index;
    bool active;
} ta_delayed_send;

typedef struct {
    ta_allocator allocator;
    ta_delayed_send *items;
    uint32_t count;
    uint32_t capacity;
} ta_timer_heap;

ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator);
void ta_timer_heap_destroy(ta_timer_heap *heap);
ta_result ta_timer_schedule(ta_timer_heap *heap, const ta_delayed_send *send);
ta_result ta_timer_cancel(ta_timer_heap *heap,
                          ta_actor_id owner,
                          ta_send_id send_id);
const ta_delayed_send *ta_timer_peek(const ta_timer_heap *heap);
ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out);
