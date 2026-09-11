#include "internal.h"

extern ta_status_t tai_definition_validate(ta_definition_t *candidate,
                                           ta_diagnostic_t *error);

/* Definition preparation deliberately keeps all validation local to the
 * immutable revision.  The implementation uses arenas, so failure unwinds by
 * closing the definition arena; no individual object needs a destructor. */

static const char ta_def_bad_input[] = "invalid definition input";

static void ta_def_error(ta_definition_result_t *out, ta_status_t status)
{
    if (out == NULL) return;
    out->definition = NULL;
    out->error.status = status;
    out->error.actor = (ta_actor_ref_t){0};
    out->error.revision = 0;
    out->error.state = TA_INDEX_NONE;
    out->error.transition = TA_INDEX_NONE;
    out->error.detail = (ta_text_t){ta_def_bad_input, sizeof(ta_def_bad_input)-1u};
}

static ta_status_t ta_def_text(ta_arena_t *arena, ta_text_t in, ta_text_t *out)
{ return tai_text_copy(arena, in, out); }

static ta_status_t ta_def_bytes(ta_arena_t *arena, ta_bytes_t in, ta_bytes_t *out)
{
    void *p;
    ta_status_t s;
    if (out == NULL || (in.data == NULL && in.size != 0u)) return TA_INVALID;
    *out = (ta_bytes_t){NULL, 0};
    if (in.size == 0u) return TA_OK;
    s = tai_alloc(arena, in.size, &p);
    if (s != TA_OK) return s;
    tai_copy(p, in.data, in.size);
    *out = (ta_bytes_t){p, in.size};
    return TA_OK;
}

typedef struct ta_def_array_request { ta_arena_t *arena; size_t count; size_t stride; const void *source; void **out; } ta_def_array_request_t;
static ta_status_t ta_def_array(const ta_def_array_request_t *request, void *unused)
{
    ta_arena_t *arena = request->arena;
    size_t count=request->count, stride=request->stride; const void *source=request->source; void **out=request->out;
    tai_array_layout_t l;
    ta_status_t s;
    if (unused != NULL) return TA_INVALID;
    if (out == NULL || (source == NULL && count != 0u)) return TA_INVALID;
    *out = NULL;
    l.count = count; l.stride = stride; l.alignment = TAI_ALIGNMENT;
    s = tai_array(arena, &l, out);
    if (s == TA_OK && count != 0u) tai_copy(*out, source, count * stride);
    return s;
}


typedef struct ta_def_frame ta_def_frame_t;
typedef struct ta_def_copy_context {
    ta_arena_t *arena;
    ta_arena_t *scratch;
    ta_def_frame_t *top;
    ta_def_frame_t *free_frames;
} ta_def_copy_context_t;
struct ta_def_frame {
    ta_def_frame_t *next;
    const ta_block_t *in;
    ta_block_t *out;
    uint32_t index;
    uint32_t branch;
    uint32_t branch_count;
    const ta_block_t *child_in;
    ta_block_t *child_out;
};

static ta_status_t ta_def_frame_block(ta_def_copy_context_t *ctx,
                                      const ta_block_t *in, ta_block_t *out)
{
    ta_status_t s; ta_def_frame_t *f; void *memory;
    *out = *in;
    if (in->count != 0u && in->actions == NULL) return TA_INVALID;
    if (in->count == 0u) { out->actions = NULL; return TA_OK; }
    s = ta_def_array(&(ta_def_array_request_t){ctx->arena,in->count,sizeof(ta_action_t),in->actions,(void **)&out->actions}, NULL);
    if (s != TA_OK) return s;
    if (ctx->free_frames != NULL) { f = ctx->free_frames; ctx->free_frames = f->next; }
    else { s = tai_alloc(ctx->scratch, sizeof(*f), &memory); if (s != TA_OK) return s; f = (ta_def_frame_t *)memory; }
    *f = (ta_def_frame_t){ctx->top, in, out, 0u, 0u, 0u, NULL, NULL}; ctx->top = f;
    return TA_OK;
}

static ta_status_t ta_def_body(ta_def_copy_context_t *ctx, const ta_body_expr_t *in,
                               ta_body_expr_t *out);

static ta_status_t ta_def_action(ta_def_copy_context_t *ctx, const ta_action_t *in,
                                 ta_action_t *out)
{
    ta_status_t s;
    if (in == NULL || out == NULL) return TA_INVALID;
    if (in->kind > TA_ACTION_CUSTOM) return TA_UNSUPPORTED;
    *out = *in;
    switch (in->kind) {
    case TA_ACTION_RAISE: return ta_def_text(ctx->arena, in->as.raise.event, &out->as.raise.event);
    case TA_ACTION_ASSIGN: case TA_ACTION_SCRIPT: case TA_ACTION_CUSTOM: return TA_OK;
    case TA_ACTION_IF:
        if (in->as.conditional.count != 0u && in->as.conditional.branches == NULL) return TA_INVALID;
        s = ta_def_array(&(ta_def_array_request_t){ctx->arena,in->as.conditional.count,sizeof(ta_branch_t),in->as.conditional.branches,(void **)&out->as.conditional.branches}, NULL);
        return s;
    case TA_ACTION_FOREACH:
        s = ta_def_text(ctx->arena, in->as.foreach_loop.item, &out->as.foreach_loop.item); if (s != TA_OK) return s;
        return ta_def_text(ctx->arena, in->as.foreach_loop.index, &out->as.foreach_loop.index);
    case TA_ACTION_LOG: return ta_def_text(ctx->arena, in->as.log.label, &out->as.log.label);
    case TA_ACTION_SEND:
        s = ta_def_text(ctx->arena, in->as.send.event.literal, &out->as.send.event.literal); if (s != TA_OK) return s;
        s = ta_def_text(ctx->arena, in->as.send.type.literal, &out->as.send.type.literal); if (s != TA_OK) return s;
        s = ta_def_text(ctx->arena, in->as.send.target.text, &out->as.send.target.text); if (s != TA_OK) return s;
        s = ta_def_text(ctx->arena, in->as.send.id, &out->as.send.id); if (s != TA_OK) return s;
        return ta_def_body(ctx, &in->as.send.body, &out->as.send.body);
    case TA_ACTION_CANCEL: return ta_def_text(ctx->arena, in->as.cancel.sendid.literal, &out->as.cancel.sendid.literal);
    }
    return TA_UNSUPPORTED;
}

static ta_status_t ta_def_block(ta_def_copy_context_t *ctx, const ta_block_t *in,
                                ta_block_t *out)
{
    ta_status_t s; ta_def_frame_t *f;
    if (ctx == NULL || in == NULL || out == NULL) return TA_INVALID;
    s = ta_def_frame_block(ctx, in, out); if (s != TA_OK || in->count == 0u) return s;
    while (ctx->top != NULL) {
        f = ctx->top;
        if (f->child_in != NULL) {
            const ta_block_t *child_in = f->child_in;
            ta_block_t *child_out = f->child_out;
            f->child_in = NULL;
            s = ta_def_frame_block(ctx, child_in, child_out); if (s != TA_OK) return s;
            continue;
        }
        if (f->index >= f->in->count) {
            ta_def_frame_t *parent = f->next;
            if (parent != NULL && parent->branch_count != 0u && parent->branch + 1u < parent->branch_count) {
                const ta_action_t *src = &parent->in->actions[parent->index - 1u];
                ta_action_t *dst = (ta_action_t *)&parent->out->actions[parent->index - 1u];
                ++parent->branch;
                parent->child_in = &src->as.conditional.branches[parent->branch].body;
                parent->child_out = &((ta_branch_t *)dst->as.conditional.branches)[parent->branch].body;
            } else if (parent != NULL) {
                parent->branch_count = 0u;
            }
            ctx->top = parent; f->next = ctx->free_frames; ctx->free_frames = f; continue;
        }
        {
            uint32_t n = f->index++;
            const ta_action_t *src = &f->in->actions[n];
            ta_action_t *dst = (ta_action_t *)&f->out->actions[n];
            s = ta_def_action(ctx, src, dst); if (s != TA_OK) return s;
            if (src->kind == TA_ACTION_IF && src->as.conditional.count != 0u) {
                /* The parent remains on the stack while each branch is visited. */
                f->branch = 0u;
                f->branch_count = src->as.conditional.count;
                f->child_in = &src->as.conditional.branches[0].body;
                f->child_out = &((ta_branch_t *)dst->as.conditional.branches)[0].body;
            } else if (src->kind == TA_ACTION_FOREACH) {
                f->child_in = &src->as.foreach_loop.body;
                f->child_out = &dst->as.foreach_loop.body;
            }
        }
    }
    return TA_OK;
}

static ta_status_t ta_def_blocks(ta_def_copy_context_t *ctx, const ta_blocks_t *in,
                                 ta_blocks_t *out)
{
    ta_status_t s; uint32_t i;
    if (in == NULL || out == NULL) return TA_INVALID;
    *out = *in;
    if (in->count == 0u) { out->blocks = NULL; return TA_OK; }
    s = ta_def_array(&(ta_def_array_request_t){ctx->arena,in->count,sizeof(ta_block_t),in->blocks,(void **)&out->blocks}, NULL);
    if (s != TA_OK) return s;
    for (i = 0; i < in->count; ++i) {
        s = ta_def_block(ctx, &in->blocks[i], (ta_block_t *)&out->blocks[i]);
        if (s != TA_OK) return s;
    }
    return TA_OK;
}

static ta_status_t ta_def_body(ta_def_copy_context_t *ctx, const ta_body_expr_t *in,
                               ta_body_expr_t *out)
{
    ta_status_t s; uint32_t i;
    if (in == NULL || out == NULL) return TA_INVALID;
    *out = *in;
    if (in->parameter_count != 0u && in->parameters == NULL) return TA_INVALID;
    s = ta_def_array(&(ta_def_array_request_t){ctx->arena,in->parameter_count,sizeof(ta_parameter_expr_t),in->parameters,(void **)&out->parameters}, NULL);
    if (s != TA_OK) return s;
    for (i = 0; i < in->parameter_count; ++i) {
        ta_parameter_expr_t *p = (ta_parameter_expr_t *)&out->parameters[i];
        s = ta_def_text(ctx->arena, in->parameters[i].name, &p->name);
        if (s != TA_OK) return s;
    }
    return TA_OK;
}

static ta_status_t ta_def_targets(ta_arena_t *arena, const ta_targets_t *in,
                                  ta_targets_t *out)
{
    if (in == NULL || out == NULL) return TA_INVALID;
    *out = *in;
    { ta_status_t s = ta_def_array(&(ta_def_array_request_t){arena,in->count,sizeof(ta_state_id_t),in->states,(void **)&out->states}, NULL); return s; }
}

static ta_status_t ta_def_invoke(ta_def_copy_context_t *ctx, const ta_invoke_desc_t *in,
                                 ta_invoke_desc_t *out)
{
    ta_status_t s;
    if (in == NULL || out == NULL) return TA_INVALID;
    *out = *in;
    s = ta_def_text(ctx->arena, in->type.literal, &out->type.literal); if (s != TA_OK) return s;
    s = ta_def_text(ctx->arena, in->source.literal, &out->source.literal); if (s != TA_OK) return s;
    s = ta_def_text(ctx->arena, in->id, &out->id); if (s != TA_OK) return s;
    s = ta_def_body(ctx, (const ta_body_expr_t *)&in->body, (ta_body_expr_t *)&out->body);
    if (s != TA_OK) return s;
    if (in->finalize_kind == TA_FINALIZE_BLOCK)
        return ta_def_block(ctx, &in->finalize, &out->finalize);
    return TA_OK;
}

static ta_status_t ta_def_copy_desc(ta_def_copy_context_t *ctx, const ta_definition_desc_t *in,
                                    ta_definition_desc_t *out)
{
    ta_status_t s; uint32_t i;
    *out = *in;
    s = ta_def_text(ctx->arena, in->name, &out->name); if (s != TA_OK) return s;
    s = ta_def_text(ctx->arena, in->data_model, &out->data_model); if (s != TA_OK) return s;
    s = ta_def_text(ctx->arena, in->data_layout.schema, &out->data_layout.schema); if (s != TA_OK) return s;
    s = ta_def_bytes(ctx->arena, in->data_layout.initial_bytes, &out->data_layout.initial_bytes); if (s != TA_OK) return s;
    if (in->state_count == 0u || in->states == NULL) return TA_INVALID;
    s = ta_def_array(&(ta_def_array_request_t){ctx->arena,in->state_count,sizeof(ta_state_desc_t),in->states,(void **)&out->states}, NULL);
    if (s != TA_OK) return s;
    s = ta_def_array(&(ta_def_array_request_t){ctx->arena,in->transition_count,sizeof(ta_transition_desc_t),in->transitions,(void **)&out->transitions}, NULL);
    if (s != TA_OK) return s;
    for (i = 0; i < in->state_count; ++i) {
        ta_state_desc_t *d = (ta_state_desc_t *)&out->states[i];
        const ta_state_desc_t *q = &in->states[i];
        s = ta_def_text(ctx->arena, q->name, &d->name); if (s != TA_OK) return s;
        s = ta_def_blocks(ctx, &q->onentry, &d->onentry); if (s != TA_OK) return s;
        s = ta_def_blocks(ctx, &q->onexit, &d->onexit); if (s != TA_OK) return s;
        s = ta_def_block(ctx, &q->initial.content, &d->initial.content); if (s != TA_OK) return s;
        s = ta_def_targets(ctx->arena, &q->initial.targets, &d->initial.targets); if (s != TA_OK) return s;
        s = ta_def_body(ctx, &q->done_data, &d->done_data); if (s != TA_OK) return s;
        s = ta_def_array(&(ta_def_array_request_t){ctx->arena,q->data_count,sizeof(ta_data_desc_t),q->data,(void **)&d->data}, NULL); if (s != TA_OK) return s;
        s = ta_def_array(&(ta_def_array_request_t){ctx->arena,q->invoke_count,sizeof(ta_invoke_desc_t),q->invokes,(void **)&d->invokes}, NULL); if (s != TA_OK) return s;
        if (q->data_count) { uint32_t k; for (k=0;k<q->data_count;++k) { ta_data_desc_t *x=(ta_data_desc_t *)&d->data[k]; s=ta_def_text(ctx->arena,q->data[k].id,&x->id); if(s!=TA_OK)return s; } }
        if (q->invoke_count) { uint32_t k; for (k=0;k<q->invoke_count;++k) { s=ta_def_invoke(ctx,&q->invokes[k],(ta_invoke_desc_t *)&d->invokes[k]); if(s!=TA_OK)return s; } }
    }
    for (i = 0; i < in->transition_count; ++i) {
        ta_transition_desc_t *d = (ta_transition_desc_t *)&out->transitions[i];
        const ta_transition_desc_t *q = &in->transitions[i];
        s = ta_def_array(&(ta_def_array_request_t){ctx->arena,q->event_count,sizeof(ta_text_t),q->events,(void **)&d->events}, NULL); if (s != TA_OK) return s;
        if (q->event_count != 0u) { uint32_t j; for (j = 0; j < q->event_count; ++j) { s = ta_def_text(ctx->arena, q->events[j], (ta_text_t *)&d->events[j]); if (s != TA_OK) return s; } }
        s = ta_def_block(ctx, &q->content, &d->content); if (s != TA_OK) return s;
        s = ta_def_targets(ctx->arena, &q->targets, &d->targets); if (s != TA_OK) return s;
    }
    return TA_OK;
}

static void ta_def_storage_release(void *arg)
{
    ta_definition_t *d = (ta_definition_t *)arg;
    if (d->storage.storage_release) d->storage.storage_release(d->storage.storage_user);
}

ta_status_t ta_definition_prepare(ta_system_t *system, const ta_definition_input_t *input,
                                  ta_definition_result_t *out_result)
{
    ta_definition_t *d; ta_arena_t *a; ta_status_t s;
    if (out_result == NULL) return TA_INVALID;
    ta_def_error(out_result, TA_INVALID);
    if (system == NULL || input == NULL || input->description == NULL || system->arena == NULL) return TA_INVALID;
    if (system->shutdown) { ta_def_error(out_result, TA_CANCELLED); return TA_CANCELLED; }
    if (system->running) { ta_def_error(out_result, TA_BUSY); return TA_BUSY; }
    if (input->options.storage > TA_DEFINITION_BORROW) return TA_INVALID;
    if (system->config.limits.definitions != TA_UNLIMITED && system->stats.definitions >= system->config.limits.definitions) { ta_def_error(out_result, TA_LIMIT); return TA_LIMIT; }
    if (system->next_revision == TA_REVISION_ANY) { ta_def_error(out_result, TA_LIMIT); return TA_LIMIT; }
    s = tai_scope(system, TA_LIFETIME_DEFINITION, &a); if (s != TA_OK) { ta_def_error(out_result,s); return s; }
    s = tai_alloc(a, sizeof(*d), (void **)&d); if (s != TA_OK) { ta_arena_close(a); ta_def_error(out_result,s); return s; }
    *d=(ta_definition_t){0}; d->system=system; d->arena=a; d->references=1u; d->storage=input->options;
    d->description=*input->description;
    s=tai_definition_validate(d,&out_result->error);
    if (s != TA_OK) { ta_arena_close(a); out_result->definition=NULL; out_result->error.status=s; if (out_result->error.detail.data==NULL) out_result->error.detail=(ta_text_t){ta_def_bad_input,sizeof(ta_def_bad_input)-1u}; return s; }
    if (input->options.storage == TA_DEFINITION_COPY) {
        ta_arena_t *scratch = NULL;
        ta_def_copy_context_t ctx = {a, NULL, NULL, NULL};
        s = ta_arena_open(a, &system->config.arenas[TA_LIFETIME_DEFINITION], &scratch);
        if (s == TA_OK) { ctx.scratch = scratch; s = ta_def_copy_desc(&ctx, input->description, &d->description); }
        if (scratch != NULL) ta_arena_close(scratch);
        if (s != TA_OK) { ta_arena_close(a); ta_def_error(out_result,s); return s; }
    }
    /* Inline revisions are pins owned by this accepted graph. */
    { uint32_t i, j; for (i=0; i<d->description.state_count; ++i) for (j=0; j<d->description.states[i].invoke_count; ++j) {
        ta_definition_t *inline_def = d->description.states[i].invokes[j].inline_definition;
        if (inline_def != NULL) { ta_definition_retain(inline_def); ++inline_def->held_by_definitions; }
    } }
    if (d->storage.storage == TA_DEFINITION_BORROW && d->storage.storage_release) { s=ta_arena_defer(a,ta_def_storage_release,d); if(s!=TA_OK){ uint32_t i,j; for(i=0;i<d->description.state_count;++i) for(j=0;j<d->description.states[i].invoke_count;++j) { ta_definition_t *x=d->description.states[i].invokes[j].inline_definition; if(x){if(x->held_by_definitions)--x->held_by_definitions; ta_definition_release(x);} } ta_arena_close(a); ta_def_error(out_result,s); return s;} }
    if (d->description.code_owner.handle && system->config.values.retain) system->config.values.retain(system->config.host_user,d->description.code_owner);
    d->revision=system->next_revision; if(d->revision==0u) d->revision=1u; if (system->next_revision == UINT64_MAX-1u) system->next_revision=TA_REVISION_ANY; else ++system->next_revision;
    d->next=system->definitions; system->definitions=d; ++system->stats.definitions;
    out_result->definition=d; out_result->error=(ta_diagnostic_t){0}; tai_mutated(system); return TA_OK;
}

void ta_definition_retain(ta_definition_t *definition) { if (definition && definition->references != (size_t)-1) { ++definition->references; tai_mutated(definition->system); } }

static void ta_def_unlink(ta_definition_t *definition)
{
    ta_definition_t **link = &definition->system->definitions;
    while (*link != NULL && *link != definition) link = &(*link)->next;
    if (*link == definition) {
        *link = definition->next;
        --definition->system->stats.definitions;
    }
    definition->next = NULL;
}

void ta_definition_release(ta_definition_t *definition)
{
    ta_definition_t *work;
    if (definition == NULL || definition->references == 0u) return;
    tai_mutated(definition->system);
    if (--definition->references != 0u) return;
    ta_def_unlink(definition);
    work = definition;
    while (work != NULL) {
        ta_definition_t *current = work;
        work = current->next;
        if (current->description.code_owner.handle && current->system->config.values.release) current->system->config.values.release(current->system->config.host_user,current->description.code_owner);
        { uint32_t i, j; for (i = 0; i < current->description.state_count; ++i) for (j = 0; j < current->description.states[i].invoke_count; ++j) {
            ta_definition_t *inline_def = current->description.states[i].invokes[j].inline_definition;
            if (inline_def != NULL) {
                if (inline_def->held_by_definitions) --inline_def->held_by_definitions;
                if (inline_def->references != 0u && --inline_def->references == 0u) {
                    ta_def_unlink(inline_def);
                    inline_def->next = work;
                    work = inline_def;
                }
            }
        } }
        ta_arena_close(current->arena);
    }
}

uint64_t ta_definition_revision(const ta_definition_t *definition) { return definition ? definition->revision : 0u; }
const ta_definition_desc_t *ta_definition_description(const ta_definition_t *definition) { return definition ? &definition->description : NULL; }
