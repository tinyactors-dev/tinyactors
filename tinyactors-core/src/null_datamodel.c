#include "null_datamodel.h"

/*
 * The Null data model (SCXML B.1, ADR 0057). Its whole expression language
 * is the `In(state)` predicate used in a `cond`. `prepare_definition` refuses
 * a chart with any other expression, naming the expression (ADR 0058), so
 * such a chart fails when it is defined rather than on first use. The
 * remaining operations need data the profile does not have; each fails with
 * a TA_ERR_UNSUPPORTED fault, which the interpreter turns into
 * error.execution.
 *
 * Nothing is stored per actor: instance size 0. A definition with
 * expressions keeps one small record, the expression table core already
 * owns, because an expression reaches the adapter as a ta_expr_id. A
 * definition without expressions keeps nothing.
 */

typedef struct {
    const ta_expression *expressions;
    uint32_t expression_count;
} ta_null_shared;

#define TA_NULL_TEXT(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* Every operation the profile does not support answers this way. */
static ta_op_status ta_null_unsupported(ta_fault *fault, ta_str message) {
    if (fault != NULL) {
        *fault = (ta_fault){ .code = TA_ERR_UNSUPPORTED, .message = message };
    }
    return TA_OP_ERROR;
}

static bool ta_null_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static uint32_t ta_null_skip_space(ta_str source, uint32_t at) {
    while (at < source.len && ta_null_space(source.ptr[at])) {
        at += 1u;
    }
    return at;
}

/*
 * The only expression the profile has: `In(id)`, with the ID optionally
 * quoted in ' or ". Returns false when `source` is anything else.
 */
static bool ta_null_parse_in(ta_str source, ta_str *out) {
    uint32_t at = ta_null_skip_space(source, 0);
    if (source.len - at < 4u || source.ptr[at] != 'I' || source.ptr[at + 1u] != 'n') {
        return false;
    }
    at = ta_null_skip_space(source, at + 2u);
    if (at >= source.len || source.ptr[at] != '(') {
        return false;
    }
    at = ta_null_skip_space(source, at + 1u);
    char quote = 0;
    if (at < source.len && (source.ptr[at] == '\'' || source.ptr[at] == '"')) {
        quote = source.ptr[at];
        at += 1u;
    }
    uint32_t start = at;
    while (at < source.len && source.ptr[at] != ')' && source.ptr[at] != quote &&
           (quote != 0 || !ta_null_space(source.ptr[at]))) {
        at += 1u;
    }
    uint32_t end = at;
    if (quote != 0) {
        if (at >= source.len || source.ptr[at] != quote) {
            return false;
        }
        at += 1u;
    }
    at = ta_null_skip_space(source, at);
    if (at >= source.len || source.ptr[at] != ')') {
        return false;
    }
    at = ta_null_skip_space(source, at + 1u);
    if (at != source.len || end == start) {
        return false;
    }
    *out = (ta_str){ .ptr = source.ptr + start, .len = end - start };
    return true;
}

static ta_result ta_null_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    if (params == NULL || params->shared == NULL || params->allocator.v == NULL ||
        (params->expressions == NULL && params->expression_count != 0)) {
        return TA_ERR_INVALID;
    }
    *params->shared = NULL;
    for (uint32_t index = 0; index < params->expression_count; index += 1u) {
        const ta_expression *expression = &params->expressions[index];
        ta_str id = {0};
        if (expression->kind == TA_EXPRESSION_NONE) {
            continue;
        }
        if (expression->kind == TA_EXPRESSION_SOURCE && ta_null_parse_in(expression->as.source, &id)) {
            continue;
        }
        if (params->rejected != NULL) {
            *params->rejected = (ta_expression_rejection){
                .expression = index,
                .offset = 0,
                .length = expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source.len : 0u,
                .message = expression->kind == TA_EXPRESSION_SOURCE
                               ? TA_NULL_TEXT("the null data model has only the In(state) predicate")
                               : TA_NULL_TEXT("the null data model takes expressions as source text"),
            };
        }
        return expression->kind == TA_EXPRESSION_SOURCE ? TA_ERR_INVALID : TA_ERR_UNSUPPORTED;
    }
    if (params->expression_count == 0) {
        return TA_OK;
    }
    ta_null_shared *shared = ta_mem_alloc(params->allocator, sizeof(ta_null_shared), _Alignof(ta_null_shared));
    if (shared == NULL) {
        return TA_ERR_NOMEM;
    }
    *shared = (ta_null_shared){ .expressions = params->expressions, .expression_count = params->expression_count };
    *params->shared = shared;
    return TA_OK;
}

static void ta_null_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self;
    if (shared == NULL || allocator.v == NULL) {
        return;
    }
    ta_mem_free(allocator, shared,
                &(ta_allocator_free_params){ .size = sizeof(ta_null_shared), .alignment = _Alignof(ta_null_shared) });
}

/* No data: an actor of a null-model chart allocates nothing for it. */
static uint32_t ta_null_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 0;
}

static uint32_t ta_null_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 1;
}

static ta_result ta_null_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self;
    /* A binding is host data, which this profile cannot hold. */
    return params != NULL && params->binding != NULL && params->binding->v != NULL ? TA_ERR_UNSUPPORTED : TA_OK;
}

static void ta_null_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    (void)instance;
}

static ta_str ta_null_source(const ta_null_shared *shared, ta_expr_id expr) {
    if (shared == NULL || shared->expressions == NULL || expr >= shared->expression_count) {
        return (ta_str){0};
    }
    const ta_expression *expression = &shared->expressions[expr];
    return expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source : (ta_str){0};
}

static ta_op_status ta_null_eval_condition(void *self, const ta_datamodel_eval_condition_params *params) {
    (void)self;
    ta_str id = {0};
    if (!ta_null_parse_in(ta_null_source(params->shared, params->expr), &id)) {
        return ta_null_unsupported(params->fault,
                                   TA_NULL_TEXT("the null data model evaluates only the In(state) predicate"));
    }
    /* SCXML B.1: In() of a state the chart does not have is simply false;
     * core answers TA_ERR_NOT_FOUND for it (ADR 0049). */
    ta_state_id state = 0;
    *params->out = ta_dm_host_find_state(params->host, id, &state) == TA_OK && ta_dm_host_in_state(params->host, state);
    return TA_OP_OK;
}

static ta_op_status ta_null_declare(void *self, const ta_datamodel_declare_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no data bindings"));
}

static ta_op_status ta_null_set_named(void *self, const ta_datamodel_set_named_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no data bindings"));
}

static ta_op_status ta_null_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no value expressions"));
}

static ta_op_status ta_null_read_location(void *self, const ta_datamodel_read_location_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no locations"));
}

static ta_op_status ta_null_assign(void *self, const ta_datamodel_assign_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no assignment"));
}

static ta_op_status ta_null_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no scripts"));
}

static ta_op_status ta_null_make_params(void *self, const ta_datamodel_make_params_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no values to make params from"));
}

static ta_op_status ta_null_value_text(void *self, const ta_datamodel_value_text_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no values"));
}

static ta_op_status ta_null_text_value(void *self, const ta_datamodel_text_value_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no values"));
}

static ta_op_status ta_null_import_content(void *self, const ta_datamodel_import_content_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model cannot import content"));
}

static ta_op_status ta_null_foreach_snapshot(void *self, const ta_datamodel_foreach_snapshot_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no collections to iterate"));
}

static ta_op_status ta_null_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no collections to iterate"));
}

/*
 * `inspect` stays absent: the profile gives the application no data to read,
 * so ta_actor_datamodel_root() reports TA_ERR_UNSUPPORTED for these actors.
 */
static const ta_datamodel_vtable ta_null_datamodel_vtable = {
    .name = { .ptr = "null", .len = 4 },
    .prepare_definition = ta_null_prepare,
    .destroy_definition = ta_null_destroy_definition,
    .instance_size = ta_null_instance_size,
    .instance_alignment = ta_null_instance_alignment,
    .init_instance = ta_null_init_instance,
    .destroy_instance = ta_null_destroy_instance,
    .declare = ta_null_declare,
    .set_named = ta_null_set_named,
    .eval_condition = ta_null_eval_condition,
    .eval_value = ta_null_eval_value,
    .read_location = ta_null_read_location,
    .assign = ta_null_assign,
    .execute_script = ta_null_execute_script,
    .make_params = ta_null_make_params,
    .value_text = ta_null_value_text,
    .text_value = ta_null_text_value,
    .import_content = ta_null_import_content,
    .foreach_snapshot = ta_null_foreach_snapshot,
    .foreach_bind = ta_null_foreach_bind,
};

ta_datamodel ta_null_datamodel(void) {
    return (ta_datamodel){ .self = NULL, .v = &ta_null_datamodel_vtable };
}
