#include "allocator.h"
#include "binding.h"
#include "json_datamodel.h"

/*
 * Hand-written adapter factories. A vtable-bearing argument crosses the
 * protocol as {"$adapter": "<name>", "config": {...}}; callbacks never become
 * nested RPC conversations, so every adapter behaves synchronously in C.
 */

static bool tat_no_config(tat_bind *bind, jv config) {
    if (!jv_is_valid(config) || jv_get_kind(config) == JV_KIND_NULL) {
        return true;
    }
    return tat_fail(bind, "this adapter takes no config");
}

static bool tat_make_libc_allocator(tat_bind *bind, jv config, void *out) {
    if (!tat_no_config(bind, config)) {
        return false;
    }
    *(ta_allocator *)out = tat_libc_allocator();
    return true;
}

static bool tat_make_jq_datamodel(tat_bind *bind, jv config, void *out) {
    if (!tat_no_config(bind, config)) {
        return false;
    }
    *(ta_datamodel *)out = tat_json_datamodel(&bind->json);
    return true;
}

/*
 * router.direct: delivers each external event straight to a resident runtime
 * actor with ta_actor_deliver(). A busy actor makes the router answer RETRY,
 * so core keeps the event and offers it again on a later dispatch. Config:
 * {"system": <system handle>}.
 */
static ta_external_event_submit_status tat_direct_submit(void *self,
                                                         const ta_external_event_submit_params *params) {
    static const char not_runtime[] = "router.direct delivers to runtime actor IDs only";
    static const char not_found[] = "router.direct: no resident actor with that ID";
    const ta_external_delivery *delivery = params->delivery;
    if (delivery->destination.kind != TA_ACTOR_REF_RUNTIME) {
        *params->fault = (ta_fault){.code = TA_ERR_UNSUPPORTED,
                                    .message = {.ptr = not_runtime, .len = sizeof(not_runtime) - 1}};
        return TA_EXTERNAL_EVENT_ERROR;
    }
    ta_result result = ta_actor_deliver(self, delivery->destination.as.runtime, &delivery->event);
    if (result == TA_OK) {
        return TA_EXTERNAL_EVENT_ACCEPTED;
    }
    if (result == TA_ERR_BUSY) {
        return TA_EXTERNAL_EVENT_RETRY;
    }
    *params->fault = (ta_fault){.code = result, .message = {.ptr = not_found, .len = sizeof(not_found) - 1}};
    return TA_EXTERNAL_EVENT_ERROR;
}

static const ta_external_event_router_vtable TAT_DIRECT_ROUTER = {.submit = tat_direct_submit};

static bool tat_make_direct_router(tat_bind *bind, jv config, void *out) {
    jv system = jv_invalid();
    if (jv_is_valid(config) && jv_get_kind(config) == JV_KIND_OBJECT) {
        system = jv_object_get(jv_copy(config), jv_string("system"));
    }
    void *handle = NULL;
    bool ok = jv_is_valid(system) && tat_handle_resolve(bind, system, "system", &handle);
    jv_free(system);
    if (!ok) {
        return tat_fail(bind, "router.direct needs config {\"system\": <system handle>}");
    }
    *(ta_external_event_router *)out = (ta_external_event_router){.self = handle, .v = &TAT_DIRECT_ROUTER};
    return true;
}

const tat_adapter TAT_ADAPTERS[] = {
    {"allocator.libc", "ta_allocator", sizeof(ta_allocator), tat_make_libc_allocator},
    {"datamodel.jq", "ta_datamodel", sizeof(ta_datamodel), tat_make_jq_datamodel},
    {"router.direct", "ta_external_event_router", sizeof(ta_external_event_router),
     tat_make_direct_router},
};

const size_t TAT_ADAPTER_COUNT = sizeof(TAT_ADAPTERS) / sizeof(TAT_ADAPTERS[0]);
