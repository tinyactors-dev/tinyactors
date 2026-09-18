#include "json_datamodel_jv.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "jq.h"

typedef struct {
    tat_json_datamodel_context *context;
    void *next_free;
    size_t references;
    jv value;
} tat_json_value;

/* Per-definition state; the expression table is borrowed from the definition. */
typedef struct {
    const ta_expression *expressions;
    uint32_t expression_count;
} tat_json_shared;

typedef struct {
    tat_json_datamodel_context *context;
    const tat_json_shared *shared;
    jv root;
} tat_json_instance;

static void tat_json_retain(void *self);
static void tat_json_release(void *self);
static ta_result tat_json_clone(void *self, ta_value *out);

static const ta_value_vtable TAT_JSON_VALUE_VTABLE = {
    .retain = tat_json_retain,
    .release = tat_json_release,
    .clone = tat_json_clone,
};

static ta_result tat_json_wrap(tat_json_datamodel_context *context,
                               jv value,
                               ta_value *out) {
    tat_json_value *wrapped = context->free_values;
    if (wrapped != NULL) {
        context->free_values = wrapped->next_free;
    } else {
        wrapped = ta_arena_alloc(&context->values, sizeof(*wrapped),
                                 _Alignof(tat_json_value));
    }
    if (wrapped == NULL) {
        jv_free(value);
        return TA_ERR_NOMEM;
    }
    *wrapped = (tat_json_value){
        .context = context,
        .references = 1,
        .value = value,
    };
    *out = (ta_value){.self = wrapped, .v = &TAT_JSON_VALUE_VTABLE};
    return TA_OK;
}

static bool tat_json_unwrap(const ta_value *value, jv *out) {
    if (value == NULL || value->v != &TAT_JSON_VALUE_VTABLE || value->self == NULL) {
        return false;
    }
    tat_json_value *wrapped = value->self;
    *out = jv_copy(wrapped->value);
    return true;
}

static void tat_json_retain(void *self) {
    tat_json_value *value = self;
    value->references += 1;
}

static void tat_json_release(void *self) {
    tat_json_value *value = self;
    value->references -= 1;
    if (value->references == 0) {
        jv_free(value->value);
        value->next_free = value->context->free_values;
        value->context->free_values = value;
    }
}

static ta_result tat_json_clone(void *self, ta_value *out) {
    tat_json_value *value = self;
    return tat_json_wrap(value->context, jv_copy(value->value), out);
}

ta_result tat_json_datamodel_context_init(tat_json_datamodel_context *context,
                                          ta_allocator allocator) {
    if (context == NULL) {
        return TA_ERR_INVALID;
    }
    *context = (tat_json_datamodel_context){0};
    ta_result result = ta_arena_init(&context->values, allocator, 4096);
    if (result != TA_OK) {
        return result;
    }
    result = ta_arena_init(&context->scratch, allocator, 4096);
    if (result != TA_OK) {
        ta_arena_destroy(&context->values);
        return result;
    }
    return TA_OK;
}

void tat_json_datamodel_context_destroy(tat_json_datamodel_context *context) {
    if (context == NULL) {
        return;
    }
    ta_arena_destroy(&context->scratch);
    ta_arena_destroy(&context->values);
    *context = (tat_json_datamodel_context){0};
}

ta_result tat_json_value_from_jv(tat_json_datamodel_context *context, jv value, ta_value *out) {
    if (context == NULL || out == NULL || !jv_is_valid(value)) {
        jv_free(value);
        return TA_ERR_INVALID;
    }
    return tat_json_wrap(context, value, out);
}

bool tat_json_value_to_jv(const ta_value *value, jv *out) {
    return tat_json_unwrap(value, out);
}

ta_result tat_json_value_parse(tat_json_datamodel_context *context,
                               const char *json,
                               ta_value *out) {
    if (context == NULL || json == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    jv value = jv_parse(json);
    if (!jv_is_valid(value)) {
        jv_free(value);
        return TA_ERR_INVALID;
    }
    return tat_json_wrap(context, value, out);
}

ta_result tat_json_value_format(const ta_value *value,
                                char *buffer,
                                size_t capacity,
                                size_t *needed) {
    if (needed == NULL) {
        return TA_ERR_INVALID;
    }
    jv json;
    if (!tat_json_unwrap(value, &json)) {
        return TA_ERR_INVALID;
    }
    jv text = jv_dump_string(json, 0);
    size_t length = (size_t)jv_string_length_bytes(jv_copy(text));
    *needed = length + 1;
    if (buffer == NULL || capacity < length + 1) {
        jv_free(text);
        return TA_ERR_NOMEM;
    }
    memcpy(buffer, jv_string_value(text), length + 1);
    jv_free(text);
    return TA_OK;
}

static void tat_json_fault(ta_fault *fault, int32_t code, const char *message) {
    if (fault == NULL) {
        return;
    }
    *fault = (ta_fault){
        .code = code,
        .message = {.ptr = message, .len = (uint32_t)strlen(message)},
    };
}

/* Returns the jq source of a definition expression; ptr is null when unusable. */
static ta_str tat_json_expression(const tat_json_instance *instance, ta_expr_id id) {
    if (instance == NULL || instance->shared == NULL ||
        id >= instance->shared->expression_count) {
        return (ta_str){0};
    }
    const ta_expression *expression = &instance->shared->expressions[id];
    return expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source : (ta_str){0};
}

static char *tat_json_source(tat_json_datamodel_context *context, ta_str expression) {
    size_t length = expression.len;
    char *source = ta_arena_alloc(&context->scratch, length + 1, _Alignof(char));
    if (source == NULL) {
        return NULL;
    }
    memcpy(source, expression.ptr, length);
    source[length] = '\0';
    return source;
}

static ta_op_status tat_json_run_source(const char *source,
                                        jv input,
                                        jv arguments,
                                        jv *out,
                                        ta_fault *fault) {
    jq_state *jq = jq_init();
    if (jq == NULL) {
        jv_free(input);
        jv_free(arguments);
        tat_json_fault(fault, TA_ERR_NOMEM, "could not initialize JSON expression");
        return TA_OP_ERROR;
    }
    int compiled = jq_compile_args(jq, source, arguments);
    if (!compiled) {
        jq_teardown(&jq);
        jv_free(input);
        tat_json_fault(fault, TA_ERR_INVALID, "JSON expression did not compile");
        return TA_OP_ERROR;
    }

    jq_start(jq, input, 0);
    jv result = jq_next(jq);
    if (!jv_is_valid(result)) {
        jv_free(result);
        jq_teardown(&jq);
        tat_json_fault(fault, TA_ERR_INVALID, "JSON expression produced no value");
        return TA_OP_ERROR;
    }
    jv extra = jq_next(jq);
    if (jv_is_valid(extra) || jv_invalid_has_msg(jv_copy(extra))) {
        jv_free(extra);
        jv_free(result);
        jq_teardown(&jq);
        tat_json_fault(fault, TA_ERR_INVALID, "JSON expression must produce exactly one value");
        return TA_OP_ERROR;
    }
    jv_free(extra);
    jq_teardown(&jq);
    *out = result;
    return TA_OP_OK;
}

static ta_op_status tat_json_run(tat_json_datamodel_context *context,
                                 ta_str expression,
                                 jv input,
                                 jv arguments,
                                 jv *out,
                                 ta_fault *fault) {
    if (expression.ptr == NULL) {
        jv_free(input);
        jv_free(arguments);
        tat_json_fault(fault, TA_ERR_INVALID, "unknown JSON expression");
        return TA_OP_ERROR;
    }
    ta_arena_reset(&context->scratch);
    char *source = tat_json_source(context, expression);
    if (source == NULL) {
        jv_free(input);
        jv_free(arguments);
        tat_json_fault(fault, TA_ERR_NOMEM, "could not stage JSON expression");
        return TA_OP_ERROR;
    }
    return tat_json_run_source(source, input, arguments, out, fault);
}

static ta_result tat_json_prepare(void *self,
                                  const ta_datamodel_prepare_definition_params *params) {
    if (self == NULL || params == NULL || params->shared == NULL ||
        (params->expressions == NULL && params->expression_count != 0) ||
        params->allocator.v == NULL) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < params->expression_count; index += 1) {
        ta_expression_kind kind = params->expressions[index].kind;
        if (kind != TA_EXPRESSION_NONE && kind != TA_EXPRESSION_SOURCE) {
            return TA_ERR_UNSUPPORTED;
        }
    }
    tat_json_shared *shared = params->allocator.v->alloc(
        params->allocator.self, sizeof(*shared), _Alignof(tat_json_shared));
    if (shared == NULL) {
        return TA_ERR_NOMEM;
    }
    *shared = (tat_json_shared){
        .expressions = params->expressions,
        .expression_count = params->expression_count,
    };
    *params->shared = shared;
    return TA_OK;
}

static void tat_json_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self;
    if (shared == NULL || allocator.v == NULL) {
        return;
    }
    allocator.v->free(allocator.self,
                      &(ta_allocator_free_params){
                          .ptr = shared,
                          .size = sizeof(tat_json_shared),
                          .alignment = _Alignof(tat_json_shared),
                      });
}

static uint32_t tat_json_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return (uint32_t)sizeof(tat_json_instance);
}

static uint32_t tat_json_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return (uint32_t)_Alignof(tat_json_instance);
}

static ta_result tat_json_init(void *self, const ta_datamodel_init_instance_params *params) {
    if (self == NULL || params == NULL || params->shared == NULL ||
        params->instance == NULL) {
        return TA_ERR_INVALID;
    }
    tat_json_instance *instance = params->instance;
    instance->context = self;
    instance->shared = params->shared;
    if (params->binding == NULL) {
        instance->root = jv_object();
        return TA_OK;
    }
    if (!tat_json_unwrap(params->binding, &instance->root)) {
        return TA_ERR_INVALID;
    }
    return TA_OK;
}

static void tat_json_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    tat_json_instance *json = instance;
    jv_free(json->root);
    *json = (tat_json_instance){0};
}

static bool tat_json_has_member(jv root, ta_str name) {
    return name.ptr != NULL && name.len <= INT_MAX &&
           jv_object_has(jv_copy(root), jv_string_sized(name.ptr, (int)name.len));
}

/* Top-level named bindings are members of the root object. */
static ta_op_status tat_json_set_member(tat_json_instance *instance, ta_str name, jv value,
                                        ta_fault *fault) {
    if (jv_get_kind(instance->root) != JV_KIND_OBJECT || name.ptr == NULL || name.len > INT_MAX) {
        jv_free(value);
        tat_json_fault(fault, TA_ERR_INVALID, "JSON data bindings need an object root");
        return TA_OP_ERROR;
    }
    instance->root = jv_object_set(instance->root, jv_string_sized(name.ptr, (int)name.len), value);
    return TA_OP_OK;
}

static ta_op_status tat_json_declare(void *self, const ta_datamodel_declare_params *params) {
    (void)self;
    tat_json_instance *instance = params->instance;
    if (jv_get_kind(instance->root) == JV_KIND_OBJECT && tat_json_has_member(instance->root, params->name)) {
        return TA_OP_OK;
    }
    return tat_json_set_member(instance, params->name, jv_null(), params->fault);
}

static ta_op_status tat_json_set_named(void *self, const ta_datamodel_set_named_params *params) {
    (void)self;
    jv value;
    if (!tat_json_unwrap(params->value, &value)) {
        tat_json_fault(params->fault, TA_ERR_INVALID, "data value is not a JSON value");
        return TA_OP_ERROR;
    }
    return tat_json_set_member(params->instance, params->name, value, params->fault);
}

static ta_op_status tat_json_eval_value(void *self,
                                        const ta_datamodel_eval_value_params *params) {
    (void)self;
    tat_json_instance *instance = params->instance;
    jv result;
    ta_op_status status = tat_json_run(instance->context,
                                       tat_json_expression(instance, params->expr),
                                       jv_copy(instance->root), jv_object(), &result,
                                       params->fault);
    if (status == TA_OP_OK &&
        tat_json_wrap(instance->context, result, params->out) != TA_OK) {
        tat_json_fault(params->fault, TA_ERR_NOMEM, "could not retain JSON result");
        return TA_OP_ERROR;
    }
    return status;
}

static ta_op_status tat_json_eval_condition(
    void *self, const ta_datamodel_eval_condition_params *params) {
    (void)self;
    tat_json_instance *instance = params->instance;
    jv result;
    ta_op_status status = tat_json_run(instance->context,
                                       tat_json_expression(instance, params->expr),
                                       jv_copy(instance->root), jv_object(), &result,
                                       params->fault);
    if (status == TA_OP_OK) {
        jv_kind kind = jv_get_kind(result);
        *params->out = kind != JV_KIND_NULL && kind != JV_KIND_FALSE;
        jv_free(result);
    }
    return status;
}

static ta_op_status tat_json_read_location(
    void *self, const ta_datamodel_read_location_params *params) {
    ta_datamodel_eval_value_params eval = {
        .shared = params->shared,
        .instance = params->instance,
        .expr = params->location,
        .host = params->host,
        .out = params->out,
        .fault = params->fault,
    };
    return tat_json_eval_value(self, &eval);
}

static ta_op_status tat_json_assign(void *self,
                                    const ta_datamodel_assign_params *params) {
    (void)self;
    tat_json_instance *instance = params->instance;
    ta_str location = tat_json_expression(instance, params->location);
    jv assigned;
    if (location.ptr == NULL || !tat_json_unwrap(params->value, &assigned)) {
        tat_json_fault(params->fault, TA_ERR_INVALID, "invalid JSON assignment");
        return TA_OP_ERROR;
    }
    if (location.len > INT_MAX) {
        jv_free(assigned);
        tat_json_fault(params->fault, TA_ERR_INVALID, "JSON location is too long");
        return TA_OP_ERROR;
    }
    ta_arena_reset(&instance->context->scratch);
    size_t program_size = (size_t)location.len + sizeof("() = $value");
    char *program = ta_arena_alloc(&instance->context->scratch, program_size,
                                   _Alignof(char));
    if (program == NULL) {
        jv_free(assigned);
        tat_json_fault(params->fault, TA_ERR_NOMEM, "could not stage JSON assignment");
        return TA_OP_ERROR;
    }
    snprintf(program, program_size, "(%.*s) = $value", (int)location.len,
             location.ptr);
    jv arguments = jv_object_set(jv_object(), jv_string("value"), assigned);
    jv root;
    ta_op_status status = tat_json_run_source(program, jv_copy(instance->root),
                                              arguments, &root, params->fault);
    if (status == TA_OP_OK) {
        jv_free(instance->root);
        instance->root = root;
    }
    return status;
}

static ta_op_status tat_json_execute_script(
    void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    tat_json_instance *instance = params->instance;
    jv root;
    ta_op_status status = tat_json_run(instance->context,
                                       tat_json_expression(instance, params->script),
                                       jv_copy(instance->root), jv_object(), &root,
                                       params->fault);
    if (status == TA_OP_OK) {
        jv_free(instance->root);
        instance->root = root;
    }
    return status;
}

#define TAT_SCXML_ACTOR_PREFIX "#_scxml_"

static ta_op_status tat_json_decode_target(
    void *self, const ta_datamodel_decode_scxml_target_params *params) {
    (void)self;
    jv value = jv_invalid();
    if (!tat_json_unwrap(params->value, &value) || jv_get_kind(value) != JV_KIND_STRING) {
        jv_free(value);
        tat_json_fault(params->fault, TA_ERR_INVALID, "SCXML target must be a JSON string");
        return TA_OP_ERROR;
    }
    const char *target = jv_string_value(value);
    ta_scxml_target decoded = {0};
    if (strcmp(target, "#_internal") == 0) {
        decoded.kind = TA_SCXML_TARGET_INTERNAL;
    } else if (strcmp(target, "#_self") == 0) {
        decoded.kind = TA_SCXML_TARGET_SELF;
    } else if (strcmp(target, "#_parent") == 0) {
        decoded.kind = TA_SCXML_TARGET_PARENT;
    } else if (strncmp(target, TAT_SCXML_ACTOR_PREFIX,
                       sizeof(TAT_SCXML_ACTOR_PREFIX) - 1) == 0) {
        const char *digits = target + sizeof(TAT_SCXML_ACTOR_PREFIX) - 1;
        char *end = NULL;
        uint64_t actor = strtoull(digits, &end, 10);
        if (end == digits || *end != '\0') {
            jv_free(value);
            tat_json_fault(params->fault, TA_ERR_INVALID, "invalid SCXML actor target");
            return TA_OP_ERROR;
        }
        decoded.kind = TA_SCXML_TARGET_ACTOR;
        decoded.as.actor.kind = TA_ACTOR_REF_RUNTIME;
        decoded.as.actor.as.runtime = actor;
    } else if (strncmp(target, "#_", 2) == 0) {
        char *end = NULL;
        uint64_t invoke = strtoull(target + 2, &end, 10);
        if (end == target + 2 || *end != '\0') {
            jv_free(value);
            tat_json_fault(params->fault, TA_ERR_INVALID, "invalid SCXML invoke target");
            return TA_OP_ERROR;
        }
        decoded.kind = TA_SCXML_TARGET_INVOKE;
        decoded.as.invoke = invoke;
    } else {
        jv_free(value);
        tat_json_fault(params->fault, TA_ERR_INVALID, "unsupported SCXML target");
        return TA_OP_ERROR;
    }
    jv_free(value);
    *params->out = decoded;
    return TA_OP_OK;
}

static ta_op_status tat_json_make_params(void *self,
                                         const ta_datamodel_make_params_params *params) {
    (void)self;
    jv values = jv_array();
    for (uint32_t index = 0; index < params->count; index += 1) {
        jv value;
        if (!tat_json_unwrap(&params->values[index].value, &value)) {
            jv_free(values);
            tat_json_fault(params->fault, TA_ERR_INVALID, "parameter is not a JSON value");
            return TA_OP_ERROR;
        }
        jv entry = jv_object();
        entry = jv_object_set(entry, jv_string("name_atom"),
                              jv_number((double)params->values[index].name));
        entry = jv_object_set(entry, jv_string("value"), value);
        values = jv_array_append(values, entry);
    }
    tat_json_instance *instance = params->instance;
    if (tat_json_wrap(instance->context, values, params->out) != TA_OK) {
        tat_json_fault(params->fault, TA_ERR_NOMEM, "could not retain JSON parameters");
        return TA_OP_ERROR;
    }
    return TA_OP_OK;
}

static ta_result tat_json_inspect(void *self, const ta_datamodel_inspect_params *params) {
    (void)self;
    tat_json_instance *instance = params->instance;
    return tat_json_wrap(instance->context, jv_copy(instance->root), params->out);
}

static const ta_datamodel_vtable TAT_JSON_DATAMODEL_VTABLE = {
    .name = {.ptr = "jq", .len = 2},
    .prepare_definition = tat_json_prepare,
    .destroy_definition = tat_json_destroy_definition,
    .instance_size = tat_json_instance_size,
    .instance_alignment = tat_json_instance_alignment,
    .init_instance = tat_json_init,
    .destroy_instance = tat_json_destroy_instance,
    .declare = tat_json_declare,
    .set_named = tat_json_set_named,
    .eval_condition = tat_json_eval_condition,
    .eval_value = tat_json_eval_value,
    .read_location = tat_json_read_location,
    .assign = tat_json_assign,
    .execute_script = tat_json_execute_script,
    .decode_scxml_target = tat_json_decode_target,
    .make_params = tat_json_make_params,
    .inspect = tat_json_inspect,
};

ta_datamodel tat_json_datamodel(tat_json_datamodel_context *context) {
    return (ta_datamodel){.self = context, .v = &TAT_JSON_DATAMODEL_VTABLE};
}
