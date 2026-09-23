#include "router.h"

#include "system.h"

ta_result tna_router_post_named(tna_system *system, ta_address *address, const ta_event *event) {
    ta_actor_directory_entry entry = tna_directory_find(&system->directory, address);
    if (entry.kind == TA_ACTOR_DIRECTORY_RESIDENT) {
        return tna_sched_post(&system->sched, entry.actor, event);
    }
    ta_address_retain(address);
    ta_result result = tna_sched_post_dead(&system->sched, &(tna_dead_post){
                                                               .address = address,
                                                               .event = event,
                                                               .reason = entry.kind == TA_ACTOR_DIRECTORY_UNLOADED
                                                                             ? TNA_DEAD_UNLOADED
                                                                             : TNA_DEAD_NOT_FOUND,
                                                           });
    if (result != TA_OK) {
        ta_address_release(address);
    }
    return result;
}

static ta_external_event_submit_status tna_router_submit(void *self, const ta_external_event_submit_params *params) {
    tna_system *system = self;
    const ta_external_delivery *delivery = params->delivery;
    ta_event event;
    ta_event_retain(&delivery->event, &event);
    ta_result result = delivery->destination.kind == TA_ACTOR_REF_NAMED
                           ? tna_router_post_named(system, delivery->destination.as.named, &event)
                           : tna_sched_post(&system->sched, delivery->destination.as.runtime, &event);
    if (result != TA_OK) {
        ta_event_release(&event);
        return TA_EXTERNAL_EVENT_RETRY; /* full, or out of memory: core offers it again */
    }
    tna_system_want_turn(system);
    return TA_EXTERNAL_EVENT_ACCEPTED;
}

static const ta_external_event_router_vtable tna_router_vtable = { .submit = tna_router_submit };

ta_external_event_router tna_router_adapter(tna_system *system) {
    return (ta_external_event_router){ .self = system, .v = &tna_router_vtable };
}
