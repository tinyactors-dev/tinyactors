#pragma once

#include "definition.h"

/*
 * ta_definition_export() rebuilds a ta_scxml_spec from a compiled definition.
 * It reads the compiled tables and the authored text the compiler keeps for
 * it (ta_definition.authored_texts and the other `authored_*` fields); see
 * docs/internal/definition-inspection.md.
 */

/*
 * The export behind ta_definition_export() and ta_definition_encode().
 * An element with a known span always indexes the spans table (ADR 0086):
 * the element of `kind` with ID `id` has `span`
 * ta_definition_span_base(kind) + id + 1. `numbered` gives every element
 * that `span`, and the spec a table even when the definition keeps none, so
 * an encode can report where each element landed.
 */
typedef struct {
    ta_definition_id definition;
    ta_arena *arena;
    bool numbered;
    const ta_scxml_spec **out;
} ta_export_request;

ta_result ta_export_spec(const ta_system *system, const ta_export_request *request);
