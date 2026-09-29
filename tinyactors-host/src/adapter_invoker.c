#include "tinyactors_host/adapter_invoker.h"

#include "tinyactors_host/system.h"

static ta_op_status tah_invoke_fault(ta_fault *fault, ta_result code, const char *message, uint32_t length) {
    *fault = (ta_fault){ .code = code, .message = { .ptr = message, .len = length } };
    return TA_OP_ERROR;
}

#define TAH_FAULT(fault, code, text) tah_invoke_fault((fault), (code), (text), (uint32_t)(sizeof(text) - 1u))

/* ------------------------------------------------------------------------
 * JavaScript invokers
 */

static ta_op_status tah_js_start(void *self, const ta_invoker_start_params *params) {
    tah_system *system = self;
    const ta_invoke_request *request = params->request;
    uint64_t service = system->next_service + 1u;
    tah_effect effect = {
        .kind = TAH_EFFECT_INVOKE_START,
        .parent = request->parent,
        .invoke_id = request->invoke_id,
        .service = service,
        .type = request->type,
        .value = request->data,
        .target = request->src,
        .target_text = request->src_text,
        .params = request->params,
        .content_type = request->content_type,
        .content = request->content,
        .autoforward = request->autoforward,
        .trace = request->trace,
    };
    ta_value_retain(&effect.value);
    ta_value_retain(&effect.target);
    ta_value_retain(&effect.params);
    if (!tah_system_effect(system, &effect)) {
        /* Nothing was queued: core starts the invocation again (core ADR 0070). */
        return TA_OP_NOMEM;
    }
    system->next_service = service;
    *params->service_handle = service;
    return TA_OP_OK;
}

static ta_op_status tah_js_cancel(void *self, uint64_t service, ta_fault *fault) {
    (void)fault;
    tah_system *system = self;
    tah_effect effect = { .kind = TAH_EFFECT_INVOKE_CANCEL, .service = service };
    (void)tah_system_effect(system, &effect); /* dropped only while closing */
    return TA_OP_OK;
}

static ta_op_status tah_js_forward(void *self, const ta_invoker_forward_params *params) {
    tah_system *system = self;
    tah_effect effect = { .kind = TAH_EFFECT_INVOKE_FORWARD, .service = params->service_handle };
    ta_event_retain(params->event, &effect.forwarded);
    if (!tah_system_effect(system, &effect)) {
        return TAH_FAULT(params->fault, TA_ERR_NOMEM, "the invoker could not queue the forwarded event");
    }
    return TA_OP_OK;
}

static const ta_invoker_vtable tah_js_invoker_vtable = {
    .start = tah_js_start,
    .cancel = tah_js_cancel,
    .forward = tah_js_forward,
};

ta_invoker tah_js_invoker_adapter(tah_system *system) {
    return (ta_invoker){ .self = system, .v = &tah_js_invoker_vtable };
}

/* ------------------------------------------------------------------------
 * The in-system SCXML invoker
 */

static ta_op_status tah_scxml_try_start(void *self, const ta_invoker_start_params *params) {
    tah_system *system = self;
    const ta_invoke_request *request = params->request;
    if (system->effects.closed) {
        return TAH_FAULT(params->fault, TA_ERR_INVALID, "invoker.scxml: the system is closing");
    }
    ta_definition_id definition = 0;
    ta_value binding = { NULL, NULL };
    bool release = false;
    if (system->child_definition == NULL) {
        return TAH_FAULT(params->fault, TA_ERR_INVALID, "invoker.scxml: the system is closing");
    }
    ta_op_status status = system->child_definition(system->child_context,
                                                   &(tah_child_params){ .request = request,
                                                                        .out = &definition,
                                                                        .binding = &binding,
                                                                        .fault = params->fault,
                                                                        .release = &release });
    if (status != TA_OP_OK) {
        return status;
    }
    ta_actor_id child = 0;
    ta_result result = ta_actor_spawn(system->core, &(ta_actor_spawn_params){
                                                        .definition = definition,
                                                        .datamodel_binding = binding.v != NULL ? &binding : NULL,
                                                        .parent = &(ta_actor_parent){ .actor = request->parent,
                                                                                      .invoke_id = request->invoke_id },
                                                        .out = &child,
                                                        /* The child's bootstrap runs under the invoking step. */
                                                        .trace = request->trace,
                                                    });
    ta_value_release(&binding);
    if (release) {
        /* The child holds it now; without one, it is freed here (core ADR 0077). */
        (void)ta_system_release_definition(system->core, definition);
    }
    if (result != TA_OK) {
        return TAH_FAULT(params->fault, result, "invoker.scxml: could not spawn the child");
    }
    if (tah_sched_add(&system->sched, child) != TA_OK) {
        (void)ta_actor_destroy(system->core, child);
        return TAH_FAULT(params->fault, TA_ERR_NOMEM, "invoker.scxml: could not schedule the child");
    }
    tah_system_want_turn(system);
    *params->service_handle = child;
    return TA_OP_OK;
}

/*
 * Starts the child; a child that cannot be compiled or spawned is reported
 * to the parent, not answered (core ADR 0089 section 4, napi ADR 0032
 * section 3): start answers OK with no service, and
 * error.communication.invoke.<id>.failed, with data { code, message }, waits
 * in the parent's mailbox for the parent to be between macrosteps. Out of
 * memory is still answered, and core starts the invocation again.
 */
static ta_op_status tah_scxml_start(void *self, const ta_invoker_start_params *params) {
    tah_system *system = self;
    ta_op_status status = tah_scxml_try_start(self, params);
    if (status != TA_OP_ERROR) {
        return status;
    }
    ta_fault *fault = params->fault;
    ta_event event = { .type = TA_EVENT_PLATFORM, .invoke_id = params->request->invoke_id };
    bool made = system->fault_data == NULL ||
                system->fault_data(system->fault_context, fault->code, fault->message, &event.data);
    ta_value_release(&fault->data);
    *fault = (ta_fault){ 0 };
    if (!made) {
        return TA_OP_NOMEM;
    }
    ta_trace_parent trace = params->request->trace;
    ta_result result = tah_sched_post(&system->sched, &(tah_message){ .target = params->request->parent,
                                                                      .event = &event,
                                                                      .trace = &trace,
                                                                      .report = TAH_REPORT_FAILED });
    if (result != TA_OK) {
        ta_value_release(&event.data);
        return TA_OP_NOMEM; /* no room for the report: core starts the invocation again */
    }
    tah_system_want_turn(system);
    *params->service_handle = 0;
    return TA_OP_OK;
}

static ta_op_status tah_scxml_cancel(void *self, uint64_t service, ta_fault *fault) {
    (void)fault;
    tah_system *system = self;
    if (service == 0) {
        return TA_OP_OK; /* a child that never started: its failure is reported */
    }
    ta_result result = ta_actor_destroy(system->core, service);
    if (result == TA_OK || result == TA_ERR_NOT_FOUND) {
        tah_sched_remove(&system->sched, service, TAH_DEAD_DESTROYED);
        tah_system_want_turn(system);
    }
    return TA_OP_OK;
}

static ta_op_status tah_scxml_forward(void *self, const ta_invoker_forward_params *params) {
    tah_system *system = self;
    if (params->service_handle == 0) {
        return TA_OP_OK; /* a child that never started has nobody to forward to */
    }
    ta_event event;
    ta_event_retain(params->event, &event);
    ta_result result = tah_sched_post(&system->sched, &(tah_message){ .target = params->service_handle, .event = &event });
    if (result != TA_OK) {
        ta_event_release(&event);
        return TAH_FAULT(params->fault, result, "invoker.scxml: the child's mailbox did not accept the event");
    }
    tah_system_want_turn(system);
    return TA_OP_OK;
}

static const ta_invoker_vtable tah_scxml_invoker_vtable = {
    .start = tah_scxml_start,
    .cancel = tah_scxml_cancel,
    .forward = tah_scxml_forward,
};

ta_result tah_scxml_invoker_register(tah_system *system) {
    static const char type[] = "http://www.w3.org/TR/scxml/";
    ta_atom atom = 0;
    ta_result result = ta_system_intern_atom(system->core, (ta_str){ .ptr = type, .len = sizeof type - 1u }, &atom);
    if (result != TA_OK) {
        return result;
    }
    return ta_system_register_invoker(system->core, atom,
                                      (ta_invoker){ .self = system, .v = &tah_scxml_invoker_vtable });
}
