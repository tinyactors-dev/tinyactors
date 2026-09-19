#include "definition.h"
#include "system.h"

void ta_definition_destroy(ta_definition *definition) {
    if (definition == NULL) {
        return;
    }
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (definition->datamodel_prepared) {
        v->destroy_definition(definition->datamodel.self, definition->datamodel_shared,
                              definition->allocator);
    }
    for (uint32_t index = 0; index < definition->expression_count; index += 1) {
        ta_expression *expression = &definition->expressions[index];
        if (expression->kind == TA_EXPRESSION_HOST && expression->as.host.v->release != NULL) {
            expression->as.host.v->release(expression->as.host.self, expression->as.host.value,
                                           definition->allocator);
        }
    }
    ta_allocator allocator = definition->allocator;
    ta_arena_destroy(&definition->arena);
    ta_mem_free(allocator, definition,
                &(ta_allocator_free_params){ .size = sizeof(ta_definition),
                                             .alignment = _Alignof(ta_definition) });
}

const ta_definition *ta_definition_lookup(const ta_system *system, ta_definition_id id) {
    return ta_system_definition(system, id);
}

bool ta_definition_is_atomic(const ta_definition *definition, ta_state_id state) {
    ta_state_kind kind = definition->states[state].kind;
    return kind == TA_STATE_ATOMIC || kind == TA_STATE_FINAL;
}

bool ta_definition_is_descendant(const ta_definition *definition, ta_state_id state, ta_state_id ancestor) {
    if (ancestor == TA_STATE_NONE) {
        return state != TA_STATE_NONE;
    }
    for (ta_state_id at = definition->states[state].parent; at != TA_STATE_NONE;
         at = definition->states[at].parent) {
        if (at == ancestor) {
            return true;
        }
    }
    return false;
}

typedef struct {
    const ta_atom *event;
    uint32_t event_count;
    const ta_event_pattern_segment *pattern;
    uint32_t pattern_count;
} ta_pattern_match;

/* A wildcard matches zero or more segments; trailing event segments are a prefix match. */
static bool ta_pattern_match_from(const ta_pattern_match *match, uint32_t event_index, uint32_t pattern_index) {
    if (pattern_index == match->pattern_count) {
        return true;
    }
    const ta_event_pattern_segment *segment = &match->pattern[pattern_index];
    if (segment->flags & TA_EVENT_PATTERN_SEGMENT_WILDCARD) {
        for (uint32_t skip = event_index; skip <= match->event_count; skip += 1) {
            if (ta_pattern_match_from(match, skip, pattern_index + 1u)) {
                return true;
            }
        }
        return false;
    }
    return event_index < match->event_count && match->event[event_index] == segment->atom &&
           ta_pattern_match_from(match, event_index + 1u, pattern_index + 1u);
}

bool ta_event_pattern_matches(const ta_definition *definition,
                              const ta_event_pattern *pattern,
                              ta_event_name_id event) {
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_event_name_segments(&definition->system->event_names, event, segments);
    if (count == 0) {
        return false;
    }
    ta_pattern_match match = {
        .event = segments,
        .event_count = count,
        .pattern = definition->event_pattern_segments + pattern->segments.offset,
        .pattern_count = pattern->segments.length,
    };
    return ta_pattern_match_from(&match, 0, 0);
}

ta_result ta_definition_state(const ta_system *system, const ta_definition_state_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (params->state >= definition->state_count) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_state_def *state = &definition->states[params->state];
    *params->out = (ta_state_info){
        .id = ta_atom_text(&system->atoms, state->name),
        .parent = state->parent,
        .kind = state->kind,
    };
    return TA_OK;
}

size_t ta_definition_session_bytes(const ta_definition *definition) {
    /* The configuration, the late-binding bits (sized like it), and history. */
    return definition->configuration.bytes * 2u + definition->history.storage_bytes;
}

size_t ta_definition_instance_bytes(const ta_definition *definition) {
    return definition->datamodel_instance_size == 0 ? 1u : definition->datamodel_instance_size;
}

ta_result ta_definition_inspect(const ta_system *system,
                                ta_definition_id id,
                                ta_definition_inspection *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, id);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_str document_name = { .ptr = "", .len = 0 };
    if (definition->document_name != TA_ATOM_NONE) {
        document_name = ta_atom_text(&system->atoms, definition->document_name);
    }
    *out = (ta_definition_inspection){
        .id = definition->id,
        .identity = definition->identity,
        .document_name = document_name,
        .datamodel = definition->datamodel,
        .binding = definition->binding_mode == TA_BINDING_LATE ? TA_SCXML_BINDING_LATE : TA_SCXML_BINDING_EARLY,
        .state_count = definition->state_count,
        .retired = definition->retired,
        .actor_count = definition->actor_count,
        .actor_fixed_bytes = sizeof(ta_actor) + ta_definition_session_bytes(definition) +
                             ta_definition_instance_bytes(definition),
        .definition_bytes = ta_arena_allocated_bytes(&definition->arena),
    };
    return TA_OK;
}
