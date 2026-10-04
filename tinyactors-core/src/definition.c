#include "definition.h"
#include "layout.h"
#include "interpreter.h"
#include "system.h"

void ta_definition_destroy(ta_definition *definition) {
    if (definition == NULL) {
        return;
    }
    if (!definition->registered && definition->system != NULL) {
        ta_system *system = definition->system;
        if (definition->unregistered_previous != NULL) {
            definition->unregistered_previous->unregistered_next = definition->unregistered_next;
        } else if (system->unregistered_definitions == definition) {
            system->unregistered_definitions = definition->unregistered_next;
        }
        if (definition->unregistered_next != NULL) {
            definition->unregistered_next->unregistered_previous = definition->unregistered_previous;
        }
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
    if (definition->spans != NULL) {
        ta_mem_free(definition->span_allocator, definition->spans,
                    &(ta_allocator_free_params){ .size = definition->span_bytes,
                                                 .alignment = _Alignof(ta_source_span) });
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

/*
 * Matches an event against one descriptor (ADR 0060 section 5). A `*`
 * segment matches zero or more event segments, anywhere in the descriptor;
 * a descriptor used up before the event is a prefix match. Two pointers,
 * no recursion: on a mismatch the last `*` passed takes one more event
 * segment and matching resumes after it. O(event x descriptor) time, O(1)
 * space.
 */
typedef struct {
    const ta_atom *event;
    uint32_t event_count;
    const ta_event_pattern_segment *pattern;
    uint32_t pattern_count;
} ta_pattern_match;

static bool ta_pattern_match_run(const ta_pattern_match *match) {
    uint32_t event = 0;
    uint32_t pattern = 0;
    bool starred = false;
    uint32_t star_pattern = 0;
    uint32_t star_event = 0;
    while (pattern < match->pattern_count) {
        const ta_event_pattern_segment *segment = &match->pattern[pattern];
        if (segment->flags & TA_EVENT_PATTERN_SEGMENT_WILDCARD) {
            starred = true;
            star_pattern = pattern;
            star_event = event;
            pattern += 1u;
        } else if (event < match->event_count && match->event[event] == segment->atom) {
            event += 1u;
            pattern += 1u;
        } else if (starred && star_event < match->event_count) {
            star_event += 1u;
            event = star_event;
            pattern = star_pattern + 1u;
        } else {
            return false;
        }
    }
    return true;
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
    return ta_pattern_match_run(&match);
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

ta_result ta_definition_content(const ta_system *system, const ta_definition_content_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL || params->content >= definition->content_count) {
        return TA_ERR_NOT_FOUND;
    }
    ta_interpreter_literal_content(definition, params->content, &params->out->media_type, &params->out->bytes);
    return TA_OK;
}

ta_result ta_definition_find_state(const ta_system *system, const ta_definition_find_state_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_atom name = 0;
    if (ta_atom_find(&system->atoms, params->id, &name) != TA_OK) {
        return TA_ERR_NOT_FOUND;
    }
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (definition->states[state].name == name) {
            *params->out = state;
            return TA_OK;
        }
    }
    return TA_ERR_NOT_FOUND;
}

/* Places the actions of one block. */
static void ta_place_block(ta_definition *definition, ta_span32 block, ta_action_place place) {
    for (uint32_t index = 0; index < block.length; index += 1u) {
        place.index = index;
        definition->action_places[block.offset + index] = place;
    }
}

ta_result ta_definition_place_actions(ta_definition *definition) {
    definition->action_places = NULL;
    if (definition->action_count == 0) {
        return TA_OK;
    }
    definition->action_places = ta_arena_alloc(&definition->arena,
                                               (size_t)definition->action_count * sizeof(ta_action_place),
                                               _Alignof(ta_action_place));
    if (definition->action_places == NULL) {
        return TA_ERR_NOMEM;
    }
    /* The top-level blocks: handlers, transitions, <finalize>, the <script>. */
    for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
        const ta_state_def *def = &definition->states[state];
        for (uint32_t handler = 0; handler < def->onentry_handlers.length; handler += 1u) {
            ta_place_block(definition, definition->handlers[def->onentry_handlers.offset + handler],
                           (ta_action_place){ .block = TA_ACTION_IN_ONENTRY, .owner = state, .block_index = handler });
        }
        for (uint32_t handler = 0; handler < def->onexit_handlers.length; handler += 1u) {
            ta_place_block(definition, definition->handlers[def->onexit_handlers.offset + handler],
                           (ta_action_place){ .block = TA_ACTION_IN_ONEXIT, .owner = state, .block_index = handler });
        }
    }
    for (ta_transition_id transition = 0; transition < definition->transition_count; transition += 1u) {
        ta_place_block(definition, definition->transitions[transition].actions,
                       (ta_action_place){ .block = TA_ACTION_IN_TRANSITION, .owner = transition });
    }
    for (uint32_t initial = 0; initial < definition->initial_transition_count; initial += 1u) {
        ta_place_block(definition, definition->initial_transitions[initial].actions,
                       (ta_action_place){ .block = TA_ACTION_IN_TRANSITION,
                                          .owner = definition->transition_count + initial });
    }
    for (uint32_t invoke = 0; invoke < definition->invoke_decl_count; invoke += 1u) {
        /* Which <invoke> of its state: the declarations of a state are in document order. */
        uint32_t nth = 0;
        for (uint32_t earlier = 0; earlier < invoke; earlier += 1u) {
            nth += definition->invokes[earlier].owner == definition->invokes[invoke].owner ? 1u : 0u;
        }
        ta_place_block(definition, definition->invokes[invoke].finalize_actions,
                       (ta_action_place){ .block = TA_ACTION_IN_FINALIZE, .owner = definition->invokes[invoke].owner,
                                          .block_index = nth });
    }
    ta_place_block(definition, definition->global_script_actions,
                   (ta_action_place){ .block = TA_ACTION_IN_SCRIPT, .owner = TA_STATE_NONE });
    /* The blocks nested in <if> and <foreach>, whose owner is an action. */
    for (ta_action_id action = 0; action < definition->action_count; action += 1u) {
        const ta_action_def *def = &definition->action_defs[action];
        if (def->kind == TA_ACTION_IF) {
            ta_place_block(definition, def->as.conditional.then_actions,
                           (ta_action_place){ .block = TA_ACTION_IN_THEN, .owner = action });
            ta_place_block(definition, def->as.conditional.else_actions,
                           (ta_action_place){ .block = TA_ACTION_IN_ELSE, .owner = action });
        } else if (def->kind == TA_ACTION_FOREACH) {
            ta_place_block(definition, def->as.foreach.body_actions,
                           (ta_action_place){ .block = TA_ACTION_IN_FOREACH, .owner = action });
        }
    }
    return TA_OK;
}

ta_result ta_definition_transition(const ta_system *system, const ta_definition_transition_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_transition_id id = params->transition;
    if (id < definition->transition_count) {
        const ta_transition_def *def = &definition->transitions[id];
        *params->out = (ta_transition_info){
            .origin = TA_TRANSITION_AUTHORED,
            .source = def->source,
            .type = def->type == TA_TRANSITION_INTERNAL ? TA_SCXML_TRANSITION_INTERNAL : TA_SCXML_TRANSITION_EXTERNAL,
            .event = def->event_text == TA_AUTHORED_TEXT_NONE ? (ta_str){ .ptr = "", .len = 0 }
                                                                : definition->authored_texts[def->event_text],
            .targets = def->targets.length == 0 ? NULL : definition->targets + def->targets.offset,
            .target_count = def->targets.length,
            .condition = def->condition,
            .action_count = def->actions.length,
            .document_order = def->document_order,
        };
        return TA_OK;
    }
    /* After the authored transitions come the initial ones (see the header). */
    uint32_t initial = id - definition->transition_count;
    if (initial >= definition->initial_transition_count) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_initial_transition_def *def = &definition->initial_transitions[initial];
    bool bootstrap = initial == definition->root_initial;
    *params->out = (ta_transition_info){
        .origin = bootstrap ? TA_TRANSITION_BOOTSTRAP : TA_TRANSITION_INITIAL,
        .source = bootstrap ? TA_STATE_NONE : def->owner,
        .type = TA_SCXML_TRANSITION_EXTERNAL,
        .event = { .ptr = "", .len = 0 },
        .targets = def->targets.length == 0 ? NULL : definition->targets + def->targets.offset,
        .target_count = def->targets.length,
        .default_targets = !def->authored_targets,
        .condition = TA_EXPR_ID_NONE,
        .action_count = def->actions.length,
    };
    return TA_OK;
}

static ta_scxml_action_kind ta_public_action_kind(ta_action_kind kind) {
    switch (kind) {
        case TA_ACTION_RAISE: return TA_SCXML_ACTION_RAISE;
        case TA_ACTION_SEND: return TA_SCXML_ACTION_SEND;
        case TA_ACTION_CANCEL: return TA_SCXML_ACTION_CANCEL;
        case TA_ACTION_ASSIGN: return TA_SCXML_ACTION_ASSIGN;
        case TA_ACTION_SCRIPT: return TA_SCXML_ACTION_SCRIPT;
        case TA_ACTION_LOG: return TA_SCXML_ACTION_LOG;
        case TA_ACTION_IF: return TA_SCXML_ACTION_IF;
        case TA_ACTION_FOREACH: return TA_SCXML_ACTION_FOREACH;
        case TA_ACTION_INVOKE: return TA_SCXML_ACTION_INVOKE;
    }
    return TA_SCXML_ACTION_NONE;
}

ta_result ta_definition_action(const ta_system *system, const ta_definition_action_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL || params->action >= definition->action_count) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_action_place *place = &definition->action_places[params->action];
    *params->out = (ta_action_info){
        .kind = ta_public_action_kind(definition->action_defs[params->action].kind),
        .block = place->block,
        .owner = place->owner,
        .block_index = place->block_index,
        .index = place->index,
    };
    return TA_OK;
}

ta_result ta_definition_expression(const ta_system *system, const ta_definition_expression_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL || params->expression >= definition->expression_count) {
        return TA_ERR_NOT_FOUND;
    }
    *params->out = definition->expressions[params->expression];
    return TA_OK;
}

uint32_t ta_definition_element_count(const ta_definition *definition, ta_element_kind kind) {
    switch (kind) {
        case TA_ELEMENT_STATE: return definition->state_count;
        case TA_ELEMENT_TRANSITION: return definition->transition_count + definition->initial_transition_count;
        case TA_ELEMENT_ACTION: return definition->action_count;
        case TA_ELEMENT_EXPRESSION: return definition->expression_count;
    }
    return 0;
}

uint32_t ta_definition_span_base(const ta_definition *definition, ta_element_kind kind) {
    uint32_t base = 0;
    for (uint32_t before = TA_ELEMENT_STATE; before < (uint32_t)kind; before += 1u) {
        base += ta_definition_element_count(definition, (ta_element_kind)before);
    }
    return base;
}

ta_result ta_definition_span(const ta_system *system, const ta_definition_span_params *params) {
    if (system == NULL || params == NULL || params->out == NULL || (uint32_t)params->kind > TA_ELEMENT_EXPRESSION) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL || definition->spans == NULL ||
        params->id >= ta_definition_element_count(definition, params->kind)) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_source_span *span = &definition->spans[ta_definition_span_base(definition, params->kind) + params->id];
    if (span->line == 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_str source = { .ptr = "", .len = 0 };
    if (span->source < definition->source_count) {
        source = definition->sources[span->source].name;
    }
    *params->out = (ta_source_location){ .span = *span, .source = source };
    return TA_OK;
}

/* Whether two definitions' span tables say the same, names included. */
static bool ta_definition_same_spans(const ta_definition *left, const ta_definition *right) {
    if (left->spans == NULL || left->span_count != right->span_count || left->source_count != right->source_count) {
        return false;
    }
    for (uint32_t index = 0; index < left->span_count; index += 1u) {
        const ta_source_span *a = &left->spans[index];
        const ta_source_span *b = &right->spans[index];
        if (a->offset != b->offset || a->length != b->length || a->source != b->source || a->line != b->line ||
            a->column != b->column || a->end_line != b->end_line || a->end_column != b->end_column) {
            return false;
        }
    }
    for (uint32_t index = 0; index < left->source_count; index += 1u) {
        if (!ta_str_equal(left->sources[index].name, right->sources[index].name)) {
            return false;
        }
    }
    return true;
}

void ta_definition_take_spans(ta_definition *live, ta_definition *definition) {
    /* Unchanged spans stay where they are, so what a caller borrowed of
     * them (an export, a source's name) stays valid. */
    if (definition->spans == NULL || ta_definition_same_spans(live, definition)) {
        return;
    }
    ta_source_span *spans = live->spans;
    uint32_t span_count = live->span_count;
    ta_scxml_source *sources = live->sources;
    uint32_t source_count = live->source_count;
    size_t span_bytes = live->span_bytes;
    ta_allocator span_allocator = live->span_allocator;
    live->spans = definition->spans;
    live->span_count = definition->span_count;
    live->sources = definition->sources;
    live->source_count = definition->source_count;
    live->span_bytes = definition->span_bytes;
    live->span_allocator = definition->span_allocator;
    definition->spans = spans;
    definition->span_count = span_count;
    definition->sources = sources;
    definition->source_count = source_count;
    definition->span_bytes = span_bytes;
    definition->span_allocator = span_allocator;
}

ta_result ta_definition_session_bytes(const ta_definition *definition, size_t *bytes) {
    /* The configuration, the late-binding bits (sized like it), and history. */
    size_t total;
    if (!ta_checked_mul((size_t)definition->configuration.bytes, 2u, &total) ||
        !ta_checked_add(total, (size_t)definition->history.storage_bytes, &total)) {
        return TA_ERR_NOMEM;
    }
    *bytes = total;
    return TA_OK;
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
        .transition_count = definition->transition_count + definition->initial_transition_count,
        .action_count = definition->action_count,
        .expression_count = definition->expression_count,
        .content_count = definition->content_count,
        .invoke_count = definition->invoke_decl_count,
        .retired = definition->retired,
        .actor_count = definition->actor_count,
        .holds = definition->holds,
        .holders = definition->holders,
        .actor_fixed_bytes = definition->actor_block_bytes,
        .definition_bytes = ta_arena_allocated_bytes(&definition->arena) + definition->span_bytes,
    };
    return TA_OK;
}
