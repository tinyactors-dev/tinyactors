#pragma once

#include "event.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct { ta_actor_id parent; ta_invoke_id invoke_id; ta_atom type; ta_value src; ta_value data; bool autoforward; } ta_invoke_request;
typedef struct { const ta_invoke_request *request; uint64_t *service_handle; ta_fault *fault; } ta_invoker_start_params;
typedef struct { uint64_t service_handle; const ta_event *event; ta_fault *fault; } ta_invoker_forward_params;
typedef struct ta_invoker_vtable {
    ta_op_status (*start)(void *self, const ta_invoker_start_params *params);
    ta_op_status (*cancel)(void *self, uint64_t service_handle, ta_fault *fault);
    ta_op_status (*forward)(void *self, const ta_invoker_forward_params *params);
} ta_invoker_vtable;
typedef struct { void *self; const ta_invoker_vtable *v; } ta_invoker;

#ifdef __cplusplus
}
#endif

#include "fault.h"

typedef struct {
    ta_invoke_id id;
    ta_atom type;
    uint64_t service_handle;
    bool autoforward;
    bool active;
} ta_active_invoke;

typedef struct {
    ta_atom type;
    ta_invoker invoker;
} ta_invoker_registration;
