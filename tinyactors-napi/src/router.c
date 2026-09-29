#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inputs.h"
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

/*
 * Whether mail for the runtime `destination` goes to its family's loader
 * (napi ADR 0032 section 2): the session is gone or has finished (core gives
 * a session another family holds as session 0), the reference names its
 * family (core ADR 0088), and the family has a loader. `family` gets the
 * family's name.
 */
static bool tna_router_session_loads(tna_system *system, const ta_actor_ref *destination, ta_str *family) {
    if (destination->kind != TA_ACTOR_REF_RUNTIME || destination->family == 0 || system->loads.families.count == 0) {
        return false;
    }
    if (destination->as.runtime != 0) {
        const tna_sched_entry *entry = tna_sched_lookup(&system->sched, destination->as.runtime);
        if (entry != NULL && (entry->flags & TNA_SCHED_FINISHED) == 0) {
            return false;
        }
    }
    return ta_system_atom_text(system->core, destination->family, family) == TA_OK &&
           tna_loads_family_loads(&system->loads, *family);
}

/*
 * Puts mail for a gone session into its load (loads.h): the load's key is
 * `{ family, <session text> }`. A session this system cannot name (0) is
 * `<incarnation>.0.<family>`, and gets a load of its own.
 */
static ta_result tna_router_post_session(tna_system *system, const ta_actor_ref *destination, ta_str family,
                                         const tna_message *message) {
    char small[256];
    char *text = small;
    uint32_t length = 0;
    uint64_t session = destination->as.runtime;
    if (session != 0) {
        uint32_t needed = 0;
        ta_session_id_format_params params = { .session = session, .family = family, .buffer = small,
                                               .capacity = sizeof small, .needed = &needed };
        ta_result result = ta_session_id_format(system->core, &params);
        if (result != TA_OK && needed > sizeof small) {
            text = malloc(needed);
            if (text == NULL) {
                return TA_ERR_NOMEM;
            }
            params.buffer = text;
            params.capacity = needed;
            result = ta_session_id_format(system->core, &params);
        }
        if (result != TA_OK) {
            if (text != small) {
                free(text);
            }
            return result;
        }
        length = needed - 1u;
    } else {
        size_t needed = (size_t)family.len + 24u;
        if (needed > sizeof small) {
            text = malloc(needed);
            if (text == NULL) {
                return TA_ERR_NOMEM;
            }
        }
        int written = snprintf(text, needed, "%llu.0.%.*s", (unsigned long long)ta_system_incarnation(system->core),
                               (int)family.len, family.ptr);
        length = written < 0 ? 0 : (uint32_t)written;
    }
    ta_address *address = NULL;
    ta_result result = ta_address_intern(system->core,
                                         &(ta_actor_address){ .family = { .name = family },
                                                              .name = { .ptr = text, .len = length } },
                                         &address);
    if (text != small) {
        free(text);
    }
    if (result != TA_OK) {
        return result;
    }
    result = tna_loads_route_session(&system->loads, &(tna_load_session_route){ .sched = &system->sched,
                                                                               .address = address,
                                                                               .message = message,
                                                                               .shared = session != 0,
                                                                               .now_ns = tna_monotonic_ns(NULL) });
    ta_address_release(address);
    return result;
}

/*
 * Whether mail a chart sends to `destination` has nowhere to go (napi
 * ADR 0022): a runtime ID with no actor, or one that finished, whose family
 * has no loader (napi ADR 0032 section 2); a name with
 * no resident actor, no load in flight, and no loader that would start one.
 * Such mail is not accepted: core raises error.communication in the sender
 * (SCXML 6.2.4). Mail from JavaScript does not come here, and stays a dead
 * letter (ADR 0008).
 */
static bool tna_router_unreachable(tna_system *system, const ta_actor_ref *destination, tna_dead_reason *reason) {
    if (destination->kind == TA_ACTOR_REF_NAMED) {
        ta_address *address = destination->as.named;
        ta_actor_directory_entry entry = tna_directory_find(&system->directory, address);
        if (entry.kind == TA_ACTOR_DIRECTORY_RESIDENT || tna_loads_waiting(&system->loads, address) ||
            tna_loads_would_load(&system->loads, address)) {
            return false;
        }
        *reason = entry.kind == TA_ACTOR_DIRECTORY_UNLOADED ? TNA_DEAD_UNLOADED : TNA_DEAD_NOT_FOUND;
        return true;
    }
    const tna_sched_entry *entry = tna_sched_lookup(&system->sched, destination->as.runtime);
    if (entry != NULL && (entry->flags & TNA_SCHED_FINISHED) == 0) {
        return false;
    }
    *reason = entry == NULL ? TNA_DEAD_NOT_FOUND : TNA_DEAD_DONE;
    return true;
}

static const char *tna_router_unreachable_message(tna_dead_reason reason) {
    switch (reason) {
    case TNA_DEAD_DONE: return "the target session has finished";
    case TNA_DEAD_UNLOADED: return "the target is unloaded and no loader would bring it back";
    default: return "no session has that ID or name";
    }
}

static ta_external_event_submit_status tna_router_submit(void *self, const ta_external_event_submit_params *params) {
    tna_system *system = self;
    const ta_external_delivery *delivery = params->delivery;
    tna_dead_reason reason = TNA_DEAD_NOT_FOUND;
    if (system->dispatching_due && delivery->event.origin.kind == TA_ORIGIN_ACTOR) {
        /* A delayed send left its sender: the input log says so (ADR 0033 section 3). */
        tna_inputs_fired(system, &delivery->event.origin.as.actor, delivery->event.send_id);
    }
    ta_str family = { 0 };
    bool to_loader = tna_router_session_loads(system, &delivery->destination, &family);
    if (!to_loader && tna_router_unreachable(system, &delivery->destination, &reason)) {
        const char *text = tna_router_unreachable_message(reason);
        ta_fault fault = { .code = TA_ERR_NOT_FOUND, .message = { text, (uint32_t)strlen(text) } };
        if (system->unreachable_data != NULL) {
            (void)system->unreachable_data(system->unreachable_context,
                                           &(tna_unreachable){ .reason = reason, .destination = &delivery->destination },
                                           &fault.data);
        }
        if (params->fault != NULL) {
            *params->fault = fault;
        } else {
            ta_value_release(&fault.data);
        }
        if (delivery->event.origin.kind == TA_ORIGIN_ACTOR) {
            tna_system_wake_sender(system, &delivery->event.origin.as.actor);
        }
        return TA_EXTERNAL_EVENT_ERROR;
    }
    ta_event event;
    ta_event_retain(&delivery->event, &event);
    /* Mail between actors carries no retained-bytes estimate: its data is a
     * value made inside a chart, and sizing it would mean calling JavaScript. */
    /* Traced mail keeps the sending step as its parent (core ADR 0067). */
    tna_message message = { .target = delivery->destination.as.runtime, .event = &event, .trace = &delivery->trace };
    ta_result result = to_loader ? tna_router_post_session(system, &delivery->destination, family, &message)
                       : delivery->destination.kind == TA_ACTOR_REF_NAMED
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
