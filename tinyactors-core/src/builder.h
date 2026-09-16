#pragma once

#include "definition.h"

struct ta_definition_builder {
    ta_system *system;
    ta_allocator allocator;
    ta_atom name;
    void *impl;
};

ta_result ta_definition_builder_init(ta_definition_builder *builder,
                                     ta_system *system,
                                     ta_str name,
                                     ta_allocator allocator);
/* Must be called before compilation. `identity.family.name` must equal `name`. */
ta_result ta_definition_builder_set_identity(
    ta_definition_builder *builder,
    const ta_definition_identity *identity);
void ta_definition_builder_destroy(ta_definition_builder *builder);
ta_state_id ta_definition_add_state(ta_definition_builder *builder,
                                    ta_state_id parent,
                                    ta_state_kind kind,
                                    ta_str name);
ta_transition_id ta_definition_add_transition(ta_definition_builder *builder,
                                              ta_state_id source,
                                              ta_expr_id condition);
ta_result ta_definition_add_event_pattern(
    ta_definition_builder *builder,
    ta_transition_id transition,
    const ta_event_pattern_spec *pattern);
ta_result ta_definition_add_target(ta_definition_builder *builder,
                                   ta_transition_id transition,
                                   ta_state_id target);
ta_result ta_definition_compile(ta_definition_builder *builder,
                                ta_datamodel datamodel,
                                ta_definition **out);
