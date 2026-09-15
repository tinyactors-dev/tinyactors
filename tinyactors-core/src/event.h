#pragma once

#include "event_name.h"
#include "value.h"

typedef struct {
    ta_event event;
    uint32_t flags;
} ta_queued_event;

enum {
    TA_EVENT_FLAG_OWNS_DATA = 1u << 0,
    TA_EVENT_FLAG_GENERATED = 1u << 1
};

ta_result ta_event_clone(const ta_event *event, ta_event *out);
void ta_event_release(ta_event *event);
