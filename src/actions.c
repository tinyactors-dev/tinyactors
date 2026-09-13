#include "internal.h"

typedef struct action_frame {
    struct action_frame *previous;
    const ta_block_t *block;
    uint32_t index;
    uint8_t loop;
    ta_iteration_t iteration;
    const ta_action_t *action;
    size_t ordinal;
} action_frame_t;

static ta_status_t cb_status(ta_status_t s)
{
    return s == TA_AGAIN ? TA_EXECUTION_ERROR : s;
}

static void value_drop(ta_context_t *c, ta_value_t v)
{
    if (c != NULL && c->actor != NULL && v.handle != 0u &&
        c->actor->system->config.values.release != NULL)
        c->actor->system->config.values.release(c->actor->system->config.host_user, v);
}

static ta_status_t eval_value(ta_context_t *c, ta_code_t code, ta_value_t *out)
{
    ta_data_model_ops_t *ops;
    if (out == NULL || c == NULL || c->actor == NULL) return TA_INVALID;
    *out = (ta_value_t){0};
    ops = &c->actor->system->config.data;
    if (ops->evaluate == NULL) return TA_UNSUPPORTED;
    return cb_status(ops->evaluate(c, code, out));
}

static ta_status_t eval_text(ta_context_t *c, const ta_text_expr_t *e, ta_text_t *out)
{
    ta_value_t v;
    ta_status_t s;
    if (out == NULL || e == NULL) return TA_INVALID;
    *out = e->literal;
    if (e->expression == 0u) return TA_OK;
    s = eval_value(c, e->expression, &v);
    if (s != TA_OK) return s;
    if (c->actor->system->config.data.to_text == NULL) s = TA_UNSUPPORTED;
    else s = cb_status(c->actor->system->config.data.to_text(c, v, out));
    value_drop(c, v);
    return s;
}

static ta_text_t generated_id(ta_context_t *c)
{
    ta_system_t *s = c->actor->system; uint64_t n = s->next_sendid; size_t p = 0, q;
    static const char prefix[] = "send."; char b[25];
    ta_arena_t *arena; ta_text_t result = {0};
    if (n == 0u) return result;
    s->next_sendid = n == UINT64_MAX ? 0 : n + 1;
    while (prefix[p] != '\0') { b[p] = prefix[p]; ++p; }
    q = p; do { b[q++] = (char)('0' + (n % 10u)); n /= 10u; } while (n != 0u);
    { size_t i = p, j = q; while (i < --j) { char x = b[i]; b[i++] = b[j]; b[j] = x; } }
    if (ta_context_arena(c, TA_LIFETIME_MACROSTEP, &arena) == TA_OK)
        (void)tai_text_copy(arena, (ta_text_t){b,q}, &result);
    return result;
}

ta_status_t tai_text_eval(ta_context_t *context, const ta_text_expr_t *expression,
                          ta_text_t *out)
{
    return eval_text(context, expression, out);
}

static ta_status_t do_send(ta_context_t *c, const ta_send_action_t *a)
{
    ta_send_t send;
    ta_text_t event, type, id, text;
    ta_status_t s;
    ta_value_t v, *vals = NULL;
    ta_parameter_t *params = NULL;
    ta_arena_t *scratch = NULL; void *memory;
    ta_body_t body = {0}; ta_target_t target = a->target; uint64_t delay = a->delay_ns;
    size_t i;
    ta_data_model_ops_t *ops = &c->actor->system->config.data;
    s = eval_text(c, &a->event, &event); if (s != TA_OK) return s;
    s = eval_text(c, &a->type, &type); if (s != TA_OK) return s;
    s = ta_context_arena(c, TA_LIFETIME_MACROSTEP, &scratch); if (s != TA_OK) return s;
    id = a->id;
    if (a->idlocation != 0u) {
        ta_value_t generated;
        id = generated_id(c); if (id.data == NULL) return TA_LIMIT;
        s = tai_text_copy(scratch, id, &id); if (s != TA_OK) return s;
        if (ops->from_text == NULL || ops->assign == NULL) return TA_UNSUPPORTED;
        s = cb_status(ops->from_text(c->actor->system->config.host_user, id, &generated));
        if (s != TA_OK) return s;
        s = cb_status(ops->assign(c, a->idlocation, generated)); value_drop(c, generated);
        if (s != TA_OK) return s;
    }
    if (a->target_expression != 0u) {
        s = eval_text(c, &(ta_text_expr_t){.literal={0}, .expression=a->target_expression}, &text);
        if (s != TA_OK) return s;
        target.text = text;
    }
    if (a->delay_expression != 0u) {
        s = eval_value(c, a->delay_expression, &v); if (s != TA_OK) return s;
        if (ops->to_delay == NULL) { value_drop(c, v); return TA_UNSUPPORTED; }
        s = cb_status(ops->to_delay(c, v, &delay)); value_drop(c, v); if (s != TA_OK) return s;
    }
    if (a->body.kind == TA_BODY_CONTENT && a->body.bytes_content != 0u) {
        ta_bytes_t bytes;
        if (ops->evaluate_bytes == NULL) return TA_UNSUPPORTED;
        s = cb_status(ops->evaluate_bytes(c, a->body.bytes_content, &bytes));
        if (s != TA_OK) return s;
        body.kind = TA_BODY_CONTENT; body.content.kind = TA_PAYLOAD_BYTES; body.content.as.bytes = bytes;
    } else if (a->body.kind == TA_BODY_CONTENT && a->body.content != 0u) {
        s = eval_value(c, a->body.content, &v); if (s != TA_OK) return s;
        body.kind = TA_BODY_CONTENT; body.content.kind = TA_PAYLOAD_NATIVE; body.content.as.native = v;
    } else if (a->body.kind == TA_BODY_PARAMETERS) {
        if (a->body.parameter_count != 0u) {
            tai_array_layout_t layout = {a->body.parameter_count,sizeof(*params),TAI_ALIGNMENT};
            s = tai_array(scratch, &layout, &memory); if (s != TA_OK) return s;
            params = (ta_parameter_t *)memory;
            layout.stride=sizeof(*vals);
            s = tai_array(scratch, &layout, &memory); if (s != TA_OK) return s;
            vals = (ta_value_t *)memory;
        }
        body.kind = TA_BODY_PARAMETERS; body.parameters = params; body.parameter_count = a->body.parameter_count;
        for (i = 0; i < body.parameter_count; ++i) {
            params[i] = (ta_parameter_t){a->body.parameters[i].name, {0}};
            if (a->body.parameters[i].expression != 0u) s = eval_value(c, a->body.parameters[i].expression, &vals[i]);
            else if (ops->read_location != NULL) s = cb_status(ops->read_location(c, a->body.parameters[i].location, &vals[i]));
            else s = TA_UNSUPPORTED;
            if (s != TA_OK) { while (i != 0u) value_drop(c, vals[--i]); return s; }
            params[i].value = vals[i];
        }
    }
    send = (ta_send_t){event, type, target, id, body, delay};
    s = cb_status(ta_context_send(c, &send, NULL));
    if (body.kind == TA_BODY_CONTENT && body.content.kind == TA_PAYLOAD_NATIVE) value_drop(c, body.content.as.native);
    for (i = 0; i < body.parameter_count; ++i) value_drop(c, params[i].value);
    return s;
}

static ta_status_t do_action(ta_context_t *c, const ta_action_t *a)
{
    ta_data_model_ops_t *ops = &c->actor->system->config.data;
    ta_status_t s; ta_value_t v; ta_text_t text;
    if (a == NULL) return TA_INVALID;
    switch (a->kind) {
    case TA_ACTION_RAISE: return cb_status(ta_context_raise(c, a->as.raise.event, NULL));
    case TA_ACTION_ASSIGN:
        s = eval_value(c, a->as.assign.value, &v); if (s != TA_OK) return s;
        if (ops->assign == NULL) s = TA_UNSUPPORTED; else s = cb_status(ops->assign(c, a->as.assign.location, v));
        value_drop(c, v); return s;
    case TA_ACTION_SCRIPT: case TA_ACTION_CUSTOM:
        if (ops->run == NULL) return TA_UNSUPPORTED;
        return cb_status(ops->run(c, a->kind, a->kind == TA_ACTION_SCRIPT ? a->as.script.code : a->as.custom.code));
    case TA_ACTION_LOG:
        s = eval_value(c, a->as.log.value, &v); if (s != TA_OK) return s;
        if (ops->log != NULL) ops->log(c->actor->system->config.host_user, &(ta_log_record_t){c->view, a->as.log.label, v});
        value_drop(c, v); return TA_OK;
    case TA_ACTION_SEND: return do_send(c, &a->as.send);
    case TA_ACTION_CANCEL:
        s = eval_text(c, &a->as.cancel.sendid, &text); if (s != TA_OK) return s;
        return cb_status(ta_context_cancel(c, text));
    case TA_ACTION_IF:
    case TA_ACTION_FOREACH: return TA_OK;
    default: return TA_INVALID;
    }
}

ta_status_t tai_execute(ta_context_t *c, const ta_block_t *block)
{
    action_frame_t root = {0}, *top = &root, *free_frames = NULL;
    ta_arena_t *scratch = NULL;
    ta_status_t status = TA_OK;
    size_t depth = 1;
    ta_system_t *system;
    if (c == NULL || c->actor == NULL || block == NULL || (block->count != 0 && block->actions == NULL)) return TA_INVALID;
    if (block->count == 0) return TA_OK;
    system = c->actor->system;
    if (system->config.limits.action_depth == 0) return TA_LIMIT;
    root.block = block;
    while (top != NULL) {
        action_frame_t *frame = top;
        const ta_block_t *child = NULL;
        if (frame->loop) {
            if (frame->ordinal == frame->iteration.count) {
                value_drop(c, frame->iteration.snapshot);
                frame->iteration = (ta_iteration_t){0};
                frame->loop = 0;
                continue;
            }
            if (c->actor->actions >= c->actor->limits->actions_per_macrostep) { status = TA_FAULTED; break; }
            ++c->actor->actions;
            status = cb_status(system->config.data.iteration_bind(c,
                &(ta_iteration_step_t){frame->iteration.snapshot,frame->ordinal++,frame->action->as.foreach_loop.item,frame->action->as.foreach_loop.index}));
            if (status != TA_OK) break;
            child = &frame->action->as.foreach_loop.body;
        } else if (frame->index == frame->block->count) {
            top = frame->previous;
            if (frame != &root) { frame->previous = free_frames; free_frames = frame; }
            --depth;
            continue;
        } else {
            const ta_action_t *action = &frame->block->actions[frame->index++];
            frame->action = action;
            if (c->actor->actions >= c->actor->limits->actions_per_macrostep) { status = TA_FAULTED; break; }
            ++c->actor->actions;
            if (action->kind == TA_ACTION_FOREACH) {
                status = cb_status(system->config.data.iteration_open(c, action->as.foreach_loop.collection, &frame->iteration));
                if (status != TA_OK) break;
                frame->loop = 1;
                frame->ordinal = 0;
                continue;
            } else if (action->kind == TA_ACTION_IF) {
                uint32_t i;
                for (i = 0; i < action->as.conditional.count; ++i) {
                    const ta_branch_t *branch = &action->as.conditional.branches[i];
                    uint8_t yes = 1;
                    if (branch->condition != 0) status = cb_status(system->config.data.test(c, branch->condition, &yes));
                    if (status != TA_OK) break;
                    if (yes) { child = &branch->body; break; }
                }
                if (status != TA_OK) break;
            } else {
                status = do_action(c, action);
                if (status != TA_OK) break;
            }
        }
        if (child != NULL && child->count != 0) {
            action_frame_t *next = free_frames;
            if (depth >= system->config.limits.action_depth) { status = TA_LIMIT; break; }
            if (next != NULL) free_frames = next->previous;
            else {
                if (scratch == NULL) status = tai_actor_scope(c->actor, TA_LIFETIME_MACROSTEP, &scratch);
                if (status == TA_OK) status = tai_alloc(scratch, sizeof(*next), (void **)&next);
                if (status != TA_OK) break;
            }
            *next = (action_frame_t){0};
            next->previous = top;
            next->block = child;
            top = next;
            ++depth;
        }
    }
    while (top != NULL) {
        if (top->loop) value_drop(c, top->iteration.snapshot);
        top = top->previous;
    }
    if (scratch != NULL) ta_arena_close(scratch);
    return status;
}
