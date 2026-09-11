#include "internal.h"

enum { INSPECT_ACTOR = 0, INSPECT_ACTIVE = 1, INSPECT_HISTORY = 2,
       INSPECT_EVENT = 3, INSPECT_NEXT = 4 };

static uint64_t inspect_fingerprint(const ta_inspect_options_t *o)
{
    uint64_t h = UINT64_C(14695981039346656037);
    h ^= o->actor.id; h *= UINT64_C(1099511628211);
    h ^= o->revision; h *= UINT64_C(1099511628211);
    h ^= o->stable_only; h *= UINT64_C(1099511628211);
    h ^= o->details; h *= UINT64_C(1099511628211);
    return h;
}

static void inspect_reset(ta_inspect_cursor_t *c)
{
    *c = (ta_inspect_cursor_t){{0, 0, 0, 0, 0, 0}};
}

static uint8_t inspect_match(const tai_actor_t *a, const ta_inspect_options_t *o)
{
    if (o->actor.id != 0 && a->id != o->actor.id) return 0;
    if (o->revision != 0 && a->definition->revision != o->revision) return 0;
    if (o->stable_only && !a->stable) return 0;
    return 1;
}

static tai_actor_t *inspect_next(const ta_system_t *s, const ta_inspect_cursor_t *c)
{
    if (c->opaque[2] == 0) return NULL;
    return tai_actor_find(s, (ta_actor_ref_t){c->opaque[2]});
}

static ta_visit_t inspect_visit(ta_system_t *s, const ta_inspect_sink_t *sink,
                                const ta_inspect_record_t *r)
{
    uint8_t was_running = s->running;
    ta_visit_t v;
    s->running = 1;
    v = sink->visit(sink->user, r);
    s->running = was_running;
    return v;
}

typedef struct inspect_call {
    ta_system_t *system;
    const ta_inspect_options_t *options;
    ta_inspect_cursor_t *cursor;
    size_t *budget;
} inspect_call_t;

static ta_status_t inspect_record(const inspect_call_t *call,
                                  const ta_inspect_record_t *r)
{
    ta_visit_t v;
    if (*call->budget == 0) return TA_AGAIN;
    --*call->budget;
    v = inspect_visit(call->system, &call->options->sink, r);
    if (v == TA_VISIT_STOP) { inspect_reset(call->cursor); return TA_CANCELLED; }
    if (v == TA_VISIT_PAUSE) return TA_AGAIN;
    return TA_OK;
}

static ta_status_t inspect_deliver(const inspect_call_t *call,
                                   const ta_inspect_record_t *r)
{
    ta_visit_t v = inspect_visit(call->system, &call->options->sink, r);
    if (v == TA_VISIT_STOP) { inspect_reset(call->cursor); return TA_CANCELLED; }
    if (v == TA_VISIT_PAUSE) return TA_AGAIN;
    return TA_OK;
}

static tai_history_t *inspect_history(const tai_actor_t *a, uint32_t state)
{
    tai_history_t *h = a->extra == NULL ? NULL : a->extra->history;
    while (h != NULL && h->state != state) h = h->next;
    return h;
}

ta_status_t ta_system_inspect(ta_system_t *s, const ta_inspect_options_t *o,
                              ta_inspect_cursor_t *c)
{
    uint64_t fp, packed;
    tai_actor_t *a;
    uint32_t state;
    uint8_t phase;
    size_t budget;
    ta_inspect_record_t r;
    ta_status_t status;
    inspect_call_t call;
    if (s == NULL || o == NULL || c == NULL || o->work_units == 0 ||
        o->sink.visit == NULL || o->stable_only > 1 ||
        (o->details & ~(uint32_t)(TA_INSPECT_DATA | TA_INSPECT_ACTIVE |
                                   TA_INSPECT_HISTORY | TA_INSPECT_EVENT)) != 0)
        return TA_INVALID;
    if (s->running) return TA_BUSY;
    if (s->mutation_exhausted) return TA_LIMIT;
    if (o->actor.id != 0 && tai_actor_find(s, o->actor) == NULL) return TA_STALE;
    fp = inspect_fingerprint(o);
    if (c->opaque[0] == 0) {
        c->opaque[0] = (uint64_t)(uintptr_t)s;
        c->opaque[1] = s->mutation;
        c->opaque[5] = fp;
        c->opaque[2] = o->actor.id != 0 ? o->actor.id : (s->actors == NULL ? 0 : s->actors->id);
        c->opaque[3] = INSPECT_ACTOR;
        c->opaque[4] = 0;
    } else {
        if (c->opaque[0] != (uint64_t)(uintptr_t)s || c->opaque[5] != fp)
            return TA_INVALID;
        if (c->opaque[1] != s->mutation) return TA_STALE;
    }
    budget = o->work_units;
    call = (inspect_call_t){s, o, c, &budget};
    for (;;) {
        a = inspect_next(s, c);
        if (a == NULL) { inspect_reset(c); return TA_OK; }
        if (budget == 0) return TA_AGAIN;
        packed = c->opaque[3]; phase = (uint8_t)(packed & UINT64_C(0xff));
        state = (uint32_t)(packed >> 32);
        if (phase == INSPECT_ACTOR) {
            c->opaque[2] = o->actor.id != 0 || a->next == NULL ? 0 : a->next->id;
            c->opaque[3] = INSPECT_ACTOR;
            if (!inspect_match(a, o)) {
                if (budget == 0) return TA_AGAIN;
                --budget;
                continue;
            }
            if (budget == 0) return TA_AGAIN;
            r = (ta_inspect_record_t){0}; r.kind = TA_INSPECT_ACTOR;
            r.actor = (ta_actor_ref_t){a->id}; r.definition = a->definition;
            tai_actor_info(a, &r.as.info);
            if (!(o->details & TA_INSPECT_DATA)) r.as.info.state = (ta_data_view_t){0};
            if (o->details & TA_INSPECT_ACTIVE) phase = INSPECT_ACTIVE;
            else if (o->details & TA_INSPECT_HISTORY) phase = INSPECT_HISTORY;
            else if (o->details & TA_INSPECT_EVENT) phase = INSPECT_EVENT;
            else phase = INSPECT_NEXT;
            c->opaque[2] = a->id; c->opaque[3] = phase;
            --budget;
            status = inspect_deliver(&call, &r);
            if (status != TA_OK) return status;
            if (budget == 0) return TA_AGAIN;
        }
        if (phase == INSPECT_ACTIVE) {
            while (state < a->definition->description.state_count) {
                if (!tai_active(a, state)) { ++state; continue; }
                r = (ta_inspect_record_t){0}; r.kind = TA_INSPECT_ACTIVE_STATE;
                r.actor = (ta_actor_ref_t){a->id}; r.definition = a->definition;
                r.as.active.id = state; r.as.active.description = &a->definition->description.states[state];
                ++state; c->opaque[3] = ((uint64_t)state << 32) | INSPECT_ACTIVE;
                status = inspect_record(&call, &r);
                if (status != TA_OK) return status;
                if (budget == 0) return TA_AGAIN;
            }
            state = 0; phase = (o->details & TA_INSPECT_HISTORY) ? INSPECT_HISTORY :
                ((o->details & TA_INSPECT_EVENT) ? INSPECT_EVENT : INSPECT_NEXT);
            c->opaque[3] = phase; c->opaque[4] = 0;
        }
        if (phase == INSPECT_HISTORY) {
            while (state < a->definition->description.state_count) {
                tai_history_t *h;
                if (state >= a->definition->description.state_count) break;
                if (a->definition->description.states[state].kind != TA_STATE_HISTORY_SHALLOW &&
                    a->definition->description.states[state].kind != TA_STATE_HISTORY_DEEP) { ++state; continue; }
                h = inspect_history(a, state);
                if (h == NULL && c->opaque[4] != 0) { c->opaque[4] = 0; ++state; continue; }
                if (h != NULL && c->opaque[4] >= h->count) { c->opaque[4] = 0; ++state; continue; }
                r = (ta_inspect_record_t){0}; r.kind = TA_INSPECT_HISTORY_STATE;
                r.actor = (ta_actor_ref_t){a->id}; r.definition = a->definition;
                r.as.history.id = state; r.as.history.ordinal = c->opaque[4];
                r.as.history.recorded = h != NULL;
                r.as.history.target = h == NULL ? TA_INDEX_NONE : h->targets[c->opaque[4]];
                c->opaque[3] = ((uint64_t)state << 32) | INSPECT_HISTORY;
                ++c->opaque[4];
                status = inspect_record(&call, &r);
                if (status != TA_OK) return status;
                if (budget == 0) return TA_AGAIN;
            }
            c->opaque[4] = 0; phase = (o->details & TA_INSPECT_EVENT) ? INSPECT_EVENT : INSPECT_NEXT;
            c->opaque[3] = phase;
        }
        if (phase == INSPECT_EVENT) {
            ta_event_t scratch; const ta_event_t *ev = tai_current_event(a, &scratch);
            if (ev != NULL) {
                r = (ta_inspect_record_t){0}; r.kind = TA_INSPECT_CURRENT_EVENT;
                r.actor = (ta_actor_ref_t){a->id}; r.definition = a->definition; r.as.event = *ev;
                c->opaque[3] = INSPECT_NEXT;
                status = inspect_record(&call, &r);
                if (status != TA_OK) return status;
                if (budget == 0) return TA_AGAIN;
            }
            phase = INSPECT_NEXT; c->opaque[3] = phase;
        }
        if (phase == INSPECT_NEXT) {
            c->opaque[2] = o->actor.id != 0 || a->next == NULL ? 0 : a->next->id;
            c->opaque[3] = INSPECT_ACTOR;
            c->opaque[4] = 0;
        }
    }
}
