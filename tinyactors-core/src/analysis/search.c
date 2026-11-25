/* analysis/search.c — a chart's configurations (machine.h) and the path
 * between two of them (analysis.h). Freestanding. */
#include "analysis/machine.h"

#include "base/memory.h"

/* ---- arrays in the arena ------------------------------------------------------------------ */

uint32_t *ta_analysis_words(ta_analysis_machine *machine, uint32_t count) {
    uint32_t *words = (uint32_t *)ta_growable_arena_alloc(machine->arena, sizeof(uint32_t) * (count ? count : 1u), 4);
    if (words) ta_memset(words, 0, sizeof(uint32_t) * (count ? count : 1u));
    return words;
}

/* `old` (`kept` words) copied into a new array of `count` words, or NULL. */
typedef struct grown_words {
    const uint32_t *old;
    uint32_t kept, count;
} grown_words;

static uint32_t *grow(ta_analysis_machine *machine, grown_words request) {
    uint32_t *words = ta_analysis_words(machine, request.count);
    if (words && request.kept && request.old) ta_memcpy(words, request.old, sizeof(uint32_t) * request.kept);
    return words;
}

/* ---- entry --------------------------------------------------------------------------------- */

typedef enum entry_task {
    ENTER_DEFAULT = 0,  /* the state, and what entering it by default enters */
    ENTER_ANCESTORS = 1 /* the state's proper ancestors below a stop */
} entry_task;

static ta_status push(ta_analysis_machine *machine, uint32_t kind, uint32_t state, uint32_t stop) {
    if (machine->task_count + 3u > machine->task_capacity) {
        uint32_t capacity = machine->task_capacity ? machine->task_capacity * 2u : 96u;
        uint32_t *tasks = grow(machine, (grown_words){machine->tasks, machine->task_count, capacity});
        if (!tasks) return TA_ERROR_NO_MEMORY;
        machine->tasks = tasks;
        machine->task_capacity = capacity;
    }
    machine->tasks[machine->task_count++] = kind;
    machine->tasks[machine->task_count++] = state;
    machine->tasks[machine->task_count++] = stop;
    return TA_OK;
}

static const uint32_t *targets_of(const ta_analysis_machine *machine, const ta_transition_record *transition) {
    return machine->view->u32 + transition->targets_offset;
}

/* Whether `out` holds `state` or one of its descendants. */
static int holds_within(const ta_analysis_machine *machine, const uint32_t *out, uint32_t state) {
    uint32_t end = machine->states[state].end;
    for (uint32_t s = state; s < end; s++)
        if (ta_analysis_bit(out, s)) return 1;
    return 0;
}

/* Each region of a parallel state with nothing entered in it yet is
 * entered by default (addDescendantStatesToEnter, addAncestorStatesToEnter). */
static ta_status regions(ta_analysis_machine *machine, uint32_t parallel, const uint32_t *out) {
    const ta_state_record *states = machine->states;
    for (uint32_t child = parallel + 1; child < states[parallel].end; child = states[child].end) {
        if (states[child].kind >= TA_STATE_HISTORY_SHALLOW || holds_within(machine, out, child)) continue;
        ta_status status = push(machine, ENTER_DEFAULT, child, TA_NIL);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

/* The targets of a default transition (a compound state's initial, a
 * history state's default): each entered by default, then its ancestors
 * below `stop`. Pushed so the defaults run first. */
static ta_status defaults(ta_analysis_machine *machine, uint32_t transition, uint32_t stop) {
    if (transition == TA_NIL || transition >= machine->transition_count) return TA_OK;
    const ta_transition_record *record = &machine->transitions[transition];
    const uint32_t *targets = targets_of(machine, record);
    for (uint32_t i = 0; i < record->target_count; i++) {
        ta_status status = push(machine, ENTER_ANCESTORS, targets[i], stop);
        if (status != TA_OK) return status;
    }
    for (uint32_t i = record->target_count; i-- > 0;) {
        ta_status status = push(machine, ENTER_DEFAULT, targets[i], TA_NIL);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

static ta_status run_task(ta_analysis_machine *machine, const uint32_t task[3], uint32_t *out) {
    const ta_state_record *states = machine->states;
    uint32_t state = task[1];
    if (task[0] == ENTER_ANCESTORS) {
        for (uint32_t a = states[state].parent; a != TA_NIL && a != task[2]; a = states[a].parent) {
            ta_analysis_set(out, a);
            if (states[a].kind != TA_STATE_PARALLEL) continue;
            ta_status status = regions(machine, a, out);
            if (status != TA_OK) return status;
        }
        return TA_OK;
    }
    const ta_state_record *record = &states[state];
    if (record->kind >= TA_STATE_HISTORY_SHALLOW) {
        ta_analysis_set(machine->histories, state);
        return defaults(machine, record->initial, record->parent);
    }
    if (ta_analysis_bit(out, state)) return TA_OK;
    ta_analysis_set(out, state);
    if (record->kind == TA_STATE_PARALLEL) return regions(machine, state, out);
    if (record->kind == TA_STATE_COMPOUND) return defaults(machine, record->initial, state);
    return TA_OK;
}

ta_status ta_analysis_enter(ta_analysis_machine *machine, ta_analysis_entry entry, uint32_t *out) {
    ta_memset(out, 0, sizeof(uint32_t) * machine->words);
    machine->task_count = 0;
    ta_status status = TA_OK;
    /* computeEntrySet: every target's descendants first, then the ancestors */
    for (uint32_t i = 0; i < entry.count && status == TA_OK; i++)
        status = push(machine, ENTER_ANCESTORS, entry.targets[i], entry.domain);
    for (uint32_t i = entry.count; i-- > 0 && status == TA_OK;)
        status = push(machine, ENTER_DEFAULT, entry.targets[i], TA_NIL);
    while (status == TA_OK && machine->task_count) {
        machine->task_count -= 3;
        uint32_t task[3] = {machine->tasks[machine->task_count], machine->tasks[machine->task_count + 1],
                            machine->tasks[machine->task_count + 2]};
        status = run_task(machine, task, out);
    }
    return status;
}

/* ---- the machine ----------------------------------------------------------------------------- */

int ta_analysis_movable(const ta_analysis_machine *machine, uint32_t transition) {
    const ta_transition_record *record = &machine->transitions[transition];
    return !(record->flags & TA_TRANSITION_FLAG_INITIAL) && record->target_count &&
           machine->shadows[transition] == TA_NIL;
}

/* Whether every descriptor of `later` is within one of `earlier`'s (the
 * event names' trie: a descriptor is the interval of the names it
 * matches). */
static int covered(const ta_image_view *view, const ta_transition_record *earlier, const ta_transition_record *later) {
    const uint32_t *wide = view->u32 + earlier->events_offset, *narrow = view->u32 + later->events_offset;
    for (uint32_t j = 0; j < later->event_count; j++) {
        int within = 0;
        for (uint32_t i = 0; i < earlier->event_count && !within; i++)
            within = wide[2 * i] <= narrow[2 * j] && narrow[2 * j + 1] <= wide[2 * i + 1];
        if (!within) return 0;
    }
    return 1;
}

/* The transition of the same state always taken before `later`: one
 * without a condition whose descriptors cover all of later's, earlier in
 * document order; or an eventless one without a condition, taken before
 * any event and before the eventless transitions after it. */
static uint32_t shadow_of(const ta_analysis_machine *machine, uint32_t later) {
    const ta_transition_record *record = &machine->transitions[later];
    if (record->flags & TA_TRANSITION_FLAG_INITIAL) return TA_NIL;
    const ta_state_record *state = &machine->states[record->source];
    for (uint32_t i = state->first_transition; i < state->first_transition + state->transition_count; i++) {
        const ta_transition_record *earlier = &machine->transitions[i];
        if (i == later || earlier->condition != TA_NIL) continue;
        if (!earlier->event_count) {
            if (record->event_count || i < later) return i;
        } else if (i < later && record->event_count && covered(machine->view, earlier, record)) {
            return i;
        }
    }
    return TA_NIL;
}

static int all_below(const ta_analysis_machine *machine, const ta_transition_record *record, uint32_t ancestor) {
    const uint32_t *targets = targets_of(machine, record);
    for (uint32_t i = 0; i < record->target_count; i++)
        if (!ta_analysis_below(machine, targets[i], ancestor)) return 0;
    return 1;
}

/* SCXML's getTransitionDomain: the compiler's, but where a target is a
 * history state, whose domain it leaves to run time. */
static uint32_t domain_of(const ta_analysis_machine *machine, uint32_t transition) {
    const ta_transition_record *record = &machine->transitions[transition];
    if (record->domain != TA_NIL && !(record->flags & TA_TRANSITION_FLAG_HISTORY_TARGET)) return record->domain;
    const ta_state_record *states = machine->states;
    uint32_t source = record->source;
    if ((record->flags & TA_TRANSITION_FLAG_INTERNAL) && states[source].kind == TA_STATE_COMPOUND &&
        all_below(machine, record, source))
        return source;
    for (uint32_t a = states[source].parent; a != TA_NIL; a = states[a].parent)
        if ((a == 0 || states[a].kind == TA_STATE_COMPOUND) && all_below(machine, record, a)) return a;
    return 0;
}

ta_status ta_analysis_machine_init(ta_analysis_machine *machine, ta_analysis_machine_setup setup) {
    ta_memset(machine, 0, sizeof *machine);
    const ta_chart_record *chart = &setup.view->charts[setup.chart];
    machine->view = setup.view;
    machine->chart = chart;
    machine->states = ta_chart_states(setup.view, chart);
    machine->transitions = ta_chart_transitions(setup.view, chart);
    machine->state_count = chart->state_count;
    machine->transition_count = chart->transition_count;
    machine->words = (chart->state_count + 31u) / 32u;
    machine->arena = setup.arena;
    machine->domains = ta_analysis_words(machine, chart->transition_count);
    machine->shadows = ta_analysis_words(machine, chart->transition_count);
    machine->entries = ta_analysis_words(machine, chart->transition_count * machine->words);
    machine->initial = ta_analysis_words(machine, machine->words);
    machine->histories = ta_analysis_words(machine, machine->words);
    if (!machine->domains || !machine->shadows || !machine->entries || !machine->initial || !machine->histories)
        return TA_ERROR_NO_MEMORY;
    for (uint32_t t = 0; t < chart->transition_count; t++)
        machine->shadows[t] = shadow_of(machine, t);
    for (uint32_t t = 0; t < chart->transition_count; t++) {
        machine->domains[t] = TA_NIL;
        if (!ta_analysis_movable(machine, t)) continue;
        const ta_transition_record *record = &machine->transitions[t];
        machine->domains[t] = domain_of(machine, t);
        ta_analysis_entry entry = {targets_of(machine, record), record->target_count, machine->domains[t]};
        ta_status status = ta_analysis_enter(machine, entry, machine->entries + t * machine->words);
        if (status != TA_OK) return status;
    }
    uint32_t root = 0;
    return ta_analysis_enter(machine, (ta_analysis_entry){&root, 1, TA_NIL}, machine->initial);
}

void ta_analysis_apply(const ta_analysis_machine *machine, ta_analysis_move move, uint32_t *next) {
    uint32_t domain = machine->domains[move.transition];
    const uint32_t *entered = machine->entries + move.transition * machine->words;
    for (uint32_t w = 0; w < machine->words; w++)
        next[w] = move.from[w];
    /* the exit set: every active descendant of the domain */
    for (uint32_t s = domain + 1; s < machine->states[domain].end; s++)
        next[s >> 5] &= ~(1u << (s & 31u));
    for (uint32_t w = 0; w < machine->words; w++)
        next[w] |= entered[w];
}

int ta_analysis_ends(const ta_analysis_machine *machine, const uint32_t *configuration) {
    const ta_state_record *states = machine->states;
    for (uint32_t child = 1; child < machine->state_count; child = states[child].end)
        if (states[child].kind == TA_STATE_FINAL && ta_analysis_bit(configuration, child)) return 1;
    return 0;
}

/* ---- the search ----------------------------------------------------------------------------- */

static uint32_t hash_of(const uint32_t *words, uint32_t count) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < count; i++) {
        hash ^= words[i];
        hash *= 16777619u;
    }
    return hash;
}

static const uint32_t *configuration_at(const ta_analysis_search *search, uint32_t index) {
    return search->configurations + index * search->machine->words;
}

static int same(const uint32_t *a, const uint32_t *b, uint32_t words) {
    for (uint32_t i = 0; i < words; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static void place(ta_analysis_search *search, uint32_t index) {
    uint32_t words = search->machine->words, mask = search->table_size - 1u;
    uint32_t slot = hash_of(configuration_at(search, index), words) & mask;
    while (search->table[slot]) slot = (slot + 1u) & mask;
    search->table[slot] = index + 1u;
}

/* Room for one more configuration (and its table entry). */
static ta_status make_room(ta_analysis_search *search) {
    ta_analysis_machine *machine = search->machine;
    if (search->count < search->capacity) return TA_OK;
    uint32_t capacity = search->capacity ? search->capacity * 2u : 64u;
    uint32_t words = machine->words;
    uint32_t *configurations = grow(machine, (grown_words){search->configurations, search->count * words, capacity * words});
    uint32_t *parents = grow(machine, (grown_words){search->parents, search->count, capacity});
    uint32_t *via = grow(machine, (grown_words){search->via, search->count, capacity});
    uint32_t *first_edge = grow(machine, (grown_words){search->first_edge, search->count + 1u, capacity + 1u});
    uint32_t *table = ta_analysis_words(machine, capacity * 2u);
    if (!configurations || !parents || !via || !first_edge || !table) return TA_ERROR_NO_MEMORY;
    search->configurations = configurations;
    search->parents = parents;
    search->via = via;
    search->first_edge = first_edge;
    search->capacity = capacity;
    search->table = table;
    search->table_size = capacity * 2u;
    for (uint32_t i = 0; i < search->count; i++)
        place(search, i);
    return TA_OK;
}

/* The index of `configuration`, added when new (reached from `parent`
 * through `via`). TA_NIL: new, but the limit is reached. */
typedef struct reached_from {
    const uint32_t *configuration;
    uint32_t parent, via;
} reached_from;

static ta_status find_or_add(ta_analysis_search *search, reached_from reached, uint32_t *index) {
    uint32_t words = search->machine->words;
    if (search->table_size) {
        uint32_t mask = search->table_size - 1u;
        for (uint32_t slot = hash_of(reached.configuration, words) & mask; search->table[slot];
             slot = (slot + 1u) & mask) {
            uint32_t candidate = search->table[slot] - 1u;
            if (same(configuration_at(search, candidate), reached.configuration, words)) {
                *index = candidate;
                return TA_OK;
            }
        }
    }
    if (search->count >= search->limit) {
        search->stopped = 1;
        *index = TA_NIL;
        return TA_OK;
    }
    ta_status status = make_room(search);
    if (status != TA_OK) return status;
    uint32_t added = search->count++;
    ta_memcpy(search->configurations + added * words, reached.configuration, sizeof(uint32_t) * words);
    search->parents[added] = reached.parent;
    search->via[added] = reached.via;
    place(search, added);
    *index = added;
    return TA_OK;
}

static ta_status add_edge(ta_analysis_search *search, uint32_t to) {
    if (search->edge_count >= search->edge_capacity) {
        uint32_t capacity = search->edge_capacity ? search->edge_capacity * 2u : 256u;
        uint32_t *edges = grow(search->machine, (grown_words){search->edges, search->edge_count, capacity});
        if (!edges) return TA_ERROR_NO_MEMORY;
        search->edges = edges;
        search->edge_capacity = capacity;
    }
    search->edges[search->edge_count++] = to;
    return TA_OK;
}

/* Every successor of configuration `index`, in document order of the
 * transitions' sources. `found` set when one holds `target`. */
typedef struct expansion {
    uint32_t index, target;
    uint32_t *next; /* scratch, words */
} expansion;

static ta_status expand(ta_analysis_search *search, expansion step, uint32_t *found) {
    ta_analysis_machine *machine = search->machine;
    if (search->record_edges) search->first_edge[step.index] = search->edge_count;
    if (ta_analysis_ends(machine, configuration_at(search, step.index))) return TA_OK;
    for (uint32_t s = 0; s < machine->state_count; s++) {
        if (!ta_analysis_bit(configuration_at(search, step.index), s)) continue;
        const ta_state_record *state = &machine->states[s];
        for (uint32_t t = state->first_transition; t < state->first_transition + state->transition_count; t++) {
            if (!ta_analysis_movable(machine, t)) continue;
            ta_analysis_apply(machine, (ta_analysis_move){configuration_at(search, step.index), t}, step.next);
            uint32_t index;
            ta_status status = find_or_add(search, (reached_from){step.next, step.index, t}, &index);
            if (status != TA_OK) return status;
            if (index == TA_NIL) continue;
            if (search->record_edges && (status = add_edge(search, index)) != TA_OK) return status;
            if (step.target != TA_NIL && ta_analysis_bit(configuration_at(search, index), step.target)) {
                *found = index;
                return TA_OK;
            }
        }
    }
    return TA_OK;
}

ta_status ta_analysis_search_run(ta_analysis_search *search, ta_analysis_goal goal, uint32_t *found) {
    *found = TA_NIL;
    uint32_t start;
    ta_status status = find_or_add(search, (reached_from){goal.start, TA_NIL, TA_NIL}, &start);
    if (status != TA_OK) return status;
    if (start == TA_NIL) return TA_OK; /* a limit of 0 */
    if (goal.target != TA_NIL && ta_analysis_bit(goal.start, goal.target)) {
        *found = start;
        return TA_OK;
    }
    uint32_t *next = ta_analysis_words(search->machine, search->machine->words);
    if (!next) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < search->count && *found == TA_NIL; i++) {
        status = expand(search, (expansion){i, goal.target, next}, found);
        if (status != TA_OK) return status;
    }
    if (search->record_edges) search->first_edge[search->count] = search->edge_count;
    return TA_OK;
}

/* ---- path ------------------------------------------------------------------------------------- */

static uint32_t limit_of(uint32_t limit) {
    return limit ? limit : TA_ANALYSIS_DEFAULT_LIMIT;
}

static int is_history(const ta_analysis_machine *machine, uint32_t state) {
    return machine->states[state].kind >= TA_STATE_HISTORY_SHALLOW;
}

/* The steps from the first configuration to `found`, in order. */
static ta_status steps_to(ta_analysis_search *search, uint32_t found, ta_analysis_path *out) {
    uint32_t count = 0;
    for (uint32_t i = found; i != TA_NIL; i = search->parents[i])
        count++;
    ta_analysis_step *steps = (ta_analysis_step *)ta_growable_arena_alloc(search->machine->arena, sizeof *steps * count, 8);
    if (!steps) return TA_ERROR_NO_MEMORY;
    uint32_t at = count;
    for (uint32_t i = found; i != TA_NIL; i = search->parents[i]) {
        at--;
        steps[at].transition = search->via[i];
        steps[at].configuration = configuration_at(search, i);
    }
    out->steps = steps;
    out->step_count = count;
    return TA_OK;
}

ta_status ta_analysis_path_find(const ta_analysis_path_request *request, ta_analysis_path *out) {
    ta_memset(out, 0, sizeof *out);
    if (!request->view || !request->arena || request->chart >= request->view->chart_count) return TA_ERROR_INVALID;
    const ta_chart_record *chart = &request->view->charts[request->chart];
    if (request->to >= chart->state_count || (request->from != TA_NIL && request->from >= chart->state_count))
        return TA_ERROR_INVALID;
    ta_analysis_machine machine;
    ta_status status = ta_analysis_machine_init(&machine, (ta_analysis_machine_setup){request->view, request->chart, request->arena});
    if (status != TA_OK) return status;
    if (is_history(&machine, request->to) || (request->from != TA_NIL && is_history(&machine, request->from)))
        return TA_ERROR_INVALID;
    const uint32_t *start = machine.initial;
    if (request->from != TA_NIL) {
        uint32_t *from = ta_analysis_words(&machine, machine.words);
        if (!from) return TA_ERROR_NO_MEMORY;
        status = ta_analysis_enter(&machine, (ta_analysis_entry){&request->from, 1, TA_NIL}, from);
        if (status != TA_OK) return status;
        start = from;
    }
    ta_analysis_search search;
    ta_memset(&search, 0, sizeof search);
    search.machine = &machine;
    search.limit = limit_of(request->limit);
    uint32_t found;
    status = ta_analysis_search_run(&search, (ta_analysis_goal){start, request->to}, &found);
    if (status != TA_OK) return status;
    out->explored = search.count;
    if (found != TA_NIL) {
        out->result = TA_ANALYSIS_PATH_FOUND;
        return steps_to(&search, found, out);
    }
    out->result = search.stopped ? TA_ANALYSIS_PATH_LIMIT : TA_ANALYSIS_PATH_UNREACHABLE;
    ta_analysis_step *first = (ta_analysis_step *)ta_growable_arena_alloc(request->arena, sizeof *first, 8);
    if (!first) return TA_ERROR_NO_MEMORY;
    first->transition = TA_NIL;
    first->configuration = start;
    out->steps = first;
    out->step_count = 1;
    return TA_OK;
}
