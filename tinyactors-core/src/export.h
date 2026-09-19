#pragma once

#include "definition.h"

/*
 * ta_definition_export() rebuilds a ta_scxml_spec from a compiled definition.
 * It reads the compiled tables and the authored text the compiler keeps for
 * it (ta_definition.authored_texts and the other `authored_*` fields); see
 * docs/internal/definition-inspection.md.
 */
