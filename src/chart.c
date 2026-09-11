#include "internal.h"

/* Lower the temporary authoring tree into the common validated, copied form. */
typedef struct chart_record {
    struct chart_record *next;
    const ta_chart_state_t *source;
    uint32_t id, parent, transitions;
} chart_record_t;
typedef struct chart_frame {
    struct chart_frame *previous;
    const ta_chart_state_t *next;
    uint32_t parent;
} chart_frame_t;
typedef struct chart_work {
    ta_system_t *system;
    ta_arena_t *arena;
    chart_record_t *head, *tail;
    uint32_t states, transitions;
} chart_work_t;
typedef struct chart_tokens {
    const char *text;
    uint8_t events;
    void *items;
    uint32_t count;
} chart_tokens_t;

static uint8_t whitespace(char c)
{ return (uint8_t)(c == ' ' || c == '\t' || c == '\r' || c == '\n'); }

static uint8_t empty_body(const ta_body_expr_t *b)
{
    return (uint8_t)(b->kind == TA_BODY_NONE && b->content == 0 &&
        b->bytes_content == 0 && b->parameters == NULL && b->parameter_count == 0);
}

static uint8_t state_end(const ta_chart_state_t *s)
{
    return (uint8_t)(s->id == NULL && s->kind == TA_CHART_STATE &&
        s->initial.target == NULL && s->initial.content.actions == NULL &&
        s->initial.content.count == 0 && s->states == NULL && s->transitions == NULL &&
        s->onentry.blocks == NULL && s->onentry.count == 0 &&
        s->onexit.blocks == NULL && s->onexit.count == 0 &&
        s->data.items == NULL && s->data.count == 0 &&
        s->invokes.items == NULL && s->invokes.count == 0 && empty_body(&s->done_data));
}

static uint8_t transition_end(const ta_chart_transition_t *t)
{
    return (uint8_t)(t->kind == TA_TRANSITION_EXTERNAL && t->event == NULL &&
        t->target == NULL && t->condition == 0 &&
        t->content.actions == NULL && t->content.count == 0);
}

static ta_status_t record_state(chart_work_t *w, const chart_frame_t *frame,
                                chart_record_t **out)
{
    const ta_chart_state_t *source = frame->next;
    chart_record_t *record;
    const ta_chart_transition_t *transition;
    ta_text_t name;
    ta_status_t status;
    size_t i;
    if (source->id == NULL || source->id[0] == '\0') return TA_INVALID;
    if ((unsigned)source->kind > TA_CHART_HISTORY_DEEP) return TA_UNSUPPORTED;
    name = ta_text_from_cstr(source->id);
    if (tai_text_compare(name, ta_text_from_cstr("$root")) == 0) return TA_CONFLICT;
    for (i = 0; i < name.size; ++i) if (whitespace(name.data[i])) return TA_INVALID;
    for (record = w->head; record != NULL; record = record->next)
        if (record->source == source ||
            tai_text_compare(name, ta_text_from_cstr(record->source->id)) == 0)
            return TA_CONFLICT;
    if (w->states == UINT32_MAX || w->states >= w->system->config.limits.definition_states)
        return TA_LIMIT;
    status = tai_alloc(w->arena, sizeof(*record), (void **)&record);
    if (status != TA_OK) return status;
    *record = (chart_record_t){NULL, source, w->states++, frame->parent, 0};
    if (w->tail != NULL) w->tail->next = record; else w->head = record;
    w->tail = record;
    for (transition = source->transitions;
         transition != NULL && !transition_end(transition); ++transition) {
        if (w->transitions == UINT32_MAX ||
            w->transitions >= w->system->config.limits.definition_transitions) return TA_LIMIT;
        ++w->transitions;
        ++record->transitions;
    }
    *out = record;
    return TA_OK;
}

static ta_status_t collect(chart_work_t *w, const ta_chart_state_t *states)
{
    chart_frame_t root = {NULL, states, 0}, *top = &root, *free_frames = NULL;
    size_t depth = 1;
    if (w->system->config.limits.state_depth < 1 ||
        w->system->config.limits.definition_states < 1) return TA_LIMIT;
    while (top != NULL) {
        chart_record_t *record;
        ta_status_t status;
        if (top->next == NULL || state_end(top->next)) {
            chart_frame_t *finished = top;
            top = top->previous;
            if (finished != &root) { finished->previous = free_frames; free_frames = finished; }
            --depth;
            continue;
        }
        if (depth >= w->system->config.limits.state_depth) return TA_LIMIT;
        status = record_state(w, top, &record);
        if (status != TA_OK) return status;
        ++top->next;
        if (record->source->states != NULL) {
            chart_frame_t *child = free_frames;
            if (child != NULL) free_frames = child->previous;
            else {
                status = tai_alloc(w->arena, sizeof(*child), (void **)&child);
                if (status != TA_OK) return status;
            }
            *child = (chart_frame_t){top, record->source->states, record->id};
            top = child;
            ++depth;
        }
    }
    return TA_OK;
}

static ta_status_t tokens(chart_work_t *w, chart_tokens_t *request)
{
    const char *p = request->text;
    ta_status_t status;
    tai_array_layout_t layout;
    uint32_t count = 0;
    request->items = NULL;
    request->count = 0;
    if (p == NULL) return TA_OK;
    for (;;) {
        while (whitespace(*p)) ++p;
        if (*p == '\0') break;
        if (count == UINT32_MAX) return TA_LIMIT;
        ++count;
        while (*p != '\0' && !whitespace(*p)) ++p;
    }
    if (count == 0) return TA_INVALID;
    layout = (tai_array_layout_t){count,
        request->events ? sizeof(ta_text_t) : sizeof(ta_state_id_t), TAI_ALIGNMENT};
    status = tai_array(w->arena, &layout, &request->items);
    if (status != TA_OK) return status;
    p = request->text;
    while (request->count != count) {
        ta_text_t token;
        while (whitespace(*p)) ++p;
        token.data = p;
        while (*p != '\0' && !whitespace(*p)) ++p;
        token.size = (size_t)(p - token.data);
        if (request->events) {
            if (token.size > w->system->config.limits.event_name_bytes) return TA_LIMIT;
            ((ta_text_t *)request->items)[request->count] = token;
        } else {
            chart_record_t *record;
            for (record = w->head; record != NULL; record = record->next)
                if (tai_text_compare(token, ta_text_from_cstr(record->source->id)) == 0) break;
            if (record == NULL) return TA_NOT_FOUND;
            ((ta_state_id_t *)request->items)[request->count] = record->id;
        }
        ++request->count;
    }
    return TA_OK;
}

static ta_status_t initial(chart_work_t *w, const ta_chart_initial_t *source,
                            ta_initial_t *out)
{
    chart_tokens_t request = {source->target, 0, NULL, 0};
    ta_status_t status = tokens(w, &request);
    if (status == TA_OK)
        *out = (ta_initial_t){{(const ta_state_id_t *)request.items, request.count}, source->content};
    return status;
}

static ta_status_t lower_transition(chart_work_t *w,
                                    const ta_chart_transition_t *source,
                                    ta_transition_desc_t *out)
{
    chart_tokens_t event = {source->event, 1, NULL, 0};
    chart_tokens_t target = {source->target, 0, NULL, 0};
    ta_status_t status = tokens(w, &event);
    if (status == TA_OK) status = tokens(w, &target);
    if (status != TA_OK) return status;
    out->kind = source->kind;
    out->events = (const ta_text_t *)event.items;
    out->event_count = event.count;
    out->condition = source->condition;
    out->targets = (ta_targets_t){(const ta_state_id_t *)target.items, target.count};
    out->content = source->content;
    return TA_OK;
}

static ta_state_kind_t kind(const ta_chart_state_t *source)
{
    switch (source->kind) {
    case TA_CHART_PARALLEL: return TA_STATE_PARALLEL;
    case TA_CHART_FINAL: return TA_STATE_FINAL;
    case TA_CHART_HISTORY_SHALLOW: return TA_STATE_HISTORY_SHALLOW;
    case TA_CHART_HISTORY_DEEP: return TA_STATE_HISTORY_DEEP;
    case TA_CHART_STATE:
        return source->states != NULL && !state_end(source->states) ? TA_STATE_COMPOUND : TA_STATE_ATOMIC;
    }
    return TA_STATE_ATOMIC;
}

static ta_status_t lower(chart_work_t *w, const ta_chart_t *chart,
                         ta_definition_desc_t *out)
{
    ta_state_desc_t *states;
    ta_transition_desc_t *transitions;
    chart_record_t *record;
    uint32_t i, index = 0;
    tai_array_layout_t layout = {w->states, sizeof(*states), TAI_ALIGNMENT};
    ta_status_t status = tai_array(w->arena, &layout, (void **)&states);
    if (status != TA_OK) return status;
    layout = (tai_array_layout_t){w->transitions, sizeof(*transitions), TAI_ALIGNMENT};
    status = tai_array(w->arena, &layout, (void **)&transitions);
    if (status != TA_OK) return status;
    for (i = 0; i < w->states; ++i) states[i] = (ta_state_desc_t){0};
    for (i = 0; i < w->transitions; ++i) transitions[i] = (ta_transition_desc_t){0};
    states[0].name = ta_text_from_cstr("$root");
    states[0].kind = TA_STATE_ROOT;
    states[0].parent = TA_INDEX_NONE;
    states[0].data = chart->data.items;
    states[0].data_count = chart->data.count;
    status = initial(w, &chart->initial, &states[0].initial);
    if (status != TA_OK) return status;
    for (record = w->head; record != NULL; record = record->next) {
        const ta_chart_state_t *source = record->source;
        ta_state_desc_t *state = &states[record->id];
        state->name = ta_text_from_cstr(source->id);
        state->parent = record->parent;
        state->kind = kind(source);
        state->onentry = source->onentry;
        state->onexit = source->onexit;
        state->data = source->data.items;
        state->data_count = source->data.count;
        state->invokes = source->invokes.items;
        state->invoke_count = source->invokes.count;
        state->done_data = source->done_data;
        status = initial(w, &source->initial, &state->initial);
        if (status != TA_OK) return status;
        state->first_transition = index;
        state->transition_count = record->transitions;
        for (i = 0; i < record->transitions; ++i, ++index) {
            transitions[index].source = record->id;
            status = lower_transition(w, &source->transitions[i], &transitions[index]);
            if (status != TA_OK) return status;
        }
    }
    *out = (ta_definition_desc_t){0};
    out->name = ta_text_from_cstr(chart->name);
    out->data_model = ta_text_from_cstr(chart->data_model);
    out->code_owner = chart->code_owner;
    out->data_layout = chart->data_layout;
    out->binding = chart->binding;
    out->states = states;
    out->state_count = w->states;
    out->transitions = transitions;
    out->transition_count = w->transitions;
    out->root_script = chart->root_script;
    return TA_OK;
}

ta_status_t ta_definition_build(ta_system_t *system, const ta_chart_t *chart,
                                ta_definition_result_t *out)
{
    chart_work_t work = {0};
    ta_definition_desc_t description;
    ta_status_t status;
    if (out == NULL) return TA_INVALID;
    *out = (ta_definition_result_t){0};
    out->error.status = TA_INVALID;
    out->error.state = TA_INDEX_NONE;
    out->error.transition = TA_INDEX_NONE;
    if (system == NULL || chart == NULL) return TA_INVALID;
    if (system->running) { out->error.status = TA_BUSY; return TA_BUSY; }
    if (system->shutdown) { out->error.status = TA_CANCELLED; return TA_CANCELLED; }
    work.system = system;
    work.states = 1;
    status = tai_scope(system, TA_LIFETIME_DEFINITION, &work.arena);
    if (status == TA_OK) status = collect(&work, chart->states);
    if (status == TA_OK) status = lower(&work, chart, &description);
    if (status == TA_OK) {
        ta_definition_input_t input = {&description, {TA_DEFINITION_COPY, NULL, NULL}};
        status = ta_definition_prepare(system, &input, out);
    }
    if (work.arena != NULL) ta_arena_close(work.arena);
    out->error.status = status;
    return status;
}
