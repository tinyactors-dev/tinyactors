#include "napi_datamodel.h"

#include <stdlib.h>
#include <string.h>

#include "napi_system.h"
#include "values.h"

/* ------------------------------------------------------------------------
 * Expression records (TA_EXPRESSION_HOST values)
 */

typedef struct {
    tna_values *values; /* env, allocator, torn_down */
    napi_ref ref;
    uint32_t references;
    tna_expr_kind kind;
} tna_expr_record;

static ta_result tna_expr_retain(void *self, void *value, ta_allocator allocator, void **out) {
    (void)self;
    (void)allocator;
    ((tna_expr_record *)value)->references += 1u;
    *out = value;
    return TA_OK;
}

static void tna_expr_release(void *self, void *value, ta_allocator allocator) {
    (void)self;
    (void)allocator;
    tna_expr_record *record = value;
    if (--record->references != 0) {
        return;
    }
    tna_values *values = record->values;
    if (record->ref != NULL && !*values->torn_down) {
        napi_delete_reference(values->env, record->ref);
    }
    tna_free(values->allocator,
             &(ta_allocator_free_params){ .ptr = record, .size = sizeof *record, .alignment = _Alignof(tna_expr_record) });
}

static const ta_expression_host_vtable tna_expr_vtable = { tna_expr_retain, tna_expr_release };

static const tna_expr_record *tna_record_of(const ta_expression *expression) {
    if (expression == NULL || expression->kind != TA_EXPRESSION_HOST || expression->as.host.v != &tna_expr_vtable) {
        return NULL;
    }
    return expression->as.host.value;
}

bool tna_expression_make(tna_js_datamodel *dm, napi_value value, tna_expr_kind kind, ta_expression *out) {
    tna_values *values = &dm->wrapper->values;
    tna_expr_record *record = tna_alloc(values->allocator, sizeof *record, _Alignof(tna_expr_record));
    if (record == NULL) {
        return false;
    }
    *record = (tna_expr_record){ .values = values, .references = 1u, .kind = kind };
    if (napi_create_reference(dm->env, value, 1, &record->ref) != napi_ok) {
        tna_free(values->allocator,
                 &(ta_allocator_free_params){ .ptr = record, .size = sizeof *record, .alignment = _Alignof(tna_expr_record) });
        return false;
    }
    *out = (ta_expression){ .kind = TA_EXPRESSION_HOST,
                            .as.host = { .self = NULL, .v = &tna_expr_vtable, .value = record } };
    return true;
}

void tna_expression_release(ta_expression *expression) {
    if (tna_record_of(expression) != NULL) {
        tna_expr_release(NULL, expression->as.host.value, (ta_allocator){ 0 });
    }
    *expression = (ta_expression){ .kind = TA_EXPRESSION_NONE };
}

napi_value tna_expression_value(napi_env env, const ta_expression *expression) {
    const tna_expr_record *record = tna_record_of(expression);
    napi_value value = NULL;
    if (record == NULL || napi_get_reference_value(env, record->ref, &value) != napi_ok) {
        return NULL;
    }
    return value;
}

bool tna_expression_is_literal(napi_env env, napi_value value) {
    tna_env *data = tna_env_get(env);
    napi_valuetype type = napi_undefined;
    napi_value brand = NULL;
    bool has = false;
    if (data == NULL || data->literal_brand == NULL || napi_typeof(env, value, &type) != napi_ok ||
        type != napi_object || napi_get_reference_value(env, data->literal_brand, &brand) != napi_ok ||
        napi_has_property(env, value, brand, &has) != napi_ok) {
        return false;
    }
    return has;
}

/* ------------------------------------------------------------------------
 * Source text: `In(state)` and data names only.
 */

typedef enum { TNA_SOURCE_OTHER = 0, TNA_SOURCE_IN, TNA_SOURCE_NAME } tna_source_kind;

static bool tna_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static ta_str tna_trim(ta_str text) {
    while (text.len > 0 && tna_space(text.ptr[0])) {
        text.ptr += 1;
        text.len -= 1u;
    }
    while (text.len > 0 && tna_space(text.ptr[text.len - 1u])) {
        text.len -= 1u;
    }
    return text;
}

static bool tna_identifier(ta_str text) {
    if (text.len == 0) {
        return false;
    }
    for (uint32_t at = 0; at < text.len; at += 1) {
        char c = text.ptr[at];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
        if (!letter && !(at > 0 && c >= '0' && c <= '9')) {
            return false;
        }
    }
    return true;
}

/* Classifies source text; for In(state), `argument` is the state ID. */
static tna_source_kind tna_source(ta_str source, ta_str *argument) {
    ta_str text = tna_trim(source);
    if (tna_identifier(text)) {
        *argument = text;
        return TNA_SOURCE_NAME;
    }
    if (text.len < 4 || memcmp(text.ptr, "In(", 3) != 0 || text.ptr[text.len - 1u] != ')') {
        return TNA_SOURCE_OTHER;
    }
    ta_str inner = tna_trim((ta_str){ text.ptr + 3, text.len - 4u });
    if (inner.len >= 2 && (inner.ptr[0] == '\'' || inner.ptr[0] == '"') && inner.ptr[inner.len - 1u] == inner.ptr[0]) {
        inner = (ta_str){ inner.ptr + 1, inner.len - 2u };
    }
    if (inner.len == 0) {
        return TNA_SOURCE_OTHER;
    }
    *argument = inner;
    return TNA_SOURCE_IN;
}

/* ------------------------------------------------------------------------
 * Definitions and instances
 */

typedef struct {
    tna_js_datamodel *dm;
    const ta_expression *expressions;
    uint32_t count;
} tna_dm_shared;

typedef struct {
    ta_value root; /* the data object, or absent before init */
} tna_dm_instance;

static const char tna_source_refused[] =
    "the JavaScript data model takes functions; source text is only In(state) or a data name";

static ta_result tna_dm_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    tna_js_datamodel *dm = self;
    for (uint32_t index = 0; index < params->expression_count; index += 1) {
        const ta_expression *expression = &params->expressions[index];
        ta_str argument = { NULL, 0 };
        bool refused = false;
        if (expression->kind == TA_EXPRESSION_SOURCE) {
            refused = tna_source(expression->as.source, &argument) == TNA_SOURCE_OTHER;
        } else if (expression->kind == TA_EXPRESSION_HOST) {
            refused = tna_record_of(expression) == NULL;
        }
        if (refused) {
            if (params->rejected != NULL) {
                *params->rejected = (ta_expression_rejection){
                    .expression = index,
                    .message = { tna_source_refused, (uint32_t)(sizeof tna_source_refused - 1u) },
                };
            }
            return expression->kind == TA_EXPRESSION_HOST ? TA_ERR_UNSUPPORTED : TA_ERR_INVALID;
        }
    }
    tna_dm_shared *shared = tna_alloc(params->allocator, sizeof *shared, _Alignof(tna_dm_shared));
    if (shared == NULL) {
        return TA_ERR_NOMEM;
    }
    *shared = (tna_dm_shared){ .dm = dm, .expressions = params->expressions, .count = params->expression_count };
    *params->shared = shared;
    return TA_OK;
}

static void tna_dm_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self;
    if (shared != NULL) {
        tna_free(allocator,
                 &(ta_allocator_free_params){ .ptr = shared, .size = sizeof(tna_dm_shared), .alignment = _Alignof(tna_dm_shared) });
    }
}

static uint32_t tna_dm_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return (uint32_t)sizeof(tna_dm_instance);
}

static uint32_t tna_dm_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return (uint32_t)_Alignof(tna_dm_instance);
}

/* ------------------------------------------------------------------------
 * One callback: a handle scope, the call in progress, and faults.
 */

typedef struct {
    tna_js_datamodel *dm;
    tna_dm_instance *instance;
    ta_fault *fault;
    napi_handle_scope scope;
    const ta_dm_host *saved_host;
    void *saved_instance;
    napi_ref saved_event;
} tna_frame;

static void tna_fault(tna_frame *frame, const char *message) {
    if (frame->fault == NULL) {
        return;
    }
    *frame->fault = (ta_fault){ .code = TA_ERR_INVALID, .message = { message, (uint32_t)strlen(message) } };
    napi_env env = frame->dm->env;
    bool pending = false;
    napi_value error = NULL;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending &&
        napi_get_and_clear_last_exception(env, &error) == napi_ok && error != NULL) {
        ta_value data;
        if (tna_value_make(&frame->dm->wrapper->values, error, &data)) {
            frame->fault->data = data;
        }
    }
}

static ta_op_status tna_fail(tna_frame *frame, const char *message) {
    tna_fault(frame, message);
    return TA_OP_ERROR;
}

static bool tna_begin(tna_frame *frame, tna_js_datamodel *dm, void *instance, const ta_dm_host *host, ta_fault *fault) {
    *frame = (tna_frame){ .dm = dm, .instance = instance, .fault = fault };
    if (!dm->js_usable) {
        tna_fault(frame, "javascript: no JavaScript can run while the system closes");
        return false;
    }
    if (napi_open_handle_scope(dm->env, &frame->scope) != napi_ok) {
        tna_fault(frame, "javascript: cannot open a handle scope");
        return false;
    }
    frame->saved_host = dm->host;
    frame->saved_instance = dm->instance;
    frame->saved_event = dm->event;
    dm->host = host;
    dm->instance = instance;
    dm->event = NULL;
    return true;
}

static void tna_end(tna_frame *frame) {
    tna_js_datamodel *dm = frame->dm;
    if (dm->event != NULL) {
        napi_delete_reference(dm->env, dm->event);
    }
    dm->host = frame->saved_host;
    dm->instance = frame->saved_instance;
    dm->event = frame->saved_event;
    napi_close_handle_scope(dm->env, frame->scope);
}

static bool tna_root(tna_frame *frame, napi_value *out) {
    return frame->instance != NULL && tna_value_get(frame->dm->env, &frame->instance->root, out);
}

static bool tna_context(tna_frame *frame, napi_value *out) {
    return napi_get_reference_value(frame->dm->env, frame->dm->context, out) == napi_ok && *out != NULL;
}

/* Calls `function` with `this` = the root and (args..., context). */
static bool tna_invoke(tna_frame *frame, napi_value function, size_t argc, const napi_value *args, napi_value *result) {
    napi_value argv[3];
    napi_value root = NULL;
    if (argc > 2 || !tna_root(frame, &root) || !tna_context(frame, &argv[argc])) {
        return false;
    }
    for (size_t at = 0; at < argc; at += 1) {
        argv[at] = args[at];
    }
    napi_value ignored = NULL;
    return napi_call_function(frame->dm->env, root, function, argc + 1u, argv, result == NULL ? &ignored : result) ==
           napi_ok;
}

static const ta_expression *tna_expression(const tna_dm_shared *shared, ta_expr_id id) {
    return shared == NULL || id >= shared->count ? NULL : &shared->expressions[id];
}

static bool tna_named_property(tna_frame *frame, ta_str name, napi_value *key) {
    return napi_create_string_utf8(frame->dm->env, name.ptr, name.len, key) == napi_ok;
}

/* root[name] */
static bool tna_read_name(tna_frame *frame, ta_str name, napi_value *out) {
    napi_value root = NULL;
    napi_value key = NULL;
    return tna_root(frame, &root) && tna_named_property(frame, name, &key) &&
           napi_get_property(frame->dm->env, root, key, out) == napi_ok;
}

/* In(state): false for a state the chart does not have (SCXML B.1). */
static bool tna_in_state(const ta_dm_host *host, ta_str id) {
    ta_state_id state = 0;
    return ta_dm_host_find_state(host, id, &state) == TA_OK && ta_dm_host_in_state(host, state);
}

/*
 * Evaluates expression `id` as a value. A function is called; a literal is
 * its value; a location is read; a data name reads the root; In(state) is a
 * boolean.
 */
static bool tna_evaluate(tna_frame *frame, const tna_dm_shared *shared, ta_expr_id id, napi_value *out) {
    napi_env env = frame->dm->env;
    const ta_expression *expression = tna_expression(shared, id);
    if (expression == NULL) {
        return false;
    }
    if (expression->kind == TA_EXPRESSION_SOURCE) {
        ta_str argument = { NULL, 0 };
        switch (tna_source(expression->as.source, &argument)) {
        case TNA_SOURCE_NAME:
            return tna_read_name(frame, argument, out);
        case TNA_SOURCE_IN:
            return napi_get_boolean(env, tna_in_state(frame->dm->host, argument), out) == napi_ok;
        default:
            return false;
        }
    }
    const tna_expr_record *record = tna_record_of(expression);
    napi_value value = tna_expression_value(env, expression);
    if (record == NULL || value == NULL) {
        return false;
    }
    switch (record->kind) {
    case TNA_EXPR_FUNCTION:
        return tna_invoke(frame, value, 0, NULL, out);
    case TNA_EXPR_LITERAL:
        return napi_get_named_property(env, value, "value", out) == napi_ok;
    case TNA_EXPR_LOCATION: {
        napi_value get = NULL;
        return napi_get_named_property(env, value, "get", &get) == napi_ok && tna_invoke(frame, get, 0, NULL, out);
    }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * The vtable
 */

static ta_result tna_dm_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    tna_js_datamodel *dm = self;
    tna_dm_instance *instance = params->instance;
    *instance = (tna_dm_instance){ .root = { NULL, NULL } };
    if (params->binding != NULL && params->binding->v != NULL) {
        instance->root = *params->binding;
        ta_value_retain(&instance->root);
        return TA_OK;
    }
    if (!dm->js_usable) {
        return TA_ERR_UNSUPPORTED;
    }
    napi_handle_scope scope = NULL;
    napi_value root = NULL;
    if (napi_open_handle_scope(dm->env, &scope) != napi_ok) {
        return TA_ERR_NOMEM;
    }
    bool made = napi_create_object(dm->env, &root) == napi_ok && tna_value_make(&dm->wrapper->values, root, &instance->root);
    napi_close_handle_scope(dm->env, scope);
    return made ? TA_OK : TA_ERR_NOMEM;
}

static void tna_dm_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    tna_dm_instance *data = instance;
    ta_value_release(&data->root);
    data->root = (ta_value){ NULL, NULL };
}

static ta_op_status tna_dm_declare(void *self, const ta_datamodel_declare_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    napi_value root = NULL;
    napi_value key = NULL;
    napi_value undefined = NULL;
    bool has = false;
    ta_op_status status = TA_OP_OK;
    if (!tna_root(&frame, &root) || !tna_named_property(&frame, params->name, &key) ||
        napi_has_own_property(env, root, key, &has) != napi_ok) {
        status = tna_fail(&frame, "javascript: could not declare data");
    } else if (has) {
        if (params->bound != NULL) {
            *params->bound = true;
        }
    } else if (napi_get_undefined(env, &undefined) != napi_ok || napi_set_property(env, root, key, undefined) != napi_ok) {
        status = tna_fail(&frame, "javascript: could not declare data");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_set_named(void *self, const ta_datamodel_set_named_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    napi_value root = NULL;
    napi_value key = NULL;
    napi_value value = NULL;
    ta_op_status status = TA_OP_OK;
    if (!tna_value_get(env, params->value, &value)) {
        status = tna_fail(&frame, "javascript: data value is not a JavaScript value");
    } else if (!tna_root(&frame, &root) || !tna_named_property(&frame, params->name, &key) ||
               napi_set_property(env, root, key, value) != napi_ok) {
        status = tna_fail(&frame, "javascript: could not set data");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_eval_condition(void *self, const ta_datamodel_eval_condition_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_value value = NULL;
    napi_value flag = NULL;
    ta_op_status status = TA_OP_OK;
    if (!tna_evaluate(&frame, params->shared, params->expr, &value) ||
        napi_coerce_to_bool(frame.dm->env, value, &flag) != napi_ok ||
        napi_get_value_bool(frame.dm->env, flag, params->out) != napi_ok) {
        status = tna_fail(&frame, "javascript: condition threw");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_value value = NULL;
    ta_op_status status = TA_OP_OK;
    if (!tna_evaluate(&frame, params->shared, params->expr, &value)) {
        status = tna_fail(&frame, "javascript: expression threw");
    } else if (!tna_value_make(&frame.dm->wrapper->values, value, params->out)) {
        status = tna_fail(&frame, "javascript: cannot keep the value");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_read_location(void *self, const ta_datamodel_read_location_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    const ta_expression *expression = tna_expression(params->shared, params->location);
    const tna_expr_record *record = tna_record_of(expression);
    ta_str name = { NULL, 0 };
    napi_value value = NULL;
    ta_op_status status = TA_OP_OK;
    bool location = record != NULL ? record->kind == TNA_EXPR_LOCATION
                                   : expression != NULL && expression->kind == TA_EXPRESSION_SOURCE &&
                                         tna_source(expression->as.source, &name) == TNA_SOURCE_NAME;
    if (!location) {
        status = tna_fail(&frame, "javascript: not a location");
    } else if (!tna_evaluate(&frame, params->shared, params->location, &value)) {
        status = tna_fail(&frame, "javascript: location threw");
    } else if (!tna_value_make(&frame.dm->wrapper->values, value, params->out)) {
        status = tna_fail(&frame, "javascript: cannot keep the value");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_assign(void *self, const ta_datamodel_assign_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    const ta_expression *expression = tna_expression(params->shared, params->location);
    const tna_expr_record *record = tna_record_of(expression);
    napi_value value = NULL;
    ta_op_status status = TA_OP_OK;
    if (!tna_value_get(env, params->value, &value)) {
        status = tna_fail(&frame, "javascript: assigned value is not a JavaScript value");
    } else if (record != NULL && record->kind == TNA_EXPR_LOCATION) {
        napi_value location = tna_expression_value(env, expression);
        napi_value set = NULL;
        if (location == NULL || napi_get_named_property(env, location, "set", &set) != napi_ok ||
            !tna_invoke(&frame, set, 1, &value, NULL)) {
            status = tna_fail(&frame, "javascript: assign threw");
        }
    } else {
        ta_str name = { NULL, 0 };
        napi_value root = NULL;
        napi_value key = NULL;
        bool has = false;
        if (record != NULL || expression == NULL || expression->kind != TA_EXPRESSION_SOURCE ||
            tna_source(expression->as.source, &name) != TNA_SOURCE_NAME) {
            status = tna_fail(&frame, "javascript: not a location");
        } else if (!tna_root(&frame, &root) || !tna_named_property(&frame, name, &key) ||
                   napi_has_own_property(env, root, key, &has) != napi_ok) {
            status = tna_fail(&frame, "javascript: invalid location");
        } else if (!has) {
            status = tna_fail(&frame, "javascript: assignment to an undeclared data name");
        } else if (napi_set_property(env, root, key, value) != napi_ok) {
            status = tna_fail(&frame, "javascript: assign threw");
        }
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    const ta_expression *expression = tna_expression(params->shared, params->script);
    const tna_expr_record *record = tna_record_of(expression);
    napi_value function = tna_expression_value(frame.dm->env, expression);
    ta_op_status status = TA_OP_OK;
    if (record == NULL || record->kind != TNA_EXPR_FUNCTION || function == NULL) {
        status = tna_fail(&frame, "javascript: a script must be a function");
    } else if (!tna_invoke(&frame, function, 0, NULL, NULL)) {
        status = tna_fail(&frame, "javascript: script threw");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_make_params(void *self, const ta_datamodel_make_params_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    napi_value object = NULL;
    ta_op_status status = TA_OP_OK;
    if (napi_create_object(env, &object) != napi_ok) {
        status = tna_fail(&frame, "javascript: could not make params");
    }
    for (uint32_t at = 0; status == TA_OP_OK && at < params->count; at += 1) {
        napi_value key = NULL;
        napi_value value = NULL;
        if (!tna_value_get(env, &params->values[at].value, &value)) {
            status = tna_fail(&frame, "javascript: parameter is not a JavaScript value");
        } else if (!tna_named_property(&frame, params->values[at].name, &key) ||
                   napi_set_property(env, object, key, value) != napi_ok) {
            status = tna_fail(&frame, "javascript: could not make params");
        }
    }
    if (status == TA_OP_OK && !tna_value_make(&frame.dm->wrapper->values, object, params->out)) {
        status = tna_fail(&frame, "javascript: cannot keep the params");
    }
    tna_end(&frame);
    return status;
}

static ta_result tna_dm_inspect(void *self, const ta_datamodel_inspect_params *params) {
    (void)self;
    const tna_dm_instance *instance = params->instance;
    *params->out = instance->root;
    ta_value_retain(params->out);
    return TA_OK;
}

static ta_op_status tna_dm_value_text(void *self, const ta_datamodel_value_text_params *params) {
    tna_js_datamodel *dm = self;
    if (!tna_value_ours(params->value)) {
        *params->fault = (ta_fault){ .code = TA_ERR_UNSUPPORTED,
                                     .message = { "javascript: not a JavaScript value", 34 } };
        return TA_OP_ERROR;
    }
    tna_frame frame;
    if (!tna_begin(&frame, dm, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    ta_op_status status = tna_value_text(&dm->wrapper->values, params->value, params->out)
                              ? TA_OP_OK
                              : tna_fail(&frame, "javascript: value has no text");
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_text_value(void *self, const ta_datamodel_text_value_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_value text = NULL;
    ta_op_status status = TA_OP_OK;
    if (napi_create_string_utf8(frame.dm->env, params->text.ptr, params->text.len, &text) != napi_ok ||
        !tna_value_make(&frame.dm->wrapper->values, text, params->out)) {
        status = tna_fail(&frame, "javascript: could not make text");
    }
    tna_end(&frame);
    return status;
}

/* Literal content: JSON when it parses, otherwise the text (a string). */
static ta_op_status tna_dm_import_content(void *self, const ta_datamodel_import_content_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    napi_value text = NULL;
    napi_value parse = NULL;
    napi_value global = NULL;
    napi_value value = NULL;
    ta_op_status status = TA_OP_OK;
    if (napi_create_string_utf8(env, (const char *)params->content.ptr, params->content.len, &text) != napi_ok) {
        status = tna_fail(&frame, "javascript: content is not text");
    } else {
        if (napi_get_reference_value(env, frame.dm->json_parse, &parse) != napi_ok ||
            napi_get_global(env, &global) != napi_ok ||
            napi_call_function(env, global, parse, 1, &text, &value) != napi_ok) {
            napi_value ignored = NULL;
            bool pending = false;
            if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
                napi_get_and_clear_last_exception(env, &ignored);
            }
            value = text;
        }
        if (!tna_value_make(&frame.dm->wrapper->values, value, params->out)) {
            status = tna_fail(&frame, "javascript: could not import content");
        }
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_foreach_snapshot(void *self, const ta_datamodel_foreach_snapshot_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    napi_value array = NULL;
    napi_value from = NULL;
    napi_value global = NULL;
    napi_value snapshot = NULL;
    uint32_t length = 0;
    bool is_array = false;
    ta_op_status status = TA_OP_OK;
    if (!tna_evaluate(&frame, params->shared, params->array, &array)) {
        status = tna_fail(&frame, "javascript: foreach array threw");
    } else if (napi_is_array(env, array, &is_array) != napi_ok || !is_array) {
        status = tna_fail(&frame, "javascript: foreach array is not an array");
    } else if (napi_get_reference_value(env, frame.dm->array_from, &from) != napi_ok ||
               napi_get_global(env, &global) != napi_ok ||
               napi_call_function(env, global, from, 1, &array, &snapshot) != napi_ok ||
               napi_get_array_length(env, snapshot, &length) != napi_ok) {
        status = tna_fail(&frame, "javascript: cannot copy the foreach array");
    } else if (!tna_value_make(&frame.dm->wrapper->values, snapshot, params->out)) {
        status = tna_fail(&frame, "javascript: cannot keep the foreach array");
    } else {
        *params->count = length;
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, self, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    napi_value snapshot = NULL;
    napi_value item = NULL;
    napi_value root = NULL;
    napi_value key = NULL;
    ta_op_status status = TA_OP_OK;
    if (!tna_value_get(env, params->snapshot, &snapshot) || napi_get_element(env, snapshot, params->index, &item) != napi_ok ||
        !tna_root(&frame, &root) || !tna_named_property(&frame, params->item, &key) ||
        napi_set_property(env, root, key, item) != napi_ok) {
        status = tna_fail(&frame, "javascript: cannot bind the foreach item");
    } else if (params->index_name.ptr != NULL) {
        napi_value index = NULL;
        if (!tna_named_property(&frame, params->index_name, &key) ||
            napi_create_uint32(env, params->index, &index) != napi_ok || napi_set_property(env, root, key, index) != napi_ok) {
            status = tna_fail(&frame, "javascript: cannot bind the foreach index");
        }
    }
    tna_end(&frame);
    return status;
}

static const ta_datamodel_vtable tna_dm_vtable = {
    .name = { "javascript", 10 },
    .prepare_definition = tna_dm_prepare,
    .destroy_definition = tna_dm_destroy_definition,
    .instance_size = tna_dm_instance_size,
    .instance_alignment = tna_dm_instance_alignment,
    .init_instance = tna_dm_init_instance,
    .destroy_instance = tna_dm_destroy_instance,
    .declare = tna_dm_declare,
    .set_named = tna_dm_set_named,
    .eval_condition = tna_dm_eval_condition,
    .eval_value = tna_dm_eval_value,
    .read_location = tna_dm_read_location,
    .assign = tna_dm_assign,
    .execute_script = tna_dm_execute_script,
    .make_params = tna_dm_make_params,
    .inspect = tna_dm_inspect,
    .value_text = tna_dm_value_text,
    .text_value = tna_dm_text_value,
    .import_content = tna_dm_import_content,
    .foreach_snapshot = tna_dm_foreach_snapshot,
    .foreach_bind = tna_dm_foreach_bind,
};

ta_datamodel tna_js_datamodel_adapter(tna_js_datamodel *dm) {
    return (ta_datamodel){ .self = dm, .v = &tna_dm_vtable };
}

/* ------------------------------------------------------------------------
 * The evaluation context
 */

static tna_js_datamodel *tna_context_dm(napi_env env, napi_callback_info info, size_t *argc, napi_value *argv) {
    void *data = NULL;
    if (napi_get_cb_info(env, info, argc, argv, NULL, &data) != napi_ok || data == NULL) {
        return NULL;
    }
    tna_js_datamodel *dm = data;
    if (dm->host == NULL) {
        tna_throw(env, "invalid", "the evaluation context is valid only while its expression runs");
        return NULL;
    }
    return dm;
}

/* System-variable text (core's formatting), or undefined when absent. */
static napi_value tna_host_text(napi_env env, const ta_dm_host *host, ta_dm_text_kind kind) {
    char stack[256];
    uint32_t needed = 0;
    napi_value out = NULL;
    ta_result result = ta_dm_host_text(host, &(ta_dm_text_params){ .kind = kind, .buffer = stack,
                                                                   .capacity = sizeof stack, .needed = &needed });
    if (result == TA_ERR_NOMEM && needed > sizeof stack) {
        char *heap = malloc(needed);
        if (heap != NULL &&
            ta_dm_host_text(host, &(ta_dm_text_params){ .kind = kind, .buffer = heap, .capacity = needed,
                                                        .needed = &needed }) == TA_OK) {
            napi_create_string_utf8(env, heap, needed - 1u, &out);
        }
        free(heap);
    } else if (result == TA_OK) {
        napi_create_string_utf8(env, stack, needed == 0 ? 0 : needed - 1u, &out);
    }
    if (out == NULL) {
        napi_get_undefined(env, &out);
    }
    return out;
}

static napi_value tna_get_data(napi_env env, napi_callback_info info) {
    tna_js_datamodel *dm = tna_context_dm(env, info, NULL, NULL);
    napi_value root = NULL;
    if (dm == NULL || dm->instance == NULL || !tna_value_get(env, &((tna_dm_instance *)dm->instance)->root, &root)) {
        return NULL;
    }
    return root;
}

static const char *const tna_event_types[] = { "external", "internal", "platform" };

static napi_status tna_set_text(napi_env env, napi_value object, const char *name, napi_value text) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, text, &type) != napi_ok || type == napi_undefined) {
        return napi_ok;
    }
    return napi_set_named_property(env, object, name, text);
}

/*
 * _event: built once per callback, on first use, and held by a reference
 * until the callback ends (each getter call has its own handle scope).
 */
static napi_value tna_get_event(napi_env env, napi_callback_info info) {
    tna_js_datamodel *dm = tna_context_dm(env, info, NULL, NULL);
    if (dm == NULL) {
        return NULL;
    }
    napi_value cached = NULL;
    if (dm->event != NULL && napi_get_reference_value(env, dm->event, &cached) == napi_ok && cached != NULL) {
        return cached;
    }
    const ta_event *event = ta_dm_host_event(dm->host);
    napi_value object = NULL;
    napi_value type = NULL;
    napi_value data = NULL;
    if (event == NULL) {
        napi_get_undefined(env, &object);
        return object;
    }
    if (napi_create_object(env, &object) != napi_ok ||
        napi_set_named_property(env, object, "name", tna_host_text(env, dm->host, TA_DM_TEXT_EVENT_NAME)) != napi_ok ||
        napi_create_string_utf8(env, tna_event_types[event->type <= TA_EVENT_PLATFORM ? event->type : 0], NAPI_AUTO_LENGTH,
                                &type) != napi_ok ||
        napi_set_named_property(env, object, "type", type) != napi_ok ||
        tna_set_text(env, object, "sendId", tna_host_text(env, dm->host, TA_DM_TEXT_EVENT_SENDID)) != napi_ok ||
        tna_set_text(env, object, "origin", tna_host_text(env, dm->host, TA_DM_TEXT_EVENT_ORIGIN)) != napi_ok ||
        tna_set_text(env, object, "originType", tna_host_text(env, dm->host, TA_DM_TEXT_EVENT_ORIGINTYPE)) != napi_ok ||
        tna_set_text(env, object, "invocationId", tna_host_text(env, dm->host, TA_DM_TEXT_EVENT_INVOKEID)) != napi_ok ||
        !tna_value_get(env, &event->data, &data) || napi_set_named_property(env, object, "data", data) != napi_ok) {
        return NULL;
    }
    napi_create_reference(env, object, 1, &dm->event);
    return object;
}

static napi_value tna_get_session_id(napi_env env, napi_callback_info info) {
    tna_js_datamodel *dm = tna_context_dm(env, info, NULL, NULL);
    return dm == NULL ? NULL : tna_host_text(env, dm->host, TA_DM_TEXT_SESSION_ID);
}

static napi_value tna_get_name(napi_env env, napi_callback_info info) {
    tna_js_datamodel *dm = tna_context_dm(env, info, NULL, NULL);
    return dm == NULL ? NULL : tna_host_text(env, dm->host, TA_DM_TEXT_SESSION_NAME);
}

/* _ioprocessors: the SCXML Event I/O Processor, by its type URI and "scxml". */
static napi_value tna_get_io_processors(napi_env env, napi_callback_info info) {
    tna_js_datamodel *dm = tna_context_dm(env, info, NULL, NULL);
    if (dm == NULL) {
        return NULL;
    }
    napi_value processors = NULL;
    napi_value entry = NULL;
    napi_value type = tna_host_text(env, dm->host, TA_DM_TEXT_SCXML_IO_TYPE);
    size_t length = 0;
    char key[256];
    if (napi_create_object(env, &processors) != napi_ok || napi_create_object(env, &entry) != napi_ok ||
        napi_set_named_property(env, entry, "location", tna_host_text(env, dm->host, TA_DM_TEXT_SCXML_IO_LOCATION)) !=
            napi_ok ||
        napi_set_named_property(env, processors, "scxml", entry) != napi_ok) {
        return NULL;
    }
    if (napi_get_value_string_utf8(env, type, key, sizeof key, &length) == napi_ok && length > 0) {
        napi_set_named_property(env, processors, key, entry);
    }
    return processors;
}

static napi_value tna_in_state_method(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    tna_js_datamodel *dm = tna_context_dm(env, info, &argc, argv);
    if (dm == NULL) {
        return NULL;
    }
    char stack[256];
    size_t length = 0;
    napi_valuetype type = napi_undefined;
    if (argc < 1 || napi_typeof(env, argv[0], &type) != napi_ok || type != napi_string ||
        napi_get_value_string_utf8(env, argv[0], stack, sizeof stack, &length) != napi_ok) {
        return tna_throw(env, "invalid", "inState(): expected a state ID");
    }
    napi_value result = NULL;
    napi_get_boolean(env, tna_in_state(dm->host, (ta_str){ stack, (uint32_t)length }), &result);
    return result;
}

static bool tna_global_function(napi_env env, const char *object_name, const char *function_name, napi_ref *out) {
    napi_value global = NULL;
    napi_value object = NULL;
    napi_value function = NULL;
    return napi_get_global(env, &global) == napi_ok &&
           napi_get_named_property(env, global, object_name, &object) == napi_ok &&
           napi_get_named_property(env, object, function_name, &function) == napi_ok &&
           napi_create_reference(env, function, 1, out) == napi_ok;
}

bool tna_js_datamodel_init(tna_js_datamodel *dm, tna_napi_system *wrapper, napi_env env) {
    *dm = (tna_js_datamodel){ .wrapper = wrapper, .env = env, .js_usable = true };
    napi_value context = NULL;
    napi_property_descriptor properties[] = {
        { "data", NULL, NULL, tna_get_data, NULL, NULL, napi_enumerable, dm },
        { "event", NULL, NULL, tna_get_event, NULL, NULL, napi_enumerable, dm },
        { "sessionId", NULL, NULL, tna_get_session_id, NULL, NULL, napi_enumerable, dm },
        { "name", NULL, NULL, tna_get_name, NULL, NULL, napi_enumerable, dm },
        { "ioProcessors", NULL, NULL, tna_get_io_processors, NULL, NULL, napi_enumerable, dm },
        { "inState", NULL, tna_in_state_method, NULL, NULL, NULL, napi_enumerable, dm },
    };
    return napi_create_object(env, &context) == napi_ok &&
           napi_define_properties(env, context, sizeof properties / sizeof properties[0], properties) == napi_ok &&
           napi_object_freeze(env, context) == napi_ok && napi_create_reference(env, context, 1, &dm->context) == napi_ok &&
           tna_global_function(env, "JSON", "parse", &dm->json_parse) &&
           tna_global_function(env, "Array", "from", &dm->array_from);
}

void tna_js_datamodel_close(tna_js_datamodel *dm, bool refs_usable) {
    napi_ref *refs[] = { &dm->context, &dm->json_parse, &dm->array_from };
    for (size_t at = 0; at < sizeof refs / sizeof refs[0]; at += 1) {
        if (*refs[at] != NULL && refs_usable) {
            napi_delete_reference(dm->env, *refs[at]);
        }
        *refs[at] = NULL;
    }
}
