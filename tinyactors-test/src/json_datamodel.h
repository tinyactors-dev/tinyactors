#pragma once

#include <stddef.h>

#include "tinyactors.h"

typedef struct {
    ta_arena values;
    ta_arena scratch;
    void *free_values;
} tat_json_datamodel_context;

ta_result tat_json_datamodel_context_init(tat_json_datamodel_context *context,
                                          ta_allocator allocator);
void tat_json_datamodel_context_destroy(tat_json_datamodel_context *context);

/* A deterministic jq-backed data model used by the verification host. */
ta_datamodel tat_json_datamodel(tat_json_datamodel_context *context);

/* Creates and inspects JSON values accepted by the data model. */
ta_result tat_json_value_parse(tat_json_datamodel_context *context,
                               const char *json,
                               ta_value *out);
ta_result tat_json_value_format(const ta_value *value,
                                char *buffer,
                                size_t capacity,
                                size_t *needed);
