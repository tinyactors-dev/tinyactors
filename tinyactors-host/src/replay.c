#include "tinyactors_host/replay.h"

#include "memory.h"

#include "tinyactors_host/adapter_io.h"
#include "tinyactors_host/events.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "scheduler_private.h"
#include "system_private.h"

/*
 * Replaying the events an actor took (kernel ADR 0006 section 3): the
 * application's journal, delivered to an actor prepared with `at`, which
 * crosses no border until it is published (core ADR 0089 section 3).
 */

/* The origin an origin URI and its I/O processor's type name (core ADR 0068). */
static ta_result tah_replay_origin_of(tah_system *system, ta_str origin, ta_str type, ta_origin *out) {
    *out = (ta_origin){ .kind = TA_ORIGIN_NONE };
    if (origin.len == 0 || type.len == 0) {
        return TA_OK;
    }
    ta_str scxml = { 0 };
    (void)ta_system_atom_text(system->core, ta_system_scxml_io_type(system->core), &scxml);
    if (type.len == scxml.len && tah_mem_equal(type.ptr, scxml.ptr, type.len)) {
        ta_scxml_target target = { 0 };
        ta_result result = ta_scxml_target_parse(system->core, origin, &target);
        if (result == TA_ERR_NOMEM) {
            return result;
        }
        if (result == TA_OK && target.kind == TA_SCXML_TARGET_ACTOR) {
            *out = (ta_origin){ .kind = TA_ORIGIN_ACTOR, .as.actor = target.as.actor };
            return TA_OK;
        }
        if (result == TA_OK) {
            ta_scxml_target_release(&target);
        }
        return TA_OK;
    }
    ta_atom atom = 0;
    if (ta_system_intern_atom(system->core, type, &atom) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    uint32_t processor = tah_io_index_of(system, atom);
    if (processor == 0) {
        return TA_OK; /* no such processor here: the message arrives without an origin */
    }
    return ta_io_origin_make(system->core, &(ta_io_origin_make_params){
                                               .processor = processor,
                                               .origin_class = TA_IO_ORIGIN_MESSAGE,
                                               .uri = origin,
                                               .out = out,
                                           });
}


/* Resumes a replaying actor until it is between macrosteps; `done` when it finished. */
static ta_result tah_replay_resume(tah_system *system, ta_actor_id actor, bool unlimited, bool *ignored, bool *done) {
    uint32_t budget = system->budgets.microsteps != 0 ? system->budgets.microsteps : 64u;
    for (;;) {
        ta_run_status status = TA_RUN_STABLE;
        bool took_nothing = false;
        system->inside_core = true;
        ta_result result = ta_actor_resume(system->core, &(ta_actor_resume_params){
                                                             .actor = actor,
                                                             .budget = budget,
                                                             .status = &status,
                                                             .action_budget = system->budgets.actions,
                                                             .no_microstep_limit = unlimited,
                                                             .ignored = &took_nothing,
                                                         });
        tah_system_leave_core(system);
        if (result != TA_OK) {
            return result;
        }
        if (status == TA_RUN_PAUSED) {
            continue;
        }
        *ignored = took_nothing;
        *done = status == TA_RUN_DONE;
        return TA_OK;
    }
}


/* Whether `invoke` is one of the actor's active invocations. */
static bool tah_replay_invoke_active(tah_system *system, ta_actor_id actor, ta_invoke_id invoke) {
    ta_actor_inspection view;
    if (invoke == TA_INVOKE_ID_NONE || ta_actor_inspect(system->core, actor, &view) != TA_OK) {
        return false;
    }
    for (uint32_t index = 0; index < view.invoke_count; index += 1u) {
        if (view.invokes[index].id == invoke) {
            return true;
        }
    }
    return false;
}


/* ------------------------------------------------------------------------
 * Replaying events (kernel ADR 0006 section 3)
 */

/* Whether `text` is error.communication.invoke.<invoke>.<reason>; `reason` gets the last token. */
static bool tah_replay_error_report(ta_str text, ta_str invoke, ta_str *reason) {
    static const char prefix[] = "error.communication.invoke.";
    size_t head = sizeof prefix - 1u;
    if (invoke.len == 0 || text.len <= head + invoke.len + 1u || !tah_mem_equal(text.ptr, prefix, head) ||
        !tah_mem_equal(text.ptr + head, invoke.ptr, invoke.len) || text.ptr[head + invoke.len] != '.') {
        return false;
    }
    *reason = (ta_str){ .ptr = text.ptr + head + invoke.len + 1u, .len = text.len - (uint32_t)(head + invoke.len + 1u) };
    for (uint32_t index = 0; index < reason->len; index += 1u) {
        if (reason->ptr[index] == '.') {
            return false;
        }
    }
    return true;
}

/* A replaying actor's event, built from its texts; the caller releases it. */
static ta_result tah_replay_event_of(tah_system *system, const tah_replay_event_params *params, ta_event *out) {
    *out = (ta_event){ .type = params->type, .invoke_id = TA_INVOKE_ID_NONE };
    ta_result result = tah_event_name_intern(system->core, params->name, &out->name);
    if (result == TA_OK && params->send_id.len != 0) {
        result = ta_send_id_intern(system->core, params->send_id, &out->send_id);
    }
    if (result == TA_OK && params->invoke_id.len != 0) {
        result = ta_system_intern_invoke_id(system->core, params->invoke_id, &out->invoke_id);
    }
    if (result == TA_OK && params->invoke_id.len == 0) {
        /* An invocation's event gets its origin from core when it is delivered (core ADR 0068). */
        result = tah_replay_origin_of(system, params->origin, params->origin_type, &out->origin);
    }
    if (result == TA_OK && params->data != NULL) {
        out->data = *params->data;
        ta_value_retain(&out->data);
    }
    if (result != TA_OK) {
        ta_event_release(out);
        *out = (ta_event){ .invoke_id = TA_INVOKE_ID_NONE };
    }
    return result;
}

/* Resumes a replaying actor until it is between macrosteps (its bootstrap, or internal events). */
static ta_result tah_replay_settle(tah_system *system, ta_actor_id actor, bool *finished) {
    bool ignored = false;
    return tah_replay_resume(system, actor, false, &ignored, finished);
}

ta_result tah_replay_event(tah_system *system, const tah_replay_event_params *params) {
    if (params->consumed != NULL) {
        *params->consumed = false;
    }
    if (params->finished != NULL) {
        *params->finished = false;
    }
    if (params->name.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_actor_inspection view;
    if (ta_actor_inspect(system->core, params->actor, &view) != TA_OK) {
        return TA_ERR_NOT_FOUND;
    }
    if (view.published) {
        return TA_ERR_INVALID;
    }
    tah_sched_entry *entry = tah_sched_lookup(&system->sched, params->actor);
    if (entry != NULL) {
        entry->flags |= TAH_SCHED_REPLAYING; /* the replay resumes it, not the scheduler */
    }
    bool finished = false;
    ta_result result = tah_replay_settle(system, params->actor, &finished);
    if (result != TA_OK) {
        return result;
    }
    if (finished) {
        if (params->finished != NULL) {
            *params->finished = true;
        }
        return TA_ERR_DONE;
    }
    ta_event event;
    result = tah_replay_event_of(system, params, &event);
    if (result != TA_OK) {
        return result == TA_ERR_INVALID ? TA_ERR_INVALID : result;
    }
    ta_str reason = { 0 };
    if (params->type == TA_EVENT_PLATFORM && tah_replay_error_report(params->name, params->invoke_id, &reason)) {
        if (!tah_replay_invoke_active(system, params->actor, event.invoke_id)) {
            ta_event_release(&event);
            return TA_ERR_NOT_FOUND;
        }
        static const char quarantined[] = "quarantined";
        ta_invoke_error error = {
            .reason = reason,
            .data = event.data.v != NULL ? &event.data : NULL,
            .ends = !(reason.len == sizeof quarantined - 1u && tah_mem_equal(reason.ptr, quarantined, reason.len)),
        };
        system->inside_core = true;
        result = ta_actor_report_invoke(system->core, &(ta_invoke_report_params){
                                                          .actor = params->actor,
                                                          .invoke_id = event.invoke_id,
                                                          .kind = TA_INVOKE_REPORT_ERROR,
                                                          .time = params->time,
                                                          .as.error = &error,
                                                      });
        tah_system_leave_core(system);
    } else {
        if (event.invoke_id != TA_INVOKE_ID_NONE && !tah_replay_invoke_active(system, params->actor, event.invoke_id)) {
            /* The invocation it came from is not active in the replay: the event arrives without it. */
            event.invoke_id = TA_INVOKE_ID_NONE;
        }
        system->inside_core = true;
        result = ta_actor_deliver(system->core, &(ta_actor_deliver_params){
                                                    .actor = params->actor, .event = &event, .time = params->time });
        tah_system_leave_core(system);
    }
    ta_event_release(&event);
    if (result != TA_OK) {
        return result;
    }
    bool ignored = false;
    result = tah_replay_resume(system, params->actor, false, &ignored, &finished);
    if (result == TA_OK) {
        if (params->consumed != NULL) {
            *params->consumed = !ignored;
        }
        if (params->finished != NULL) {
            *params->finished = finished;
        }
    }
    return result;
}

ta_result tah_replay_events_done(tah_system *system, ta_actor_id actor) {
    bool finished = false;
    ta_result result = tah_replay_settle(system, actor, &finished);
    tah_sched_entry *entry = tah_sched_lookup(&system->sched, actor);
    if (entry != NULL) {
        entry->flags &= (uint8_t)~TAH_SCHED_REPLAYING;
    }
    return result == TA_OK && finished ? TA_ERR_DONE : result;
}
