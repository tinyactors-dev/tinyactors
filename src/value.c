#include "internal.h"

typedef struct value_root {
    const ta_system_t *system;
    ta_value_t value;
} value_root_t;

static void release_root(void *argument)
{
    value_root_t *root = (value_root_t *)argument;
    if (root != NULL && root->value.handle != 0u && root->system != NULL &&
        root->system->config.values.release != NULL)
        root->system->config.values.release(root->system->config.host_user, root->value);
}

static ta_status_t root_prepare(const tai_value_scope_t *scope, value_root_t **out)
{
    void *memory;
    ta_status_t status;
    if (scope == NULL || scope->system == NULL || scope->arena == NULL || out == NULL)
        return TA_INVALID;
    *out = NULL;
    status = tai_alloc(scope->arena, sizeof(value_root_t), &memory);
    if (status != TA_OK) return status;
    *out = (value_root_t *)memory;
    (*out)->system = scope->system;
    (*out)->value.handle = 0u;
    return ta_arena_defer(scope->arena, release_root, *out);
}

ta_status_t tai_value_snapshot(const tai_value_scope_t *scope, ta_value_t source,
                               ta_value_t *out)
{
    value_root_t *root;
    ta_value_t value;
    ta_status_t status;
    if (out == NULL) return TA_INVALID;
    out->handle = 0u;
    if (source.handle == 0u) {
        if (scope == NULL || scope->system == NULL || scope->arena == NULL) return TA_INVALID;
        return TA_OK;
    }
    if (scope == NULL || scope->system == NULL || scope->arena == NULL ||
        scope->system->config.values.snapshot == NULL ||
        scope->system->config.values.release == NULL ||
        scope->system->config.values.retain == NULL)
        return TA_UNSUPPORTED;
    status = root_prepare(scope, &root);
    if (status != TA_OK) return status;
    value.handle = 0u;
    status = scope->system->config.values.snapshot(scope->system->config.host_user,
                                                    source, &value);
    if (status != TA_OK) return status;
    if (value.handle == 0u) return TA_INVALID;
    root->value = value;
    *out = value;
    return TA_OK;
}

static uint8_t bytes_valid(ta_bytes_t bytes)
{
    return (uint8_t)(bytes.size == 0u ? bytes.data == NULL : bytes.data != NULL);
}

ta_status_t tai_payload_snapshot(const tai_value_scope_t *scope,
                                 const ta_payload_t *source, ta_payload_t *out)
{
    ta_status_t status;
    void *memory;
    if (source == NULL || out == NULL) return TA_INVALID;
    *out = (ta_payload_t){0};
    if (scope == NULL || scope->system == NULL || scope->arena == NULL) return TA_INVALID;
    if (source->kind == TA_PAYLOAD_NONE) {
        return TA_OK;
    }
    if (source->kind == TA_PAYLOAD_NATIVE) {
        if (source->as.native.handle == 0u) return TA_INVALID;
        status = tai_value_snapshot(scope, source->as.native, &out->as.native);
    } else if (source->kind == TA_PAYLOAD_BYTES) {
        if (!bytes_valid(source->as.bytes)) return TA_INVALID;
        if (source->as.bytes.size == 0u) {
            out->kind = TA_PAYLOAD_BYTES;
            return TA_OK;
        }
        status = tai_alloc(scope == NULL ? NULL : scope->arena, source->as.bytes.size, &memory);
        if (status == TA_OK) {
            tai_copy(memory, source->as.bytes.data, source->as.bytes.size);
            out->kind = TA_PAYLOAD_BYTES;
            out->as.bytes.data = memory;
            out->as.bytes.size = source->as.bytes.size;
            return TA_OK;
        }
    } else return TA_INVALID;
    if (status != TA_OK) return status;
    out->kind = TA_PAYLOAD_NATIVE;
    return TA_OK;
}

ta_status_t tai_body_snapshot(const tai_value_scope_t *scope,
                              const ta_body_t *source, ta_body_t *out)
{
    size_t i;
    ta_status_t status;
    void *memory;
    ta_parameter_t *parameters;
    if (source == NULL || out == NULL) return TA_INVALID;
    *out = (ta_body_t){0};
    if (scope == NULL || scope->system == NULL || scope->arena == NULL) return TA_INVALID;
    if (source->kind == TA_BODY_NONE) {
        if (source->parameter_count != 0u || source->parameters != NULL || source->content.kind != TA_PAYLOAD_NONE)
            return TA_INVALID;
        return TA_OK;
    }
    if (source->kind == TA_BODY_CONTENT) {
        if (source->parameter_count != 0u || source->parameters != NULL) return TA_INVALID;
        status = tai_payload_snapshot(scope, &source->content, &out->content);
        if (status == TA_OK) out->kind = TA_BODY_CONTENT;
        return status;
    }
    if (source->kind != TA_BODY_PARAMETERS || source->content.kind != TA_PAYLOAD_NONE ||
        (source->parameter_count != 0u && source->parameters == NULL)) return TA_INVALID;
    if (source->parameter_count == 0u) { out->kind = TA_BODY_PARAMETERS; return TA_OK; }
    if (source->parameter_count > (size_t)-1 / sizeof(*parameters)) return TA_LIMIT;
    status = tai_alloc(scope == NULL ? NULL : scope->arena,
                       source->parameter_count * sizeof(*parameters), &memory);
    if (status != TA_OK) return status;
    parameters = (ta_parameter_t *)memory;
    for (i = 0u; i < source->parameter_count; ++i) {
        parameters[i] = (ta_parameter_t){0};
        if (!tai_text_valid(source->parameters[i].name, 0u)) return TA_INVALID;
        status = tai_text_copy(scope->arena, source->parameters[i].name, &parameters[i].name);
        if (status != TA_OK) return status;
        status = tai_value_snapshot(scope, source->parameters[i].value, &parameters[i].value);
        if (status != TA_OK) return status;
    }
    out->kind = TA_BODY_PARAMETERS;
    out->parameters = parameters;
    out->parameter_count = source->parameter_count;
    return TA_OK;
}

ta_status_t tai_trace_copy(const tai_value_scope_t *scope,
                           const ta_trace_context_t *source,
                           const ta_trace_context_t **out)
{
    ta_trace_context_t *copy;
    size_t total;
    ta_status_t status;
    void *memory;
    if (out == NULL) return TA_INVALID;
    *out = NULL;
    if (source == NULL) return TA_OK;
    if (scope == NULL || scope->system == NULL || scope->arena == NULL) return TA_INVALID;
    if (source->tracestate.data == NULL && source->tracestate.size != 0u) return TA_INVALID;
    if (source->baggage.data == NULL && source->baggage.size != 0u) return TA_INVALID;
    if (source->tracestate.size > (size_t)-1 - source->baggage.size) return TA_LIMIT;
    total = source->tracestate.size + source->baggage.size;
    if (total > scope->system->config.limits.trace_metadata_bytes) return TA_LIMIT;
    status = tai_alloc(scope->arena, sizeof(*copy), &memory);
    if (status != TA_OK) return status;
    copy = (ta_trace_context_t *)memory;
    *copy = *source;
    status = tai_text_copy(scope->arena, source->tracestate, &copy->tracestate);
    if (status != TA_OK) return status;
    status = tai_text_copy(scope->arena, source->baggage, &copy->baggage);
    if (status != TA_OK) return status;
    *out = copy;
    return TA_OK;
}
