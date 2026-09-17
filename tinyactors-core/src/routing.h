#pragma once

#include "event.h"
#include "fault.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A fully normalized event destined for another SCXML external event queue. */
typedef struct {
    ta_actor_ref destination;
    ta_event event;
} ta_external_delivery;

/* Deep-copies a named destination and clones event data. */
ta_result ta_external_delivery_clone(const ta_external_delivery *delivery,
                                     ta_allocator allocator,
                                     ta_external_delivery *out);
void ta_external_delivery_release(ta_external_delivery *delivery,
                                  ta_allocator allocator);

typedef struct {
    const ta_external_delivery *delivery;
    ta_fault *fault;
} ta_external_event_submit_params;

typedef enum {
    /* Delivery was accepted; an asynchronous router copied borrowed fields. */
    TA_EXTERNAL_EVENT_ACCEPTED = 0,
    /* Nothing was accepted; core retains and retries the delivery. */
    TA_EXTERNAL_EVENT_RETRY,
    /* Nothing was accepted; fault describes a communication failure. */
    TA_EXTERNAL_EVENT_ERROR
} ta_external_event_submit_status;

typedef struct ta_external_event_router_vtable {
    ta_external_event_submit_status (*submit)(
        void *self,
        const ta_external_event_submit_params *params);
} ta_external_event_router_vtable;

typedef struct {
    void *self;
    const ta_external_event_router_vtable *v;
} ta_external_event_router;

#ifdef __cplusplus
}
#endif
