#include "internal.h"

typedef struct validation {
    ta_definition_t *candidate;
    const ta_definition_desc_t *description;
    const ta_data_model_ops_t *ops;
    ta_diagnostic_t *error;
    ta_arena_t *scratch;
    size_t actions;
    uint8_t finalize;
} validation_t;

typedef struct action_frame {
    struct action_frame *parent;
    struct action_frame *free_next;
    ta_block_t block;
    uint32_t index;
    const ta_branch_t *branches;
    uint32_t branch_count;
    uint32_t branch_index;
} action_frame_t;

static ta_status_t invalid(validation_t *v, const char *detail)
{
    v->error->detail = ta_text_from_cstr(detail);
    return TA_INVALID;
}

static ta_status_t unsupported(validation_t *v, const char *detail)
{
    v->error->detail = ta_text_from_cstr(detail);
    return TA_UNSUPPORTED;
}

static uint8_t history(ta_state_kind_t kind)
{
    return kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
}

static uint8_t descendant(const ta_definition_t *d, uint32_t child, uint32_t parent)
{
    return child > parent && child < d->subtree_end[parent];
}

static uint8_t native_hooks(const validation_t *v)
{
    const ta_value_ops_t *values = &v->candidate->system->config.values;
    return values->retain != NULL && values->release != NULL && values->snapshot != NULL;
}

static uint8_t text_expression_valid(ta_text_expr_t operand)
{
    return tai_text_valid(operand.literal, 1) &&
           !(operand.literal.data != NULL && operand.expression != 0);
}

static ta_status_t text_expression(validation_t *v, ta_text_expr_t operand)
{
    if (!text_expression_valid(operand)) return invalid(v, "literal and expression conflict");
    if (operand.expression != 0 &&
        (!native_hooks(v) || v->ops->evaluate == NULL || v->ops->to_text == NULL))
        return unsupported(v, "text expression callbacks missing");
    return TA_OK;
}

static ta_status_t body(validation_t *v, const ta_body_expr_t *value, uint8_t pack)
{
    uint32_t i;
    if (value->kind < TA_BODY_NONE || value->kind > TA_BODY_PARAMETERS ||
        (value->parameter_count != 0 && value->parameters == NULL))
        return invalid(v, "invalid body");
    if (value->kind == TA_BODY_NONE) {
        if (value->content != 0 || value->bytes_content != 0 || value->parameter_count != 0 ||
            value->parameters != NULL) return invalid(v, "nonempty absent body");
    } else if (value->kind == TA_BODY_CONTENT) {
        if ((value->content == 0) == (value->bytes_content == 0) ||
            value->parameters != NULL || value->parameter_count != 0)
            return invalid(v, "content must have exactly one expression");
        if (value->content != 0 && (!native_hooks(v) || v->ops->evaluate == NULL))
            return unsupported(v, "native content callbacks missing");
        if (value->bytes_content != 0 && v->ops->evaluate_bytes == NULL)
            return unsupported(v, "byte content callback missing");
    } else {
        if (value->content != 0 || value->bytes_content != 0)
            return invalid(v, "parameters conflict with content");
        if ((value->parameter_count != 0 && !native_hooks(v)) ||
            (pack && (!native_hooks(v) || v->ops->pack_body == NULL)))
            return unsupported(v, "parameter packing callbacks missing");
        for (i = 0; i < value->parameter_count; ++i) {
            const ta_parameter_expr_t *p = &value->parameters[i];
            if (!tai_text_valid(p->name, 0) || (p->expression == 0) == (p->location == 0))
                return invalid(v, "invalid parameter");
            if ((p->expression != 0 && v->ops->evaluate == NULL) ||
                (p->location != 0 && v->ops->read_location == NULL))
                return unsupported(v, "parameter evaluation callbacks missing");
        }
    }
    return TA_OK;
}

static ta_status_t action(validation_t *v, const ta_action_t *a)
{
    ta_status_t status;
    uint32_t i;
    if (a->kind < TA_ACTION_RAISE || a->kind > TA_ACTION_CUSTOM)
        return invalid(v, "unknown action kind");
    if (v->finalize && (a->kind == TA_ACTION_RAISE || a->kind == TA_ACTION_SEND ||
                        a->kind == TA_ACTION_CANCEL)) return invalid(v, "effect in finalize");
    switch (a->kind) {
    case TA_ACTION_RAISE:
        if (!tai_text_valid(a->as.raise.event, 0)) return invalid(v, "empty raised event");
        if (a->as.raise.event.size > v->candidate->system->config.limits.event_name_bytes)
            return TA_LIMIT;
        break;
    case TA_ACTION_ASSIGN:
        if (a->as.assign.location == 0 || a->as.assign.value == 0)
            return invalid(v, "assignment needs location and value");
        if (!native_hooks(v) || v->ops->evaluate == NULL || v->ops->assign == NULL)
            return unsupported(v, "assignment callbacks missing");
        break;
    case TA_ACTION_SCRIPT:
    case TA_ACTION_CUSTOM:
        if ((a->kind == TA_ACTION_SCRIPT ? a->as.script.code : a->as.custom.code) == 0)
            return invalid(v, "missing executable token");
        if (v->ops->run == NULL) return unsupported(v, "run callback missing");
        break;
    case TA_ACTION_IF:
        if (a->as.conditional.count == 0 || a->as.conditional.branches == NULL)
            return invalid(v, "if has no branches");
        for (i = 0; i < a->as.conditional.count; ++i) {
            if (a->as.conditional.branches[i].condition == 0) {
                if (i != a->as.conditional.count - 1) return invalid(v, "else is not last");
            } else if (v->ops->test == NULL) return unsupported(v, "condition callback missing");
        }
        break;
    case TA_ACTION_FOREACH:
        if (a->as.foreach_loop.collection == 0 || !tai_text_valid(a->as.foreach_loop.item, 0) ||
            !tai_text_valid(a->as.foreach_loop.index, 1)) return invalid(v, "invalid foreach");
        if (!native_hooks(v) || v->ops->iteration_open == NULL || v->ops->iteration_bind == NULL)
            return unsupported(v, "iteration callbacks missing");
        break;
    case TA_ACTION_LOG:
        if (!tai_text_valid(a->as.log.label, 1)) return invalid(v, "invalid log label");
        if (v->ops->log == NULL || (a->as.log.value != 0 &&
            (!native_hooks(v) || v->ops->evaluate == NULL)))
            return unsupported(v, "log callbacks missing");
        break;
    case TA_ACTION_SEND: {
        const ta_send_action_t *send = &a->as.send;
        status = text_expression(v, send->event);
        if (status == TA_OK) status = text_expression(v, send->type);
        if (status == TA_OK) status = body(v, &send->body,
            send->type.expression != 0 || tai_processor(v->candidate->system,send->type.literal) ==
                &v->candidate->system->config.ioprocessors[0]);
        if (status != TA_OK) return status;
        if (send->event.literal.data == NULL && send->event.expression == 0)
            return invalid(v, "send event missing");
        if (send->target.kind < TA_TARGET_SELF || send->target.kind > TA_TARGET_URI ||
            !tai_text_valid(send->target.text, 1) || !tai_text_valid(send->id, 1) ||
            (send->delay_ns != 0 && send->delay_expression != 0) ||
            (send->id.data != NULL && send->idlocation != 0))
            return invalid(v, "conflicting send operands");
        if (send->target_expression != 0) {
            if ((send->target.kind != TA_TARGET_NAME && send->target.kind != TA_TARGET_URI) ||
                send->target.text.data != NULL) return invalid(v, "invalid target expression");
            if (!native_hooks(v) || v->ops->evaluate == NULL || v->ops->to_text == NULL)
                return unsupported(v, "target expression callbacks missing");
        }
        if (send->delay_expression != 0 && (!native_hooks(v) ||
            v->ops->evaluate == NULL || v->ops->to_delay == NULL))
            return unsupported(v, "delay callbacks missing");
        if (send->idlocation != 0 && (!native_hooks(v) || v->ops->from_text == NULL ||
                                     v->ops->assign == NULL))
            return unsupported(v, "send ID callbacks missing");
        break;
    }
    case TA_ACTION_CANCEL:
        status = text_expression(v, a->as.cancel.sendid);
        if (status != TA_OK) return status;
        if (a->as.cancel.sendid.literal.data == NULL && a->as.cancel.sendid.expression == 0)
            return invalid(v, "cancel sendid missing");
        break;
    }
    return TA_OK;
}

static ta_status_t block(validation_t *v, ta_block_t initial)
{
    action_frame_t first = {0}, *top = &first, *free_frames = NULL;
    size_t depth = 1;
    ta_block_t child;
    ta_status_t status;
    if (initial.count == 0) return TA_OK;
    if (initial.actions == NULL) return invalid(v, "missing action array");
    if (v->candidate->system->config.limits.action_depth == 0) return TA_LIMIT;
    first.block = initial;
    while (top != NULL) {
        child = (ta_block_t){0};
        if (top->branch_index < top->branch_count) {
            child = top->branches[top->branch_index++].body;
        } else if (top->index < top->block.count) {
            const ta_action_t *a = &top->block.actions[top->index++];
            if (v->actions >= v->candidate->system->config.limits.definition_actions) return TA_LIMIT;
            ++v->actions;
            status = action(v, a);
            if (status != TA_OK) return status;
            if (a->kind == TA_ACTION_IF) {
                top->branches = a->as.conditional.branches;
                top->branch_count = a->as.conditional.count;
                top->branch_index = 0;
            } else if (a->kind == TA_ACTION_FOREACH) child = a->as.foreach_loop.body;
        } else {
            action_frame_t *done = top;
            top = top->parent;
            if (done != &first) { done->free_next = free_frames; free_frames = done; }
            --depth;
            continue;
        }
        if (child.count != 0) {
            action_frame_t *frame, *ancestor;
            void *memory;
            if (child.actions == NULL) return invalid(v, "missing nested actions");
            if (depth >= v->candidate->system->config.limits.action_depth) return TA_LIMIT;
            for (ancestor = top; ancestor != NULL; ancestor = ancestor->parent)
                if (ancestor->block.actions == child.actions) return invalid(v, "cyclic action graph");
            if (free_frames != NULL) { frame = free_frames; free_frames = frame->free_next; }
            else {
                status = tai_alloc(v->scratch, sizeof(*frame), &memory);
                if (status != TA_OK) return status;
                frame = (action_frame_t *)memory;
            }
            *frame = (action_frame_t){0};
            frame->block = child;
            frame->parent = top;
            top = frame;
            ++depth;
        }
    }
    return TA_OK;
}

static ta_status_t blocks(validation_t *v, ta_blocks_t values)
{
    uint32_t i;
    if (values.count != 0 && values.blocks == NULL) return invalid(v, "missing blocks");
    for (i = 0; i < values.count; ++i) {
        ta_status_t status = block(v, values.blocks[i]);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

static ta_status_t targets(validation_t *v, ta_targets_t values)
{
    uint32_t i, j;
    const ta_definition_desc_t *d = v->description;
    if (values.count != 0 && values.states == NULL) return invalid(v, "missing target array");
    for (i = 0; i < values.count; ++i) {
        uint32_t a = values.states[i];
        if (a == 0 || a >= d->state_count) return invalid(v, "target outside definition");
        for (j = 0; j < i; ++j) {
            uint32_t b = values.states[j], x, y;
            if (a == b || descendant(v->candidate, a, b) || descendant(v->candidate, b, a))
                return invalid(v, "duplicate or nested target set");
            /* Divergence below a compound is incompatible; below parallel it
             * selects different regions. History defaults are expanded later. */
            if (history(d->states[a].kind) || history(d->states[b].kind)) continue;
            x = a; y = b;
            while (v->candidate->depth[x] > v->candidate->depth[y]) x = d->states[x].parent;
            while (v->candidate->depth[y] > v->candidate->depth[x]) y = d->states[y].parent;
            while (d->states[x].parent != d->states[y].parent) {
                x = d->states[x].parent; y = d->states[y].parent;
            }
            if (d->states[d->states[x].parent].kind != TA_STATE_PARALLEL)
                return invalid(v, "targets select multiple compound children");
        }
    }
    return TA_OK;
}

static ta_status_t topology(validation_t *v)
{
    ta_definition_t *candidate = v->candidate;
    const ta_definition_desc_t *d = v->description;
    tai_array_layout_t layout = { d->state_count, sizeof(uint32_t), TAI_ALIGNMENT };
    void *memory;
    uint32_t i, j, previous = 0;
    size_t transitions_seen = 0;
    ta_status_t status = tai_array(candidate->arena, &layout, &memory);
    if (status != TA_OK) return status;
    candidate->depth = (uint32_t *)memory;
    status = tai_array(candidate->arena, &layout, &memory);
    if (status != TA_OK) return status;
    candidate->subtree_end = (uint32_t *)memory;
    for (i = 0; i < d->state_count; ++i) {
        const ta_state_desc_t *s = &d->states[i];
        v->error->state = i;
        if (!tai_text_valid(s->name, 0) || s->kind < TA_STATE_ROOT || s->kind > TA_STATE_HISTORY_DEEP)
            return invalid(v, "invalid state name or kind");
        if (i == 0) {
            if (s->kind != TA_STATE_ROOT || s->parent != UINT32_MAX)
                return invalid(v, "invalid synthetic root");
            candidate->depth[i] = 0;
        } else {
            if (s->parent >= i || s->kind == TA_STATE_ROOT)
                return invalid(v, "parent must precede child");
            while (previous != s->parent && previous != 0) previous = d->states[previous].parent;
            if (previous != s->parent) return invalid(v, "noncontiguous state subtree");
            candidate->depth[i] = candidate->depth[s->parent] + 1;
        }
        if ((size_t)candidate->depth[i] + 1 > candidate->system->config.limits.state_depth)
            return TA_LIMIT;
        previous = i;
        candidate->subtree_end[i] = i + 1;
        for (j = 0; j < i; ++j)
            if (tai_text_compare(s->name, d->states[j].name) == 0)
                return invalid(v, "duplicate state ID");
        if (s->first_transition > d->transition_count ||
            s->transition_count > d->transition_count - s->first_transition)
            return invalid(v, "transition range out of bounds");
        if (s->transition_count != 0 && s->first_transition != transitions_seen)
            return invalid(v, "transition slices not in source order");
        for (j = 0; j < s->transition_count; ++j)
            if (d->transitions[s->first_transition + j].source != i)
                return invalid(v, "transition source disagrees with owner");
        transitions_seen += s->transition_count;
    }
    if (transitions_seen != d->transition_count) return invalid(v, "unowned transitions");
    for (i = d->state_count; i-- > 1;) {
        uint32_t parent = d->states[i].parent;
        if (candidate->subtree_end[parent] < candidate->subtree_end[i])
            candidate->subtree_end[parent] = candidate->subtree_end[i];
    }
    return TA_OK;
}

static ta_status_t invocation(validation_t *v, const ta_invoke_desc_t *invoke)
{
    ta_status_t status = text_expression(v, invoke->type);
    uint8_t scxml;
    if (status == TA_OK) status = text_expression(v, invoke->source);
    if (status == TA_OK) status = body(v, &invoke->body, 0);
    if (status != TA_OK) return status;
    if (!tai_text_valid(invoke->id, 1) || (invoke->id.data != NULL && invoke->idlocation != 0) ||
        invoke->autoforward > 1 || invoke->finalize_kind < TA_FINALIZE_ABSENT ||
        invoke->finalize_kind > TA_FINALIZE_BLOCK)
        return invalid(v, "invalid invoke operands");
    if (invoke->idlocation != 0 && (!native_hooks(v) || v->ops->from_text == NULL ||
                                    v->ops->assign == NULL))
        return unsupported(v, "invoke ID callbacks missing");
    scxml = (uint8_t)(invoke->type.expression == 0 &&
        (invoke->type.literal.data == NULL ||
         tai_text_compare(invoke->type.literal, ta_text_from_cstr("scxml")) == 0 ||
         tai_text_compare(invoke->type.literal, ta_text_from_cstr(TA_SCXML_INVOKE_TYPE)) == 0));
    if (scxml && invoke->body.kind == TA_BODY_CONTENT)
        return invalid(v, "SCXML invoke content is a definition, not value data");
    if (invoke->inline_definition != NULL &&
        (invoke->source.literal.data != NULL || invoke->source.expression != 0 ||
         invoke->inline_definition->system != v->candidate->system ||
         (!scxml && invoke->type.expression == 0)))
        return invalid(v, "incompatible inline definition");
    if (invoke->finalize_kind != TA_FINALIZE_BLOCK && invoke->finalize.count != 0)
        return invalid(v, "unexpected finalize block");
    if (invoke->finalize_kind == TA_FINALIZE_AUTOMATIC && v->ops->finalize == NULL)
        return unsupported(v, "automatic finalize callback missing");
    if (invoke->finalize_kind == TA_FINALIZE_BLOCK) {
        v->finalize = 1;
        status = block(v, invoke->finalize);
        v->finalize = 0;
    }
    return status;
}

static ta_status_t states(validation_t *v)
{
    const ta_definition_desc_t *d = v->description;
    uint32_t i, j, k;
    for (i = 0; i < d->state_count; ++i) {
        const ta_state_desc_t *s = &d->states[i];
        uint32_t real_children = 0, all_children = 0;
        ta_status_t status;
        v->error->state = i;
        if ((s->data_count != 0 && s->data == NULL) ||
            (s->invoke_count != 0 && s->invokes == NULL)) return invalid(v, "missing state arrays");
        for (j = i + 1; j < v->candidate->subtree_end[i]; ++j) {
            if (d->states[j].parent == i) {
                ++all_children;
                if (!history(d->states[j].kind)) ++real_children;
            }
        }
        if (i != 0) {
            ta_state_kind_t parent = d->states[s->parent].kind;
            if (parent != TA_STATE_ROOT && parent != TA_STATE_COMPOUND && parent != TA_STATE_PARALLEL)
                return invalid(v, "children of a non-container state");
            if (history(s->kind) && parent != TA_STATE_COMPOUND && parent != TA_STATE_PARALLEL)
                return invalid(v, "invalid history parent");
            if (s->kind == TA_STATE_FINAL && parent != TA_STATE_ROOT && parent != TA_STATE_COMPOUND)
                return invalid(v, "invalid final parent");
        }
        if ((s->kind == TA_STATE_ROOT || s->kind == TA_STATE_COMPOUND || s->kind == TA_STATE_PARALLEL)
            && real_children == 0) return invalid(v, "container has no real children");
        if ((s->kind == TA_STATE_ATOMIC || s->kind == TA_STATE_FINAL || history(s->kind)) &&
            all_children != 0) return invalid(v, "leaf has children");
        if (s->kind == TA_STATE_ROOT &&
            (s->transition_count != 0 || s->onentry.count != 0 || s->onexit.count != 0 || s->invoke_count != 0))
            return invalid(v, "root has state executable content");
        if (s->kind == TA_STATE_FINAL && (s->transition_count != 0 || s->invoke_count != 0 || s->data_count != 0))
            return invalid(v, "final has forbidden content");
        if (history(s->kind) && (s->onentry.count != 0 || s->onexit.count != 0 || s->data_count != 0 ||
                                  s->transition_count != 0 || s->invoke_count != 0))
            return invalid(v, "history has forbidden content");
        if (s->kind != TA_STATE_FINAL && (s->done_data.kind != TA_BODY_NONE || s->done_data.content != 0 ||
            s->done_data.bytes_content != 0 || s->done_data.parameter_count != 0 || s->done_data.parameters != NULL))
            return invalid(v, "done data outside final state");
        if (s->kind == TA_STATE_ATOMIC || s->kind == TA_STATE_FINAL || s->kind == TA_STATE_PARALLEL) {
            if (s->initial.targets.count != 0 || s->initial.content.count != 0)
                return invalid(v, "initial transition outside compound/root/history");
        }
        if (history(s->kind) && s->initial.targets.count == 0) return invalid(v, "history default missing");
        if (s->initial.targets.count == 0 && s->initial.content.count != 0)
            return invalid(v, "implicit initial transition has content");
        status = targets(v, s->initial.targets);
        if (status != TA_OK) return status;
        for (j = 0; j < s->initial.targets.count; ++j) {
            uint32_t target = s->initial.targets.states[j];
            uint32_t owner = history(s->kind) ? s->parent : i;
            if (!descendant(v->candidate, target, owner)) return invalid(v, "initial/default target outside owner");
            if (s->kind == TA_STATE_HISTORY_SHALLOW &&
                (d->states[target].parent != owner || history(d->states[target].kind)))
                return invalid(v, "shallow history default must select real children");
        }
        status = block(v, s->initial.content);
        if (status == TA_OK) status = blocks(v, s->onentry);
        if (status == TA_OK) status = blocks(v, s->onexit);
        if (status == TA_OK) status = body(v, &s->done_data, 1);
        if (status != TA_OK) return status;
        if (s->data_count != 0 && (v->ops->declare == NULL || v->ops->initialize == NULL))
            return unsupported(v, "data declaration callbacks missing");
        for (j = 0; j < s->data_count; ++j) {
            ta_text_t id = s->data[j].id;
            if (!tai_text_valid(id, 0) || id.data[0] == '_') return invalid(v, "reserved/invalid data ID");
            for (k = 0; k <= i; ++k) {
                uint32_t n, end = k == i ? j : d->states[k].data_count;
                for (n = 0; n < end; ++n)
                    if (tai_text_compare(id, d->states[k].data[n].id) == 0)
                        return invalid(v, "duplicate data ID");
            }
        }
        for (j = 0; j < s->invoke_count; ++j) {
            status = invocation(v, &s->invokes[j]);
            if (status != TA_OK) return status;
        }
    }
    return TA_OK;
}

static ta_status_t transitions(validation_t *v)
{
    uint32_t i, j;
    for (i = 0; i < v->description->transition_count; ++i) {
        const ta_transition_desc_t *t = &v->description->transitions[i];
        ta_status_t status;
        v->error->transition = i;
        v->error->state = t->source;
        if (t->kind < TA_TRANSITION_EXTERNAL || t->kind > TA_TRANSITION_INTERNAL ||
            (t->event_count != 0 && t->events == NULL)) return invalid(v, "invalid transition");
        if (t->condition != 0 && v->ops->test == NULL) return unsupported(v, "guard callback missing");
        for (j = 0; j < t->event_count; ++j) {
            size_t n;
            if (!tai_text_valid(t->events[j], 0)) return invalid(v, "empty event descriptor");
            if (t->events[j].size > v->candidate->system->config.limits.event_name_bytes) return TA_LIMIT;
            for (n = 0; n < t->events[j].size; ++n) {
                char c = t->events[j].data[n];
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                    return invalid(v, "multiple descriptors in one token");
            }
        }
        status = targets(v, t->targets);
        if (status == TA_OK) status = block(v, t->content);
        if (status != TA_OK) return status;
    }
    v->error->transition = UINT32_MAX;
    return TA_OK;
}

typedef struct history_frame { uint32_t state; uint32_t index; } history_frame_t;
typedef struct target_work {
    uint32_t *pending;
    uint32_t *resolved;
    uint8_t *seen;
} target_work_t;

static ta_status_t resolve_targets(validation_t *v, ta_targets_t list, target_work_t *work)
{
    const ta_definition_desc_t *d = v->description;
    size_t pending = 0;
    uint32_t i, count = 0;
    tai_zero(work->seen, d->state_count);
    for (i = 0; i < list.count; ++i) {
        uint32_t target = list.states[i];
        if (!work->seen[target]) { work->seen[target] = 1; work->pending[pending++] = target; }
    }
    while (pending != 0) {
        uint32_t target = work->pending[--pending];
        const ta_state_desc_t *s = &d->states[target];
        if (!history(s->kind)) work->resolved[count++] = target;
        else {
            for (i = 0; i < s->initial.targets.count; ++i) {
                uint32_t child = s->initial.targets.states[i];
                if (!work->seen[child]) { work->seen[child] = 1; work->pending[pending++] = child; }
            }
        }
    }
    return targets(v, (ta_targets_t){ work->resolved, count });
}

static ta_status_t history_defaults(validation_t *v)
{
    const ta_definition_desc_t *d = v->description;
    tai_array_layout_t layout = {d->state_count, sizeof(history_frame_t), TAI_ALIGNMENT};
    history_frame_t *stack;
    target_work_t work;
    uint8_t *color;
    uint32_t i;
    void *memory;
    ta_status_t status = tai_array(v->scratch, &layout, &memory);
    if (status != TA_OK) return status;
    stack = (history_frame_t *)memory;
    layout.stride = 1;
    status = tai_array(v->scratch, &layout, &memory);
    if (status != TA_OK) return status;
    color = (uint8_t *)memory;
    tai_zero(color, d->state_count);
    for (i = 1; i < d->state_count; ++i) {
        size_t depth = 0;
        if (!history(d->states[i].kind) || color[i] != 0) continue;
        stack[depth++] = (history_frame_t){i, 0};
        color[i] = 1;
        while (depth != 0) {
            history_frame_t *frame = &stack[depth - 1];
            ta_targets_t defaults = d->states[frame->state].initial.targets;
            if (frame->index == defaults.count) { color[frame->state] = 2; --depth; }
            else {
                uint32_t child = defaults.states[frame->index++];
                if (!history(d->states[child].kind)) continue;
                if (color[child] == 1) return invalid(v, "recursive history defaults");
                if (color[child] == 0) {
                    color[child] = 1;
                    stack[depth++] = (history_frame_t){child, 0};
                }
            }
        }
    }
    work.seen = color;
    layout.stride = sizeof(uint32_t);
    status = tai_array(v->scratch, &layout, &memory);
    if (status != TA_OK) return status;
    work.pending = (uint32_t *)memory;
    status = tai_array(v->scratch, &layout, &memory);
    if (status != TA_OK) return status;
    work.resolved = (uint32_t *)memory;
    for (i = 0; i < d->state_count; ++i) {
        v->error->state = i;
        status = resolve_targets(v, d->states[i].initial.targets, &work);
        if (status != TA_OK) return status;
    }
    for (i = 0; i < d->transition_count; ++i) {
        v->error->state = d->transitions[i].source;
        v->error->transition = i;
        status = resolve_targets(v, d->transitions[i].targets, &work);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

ta_status_t tai_definition_validate(ta_definition_t *candidate, ta_diagnostic_t *error)
{
    validation_t v = {0};
    const ta_definition_desc_t *d = &candidate->description;
    const ta_system_config_t *config = &candidate->system->config;
    const ta_data_layout_t *data = &d->data_layout;
    ta_status_t status;
    v.candidate = candidate;
    v.description = d;
    v.ops = &config->data;
    v.error = error;
    *error = (ta_diagnostic_t){ .state = UINT32_MAX, .transition = UINT32_MAX };
    if (d->state_count < 2 || d->states == NULL ||
        (d->transition_count != 0 && d->transitions == NULL) ||
        !tai_text_valid(d->name, 1) || !tai_text_valid(d->data_model, 0) ||
        tai_text_compare(d->data_model, config->data_model) != 0 ||
        d->binding < TA_BINDING_EARLY || d->binding > TA_BINDING_LATE ||
        data->storage < TA_DATA_NATIVE || data->storage > TA_DATA_BYTES)
        return invalid(&v, "invalid definition metadata");
    if (d->state_count > config->limits.definition_states ||
        d->transition_count > config->limits.definition_transitions) return TA_LIMIT;
    if (!tai_text_valid(data->schema, 1)) return invalid(&v, "invalid schema text");
    if (data->storage == TA_DATA_NATIVE) {
        if (data->bytes_layout.size != 0 || data->bytes_layout.alignment != 0 ||
            data->initial_bytes.data != NULL || data->initial_bytes.size != 0)
            return invalid(&v, "byte layout on native definition");
        if (!native_hooks(&v) || v.ops->create == NULL || v.ops->bind_system == NULL ||
            v.ops->bind_event == NULL) return unsupported(&v, "native storage callbacks missing");
    } else {
        size_t alignment = data->bytes_layout.alignment;
        if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
            (data->bytes_layout.size == 0 && alignment != 1) ||
            (data->initial_bytes.data == NULL && data->initial_bytes.size != 0) ||
            (data->initial_bytes.data != NULL && data->initial_bytes.size != data->bytes_layout.size))
            return invalid(&v, "invalid byte layout/image");
    }
    if (d->code_owner.handle != 0 && !native_hooks(&v)) return unsupported(&v, "code owner hooks missing");
    if (d->root_script != 0 && v.ops->run == NULL) return unsupported(&v, "root script callback missing");
    status = ta_arena_open(candidate->arena, &config->arenas[TA_LIFETIME_DEFINITION], &v.scratch);
    if (status != TA_OK) return status;
    status = topology(&v);
    if (status == TA_OK) status = states(&v);
    if (status == TA_OK) status = transitions(&v);
    if (status == TA_OK) status = history_defaults(&v);
    if (status == TA_OK && config->validate_code != NULL)
        status = config->validate_code(config->host_user, d, error);
    ta_arena_close(v.scratch);
    if (status != TA_OK) error->status = status;
    return status;
}
