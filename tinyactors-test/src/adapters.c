#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "binding.h"
#include "json_datamodel.h"
#include "ta_rpc.h"
#include "value_codec.h"

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

/* datamodel.ecmascript: SCXML B.2 on quickjs-ng; one runtime per host process. */
static bool tat_make_ecmascript_datamodel(tat_bind *bind, jv config, void *out) {
    if (!tat_no_config(bind, config)) {
        return false;
    }
    *(ta_datamodel *)out = tat_js_datamodel(&bind->js);
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

/*
 * invoker.scxml: the SCXML invoker type. It runs each invoked machine as a
 * child actor of the invoking session. The machine is the invoke's inline
 * <content> (a chart spec as JSON) or, for src="file:NAME.scxml", the chart
 * "irp_NAME" from config.charts. <param>s become the child's data-model
 * binding. Core routes events between parent and child and reports
 * done.invoke itself. Config: {"system": <handle>, "charts": {name: spec}}.
 *
 * Values cross into the child only through the RPC codec: src, content, and
 * params are encoded with the parent's data model and the binding is decoded
 * with the child's. The child's data model is the one its spec's `datamodel`
 * names ("jq" or "ecmascript"), or the parent's when absent; a child whose
 * data model differs from the parent's is refused, which the core reports to
 * the parent as error.execution.
 */
typedef struct {
    tat_bind *bind;
    ta_system *system;
    jv charts;
    /* Canonical spec JSON text -> registered ta_definition_id as decimal text. */
    jv definitions;
} tat_scxml_invoker;

static void tat_invoker_fault(ta_fault *fault, const char *message) {
    *fault = (ta_fault){.code = TA_ERR_INVALID, .message = {.ptr = message, .len = (uint32_t)strlen(message)}};
}

/* The child machine's spec: inline content, or the chart a file: src names. */
static bool tat_invoker_spec(tat_scxml_invoker *invoker, const ta_invoke_request *request, jv *out) {
    jv value;
    if (request->data.v != NULL) {
        if (tat_value_codec_for_value(&request->data) == NULL) {
            return false;
        }
        value = tat_value_encode(&request->data);
        if (jv_get_kind(value) == JV_KIND_OBJECT) {
            *out = value;
            return true;
        }
        jv_free(value);
        return false;
    }
    if (request->src.v == NULL || tat_value_codec_for_value(&request->src) == NULL) {
        return false;
    }
    value = tat_value_encode(&request->src);
    if (jv_get_kind(value) != JV_KIND_STRING) {
        jv_free(value);
        return false;
    }
    const char *src = jv_string_value(value);
    size_t length = strlen(src);
    static const char prefix[] = "file:";
    static const char suffix[] = ".scxml";
    bool ok = length > sizeof(prefix) - 1 + sizeof(suffix) - 1 && strncmp(src, prefix, sizeof(prefix) - 1) == 0 &&
              strcmp(src + length - (sizeof(suffix) - 1), suffix) == 0;
    if (ok) {
        jv name = jv_string_fmt("irp_%.*s", (int)(length - (sizeof(prefix) - 1) - (sizeof(suffix) - 1)),
                                src + sizeof(prefix) - 1);
        *out = jv_object_get(jv_copy(invoker->charts), name);
        ok = jv_get_kind(*out) == JV_KIND_OBJECT;
        if (!ok) {
            jv_free(*out);
        }
    }
    jv_free(value);
    return ok;
}

/* The child's data model: the one its spec names, or the parent's when it names none. */
static bool tat_invoker_child_model(tat_scxml_invoker *invoker, jv spec, ta_datamodel parent, ta_datamodel *out) {
    jv name = jv_object_get(jv_copy(spec), jv_string("datamodel"));
    bool ok = true;
    if (jv_get_kind(name) != JV_KIND_STRING) {
        *out = parent;
    } else if (strcmp(jv_string_value(name), "ecmascript") == 0) {
        *out = tat_js_datamodel(&invoker->bind->js);
    } else if (strcmp(jv_string_value(name), "jq") == 0) {
        *out = tat_json_datamodel(&invoker->bind->json);
    } else {
        ok = false;
    }
    jv_free(name);
    return ok;
}

static bool tat_invoker_definition(tat_scxml_invoker *invoker, jv spec, ta_datamodel model, ta_definition_id *out) {
    /* A spec without `datamodel` compiles once per data model that invokes it. */
    jv key = jv_string_fmt("%.*s:", (int)model.v->name.len, model.v->name.ptr);
    key = jv_string_concat(key, jv_dump_string(jv_copy(spec), JV_PRINT_SORTED));
    jv cached = jv_object_get(jv_copy(invoker->definitions), jv_copy(key));
    if (jv_get_kind(cached) == JV_KIND_STRING) {
        /* A retired and freed definition's ID is stale (ADR 0043): recompile. */
        *out = (ta_definition_id)strtoull(jv_string_value(cached), NULL, 10);
        ta_definition_inspection facts;
        if (ta_definition_inspect(invoker->system, *out, &facts) == TA_OK && !facts.retired) {
            jv_free(cached);
            jv_free(key);
            return true;
        }
    }
    jv_free(cached);
    tat_bind *bind = invoker->bind;
    ta_scxml_spec decoded;
    bool ok = tat_decode_ta_scxml_spec(bind, spec, &decoded);
    /* A nested decode must not leave its error on the enclosing request. */
    jv_free(bind->error);
    bind->error = jv_invalid();
    ta_definition *definition = NULL;
    ok = ok && ta_definition_compile_spec(invoker->system,
                                          &(ta_definition_compile_params){
                                              .spec = &decoded,
                                              .datamodel = model,
                                              .allocator = tat_libc_allocator(),
                                              .out = &definition,
                                          }) == TA_OK;
    if (ok && ta_system_register_definition(invoker->system, definition, out) != TA_OK) {
        ta_definition_destroy(definition);
        ok = false;
    }
    if (ok) {
        invoker->definitions = jv_object_set(invoker->definitions, jv_copy(key), jv_string_fmt("%llu", (unsigned long long)*out));
    }
    jv_free(key);
    return ok;
}

/*
 * <param>s arrive as the parent data model's {name: value} object, encoded
 * as wire JSON. Only names that match one of the child's top-level <data>
 * IDs enter its binding (SCXML 6.4).
 */
static bool tat_invoker_binding(const ta_invoke_request *request, jv spec, jv *out) {
    *out = jv_object();
    if (request->params.v == NULL) {
        return true;
    }
    if (tat_value_codec_for_value(&request->params) == NULL) {
        return false;
    }
    jv params = tat_value_encode(&request->params);
    if (jv_get_kind(params) != JV_KIND_OBJECT) {
        jv_free(params);
        return false;
    }
    jv data = jv_object_get(jv_copy(spec), jv_string("data"));
    if (jv_get_kind(data) == JV_KIND_ARRAY) {
        jv_array_foreach(data, index, declaration) {
            jv name = jv_object_get(declaration, jv_string("name"));
            if (jv_get_kind(name) == JV_KIND_STRING && jv_object_has(jv_copy(params), jv_copy(name))) {
                *out = jv_object_set(*out, jv_copy(name), jv_object_get(jv_copy(params), jv_copy(name)));
            }
            jv_free(name);
            (void)index;
        }
    }
    jv_free(data);
    jv_free(params);
    return true;
}

static ta_op_status tat_invoker_start(void *self, const ta_invoker_start_params *params) {
    tat_scxml_invoker *invoker = self;
    const ta_invoke_request *request = params->request;
    ta_datamodel parent = {0};
    if (tat_actor_datamodel(invoker->system, request->parent, &parent) != TA_OK ||
        tat_value_codec_for_model(parent) == NULL) {
        tat_invoker_fault(params->fault, "invoker.scxml: the parent's data model is unknown");
        return TA_OP_ERROR;
    }
    jv spec;
    if (!tat_invoker_spec(invoker, request, &spec)) {
        tat_invoker_fault(params->fault, "invoker.scxml: no chart in the invoke's content or src");
        return TA_OP_ERROR;
    }
    ta_datamodel model = {0};
    if (!tat_invoker_child_model(invoker, spec, parent, &model)) {
        jv_free(spec);
        tat_invoker_fault(params->fault, "invoker.scxml: the child names an unknown data model");
        return TA_OP_ERROR;
    }
    if (model.v != parent.v || model.self != parent.self) {
        jv_free(spec);
        tat_invoker_fault(params->fault, "invoker.scxml: the child's data model differs from the parent's");
        return TA_OP_ERROR;
    }
    ta_definition_id definition = 0;
    if (!tat_invoker_definition(invoker, spec, model, &definition)) {
        jv_free(spec);
        tat_invoker_fault(params->fault, "invoker.scxml: the child chart did not compile");
        return TA_OP_ERROR;
    }
    jv binding;
    bool ok = tat_invoker_binding(request, spec, &binding);
    jv_free(spec);
    if (!ok) {
        jv_free(binding);
        tat_invoker_fault(params->fault, "invoker.scxml: invalid invoke params");
        return TA_OP_ERROR;
    }
    ta_value value;
    ta_result decoded = tat_value_decode(model, binding, &value);
    jv_free(binding);
    if (decoded != TA_OK) {
        tat_invoker_fault(params->fault, "invoker.scxml: could not make the child binding");
        return TA_OP_ERROR;
    }
    ta_actor_id child = 0;
    ta_result result = ta_actor_spawn(invoker->system,
                                      &(ta_actor_spawn_params){
                                          .definition = definition,
                                          .datamodel_binding = &value,
                                          .parent = &(ta_actor_parent){.actor = request->parent,
                                                                       .invoke_id = request->invoke_id},
                                          .out = &child,
                                      });
    ta_value_release(&value);
    if (result != TA_OK) {
        tat_invoker_fault(params->fault, "invoker.scxml: could not spawn the child");
        return TA_OP_ERROR;
    }
    *params->service_handle = child;
    return TA_OP_OK;
}

static ta_op_status tat_invoker_cancel(void *self, uint64_t service_handle, ta_fault *fault) {
    (void)fault;
    tat_scxml_invoker *invoker = self;
    (void)ta_actor_destroy(invoker->system, service_handle);
    return TA_OP_OK;
}

static ta_op_status tat_invoker_forward(void *self, const ta_invoker_forward_params *params) {
    tat_scxml_invoker *invoker = self;
    if (ta_actor_deliver(invoker->system, params->service_handle, params->event) != TA_OK) {
        tat_invoker_fault(params->fault, "invoker.scxml: the child did not accept the event");
        return TA_OP_ERROR;
    }
    return TA_OP_OK;
}

static const ta_invoker_vtable TAT_SCXML_INVOKER = {
    .start = tat_invoker_start,
    .cancel = tat_invoker_cancel,
    .forward = tat_invoker_forward,
};

static bool tat_make_scxml_invoker(tat_bind *bind, jv config, void *out) {
    jv system = jv_invalid();
    jv charts = jv_object();
    if (jv_is_valid(config) && jv_get_kind(config) == JV_KIND_OBJECT) {
        system = jv_object_get(jv_copy(config), jv_string("system"));
        jv given = jv_object_get(jv_copy(config), jv_string("charts"));
        if (jv_get_kind(given) == JV_KIND_OBJECT) {
            jv_free(charts);
            charts = given;
        } else {
            jv_free(given);
        }
    }
    void *handle = NULL;
    bool ok = jv_is_valid(system) && tat_handle_resolve(bind, system, "system", &handle);
    jv_free(system);
    if (!ok) {
        jv_free(charts);
        return tat_fail(bind, "invoker.scxml needs config {\"system\": <system handle>, \"charts\": {...}}");
    }
    /* Lives as long as the host process; a scenario creates one system and one invoker. */
    tat_scxml_invoker *invoker = malloc(sizeof(*invoker));
    if (invoker == NULL) {
        jv_free(charts);
        return tat_fail(bind, "out of memory");
    }
    *invoker = (tat_scxml_invoker){.bind = bind, .system = handle, .charts = charts, .definitions = jv_object()};
    *(ta_invoker *)out = (ta_invoker){.self = invoker, .v = &TAT_SCXML_INVOKER};
    return true;
}

const tat_adapter TAT_ADAPTERS[] = {
    {"allocator.libc", "ta_allocator", sizeof(ta_allocator), tat_make_libc_allocator},
    {"datamodel.jq", "ta_datamodel", sizeof(ta_datamodel), tat_make_jq_datamodel},
    {"datamodel.ecmascript", "ta_datamodel", sizeof(ta_datamodel), tat_make_ecmascript_datamodel},
    {"router.direct", "ta_external_event_router", sizeof(ta_external_event_router),
     tat_make_direct_router},
    {"invoker.scxml", "ta_invoker", sizeof(ta_invoker), tat_make_scxml_invoker},
};

const size_t TAT_ADAPTER_COUNT = sizeof(TAT_ADAPTERS) / sizeof(TAT_ADAPTERS[0]);
