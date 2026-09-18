#include "builder.h"
#include "system.h"

/*
 * Compiles a ta_scxml_spec in two passes. Pass one assigns state IDs in
 * document (pre-)order and records child ranges; pass two resolves targets and
 * compiles transitions, initial transitions, executable content, and data.
 * Every table is built in a growable vector and finally copied into the
 * definition's arena, which owns all compiled storage.
 */
typedef struct {
    ta_system *system;
    ta_allocator allocator;
    const ta_scxml_spec *spec;
    ta_definition *definition;
    ta_vec states;       /* ta_state_def */
    ta_vec state_specs;  /* const ta_scxml_state_spec * */
    ta_vec children;     /* ta_state_id */
    ta_vec transitions;  /* ta_transition_def */
    ta_vec initials;     /* ta_initial_transition_def */
    ta_vec targets;      /* ta_state_id */
    ta_vec actions;      /* ta_action_def */
    ta_vec params;       /* ta_param_def */
    ta_vec data_decls;   /* ta_data_decl */
    ta_vec initializers; /* ta_initializer_def */
    ta_vec contents;     /* ta_content_def */
    ta_vec patterns;     /* ta_event_pattern */
    ta_vec segments;     /* ta_event_pattern_segment */
    ta_vec expressions;  /* ta_expression */
    ta_vec history;      /* ta_history_slot */
    uint64_t hash;
} ta_compiler;

typedef struct {
    ta_vec *vec;
    void **out;
    uint32_t *count;
} ta_compiler_seal_params;

static void ta_compiler_hash(ta_compiler *compiler, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    for (size_t index = 0; index < size; index += 1) {
        compiler->hash ^= in[index];
        compiler->hash *= 1099511628211ull;
    }
}

static void ta_compiler_hash_str(ta_compiler *compiler, ta_str text) {
    ta_compiler_hash(compiler, &text.len, sizeof(text.len));
    if (text.ptr != NULL) {
        ta_compiler_hash(compiler, text.ptr, text.len);
    }
}

static void ta_compiler_hash_u32(ta_compiler *compiler, uint32_t value) {
    ta_compiler_hash(compiler, &value, sizeof(value));
}

static ta_atom_table *ta_compiler_atoms(ta_compiler *compiler) {
    return &compiler->system->atoms;
}

static ta_result ta_compiler_atom(ta_compiler *compiler, ta_str text, ta_atom *out) {
    if (text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_compiler_hash_str(compiler, text);
    return ta_atom_intern(ta_compiler_atoms(compiler), text, out);
}

ta_result ta_duration_parse(ta_str text, ta_time *out) {
    if (text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    uint32_t index = 0;
    uint64_t whole = 0;
    uint64_t fraction = 0;
    uint64_t scale = 1;
    bool digits = false;
    while (index < text.len && text.ptr[index] >= '0' && text.ptr[index] <= '9') {
        if (whole > (UINT64_MAX - 9u) / 10u) {
            return TA_ERR_INVALID;
        }
        whole = whole * 10u + (uint64_t)(text.ptr[index] - '0');
        digits = true;
        index += 1;
    }
    if (index < text.len && text.ptr[index] == '.') {
        index += 1;
        bool fraction_digits = false;
        while (index < text.len && text.ptr[index] >= '0' && text.ptr[index] <= '9') {
            if (scale < 1000000000ull) {
                fraction = fraction * 10u + (uint64_t)(text.ptr[index] - '0');
                scale *= 10u;
            }
            fraction_digits = true;
            index += 1;
        }
        if (!fraction_digits) {
            return TA_ERR_INVALID;
        }
    }
    if (!digits) {
        return TA_ERR_INVALID;
    }
    ta_str unit = { .ptr = text.ptr + index, .len = text.len - index };
    uint64_t multiplier = 0;
    if (ta_str_equal(unit, (ta_str){ .ptr = "ms", .len = 2 })) {
        multiplier = 1;
    } else if (ta_str_equal(unit, (ta_str){ .ptr = "s", .len = 1 })) {
        multiplier = 1000;
    } else {
        return TA_ERR_INVALID;
    }
    if (whole > UINT64_MAX / multiplier) {
        return TA_ERR_INVALID;
    }
    /* Sub-millisecond remainders are truncated. */
    *out = whole * multiplier + (fraction * multiplier) / scale;
    return TA_OK;
}

static ta_result ta_compiler_expression(ta_compiler *compiler, const ta_expression *expression,
                                        ta_expr_id *out) {
    ta_expression copy = { .kind = expression->kind };
    ta_result result = TA_OK;
    switch (expression->kind) {
        case TA_EXPRESSION_SOURCE:
            ta_compiler_hash_str(compiler, expression->as.source);
            if (expression->as.source.ptr == NULL) {
                return TA_ERR_INVALID;
            }
            result = ta_arena_copy_str(&compiler->definition->arena, expression->as.source,
                                       &copy.as.source);
            break;
        case TA_EXPRESSION_HOST:
            if (expression->as.host.v == NULL || expression->as.host.v->retain == NULL) {
                return TA_ERR_INVALID;
            }
            copy.as.host = expression->as.host;
            result = expression->as.host.v->retain(expression->as.host.self, expression->as.host.value,
                                                   compiler->allocator, &copy.as.host.value);
            break;
        default:
            return TA_ERR_INVALID;
    }
    if (result != TA_OK) {
        return result;
    }
    *out = compiler->expressions.count;
    result = ta_vec_push(&compiler->expressions, &copy);
    if (result != TA_OK && copy.kind == TA_EXPRESSION_HOST && copy.as.host.v->release != NULL) {
        copy.as.host.v->release(copy.as.host.self, copy.as.host.value, compiler->allocator);
    }
    return result;
}

static ta_result ta_compiler_content(ta_compiler *compiler, ta_bytes bytes, ta_content_id *out) {
    ta_content_def content = {0};
    if (bytes.len != 0) {
        uint8_t *copy = ta_arena_alloc(&compiler->definition->arena, bytes.len, 1);
        if (copy == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(copy, bytes.ptr, bytes.len);
        content.bytes = (ta_bytes){ .ptr = copy, .len = bytes.len };
    }
    ta_compiler_hash(compiler, bytes.ptr == NULL ? "" : (const void *)bytes.ptr, bytes.len);
    *out = compiler->contents.count;
    return ta_vec_push(&compiler->contents, &content);
}

static ta_result ta_compiler_value(ta_compiler *compiler, const ta_scxml_value_spec *spec,
                                   ta_value_source *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    switch (spec->kind) {
        case TA_SCXML_VALUE_NONE:
            *out = (ta_value_source){ .kind = TA_VALUE_SOURCE_NONE };
            return TA_OK;
        case TA_SCXML_VALUE_EXPR:
            out->kind = TA_VALUE_SOURCE_EXPR;
            return ta_compiler_expression(compiler, &spec->as.expr, &out->as.expr);
        case TA_SCXML_VALUE_LOCATION:
            out->kind = TA_VALUE_SOURCE_LOCATION;
            return ta_compiler_expression(compiler, &spec->as.location, &out->as.location);
        case TA_SCXML_VALUE_CONTENT:
            out->kind = TA_VALUE_SOURCE_CONTENT;
            return ta_compiler_content(compiler, spec->as.content, &out->as.content);
        default:
            return TA_ERR_INVALID;
    }
}

static ta_result ta_compiler_params(ta_compiler *compiler, const ta_scxml_param_spec *specs,
                                    ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].name.ptr != NULL) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->params, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_param_def param = {0};
        result = ta_compiler_atom(compiler, specs[index].name, &param.name);
        if (result == TA_OK) {
            result = ta_compiler_value(compiler, &specs[index].value, &param.value);
        }
        if (result == TA_OK) {
            *(ta_param_def *)ta_vec_at(&compiler->params, base + index) = param;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_result ta_compiler_block(ta_compiler *compiler, const ta_scxml_action_spec *specs,
                                   ta_span32 *out);

static ta_result ta_compiler_send(ta_compiler *compiler, const ta_scxml_send_spec *spec,
                                  ta_send_action *out) {
    ta_result result = TA_OK;
    switch (spec->id.kind) {
        case TA_SCXML_ID_NONE:
            out->id.kind = TA_ID_SOURCE_GENERATED;
            break;
        case TA_SCXML_ID_LITERAL:
            out->id.kind = TA_ID_SOURCE_LITERAL;
            result = ta_compiler_atom(compiler, spec->id.as.literal, &out->id.as.literal);
            break;
        default:
            /* idlocation needs a data-model value made from a generated ID. */
            return TA_ERR_UNSUPPORTED;
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->type.kind) {
        case TA_SCXML_STRING_NONE:
            out->type.kind = TA_TEXT_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->type.kind = TA_TEXT_SOURCE_ATOM;
            result = ta_compiler_atom(compiler, spec->type.as.literal, &out->type.as.atom);
            break;
        default:
            return TA_ERR_UNSUPPORTED;
    }
    if (result != TA_OK) {
        return result;
    }
    if (spec->event.kind != TA_SCXML_EVENT_NAME) {
        return spec->event.kind == TA_SCXML_EVENT_EXPR ? TA_ERR_UNSUPPORTED : TA_ERR_INVALID;
    }
    out->event.kind = TA_EVENT_SOURCE_NAME;
    ta_compiler_hash_str(compiler, spec->event.as.name);
    result = ta_event_name_intern_dotted(&compiler->system->event_names, spec->event.as.name,
                                         &out->event.as.name);
    if (result != TA_OK) {
        return result;
    }
    switch (spec->target.kind) {
        case TA_SCXML_STRING_NONE:
            out->target.kind = TA_VALUE_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->target.kind = TA_VALUE_SOURCE_CONTENT;
            result = ta_compiler_content(compiler,
                                         (ta_bytes){ .ptr = (const uint8_t *)spec->target.as.literal.ptr,
                                                     .len = spec->target.as.literal.len },
                                         &out->target.as.content);
            break;
        case TA_SCXML_STRING_EXPR:
            out->target.kind = TA_VALUE_SOURCE_EXPR;
            result = ta_compiler_expression(compiler, &spec->target.as.expr, &out->target.as.expr);
            break;
        default:
            return TA_ERR_INVALID;
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->delay.kind) {
        case TA_SCXML_STRING_NONE:
            out->delay.kind = TA_DELAY_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->delay.kind = TA_DELAY_SOURCE_LITERAL;
            ta_compiler_hash_str(compiler, spec->delay.as.literal);
            result = ta_duration_parse(spec->delay.as.literal, &out->delay.as.literal);
            break;
        default:
            /* delayexpr needs a data-model value converted to a duration. */
            return TA_ERR_UNSUPPORTED;
    }
    if (result == TA_OK) {
        result = ta_compiler_value(compiler, &spec->data, &out->data);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &out->params);
    }
    if (result == TA_OK && out->params.length != 0 && out->data.kind != TA_VALUE_SOURCE_NONE) {
        result = TA_ERR_INVALID;
    }
    return result;
}

static ta_result ta_compiler_action(ta_compiler *compiler, const ta_scxml_action_spec *spec,
                                    ta_action_def *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    ta_result result = TA_OK;
    switch (spec->kind) {
        case TA_SCXML_ACTION_RAISE:
            out->kind = TA_ACTION_RAISE;
            ta_compiler_hash_str(compiler, spec->as.raise.event);
            result = ta_event_name_intern_dotted(&compiler->system->event_names, spec->as.raise.event,
                                                 &out->as.raise.event);
            if (result == TA_OK) {
                result = ta_compiler_value(compiler, &spec->as.raise.data, &out->as.raise.data);
            }
            return result;
        case TA_SCXML_ACTION_SEND:
            out->kind = TA_ACTION_SEND;
            return ta_compiler_send(compiler, &spec->as.send, &out->as.send);
        case TA_SCXML_ACTION_CANCEL:
            out->kind = TA_ACTION_CANCEL;
            if (spec->as.cancel.sendid.kind == TA_SCXML_STRING_EXPR) {
                /* sendidexpr needs a data-model value converted to a send ID. */
                return TA_ERR_UNSUPPORTED;
            }
            if (spec->as.cancel.sendid.kind != TA_SCXML_STRING_LITERAL) {
                return TA_ERR_INVALID;
            }
            out->as.cancel.sendid.kind = TA_TEXT_SOURCE_ATOM;
            return ta_compiler_atom(compiler, spec->as.cancel.sendid.as.literal,
                                    &out->as.cancel.sendid.as.atom);
        case TA_SCXML_ACTION_ASSIGN:
            out->kind = TA_ACTION_ASSIGN;
            if (spec->as.assign.value.kind == TA_SCXML_VALUE_NONE) {
                return TA_ERR_INVALID;
            }
            result = ta_compiler_expression(compiler, &spec->as.assign.location, &out->as.assign.location);
            if (result == TA_OK) {
                result = ta_compiler_value(compiler, &spec->as.assign.value, &out->as.assign.value);
            }
            return result;
        case TA_SCXML_ACTION_SCRIPT:
            out->kind = TA_ACTION_SCRIPT;
            return ta_compiler_expression(compiler, &spec->as.script.script, &out->as.script);
        case TA_SCXML_ACTION_LOG:
            out->kind = TA_ACTION_LOG;
            if (spec->as.log.label.ptr != NULL) {
                out->as.log.has_label = true;
                result = ta_atom_intern(ta_compiler_atoms(compiler), spec->as.log.label, &out->as.log.label);
            }
            if (result == TA_OK && spec->as.log.expr.kind != TA_EXPRESSION_NONE) {
                out->as.log.has_expr = true;
                result = ta_compiler_expression(compiler, &spec->as.log.expr, &out->as.log.expr);
            }
            return result;
        case TA_SCXML_ACTION_IF:
            out->kind = TA_ACTION_IF;
            result = ta_compiler_expression(compiler, &spec->as.conditional.condition,
                                            &out->as.conditional.condition);
            if (result == TA_OK) {
                result = ta_compiler_block(compiler, spec->as.conditional.then_actions,
                                           &out->as.conditional.then_actions);
            }
            if (result == TA_OK) {
                result = ta_compiler_block(compiler, spec->as.conditional.else_actions,
                                           &out->as.conditional.else_actions);
            }
            return result;
        case TA_SCXML_ACTION_FOREACH:
        case TA_SCXML_ACTION_INVOKE:
        case TA_SCXML_ACTION_EXTENSION:
            return TA_ERR_UNSUPPORTED;
        default:
            return TA_ERR_INVALID;
    }
}

static ta_result ta_compiler_block(ta_compiler *compiler, const ta_scxml_action_spec *specs,
                                   ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].kind != TA_SCXML_ACTION_NONE) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->actions, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        /* Nested blocks append behind this block, so write by index. */
        ta_action_def action = {0};
        result = ta_compiler_action(compiler, &specs[index], &action);
        if (result == TA_OK) {
            *(ta_action_def *)ta_vec_at(&compiler->actions, base + index) = action;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_state_def *ta_compiler_state(ta_compiler *compiler, ta_state_id state) {
    return ta_vec_at(&compiler->states, state);
}

static ta_result ta_compiler_kind(ta_scxml_state_kind kind, ta_state_kind *out) {
    switch (kind) {
        case TA_SCXML_STATE_ATOMIC: *out = TA_STATE_ATOMIC; return TA_OK;
        case TA_SCXML_STATE_COMPOUND: *out = TA_STATE_COMPOUND; return TA_OK;
        case TA_SCXML_STATE_PARALLEL: *out = TA_STATE_PARALLEL; return TA_OK;
        case TA_SCXML_STATE_FINAL: *out = TA_STATE_FINAL; return TA_OK;
        case TA_SCXML_STATE_HISTORY_SHALLOW: *out = TA_STATE_HISTORY_SHALLOW; return TA_OK;
        case TA_SCXML_STATE_HISTORY_DEEP: *out = TA_STATE_HISTORY_DEEP; return TA_OK;
        default: return TA_ERR_INVALID;
    }
}

static bool ta_state_kind_is_history(ta_state_kind kind) {
    return kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
}

/* Pass one: assigns IDs in document order and records child ranges. */
static ta_result ta_compiler_collect(ta_compiler *compiler, const ta_scxml_state_spec *specs,
                                     ta_state_id parent, ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].kind != TA_SCXML_STATE_NONE) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->children, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        const ta_scxml_state_spec *spec = &specs[index];
        ta_state_def state = {
            .parent = parent,
            .initial = TA_INITIAL_TRANSITION_NONE,
            .done_data = TA_DONE_DATA_NONE,
            .history_slot = UINT32_MAX,
        };
        ta_compiler_hash_u32(compiler, spec->kind);
        result = ta_compiler_kind(spec->kind, &state.kind);
        if (result == TA_OK) {
            result = ta_compiler_atom(compiler, spec->id, &state.name);
        }
        for (uint32_t other = 0; result == TA_OK && other < compiler->states.count; other += 1) {
            if (ta_compiler_state(compiler, other)->name == state.name) {
                result = TA_ERR_INVALID;
            }
        }
        if (result != TA_OK) {
            return result;
        }
        ta_state_id id = compiler->states.count;
        *(ta_state_id *)ta_vec_at(&compiler->children, base + index) = id;
        result = ta_vec_push(&compiler->states, &state);
        if (result == TA_OK) {
            result = ta_vec_push(&compiler->state_specs, &spec);
        }
        ta_span32 children = {0};
        if (result == TA_OK) {
            result = ta_compiler_collect(compiler, spec->states, id, &children);
        }
        if (result == TA_OK) {
            ta_compiler_state(compiler, id)->children = children;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_result ta_compiler_resolve(ta_compiler *compiler, ta_str id, ta_state_id *out) {
    ta_atom atom = 0;
    ta_result result = ta_compiler_atom(compiler, id, &atom);
    if (result != TA_OK) {
        return result;
    }
    for (ta_state_id state = 0; state < compiler->states.count; state += 1) {
        if (ta_compiler_state(compiler, state)->name == atom) {
            *out = state;
            return TA_OK;
        }
    }
    return TA_ERR_INVALID;
}

static bool ta_compiler_descends(ta_compiler *compiler, ta_state_id state, ta_state_id ancestor) {
    if (ancestor == TA_STATE_NONE) {
        return true;
    }
    for (ta_state_id at = ta_compiler_state(compiler, state)->parent; at != TA_STATE_NONE;
         at = ta_compiler_state(compiler, at)->parent) {
        if (at == ancestor) {
            return true;
        }
    }
    return false;
}

typedef struct {
    const ta_str *ids;
    /* Targets must be proper descendants of this state (TA_STATE_NONE: any). */
    ta_state_id within;
    ta_span32 *out;
} ta_compiler_targets_params;

static ta_result ta_compiler_targets(ta_compiler *compiler, const ta_compiler_targets_params *params) {
    uint32_t count = 0;
    while (params->ids != NULL && params->ids[count].ptr != NULL) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->targets, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_state_id target = TA_STATE_NONE;
        result = ta_compiler_resolve(compiler, params->ids[index], &target);
        if (result == TA_OK && !ta_compiler_descends(compiler, target, params->within)) {
            result = TA_ERR_INVALID;
        }
        if (result == TA_OK) {
            *(ta_state_id *)ta_vec_at(&compiler->targets, base + index) = target;
        }
    }
    *params->out = (ta_span32){ .offset = base, .length = count };
    return result;
}

/* Parses space-separated descriptors; a `*` segment is a wildcard. */
static ta_result ta_compiler_descriptors(ta_compiler *compiler, ta_str text, ta_span32 *out) {
    ta_compiler_hash_str(compiler, text);
    uint32_t first = compiler->patterns.count;
    uint32_t index = 0;
    while (text.ptr != NULL && index < text.len) {
        while (index < text.len && (text.ptr[index] == ' ' || text.ptr[index] == '\t' ||
                                    text.ptr[index] == '\n' || text.ptr[index] == '\r')) {
            index += 1;
        }
        uint32_t start = index;
        while (index < text.len && text.ptr[index] != ' ' && text.ptr[index] != '\t' &&
               text.ptr[index] != '\n' && text.ptr[index] != '\r') {
            index += 1;
        }
        if (index == start) {
            break;
        }
        ta_event_pattern pattern = { .segments = { .offset = compiler->segments.count } };
        uint32_t segment_start = start;
        for (uint32_t at = start; at <= index; at += 1) {
            if (at < index && text.ptr[at] != '.') {
                continue;
            }
            ta_str segment = { .ptr = text.ptr + segment_start, .len = at - segment_start };
            if (segment.len == 0) {
                return TA_ERR_INVALID;
            }
            ta_event_pattern_segment compiled = {0};
            ta_result result = TA_OK;
            if (segment.len == 1 && segment.ptr[0] == '*') {
                compiled.flags = TA_EVENT_PATTERN_SEGMENT_WILDCARD;
                compiled.atom = TA_ATOM_NONE;
            } else {
                result = ta_atom_intern(ta_compiler_atoms(compiler), segment, &compiled.atom);
            }
            /* A trailing `.*` adds nothing to SCXML prefix matching. */
            bool trailing_wildcard = at == index && pattern.segments.length > 0 &&
                                     compiled.flags == TA_EVENT_PATTERN_SEGMENT_WILDCARD;
            if (result == TA_OK && !trailing_wildcard) {
                result = ta_vec_push(&compiler->segments, &compiled);
                pattern.segments.length += 1;
            }
            if (result != TA_OK) {
                return result;
            }
            segment_start = at + 1u;
        }
        ta_result result = ta_vec_push(&compiler->patterns, &pattern);
        if (result != TA_OK) {
            return result;
        }
    }
    *out = (ta_span32){ .offset = first, .length = compiler->patterns.count - first };
    return TA_OK;
}

static ta_result ta_compiler_transition(ta_compiler *compiler, ta_state_id source,
                                        const ta_scxml_transition_spec *spec) {
    ta_transition_def transition = {
        .source = source,
        .condition = UINT32_MAX,
        .document_order = compiler->transitions.count,
    };
    ta_compiler_hash_u32(compiler, spec->kind);
    if (spec->kind == TA_SCXML_TRANSITION_EXTERNAL) {
        transition.type = TA_TRANSITION_EXTERNAL;
    } else if (spec->kind == TA_SCXML_TRANSITION_INTERNAL) {
        transition.type = TA_TRANSITION_INTERNAL;
    } else {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_compiler_descriptors(compiler, spec->event, &transition.event_patterns);
    if (result == TA_OK && spec->condition.kind != TA_EXPRESSION_NONE) {
        result = ta_compiler_expression(compiler, &spec->condition, &transition.condition);
    }
    if (result == TA_OK) {
        result = ta_compiler_targets(compiler, &(ta_compiler_targets_params){
                                                   .ids = spec->targets,
                                                   .within = TA_STATE_NONE,
                                                   .out = &transition.targets,
                                               });
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, spec->actions, &transition.actions);
    }
    if (result == TA_OK) {
        result = ta_vec_push(&compiler->transitions, &transition);
    }
    return result;
}

typedef struct {
    ta_state_id owner;
    const ta_scxml_initial_spec *spec;
    ta_span32 children;
    ta_initial_transition_id *out;
} ta_compiler_initial_params;

static ta_result ta_compiler_initial(ta_compiler *compiler, const ta_compiler_initial_params *params) {
    ta_initial_transition_def initial = { .owner = params->owner };
    ta_result result = TA_OK;
    if (params->spec != NULL && params->spec->targets != NULL && params->spec->targets[0].ptr != NULL) {
        result = ta_compiler_targets(compiler, &(ta_compiler_targets_params){
                                                   .ids = params->spec->targets,
                                                   .within = params->owner,
                                                   .out = &initial.targets,
                                               });
    } else {
        /* SCXML default: the first child state in document order. */
        ta_state_id first = TA_STATE_NONE;
        for (uint32_t index = 0; index < params->children.length && first == TA_STATE_NONE; index += 1) {
            ta_state_id child = *(ta_state_id *)ta_vec_at(&compiler->children, params->children.offset + index);
            if (!ta_state_kind_is_history(ta_compiler_state(compiler, child)->kind)) {
                first = child;
            }
        }
        if (first == TA_STATE_NONE) {
            return TA_ERR_INVALID;
        }
        initial.targets.offset = compiler->targets.count;
        initial.targets.length = 1;
        result = ta_vec_push(&compiler->targets, &first);
    }
    if (result == TA_OK && params->spec != NULL) {
        result = ta_compiler_block(compiler, params->spec->actions, &initial.actions);
    }
    if (result == TA_OK) {
        *params->out = compiler->initials.count;
        result = ta_vec_push(&compiler->initials, &initial);
    }
    return result;
}

static ta_result ta_compiler_data(ta_compiler *compiler, const ta_scxml_data_spec *specs, ta_state_id owner) {
    for (uint32_t index = 0; specs != NULL && specs[index].name.ptr != NULL; index += 1) {
        ta_data_decl decl = { .owner_state = owner, .initializer = TA_INITIALIZER_NONE };
        ta_result result = ta_compiler_atom(compiler, specs[index].name, &decl.name);
        if (result == TA_OK && specs[index].initializer.kind != TA_SCXML_VALUE_NONE) {
            ta_initializer_def initializer = {0};
            result = ta_compiler_value(compiler, &specs[index].initializer, &initializer.value);
            if (result == TA_OK) {
                decl.initializer = compiler->initializers.count;
                result = ta_vec_push(&compiler->initializers, &initializer);
            }
        }
        if (result == TA_OK) {
            result = ta_vec_push(&compiler->data_decls, &decl);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* Pass two for one state. */
static ta_result ta_compiler_compile_state(ta_compiler *compiler, ta_state_id id) {
    const ta_scxml_state_spec *spec = *(const ta_scxml_state_spec **)ta_vec_at(&compiler->state_specs, id);
    ta_state_def state = *ta_compiler_state(compiler, id);
    ta_state_def parent = {0};
    if (state.parent != TA_STATE_NONE) {
        parent = *ta_compiler_state(compiler, state.parent);
    }
    uint32_t real_children = 0;
    for (uint32_t index = 0; index < state.children.length; index += 1) {
        ta_state_id child = *(ta_state_id *)ta_vec_at(&compiler->children, state.children.offset + index);
        real_children += ta_state_kind_is_history(ta_compiler_state(compiler, child)->kind) ? 0u : 1u;
    }
    bool history = ta_state_kind_is_history(state.kind);
    if (spec->invokes != NULL && spec->invokes[0].kind != TA_SCXML_INVOKE_NONE) {
        return TA_ERR_UNSUPPORTED;
    }
    if (spec->done_data != NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    switch (state.kind) {
        case TA_STATE_ATOMIC:
        case TA_STATE_FINAL:
            if (state.children.length != 0 || spec->initial != NULL) {
                return TA_ERR_INVALID;
            }
            break;
        case TA_STATE_COMPOUND:
        case TA_STATE_PARALLEL:
            if (real_children == 0 || (state.kind == TA_STATE_PARALLEL && spec->initial != NULL)) {
                return TA_ERR_INVALID;
            }
            break;
        default:
            if (state.parent == TA_STATE_NONE || state.children.length != 0 ||
                (parent.kind != TA_STATE_COMPOUND && parent.kind != TA_STATE_PARALLEL)) {
                return TA_ERR_INVALID;
            }
            break;
    }

    state.transitions.offset = compiler->transitions.count;
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && spec->transitions != NULL &&
                             spec->transitions[index].kind != TA_SCXML_TRANSITION_NONE;
         index += 1) {
        result = ta_compiler_transition(compiler, id, &spec->transitions[index]);
    }
    state.transitions.length = compiler->transitions.count - state.transitions.offset;
    if (result != TA_OK) {
        return result;
    }
    if (history) {
        /* A history state has exactly one targetless-event default transition into its parent. */
        if (state.transitions.length != 1) {
            return TA_ERR_INVALID;
        }
        const ta_transition_def *fallback = ta_vec_at(&compiler->transitions, state.transitions.offset);
        if (fallback->event_patterns.length != 0 || fallback->condition != UINT32_MAX ||
            fallback->targets.length == 0) {
            return TA_ERR_INVALID;
        }
        for (uint32_t index = 0; index < fallback->targets.length; index += 1) {
            ta_state_id target = *(ta_state_id *)ta_vec_at(&compiler->targets, fallback->targets.offset + index);
            if (!ta_compiler_descends(compiler, target, state.parent)) {
                return TA_ERR_INVALID;
            }
        }
        state.history_slot = compiler->history.count;
        ta_history_slot slot = {0};
        result = ta_vec_push(&compiler->history, &slot);
    }
    if (result == TA_OK && state.kind == TA_STATE_COMPOUND) {
        result = ta_compiler_initial(compiler, &(ta_compiler_initial_params){
                                                   .owner = id,
                                                   .spec = spec->initial,
                                                   .children = state.children,
                                                   .out = &state.initial,
                                               });
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, spec->onentry, &state.onentry_actions);
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, spec->onexit, &state.onexit_actions);
    }
    if (result == TA_OK) {
        result = ta_compiler_data(compiler, spec->data, id);
    }
    if (result == TA_OK) {
        ta_atom segment = state.name;
        result = ta_event_name_child(&compiler->system->event_names, TA_EVENT_NAME_DONE_STATE, segment,
                                     &state.done_event);
    }
    if (result == TA_OK) {
        *ta_compiler_state(compiler, id) = state;
    }
    return result;
}

static ta_result ta_compiler_seal(ta_compiler *compiler, const ta_compiler_seal_params *params) {
    *params->out = NULL;
    if (params->count != NULL) {
        *params->count = params->vec->count;
    }
    if (params->vec->count == 0) {
        return TA_OK;
    }
    size_t size = params->vec->count * params->vec->element_size;
    void *copy = ta_arena_alloc(&compiler->definition->arena, size, params->vec->element_alignment);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, params->vec->items, size);
    *params->out = copy;
    return TA_OK;
}

static void ta_compiler_destroy(ta_compiler *compiler) {
    ta_vec *vecs[] = {
        &compiler->states, &compiler->state_specs, &compiler->children, &compiler->transitions,
        &compiler->initials, &compiler->targets, &compiler->actions, &compiler->params,
        &compiler->data_decls, &compiler->initializers, &compiler->contents, &compiler->patterns,
        &compiler->segments, &compiler->expressions, &compiler->history,
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(vecs); index += 1) {
        ta_vec_destroy(vecs[index]);
    }
}

static ta_result ta_compiler_identity(ta_compiler *compiler) {
    ta_definition *definition = compiler->definition;
    const ta_scxml_spec *spec = compiler->spec;
    ta_str family = spec->name;
    ta_bytes revision = {0};
    static const char hex[] = "0123456789abcdef";
    char fingerprint[16];
    if (spec->identity != NULL) {
        family = spec->identity->family.name;
        revision = spec->identity->revision;
    } else {
        for (uint32_t index = 0; index < 16; index += 1) {
            fingerprint[index] = hex[(compiler->hash >> (60u - index * 4u)) & 0xfu];
        }
        revision = (ta_bytes){ .ptr = (const uint8_t *)fingerprint, .len = 16 };
    }
    if (family.ptr == NULL || family.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_arena_copy_str(&definition->arena, family, &definition->identity.family.name);
    ta_str revision_copy = {0};
    if (result == TA_OK) {
        result = ta_arena_copy_str(&definition->arena,
                                   (ta_str){ .ptr = (const char *)revision.ptr, .len = revision.len },
                                   &revision_copy);
    }
    definition->identity.revision = (ta_bytes){ .ptr = (const uint8_t *)revision_copy.ptr,
                                                .len = revision_copy.len };
    if (result == TA_OK) {
        result = ta_atom_intern(ta_compiler_atoms(compiler), family, &definition->name);
    }
    return result;
}

static ta_result ta_compiler_finish(ta_compiler *compiler) {
    ta_definition *definition = compiler->definition;
    const ta_compiler_seal_params seals[] = {
        { &compiler->states, (void **)&definition->states, &definition->state_count },
        { &compiler->children, (void **)&definition->children, &definition->child_count },
        { &compiler->transitions, (void **)&definition->transitions, &definition->transition_count },
        { &compiler->initials, (void **)&definition->initial_transitions, &definition->initial_transition_count },
        { &compiler->targets, (void **)&definition->targets, &definition->target_count },
        { &compiler->actions, (void **)&definition->action_defs, &definition->action_count },
        { &compiler->params, (void **)&definition->params, &definition->param_count },
        { &compiler->data_decls, (void **)&definition->data_decls, &definition->data_decl_count },
        { &compiler->initializers, (void **)&definition->initializers, &definition->initializer_count },
        { &compiler->contents, (void **)&definition->contents, &definition->content_count },
        { &compiler->patterns, (void **)&definition->event_patterns, &definition->event_pattern_count },
        { &compiler->segments, (void **)&definition->event_pattern_segments, &definition->event_pattern_segment_count },
        { &compiler->history, (void **)&definition->history.slots, &definition->history.slot_count },
    };
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && index < TA_ARRAY_COUNT(seals); index += 1) {
        result = ta_compiler_seal(compiler, &seals[index]);
    }
    if (result != TA_OK) {
        return result;
    }
    uint32_t words = ta_bits_words(definition->state_count);
    if (words * 8u > UINT16_MAX) {
        return TA_ERR_UNSUPPORTED;
    }
    definition->configuration = (ta_configuration_layout){
        .kind = TA_CONFIG_WORDS,
        .bytes = (uint16_t)(words * 8u),
        .alignment = (uint16_t)_Alignof(uint64_t),
        .atomic_state_count = definition->state_count,
    };
    for (uint32_t slot = 0; slot < definition->history.slot_count; slot += 1) {
        definition->history.slots[slot] = (ta_history_slot){ .offset = slot * words * 8u,
                                                             .bytes = (uint16_t)(words * 8u) };
    }
    definition->history.storage_bytes = definition->history.slot_count * words * 8u;
    return ta_compiler_identity(compiler);
}

static ta_result ta_compiler_run(ta_compiler *compiler) {
    const ta_scxml_spec *spec = compiler->spec;
    ta_definition *definition = compiler->definition;
    ta_compiler_hash_str(compiler, spec->name);
    ta_compiler_hash_u32(compiler, spec->binding);
    ta_span32 roots = {0};
    ta_result result = ta_compiler_collect(compiler, spec->states, TA_STATE_NONE, &roots);
    if (result == TA_OK && compiler->states.count == 0) {
        result = TA_ERR_INVALID;
    }
    /* Data declarations are listed root first, then by state in document order. */
    if (result == TA_OK) {
        result = ta_compiler_data(compiler, spec->data, TA_STATE_NONE);
    }
    for (ta_state_id state = 0; result == TA_OK && state < compiler->states.count; state += 1) {
        result = ta_compiler_compile_state(compiler, state);
    }
    if (result == TA_OK) {
        result = ta_compiler_initial(compiler, &(ta_compiler_initial_params){
                                                   .owner = TA_STATE_NONE,
                                                   .spec = spec->initial,
                                                   .children = roots,
                                                   .out = &definition->root_initial,
                                               });
    }
    if (result == TA_OK && spec->script.kind != TA_EXPRESSION_NONE) {
        uint32_t base = 0;
        ta_action_def script = { .kind = TA_ACTION_SCRIPT };
        result = ta_compiler_expression(compiler, &spec->script, &script.as.script);
        if (result == TA_OK) {
            result = ta_vec_extend(&compiler->actions, 1, &base);
        }
        if (result == TA_OK) {
            *(ta_action_def *)ta_vec_at(&compiler->actions, base) = script;
            definition->global_script_actions = (ta_span32){ .offset = base, .length = 1 };
        }
    }
    if (result == TA_OK) {
        result = ta_compiler_finish(compiler);
    }
    return result;
}

static bool ta_datamodel_complete(const ta_datamodel *datamodel) {
    const ta_datamodel_vtable *v = datamodel->v;
    return v != NULL && v->prepare_definition != NULL && v->destroy_definition != NULL &&
           v->instance_size != NULL && v->instance_alignment != NULL && v->init_instance != NULL &&
           v->destroy_instance != NULL;
}

ta_result ta_definition_compile_spec(ta_system *system, const ta_definition_compile_params *params) {
    if (system == NULL || params == NULL || params->spec == NULL || params->out == NULL ||
        params->allocator.v == NULL || !ta_datamodel_complete(&params->datamodel)) {
        return TA_ERR_INVALID;
    }
    const ta_scxml_spec *spec = params->spec;
    if (spec->datamodel.ptr != NULL && spec->datamodel.len != 0 &&
        !ta_str_equal(spec->datamodel, params->datamodel.v->name)) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_definition *definition = ta_mem_alloc(params->allocator, sizeof(ta_definition),
                                             _Alignof(ta_definition));
    if (definition == NULL) {
        return TA_ERR_NOMEM;
    }
    *definition = (ta_definition){
        .allocator = params->allocator,
        .system = system,
        .root_initial = TA_INITIAL_TRANSITION_NONE,
        .binding_mode = spec->binding == TA_SCXML_BINDING_LATE ? TA_BINDING_LATE : TA_BINDING_EARLY,
        .datamodel = params->datamodel,
    };
    ta_result result = ta_arena_init(&definition->arena, params->allocator, 4096);
    if (result != TA_OK) {
        ta_mem_free(params->allocator, definition,
                    &(ta_allocator_free_params){ .size = sizeof(ta_definition),
                                                 .alignment = _Alignof(ta_definition) });
        return result;
    }

    ta_compiler compiler = {
        .system = system,
        .allocator = params->allocator,
        .spec = spec,
        .definition = definition,
        .hash = 14695981039346656037ull,
    };
    const struct { ta_vec *vec; const ta_tree_list_layout *layout; } vecs[] = {
        { &compiler.states, TA_VEC_LAYOUT(ta_state_def) },
        { &compiler.state_specs, TA_VEC_LAYOUT(const ta_scxml_state_spec *) },
        { &compiler.children, TA_VEC_LAYOUT(ta_state_id) },
        { &compiler.transitions, TA_VEC_LAYOUT(ta_transition_def) },
        { &compiler.initials, TA_VEC_LAYOUT(ta_initial_transition_def) },
        { &compiler.targets, TA_VEC_LAYOUT(ta_state_id) },
        { &compiler.actions, TA_VEC_LAYOUT(ta_action_def) },
        { &compiler.params, TA_VEC_LAYOUT(ta_param_def) },
        { &compiler.data_decls, TA_VEC_LAYOUT(ta_data_decl) },
        { &compiler.initializers, TA_VEC_LAYOUT(ta_initializer_def) },
        { &compiler.contents, TA_VEC_LAYOUT(ta_content_def) },
        { &compiler.patterns, TA_VEC_LAYOUT(ta_event_pattern) },
        { &compiler.segments, TA_VEC_LAYOUT(ta_event_pattern_segment) },
        { &compiler.expressions, TA_VEC_LAYOUT(ta_expression) },
        { &compiler.history, TA_VEC_LAYOUT(ta_history_slot) },
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(vecs); index += 1) {
        ta_vec_init(vecs[index].vec, params->allocator, vecs[index].layout);
    }

    result = ta_compiler_run(&compiler);
    /* The expression table moves to the definition even on failure, so it is released once. */
    ta_result sealed = ta_compiler_seal(&compiler, &(ta_compiler_seal_params){
                                                       .vec = &compiler.expressions,
                                                       .out = (void **)&definition->expressions,
                                                       .count = &definition->expression_count,
                                                   });
    if (sealed != TA_OK) {
        /* Could not move host expressions; release them from the vector. */
        for (uint32_t index = 0; index < compiler.expressions.count; index += 1) {
            ta_expression *expression = ta_vec_at(&compiler.expressions, index);
            if (expression->kind == TA_EXPRESSION_HOST && expression->as.host.v->release != NULL) {
                expression->as.host.v->release(expression->as.host.self, expression->as.host.value,
                                               params->allocator);
            }
        }
        definition->expression_count = 0;
        result = result == TA_OK ? sealed : result;
    }
    ta_compiler_destroy(&compiler);

    if (result == TA_OK) {
        result = params->datamodel.v->prepare_definition(
            params->datamodel.self,
            &(ta_datamodel_prepare_definition_params){
                .expressions = definition->expressions,
                .expression_count = definition->expression_count,
                .allocator = params->allocator,
                .shared = &definition->datamodel_shared,
            });
        definition->datamodel_prepared = result == TA_OK;
    }
    if (result == TA_OK) {
        definition->datamodel_instance_size =
            params->datamodel.v->instance_size(params->datamodel.self, definition->datamodel_shared);
        definition->datamodel_instance_alignment =
            params->datamodel.v->instance_alignment(params->datamodel.self, definition->datamodel_shared);
        if (definition->datamodel_instance_alignment == 0) {
            definition->datamodel_instance_alignment = 1;
        }
    }
    if (result != TA_OK) {
        ta_definition_destroy(definition);
        return result;
    }
    *params->out = definition;
    return TA_OK;
}
