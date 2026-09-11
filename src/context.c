#include "internal.h"

static uint8_t tai_descends(const ta_definition_t *d, uint32_t parent, uint32_t child)
{
    if (d == NULL || parent >= d->description.state_count || child >= d->description.state_count) return 0;
    return (uint8_t)(child >= parent && child < d->subtree_end[parent]);
}

static uint8_t tai_bit(const uint8_t *bits, uint32_t n)
{
    return (uint8_t)((bits[n >> 3] >> (n & 7u)) & 1u);
}

static void tai_bit_set(uint8_t *bits, uint32_t n, uint8_t value)
{
    uint8_t mask = (uint8_t)(1u << (n & 7u));
    if (value) bits[n >> 3] |= mask;
    else bits[n >> 3] &= (uint8_t)~mask;
}

void tai_context(tai_actor_t *actor, ta_phase_t phase, ta_context_t *out)
{
    ta_text_t session;
    if (out == NULL) return;
    *out = (ta_context_t){0};
    if (actor == NULL) return;
    out->actor = actor;
    out->view.actor = (ta_actor_ref_t){actor->id};
    out->view.definition = actor->definition;
    out->view.phase = phase;
    out->view.state_id = TA_INDEX_NONE;
    out->view.transition_id = TA_INDEX_NONE;
    out->view.now_ns = actor->system->now_ns;
    out->view.state.storage = actor->definition->description.data_layout.storage;
    if (out->view.state.storage == TA_DATA_BYTES)
        out->view.state.as.bytes = actor->data.as.bytes;
    else out->view.state.as.native = actor->data.as.native;
    session = tai_session(actor, 1);
    out->values.sessionid = (ta_text_t){session.data + 8, session.size - 8};
    out->values.name = actor->identity_definition->description.name;
    out->values.actor = (ta_actor_ref_t){actor->id};
    out->values.revision = actor->definition->revision;
    out->values.event = tai_current_event(actor, &out->error_event);
    out->scxml_location = (ta_io_location_t){ta_text_from_cstr(TA_SCXML_IOPROCESSOR),session};
    out->values.ioprocessors = &out->scxml_location;
    out->values.ioprocessor_count = 1;
    if (actor->extra != NULL) {
        out->view.trace = actor->extra->trace;
        if (actor->extra->locations != NULL && actor->extra->location_count != 0) {
            out->values.ioprocessors = actor->extra->locations;
            out->values.ioprocessor_count = actor->extra->location_count;
        }
    }
    out->view.system = &out->values;
    if (out->scxml_location.location.data == NULL)
        out->scxml_location = (ta_io_location_t){0};
    out->effects = (phase == TA_PHASE_ROOT_SCRIPT || phase == TA_PHASE_ONENTRY ||
                    phase == TA_PHASE_ONEXIT || phase == TA_PHASE_TRANSITION);
}

void ta_context_inspect(const ta_context_t *context, ta_context_view_t *out_view)
{
    if (context == NULL || out_view == NULL) return;
    *out_view = context->view;
}

uint8_t ta_context_in(const ta_context_t *context, ta_text_t state_name)
{
    uint32_t i;
    const ta_definition_t *d;
    if (context == NULL || context->actor == NULL) return 0;
    d = context->actor->definition;
    for (i = 0; i < d->description.state_count; ++i)
        if (tai_text_compare(d->description.states[i].name, state_name) == 0)
            return tai_active(context->actor, i);
    return 0;
}

void *ta_context_host_user(const ta_context_t *context)
{
    if (context == NULL || context->actor == NULL || context->actor->system == NULL) return NULL;
    return context->actor->system->config.host_user;
}

ta_status_t ta_context_bytes(ta_context_t *context, ta_mut_bytes_t *out_bytes)
{
    if (context == NULL || out_bytes == NULL) return TA_INVALID;
    *out_bytes = (ta_mut_bytes_t){NULL, 0};
    if (context->view.state.storage != TA_DATA_BYTES) return TA_UNSUPPORTED;
    if (context->view.phase == TA_PHASE_GUARD || context->view.phase == TA_PHASE_MIGRATE)
        return TA_INVALID;
    out_bytes->data = (void *)context->view.state.as.bytes.data;
    out_bytes->size = context->view.state.as.bytes.size;
    return TA_OK;
}

uint8_t tai_active(const tai_actor_t *actor, uint32_t state)
{
    if (actor == NULL || actor->definition == NULL || state >= actor->definition->description.state_count) return 0;
    if (actor->extra != NULL && actor->extra->parallel != NULL)
        return tai_bit(actor->extra->parallel, state);
    if (actor->leaf == TA_INDEX_NONE) return 0;
    return tai_descends(actor->definition, state, actor->leaf);
}

ta_status_t tai_set_active(tai_actor_t *actor, uint32_t state, uint8_t active)
{
    ta_definition_t *d;
    ta_status_t s;
    if (actor == NULL || actor->definition == NULL) return TA_INVALID;
    d = actor->definition;
    if (state >= d->description.state_count) return TA_INVALID;
    if (active && tai_active(actor, state)) return TA_OK;
    if (actor->extra != NULL && actor->extra->parallel != NULL) {
        tai_bit_set(actor->extra->parallel, state, active);
        return TA_OK;
    }
    if (!active) {
        if (actor->leaf != TA_INDEX_NONE && tai_descends(d, state, actor->leaf)) {
            actor->leaf = d->description.states[state].parent;
            if (actor->leaf == TA_INDEX_NONE) actor->leaf = TA_INDEX_NONE;
        }
        return TA_OK;
    }
    if (actor->leaf != TA_INDEX_NONE && !tai_descends(d, state, actor->leaf) &&
        !tai_descends(d, actor->leaf, state)) {
        size_t bytes = ((size_t)d->description.state_count + 7u) >> 3;
        void *memory;
        s = tai_extra(actor);
        if (s != TA_OK) return s;
        s = tai_alloc(actor->extra->arena, bytes, &memory);
        if (s != TA_OK) return s;
        actor->extra->parallel = (uint8_t *)memory;
        tai_zero(memory, bytes);
        { uint32_t p = actor->leaf; while (p != TA_INDEX_NONE) { tai_bit_set(actor->extra->parallel, p, 1); p = d->description.states[p].parent; } }
    }
    if (actor->extra != NULL && actor->extra->parallel != NULL) {
        tai_bit_set(actor->extra->parallel, state, 1);
        return TA_OK;
    }
    actor->leaf = state;
    return TA_OK;
}

static tai_activation_t *tai_find_activation(tai_actor_t *a, uint32_t state)
{
    tai_activation_t *n;
    if (a->extra == NULL) return NULL;
    for (n = a->extra->activations; n != NULL; n = n->next) if (n->state == state) return n;
    return NULL;
}

ta_status_t ta_context_arena(ta_context_t *context, ta_lifetime_t lifetime, ta_arena_t **out_arena)
{
    tai_actor_t *a; ta_status_t s; ta_arena_t *parent, *child; tai_activation_t *n;
    uint32_t state, p;
    if (context == NULL || out_arena == NULL) return TA_INVALID;
    *out_arena = NULL; a = context->actor;
    if (a == NULL) return TA_INVALID;
    if (context->view.phase == TA_PHASE_MIGRATE && lifetime != TA_LIFETIME_MACROSTEP) return TA_INVALID;
    if (lifetime == TA_LIFETIME_ACTOR) { *out_arena = a->arena; return TA_OK; }
    if (lifetime == TA_LIFETIME_MACROSTEP) {
        s = tai_extra(a);
        if (s != TA_OK) return s;
        if (a->extra->macrostep == NULL) {
            s = tai_actor_scope(a, lifetime, &a->extra->macrostep);
            if (s != TA_OK) return s;
        }
        *out_arena = a->extra->macrostep;
        return TA_OK;
    }
    if (lifetime != TA_LIFETIME_ACTIVATION) return TA_INVALID;
    state = context->view.state_id;
    if (state == TA_INDEX_NONE || !tai_active(a, state)) return TA_NOT_FOUND;
    s = tai_extra(a); if (s != TA_OK) return s;
    n = tai_find_activation(a, state); if (n != NULL) { *out_arena = n->arena; return TA_OK; }
    for (;;) {
        uint32_t candidate = TA_INDEX_NONE;
        for (p = state; p != TA_INDEX_NONE; p = a->definition->description.states[p].parent)
            if (tai_find_activation(a, p) == NULL && (a->definition->description.states[p].parent == TA_INDEX_NONE || tai_find_activation(a, a->definition->description.states[p].parent) != NULL)) { candidate = p; break; }
        if (candidate == TA_INDEX_NONE) break;
        parent = a->arena;
        p = a->definition->description.states[candidate].parent;
        if (p != TA_INDEX_NONE) parent = tai_find_activation(a, p)->arena;
        s = ta_arena_open(parent, &a->system->config.arenas[TA_LIFETIME_ACTIVATION], &child);
        if (s != TA_OK) return s;
        s = tai_alloc(child, sizeof(*n), (void **)&n); if (s != TA_OK) { ta_arena_close(child); return s; }
        n->arena = child; n->state = candidate; n->next = a->extra->activations; a->extra->activations = n;
    }
    n = tai_find_activation(a, state); if (n == NULL) return TA_NO_MEMORY;
    *out_arena = n->arena; return TA_OK;
}

void tai_activation_close(tai_actor_t *actor, uint32_t state)
{
    tai_activation_t *n, *prev, *next, *q, *qp;
    if (actor == NULL || actor->extra == NULL) return;
    prev = NULL; n = actor->extra->activations;
    while (n != NULL) {
        next = n->next;
        if (tai_descends(actor->definition, state, n->state)) {
            q = actor->extra->activations; qp = NULL;
            while (q != n) { qp = q; q = q->next; }
            if (qp == NULL) actor->extra->activations = n->next; else qp->next = n->next;
            ta_arena_close(n->arena);
        }
        prev = n; n = next;
    }
    (void)prev;
}
