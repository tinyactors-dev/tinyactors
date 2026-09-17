#pragma once

#include "event.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct { ta_system *system; ta_actor_id parent; ta_invoke_id invoke_id; ta_atom type; ta_value src; ta_value data; bool autoforward; } ta_invoke_request;
typedef struct { const ta_invoke_request *request; uint64_t *service_handle; ta_fault *fault; } ta_invoker_start_params;
typedef struct { uint64_t service_handle; const ta_event *event; ta_fault *fault; } ta_invoker_forward_params;

/*
 * Asynchronous outcome of a previously successful invoker start.  EVENT and
 * DONE enter the actor's external-event flow.  UNAVAILABLE retires the service
 * and appends error.communication.invoke.<id>.unavailable to the internal queue.
 */
typedef enum {
    TA_INVOKE_REPORT_EVENT = 0,
    TA_INVOKE_REPORT_DONE,
    TA_INVOKE_REPORT_UNAVAILABLE
} ta_invoke_report_kind;

typedef struct {
    ta_actor_id actor;
    ta_invoke_id invoke_id;
    ta_invoke_report_kind kind;
    union {
        const ta_event *event;
        const ta_value *done_data;
        const ta_fault *fault;
    } as;
} ta_invoke_report_params;

typedef struct ta_invoker_vtable {
    /* start must not report reentrantly through ta_actor_report_invoke(). */
    ta_op_status (*start)(void *self, const ta_invoker_start_params *params);
    ta_op_status (*cancel)(void *self, uint64_t service_handle, ta_fault *fault);
    ta_op_status (*forward)(void *self, const ta_invoker_forward_params *params);
} ta_invoker_vtable;
typedef struct { void *self; const ta_invoker_vtable *v; } ta_invoker;

/* Externally synchronize this call; do not call it reentrantly from start. */
ta_result ta_actor_report_invoke(ta_system *system,
                                 const ta_invoke_report_params *params);

#ifdef __cplusplus
}
#endif

#include "fault.h"

typedef struct {
    ta_invoke_id id;
    ta_atom type;
    /* Runtime-only: session images do not retain or reconstruct this handle. */
    uint64_t service_handle;
    bool autoforward;
    bool active;
} ta_active_invoke;

typedef struct {
    ta_atom type;
    ta_invoker invoker;
} ta_invoker_registration;
