#include "codec.h"
#include "system.h"
#include "memory.h"
#include "str.h"
#include "text_writer.h"
#include "../include/tinyactors.h"
#include "definition.h"
#include "export.h"

/*
 * Import: decode, compile, register. The decode's arena holds the spec,
 * which may point into the input; compile copies what it keeps, so neither
 * outlives this call as far as the definition is concerned.
 */

static void ta_import_fail(const ta_definition_import_params *params, const ta_import_error *error) {
    if (params->error != NULL) {
        *params->error = *error;
    }
}

/* ------------------------------------------------------------------------
 * Locating a compile error in the input (ADR 0058 section 5)
 */

static bool ta_import_within(ta_bytes input, const char *ptr, uint32_t len) {
    const char *begin = (const char *)input.ptr;
    return ptr != NULL && input.ptr != NULL && ptr >= begin && (size_t)(ptr - begin) <= input.len &&
           len <= input.len - (size_t)(ptr - begin);
}

/* Lines and columns count from 1; a column counts bytes, as txml's do. */
static ta_chart_position ta_import_position(ta_bytes input, uint64_t offset) {
    ta_chart_position at = { .offset = offset, .line = 1, .column = 1 };
    for (uint64_t index = 0; index < offset && index < input.len; index += 1u) {
        if (input.ptr[index] == '\n') {
            at.line += 1u;
            at.column = 1;
        } else {
            at.column += 1u;
        }
    }
    return at;
}

typedef enum {
    TA_NODE_NONE = 0,
    TA_NODE_SPEC,
    TA_NODE_STATE,
    TA_NODE_TRANSITION,
    TA_NODE_HANDLER,
    TA_NODE_INITIAL,
    TA_NODE_ACTION,
    TA_NODE_DATA,
    TA_NODE_PARAM,
    TA_NODE_INVOKE,
    TA_NODE_DONE_DATA,
    TA_NODE_TEXT
} ta_node_kind;

/* One element of a spec reached by a path; TA_NODE_TEXT is a text member. */
typedef struct {
    ta_node_kind kind;
    const void *ptr;
    ta_str text;
} ta_node;

static ta_str ta_expression_text(const ta_expression *expression) {
    return expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source : (ta_str){0};
}

static ta_str ta_value_text(const ta_scxml_value_spec *value) {
    switch (value->kind) {
        case TA_SCXML_VALUE_EXPR: return ta_expression_text(&value->as.expr);
        case TA_SCXML_VALUE_LOCATION: return ta_expression_text(&value->as.location);
        case TA_SCXML_VALUE_CONTENT:
            return (ta_str){ .ptr = (const char *)value->as.content.bytes.ptr, .len = value->as.content.bytes.len };
        default: return (ta_str){0};
    }
}

static ta_str ta_string_text(const ta_scxml_string_spec *string) {
    switch (string->kind) {
        case TA_SCXML_STRING_LITERAL: return string->as.literal;
        case TA_SCXML_STRING_EXPR: return ta_expression_text(&string->as.expr);
        default: return (ta_str){0};
    }
}

static ta_str ta_id_text(const ta_scxml_id_spec *id) {
    switch (id->kind) {
        case TA_SCXML_ID_LITERAL: return id->as.literal;
        case TA_SCXML_ID_LOCATION: return ta_expression_text(&id->as.location);
        default: return (ta_str){0};
    }
}

static ta_node ta_text_node(ta_str text) {
    return (ta_node){ .kind = TA_NODE_TEXT, .text = text };
}

static ta_node ta_list_node(ta_node_kind kind, const void *element) {
    return (ta_node){ .kind = kind, .ptr = element };
}

static ta_node ta_action_step(const ta_scxml_action_spec *action, ta_spec_step step) {
    uint32_t i = step.index;
    switch (action->kind) {
        case TA_SCXML_ACTION_RAISE:
            if (step.member == TA_SPEC_MEMBER_EVENT) return ta_text_node(action->as.raise.event);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&action->as.raise.data));
            return (ta_node){0};
        case TA_SCXML_ACTION_SEND: {
            const ta_scxml_send_spec *send = &action->as.send;
            switch (step.member) {
                case TA_SPEC_MEMBER_PARAMS: return ta_list_node(TA_NODE_PARAM, &send->params[i]);
                case TA_SPEC_MEMBER_ID: return ta_text_node(ta_id_text(&send->id));
                case TA_SPEC_MEMBER_TYPE: return ta_text_node(ta_string_text(&send->type));
                case TA_SPEC_MEMBER_EVENT:
                    if (send->event.kind == TA_SCXML_EVENT_NAME) return ta_text_node(send->event.as.name);
                    if (send->event.kind == TA_SCXML_EVENT_EXPR) return ta_text_node(ta_expression_text(&send->event.as.expr));
                    return (ta_node){0};
                case TA_SPEC_MEMBER_TARGET: return ta_text_node(ta_string_text(&send->target));
                case TA_SPEC_MEMBER_DELAY: return ta_text_node(ta_string_text(&send->delay));
                case TA_SPEC_MEMBER_DATA_VALUE: return ta_text_node(ta_value_text(&send->data));
                default: return (ta_node){0};
            }
        }
        case TA_SCXML_ACTION_CANCEL:
            if (step.member == TA_SPEC_MEMBER_SENDID) return ta_text_node(ta_string_text(&action->as.cancel.sendid));
            return (ta_node){0};
        case TA_SCXML_ACTION_ASSIGN:
            if (step.member == TA_SPEC_MEMBER_LOCATION) return ta_text_node(ta_expression_text(&action->as.assign.location));
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&action->as.assign.value));
            return (ta_node){0};
        case TA_SCXML_ACTION_SCRIPT:
            if (step.member == TA_SPEC_MEMBER_EXPR) return ta_text_node(ta_expression_text(&action->as.script.script));
            return (ta_node){0};
        case TA_SCXML_ACTION_LOG:
            if (step.member == TA_SPEC_MEMBER_EXPR) return ta_text_node(ta_expression_text(&action->as.log.expr));
            return (ta_node){0};
        case TA_SCXML_ACTION_IF:
            switch (step.member) {
                case TA_SPEC_MEMBER_CONDITION: return ta_text_node(ta_expression_text(&action->as.conditional.condition));
                case TA_SPEC_MEMBER_THEN: return ta_list_node(TA_NODE_ACTION, &action->as.conditional.then_actions[i]);
                case TA_SPEC_MEMBER_ELSE: return ta_list_node(TA_NODE_ACTION, &action->as.conditional.else_actions[i]);
                default: return (ta_node){0};
            }
        case TA_SCXML_ACTION_FOREACH:
            switch (step.member) {
                case TA_SPEC_MEMBER_EXPR: return ta_text_node(ta_expression_text(&action->as.foreach.array));
                case TA_SPEC_MEMBER_ITEM: return ta_text_node(action->as.foreach.item);
                case TA_SPEC_MEMBER_INDEX: return ta_text_node(action->as.foreach.index);
                case TA_SPEC_MEMBER_BODY: return ta_list_node(TA_NODE_ACTION, &action->as.foreach.body[i]);
                default: return (ta_node){0};
            }
        default:
            return (ta_node){0};
    }
}

/* Follows one step of a compile error's path. */
static ta_node ta_node_step(ta_node node, ta_spec_step step) {
    uint32_t i = step.index;
    switch (node.kind) {
        case TA_NODE_SPEC: {
            const ta_scxml_spec *spec = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_STATES: return ta_list_node(TA_NODE_STATE, &spec->states[i]);
                case TA_SPEC_MEMBER_DATA: return ta_list_node(TA_NODE_DATA, &spec->data[i]);
                case TA_SPEC_MEMBER_INITIAL: return ta_list_node(TA_NODE_INITIAL, spec->initial);
                case TA_SPEC_MEMBER_SCRIPT: return ta_text_node(ta_expression_text(&spec->script));
                case TA_SPEC_MEMBER_NAME: return ta_text_node(spec->name);
                case TA_SPEC_MEMBER_DATAMODEL: return ta_text_node(spec->datamodel);
                default: return (ta_node){0};
            }
        }
        case TA_NODE_STATE: {
            const ta_scxml_state_spec *state = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_STATES: return ta_list_node(TA_NODE_STATE, &state->states[i]);
                case TA_SPEC_MEMBER_TRANSITIONS: return ta_list_node(TA_NODE_TRANSITION, &state->transitions[i]);
                case TA_SPEC_MEMBER_ONENTRY: return ta_list_node(TA_NODE_HANDLER, &state->onentry[i]);
                case TA_SPEC_MEMBER_ONEXIT: return ta_list_node(TA_NODE_HANDLER, &state->onexit[i]);
                case TA_SPEC_MEMBER_DATA: return ta_list_node(TA_NODE_DATA, &state->data[i]);
                case TA_SPEC_MEMBER_INVOKES: return ta_list_node(TA_NODE_INVOKE, &state->invokes[i]);
                case TA_SPEC_MEMBER_INITIAL: return ta_list_node(TA_NODE_INITIAL, state->initial);
                case TA_SPEC_MEMBER_DONE_DATA: return ta_list_node(TA_NODE_DONE_DATA, state->done_data);
                case TA_SPEC_MEMBER_ID: return ta_text_node(state->id);
                default: return (ta_node){0};
            }
        }
        case TA_NODE_TRANSITION: {
            const ta_scxml_transition_spec *transition = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_TARGETS: return ta_text_node(transition->targets[i]);
                case TA_SPEC_MEMBER_ACTIONS: return ta_list_node(TA_NODE_ACTION, &transition->actions[i]);
                case TA_SPEC_MEMBER_EVENT: return ta_text_node(transition->event);
                case TA_SPEC_MEMBER_CONDITION: return ta_text_node(ta_expression_text(&transition->condition));
                default: return (ta_node){0};
            }
        }
        case TA_NODE_HANDLER: {
            const ta_scxml_handler_spec *handler = node.ptr;
            if (step.member == TA_SPEC_MEMBER_ACTIONS) return ta_list_node(TA_NODE_ACTION, &handler->actions[i]);
            return (ta_node){0};
        }
        case TA_NODE_INITIAL: {
            const ta_scxml_initial_spec *initial = node.ptr;
            if (step.member == TA_SPEC_MEMBER_TARGETS) return ta_text_node(initial->targets[i]);
            if (step.member == TA_SPEC_MEMBER_ACTIONS) return ta_list_node(TA_NODE_ACTION, &initial->actions[i]);
            return (ta_node){0};
        }
        case TA_NODE_DATA: {
            const ta_scxml_data_spec *data = node.ptr;
            if (step.member == TA_SPEC_MEMBER_NAME) return ta_text_node(data->name);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&data->initializer));
            return (ta_node){0};
        }
        case TA_NODE_PARAM: {
            const ta_scxml_param_spec *param = node.ptr;
            if (step.member == TA_SPEC_MEMBER_NAME) return ta_text_node(param->name);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&param->value));
            return (ta_node){0};
        }
        case TA_NODE_DONE_DATA: {
            const ta_scxml_done_data_spec *done = node.ptr;
            if (step.member == TA_SPEC_MEMBER_PARAMS) return ta_list_node(TA_NODE_PARAM, &done->params[i]);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&done->content));
            return (ta_node){0};
        }
        case TA_NODE_INVOKE: {
            const ta_scxml_invoke_spec *invoke = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_PARAMS: return ta_list_node(TA_NODE_PARAM, &invoke->params[i]);
                case TA_SPEC_MEMBER_FINALIZE: return ta_list_node(TA_NODE_ACTION, &invoke->finalize[i]);
                case TA_SPEC_MEMBER_ID: return ta_text_node(ta_id_text(&invoke->id));
                case TA_SPEC_MEMBER_TYPE: return ta_text_node(ta_string_text(&invoke->type));
                case TA_SPEC_MEMBER_SRC: return ta_text_node(ta_string_text(&invoke->src));
                case TA_SPEC_MEMBER_DATA_VALUE: return ta_text_node(ta_value_text(&invoke->data));
                default: return (ta_node){0};
            }
        }
        case TA_NODE_ACTION:
            return ta_action_step(node.ptr, step);
        default:
            return (ta_node){0};
    }
}

/* The text that stands for a node, if any of it lies within `input`. */
static ta_str ta_node_anchor(ta_node node, ta_bytes input) {
    ta_str candidates[3] = { {0}, {0}, {0} };
    switch (node.kind) {
        case TA_NODE_TEXT: candidates[0] = node.text; break;
        case TA_NODE_SPEC: candidates[0] = ((const ta_scxml_spec *)node.ptr)->name; break;
        case TA_NODE_STATE: candidates[0] = ((const ta_scxml_state_spec *)node.ptr)->id; break;
        case TA_NODE_TRANSITION: {
            const ta_scxml_transition_spec *transition = node.ptr;
            candidates[0] = transition->event;
            candidates[1] = ta_expression_text(&transition->condition);
            candidates[2] = transition->targets != NULL ? transition->targets[0] : (ta_str){0};
            break;
        }
        case TA_NODE_INITIAL: {
            const ta_scxml_initial_spec *initial = node.ptr;
            candidates[0] = initial->targets != NULL ? initial->targets[0] : (ta_str){0};
            break;
        }
        case TA_NODE_DATA: candidates[0] = ((const ta_scxml_data_spec *)node.ptr)->name; break;
        case TA_NODE_PARAM: candidates[0] = ((const ta_scxml_param_spec *)node.ptr)->name; break;
        case TA_NODE_INVOKE: {
            const ta_scxml_invoke_spec *invoke = node.ptr;
            candidates[0] = ta_id_text(&invoke->id);
            candidates[1] = ta_string_text(&invoke->type);
            candidates[2] = ta_string_text(&invoke->src);
            break;
        }
        case TA_NODE_ACTION: {
            /* The first text member the action has. */
            static const ta_spec_member members[] = {
                TA_SPEC_MEMBER_EVENT, TA_SPEC_MEMBER_LOCATION, TA_SPEC_MEMBER_EXPR, TA_SPEC_MEMBER_CONDITION,
                TA_SPEC_MEMBER_SENDID, TA_SPEC_MEMBER_TARGET, TA_SPEC_MEMBER_TYPE, TA_SPEC_MEMBER_ID,
            };
            for (uint32_t index = 0; index < sizeof(members) / sizeof(members[0]); index += 1u) {
                ta_node text = ta_action_step(node.ptr, (ta_spec_step){ .member = members[index],
                                                                        .index = TA_SPEC_INDEX_NONE });
                if (text.kind == TA_NODE_TEXT && ta_import_within(input, text.text.ptr, text.text.len)) {
                    return text.text;
                }
            }
            break;
        }
        default: break;
    }
    for (uint32_t index = 0; index < 3u; index += 1u) {
        if (ta_import_within(input, candidates[index].ptr, candidates[index].len)) {
            return candidates[index];
        }
    }
    return (ta_str){0};
}

/*
 * Where in `input` a compile error is: its subject, if the subject is a
 * slice of the input; otherwise the deepest element on its path whose text
 * is. False when neither. Error path only.
 */
static bool ta_import_locate(const ta_scxml_spec *spec, const ta_compile_error *error, ta_bytes input,
                             uint64_t *offset) {
    const char *begin = (const char *)input.ptr;
    if (error->subject.len != 0 && ta_import_within(input, error->subject.ptr, error->subject.len)) {
        *offset = (uint64_t)(error->subject.ptr - begin) + error->subject_offset;
        return true;
    }
    for (uint32_t depth = error->path_length + 1u; depth > 0; depth -= 1u) {
        ta_node node = { .kind = TA_NODE_SPEC, .ptr = spec };
        for (uint32_t step = 0; step + 1u < depth && node.kind != TA_NODE_NONE; step += 1u) {
            node = ta_node_step(node, error->path[step]);
            if (node.kind != TA_NODE_TEXT && node.ptr == NULL) {
                node.kind = TA_NODE_NONE;
            }
        }
        if (node.kind == TA_NODE_NONE) {
            continue;
        }
        ta_str anchor = ta_node_anchor(node, input);
        if (anchor.ptr != NULL) {
            *offset = (uint64_t)(anchor.ptr - begin);
            return true;
        }
    }
    return false;
}

/*
 * The data model the document names, or the first when it names none. A name
 * no entry has also gets the first, whose compile then refuses the chart
 * with a mismatch at the `datamodel` attribute: one error path, located in
 * the document (ADR 0058, ADR 0076).
 */
static ta_datamodel ta_import_datamodel(const ta_definition_import_params *params, ta_str name) {
    if (name.ptr != NULL && name.len != 0) {
        for (uint32_t index = 0; index < params->datamodel_count; index += 1u) {
            const ta_datamodel *candidate = &params->datamodels[index];
            if (candidate->v != NULL && ta_str_equal(candidate->v->name, name)) {
                return *candidate;
            }
        }
    }
    return params->datamodels[0];
}

ta_result ta_definition_import(ta_system *system, const ta_definition_import_params *params) {
    if (system == NULL || params == NULL || params->codec.v == NULL || params->arena == NULL ||
        params->out == NULL || (params->input.ptr == NULL && params->input.len != 0) ||
        params->datamodels == NULL || params->datamodel_count == 0) {
        return TA_ERR_INVALID;
    }
    if (params->error != NULL) {
        *params->error = (ta_import_error){0};
    }
    if (params->codec.v->decode == NULL) {
        return TA_ERR_UNSUPPORTED;
    }

    const ta_scxml_spec *decoded = NULL;
    ta_chart_error decode_error = {0};
    ta_result result = params->codec.v->decode(params->codec.self,
                                               &(ta_chart_decode_params){
                                                   .input = params->input,
                                                   .name = params->name,
                                                   .arena = params->arena,
                                                   .resolver = params->resolver,
                                                   .lowering = params->lowering,
                                                   .identity = params->identity,
                                                   .out = &decoded,
                                                   .error = &decode_error,
                                               });
    if (result == TA_ERR_INVALID) {
        ta_import_fail(params, &(ta_import_error){ .stage = TA_IMPORT_STAGE_DECODE,
                                                   .as.decode = decode_error,
                                                   .located = true,
                                                   .at = decode_error.at });
        return result;
    }
    if (result != TA_OK) {
        return result;
    }
    if (decoded == NULL) {
        return TA_ERR_INVALID; /* a codec that reports success must produce a spec */
    }

    /* No chart syntax carries an identity, so the caller's is the chart's,
     * whether or not the codec copied it across. */
    ta_scxml_spec *spec = ta_arena_alloc(params->arena, sizeof(ta_scxml_spec), _Alignof(ta_scxml_spec));
    if (spec == NULL) {
        return TA_ERR_NOMEM;
    }
    *spec = *decoded;
    spec->identity = params->identity;

    ta_definition *definition = NULL;
    ta_compile_error compile_error = {0};
    result = ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                    .spec = spec,
                                                    .datamodel = ta_import_datamodel(params, spec->datamodel),
                                                    .allocator = params->allocator,
                                                    .out = &definition,
                                                    .error = params->error != NULL ? &compile_error : NULL,
                                                    .error_arena = params->arena,
                                                });
    if ((result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) && params->error != NULL) {
        ta_import_error error = { .stage = TA_IMPORT_STAGE_COMPILE, .as.compile = compile_error };
        uint64_t offset = 0;
        if (ta_import_locate(spec, &compile_error, params->input, &offset)) {
            error.located = true;
            error.at = ta_import_position(params->input, offset);
        }
        ta_import_fail(params, &error);
        return result;
    }
    if (result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) {
        return result;
    }
    if (result != TA_OK) {
        return result;
    }
    result = ta_system_register_definition(system, definition, params->out);
    if (result != TA_OK) {
        /* Registration takes ownership only on success. */
        ta_definition_destroy(definition);
    }
    return result;
}

/*
 * Encode: export, then the codec's encode. The identity no syntax carries is
 * handed back beside the bytes. Asked where elements landed, the export
 * numbers every element by kind and ID (ADR 0085), and the codec's reports
 * land in one table laid out the same way.
 */

typedef struct {
    ta_source_span *table;
    uint32_t count;
} ta_encode_spans;

static void ta_encode_span_written(void *self, uint32_t span, const ta_source_span *where) {
    ta_encode_spans *spans = self;
    if (span != 0 && span <= spans->count && where != NULL) {
        spans->table[span - 1u] = *where;
    }
}

static const ta_chart_span_sink_vtable ta_encode_span_sink = { .written = ta_encode_span_written };

/* The part of the table for one kind of element; null when it has none. */
static const ta_source_span *ta_encode_spans_of(const ta_encode_spans *spans, const ta_definition *definition,
                                                ta_element_kind kind) {
    if (ta_definition_element_count(definition, kind) == 0) {
        return NULL;
    }
    return spans->table + ta_definition_span_base(definition, kind);
}

ta_result ta_definition_encode(const ta_system *system, const ta_definition_encode_params *params) {
    if (system == NULL || params == NULL || params->codec.v == NULL || params->arena == NULL ||
        params->out == NULL) {
        return TA_ERR_INVALID;
    }
    if (params->error != NULL) {
        *params->error = (ta_chart_error){0};
    }
    if (params->codec.v->encode == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    const ta_scxml_spec *spec = NULL;
    ta_result result = ta_export_spec(system, &(ta_export_request){
                                                  .definition = params->definition,
                                                  .arena = params->arena,
                                                  .numbered = params->spans != NULL,
                                                  .out = &spec,
                                              });
    if (result != TA_OK) {
        return result;
    }
    ta_encode_spans spans = { .count = params->spans != NULL ? spec->span_count : 0 };
    if (spans.count != 0) {
        spans.table = ta_arena_alloc(params->arena, (size_t)spans.count * sizeof(ta_source_span),
                                     _Alignof(ta_source_span));
        if (spans.table == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_zero(spans.table, (size_t)spans.count * sizeof(ta_source_span));
    }
    ta_chart_error encode_error = {0};
    result = params->codec.v->encode(params->codec.self, &(ta_chart_encode_params){
                                                             .spec = spec,
                                                             .arena = params->arena,
                                                             .expressions = params->expressions,
                                                             .spans = { .self = &spans,
                                                                        .v = params->spans != NULL ? &ta_encode_span_sink
                                                                                                   : NULL },
                                                             .out = params->out,
                                                             .error = &encode_error,
                                                         });
    if (result == TA_ERR_INVALID && params->error != NULL) {
        *params->error = encode_error;
    }
    if (result == TA_OK && params->spans != NULL) {
        const ta_definition *definition = ta_definition_lookup(system, params->definition);
        *params->spans = (ta_source_map){
            .states = ta_encode_spans_of(&spans, definition, TA_ELEMENT_STATE),
            .state_count = ta_definition_element_count(definition, TA_ELEMENT_STATE),
            .transitions = ta_encode_spans_of(&spans, definition, TA_ELEMENT_TRANSITION),
            .transition_count = ta_definition_element_count(definition, TA_ELEMENT_TRANSITION),
            .actions = ta_encode_spans_of(&spans, definition, TA_ELEMENT_ACTION),
            .action_count = ta_definition_element_count(definition, TA_ELEMENT_ACTION),
            .expressions = ta_encode_spans_of(&spans, definition, TA_ELEMENT_EXPRESSION),
            .expression_count = ta_definition_element_count(definition, TA_ELEMENT_EXPRESSION),
        };
    }
    if (result == TA_OK && params->identity != NULL) {
        *params->identity = spec->identity;
    }
    return result;
}
