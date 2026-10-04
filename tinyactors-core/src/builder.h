#pragma once

#include "definition.h"
#include "../include/tinyactors.h"

/* The declarative specification tree and ta_tree are fully public. */

/* Parses an SCXML duration ("500ms", "2.5s") into logical milliseconds. */
ta_result ta_duration_parse(ta_str text, ta_time *out);
