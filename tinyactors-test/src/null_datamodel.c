#include "null_datamodel.h"

#include <stdbool.h>
#include <string.h>

/*
 * The Null data model (SCXML B.1). Its whole expression language is the
 * `In(state)` predicate used in a `cond`; everything that needs data is an
 * error, reported as a fault so the interpreter turns it into
 * error.execution. Nothing is stored per actor: instance size is 0.
 *
 * The adapter keeps one small record per definition, the compiled
 * expression table, because an expression reaches it as a ta_expr_id.
 */

typedef struct {
    const ta_expression *expressions;
    uint32_t expression_count;
} tat_null_shared;

static void tat_null_fault(ta_fault *fault, const char *message) {
    if (fault == NULL) {
        return;
    }
    *fault = (ta_fault){.code = TA_ERR_UNSUPPORTED,
                        .message = {.ptr = message, .len = (uint32_t)strlen(message)}};
}

/* Every operation the profile does not support answers this way. */
static ta_op_status tat_null_unsupported(ta_fault *fault, const char *what) {
    tat_null_fault(fault, what);
    return TA_OP_ERROR;
}

static ta_result tat_null_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    if (params == NULL || params->shared == NULL || params->allocator.v == NULL ||
        (params->expressions == NULL && params->expression_count != 0)) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < params->expression_count; index += 1) {
        ta_expression_kind kind = params->expressions[index].kind;
        if (kind != TA_EXPRESSION_NONE && kind != TA_EXPRESSION_SOURCE) {
            return TA_ERR_UNSUPPORTED;
        }
    }
    tat_null_shared *shared =
        params->allocator.v->alloc(params->allocator.self, sizeof(*shared), _Alignof(tat_null_shared));
    if (shared == NULL) {
        return TA_ERR_NOMEM;
    }
    *shared = (tat_null_shared){.expressions = params->expressions, .expression_count = params->expression_count};
    *params->shared = shared;
    return TA_OK;
}

static void tat_null_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self;
    if (shared == NULL || allocator.v == NULL) {
        return;
    }
    allocator.v->free(allocator.self, &(ta_allocator_free_params){.ptr = shared,
                                                                  .size = sizeof(tat_null_shared),
                                                                  .alignment = _Alignof(tat_null_shared)});
}

/* No data: an actor of a null-model chart allocates nothing for it. */
static uint32_t tat_null_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 0;
}

static uint32_t tat_null_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 1;
}

static ta_result tat_null_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self;
    /* A binding is host data, which this profile cannot hold. */
    return params != NULL && params->binding != NULL && params->binding->v != NULL ? TA_ERR_UNSUPPORTED : TA_OK;
}

static void tat_null_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    (void)instance;
}

static ta_str tat_null_source(const tat_null_shared *shared, ta_expr_id expr) {
    if (shared == NULL || shared->expressions == NULL || expr >= shared->expression_count) {
        return (ta_str){0};
    }
    const ta_expression *expression = &shared->expressions[expr];
    return expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source : (ta_str){0};
}

static bool tat_null_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/*
 * The only expression the profile has: `In(id)`, with the ID optionally
 * quoted. Returns false when `source` is anything else, so the caller can
 * report it as unsupported rather than silently answering a condition.
 */
static bool tat_null_parse_in(ta_str source, ta_str *out) {
    uint32_t at = 0;
    while (at < source.len && tat_null_space(source.ptr[at])) {
        at += 1;
    }
    if (source.len - at < 4 || source.ptr[at] != 'I' || source.ptr[at + 1] != 'n') {
        return false;
    }
    at += 2;
    while (at < source.len && tat_null_space(source.ptr[at])) {
        at += 1;
    }
    if (at >= source.len || source.ptr[at] != '(') {
        return false;
    }
    at += 1;
    while (at < source.len && tat_null_space(source.ptr[at])) {
        at += 1;
    }
    char quote = 0;
    if (at < source.len && (source.ptr[at] == '\'' || source.ptr[at] == '"')) {
        quote = source.ptr[at];
        at += 1;
    }
    uint32_t start = at;
    while (at < source.len && source.ptr[at] != ')' && source.ptr[at] != quote &&
           (quote != 0 || !tat_null_space(source.ptr[at]))) {
        at += 1;
    }
    uint32_t end = at;
    if (quote != 0) {
        if (at >= source.len || source.ptr[at] != quote) {
            return false;
        }
        at += 1;
    }
    while (at < source.len && tat_null_space(source.ptr[at])) {
        at += 1;
    }
    if (at >= source.len || source.ptr[at] != ')') {
        return false;
    }
    at += 1;
    while (at < source.len && tat_null_space(source.ptr[at])) {
        at += 1;
    }
    if (at != source.len || end == start) {
        return false;
    }
    *out = (ta_str){.ptr = source.ptr + start, .len = end - start};
    return true;
}

/* True when a state of that author ID is in the active configuration. */
static bool tat_null_in_state(const ta_dm_host *host, ta_str id) {
    ta_state_id state = 0;
    /* SCXML B.1: In() of a state the machine does not have is simply false;
     * core answers TA_ERR_NOT_FOUND for it (ADR 0049). */
    return ta_dm_host_find_state(host, id, &state) == TA_OK && ta_dm_host_in_state(host, state);
}

static ta_op_status tat_null_eval_condition(void *self, const ta_datamodel_eval_condition_params *params) {
    (void)self;
    ta_str source = tat_null_source(params->shared, params->expr);
    ta_str id;
    if (source.ptr == NULL || !tat_null_parse_in(source, &id)) {
        return tat_null_unsupported(params->fault,
                                    "the null data model evaluates only the In(state) predicate");
    }
    *params->out = tat_null_in_state(params->host, id);
    return TA_OP_OK;
}

static ta_op_status tat_null_declare(void *self, const ta_datamodel_declare_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model has no data bindings");
}

static ta_op_status tat_null_set_named(void *self, const ta_datamodel_set_named_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model has no data bindings");
}

static ta_op_status tat_null_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model has no value expressions");
}

static ta_op_status tat_null_read_location(void *self, const ta_datamodel_read_location_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model has no locations");
}

static ta_op_status tat_null_assign(void *self, const ta_datamodel_assign_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model has no assignment");
}

static ta_op_status tat_null_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model has no scripts");
}

static ta_op_status tat_null_make_params(void *self, const ta_datamodel_make_params_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model has no values to make params from");
}

static ta_op_status tat_null_value_text(void *self, const ta_datamodel_value_text_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model has no values");
}

static ta_op_status tat_null_text_value(void *self, const ta_datamodel_text_value_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model has no values");
}

static ta_op_status tat_null_import_content(void *self, const ta_datamodel_import_content_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model cannot import content");
}

static ta_op_status tat_null_foreach_snapshot(void *self, const ta_datamodel_foreach_snapshot_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model has no collections to iterate");
}

static ta_op_status tat_null_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    (void)self;
    return tat_null_unsupported(params->fault, "the null data model has no collections to iterate");
}

/*
 * `inspect` stays absent: the profile gives the application no data to read,
 * so ta_actor_datamodel_root() reports TA_ERR_UNSUPPORTED for these actors.
 */
static const ta_datamodel_vtable TAT_NULL_DATAMODEL_VTABLE = {
    .name = {.ptr = "null", .len = 4},
    .prepare_definition = tat_null_prepare,
    .destroy_definition = tat_null_destroy_definition,
    .instance_size = tat_null_instance_size,
    .instance_alignment = tat_null_instance_alignment,
    .init_instance = tat_null_init_instance,
    .destroy_instance = tat_null_destroy_instance,
    .declare = tat_null_declare,
    .set_named = tat_null_set_named,
    .eval_condition = tat_null_eval_condition,
    .eval_value = tat_null_eval_value,
    .read_location = tat_null_read_location,
    .assign = tat_null_assign,
    .execute_script = tat_null_execute_script,
    .make_params = tat_null_make_params,
    .value_text = tat_null_value_text,
    .text_value = tat_null_text_value,
    .import_content = tat_null_import_content,
    .foreach_snapshot = tat_null_foreach_snapshot,
    .foreach_bind = tat_null_foreach_bind,
};

ta_datamodel tat_null_datamodel(void) {
    return (ta_datamodel){.self = NULL, .v = &TAT_NULL_DATAMODEL_VTABLE};
}
