#include "tinyactors_host/router.h"

#include "memory.h"


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
 * The key of a load for a gone session (loads.h): `{ family, <session
 * text> }`. A session this system cannot name (0) is
 * `<incarnation>.0.<family>`, and gets a load of its own. One reference
 * goes to `*out`.
 */
static ta_result tah_router_session_key(tah_system *system, const ta_actor_ref *destination, ta_str family,
                                        ta_address **out) {
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
    ta_result result = ta_address_intern(system->core,
                                         &(ta_actor_address){ .family = { .name = family },
                                                              .name = { .ptr = text, .len = length } },
                                         out);
    if (held != 0) {
        allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = text, .size = held, .alignment = 1 });
    }
    return result;
}

/*
 * Whether mail a chart sends to `destination` has nowhere to go (napi
 * ADR 0022): a runtime ID with no actor, or one that finished, whose family
 * has no loader (napi ADR 0032 section 2); a name whose resident actor
 * finished, or with no resident actor, no load in flight, and no loader
 * that would start one. `*resident` gets the resident actor of a name that
 * is not finished, which takes the mail at once.
 */
static bool tah_router_unreachable(tah_system *system, const ta_actor_ref *destination, tah_dead_reason *reason,
                                   ta_actor_id *resident) {
    *resident = 0;
    if (destination->kind == TA_ACTOR_REF_NAMED) {
        ta_address *address = destination->as.named;
        ta_actor_directory_entry entry = tah_directory_find(&system->directory, address);
        if (entry.kind == TA_ACTOR_DIRECTORY_RESIDENT) {
            /* A resident actor that finished takes nothing, as for a runtime
             * ID; its done hook has not destroyed it yet. */
            const tah_sched_entry *scheduled = tah_sched_lookup(&system->sched, entry.actor);
            if (scheduled == NULL || (scheduled->flags & TAH_SCHED_FINISHED) == 0) {
                *resident = entry.actor;
                return false;
            }
            *reason = TAH_DEAD_DONE;
            return true;
        }
        if (tah_loads_waiting(&system->loads, address) || tah_loads_would_load(&system->loads, address)) {
            return false;
        }
        *reason = entry.kind == TA_ACTOR_DIRECTORY_UNLOADED ? TAH_DEAD_UNLOADED : TAH_DEAD_NOT_FOUND;
        return true;
    }
    const tah_sched_entry *entry = tah_sched_lookup(&system->sched, destination->as.runtime);
    *reason = entry == NULL ? TAH_DEAD_NOT_FOUND : TAH_DEAD_DONE;
    return true;
}

static const char *tah_router_unreachable_message(tah_dead_reason reason) {
    switch (reason) {
    case TAH_DEAD_DONE: return "the target session has finished";
    case TAH_DEAD_UNLOADED: return "the target is unloaded and no loader would bring it back";
    case TAH_DEAD_REJECTED: return "the loader rejected the target";
    case TAH_DEAD_LOAD_TIMEOUT: return "the target's load timed out";
    case TAH_DEAD_LOAD_FAILED: return "the target could not be loaded";
    default: return "no session has that ID or name";
    }
}

void tah_router_fail(tah_system *system, uint64_t request, const tah_router_failure *failure) {
    const char *text = tah_router_unreachable_message(failure->reason);
    ta_fault fault = { .code = TA_ERR_NOT_FOUND, .message = { text, (uint32_t)tah_str_length(text) } };
    if (system->callbacks.unreachable_data != NULL) {
        (void)system->callbacks.unreachable_data(
            system->callbacks.context,
            &(tah_unreachable){ .reason = failure->reason, .destination = failure->destination }, &fault.data);
    }
    (void)ta_system_load_failed(system->core, &(ta_load_failed_params){ .request = request, .fault = &fault });
    ta_value_release(&fault.data);
}

void tah_router_answer(tah_system *system, const ta_load_request *request) {
    const ta_actor_ref *destination = &request->destination;
    tah_dead_reason reason = TAH_DEAD_NOT_FOUND;
    ta_str family = { 0 };
    ta_address *key = NULL;
    ta_result result = TA_ERR_NOT_FOUND;
    if (tah_router_session_loads(system, destination, &family)) {
        result = tah_router_session_key(system, destination, family, &key);
        if (result == TA_OK) {
            result = tah_loads_ask(&system->loads, &(tah_load_ask){ .address = key,
                                                                    .why = TAH_LOAD_WHY_SESSION,
                                                                    .shared = destination->as.runtime != 0,
                                                                    .request = request->request,
                                                                    .waiting = request->waiting,
                                                                    .trace = &request->trace,
                                                                    .now_ns = tah_system_monotonic_ns(system) });
            ta_address_release(key);
        }
    } else {
        ta_actor_id resident = 0;
        if (!tah_router_unreachable(system, destination, &reason, &resident)) {
            if (resident != 0) {
                /* Resident again since core asked: it takes the mail now. */
                (void)ta_system_load_settled(system->core,
                                             &(ta_load_settled_params){ .request = request->request, .actor = resident });
                return;
            }
            result = tah_loads_ask(&system->loads, &(tah_load_ask){ .address = destination->as.named,
                                                                    .why = TAH_LOAD_WHY_MAIL,
                                                                    .shared = true,
                                                                    .request = request->request,
                                                                    .waiting = request->waiting,
                                                                    .trace = &request->trace,
                                                                    .now_ns = tah_system_monotonic_ns(system) });
        }
    }
    if (result == TA_OK) {
        tah_system_want_turn(system);
        return;
    }
    /* Nowhere to go, or no memory to start the load: each message becomes
     * error.communication for its sender (SCXML 6.2.4). */
    tah_router_fail(system, request->request,
                    &(tah_router_failure){ .reason = result == TA_ERR_NOMEM ? TAH_DEAD_LOAD_FAILED : reason,
                                           .destination = destination });
    tah_system_want_turn(system);
}
