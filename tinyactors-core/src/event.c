#include "event.h"

void ta_event_retain(const ta_event *event, ta_event *out) {
    *out = *event;
    ta_value_retain(&out->data);
}

void ta_event_release(ta_event *event) {
    if (event == NULL) {
        return;
    }
    ta_value_release(&event->data);
    event->data = (ta_value){0};
}
