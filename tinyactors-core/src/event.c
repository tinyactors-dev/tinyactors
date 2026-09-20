#include "event.h"

ta_result ta_event_clone(const ta_event *event, ta_event *out) {
    if (event == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_value data = {0};
    ta_result result = ta_value_clone(&event->data, &data);
    if (result != TA_OK) {
        return result;
    }
    *out = *event;
    out->data = data;
    return TA_OK;
}

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
