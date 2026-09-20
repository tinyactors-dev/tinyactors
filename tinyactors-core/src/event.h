#pragma once

#include "event_name.h"
#include "value.h"

/* Events, faults, and queued events are fully public. */

/*
 * Core's own co-ownership of an event's data (ADR 0050): the envelope is
 * copied and its host value retained, so the copy outlives the caller's.
 * It cannot fail, because ta_value_retain() cannot. The public
 * ta_event_clone() asks the host for an independent copy instead; core does
 * not use it.
 */
void ta_event_retain(const ta_event *event, ta_event *out);
