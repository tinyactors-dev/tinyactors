#include "tinyactors_host/router.h"

#include "memory.h"


#include "tinyactors_host/recorder.h"
#include "tinyactors_host/system.h"
#include "system_private.h"

ta_result tah_router_post_named(tah_system *system, ta_address *address, const tah_message *message) {
    ta_actor_directory_entry entry = tah_directory_find(&system->directory, address);
    bool resident = entry.kind == TA_ACTOR_DIRECTORY_RESIDENT;
    if (!resident || tah_loads_waiting(&system->loads, address)) {
        /* A load in flight holds the address's mail, or a loader starts one
         * (napi ADR 0020); otherwise the mail goes where it went before. */
        ta_result result = tah_loads_route(&system->loads, &(tah_load_route){ .sched = &system->sched,
                                                                               .address = address,
                                                                               .message = message,
                                                                               .resident = resident,
                                                                               .now_ns = tah_system_monotonic_ns(system) });
        if (result != TA_ERR_NOT_FOUND) {
            return result;
        }
    }
    if (resident) {
        tah_message resolved = *message;
        resolved.target = entry.actor;
        return tah_sched_post(&system->sched, &resolved);
    }
    ta_address_retain(address);
    ta_result result = tah_sched_post_dead(&system->sched, &(tah_dead_post){
                                                               .address = address,
                                                               .message = message,
                                                               .reason = entry.kind == TA_ACTOR_DIRECTORY_UNLOADED
                                                                             ? TAH_DEAD_UNLOADED
                                                                             : TAH_DEAD_NOT_FOUND,
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
static bool tah_router_session_loads(tah_system *system, const ta_actor_ref *destination, ta_str *family) {
    if (destination->kind != TA_ACTOR_REF_RUNTIME || destination->family == 0 || system->loads.families.count == 0) {
        return false;
    }
    if (destination->as.runtime != 0) {
        const tah_sched_entry *entry = tah_sched_lookup(&system->sched, destination->as.runtime);
        if (entry != NULL && (entry->flags & TAH_SCHED_FINISHED) == 0) {
            return false;
        }
    }
    return ta_system_atom_text(system->core, destination->family, family) == TA_OK &&
           tah_loads_family_loads(&system->loads, *family);
}

/*
 * Puts mail for a gone session into its load (loads.h): the load's key is
 * `{ family, <session text> }`. A session this system cannot name (0) is
 * `<incarnation>.0.<family>`, and gets a load of its own.
 */
static ta_result tah_router_post_session(tah_system *system, const ta_actor_ref *destination, ta_str family,
                                         const tah_message *message) {
    char small[256];
    char *text = small;
    size_t held = 0; /* bytes of `text` taken from the allocator, 0 for `small` */
    ta_allocator allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER);
    uint32_t length = 0;
    uint64_t session = destination->as.runtime;
    if (session != 0) {
        uint32_t needed = 0;
        ta_session_id_format_params params = { .session = session, .family = family, .buffer = small,
                                               .capacity = sizeof small, .needed = &needed };
        ta_result result = ta_session_id_format(system->core, &params);
        if (result != TA_OK && needed > sizeof small) {
            text = allocator.v->alloc(allocator.self, needed, 1);
            if (text == NULL) {
                return TA_ERR_NOMEM;
            }
            held = needed;
            params.buffer = text;
            params.capacity = needed;
            result = ta_session_id_format(system->core, &params);
        }
        if (result != TA_OK) {
            if (held != 0) {
                allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = text, .size = held, .alignment = 1 });
            }
            return result;
        }
        length = needed - 1u;
    } else {
        /* "<incarnation>.0.<family>": a session this system cannot name (core ADR 0088). */
        size_t needed = (size_t)family.len + 24u;
        if (needed > sizeof small) {
            text = allocator.v->alloc(allocator.self, needed, 1);
            if (text == NULL) {
                return TA_ERR_NOMEM;
            }
            held = needed;
        }
        length = tah_format_u64(text, ta_system_incarnation(system->core));
        tah_mem_copy(text + length, ".0.", 3u);
        length += 3u;
        tah_mem_copy(text + length, family.ptr, family.len);
        length += family.len;
    }
    ta_address *address = NULL;
    ta_result result = ta_address_intern(system->core,
                                         &(ta_actor_address){ .family = { .name = family },
                                                              .name = { .ptr = text, .len = length } },
                                         &address);
    if (held != 0) {
        allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = text, .size = held, .alignment = 1 });
    }
    if (result != TA_OK) {
        return result;
    }
    result = tah_loads_route_session(&system->loads, &(tah_load_session_route){ .sched = &system->sched,
                                                                               .address = address,
                                                                               .message = message,
                                                                               .shared = session != 0,
                                                                               .now_ns = tah_system_monotonic_ns(system) });
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
static bool tah_router_unreachable(tah_system *system, const ta_actor_ref *destination, tah_dead_reason *reason) {
    if (destination->kind == TA_ACTOR_REF_NAMED) {
        ta_address *address = destination->as.named;
        ta_actor_directory_entry entry = tah_directory_find(&system->directory, address);
        if (entry.kind == TA_ACTOR_DIRECTORY_RESIDENT || tah_loads_waiting(&system->loads, address) ||
            tah_loads_would_load(&system->loads, address)) {
            return false;
        }
        *reason = entry.kind == TA_ACTOR_DIRECTORY_UNLOADED ? TAH_DEAD_UNLOADED : TAH_DEAD_NOT_FOUND;
        return true;
    }
    const tah_sched_entry *entry = tah_sched_lookup(&system->sched, destination->as.runtime);
    if (entry != NULL && (entry->flags & TAH_SCHED_FINISHED) == 0) {
        return false;
    }
    *reason = entry == NULL ? TAH_DEAD_NOT_FOUND : TAH_DEAD_DONE;
    return true;
}

static const char *tah_router_unreachable_message(tah_dead_reason reason) {
    switch (reason) {
    case TAH_DEAD_DONE: return "the target session has finished";
    case TAH_DEAD_UNLOADED: return "the target is unloaded and no loader would bring it back";
    default: return "no session has that ID or name";
    }
}

static ta_external_event_submit_status tah_router_submit(void *self, const ta_external_event_submit_params *params) {
    tah_system *system = self;
    const ta_external_delivery *delivery = params->delivery;
    tah_dead_reason reason = TAH_DEAD_NOT_FOUND;
    ta_str family = { 0 };
    bool to_loader = tah_router_session_loads(system, &delivery->destination, &family);
    if (!to_loader && tah_router_unreachable(system, &delivery->destination, &reason)) {
        const char *text = tah_router_unreachable_message(reason);
        ta_fault fault = { .code = TA_ERR_NOT_FOUND, .message = { text, (uint32_t)tah_str_length(text) } };
        if (system->callbacks.unreachable_data != NULL) {
            (void)system->callbacks.unreachable_data(system->callbacks.context,
                                           &(tah_unreachable){ .reason = reason, .destination = &delivery->destination },
                                           &fault.data);
        }
        if (params->fault != NULL) {
            *params->fault = fault;
        } else {
            ta_value_release(&fault.data);
        }
        if (delivery->event.origin.kind == TA_ORIGIN_ACTOR) {
            tah_system_wake_sender(system, &delivery->event.origin.as.actor);
            if (system->dispatching_due) {
                tah_recorder_fired(system, &delivery->event.origin.as.actor); /* consumed: its wake moves */
            }
        }
        return TA_EXTERNAL_EVENT_ERROR;
    }
    ta_event event;
    ta_event_retain(&delivery->event, &event);
    /* Mail between actors carries no retained-bytes estimate: its data is a
     * value made inside a chart, and sizing it would mean calling JavaScript. */
    /* Traced mail keeps the sending step as its parent (core ADR 0067). */
    /* Past the queue's limits it waits in the receiver's overflow: a chart
     * cannot retry, and a refused send is logged nowhere (kernel ADR 0003). */
    tah_message message = { .target = delivery->destination.as.runtime,
                            .event = &event,
                            .trace = &delivery->trace,
                            .may_overflow = true };
    ta_result result = to_loader ? tah_router_post_session(system, &delivery->destination, family, &message)
                       : delivery->destination.kind == TA_ACTOR_REF_NAMED
                           ? tah_router_post_named(system, delivery->destination.as.named, &message)
                           : tah_sched_post(&system->sched, &message);
    if (result != TA_OK) {
        ta_event_release(&event);
        return TA_EXTERNAL_EVENT_RETRY; /* out of memory: core offers it again */
    }
    if (system->dispatching_due) {
        tah_system_record_lateness(system, delivery->due, &delivery->trace);
        if (delivery->event.origin.kind == TA_ORIGIN_ACTOR) {
            tah_recorder_fired(system, &delivery->event.origin.as.actor);
        }
    }
    tah_system_want_turn(system);
    return TA_EXTERNAL_EVENT_ACCEPTED;
}

static const ta_external_event_router_vtable tah_router_vtable = { .submit = tah_router_submit };

ta_external_event_router tah_router_adapter(tah_system *system) {
    return (ta_external_event_router){ .self = system, .v = &tah_router_vtable };
}
