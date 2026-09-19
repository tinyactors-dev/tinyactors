#include "export.h"
#include "system.h"

/*
 * Rebuilds the author-level ta_scxml_spec of a compiled definition. The
 * walk mirrors the compiler (builder.c) so that recompiling the result hashes
 * the same input in the same order: every list is rebuilt in the order the
 * compiler recorded it, and input the compiler normalizes comes from the
 * authored text the definition keeps (delay literals, transition event
 * descriptors, authored initial targets, the <scxml> binding and datamodel).
 */
typedef struct {
    const ta_system *system;
    const ta_definition *definition;
    ta_arena *arena;
    /* The first failure; later allocations are skipped. */
    ta_result result;
} ta_exporter;

/* Non-null text for an empty literal, which the RPC codecs keep apart from absent. */
static const char ta_export_empty[] = "";

/*
 * Allocates `count` zeroed list elements plus the zero terminator. An empty
 * list is null.
 */
static void *ta_export_list(ta_exporter *exporter, const ta_tree_list_layout *layout, uint32_t count) {
    if (count == 0 || exporter->result != TA_OK) {
        return NULL;
    }
    size_t size = layout->element_size * ((size_t)count + 1u);
    void *items = ta_arena_alloc(exporter->arena, size, layout->element_alignment);
    if (items == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return NULL;
    }
    ta_mem_zero(items, size);
    return items;
}

static ta_str ta_export_atom(const ta_exporter *exporter, ta_atom atom) {
    ta_str text = ta_atom_text(&exporter->system->atoms, atom);
    return text.ptr == NULL ? (ta_str){ .ptr = ta_export_empty, .len = 0 } : text;
}

static ta_str ta_export_state_id(const ta_exporter *exporter, ta_state_id state) {
    return ta_export_atom(exporter, exporter->definition->states[state].name);
}

static ta_str ta_export_authored(const ta_exporter *exporter, ta_authored_text_id text) {
    if (text == TA_AUTHORED_TEXT_NONE) {
        return (ta_str){0};
    }
    return exporter->definition->authored_texts[text];
}

/* A dotted event name. The compiler rejects empty segments, so joining them restores the text. */
static ta_str ta_export_event_name(ta_exporter *exporter, ta_event_name_id name) {
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_event_name_segments(&exporter->system->event_names, name, segments);
    if (count == 1) {
        return ta_export_atom(exporter, segments[0]);
    }
    size_t length = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        length += ta_atom_text(&exporter->system->atoms, segments[index]).len + (index == 0 ? 0u : 1u);
    }
    char *text = exporter->result == TA_OK ? ta_arena_alloc(exporter->arena, length + 1u, 1) : NULL;
    if (text == NULL) {
        exporter->result = exporter->result == TA_OK ? TA_ERR_NOMEM : exporter->result;
        return (ta_str){ .ptr = ta_export_empty, .len = 0 };
    }
    size_t at = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        ta_str segment = ta_atom_text(&exporter->system->atoms, segments[index]);
        if (index != 0) {
            text[at++] = '.';
        }
        if (segment.len != 0) {
            ta_mem_copy(text + at, segment.ptr, segment.len);
        }
        at += segment.len;
    }
    text[at] = '\0';
    return (ta_str){ .ptr = text, .len = (uint32_t)length };
}

static ta_expression ta_export_expression(const ta_exporter *exporter, ta_expr_id expr) {
    return exporter->definition->expressions[expr];
}

static ta_bytes ta_export_content(const ta_exporter *exporter, ta_content_id content) {
    ta_bytes bytes = exporter->definition->contents[content].bytes;
    return bytes.ptr == NULL ? (ta_bytes){ .ptr = (const uint8_t *)ta_export_empty, .len = 0 } : bytes;
}

static ta_scxml_value_spec ta_export_value(const ta_exporter *exporter, const ta_value_source *source) {
    ta_scxml_value_spec out = {0};
    switch (source->kind) {
        case TA_VALUE_SOURCE_EXPR:
            out.kind = TA_SCXML_VALUE_EXPR;
            out.as.expr = ta_export_expression(exporter, source->as.expr);
            break;
        case TA_VALUE_SOURCE_LOCATION:
            out.kind = TA_SCXML_VALUE_LOCATION;
            out.as.location = ta_export_expression(exporter, source->as.location);
            break;
        case TA_VALUE_SOURCE_CONTENT:
            out.kind = TA_SCXML_VALUE_CONTENT;
            out.as.content = ta_export_content(exporter, source->as.content);
            break;
        default:
            break;
    }
    return out;
}

/* A `type` / `typeexpr` or `sendid` / `sendidexpr` pair. */
static ta_scxml_string_spec ta_export_text_source(const ta_exporter *exporter, const ta_text_source *source) {
    ta_scxml_string_spec out = {0};
    if (source->kind == TA_TEXT_SOURCE_ATOM) {
        out.kind = TA_SCXML_STRING_LITERAL;
        out.as.literal = ta_export_atom(exporter, source->as.atom);
    } else if (source->kind == TA_TEXT_SOURCE_EXPR) {
        out.kind = TA_SCXML_STRING_EXPR;
        out.as.expr = ta_export_expression(exporter, source->as.expr);
    }
    return out;
}

/* A send `target` or invoke `src`: the compiler stores literal text as content. */
static ta_scxml_string_spec ta_export_literal_source(const ta_exporter *exporter, const ta_value_source *source) {
    ta_scxml_string_spec out = {0};
    if (source->kind == TA_VALUE_SOURCE_CONTENT) {
        ta_bytes bytes = ta_export_content(exporter, source->as.content);
        out.kind = TA_SCXML_STRING_LITERAL;
        out.as.literal = (ta_str){ .ptr = (const char *)bytes.ptr, .len = bytes.len };
    } else if (source->kind == TA_VALUE_SOURCE_EXPR) {
        out.kind = TA_SCXML_STRING_EXPR;
        out.as.expr = ta_export_expression(exporter, source->as.expr);
    }
    return out;
}

static ta_scxml_id_spec ta_export_id(const ta_exporter *exporter, const ta_id_source *source) {
    ta_scxml_id_spec out = {0};
    if (source->kind == TA_ID_SOURCE_LITERAL) {
        out.kind = TA_SCXML_ID_LITERAL;
        out.as.literal = ta_export_atom(exporter, source->as.literal);
    } else if (source->kind == TA_ID_SOURCE_LOCATION) {
        out.kind = TA_SCXML_ID_LOCATION;
        out.as.location = ta_export_expression(exporter, source->as.location);
    }
    return out;
}

static const ta_scxml_param_spec *ta_export_params(ta_exporter *exporter, ta_span32 span) {
    ta_scxml_param_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_param_spec), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        const ta_param_def *param = &exporter->definition->params[span.offset + index];
        items[index] = (ta_scxml_param_spec){
            .name = ta_export_atom(exporter, param->name),
            .value = ta_export_value(exporter, &param->value),
        };
    }
    return items;
}

static const ta_scxml_action_spec *ta_export_actions(ta_exporter *exporter, ta_span32 block);

static void ta_export_send(ta_exporter *exporter, const ta_send_action *send, ta_scxml_send_spec *out) {
    out->id = ta_export_id(exporter, &send->id);
    out->type = ta_export_text_source(exporter, &send->type);
    if (send->event.kind == TA_EVENT_SOURCE_EXPR) {
        out->event.kind = TA_SCXML_EVENT_EXPR;
        out->event.as.expr = ta_export_expression(exporter, send->event.as.expr);
    } else {
        out->event.kind = TA_SCXML_EVENT_NAME;
        out->event.as.name = ta_export_event_name(exporter, send->event.as.name);
    }
    out->target = ta_export_literal_source(exporter, &send->target);
    if (send->delay.kind == TA_DELAY_SOURCE_LITERAL) {
        out->delay.kind = TA_SCXML_STRING_LITERAL;
        out->delay.as.literal = ta_export_authored(exporter, send->delay.text);
    } else if (send->delay.kind == TA_DELAY_SOURCE_EXPR) {
        out->delay.kind = TA_SCXML_STRING_EXPR;
        out->delay.as.expr = ta_export_expression(exporter, send->delay.as.expr);
    }
    out->data = ta_export_value(exporter, &send->data);
    out->params = ta_export_params(exporter, send->params);
}

static void ta_export_action(ta_exporter *exporter, const ta_action_def *action, ta_scxml_action_spec *out) {
    switch (action->kind) {
        case TA_ACTION_RAISE:
            out->kind = TA_SCXML_ACTION_RAISE;
            out->as.raise.event = ta_export_event_name(exporter, action->as.raise.event);
            out->as.raise.data = ta_export_value(exporter, &action->as.raise.data);
            break;
        case TA_ACTION_SEND:
            out->kind = TA_SCXML_ACTION_SEND;
            ta_export_send(exporter, &action->as.send, &out->as.send);
            break;
        case TA_ACTION_CANCEL:
            out->kind = TA_SCXML_ACTION_CANCEL;
            out->as.cancel.sendid = ta_export_text_source(exporter, &action->as.cancel.sendid);
            break;
        case TA_ACTION_ASSIGN:
            out->kind = TA_SCXML_ACTION_ASSIGN;
            out->as.assign.location = ta_export_expression(exporter, action->as.assign.location);
            out->as.assign.value = ta_export_value(exporter, &action->as.assign.value);
            break;
        case TA_ACTION_SCRIPT:
            out->kind = TA_SCXML_ACTION_SCRIPT;
            out->as.script.script = ta_export_expression(exporter, action->as.script);
            break;
        case TA_ACTION_LOG:
            out->kind = TA_SCXML_ACTION_LOG;
            if (action->as.log.has_label) {
                out->as.log.label = ta_export_atom(exporter, action->as.log.label);
            }
            if (action->as.log.has_expr) {
                out->as.log.expr = ta_export_expression(exporter, action->as.log.expr);
            }
            break;
        case TA_ACTION_IF:
            out->kind = TA_SCXML_ACTION_IF;
            out->as.conditional.condition = ta_export_expression(exporter, action->as.conditional.condition);
            out->as.conditional.then_actions = ta_export_actions(exporter, action->as.conditional.then_actions);
            out->as.conditional.else_actions = ta_export_actions(exporter, action->as.conditional.else_actions);
            break;
        case TA_ACTION_FOREACH:
            out->kind = TA_SCXML_ACTION_FOREACH;
            out->as.foreach.array = ta_export_expression(exporter, action->as.foreach.array);
            out->as.foreach.item = ta_export_atom(exporter, action->as.foreach.item);
            if (action->as.foreach.has_index) {
                out->as.foreach.index = ta_export_atom(exporter, action->as.foreach.index);
            }
            out->as.foreach.body = ta_export_actions(exporter, action->as.foreach.body_actions);
            break;
        default:
            /* The compiler accepts neither <invoke> nor extensions as executable content. */
            exporter->result = exporter->result == TA_OK ? TA_ERR_UNSUPPORTED : exporter->result;
            break;
    }
}

static const ta_scxml_action_spec *ta_export_actions(ta_exporter *exporter, ta_span32 block) {
    ta_scxml_action_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_action_spec), block.length);
    for (uint32_t index = 0; items != NULL && index < block.length; index += 1) {
        ta_export_action(exporter, &exporter->definition->action_defs[block.offset + index], &items[index]);
    }
    return items;
}

/* One <onentry>/<onexit> handler per element; an empty handler keeps a non-null, empty action list. */
static const ta_scxml_handler_spec *ta_export_handlers(ta_exporter *exporter, ta_span32 span) {
    ta_scxml_handler_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_handler_spec), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        ta_span32 block = exporter->definition->handlers[span.offset + index];
        const ta_scxml_action_spec *actions = ta_export_actions(exporter, block);
        if (actions == NULL && exporter->result == TA_OK) {
            actions = ta_arena_alloc(exporter->arena, sizeof(ta_scxml_action_spec), _Alignof(ta_scxml_action_spec));
            if (actions == NULL) {
                exporter->result = TA_ERR_NOMEM;
            } else {
                ta_mem_zero((void *)actions, sizeof(ta_scxml_action_spec));
            }
        }
        items[index].actions = actions;
    }
    return items;
}

static const ta_str *ta_export_targets(ta_exporter *exporter, ta_span32 span) {
    ta_str *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_str), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        items[index] = ta_export_state_id(exporter, exporter->definition->targets[span.offset + index]);
    }
    return items;
}

/* Null when the author wrote neither targets nor actions: the compiler then takes the first child. */
static const ta_scxml_initial_spec *ta_export_initial(ta_exporter *exporter, ta_initial_transition_id id) {
    if (id == TA_INITIAL_TRANSITION_NONE || exporter->result != TA_OK) {
        return NULL;
    }
    const ta_initial_transition_def *initial = &exporter->definition->initial_transitions[id];
    if (!initial->authored_targets && initial->actions.length == 0) {
        return NULL;
    }
    ta_scxml_initial_spec *out = ta_arena_alloc(exporter->arena, sizeof(*out), _Alignof(ta_scxml_initial_spec));
    if (out == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return NULL;
    }
    *out = (ta_scxml_initial_spec){
        .targets = initial->authored_targets ? ta_export_targets(exporter, initial->targets) : NULL,
        .actions = ta_export_actions(exporter, initial->actions),
    };
    return out;
}

static const ta_scxml_transition_spec *ta_export_transitions(ta_exporter *exporter, ta_span32 span) {
    ta_scxml_transition_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_transition_spec), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        const ta_transition_def *transition = &exporter->definition->transitions[span.offset + index];
        items[index] = (ta_scxml_transition_spec){
            .kind = transition->type == TA_TRANSITION_INTERNAL ? TA_SCXML_TRANSITION_INTERNAL
                                                                : TA_SCXML_TRANSITION_EXTERNAL,
            .event = ta_export_authored(exporter, transition->event_text),
            .targets = ta_export_targets(exporter, transition->targets),
            .actions = ta_export_actions(exporter, transition->actions),
        };
        if (transition->condition != UINT32_MAX) {
            items[index].condition = ta_export_expression(exporter, transition->condition);
        }
    }
    return items;
}

/* The <data> children of `owner` (TA_STATE_NONE: the <scxml> element), in document order. */
static const ta_scxml_data_spec *ta_export_data(ta_exporter *exporter, ta_state_id owner) {
    const ta_definition *definition = exporter->definition;
    uint32_t count = 0;
    for (uint32_t index = 0; index < definition->data_decl_count; index += 1) {
        count += definition->data_decls[index].owner_state == owner ? 1u : 0u;
    }
    ta_scxml_data_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_data_spec), count);
    uint32_t at = 0;
    for (uint32_t index = 0; items != NULL && index < definition->data_decl_count; index += 1) {
        const ta_data_decl *decl = &definition->data_decls[index];
        if (decl->owner_state != owner) {
            continue;
        }
        items[at].name = ta_export_atom(exporter, decl->name);
        if (decl->initializer != TA_INITIALIZER_NONE) {
            items[at].initializer = ta_export_value(exporter, &definition->initializers[decl->initializer].value);
        }
        at += 1;
    }
    return items;
}

static const ta_scxml_invoke_spec *ta_export_invokes(ta_exporter *exporter, ta_state_id owner) {
    const ta_definition *definition = exporter->definition;
    uint32_t count = 0;
    for (uint32_t index = 0; index < definition->invoke_decl_count; index += 1) {
        count += definition->invokes[index].owner == owner ? 1u : 0u;
    }
    ta_scxml_invoke_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_invoke_spec), count);
    uint32_t at = 0;
    for (uint32_t index = 0; items != NULL && index < definition->invoke_decl_count; index += 1) {
        const ta_invoke_decl *invoke = &definition->invokes[index];
        if (invoke->owner != owner) {
            continue;
        }
        items[at] = (ta_scxml_invoke_spec){
            .kind = TA_SCXML_INVOKE,
            .id = ta_export_id(exporter, &invoke->id),
            .type = ta_export_text_source(exporter, &invoke->type),
            .src = ta_export_literal_source(exporter, &invoke->src),
            .data = ta_export_value(exporter, &invoke->data),
            .autoforward = invoke->autoforward,
            .params = ta_export_params(exporter, invoke->params),
            .finalize = ta_export_actions(exporter, invoke->finalize_actions),
        };
        at += 1;
    }
    return items;
}

static const ta_scxml_done_data_spec *ta_export_done_data(ta_exporter *exporter, ta_done_data_id id) {
    if (id == TA_DONE_DATA_NONE || exporter->result != TA_OK) {
        return NULL;
    }
    const ta_done_data_def *done = &exporter->definition->done_data[id];
    ta_scxml_done_data_spec *out = ta_arena_alloc(exporter->arena, sizeof(*out), _Alignof(ta_scxml_done_data_spec));
    if (out == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return NULL;
    }
    *out = (ta_scxml_done_data_spec){
        .content = ta_export_value(exporter, &done->content),
        .params = ta_export_params(exporter, done->params),
    };
    return out;
}

static ta_scxml_state_kind ta_export_state_kind(ta_state_kind kind) {
    switch (kind) {
        case TA_STATE_ATOMIC: return TA_SCXML_STATE_ATOMIC;
        case TA_STATE_COMPOUND: return TA_SCXML_STATE_COMPOUND;
        case TA_STATE_PARALLEL: return TA_SCXML_STATE_PARALLEL;
        case TA_STATE_FINAL: return TA_SCXML_STATE_FINAL;
        case TA_STATE_HISTORY_SHALLOW: return TA_SCXML_STATE_HISTORY_SHALLOW;
        case TA_STATE_HISTORY_DEEP: return TA_SCXML_STATE_HISTORY_DEEP;
        default: return TA_SCXML_STATE_NONE;
    }
}

static const ta_scxml_state_spec *ta_export_states(ta_exporter *exporter, const ta_state_id *ids, uint32_t count);

static void ta_export_state(ta_exporter *exporter, ta_state_id id, ta_scxml_state_spec *out) {
    const ta_definition *definition = exporter->definition;
    const ta_state_def *state = &definition->states[id];
    *out = (ta_scxml_state_spec){
        .kind = ta_export_state_kind(state->kind),
        .id = ta_export_state_id(exporter, id),
        .states = ta_export_states(exporter, definition->children + state->children.offset, state->children.length),
        .initial = state->kind == TA_STATE_COMPOUND ? ta_export_initial(exporter, state->initial) : NULL,
        .transitions = ta_export_transitions(exporter, state->transitions),
        .onentry = ta_export_handlers(exporter, state->onentry_handlers),
        .onexit = ta_export_handlers(exporter, state->onexit_handlers),
        .data = ta_export_data(exporter, id),
        .invokes = ta_export_invokes(exporter, id),
        .done_data = ta_export_done_data(exporter, state->done_data),
    };
}

static const ta_scxml_state_spec *ta_export_states(ta_exporter *exporter, const ta_state_id *ids, uint32_t count) {
    ta_scxml_state_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_state_spec), count);
    for (uint32_t index = 0; items != NULL && index < count; index += 1) {
        ta_export_state(exporter, ids[index], &items[index]);
    }
    return items;
}

/*
 * Top-level states in document order. The compiler numbers states in
 * pre-order and records the roots first in `children`.
 */
static const ta_scxml_state_spec *ta_export_roots(ta_exporter *exporter) {
    const ta_definition *definition = exporter->definition;
    uint32_t count = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        count += definition->states[state].parent == TA_STATE_NONE ? 1u : 0u;
    }
    return ta_export_states(exporter, definition->children, count);
}

ta_result ta_definition_export(const ta_system *system, const ta_definition_export_params *params) {
    if (system == NULL || params == NULL || params->arena == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_exporter exporter = { .system = system, .definition = definition, .arena = params->arena };
    ta_scxml_spec *spec = ta_arena_alloc(params->arena, sizeof(*spec), _Alignof(ta_scxml_spec));
    if (spec == NULL) {
        return TA_ERR_NOMEM;
    }
    *spec = (ta_scxml_spec){
        .name = definition->document_name == TA_ATOM_NONE ? (ta_str){0}
                                                          : ta_export_atom(&exporter, definition->document_name),
        .datamodel = definition->authored_datamodel,
        .identity = definition->fingerprinted ? NULL : &definition->identity,
        .binding = definition->authored_binding,
        .states = ta_export_roots(&exporter),
        .initial = ta_export_initial(&exporter, definition->root_initial),
        .data = ta_export_data(&exporter, TA_STATE_NONE),
    };
    if (definition->global_script_actions.length == 1) {
        const ta_action_def *script = &definition->action_defs[definition->global_script_actions.offset];
        spec->script = ta_export_expression(&exporter, script->as.script);
    }
    if (exporter.result != TA_OK) {
        return exporter.result;
    }
    *params->out = spec;
    return TA_OK;
}
