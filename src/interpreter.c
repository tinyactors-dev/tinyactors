#include "internal.h"

typedef struct selection { uint32_t transition, domain; } selection_t;
typedef struct step_work {
    tai_actor_t *actor;
    ta_arena_t *arena;
    selection_t *selected;
    size_t count;
    uint8_t *exits, *entries, *defaults, *history_defaults, *effective;
} step_work_t;

static uint8_t descendant(const ta_definition_t *d, uint32_t state, uint32_t parent)
{ return (uint8_t)(state > parent && state < d->subtree_end[parent]); }
static uint8_t history_kind(ta_state_kind_t kind)
{ return (uint8_t)(kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP); }
static uint8_t atomic_kind(ta_state_kind_t kind)
{ return (uint8_t)(kind == TA_STATE_ATOMIC || kind == TA_STATE_FINAL); }

static tai_history_t *history_value(tai_actor_t *actor, uint32_t state)
{
    tai_history_t *value = actor->extra == NULL ? NULL : actor->extra->history;
    while (value != NULL && value->state != state) value = value->next;
    return value;
}

static ta_status_t workspace(tai_actor_t *actor, step_work_t *work)
{
    size_t count = actor->definition->description.state_count;
    tai_array_layout_t layout = {count, sizeof(selection_t), TAI_ALIGNMENT};
    ta_status_t status;
    *work = (step_work_t){0};
    work->actor = actor;
    status = tai_actor_scope(actor, TA_LIFETIME_MACROSTEP, &work->arena);
    if (status == TA_OK) status = tai_array(work->arena, &layout, (void **)&work->selected);
    layout.stride = 5;
    if (status == TA_OK) status = tai_array(work->arena, &layout, (void **)&work->exits);
    if (status != TA_OK) { if (work->arena != NULL) ta_arena_close(work->arena); work->arena = NULL; return status; }
    work->entries = work->exits + count;
    work->defaults = work->entries + count;
    work->history_defaults = work->defaults + count;
    work->effective = work->history_defaults + count;
    tai_zero(work->exits, count * 5);
    return TA_OK;
}

static void execute_block(ta_context_t *context, const ta_block_t *block)
{
    ta_status_t status;
    if (block->count == 0 || context->actor->status >= TA_ACTOR_DONE) return;
    status = tai_execute(context, block);
    if (status != TA_OK) tai_execution_error(context, status, (ta_text_t){0});
}

static void execute_blocks(ta_context_t *context, const ta_blocks_t *blocks)
{
    uint32_t i;
    for (i = 0; i < blocks->count && context->actor->status < TA_ACTOR_DONE; ++i)
        execute_block(context, &blocks->blocks[i]);
}

static void effective_targets(step_work_t *work, ta_targets_t targets)
{
    const ta_definition_desc_t *d = &work->actor->definition->description;
    uint32_t i, j;
    uint8_t changed = 1;
    tai_zero(work->effective, d->state_count);
    for (i = 0; i < targets.count; ++i) work->effective[targets.states[i]] = 1;
    while (changed) {
        changed = 0;
        for (i = 1; i < d->state_count; ++i) {
            if (work->effective[i] == 1 && history_kind(d->states[i].kind)) {
                tai_history_t *value = history_value(work->actor, i);
                work->effective[i] = 2;
                if (value != NULL) {
                    for (j = 0; j < value->count; ++j) work->effective[value->targets[j]] = 1;
                } else {
                    work->history_defaults[i] = 1;
                    for (j = 0; j < d->states[i].initial.targets.count; ++j) {
                        uint32_t target = d->states[i].initial.targets.states[j];
                        if (work->effective[target] != 2) work->effective[target] = 1;
                    }
                }
                changed = 1;
            }
        }
    }
}

static uint32_t domain(step_work_t *work, const ta_transition_desc_t *transition)
{
    ta_definition_t *d = work->actor->definition;
    uint32_t candidate, i;
    if (transition->targets.count == 0) return TA_INDEX_NONE;
    effective_targets(work, transition->targets);
    candidate = transition->source;
    if (transition->kind != TA_TRANSITION_INTERNAL ||
        d->description.states[candidate].kind != TA_STATE_COMPOUND)
        candidate = d->description.states[candidate].parent;
    while (candidate != TA_INDEX_NONE) {
        ta_state_kind_t kind = d->description.states[candidate].kind;
        if (kind == TA_STATE_ROOT || kind == TA_STATE_COMPOUND) {
            for (i = 1; i < d->description.state_count; ++i)
                if (work->effective[i] == 1 && !descendant(d, i, candidate)) break;
            if (i == d->description.state_count) return candidate;
        }
        candidate = d->description.states[candidate].parent;
    }
    return 0;
}

static uint8_t enabled(ta_context_t *context, const ta_transition_desc_t *transition,
                        const ta_event_t *event)
{
    uint32_t i;
    uint8_t yes = 1;
    ta_status_t status;
    if (event == NULL) { if (transition->event_count != 0) return 0; }
    else {
        for (i = 0; i < transition->event_count; ++i)
            if (tai_event_matches(transition->events[i], event->name)) break;
        if (i == transition->event_count) return 0;
    }
    if (transition->condition == 0) return 1;
    status = context->actor->system->config.data.test(context, transition->condition, &yes);
    if (status != TA_OK) { tai_execution_error(context, status, (ta_text_t){0}); return 0; }
    return yes;
}

static uint8_t conflicts(const ta_definition_t *d, uint32_t a, uint32_t b)
{
    return a != TA_INDEX_NONE && b != TA_INDEX_NONE &&
        (a == b || descendant(d, a, b) || descendant(d, b, a));
}

static void select_transition(step_work_t *work, uint32_t transition)
{
    ta_definition_t *d = work->actor->definition;
    selection_t candidate = {transition, domain(work, &d->description.transitions[transition])};
    uint32_t source = d->description.transitions[transition].source;
    size_t i, count = 0;
    for (i = 0; i < work->count; ++i) {
        selection_t old = work->selected[i];
        if (old.transition == transition) return;
        if (conflicts(d, candidate.domain, old.domain) &&
            !descendant(d, source, d->description.transitions[old.transition].source)) return;
    }
    for (i = 0; i < work->count; ++i)
        if (!conflicts(d, candidate.domain, work->selected[i].domain))
            work->selected[count++] = work->selected[i];
    work->selected[count++] = candidate;
    work->count = count;
}

static void select_enabled(step_work_t *work, const ta_event_t *event)
{
    tai_actor_t *actor = work->actor;
    const ta_definition_desc_t *d = &actor->definition->description;
    uint32_t leaf;
    ta_context_t context;
    tai_context(actor, TA_PHASE_GUARD, &context);
    for (leaf = 1; leaf < d->state_count && actor->status < TA_ACTOR_DONE; ++leaf) {
        uint32_t state;
        if (!atomic_kind(d->states[leaf].kind) || !tai_active(actor, leaf)) continue;
        state = leaf;
        while (state != TA_INDEX_NONE) {
            uint32_t n;
            const ta_state_desc_t *owner = &d->states[state];
            context.view.state_id = state;
            for (n = 0; n < owner->transition_count; ++n) {
                uint32_t index = owner->first_transition + n;
                context.view.transition_id = index;
                if (enabled(&context, &d->transitions[index], event)) {
                    select_transition(work, index);
                    break;
                }
            }
            if (n != owner->transition_count) break;
            state = owner->parent;
        }
    }
}

static ta_status_t save_history(step_work_t *work, uint32_t state)
{
    tai_actor_t *actor = work->actor;
    ta_definition_t *d = actor->definition;
    uint32_t h, i;
    for (h = state + 1; h < d->subtree_end[state]; ++h) {
        tai_history_t *value, **link;
        ta_arena_t *arena;
        ta_status_t status;
        size_t count = 0;
        tai_array_layout_t layout;
        if (d->description.states[h].parent != state || !history_kind(d->description.states[h].kind)) continue;
        status = tai_extra(actor);
        if (status != TA_OK) return status;
        for (i = state + 1; i < d->subtree_end[state]; ++i)
            if (tai_active(actor, i) && (d->description.states[h].kind == TA_STATE_HISTORY_DEEP ?
                 atomic_kind(d->description.states[i].kind) : d->description.states[i].parent == state)) ++count;
        status = tai_actor_scope(actor, TA_LIFETIME_ACTOR, &arena);
        if (status != TA_OK) return status;
        status = tai_alloc(arena, sizeof(*value), (void **)&value);
        if (status != TA_OK) { ta_arena_close(arena); return status; }
        *value = (tai_history_t){0};
        value->arena = arena;
        value->state = h;
        value->count = count;
        layout = (tai_array_layout_t){count,sizeof(uint32_t),TAI_ALIGNMENT};
        status = tai_array(arena, &layout, (void **)&value->targets);
        if (status != TA_OK) { ta_arena_close(arena); return status; }
        count = 0;
        for (i = state + 1; i < d->subtree_end[state]; ++i)
            if (tai_active(actor, i) && (d->description.states[h].kind == TA_STATE_HISTORY_DEEP ?
                 atomic_kind(d->description.states[i].kind) : d->description.states[i].parent == state))
                value->targets[count++] = i;
        link = &actor->extra->history;
        while (*link != NULL && (*link)->state != h) link = &(*link)->next;
        if (*link != NULL) {
            tai_history_t *old = *link;
            value->next = old->next;
            *link = value;
            ta_arena_close(old->arena);
        } else *link = value;
    }
    return TA_OK;
}

static void exit_state(tai_actor_t *actor, uint32_t state)
{
    ta_context_t context;
    tai_context(actor, TA_PHASE_ONEXIT, &context);
    context.view.state_id = state;
    execute_blocks(&context, &actor->definition->description.states[state].onexit);
    tai_invoke_cancel_state(actor, state);
    tai_activation_close(actor, state);
    (void)tai_set_active(actor, state, 0);
}

static void add_targets(step_work_t *work, ta_targets_t targets, uint32_t ancestor)
{
    uint32_t i;
    ta_definition_t *d = work->actor->definition;
    effective_targets(work, targets);
    for (i = 1; i < d->description.state_count; ++i) if (work->effective[i] == 1) {
        uint32_t state = i;
        while (state != ancestor && state != TA_INDEX_NONE) {
            if (!tai_active(work->actor, state)) work->entries[state] = 1;
            state = d->description.states[state].parent;
        }
    }
}

static void complete_entries(step_work_t *work)
{
    const ta_definition_desc_t *d = &work->actor->definition->description;
    ta_definition_t *definition = work->actor->definition;
    uint8_t changed = 1;
    uint32_t i, child;
    while (changed) {
        changed = 0;
        for (i = 0; i < d->state_count; ++i) if (work->entries[i]) {
            const ta_state_desc_t *state = &d->states[i];
            if (state->kind == TA_STATE_ROOT || state->kind == TA_STATE_COMPOUND) {
                for (child = i + 1; child < definition->subtree_end[i]; ++child)
                    if (d->states[child].parent == i &&
                        (work->entries[child] || tai_active(work->actor, child))) break;
                if (child != definition->subtree_end[i] || work->defaults[i]) continue;
                work->defaults[i] = 1;
                if (state->initial.targets.count != 0) add_targets(work, state->initial.targets, i);
                else {
                    for (child = i + 1; child < definition->subtree_end[i]; ++child)
                        if (d->states[child].parent == i && !history_kind(d->states[child].kind)) break;
                    work->entries[child] = 1;
                }
                changed = 1;
            } else if (state->kind == TA_STATE_PARALLEL) {
                for (child = i + 1; child < definition->subtree_end[i]; ++child)
                    if (d->states[child].parent == i && !history_kind(d->states[child].kind) &&
                        !work->entries[child] && !tai_active(work->actor, child)) {
                        work->entries[child] = 1;
                        changed = 1;
                    }
            }
        }
    }
}

static void initialize_data(tai_actor_t *actor, uint32_t state)
{
    const ta_state_desc_t *owner = &actor->definition->description.states[state];
    ta_context_t context;
    uint32_t i;
    tai_context(actor, TA_PHASE_INITIALIZE, &context);
    context.view.state_id = state;
    for (i = 0; i < owner->data_count; ++i) {
        ta_data_binding_t binding = {state, &owner->data[i], {0}};
        ta_status_t status;
        if (state == 0 && actor->initial != NULL) {
            size_t n;
            for (n = 0; n < actor->initial->parameters.parameter_count; ++n)
                if (tai_text_compare(owner->data[i].id, actor->initial->parameters.parameters[n].name) == 0)
                    binding.override_value = actor->initial->parameters.parameters[n].value;
        }
        status = actor->system->config.data.initialize(&context, &binding);
        if (status != TA_OK) tai_execution_error(&context, status, (ta_text_t){0});
        if (actor->status >= TA_ACTOR_DONE) return;
    }
}

static ta_status_t late_initialize(tai_actor_t *actor, uint32_t state)
{
    ta_status_t status;
    uint8_t *flags;
    size_t bytes;
    if (actor->definition->description.binding != TA_BINDING_LATE ||
        state == 0) return TA_OK;
    status = tai_extra(actor);
    if (status != TA_OK) return status;
    if (actor->extra->late == NULL) {
        bytes = ((size_t)actor->definition->description.state_count + 7) / 8;
        status = tai_alloc(actor->extra->arena, bytes, (void **)&actor->extra->late);
        if (status != TA_OK) return status;
        tai_zero(actor->extra->late, bytes);
    }
    flags = actor->extra->late;
    if (!(flags[state / 8] & (1u << (state % 8)))) {
        flags[state / 8] |= (uint8_t)(1u << (state % 8));
        initialize_data(actor, state);
    }
    return TA_OK;
}

static uint8_t final_state(tai_actor_t *actor, uint32_t state)
{
    const ta_definition_desc_t *d = &actor->definition->description;
    uint32_t i=state, j, end=actor->definition->subtree_end[state];
    while(i<end) {
        ta_state_kind_t kind=d->states[i].kind;
        if(history_kind(kind)){++i;continue;}
        if(!tai_active(actor,i))return 0;
        if(kind==TA_STATE_PARALLEL){++i;continue;}
        if(kind==TA_STATE_COMPOUND || kind==TA_STATE_ROOT) {
            for(j=i+1;j<actor->definition->subtree_end[i];++j)
                if(d->states[j].parent==i && d->states[j].kind==TA_STATE_FINAL && tai_active(actor,j))break;
            if(j==actor->definition->subtree_end[i])return 0;
        } else if(kind!=TA_STATE_FINAL)return 0;
        i=actor->definition->subtree_end[i];
    }
    return 1;
}

static void done_event(ta_context_t *context, uint32_t state, const ta_payload_t *data)
{
    ta_text_t name = context->actor->definition->description.states[state].name;
    ta_arena_t *arena;
    char *buffer;
    tai_delivery_t event = {0};
    ta_status_t status = ta_context_arena(context, TA_LIFETIME_MACROSTEP, &arena);
    if (status == TA_OK && name.size > (size_t)-1 - 11) status = TA_LIMIT;
    if (status == TA_OK) status = tai_alloc(arena, name.size + 11, (void **)&buffer);
    if (status == TA_OK) {
        tai_copy(buffer, "done.state.", 11);
        tai_copy(buffer + 11, name.data, name.size);
        event.internal = 1;
        event.event.name = (ta_text_t){buffer, name.size + 11};
        event.event.type = TA_EVENT_INTERNAL;
        if (data != NULL) event.event.data = *data;
        status = tai_enqueue(context->actor, &event, NULL);
    }
    if (status != TA_OK) tai_execution_error(context, status, (ta_text_t){0});
}

static void entered_final(tai_actor_t *actor, uint32_t state)
{
    const ta_definition_desc_t *d = &actor->definition->description;
    uint32_t parent = d->states[state].parent, ancestor;
    ta_context_t context;
    ta_body_t body = {0};
    ta_payload_t payload = {0};
    ta_status_t status;
    tai_context(actor, TA_PHASE_DONE_DATA, &context);
    context.view.state_id = state;
    status = parent == 0 ? TA_OK : tai_body_eval(&context, &d->states[state].done_data, &body);
    if (status == TA_OK && parent != 0) status = tai_body_payload(&context, &body, &payload);
    if (status != TA_OK) { tai_execution_error(&context, status, (ta_text_t){0}); body = (ta_body_t){0}; }
    if (parent == 0) {
        for (ancestor = d->state_count; ancestor-- > 0;)
            if (tai_active(actor, ancestor)) exit_state(actor, ancestor);
        if (actor->status < TA_ACTOR_DONE) {
            tai_context(actor, TA_PHASE_DONE_DATA, &context);
            context.view.state_id = state;
            status = tai_body_eval(&context, &d->states[state].done_data, &body);
            if (status != TA_OK) { tai_execution_error(&context,status,(ta_text_t){0}); body=(ta_body_t){0}; }
        }
        if (actor->status < TA_ACTOR_DONE) {
            tai_invoke_child_terminal(actor, &body);
            tai_actor_terminal(actor, TA_ACTOR_DONE, TA_OK);
        }
    } else {
        done_event(&context, parent, &payload);
        ancestor = d->states[parent].parent;
        while (ancestor != TA_INDEX_NONE && d->states[ancestor].kind == TA_STATE_PARALLEL && final_state(actor, ancestor)) {
            done_event(&context, ancestor, NULL);
            ancestor = d->states[ancestor].parent;
        }
    }
}

static ta_status_t enter_states(step_work_t *work)
{
    tai_actor_t *actor = work->actor;
    const ta_definition_desc_t *d = &actor->definition->description;
    uint32_t i, h;
    complete_entries(work);
    for (i = 0; i < d->state_count && actor->status < TA_ACTOR_DONE; ++i) if (work->entries[i]) {
        ta_context_t context;
        ta_status_t status = tai_set_active(actor, i, 1);
        if (status == TA_OK) status = late_initialize(actor, i);
        if (status != TA_OK) return status;
        tai_context(actor, TA_PHASE_ONENTRY, &context);
        context.view.state_id = i;
        execute_blocks(&context, &d->states[i].onentry);
        if (work->defaults[i]) execute_block(&context, &d->states[i].initial.content);
        for (h = i + 1; h < actor->definition->subtree_end[i]; ++h)
            if (d->states[h].parent == i && work->history_defaults[h])
                execute_block(&context, &d->states[h].initial.content);
        if (d->states[i].kind == TA_STATE_FINAL && actor->status < TA_ACTOR_DONE) entered_final(actor, i);
    }
    return TA_OK;
}

static ta_status_t microstep(step_work_t *work)
{
    tai_actor_t *actor = work->actor;
    const ta_definition_desc_t *d = &actor->definition->description;
    size_t n;
    uint32_t i;
    ta_status_t status;
    if (actor->microsteps >= actor->limits->microsteps_per_macrostep) return TA_FAULTED;
    ++actor->microsteps;
    tai_saturate(&actor->system->stats.microsteps);
    for (n = 0; n < work->count; ++n) if (work->selected[n].domain != TA_INDEX_NONE)
        for (i = work->selected[n].domain + 1; i < actor->definition->subtree_end[work->selected[n].domain]; ++i)
            if (tai_active(actor, i)) work->exits[i] = 1;
    for (i = 1; i < d->state_count; ++i) if (work->exits[i]) {
        status = save_history(work, i);
        if (status != TA_OK) return status;
    }
    for (i = d->state_count; i-- > 0 && actor->status < TA_ACTOR_DONE;)
        if (work->exits[i]) exit_state(actor, i);
    for (n = 0; n < work->count && actor->status < TA_ACTOR_DONE; ++n) {
        ta_context_t context;
        const ta_transition_desc_t *transition = &d->transitions[work->selected[n].transition];
        tai_context(actor, TA_PHASE_TRANSITION, &context);
        context.view.state_id = transition->source;
        context.view.transition_id = work->selected[n].transition;
        execute_block(&context, &transition->content);
    }
    tai_zero(work->history_defaults, d->state_count);
    for (n = 0; n < work->count; ++n)
        add_targets(work, d->transitions[work->selected[n].transition].targets, work->selected[n].domain);
    return enter_states(work);
}

static void end_macrostep(tai_actor_t *actor, ta_status_t status)
{
    if (actor->extra != NULL) {
        tai_span_t *span = actor->extra->span;
        actor->extra->span = NULL;
        actor->extra->trace = NULL;
        if (actor->extra->macrostep != NULL) {
            ta_arena_t *arena = actor->extra->macrostep;
            actor->extra->macrostep = NULL;
            ta_arena_close(arena);
        }
        tai_span_finish(span, status);
    }
    actor->microsteps = actor->actions = 0;
}

void tai_actor_terminal(tai_actor_t *actor, ta_actor_status_t status, ta_status_t reason)
{
    ta_actor_termination_t termination = {{actor->id},status,reason};
    if (actor->notified) return;
    tai_cancel_timers(actor);
    actor->status = status;
    actor->reason = reason;
    actor->stable = 1;
    actor->notified = 1;
    tai_unready(actor);
    end_macrostep(actor, reason);
    if (actor->initial != NULL) {
        ta_arena_close(actor->initial->arena);
        actor->initial = NULL;
    }
    tai_mutated(actor->system);
    if (actor->system->config.terminal != NULL)
        actor->system->config.terminal(actor->system->config.host_user, &termination);
}

void tai_actor_fault(tai_actor_t *actor, ta_status_t reason)
{
    uint32_t i;
    if (actor->status >= TA_ACTOR_DONE) return;
    for (i = actor->definition->description.state_count; i-- > 0;) {
        tai_invoke_cancel_state(actor, i);
        tai_activation_close(actor, i);
    }
    actor->leaf = TA_INDEX_NONE;
    if (actor->extra != NULL && actor->extra->parallel != NULL)
        tai_zero(actor->extra->parallel, ((size_t)actor->definition->description.state_count + 7) / 8);
    tai_saturate(&actor->system->stats.faulted_actors);
    tai_invoke_child_terminal(actor, NULL);
    tai_actor_terminal(actor, TA_ACTOR_FAILED, reason);
}

static ta_status_t locations(tai_actor_t *actor)
{
    ta_system_t *system = actor->system;
    size_t i, count = 1;
    ta_status_t status;
    ta_context_t context;
    ta_arena_t *scratch;
    for (i = 1; i < system->processor_count; ++i) if (system->config.ioprocessors[i].location != NULL) ++count;
    if (count == 1) return TA_OK;
    status = tai_extra(actor);
    if (status != TA_OK) return status;
    {
        tai_array_layout_t layout = {count,sizeof(ta_io_location_t),TAI_ALIGNMENT};
        status = tai_array(actor->arena, &layout, (void **)&actor->extra->locations);
    }
    if (status != TA_OK) return status;
    actor->extra->locations[0].type = ta_text_from_cstr(TA_SCXML_IOPROCESSOR);
    status = tai_text_copy(actor->arena, tai_session(actor, 1), &actor->extra->locations[0].location);
    if (status != TA_OK) return status;
    actor->extra->location_count = 1;
    tai_context(actor, TA_PHASE_INITIALIZE, &context);
    status = ta_context_arena(&context, TA_LIFETIME_MACROSTEP, &scratch);
    if (status != TA_OK) return status;
    for (i = 1; i < system->processor_count; ++i) {
        const ta_ioprocessor_desc_t *processor = &system->config.ioprocessors[i];
        ta_text_t location = {0};
        ta_location_request_t request = {{actor->id},tai_session(actor,0),scratch};
        ta_io_location_t *out;
        if (processor->location == NULL) continue;
        status = processor->location(processor->user, &request, &location);
        if (status != TA_OK) return status;
        if (location.data == NULL && location.size == 0) continue;
        if (!tai_text_valid(location, 0)) return TA_INVALID;
        out = &actor->extra->locations[actor->extra->location_count];
        out->type = ta_text_from_cstr(processor->type);
        status = tai_text_copy(actor->arena, location, &out->location);
        if (status != TA_OK) return status;
        ++actor->extra->location_count;
    }
    return TA_OK;
}

static ta_status_t initialize_actor(tai_actor_t *actor)
{
    ta_context_t context;
    ta_system_t *system = actor->system;
    const ta_definition_desc_t *d = &actor->definition->description;
    uint32_t i, j;
    ta_status_t status = locations(actor);
    if(actor->initial!=NULL && actor->initial->trace!=NULL) {
        ta_arena_t *arena;
        ta_status_t trace_status;
        tai_context(actor,TA_PHASE_INITIALIZE,&context);
        trace_status=ta_context_arena(&context,TA_LIFETIME_MACROSTEP,&arena);
        if(trace_status==TA_OK) {
            tai_value_scope_t scope={actor->system,arena};
            trace_status=tai_trace_copy(&scope,actor->initial->trace,&actor->extra->trace);
        }
        if(trace_status!=TA_OK)tai_saturate(&actor->system->stats.dropped_trace_records);
    }
    step_work_t work;
    if (status != TA_OK) return status;
    tai_context(actor, TA_PHASE_INITIALIZE, &context);
    if (d->data_layout.storage == TA_DATA_NATIVE) {
        ta_value_t seed = actor->initial == NULL ? (ta_value_t){0} : actor->initial->seed;
        status = system->config.data.create(&context, seed, &actor->data.as.native);
        if (status != TA_OK || actor->data.as.native.handle == 0) return status == TA_OK ? TA_EXECUTION_ERROR : status;
        tai_context(actor, TA_PHASE_INITIALIZE, &context);
    }
    if (system->config.data.bind_system != NULL) {
        status = system->config.data.bind_system(&context, &context.values);
        if (status != TA_OK) return status;
    }
    for (i = 0; i < d->state_count; ++i) for (j = 0; j < d->states[i].data_count; ++j) {
        ta_data_binding_t binding = {i,&d->states[i].data[j],{0}};
        context.view.state_id = i;
        status = system->config.data.declare(&context, &binding);
        if (status != TA_OK) return status;
    }
    for (i = 0; i < d->state_count && actor->status < TA_ACTOR_DONE; ++i)
        if (i == 0 || d->binding == TA_BINDING_EARLY) initialize_data(actor, i);
    if (actor->status >= TA_ACTOR_DONE) return TA_OK;
    tai_context(actor, TA_PHASE_ROOT_SCRIPT, &context);
    context.view.state_id = 0;
    if (d->root_script != 0) {
        status = system->config.data.run(&context, TA_ACTION_SCRIPT, d->root_script);
        if (status != TA_OK) tai_execution_error(&context, status, (ta_text_t){0});
    }
    if (actor->status >= TA_ACTOR_DONE) return TA_OK;
    status = workspace(actor, &work);
    if (status != TA_OK) return status;
    work.entries[0] = 1;
    status = enter_states(&work);
    ta_arena_close(work.arena);
    actor->initialized = 1;
    if (actor->initial != NULL) { ta_arena_close(actor->initial->arena); actor->initial = NULL; }
    return status;
}

static void begin_process(tai_actor_t *actor)
{
    ta_event_t scratch;
    const ta_event_t *event = tai_current_event(actor, &scratch);
    ta_span_start_t start = {0};
    tai_span_t *span = NULL;
    if (event == NULL) return;
    start.operation = TA_SPAN_PROCESS;
    start.kind = TA_SPAN_CONSUMER;
    start.actor = (ta_actor_ref_t){actor->id};
    start.revision = actor->definition->revision;
    start.definition_name = actor->definition->description.name;
    start.message_id = event->message_id;
    start.event_name = event->name;
    start.event_data = &event->data;
    start.now_ns = actor->system->now_ns;
    start.links = event->trace;
    start.link_count = event->trace == NULL ? 0 : 1;
    tai_span_begin(actor->system, &start, &span);
    if (span != NULL || event->trace != NULL) {
        if (tai_extra(actor) != TA_OK) { tai_span_finish(span, TA_CANCELLED); tai_saturate(&actor->system->stats.dropped_trace_records); return; }
        actor->extra->span = span;
        actor->extra->trace = span == NULL ? NULL : &span->value.context;
        if (span == NULL && event->trace != NULL) {
            ta_arena_t *arena;
            ta_context_t context;
            ta_status_t status;
            tai_context(actor, TA_PHASE_INITIALIZE, &context);
            status = ta_context_arena(&context,TA_LIFETIME_MACROSTEP,&arena);
            if(status==TA_OK) {
                tai_value_scope_t scope={actor->system,arena};
                status=tai_trace_copy(&scope,event->trace,&actor->extra->trace);
            }
            if(status!=TA_OK)tai_saturate(&actor->system->stats.dropped_trace_records);
        }
    }
}

static void bind_event(tai_actor_t *actor)
{
    ta_context_t context;
    ta_status_t status;
    if (actor->system->config.data.bind_event == NULL) return;
    tai_context(actor, TA_PHASE_INITIALIZE, &context);
    status = actor->system->config.data.bind_event(&context, context.values.event);
    if (status != TA_OK) tai_execution_error(&context, status, (ta_text_t){0});
}

ta_status_t tai_interpret(tai_actor_t *actor, ta_run_result_t *result)
{
    step_work_t work;
    ta_status_t status;
    const ta_event_t *event = NULL;
    ta_event_t scratch;
    uint8_t invoke_work = 0;
    if (actor->stop) {
        uint32_t i;
        for (i = actor->definition->description.state_count; i-- > 0 && actor->status < TA_ACTOR_DONE;)
            if (tai_active(actor, i)) exit_state(actor, i);
        if (actor->status < TA_ACTOR_DONE) {
            tai_invoke_child_terminal(actor, NULL);
            tai_actor_terminal(actor, TA_ACTOR_STOPPED, TA_CANCELLED);
        }
        ++result->terminal_actors;
        return TA_OK;
    }
    if (!actor->initialized) {
        status = initialize_actor(actor);
        if (status != TA_OK) tai_actor_fault(actor, status);
        if (actor->status >= TA_ACTOR_DONE) ++result->terminal_actors;
        return TA_OK;
    }
    if (actor->stable && !actor->eventless) {
        if (tai_dequeue(actor, 0) != TA_OK) { tai_unready(actor); return TA_OK; }
        actor->stable = 0;
        begin_process(actor);
        bind_event(actor);
        tai_invoke_event(actor);
        ++result->external_events;
        event = tai_current_event(actor, &scratch);
    }
    actor->eventless = 0;
    status = workspace(actor, &work);
    if (status != TA_OK) { tai_actor_fault(actor, status); ++result->terminal_actors; return TA_OK; }
    select_enabled(&work, event);
    if (work.count == 0 && event == NULL && actor->internal_head != NULL && actor->status < TA_ACTOR_DONE) {
        (void)tai_dequeue(actor, 1);
        bind_event(actor);
        event = tai_current_event(actor, &scratch);
        select_enabled(&work, event);
    }
    if (work.count != 0 && actor->status < TA_ACTOR_DONE) {
        status = microstep(&work);
        if (status != TA_OK) tai_actor_fault(actor, status == TA_FAULTED ? TA_LIMIT : status);
        else ++result->microsteps;
    }
    ta_arena_close(work.arena);
    if (actor->status >= TA_ACTOR_DONE) { ++result->terminal_actors; return TA_OK; }
    if (work.count == 0 && event == NULL && actor->internal_head == NULL) {
        status = tai_invoke_pending(actor, &invoke_work);
        if (status != TA_OK) {
            ta_context_t context;
            tai_context(actor, TA_PHASE_INVOKE, &context);
            tai_execution_error(&context, status, (ta_text_t){0});
        }
        if (!invoke_work && actor->internal_head == NULL) {
            actor->stable = 1;
            actor->status = TA_ACTOR_READY;
            end_macrostep(actor, TA_OK);
            tai_saturate(&actor->system->stats.completed_macrosteps);
            if (actor->external_head == NULL) tai_unready(actor);
        }
    }
    return TA_OK;
}

ta_status_t ta_system_run(ta_system_t *system, const ta_run_options_t *options, ta_run_result_t *out)
{
    uint8_t timer_turn = 1;
    if (system == NULL || options == NULL || out == NULL) return TA_INVALID;
    *out = (ta_run_result_t){0};
    if (system->running) return TA_BUSY;
    if (options->now_ns == TA_TIME_NEVER || (system->time_set && options->now_ns < system->now_ns) ||
        options->budget.actor_quantum == 0) return TA_INVALID;
    system->now_ns = options->now_ns;
    system->time_set = 1;
    system->running = 1;
    while (out->work_units < options->budget.work_units) {
        tai_actor_t *actor = system->ready_head;
        size_t quantum = 0;
        if (tai_timer_due(system) && (timer_turn || actor == NULL)) {
            tai_timer_dispatch(system);
            ++out->work_units;
            timer_turn = 0;
            continue;
        }
        if (actor == NULL) break;
        timer_turn = 1;
        do {
            (void)tai_interpret(actor, out);
            ++out->work_units;
            ++quantum;
        } while (actor->queued && actor->status < TA_ACTOR_DONE &&
                 quantum < options->budget.actor_quantum && out->work_units < options->budget.work_units);
        if (actor->status >= TA_ACTOR_DONE && actor->extra != NULL && actor->extra->parent_invocation != NULL) {
            tai_release_child(actor);
            continue;
        }
        if (actor->queued) {
            tai_unready(actor);
            tai_ready(actor);
            if (!actor->stable && actor->status < TA_ACTOR_DONE) actor->status = TA_ACTOR_SUSPENDED;
        }
    }
    system->running = 0;
    if (out->work_units != 0) tai_mutated(system);
    out->runnable = system->ready_head != NULL || tai_timer_due(system);
    out->io_ready = system->outbox_head != NULL;
    out->next_deadline_ns = ta_system_next_deadline(system);
    return TA_OK;
}
