#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "binding.h"
#include "instruments.h"
#include "json_datamodel.h"
#include "ta_scxml.h"
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
/* datamodel.null: SCXML B.1, the minimum profile, which core ships (ADR 0057). */
static bool tat_make_null_datamodel(tat_bind *bind, jv config, void *out) {
    if (!tat_no_config(bind, config)) {
        return false;
    }
    *(ta_datamodel *)out = ta_null_datamodel();
    return true;
}

static bool tat_make_ecmascript_datamodel(tat_bind *bind, jv config, void *out) {
    if (!tat_no_config(bind, config)) {
        return false;
    }
    *(ta_datamodel *)out = tat_js_datamodel(&bind->js);
    return true;
}

/*
 * router.direct: delivers each external event straight to a resident actor
 * with ta_actor_deliver(). A runtime destination is delivered by ID; a named
 * one is resolved first with ta_actor_find(), which goes through the system's
 * actor directory (ADR 0037), so a chart may address a peer as
 * `#_actor_<family>/<name>` without the host installing a routing instrument.
 * Resolution answers TA_ERR_UNSUPPORTED when no directory is installed,
 * TA_ERR_UNLOADED for an actor the directory holds but has not loaded (this
 * router cannot load one; a real host would), and TA_ERR_NOT_FOUND for an
 * address it does not know. A busy actor, or one that ran out of memory
 * taking the event, makes the router answer RETRY, so core keeps the event
 * and offers it again on a later dispatch. Config:
 * {"system": <system handle>}.
 */
static ta_external_event_submit_status tat_direct_submit(void *self,
                                                         const ta_external_event_submit_params *params) {
    static const char unresolved[] = "router.direct: the destination address resolves to no resident actor";
    static const char not_found[] = "router.direct: no resident actor at that destination";
    const ta_external_delivery *delivery = params->delivery;
    ta_actor_id id;
    if (delivery->destination.kind == TA_ACTOR_REF_RUNTIME) {
        id = delivery->destination.as.runtime;
    } else {
        ta_actor_address view = ta_address_view(delivery->destination.as.named);
        ta_result found = ta_actor_find(self, &(ta_actor_find_params){.address = &view, .out = &id});
        if (found != TA_OK) {
            *params->fault =
                (ta_fault){.code = found, .message = {.ptr = unresolved, .len = sizeof(unresolved) - 1}};
            return TA_EXTERNAL_EVENT_ERROR;
        }
    }
    ta_result result = ta_actor_deliver(self, &(ta_actor_deliver_params){ .time = ta_system_time(self), .actor = id, .event = &delivery->event, .trace = delivery->trace });
    if (result == TA_OK) {
        return TA_EXTERNAL_EVENT_ACCEPTED;
    }
    /* A busy receiver, or one that could not take the event for lack of
     * memory, takes it later: running out of memory is never an error the
     * sender's chart sees (ADR 0047, ADR 0070). */
    if (result == TA_ERR_BUSY || result == TA_ERR_NOMEM) {
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
 * codec.scxml: the SCXML XML codec (tinyactors-scxml, ADR 0053) as a
 * ta_chart_codec. Config: null for the default limits, or an object with any
 * of the fields of ta_scxml_codec_params ("max_depth": 32, ...). One codec
 * per factory call, living as long as the host process.
 */
static bool tat_config_u64(jv config, const char *name, uint64_t *out) {
    jv value = jv_object_get(jv_copy(config), jv_string(name));
    bool ok = true;
    if (jv_get_kind(value) == JV_KIND_NUMBER) {
        double number = jv_number_value(value);
        ok = number >= 0 && number <= 18446744073709551615.0;
        *out = (uint64_t)number;
    } else {
        ok = jv_get_kind(value) == JV_KIND_INVALID || jv_get_kind(value) == JV_KIND_NULL;
    }
    jv_free(value);
    return ok;
}

static bool tat_make_scxml_codec(tat_bind *bind, jv config, void *out) {
    ta_scxml_codec_params params = {0};
    if (jv_is_valid(config) && jv_get_kind(config) == JV_KIND_OBJECT) {
        uint64_t depth = 0, attributes = 0, text = 0, names = 0, name_length = 0;
        bool ok = tat_config_u64(config, "max_document_bytes", &params.max_document_bytes) &&
                  tat_config_u64(config, "max_parse_bytes", &params.max_parse_bytes) &&
                  tat_config_u64(config, "max_depth", &depth) &&
                  tat_config_u64(config, "max_attributes", &attributes) &&
                  tat_config_u64(config, "max_text_bytes", &text) &&
                  tat_config_u64(config, "max_entity_expansion", &params.max_entity_expansion) &&
                  tat_config_u64(config, "max_name_length", &name_length) &&
                  tat_config_u64(config, "max_names", &names);
        if (!ok || depth > UINT32_MAX || attributes > UINT32_MAX || text > UINT32_MAX || names > UINT32_MAX ||
            name_length > UINT32_MAX) {
            return tat_fail(bind, "codec.scxml: limits are non-negative integers");
        }
        params.max_depth = (uint32_t)depth;
        params.max_attributes = (uint32_t)attributes;
        params.max_text_bytes = (uint32_t)text;
        params.max_names = (uint32_t)names;
        params.max_name_length = (uint32_t)name_length;
    } else if (!tat_no_config(bind, config)) {
        return false;
    }
    ta_scxml_codec *codec = malloc(sizeof(*codec));
    if (codec == NULL) {
        return tat_fail(bind, "out of memory");
    }
    if (ta_scxml_codec_init(codec, &params) != TA_OK) {
        free(codec);
        return tat_fail(bind, "codec.scxml: a limit is beyond what txml can represent");
    }
    *(ta_chart_codec *)out = ta_scxml_codec_adapter(codec);
    return true;
}

/*
 * resolver.map: a ta_chart_src_resolver over fixed resources, the way a host
 * would serve <data src> and <script src> from its own store. Config:
 * {"files": {src: text}}. An unknown src is TA_ERR_NOT_FOUND.
 */
static ta_result tat_map_fetch(void *self, ta_str src, ta_arena *arena, ta_bytes *out) {
    jv *files = self;
    jv text = jv_object_get(jv_copy(*files), jv_string_sized(src.ptr, (int)src.len));
    if (jv_get_kind(text) != JV_KIND_STRING) {
        jv_free(text);
        return TA_ERR_NOT_FOUND;
    }
    int length = jv_string_length_bytes(jv_copy(text));
    uint8_t *copy = ta_arena_alloc(arena, length == 0 ? 1 : (size_t)length, 1);
    if (copy == NULL) {
        jv_free(text);
        return TA_ERR_NOMEM;
    }
    memcpy(copy, jv_string_value(text), (size_t)length);
    jv_free(text);
    *out = (ta_bytes){.ptr = copy, .len = (uint32_t)length};
    return TA_OK;
}

static const ta_chart_src_resolver_vtable TAT_MAP_RESOLVER = {.fetch = tat_map_fetch};

/* Lives as long as the host process, like the codec. */
static jv *tat_files_config(jv config) {
    jv files = jv_object();
    if (jv_is_valid(config) && jv_get_kind(config) == JV_KIND_OBJECT) {
        jv given = jv_object_get(jv_copy(config), jv_string("files"));
        if (jv_get_kind(given) == JV_KIND_OBJECT) {
            jv_free(files);
            files = given;
        } else {
            jv_free(given);
        }
    }
    jv *stored = malloc(sizeof(*stored));
    if (stored == NULL) {
        jv_free(files);
        return NULL;
    }
    *stored = files;
    return stored;
}

static bool tat_make_map_resolver(tat_bind *bind, jv config, void *out) {
    jv *files = tat_files_config(config);
    if (files == NULL) {
        return tat_fail(bind, "out of memory");
    }
    *(ta_chart_src_resolver *)out = (ta_chart_src_resolver){.self = files, .v = &TAT_MAP_RESOLVER};
    return true;
}

/*
 * invoker.scxml: the SCXML invoker type. It runs each invoked machine as a
 * child actor of the invoking session. The machine is the invoke's inline
 * <content>, the value of its <content expr>, or, for src="file:NAME.scxml",
 * the chart "irp_NAME" from config.charts. <param>s become the child's
 * data-model binding. Core routes events between parent and child and
 * reports done.invoke itself. Config: {"system": <handle>, "charts": {name:
 * spec or SCXML text}, "files": {src: text}}.
 *
 * Literal <content> arrives as bytes with the media type its front end gave
 * it (ADR 0053); core does not import it. The invoker runs what it has a
 * reader for:
 *
 *   application/scxml+xml                       an SCXML document, decoded by
 *                                               codec.scxml
 *   application/vnd.tinyactors.scxml-spec+json  a chart spec as JSON (the
 *                                               Ruby DSL's `chart:`)
 *
 * Any other media type is refused, which the core reports to the parent as
 * error.execution. A <content expr> value that is a string is SCXML text; one
 * that is an object is a spec.
 *
 * Values cross into the child only through the RPC codec: src, content, and
 * params are encoded with the parent's data model and the binding is decoded
 * with the child's. A spec's child uses the data model its `datamodel`
 * names ("jq" or "ecmascript"), or the parent's when absent; an SCXML
 * document is compiled with the parent's, which its `datamodel` attribute
 * must then match. A child whose data model differs from the parent's is
 * refused.
 */
typedef struct {
    tat_bind *bind;
    ta_system *system;
    jv charts;
    jv *files;
    ta_scxml_codec codec;
    /* Canonical source -> registered ta_definition_id as decimal text. */
    jv definitions;
} tat_scxml_invoker;

static void tat_invoker_fault(ta_fault *fault, const char *message) {
    *fault = (ta_fault){.code = TA_ERR_INVALID, .message = {.ptr = message, .len = (uint32_t)strlen(message)}};
}

#define TAT_SPEC_MEDIA_TYPE "application/vnd.tinyactors.scxml-spec+json"

static bool tat_str_is(ta_str text, const char *literal) {
    size_t length = strlen(literal);
    return text.len == length && (length == 0 || memcmp(text.ptr, literal, length) == 0);
}

/* What the invoke names as its child: a spec (JSON object) or SCXML text. */
typedef enum { TAT_CHILD_NONE, TAT_CHILD_SPEC, TAT_CHILD_SCXML, TAT_CHILD_UNREADABLE } tat_child_kind;

static tat_child_kind tat_invoker_child(tat_scxml_invoker *invoker, const ta_invoke_request *request, jv *out) {
    jv value;
    if (request->content.ptr != NULL) {
        if (tat_str_is(request->content_type, TA_SCXML_MEDIA_TYPE)) {
            *out = jv_string_sized((const char *)request->content.ptr, (int)request->content.len);
            return TAT_CHILD_SCXML;
        }
        if (!tat_str_is(request->content_type, TAT_SPEC_MEDIA_TYPE)) {
            return TAT_CHILD_UNREADABLE;
        }
        value = jv_parse_sized((const char *)request->content.ptr, (int)request->content.len);
        if (jv_get_kind(value) == JV_KIND_OBJECT) {
            *out = value;
            return TAT_CHILD_SPEC;
        }
        jv_free(value);
        return TAT_CHILD_NONE;
    }
    if (request->data.v != NULL) {
        if (tat_value_codec_for_value(&request->data) == NULL) {
            return TAT_CHILD_NONE;
        }
        value = tat_value_encode(&request->data);
    } else {
        /* A literal src arrives as text; a srcexpr as a data-model value. */
        jv src;
        if (request->src_text.ptr != NULL) {
            src = jv_string_sized(request->src_text.ptr, (int)request->src_text.len);
        } else if (request->src.v == NULL || tat_value_codec_for_value(&request->src) == NULL) {
            return TAT_CHILD_NONE;
        } else {
            src = tat_value_encode(&request->src);
        }
        value = jv_invalid();
        if (jv_get_kind(src) == JV_KIND_STRING) {
            const char *text = jv_string_value(src);
            size_t length = strlen(text);
            static const char prefix[] = "file:";
            static const char suffix[] = ".scxml";
            if (length > sizeof(prefix) - 1 + sizeof(suffix) - 1 && strncmp(text, prefix, sizeof(prefix) - 1) == 0 &&
                strcmp(text + length - (sizeof(suffix) - 1), suffix) == 0) {
                jv name = jv_string_fmt("irp_%.*s", (int)(length - (sizeof(prefix) - 1) - (sizeof(suffix) - 1)),
                                        text + sizeof(prefix) - 1);
                value = jv_object_get(jv_copy(invoker->charts), name);
            }
        }
        jv_free(src);
    }
    if (jv_get_kind(value) == JV_KIND_OBJECT) {
        *out = value;
        return TAT_CHILD_SPEC;
    }
    if (jv_get_kind(value) == JV_KIND_STRING) {
        *out = value;
        return TAT_CHILD_SCXML;
    }
    jv_free(value);
    return TAT_CHILD_NONE;
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

/* A definition compiled earlier from the same source and data model, unless
 * it has been retired and freed since (its ID is then stale, ADR 0043). */
static bool tat_invoker_cached(tat_scxml_invoker *invoker, jv key, ta_definition_id *out) {
    jv cached = jv_object_get(jv_copy(invoker->definitions), jv_copy(key));
    bool found = false;
    if (jv_get_kind(cached) == JV_KIND_STRING) {
        *out = (ta_definition_id)strtoull(jv_string_value(cached), NULL, 10);
        ta_definition_inspection facts;
        found = ta_definition_inspect(invoker->system, *out, &facts) == TA_OK && !facts.retired;
    }
    jv_free(cached);
    return found;
}

static void tat_invoker_remember(tat_scxml_invoker *invoker, jv key, ta_definition_id id) {
    invoker->definitions = jv_object_set(invoker->definitions, key, jv_string_fmt("%llu", (unsigned long long)id));
}

/* A chart spec, compiled once per data model that invokes it and kept.
 * TA_ERR_NOMEM leaves nothing behind (ADR 0070). */
static ta_result tat_invoker_spec_definition(tat_scxml_invoker *invoker, jv spec, ta_datamodel model,
                                             ta_definition_id *out) {
    jv key = jv_string_fmt("spec:%.*s:", (int)model.v->name.len, model.v->name.ptr);
    key = jv_string_concat(key, jv_dump_string(jv_copy(spec), JV_PRINT_SORTED));
    if (tat_invoker_cached(invoker, key, out)) {
        jv_free(key);
        return TA_OK;
    }
    tat_bind *bind = invoker->bind;
    ta_scxml_spec decoded;
    ta_result result = tat_decode_ta_scxml_spec(bind, spec, &decoded) ? TA_OK : TA_ERR_INVALID;
    /* A nested decode must not leave its error on the enclosing request. */
    jv_free(bind->error);
    bind->error = jv_invalid();
    ta_definition *definition = NULL;
    if (result == TA_OK) {
        result = ta_definition_compile_spec(invoker->system, &(ta_definition_compile_params){
                                                                 .spec = &decoded,
                                                                 .datamodel = model,
                                                                 .allocator = tat_libc_allocator(),
                                                                 .out = &definition,
                                                             });
    }
    if (result == TA_OK) {
        result = ta_system_register_definition(invoker->system, definition, out);
        if (result != TA_OK) {
            ta_definition_destroy(definition);
        }
    }
    if (result == TA_OK) {
        tat_invoker_remember(invoker, key, *out);
    } else {
        jv_free(key);
    }
    return result;
}

/* An SCXML document, imported through the codec with the parent's data
 * model. TA_ERR_UNSUPPORTED when the document requires another one. */
static ta_result tat_invoker_scxml_definition(tat_scxml_invoker *invoker, jv text, ta_datamodel model,
                                              ta_definition_id *out) {
    jv key = jv_string_fmt("scxml:%.*s:", (int)model.v->name.len, model.v->name.ptr);
    key = jv_string_concat(key, jv_copy(text));
    if (tat_invoker_cached(invoker, key, out)) {
        jv_free(key);
        return TA_OK;
    }
    ta_arena arena;
    ta_result result = ta_arena_init(&arena, tat_libc_allocator(), 16384);
    if (result == TA_OK) {
        result = ta_definition_import(
            invoker->system,
            &(ta_definition_import_params){
                .codec = ta_scxml_codec_adapter(&invoker->codec),
                .input = {.ptr = (const uint8_t *)jv_string_value(text), .len = (uint32_t)jv_string_length_bytes(jv_copy(text))},
                .arena = &arena,
                .resolver = {.self = invoker->files, .v = &TAT_MAP_RESOLVER},
                .datamodels = &model,
                .datamodel_count = 1,
                .allocator = tat_libc_allocator(),
                .out = out,
            });
        ta_arena_destroy(&arena);
    }
    if (result == TA_OK) {
        tat_invoker_remember(invoker, key, *out);
    } else {
        jv_free(key);
    }
    return result;
}

/*
 * <param>s arrive as the parent data model's {name: value} object, encoded
 * as wire JSON. Only names that match one of the child's top-level <data>
 * IDs enter its binding (SCXML 6.4); the definition's export says which.
 */
static bool tat_invoker_binding(tat_scxml_invoker *invoker, const ta_invoke_request *request,
                                ta_definition_id definition, jv *out) {
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
    ta_arena arena;
    const ta_scxml_spec *spec = NULL;
    bool ok = ta_arena_init(&arena, tat_libc_allocator(), 4096) == TA_OK;
    ok = ok && ta_definition_export(invoker->system, &(ta_definition_export_params){
                                                         .definition = definition, .arena = &arena, .out = &spec}) == TA_OK;
    for (uint32_t index = 0; ok && spec->data != NULL && spec->data[index].name.ptr != NULL; index += 1u) {
        jv name = jv_string_sized(spec->data[index].name.ptr, (int)spec->data[index].name.len);
        if (jv_object_has(jv_copy(params), jv_copy(name))) {
            *out = jv_object_set(*out, jv_copy(name), jv_object_get(jv_copy(params), jv_copy(name)));
        }
        jv_free(name);
    }
    ta_arena_destroy(&arena);
    jv_free(params);
    return ok;
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
    jv child;
    tat_child_kind kind = tat_invoker_child(invoker, request, &child);
    if (kind == TAT_CHILD_UNREADABLE) {
        tat_invoker_fault(params->fault, "invoker.scxml: no reader for the content's media type");
        return TA_OP_ERROR;
    }
    if (kind == TAT_CHILD_NONE) {
        tat_invoker_fault(params->fault, "invoker.scxml: no chart in the invoke's content or src");
        return TA_OP_ERROR;
    }
    ta_datamodel model = parent;
    ta_definition_id definition = 0;
    if (kind == TAT_CHILD_SPEC) {
        if (!tat_invoker_child_model(invoker, child, parent, &model)) {
            jv_free(child);
            tat_invoker_fault(params->fault, "invoker.scxml: the child names an unknown data model");
            return TA_OP_ERROR;
        }
        if (model.v != parent.v || model.self != parent.self) {
            jv_free(child);
            tat_invoker_fault(params->fault, "invoker.scxml: the child's data model differs from the parent's");
            return TA_OP_ERROR;
        }
        ta_result compiled = tat_invoker_spec_definition(invoker, child, model, &definition);
        jv_free(child);
        if (compiled == TA_ERR_NOMEM) {
            return TA_OP_NOMEM;
        }
        if (compiled != TA_OK) {
            tat_invoker_fault(params->fault, "invoker.scxml: the child chart did not compile");
            return TA_OP_ERROR;
        }
    } else {
        ta_result imported = tat_invoker_scxml_definition(invoker, child, model, &definition);
        jv_free(child);
        if (imported == TA_ERR_NOMEM) {
            return TA_OP_NOMEM;
        }
        if (imported != TA_OK) {
            tat_invoker_fault(params->fault, imported == TA_ERR_UNSUPPORTED
                                                 ? "invoker.scxml: the child's data model differs from the parent's"
                                                 : "invoker.scxml: the child document did not import");
            return TA_OP_ERROR;
        }
    }
    jv binding;
    bool ok = tat_invoker_binding(invoker, request, definition, &binding);
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
    ta_actor_id spawned = 0;
    ta_result result = ta_actor_spawn(invoker->system,
                                      &(ta_actor_spawn_params){
                                          .definition = definition,
                                          .datamodel_binding = &value,
                                          .parent = &(ta_actor_parent){.actor = request->parent,
                                                                       .invoke_id = request->invoke_id},
                                          .out = &spawned,
                                          /* The child's bootstrap runs under the invoking step (ADR 0067). */
                                          .trace = request->trace,
                                      });
    ta_value_release(&value);
    /* A spawn that ran out of memory left no child: nothing started, and
     * core starts the invocation again later (ADR 0070). */
    if (result == TA_ERR_NOMEM) {
        return TA_OP_NOMEM;
    }
    if (result != TA_OK) {
        tat_invoker_fault(params->fault, "invoker.scxml: could not spawn the child");
        return TA_OP_ERROR;
    }
    *params->service_handle = spawned;
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
    if (ta_actor_deliver(invoker->system, &(ta_actor_deliver_params){ .time = ta_system_time(invoker->system), .actor = params->service_handle, .event = params->event }) != TA_OK) {
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
    jv *files = tat_files_config(config);
    if (invoker == NULL || files == NULL) {
        free(invoker);
        free(files);
        jv_free(charts);
        return tat_fail(bind, "out of memory");
    }
    *invoker = (tat_scxml_invoker){
        .bind = bind, .system = handle, .charts = charts, .files = files, .definitions = jv_object()};
    (void)ta_scxml_codec_init(&invoker->codec, NULL);
    *(ta_invoker *)out = (ta_invoker){.self = invoker, .v = &TAT_SCXML_INVOKER};
    return true;
}

const tat_adapter TAT_ADAPTERS[] = {
    {"allocator.libc", "ta_allocator", sizeof(ta_allocator), tat_make_libc_allocator},
    {"datamodel.jq", "ta_datamodel", sizeof(ta_datamodel), tat_make_jq_datamodel},
    {"datamodel.ecmascript", "ta_datamodel", sizeof(ta_datamodel), tat_make_ecmascript_datamodel},
    {"datamodel.null", "ta_datamodel", sizeof(ta_datamodel), tat_make_null_datamodel},
    {"router.direct", "ta_external_event_router", sizeof(ta_external_event_router),
     tat_make_direct_router},
    {"invoker.scxml", "ta_invoker", sizeof(ta_invoker), tat_make_scxml_invoker},
    {"codec.scxml", "ta_chart_codec", sizeof(ta_chart_codec), tat_make_scxml_codec},
    {"resolver.map", "ta_chart_src_resolver", sizeof(ta_chart_src_resolver), tat_make_map_resolver},
    /* Instrumented adapters: src/instruments.c. */
    {"allocator.counting", "ta_allocator", sizeof(ta_allocator), tat_make_counting_allocator},
    {"router.recording", "ta_external_event_router", sizeof(ta_external_event_router), tat_make_recording_router},
    {"directory.memory", "ta_actor_directory", sizeof(ta_actor_directory), tat_make_memory_directory},
    {"invoker.scripted", "ta_invoker", sizeof(ta_invoker), tat_make_scripted_invoker},
    {"io.recording", "ta_io", sizeof(ta_io), tat_make_recording_io},
    {"io.peer", "ta_io", sizeof(ta_io), tat_make_peer_io},
    {"io.connection", "ta_io", sizeof(ta_io), tat_make_connection_io},
    {"io.request", "ta_io", sizeof(ta_io), tat_make_request_io},
    {"log.recording", "ta_log_sink", sizeof(ta_log_sink), tat_make_recording_log},
    {"trace.recording", "ta_trace_masks", sizeof(ta_trace_masks), tat_make_recording_trace},
    {"definition.recording", "ta_definition_sink", sizeof(ta_definition_sink), tat_make_recording_definition},
    {"inspector.recording", "ta_actor_inspector", sizeof(ta_actor_inspector), tat_make_recording_inspector},
};

const size_t TAT_ADAPTER_COUNT = sizeof(TAT_ADAPTERS) / sizeof(TAT_ADAPTERS[0]);
