#include "routing.h"
#include "system.h"
#include "../include/tinyactors.h"
#include "address.h"

void ta_actor_ref_retain(const ta_actor_ref *ref) {
    if (ref->kind == TA_ACTOR_REF_NAMED) {
        ta_address_retain(ref->as.named);
    }
}

void ta_actor_ref_release(ta_actor_ref *ref) {
    if (ref->kind == TA_ACTOR_REF_NAMED) {
        ta_address_release(ref->as.named);
        ref->as.named = NULL;
    }
}

ta_result ta_actor_ref_localize(ta_system *system, ta_actor_ref *ref) {
    if (ref->kind != TA_ACTOR_REF_NAMED || ref->as.named == NULL || ta_address_in(ref->as.named, &system->addresses)) {
        return TA_OK;
    }
    ta_actor_address view = ta_address_view(ref->as.named);
    return ta_address_table_intern(&system->addresses, &view, &ref->as.named);
}

ta_result ta_actor_ref_detach(ta_arena *arena, ta_actor_ref *ref) {
    if (ref->kind != TA_ACTOR_REF_NAMED || ref->as.named == NULL) {
        return TA_OK;
    }
    return ta_address_detach(arena, ref->as.named, &ref->as.named);
}

ta_actor_ref *ta_event_origin_ref(ta_event *event) {
    return event->origin.kind == TA_ORIGIN_ACTOR ? &event->origin.as.actor : NULL;
}
