#include "internal.h"

typedef struct expression_root {
    ta_system_t *system;
    ta_value_t value;
} expression_root_t;

static void release_expression(void *argument)
{
    expression_root_t *root = (expression_root_t *)argument;
    if (root->value.handle != 0)
        root->system->config.values.release(root->system->config.host_user, root->value);
}

/* Adopt a successful callback root in macrostep scratch, releasing immediately
 * if its cleanup record cannot be installed. */
static ta_status_t adopt(ta_context_t *context, ta_value_t value)
{
    ta_arena_t *arena;
    expression_root_t *root;
    ta_status_t status;
    if (value.handle == 0) return TA_EXECUTION_ERROR;
    status = ta_context_arena(context, TA_LIFETIME_MACROSTEP, &arena);
    if (status == TA_OK) status = tai_alloc(arena, sizeof(*root), (void **)&root);
    if (status == TA_OK) {
        *root = (expression_root_t){context->actor->system, value};
        status = ta_arena_defer(arena, release_expression, root);
    }
    if (status != TA_OK)
        context->actor->system->config.values.release(context->actor->system->config.host_user, value);
    return status;
}

ta_status_t tai_body_eval(ta_context_t *context, const ta_body_expr_t *expression, ta_body_t *out)
{
    ta_data_model_ops_t *ops = &context->actor->system->config.data;
    ta_status_t status = TA_OK;
    uint32_t i;
    *out = (ta_body_t){0};
    out->kind = expression->kind;
    if (expression->kind == TA_BODY_NONE) return TA_OK;
    if (expression->kind == TA_BODY_CONTENT) {
        if (expression->bytes_content != 0) {
            out->content.kind = TA_PAYLOAD_BYTES;
            status = ops->evaluate_bytes(context, expression->bytes_content, &out->content.as.bytes);
        } else {
            out->content.kind = TA_PAYLOAD_NATIVE;
            status = ops->evaluate(context, expression->content, &out->content.as.native);
            if (status == TA_OK) status = adopt(context, out->content.as.native);
        }
    } else {
        ta_arena_t *arena;
        ta_parameter_t *parameters;
        tai_array_layout_t layout = {expression->parameter_count, sizeof(*parameters), TAI_ALIGNMENT};
        status = ta_context_arena(context, TA_LIFETIME_MACROSTEP, &arena);
        if (status == TA_OK) status = tai_array(arena, &layout, (void **)&parameters);
        if (status != TA_OK) return status;
        out->parameters = parameters;
        out->parameter_count = expression->parameter_count;
        for (i = 0; i < expression->parameter_count; ++i) {
            const ta_parameter_expr_t *source = &expression->parameters[i];
            parameters[i] = (ta_parameter_t){source->name,{0}};
            status = source->location != 0 ? ops->read_location(context, source->location, &parameters[i].value) :
                                            ops->evaluate(context, source->expression, &parameters[i].value);
            if (status == TA_OK) status = adopt(context, parameters[i].value);
            if (status != TA_OK) break;
        }
    }
    return status == TA_AGAIN ? TA_EXECUTION_ERROR : status;
}

ta_status_t tai_body_payload(ta_context_t *context, const ta_body_t *body, ta_payload_t *out)
{
    ta_system_t *system = context->actor->system;
    ta_status_t status;
    *out = (ta_payload_t){0};
    if (body->kind == TA_BODY_NONE) return TA_OK;
    if (body->kind == TA_BODY_CONTENT) { *out = body->content; return TA_OK; }
    if (system->config.data.pack_body == NULL) return TA_UNSUPPORTED;
    out->kind = TA_PAYLOAD_NATIVE;
    status = system->config.data.pack_body(system->config.host_user, body, &out->as.native);
    if (status == TA_OK) status = adopt(context, out->as.native);
    return status == TA_AGAIN ? TA_EXECUTION_ERROR : status;
}
