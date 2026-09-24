#include "event.h"
#include "routing.h"

void ta_event_retain(const ta_event *event, ta_event *out) {
    *out = *event;
    ta_value_retain(&out->data);
    if (out->origin.kind == TA_ORIGIN_ACTOR) {
        ta_actor_ref_retain(&out->origin.as.actor);
    } else if (out->origin.kind == TA_ORIGIN_IO && out->origin.as.io.origin_class == TA_IO_ORIGIN_MESSAGE) {
        ta_io_origin_entry_retain(out->origin.as.io.as.entry);
    }
}

void ta_event_release(ta_event *event) {
    if (event == NULL) {
        return;
    }
    ta_value_release(&event->data);
    event->data = (ta_value){0};
    ta_origin_release(&event->origin);
}

void ta_origin_release(ta_origin *origin) {
    if (origin == NULL) {
        return;
    }
    if (origin->kind == TA_ORIGIN_ACTOR) {
        ta_actor_ref_release(&origin->as.actor);
    } else if (origin->kind == TA_ORIGIN_IO && origin->as.io.origin_class == TA_IO_ORIGIN_MESSAGE) {
        ta_io_origin_entry_release(origin->as.io.as.entry);
        origin->as.io.as.entry = NULL;
    }
}
