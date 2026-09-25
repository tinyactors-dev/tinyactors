#include "napi_children.h"

#include <stdio.h>
#include <string.h>

#include "import.h"
#include "napi_system.h"

typedef struct {
    ta_definition_id definition;
    napi_ref resolve;
} tna_child_resolver;

typedef struct {
    ta_definition_id definition;
    uint8_t *bytes;
    uint32_t length;
} tna_child_document;

static ta_allocator tna_children_allocator(tna_napi_system *wrapper) {
    return tna_allocator_for(&wrapper->system->allocator, TNA_MEMORY_OTHER);
}

/* ------------------------------------------------------------------------
 * resolve(src) as a core resolver
 */

static ta_result tna_fetch_src(void *self, ta_str src, ta_arena *arena, ta_bytes *out) {
    tna_src_resolver *resolver = self;
    napi_env env = resolver->env;
    napi_value text = NULL;
    napi_value result = NULL;
    napi_value global = NULL;
    if (napi_create_string_utf8(env, src.len == 0 ? "" : src.ptr, src.len, &text) != napi_ok ||
        napi_get_global(env, &global) != napi_ok ||
        napi_call_function(env, global, resolver->resolve, 1, &text, &result) != napi_ok) {
        return TA_ERR_NOT_FOUND;
    }
    napi_valuetype type = napi_undefined;
    bool typed = false;
    if (napi_typeof(env, result, &type) != napi_ok || type == napi_undefined || type == napi_null) {
        return TA_ERR_NOT_FOUND;
    }
    size_t length = 0;
    if (type == napi_string) {
        if (napi_get_value_string_utf8(env, result, NULL, 0, &length) != napi_ok || length > UINT32_MAX - 1u) {
            return TA_ERR_NOT_FOUND;
        }
        char *copy = ta_arena_alloc(arena, length + 1u, 1);
        if (copy == NULL) {
            return TA_ERR_NOMEM;
        }
        napi_get_value_string_utf8(env, result, copy, length + 1u, &length);
        *out = (ta_bytes){ .ptr = (const uint8_t *)copy, .len = (uint32_t)length };
        return TA_OK;
    }
    napi_typedarray_type kind = napi_uint8_array;
    void *data = NULL;
    if (napi_is_typedarray(env, result, &typed) != napi_ok || !typed ||
        napi_get_typedarray_info(env, result, &kind, &length, &data, NULL, NULL) != napi_ok ||
        kind != napi_uint8_array || length > UINT32_MAX) {
        napi_throw_type_error(env, NULL, "resolve(src) must return a string, a Uint8Array or undefined");
        return TA_ERR_NOT_FOUND;
    }
    uint8_t *copy = ta_arena_alloc(arena, length == 0 ? 1u : length, 1);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    memcpy(copy, data, length);
    *out = (ta_bytes){ .ptr = copy, .len = (uint32_t)length };
    return TA_OK;
}

static const ta_chart_src_resolver_vtable tna_src_resolver_vtable = { .fetch = tna_fetch_src };

ta_chart_src_resolver tna_src_resolver_adapter(tna_src_resolver *resolver) {
    if (resolver->resolve == NULL) {
        return (ta_chart_src_resolver){ 0 };
    }
    return (ta_chart_src_resolver){ .self = resolver, .v = &tna_src_resolver_vtable };
}

/* ------------------------------------------------------------------------
 * Resolvers kept per definition
 */

static napi_value tna_resolver_of(tna_napi_system *wrapper, ta_definition_id definition) {
    const tna_vec *resolvers = &wrapper->children.resolvers;
    for (uint32_t at = 0; at < resolvers->count; at += 1) {
        const tna_child_resolver *entry = tna_vec_at(resolvers, at);
        if (entry->definition == definition) {
            napi_value resolve = NULL;
            return napi_get_reference_value(wrapper->env, entry->resolve, &resolve) == napi_ok ? resolve : NULL;
        }
    }
    return NULL;
}

ta_result tna_children_keep_resolver(tna_napi_system *wrapper, ta_definition_id definition, napi_value resolve) {
    tna_child_resolver entry = { .definition = definition };
    if (napi_create_reference(wrapper->env, resolve, 1, &entry.resolve) != napi_ok) {
        return TA_ERR_NOMEM;
    }
    ta_result result = tna_vec_push(&wrapper->children.resolvers, &entry);
    if (result != TA_OK) {
        napi_delete_reference(wrapper->env, entry.resolve);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Imported children, cached by document bytes
 */

static bool tna_cached_document(tna_napi_system *wrapper, ta_bytes bytes, ta_definition_id *out) {
    const tna_vec *documents = &wrapper->children.documents;
    for (uint32_t at = 0; at < documents->count; at += 1) {
        const tna_child_document *entry = tna_vec_at(documents, at);
        if (entry->length == bytes.len && (bytes.len == 0 || memcmp(entry->bytes, bytes.ptr, bytes.len) == 0)) {
            *out = entry->definition;
            return true;
        }
    }
    return false;
}

static void tna_remember_document(tna_napi_system *wrapper, ta_bytes bytes, ta_definition_id definition) {
    ta_allocator allocator = tna_children_allocator(wrapper);
    uint8_t *copy = tna_alloc(allocator, bytes.len == 0 ? 1u : bytes.len, 1);
    if (copy == NULL) {
        return; /* not cached: the next invoke imports it again */
    }
    memcpy(copy, bytes.ptr, bytes.len);
    tna_child_document entry = { .definition = definition, .bytes = copy, .length = bytes.len };
    if (tna_vec_push(&wrapper->children.documents, &entry) != TA_OK) {
        tna_free(allocator, &(ta_allocator_free_params){ .ptr = copy, .size = bytes.len == 0 ? 1u : bytes.len,
                                                         .alignment = 1 });
    }
}

/* ------------------------------------------------------------------------
 * The invoker's question
 */

static ta_op_status tna_child_fault(tna_napi_system *wrapper, ta_fault *fault, ta_result code, const char *message) {
    size_t length = strlen(message);
    if (length >= sizeof wrapper->children.failure) {
        length = sizeof wrapper->children.failure - 1u;
    }
    if (message != wrapper->children.failure) {
        memcpy(wrapper->children.failure, message, length);
    }
    wrapper->children.failure[length] = '\0';
    *fault = (ta_fault){ .code = code, .message = { .ptr = wrapper->children.failure, .len = (uint32_t)length } };
    return TA_OP_ERROR;
}

/* A definition ID carried as a bigint or a safe integer, as builder charts do. */
static bool tna_definition_value(napi_env env, napi_value js, ta_definition_id *out) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, js, &type) != napi_ok) {
        return false;
    }
    if (type == napi_bigint) {
        uint64_t id = 0;
        bool lossless = false;
        if (napi_get_value_bigint_uint64(env, js, &id, &lossless) != napi_ok || !lossless || id == 0) {
            return false;
        }
        *out = id;
        return true;
    }
    if (type == napi_number) {
        double number = 0;
        if (napi_get_value_double(env, js, &number) != napi_ok || !(number >= 1) || number > 9007199254740991.0 ||
            number != (double)(uint64_t)number) {
            return false;
        }
        *out = (uint64_t)number;
        return true;
    }
    return false;
}

/* The parent's definition, and the resolver it was defined with (or NULL). */
static napi_value tna_parent_resolver(tna_napi_system *wrapper, ta_actor_id parent) {
    ta_actor_inspection inspection;
    if (ta_actor_inspect(wrapper->system->core, parent, &inspection) != TA_OK) {
        return NULL;
    }
    return tna_resolver_of(wrapper, inspection.definition_id);
}

/* Clears what a resolver or a compiler threw inside the invoke; true when something had. */
static bool tna_clear_exception(napi_env env, char *message, size_t capacity) {
    bool pending = false;
    napi_value error = NULL;
    if (napi_is_exception_pending(env, &pending) != napi_ok || !pending ||
        napi_get_and_clear_last_exception(env, &error) != napi_ok) {
        return false;
    }
    napi_value text = NULL;
    size_t length = 0;
    if (napi_coerce_to_string(env, error, &text) == napi_ok &&
        napi_get_value_string_utf8(env, text, message, capacity, &length) == napi_ok) {
        return true;
    }
    message[0] = '\0';
    return true;
}

static ta_op_status tna_import_child(tna_napi_system *wrapper, const tna_child_params *params, ta_bytes document,
                                     napi_value resolve) {
    if (tna_cached_document(wrapper, document, params->out)) {
        return TA_OP_OK;
    }
    napi_env env = wrapper->env;
    tna_src_resolver fetcher = { .env = env, .resolve = resolve };
    ta_datamodel choices[2u + TNA_DM_REGISTERED_MAX];
    uint32_t count = tna_js_datamodel_choices(&wrapper->dm, choices);
    tna_import_failure failure;
    ta_result result = tna_import_scxml(wrapper->system, &(tna_import_params){
                                                            .input = document,
                                                            .datamodels = choices,
                                                            .datamodel_count = count,
                                                            .out = params->out,
                                                            .failure = &failure,
                                                            .resolver = tna_src_resolver_adapter(&fetcher),
                                                        });
    char thrown[512];
    if (tna_clear_exception(env, thrown, sizeof thrown)) {
        if (result == TA_OK) {
            (void)ta_system_retire_definition(wrapper->system->core, *params->out);
        }
        return tna_child_fault(wrapper, params->fault, TA_ERR_INVALID, thrown);
    }
    if (result == TA_ERR_NOMEM) {
        return TA_OP_NOMEM;
    }
    if (result != TA_OK) {
        char *message = wrapper->children.failure;
        if (failure.located) {
            snprintf(message, sizeof wrapper->children.failure, "invoker.scxml: the child document is refused at line %u, column %u: %s",
                     failure.line, failure.column, failure.message);
        } else {
            snprintf(message, sizeof wrapper->children.failure, "invoker.scxml: the child document is refused: %s",
                     failure.message);
        }
        return tna_child_fault(wrapper, params->fault, result, message);
    }
    tna_remember_document(wrapper, document, *params->out);
    if (resolve != NULL) {
        (void)tna_children_keep_resolver(wrapper, *params->out, resolve);
    }
    return TA_OP_OK;
}

static ta_op_status tna_child_definition_of(void *context, const tna_child_params *params) {
    tna_napi_system *wrapper = context;
    napi_env env = wrapper->env;
    const ta_invoke_request *request = params->request;
    if (env == NULL || wrapper->env_data->torn_down) {
        return tna_child_fault(wrapper, params->fault, TA_ERR_INVALID, "invoker.scxml: the environment is gone");
    }
    napi_value data = NULL;
    napi_value src = NULL;
    if (request->data.v != NULL && tna_value_get(env, &request->data, &data)) {
        if (tna_definition_value(env, data, params->out)) {
            return TA_OP_OK;
        }
    }
    if (request->src.v != NULL && tna_value_get(env, &request->src, &src)) {
        if (tna_definition_value(env, src, params->out)) {
            return TA_OP_OK;
        }
    }
    napi_value resolve = tna_parent_resolver(wrapper, request->parent);
    /* SCXML 6.4: content from <content expr> is the document text. */
    napi_valuetype data_type = napi_undefined;
    if (data != NULL && napi_typeof(env, data, &data_type) == napi_ok && data_type == napi_string) {
        size_t length = 0;
        if (napi_get_value_string_utf8(env, data, NULL, 0, &length) != napi_ok || length > UINT32_MAX - 1u) {
            return tna_child_fault(wrapper, params->fault, TA_ERR_INVALID, "invoker.scxml: the content is not text");
        }
        char *text = ta_arena_alloc(&wrapper->system->scratch.call, length + 1u, 1);
        if (text == NULL) {
            return TA_OP_NOMEM;
        }
        napi_get_value_string_utf8(env, data, text, length + 1u, &length);
        return tna_import_child(wrapper, params, (ta_bytes){ .ptr = (const uint8_t *)text, .len = (uint32_t)length },
                                resolve);
    }
    if (request->content.len != 0) {
        return tna_import_child(wrapper, params, request->content, resolve);
    }
    napi_valuetype src_type = napi_undefined;
    if (src != NULL && napi_typeof(env, src, &src_type) == napi_ok && src_type == napi_string) {
        if (resolve == NULL) {
            return tna_child_fault(wrapper, params->fault, TA_ERR_NOT_FOUND,
                                   "invoker.scxml: the invoke's src needs the parent's definition to have a resolve(src)");
        }
        size_t length = 0;
        napi_get_value_string_utf8(env, src, NULL, 0, &length);
        char *text = ta_arena_alloc(&wrapper->system->scratch.call, length + 1u, 1);
        if (text == NULL) {
            return TA_OP_NOMEM;
        }
        napi_get_value_string_utf8(env, src, text, length + 1u, &length);
        tna_src_resolver fetcher = { .env = env, .resolve = resolve };
        ta_bytes document = { 0 };
        ta_result fetched = tna_fetch_src(&fetcher, (ta_str){ .ptr = text, .len = (uint32_t)length },
                                          &wrapper->system->scratch.call, &document);
        char thrown[512];
        if (tna_clear_exception(env, thrown, sizeof thrown)) {
            return tna_child_fault(wrapper, params->fault, TA_ERR_INVALID, thrown);
        }
        if (fetched == TA_ERR_NOMEM) {
            return TA_OP_NOMEM;
        }
        if (fetched != TA_OK) {
            snprintf(wrapper->children.failure, sizeof wrapper->children.failure,
                     "invoker.scxml: resolve(src) knows no document for src %.*s", (int)length, text);
            return tna_child_fault(wrapper, params->fault, TA_ERR_NOT_FOUND, wrapper->children.failure);
        }
        return tna_import_child(wrapper, params, document, resolve);
    }
    return tna_child_fault(wrapper, params->fault, TA_ERR_INVALID,
                           "invoker.scxml: the invoke names no child: no definition, no <content> and no src");
}

/*
 * The child's binding (SCXML 6.4): of the invoke's <param> and namelist
 * values, only those whose names the child declares as top-level <data>.
 * A name it does not declare is not added.
 */
static ta_op_status tna_child_binding(tna_napi_system *wrapper, const tna_child_params *params) {
    const ta_invoke_request *request = params->request;
    napi_env env = wrapper->env;
    napi_value given = NULL;
    napi_valuetype type = napi_undefined;
    if (request->params.v == NULL || !tna_value_get(env, &request->params, &given) ||
        napi_typeof(env, given, &type) != napi_ok || type != napi_object) {
        return TA_OP_OK;
    }
    ta_arena arena;
    if (ta_arena_init(&arena, tna_children_allocator(wrapper), 4096) != TA_OK) {
        return TA_OP_NOMEM;
    }
    const ta_scxml_spec *spec = NULL;
    ta_result result = ta_definition_export(wrapper->system->core, &(ta_definition_export_params){
                                                                       .definition = *params->out,
                                                                       .arena = &arena,
                                                                       .out = &spec,
                                                                   });
    napi_value bound = NULL;
    bool made = result == TA_OK && napi_create_object(env, &bound) == napi_ok;
    for (const ta_scxml_data_spec *data = made ? spec->data : NULL; made && data != NULL && data->name.len != 0;
         data += 1) {
        napi_value key = NULL;
        napi_value value = NULL;
        bool has = false;
        made = napi_create_string_utf8(env, data->name.ptr, data->name.len, &key) == napi_ok &&
               napi_has_own_property(env, given, key, &has) == napi_ok;
        if (made && has) {
            made = napi_get_property(env, given, key, &value) == napi_ok &&
                   napi_set_property(env, bound, key, value) == napi_ok;
        }
    }
    ta_arena_destroy(&arena);
    if (result == TA_ERR_NOMEM || (made && !tna_value_make(&wrapper->values, bound, params->binding))) {
        return TA_OP_NOMEM;
    }
    if (!made) {
        return tna_child_fault(wrapper, params->fault, TA_ERR_INVALID, "invoker.scxml: could not bind the child's params");
    }
    return TA_OP_OK;
}

static ta_op_status tna_child_definition(void *context, const tna_child_params *params) {
    ta_op_status status = tna_child_definition_of(context, params);
    return status == TA_OP_OK ? tna_child_binding(context, params) : status;
}

void tna_children_init(tna_napi_system *wrapper) {
    ta_allocator allocator = tna_children_allocator(wrapper);
    tna_vec_init(&wrapper->children.resolvers, allocator, TNA_LAYOUT(tna_child_resolver));
    tna_vec_init(&wrapper->children.documents, allocator, TNA_LAYOUT(tna_child_document));
    wrapper->children.failure[0] = '\0';
    wrapper->system->child_definition = tna_child_definition;
    wrapper->system->child_context = wrapper;
}

void tna_children_close(tna_napi_system *wrapper, bool env_usable) {
    tna_vec *resolvers = &wrapper->children.resolvers;
    for (uint32_t at = 0; env_usable && at < resolvers->count; at += 1) {
        const tna_child_resolver *entry = tna_vec_at(resolvers, at);
        napi_delete_reference(wrapper->env, entry->resolve);
    }
    tna_vec_destroy(resolvers);
    ta_allocator allocator = tna_children_allocator(wrapper);
    tna_vec *documents = &wrapper->children.documents;
    for (uint32_t at = 0; at < documents->count; at += 1) {
        const tna_child_document *entry = tna_vec_at(documents, at);
        tna_free(allocator, &(ta_allocator_free_params){ .ptr = entry->bytes, .size = entry->length == 0 ? 1u : entry->length,
                                                         .alignment = 1 });
    }
    tna_vec_destroy(documents);
    wrapper->system->child_definition = NULL;
}
