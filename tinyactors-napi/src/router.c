#include "router.h"

#include "system.h"

ta_result tna_router_post_named(tna_system *system, ta_address *address, const tna_message *message) {
    ta_actor_directory_entry entry = tna_directory_find(&system->directory, address);
    bool resident = entry.kind == TA_ACTOR_DIRECTORY_RESIDENT;
    if (!resident || tna_loads_waiting(&system->loads, address)) {
        /* A load in flight holds the address's mail, or a loader starts one
         * (napi ADR 0020); otherwise the mail goes where it went before. */
        ta_result result = tna_loads_route(&system->loads, &(tna_load_route){ .sched = &system->sched,
                                                                               .address = address,
                                                                               .message = message,
                                                                               .resident = resident,
                                                                               .now_ns = tna_monotonic_ns(NULL) });
        if (result != TA_ERR_NOT_FOUND) {
            return result;
        }
    }
    if (resident) {
        tna_message resolved = *message;
        resolved.target = entry.actor;
        return tna_sched_post(&system->sched, &resolved);
    }
    ta_address_retain(address);
    ta_result result = tna_sched_post_dead(&system->sched, &(tna_dead_post){
                                                               .address = address,
                                                               .message = message,
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
    /* Mail between actors carries no retained-bytes estimate: its data is a
     * value made inside a chart, and sizing it would mean calling JavaScript. */
    /* Traced mail keeps the sending step as its parent (core ADR 0067). */
    tna_message message = { .target = delivery->destination.as.runtime, .event = &event, .trace = &delivery->trace };
    ta_result result = delivery->destination.kind == TA_ACTOR_REF_NAMED
                           ? tna_router_post_named(system, delivery->destination.as.named, &message)
                           : tna_sched_post(&system->sched, &message);
    if (result != TA_OK) {
        ta_event_release(&event);
        return TA_EXTERNAL_EVENT_RETRY; /* full, or out of memory: core offers it again */
    }
    if (system->dispatching_due) {
        tna_system_record_lateness(system, delivery->due, &delivery->trace);
    }
    tna_system_want_turn(system);
    return TA_EXTERNAL_EVENT_ACCEPTED;
}

static const ta_external_event_router_vtable tna_router_vtable = { .submit = tna_router_submit };

ta_external_event_router tna_router_adapter(tna_system *system) {
    return (ta_external_event_router){ .self = system, .v = &tna_router_vtable };
}
