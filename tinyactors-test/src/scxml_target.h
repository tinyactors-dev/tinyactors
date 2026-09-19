#pragma once

#include "tinyactors.h"

/*
 * Parses an SCXML Event I/O Processor target URI: #_internal, #_self,
 * #_parent, #_scxml_<actor id>, or #_<invoke id>. On failure returns false
 * and sets *error to a static message.
 */
bool tat_scxml_target_parse(ta_str target, ta_scxml_target *out, const char **error);
