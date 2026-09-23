#include "event.h"
#include "routing.h"

void ta_event_retain(const ta_event *event, ta_event *out) {
    *out = *event;
    ta_value_retain(&out->data);
    if (out->origin.kind == TA_ORIGIN_ACTOR) {
        ta_actor_ref_retain(&out->origin.as.actor);
    }
}

void ta_event_release(ta_event *event) {
    if (event == NULL) {
        return;
    }
    ta_value_release(&event->data);
    event->data = (ta_value){0};
    if (event->origin.kind == TA_ORIGIN_ACTOR) {
        ta_actor_ref_release(&event->origin.as.actor);
    }
}
