#pragma once

#include <stdbool.h>

#include "json_datamodel.h"
#include "jv.h"

/* Bridges between libjq values and data-model values. from_jv consumes value;
 * to_jv returns false for values that the JSON data model did not create. */
ta_result tat_json_value_from_jv(tat_json_datamodel_context *context, jv value, ta_value *out);
bool tat_json_value_to_jv(const ta_value *value, jv *out);
