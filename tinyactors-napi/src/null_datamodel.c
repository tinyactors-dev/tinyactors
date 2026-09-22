#include "null_datamodel.h"

#include <string.h>

static ta_op_status tinyactors_napi_null_unsupported(ta_fault *fault, const char *message) {
    if (fault != NULL) {
        *fault = (ta_fault){.code = TA_ERR_UNSUPPORTED,
                            .message = {.ptr = message, .len = (uint32_t)strlen(message)}};
    }
    return TA_OP_ERROR;
}

static ta_result tinyactors_napi_null_prepare(
    void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    if (params == NULL || params->shared == NULL || params->allocator.v == NULL ||
        (params->expressions == NULL && params->expression_count != 0)) {
        return TA_ERR_INVALID;
    }
    if (params->expression_count != 0) {
        return TA_ERR_UNSUPPORTED;
    }
    *params->shared = NULL;
    return TA_OK;
}

static void tinyactors_napi_null_destroy_definition(void *self, void *shared,
                                                     ta_allocator allocator) {
    (void)self;
    (void)shared;
    (void)allocator;
}

static uint32_t tinyactors_napi_null_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 0;
}

static uint32_t tinyactors_napi_null_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 1;
}

static ta_result tinyactors_napi_null_init_instance(
    void *self, const ta_datamodel_init_instance_params *params) {
    (void)self;
    return params != NULL && params->binding != NULL && params->binding->v != NULL
        ? TA_ERR_UNSUPPORTED : TA_OK;
}

static void tinyactors_napi_null_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    (void)instance;
}

#define TINYACTORS_NAPI_NULL_UNSUPPORTED(name, type, message) \
    static ta_op_status name(void *self, const type *params) { \
        (void)self; \
        return tinyactors_napi_null_unsupported(params == NULL ? NULL : params->fault, message); \
    }

TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_declare,
                                 ta_datamodel_declare_params,
                                 "the null data model has no data bindings")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_set_named,
                                 ta_datamodel_set_named_params,
                                 "the null data model has no data bindings")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_eval_condition,
                                 ta_datamodel_eval_condition_params,
                                 "the null data model has no conditions")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_eval_value,
                                 ta_datamodel_eval_value_params,
                                 "the null data model has no values")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_read_location,
                                 ta_datamodel_read_location_params,
                                 "the null data model has no locations")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_assign,
                                 ta_datamodel_assign_params,
                                 "the null data model has no assignments")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_script,
                                 ta_datamodel_execute_script_params,
                                 "the null data model has no scripts")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_params,
                                 ta_datamodel_make_params_params,
                                 "the null data model has no values")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_value_text,
                                 ta_datamodel_value_text_params,
                                 "the null data model has no values")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_text_value,
                                 ta_datamodel_text_value_params,
                                 "the null data model has no values")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_content,
                                 ta_datamodel_import_content_params,
                                 "the null data model cannot import content")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_foreach_snapshot,
                                 ta_datamodel_foreach_snapshot_params,
                                 "the null data model has no collections")
TINYACTORS_NAPI_NULL_UNSUPPORTED(tinyactors_napi_null_foreach_bind,
                                 ta_datamodel_foreach_bind_params,
                                 "the null data model has no collections")

/* `inspect` is deliberately absent: there is no data root to expose. */
static const ta_datamodel_vtable TINYACTORS_NAPI_NULL_VTABLE = {
    .name = {.ptr = "null", .len = 4},
    .prepare_definition = tinyactors_napi_null_prepare,
    .destroy_definition = tinyactors_napi_null_destroy_definition,
    .instance_size = tinyactors_napi_null_instance_size,
    .instance_alignment = tinyactors_napi_null_instance_alignment,
    .init_instance = tinyactors_napi_null_init_instance,
    .destroy_instance = tinyactors_napi_null_destroy_instance,
    .declare = tinyactors_napi_null_declare,
    .set_named = tinyactors_napi_null_set_named,
    .eval_condition = tinyactors_napi_null_eval_condition,
    .eval_value = tinyactors_napi_null_eval_value,
    .read_location = tinyactors_napi_null_read_location,
    .assign = tinyactors_napi_null_assign,
    .execute_script = tinyactors_napi_null_script,
    .make_params = tinyactors_napi_null_params,
    .value_text = tinyactors_napi_null_value_text,
    .text_value = tinyactors_napi_null_text_value,
    .import_content = tinyactors_napi_null_content,
    .foreach_snapshot = tinyactors_napi_null_foreach_snapshot,
    .foreach_bind = tinyactors_napi_null_foreach_bind,
};

ta_datamodel tinyactors_napi_null_datamodel(void) {
    return (ta_datamodel){.self = NULL, .v = &TINYACTORS_NAPI_NULL_VTABLE};
}
