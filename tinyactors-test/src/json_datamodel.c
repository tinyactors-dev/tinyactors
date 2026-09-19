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
    /* value_text() of a non-string: its JSON text, kept while the value lives. */
    jv text;
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
        .text = jv_invalid(),
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
        jv_free(value->text);
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

/*
 * Every program starts with this prelude. `In("id")` tests the configuration
 * through $__ta_active, the author IDs of the active states, which
 * tat_json_arguments() binds together with the SCXML system variables.
 */
#define TAT_JSON_PRELUDE "def In($id): any($__ta_active[]; . == $id); "

static char *tat_json_source(tat_json_datamodel_context *context, ta_str expression) {
    size_t prelude = sizeof(TAT_JSON_PRELUDE) - 1;
    size_t length = expression.len;
    char *source = ta_arena_alloc(&context->scratch, prelude + length + 1, _Alignof(char));
    if (source == NULL) {
        return NULL;
    }
    memcpy(source, TAT_JSON_PRELUDE, prelude);
    memcpy(source + prelude, expression.ptr, length);
    source[prelude + length] = '\0';
    return source;
}

/* One system-variable field as a JSON string, or null when it has no value. */
static jv tat_json_host_text(const ta_dm_host *host, ta_dm_text_kind kind) {
    char buffer[256];
    uint32_t needed = 0;
    ta_dm_text_params params = {.kind = kind, .buffer = buffer, .capacity = sizeof(buffer), .needed = &needed};
    ta_result result = ta_dm_host_text(host, &params);
    if (result == TA_ERR_NOMEM && needed != 0) {
        char *large = malloc(needed);
        if (large == NULL) {
            return jv_null();
        }
        params.buffer = large;
        params.capacity = needed;
        result = ta_dm_host_text(host, &params);
        jv text = result == TA_OK ? jv_string_sized(large, (int)(needed - 1)) : jv_null();
        free(large);
        return text;
    }
    return result == TA_OK ? jv_string_sized(buffer, (int)(needed - 1)) : jv_null();
}

/* $_event: null until the first event, then an object with every SCXML field. */
static jv tat_json_event(const ta_dm_host *host) {
    const ta_event *event = ta_dm_host_event(host);
    if (event == NULL) {
        return jv_null();
    }
    static const struct { const char *name; ta_dm_text_kind kind; } fields[] = {
        {"name", TA_DM_TEXT_EVENT_NAME},         {"type", TA_DM_TEXT_EVENT_TYPE},
        {"sendid", TA_DM_TEXT_EVENT_SENDID},     {"origin", TA_DM_TEXT_EVENT_ORIGIN},
        {"origintype", TA_DM_TEXT_EVENT_ORIGINTYPE}, {"invokeid", TA_DM_TEXT_EVENT_INVOKEID},
    };
    jv object = jv_object();
    for (size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); index += 1) {
        object = jv_object_set(object, jv_string(fields[index].name), tat_json_host_text(host, fields[index].kind));
    }
    jv data;
    if (!tat_json_unwrap(&event->data, &data)) {
        if (event->data.v != NULL) {
            /* Another data model's value: fault rather than read it as null. */
            jv_free(object);
            return jv_invalid_with_msg(jv_string("event data is not a JSON value"));
        }
        data = jv_null();
    }
    return jv_object_set(object, jv_string("data"), data);
}

/* Binds $_event, $_sessionid, $_name, $_ioprocessors, and $__ta_active into `arguments`. */
static jv tat_json_arguments(const ta_dm_host *host, jv arguments) {
    if (host == NULL) {
        arguments = jv_object_set(arguments, jv_string("_event"), jv_null());
        arguments = jv_object_set(arguments, jv_string("_sessionid"), jv_null());
        arguments = jv_object_set(arguments, jv_string("_name"), jv_null());
        arguments = jv_object_set(arguments, jv_string("_ioprocessors"), jv_object());
        return jv_object_set(arguments, jv_string("__ta_active"), jv_array());
    }
    jv event = tat_json_event(host);
    if (!jv_is_valid(event)) {
        jv_free(arguments);
        return event;
    }
    arguments = jv_object_set(arguments, jv_string("_event"), event);
    arguments = jv_object_set(arguments, jv_string("_sessionid"), tat_json_host_text(host, TA_DM_TEXT_SESSION_ID));
    arguments = jv_object_set(arguments, jv_string("_name"), tat_json_host_text(host, TA_DM_TEXT_SESSION_NAME));
    jv processor = jv_object_set(jv_object(), jv_string("location"),
                                 tat_json_host_text(host, TA_DM_TEXT_SCXML_IO_LOCATION));
    jv type = tat_json_host_text(host, TA_DM_TEXT_SCXML_IO_TYPE);
    jv processors = jv_object();
    if (jv_get_kind(type) == JV_KIND_STRING) {
        processors = jv_object_set(processors, jv_copy(type), jv_copy(processor));
    }
    jv_free(type);
    /* The short type name selects the same processor. */
    processors = jv_object_set(processors, jv_string("scxml"), processor);
    arguments = jv_object_set(arguments, jv_string("_ioprocessors"), processors);
    jv active = jv_array();
    ta_str name = {0};
    for (ta_state_id state = 0; ta_dm_host_state_name(host, state, &name) == TA_OK; state += 1) {
        if (ta_dm_host_in_state(host, state)) {
            active = jv_array_append(active, jv_string_sized(name.ptr, (int)name.len));
        }
    }
    return jv_object_set(arguments, jv_string("__ta_active"), active);
}

static ta_op_status tat_json_run_source(const char *source,
                                        jv input,
                                        jv arguments,
                                        jv *out,
                                        ta_fault *fault) {
    if (!jv_is_valid(arguments)) {
        jv_free(input);
        jv_free(arguments);
        tat_json_fault(fault, TA_ERR_UNSUPPORTED, "event data is not a JSON value");
        return TA_OP_ERROR;
    }
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
        /* The binding (e.g. an invoked child's <param>s) supplies this value. */
        if (params->bound != NULL) {
            *params->bound = true;
        }
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
                                       jv_copy(instance->root), tat_json_arguments(params->host, jv_object()), &result,
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
                                       jv_copy(instance->root), tat_json_arguments(params->host, jv_object()), &result,
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
    size_t program_size = sizeof(TAT_JSON_PRELUDE) - 1 + (size_t)location.len + sizeof("() = $value");
    char *program = ta_arena_alloc(&instance->context->scratch, program_size,
                                   _Alignof(char));
    if (program == NULL) {
        jv_free(assigned);
        tat_json_fault(params->fault, TA_ERR_NOMEM, "could not stage JSON assignment");
        return TA_OP_ERROR;
    }
    snprintf(program, program_size, TAT_JSON_PRELUDE "(%.*s) = $value", (int)location.len,
             location.ptr);
    jv arguments = tat_json_arguments(params->host, jv_object_set(jv_object(), jv_string("value"), assigned));
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
                                       jv_copy(instance->root), tat_json_arguments(params->host, jv_object()), &root,
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

/*
 * Named parameters become an object keyed by parameter name, so `<param
 * name="x">` reads as `$_event.data.x`. A later duplicate name replaces an
 * earlier one.
 */
static ta_op_status tat_json_make_params(void *self,
                                         const ta_datamodel_make_params_params *params) {
    (void)self;
    jv values = jv_object();
    for (uint32_t index = 0; index < params->count; index += 1) {
        jv value;
        if (!tat_json_unwrap(&params->values[index].value, &value)) {
            jv_free(values);
            tat_json_fault(params->fault, TA_ERR_INVALID, "parameter is not a JSON value");
            return TA_OP_ERROR;
        }
        ta_str name = params->values[index].name;
        if (name.len > INT_MAX || (name.ptr == NULL && name.len != 0)) {
            jv_free(value);
            jv_free(values);
            tat_json_fault(params->fault, TA_ERR_INVALID, "invalid parameter name");
            return TA_OP_ERROR;
        }
        jv key = jv_string_sized(name.len == 0 ? "" : name.ptr, (int)name.len);
        values = jv_object_set(values, key, value);
    }
    tat_json_instance *instance = params->instance;
    if (tat_json_wrap(instance->context, values, params->out) != TA_OK) {
        tat_json_fault(params->fault, TA_ERR_NOMEM, "could not retain JSON parameters");
        return TA_OP_ERROR;
    }
    return TA_OP_OK;
}

/* Strings read as their contents; any other value as its JSON text. */
static ta_op_status tat_json_value_text(void *self, const ta_datamodel_value_text_params *params) {
    (void)self;
    if (params->value == NULL || params->value->v != &TAT_JSON_VALUE_VTABLE || params->value->self == NULL) {
        tat_json_fault(params->fault, TA_ERR_INVALID, "value is not a JSON value");
        return TA_OP_ERROR;
    }
    tat_json_value *wrapped = params->value->self;
    if (jv_get_kind(wrapped->value) == JV_KIND_STRING) {
        *params->out = (ta_str){.ptr = jv_string_value(wrapped->value),
                                .len = (uint32_t)jv_string_length_bytes(jv_copy(wrapped->value))};
        return TA_OP_OK;
    }
    if (!jv_is_valid(wrapped->text)) {
        wrapped->text = jv_dump_string(jv_copy(wrapped->value), 0);
    }
    *params->out = (ta_str){.ptr = jv_string_value(wrapped->text),
                            .len = (uint32_t)jv_string_length_bytes(jv_copy(wrapped->text))};
    return TA_OP_OK;
}

static ta_op_status tat_json_text_value(void *self, const ta_datamodel_text_value_params *params) {
    (void)self;
    tat_json_instance *instance = params->instance;
    if (params->text.len > INT_MAX || (params->text.ptr == NULL && params->text.len != 0)) {
        tat_json_fault(params->fault, TA_ERR_INVALID, "text is too long");
        return TA_OP_ERROR;
    }
    jv text = jv_string_sized(params->text.len == 0 ? "" : params->text.ptr, (int)params->text.len);
    if (tat_json_wrap(instance->context, text, params->out) != TA_OK) {
        tat_json_fault(params->fault, TA_ERR_NOMEM, "could not retain JSON text");
        return TA_OP_ERROR;
    }
    return TA_OP_OK;
}

/*
 * Literal content is JSON when it parses as JSON; otherwise it is a string
 * with its whitespace normalized, as for the ECMAScript data model (B.2.2).
 */
static ta_op_status tat_json_import_content(void *self, const ta_datamodel_import_content_params *params) {
    (void)self;
    tat_json_instance *instance = params->instance;
    if (params->content.len > INT_MAX) {
        tat_json_fault(params->fault, TA_ERR_INVALID, "content is too long");
        return TA_OP_ERROR;
    }
    const char *bytes = params->content.len == 0 ? "" : (const char *)params->content.ptr;
    jv value = jv_parse_sized(bytes, (int)params->content.len);
    if (!jv_is_valid(value)) {
        jv_free(value);
        char *normalized = malloc((size_t)params->content.len + 1);
        if (normalized == NULL) {
            tat_json_fault(params->fault, TA_ERR_NOMEM, "could not import content");
            return TA_OP_ERROR;
        }
        size_t length = 0;
        bool space = false;
        for (uint32_t index = 0; index < params->content.len; index += 1) {
            char c = bytes[index];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                space = length != 0;
                continue;
            }
            if (space) {
                normalized[length++] = ' ';
                space = false;
            }
            normalized[length++] = c;
        }
        value = jv_string_sized(normalized, (int)length);
        free(normalized);
    }
    if (tat_json_wrap(instance->context, value, params->out) != TA_OK) {
        tat_json_fault(params->fault, TA_ERR_NOMEM, "could not retain JSON content");
        return TA_OP_ERROR;
    }
    return TA_OP_OK;
}

static ta_result tat_json_inspect(void *self, const ta_datamodel_inspect_params *params) {
    (void)self;
    tat_json_instance *instance = params->instance;
    return tat_json_wrap(instance->context, jv_copy(instance->root), params->out);
}

/*
 * <foreach>: the snapshot is the evaluated array or object itself (jv values
 * are immutable, so it is already a shallow copy). Objects iterate their
 * values in key order.
 */
static ta_op_status tat_json_foreach_snapshot(void *self,
                                              const ta_datamodel_foreach_snapshot_params *params) {
    (void)self;
    tat_json_instance *instance = params->instance;
    jv collection;
    ta_op_status status = tat_json_run(instance->context,
                                       tat_json_expression(instance, params->array),
                                       jv_copy(instance->root), jv_object(), &collection,
                                       params->fault);
    if (status != TA_OP_OK) {
        return status;
    }
    jv_kind kind = jv_get_kind(collection);
    if (kind != JV_KIND_ARRAY && kind != JV_KIND_OBJECT) {
        jv_free(collection);
        tat_json_fault(params->fault, TA_ERR_INVALID, "foreach array is not an array or object");
        return TA_OP_ERROR;
    }
    if (kind == JV_KIND_OBJECT) {
        jv keys = jv_keys(jv_copy(collection));
        jv values = jv_array();
        jv_array_foreach(keys, index, key) {
            values = jv_array_append(values, jv_object_get(jv_copy(collection), key));
        }
        jv_free(keys);
        jv_free(collection);
        collection = values;
    }
    *params->count = (uint32_t)jv_array_length(jv_copy(collection));
    if (tat_json_wrap(instance->context, collection, params->out) != TA_OK) {
        tat_json_fault(params->fault, TA_ERR_NOMEM, "could not retain foreach snapshot");
        return TA_OP_ERROR;
    }
    return TA_OP_OK;
}

/* A legal jq-model variable: a root member named like a jq identifier. */
static bool tat_json_variable_name(ta_str name) {
    if (name.ptr == NULL || name.len == 0) {
        return false;
    }
    for (uint32_t index = 0; index < name.len; index += 1) {
        char c = name.ptr[index];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!alpha && !(index > 0 && c >= '0' && c <= '9')) {
            return false;
        }
    }
    return true;
}

static ta_op_status tat_json_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    (void)self;
    jv snapshot;
    if (!tat_json_unwrap(params->snapshot, &snapshot)) {
        tat_json_fault(params->fault, TA_ERR_INVALID, "foreach snapshot is not a JSON value");
        return TA_OP_ERROR;
    }
    bool has_index = params->index_name.ptr != NULL;
    if (!tat_json_variable_name(params->item) || (has_index && !tat_json_variable_name(params->index_name))) {
        jv_free(snapshot);
        tat_json_fault(params->fault, TA_ERR_INVALID, "foreach item or index is not a legal variable name");
        return TA_OP_ERROR;
    }
    jv item = jv_array_get(snapshot, (int)params->index);
    ta_op_status status = tat_json_set_member(params->instance, params->item, item, params->fault);
    if (status == TA_OP_OK && has_index) {
        status = tat_json_set_member(params->instance, params->index_name, jv_number((double)params->index),
                                     params->fault);
    }
    return status;
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
    .value_text = tat_json_value_text,
    .text_value = tat_json_text_value,
    .import_content = tat_json_import_content,
    .foreach_snapshot = tat_json_foreach_snapshot,
    .foreach_bind = tat_json_foreach_bind,
};

ta_datamodel tat_json_datamodel(tat_json_datamodel_context *context) {
    return (ta_datamodel){.self = context, .v = &TAT_JSON_DATAMODEL_VTABLE};
}
