/* analysis/lint.c — the findings of ta_analysis_lint (analysis.h): each
 * rule over one chart's records, and the events between the charts of a
 * system. Freestanding. */
#include "analysis/machine.h"

#include "base/memory.h"

static const struct {
    ta_string_view name;
    uint32_t severity;
} rules[TA_ANALYSIS_RULE_COUNT] = {
    [TA_ANALYSIS_UNREACHABLE] = {{"unreachable", 11}, TA_ANALYSIS_WARNING},
    [TA_ANALYSIS_DEAD_END] = {{"dead-end", 8}, TA_ANALYSIS_WARNING},
    [TA_ANALYSIS_NEVER_ENDS] = {{"never-ends", 10}, TA_ANALYSIS_WARNING},
    [TA_ANALYSIS_SHADOWED] = {{"shadowed", 8}, TA_ANALYSIS_WARNING},
    [TA_ANALYSIS_EVENTLESS_LOOP] = {{"eventless-loop", 14}, TA_ANALYSIS_WARNING},
    [TA_ANALYSIS_UNHANDLED_EVENT] = {{"unhandled-event", 15}, TA_ANALYSIS_WARNING},
    [TA_ANALYSIS_UNSENT_EVENT] = {{"unsent-event", 12}, TA_ANALYSIS_NOTE},
    [TA_ANALYSIS_UNKNOWN_EVENTS] = {{"unknown-events", 14}, TA_ANALYSIS_NOTE},
    [TA_ANALYSIS_SEND_TARGET] = {{"send-target", 11}, TA_ANALYSIS_WARNING},
    [TA_ANALYSIS_SEARCH_LIMIT] = {{"search-limit", 12}, TA_ANALYSIS_NOTE},
};

ta_string_view ta_analysis_rule_name(uint32_t rule) {
    ta_string_view none = {"", 0};
    return rule < TA_ANALYSIS_RULE_COUNT ? rules[rule].name : none;
}

static int equal(ta_string_view a, ta_string_view b) {
    return a.length == b.length && (!a.length || ta_memcmp(a.pointer, b.pointer, a.length) == 0);
}

uint32_t ta_analysis_rule_named(ta_string_view name) {
    for (uint32_t rule = 0; rule < TA_ANALYSIS_RULE_COUNT; rule++)
        if (equal(rules[rule].name, name)) return rule;
    return TA_NIL;
}

uint32_t ta_analysis_rule_severity(uint32_t rule) {
    return rule < TA_ANALYSIS_RULE_COUNT ? rules[rule].severity : TA_ANALYSIS_NOTE;
}

int ta_analysis_descriptor_covers(ta_string_view handler, ta_string_view event) {
    ta_string_view any = {"*", 1};
    if (equal(handler, any) || equal(handler, event)) return 1;
    return event.length > handler.length && event.pointer[handler.length] == '.' &&
           equal((ta_string_view){event.pointer, handler.length}, handler);
}

/* ---- the findings ----------------------------------------------------------------------------- */

typedef struct linter {
    const ta_analysis_request *request;
    const ta_image_view *view;
    ta_analysis_finding *list;
    uint32_t count, capacity;
} linter;

/* What a finding is about; the rest of it is the rule's. */
typedef struct finding_at {
    uint32_t chart, state, transition;
} finding_at;

static ta_status add(linter *lint, const ta_analysis_finding *finding) {
    if (lint->count == lint->capacity) {
        uint32_t capacity = lint->capacity ? lint->capacity * 2u : 16u;
        ta_analysis_finding *list = (ta_analysis_finding *)ta_growable_arena_alloc(
            lint->request->arena, sizeof *list * capacity, 8);
        if (!list) return TA_ERROR_NO_MEMORY;
        if (lint->count) ta_memcpy(list, lint->list, sizeof *list * lint->count);
        lint->list = list;
        lint->capacity = capacity;
    }
    lint->list[lint->count] = *finding;
    lint->list[lint->count].severity = rules[finding->rule].severity;
    lint->count++;
    return TA_OK;
}

static ta_analysis_finding finding_of(uint32_t rule, finding_at at) {
    ta_analysis_finding finding = {rule, 0, at.chart, at.state, at.transition, TA_NIL, TA_NIL, TA_NIL, 0, 0};
    return finding;
}

/* ---- one chart ------------------------------------------------------------------------------- */

typedef struct chart_lint {
    linter *lint;
    uint32_t chart;
    ta_analysis_machine machine;
    uint32_t *reached; /* states in some configuration, and history states entered */
    uint32_t *dead;    /* dead ends reported */
    uint32_t *scratch;
} chart_lint;

static finding_at at_state(const chart_lint *lint, uint32_t state) {
    finding_at at = {lint->chart, state, TA_NIL};
    return at;
}

static const ta_expression_record *expression_of(const chart_lint *lint, uint32_t expression) {
    if (expression == TA_NIL || expression >= lint->machine.chart->expression_count) return 0;
    return &lint->lint->view->expressions[lint->machine.chart->first_expression + expression];
}

/* A transition's finding, at its condition's line when it has one. */
static ta_analysis_finding transition_finding(const chart_lint *lint, uint32_t rule, uint32_t transition) {
    const ta_transition_record *record = &lint->machine.transitions[transition];
    ta_analysis_finding finding = finding_of(rule, (finding_at){lint->chart, record->source, transition});
    const ta_expression_record *condition = expression_of(lint, record->condition);
    if (condition) {
        finding.line = condition->line;
        finding.column = condition->column;
    }
    return finding;
}

/* What an entry enters into `reached`, history states included. */
static ta_status reach(chart_lint *lint, ta_analysis_entry entry) {
    ta_analysis_machine *machine = &lint->machine;
    ta_memset(machine->histories, 0, sizeof(uint32_t) * machine->words);
    ta_status status = ta_analysis_enter(machine, entry, lint->scratch);
    for (uint32_t w = 0; w < machine->words; w++)
        lint->reached[w] |= lint->scratch[w] | machine->histories[w];
    return status;
}

/* The states some configuration holds: the root entered, then every
 * transition whose source is reached, until nothing changes. */
static ta_status reachable(chart_lint *lint) {
    ta_analysis_machine *machine = &lint->machine;
    uint32_t *taken = ta_analysis_words(machine, (machine->transition_count + 31u) / 32u);
    if (!taken) return TA_ERROR_NO_MEMORY;
    uint32_t root = 0;
    ta_status status = reach(lint, (ta_analysis_entry){&root, 1, TA_NIL});
    for (int changed = 1; changed && status == TA_OK;) {
        changed = 0;
        for (uint32_t t = 0; t < machine->transition_count && status == TA_OK; t++) {
            const ta_transition_record *record = &machine->transitions[t];
            if (ta_analysis_bit(taken, t) || !ta_analysis_movable(machine, t) ||
                !ta_analysis_bit(lint->reached, record->source))
                continue;
            ta_analysis_set(taken, t);
            changed = 1;
            status = reach(lint, (ta_analysis_entry){machine->view->u32 + record->targets_offset,
                                                      record->target_count, machine->domains[t]});
        }
    }
    return status;
}

static ta_status unreachable(chart_lint *lint) {
    const ta_state_record *states = lint->machine.states;
    for (uint32_t s = 1; s < lint->machine.state_count; s++) {
        if (ta_analysis_bit(lint->reached, s) || !ta_analysis_bit(lint->reached, states[s].parent)) continue;
        ta_analysis_finding finding = finding_of(TA_ANALYSIS_UNREACHABLE, at_state(lint, s));
        ta_status status = add(lint->lint, &finding);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

/* Whether `source` can be active while `state` is: an ancestor of it (or
 * itself), or in another region of a parallel ancestor. */
static int beside(const ta_analysis_machine *machine, uint32_t source, uint32_t state) {
    if (source == state || ta_analysis_below(machine, state, source)) return 1;
    const ta_state_record *states = machine->states;
    for (uint32_t child = state, parent = states[state].parent; parent != TA_NIL;
         child = parent, parent = states[parent].parent) {
        if (states[parent].kind != TA_STATE_PARALLEL || !ta_analysis_below(machine, source, parent)) continue;
        if (source < child || source >= states[child].end) return 1;
    }
    return 0;
}

/* Whether an actor in `state` still does something: a transition of it or
 * of an ancestor handles an event (even one that stays), or a transition
 * that can be active beside it exits it. */
static int way_out(const chart_lint *lint, uint32_t state) {
    const ta_analysis_machine *machine = &lint->machine;
    for (uint32_t t = 0; t < machine->transition_count; t++) {
        uint32_t source = machine->transitions[t].source;
        if (!(machine->transitions[t].flags & TA_TRANSITION_FLAG_INITIAL) &&
            (source == state || ta_analysis_below(machine, state, source)))
            return 1;
        if (!ta_analysis_movable(machine, t)) continue;
        if (!ta_analysis_bit(lint->reached, source) || !beside(machine, source, state)) continue;
        /* it exits the state, and does not enter it again */
        if (ta_analysis_below(machine, state, machine->domains[t]) &&
            !ta_analysis_bit(machine->entries + t * machine->words, state))
            return 1;
    }
    return 0;
}

static ta_status dead_ends(chart_lint *lint) {
    const ta_state_record *states = lint->machine.states;
    for (uint32_t s = 1; s < lint->machine.state_count; s++) {
        if (states[s].kind != TA_STATE_ATOMIC || !ta_analysis_bit(lint->reached, s) || way_out(lint, s)) continue;
        ta_analysis_set(lint->dead, s);
        ta_analysis_finding finding = finding_of(TA_ANALYSIS_DEAD_END, at_state(lint, s));
        ta_status status = add(lint->lint, &finding);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

/* ---- never-ends --------------------------------------------------------------------------- */

static int has_final(const ta_analysis_machine *machine) {
    for (uint32_t child = 1; child < machine->state_count; child = machine->states[child].end)
        if (machine->states[child].kind == TA_STATE_FINAL) return 1;
    return 0;
}

/* Which configurations can still end: those that end, and backwards
 * along the search's edges. One byte per configuration. */
static uint8_t *ending(ta_analysis_search *search) {
    ta_analysis_machine *machine = search->machine;
    uint32_t count = search->count;
    uint8_t *ends = (uint8_t *)ta_growable_arena_alloc(machine->arena, count + 1u, 1);
    uint32_t *first = ta_analysis_words(machine, count + 1u);
    uint32_t *sources = ta_analysis_words(machine, search->edge_count + 1u);
    uint32_t *queue = ta_analysis_words(machine, count + 1u);
    if (!ends || !first || !sources || !queue) return 0;
    /* the edges reversed: a configuration's predecessors */
    for (uint32_t e = 0; e < search->edge_count; e++)
        first[search->edges[e]]++;
    uint32_t total = 0;
    for (uint32_t c = 0; c <= count; c++) {
        uint32_t in = c < count ? first[c] : 0;
        first[c] = total;
        total += in;
    }
    uint32_t *fill = queue; /* the queue's room, used for filling first */
    for (uint32_t c = 0; c < count; c++)
        fill[c] = first[c];
    for (uint32_t c = 0; c < count; c++)
        for (uint32_t e = search->first_edge[c]; e < search->first_edge[c + 1]; e++)
            sources[fill[search->edges[e]]++] = c;
    uint32_t head = 0, tail = 0;
    for (uint32_t c = 0; c < count; c++) {
        ends[c] = (uint8_t)ta_analysis_ends(machine, search->configurations + c * machine->words);
        if (ends[c]) queue[tail++] = c;
    }
    while (head < tail) {
        uint32_t c = queue[head++];
        for (uint32_t i = first[c]; i < first[c + 1]; i++) {
            uint32_t predecessor = sources[i];
            if (ends[predecessor]) continue;
            ends[predecessor] = 1;
            queue[tail++] = predecessor;
        }
    }
    return ends;
}

static ta_status never_ends(chart_lint *lint) {
    ta_analysis_machine *machine = &lint->machine;
    if (!has_final(machine)) return TA_OK;
    ta_analysis_search search;
    ta_memset(&search, 0, sizeof search);
    search.machine = machine;
    search.limit = lint->lint->request->limit ? lint->lint->request->limit : TA_ANALYSIS_DEFAULT_LIMIT;
    search.record_edges = 1;
    uint32_t found;
    ta_status status = ta_analysis_search_run(&search, (ta_analysis_goal){machine->initial, TA_NIL}, &found);
    if (status != TA_OK) return status;
    if (search.stopped) {
        ta_analysis_finding finding = finding_of(TA_ANALYSIS_SEARCH_LIMIT, at_state(lint, TA_NIL));
        finding.other = search.limit;
        return add(lint->lint, &finding);
    }
    uint8_t *ends = ending(&search);
    uint32_t *seen = ta_analysis_words(machine, machine->words);
    uint32_t *good = ta_analysis_words(machine, machine->words);
    /* states of some configuration that cannot end and holds no dead end:
     * a state that only a dead end keeps from ending is that dead end's */
    uint32_t *stuck = ta_analysis_words(machine, machine->words);
    if (!ends || !seen || !good || !stuck) return TA_ERROR_NO_MEMORY;
    for (uint32_t c = 0; c < search.count; c++) {
        const uint32_t *configuration = search.configurations + c * machine->words;
        int dead = 0;
        for (uint32_t w = 0; w < machine->words; w++)
            dead |= (configuration[w] & lint->dead[w]) != 0;
        for (uint32_t w = 0; w < machine->words; w++) {
            seen[w] |= configuration[w];
            if (ends[c]) good[w] |= configuration[w];
            if (!ends[c] && !dead) stuck[w] |= configuration[w];
        }
    }
    for (uint32_t s = 0; s < machine->state_count; s++) {
        if (!ta_analysis_bit(seen, s) || ta_analysis_bit(good, s) || !ta_analysis_bit(stuck, s)) continue;
        if (s && !ta_analysis_bit(good, machine->states[s].parent)) continue; /* its parent is reported */
        ta_analysis_finding finding = finding_of(TA_ANALYSIS_NEVER_ENDS, at_state(lint, s));
        status = add(lint->lint, &finding);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

/* ---- shadowed ----------------------------------------------------------------------------- */

static ta_status shadowed(chart_lint *lint) {
    for (uint32_t s = 0; s < lint->machine.state_count; s++) {
        const ta_state_record *state = &lint->machine.states[s];
        for (uint32_t t = state->first_transition; t < state->first_transition + state->transition_count; t++) {
            uint32_t earlier = lint->machine.shadows[t];
            if (earlier == TA_NIL) continue;
            ta_analysis_finding finding = transition_finding(lint, TA_ANALYSIS_SHADOWED, t);
            finding.other = earlier;
            ta_status status = add(lint->lint, &finding);
            if (status != TA_OK) return status;
        }
    }
    return TA_OK;
}

/* ---- eventless loops ------------------------------------------------------------------------ */

static int leaf(const ta_analysis_machine *machine, uint32_t state) {
    const ta_state_record *record = &machine->states[state];
    return state && (record->kind == TA_STATE_ATOMIC || (record->kind == TA_STATE_FINAL && record->parent != 0));
}

/* The eventless transition an actor resting in `leaf` must take: the
 * first eventless one of the leaf, then of each ancestor, when it has no
 * condition. TA_NIL when that first one has a condition, or there is none. */
static uint32_t forced(const ta_analysis_machine *machine, uint32_t state) {
    for (uint32_t a = state; a != TA_NIL; a = machine->states[a].parent) {
        const ta_state_record *record = &machine->states[a];
        for (uint32_t t = record->first_transition; t < record->first_transition + record->transition_count; t++) {
            const ta_transition_record *transition = &machine->transitions[t];
            if (transition->event_count) continue;
            return transition->condition == TA_NIL ? t : TA_NIL;
        }
    }
    return TA_NIL;
}

/* Whether the forced transitions lead from `start` back to it, through
 * leaves with forced transitions only. `visited` and `queue` are scratch. */
typedef struct loop_search {
    const uint32_t *forced_of; /* per state */
    uint32_t *visited;         /* words */
    uint32_t *queue;           /* state_count + 1 */
} loop_search;

static int loops(const chart_lint *lint, const loop_search *search, uint32_t start) {
    const ta_analysis_machine *machine = &lint->machine;
    ta_memset(search->visited, 0, sizeof(uint32_t) * machine->words);
    uint32_t head = 0, tail = 0;
    search->queue[tail++] = start;
    while (head < tail) {
        uint32_t state = search->queue[head++];
        uint32_t t = search->forced_of[state];
        if (!machine->transitions[t].target_count) {
            if (state == start) return 1;
            continue;
        }
        const uint32_t *entered = machine->entries + t * machine->words;
        for (uint32_t next = 0; next < machine->state_count; next++) {
            if (!ta_analysis_bit(entered, next) || !leaf(machine, next) || search->forced_of[next] == TA_NIL) continue;
            if (next == start) return 1;
            if (ta_analysis_bit(search->visited, next)) continue;
            ta_analysis_set(search->visited, next);
            search->queue[tail++] = next;
        }
    }
    return 0;
}

static ta_status eventless_loops(chart_lint *lint) {
    ta_analysis_machine *machine = &lint->machine;
    uint32_t *forced_of = ta_analysis_words(machine, machine->state_count);
    uint32_t *reported = ta_analysis_words(machine, (machine->transition_count + 31u) / 32u);
    loop_search search = {forced_of, ta_analysis_words(machine, machine->words),
                          ta_analysis_words(machine, machine->state_count + 1u)};
    if (!forced_of || !reported || !search.visited || !search.queue) return TA_ERROR_NO_MEMORY;
    for (uint32_t s = 0; s < machine->state_count; s++)
        forced_of[s] = leaf(machine, s) && ta_analysis_bit(lint->reached, s) ? forced(machine, s) : TA_NIL;
    for (uint32_t s = 0; s < machine->state_count; s++) {
        uint32_t t = forced_of[s];
        if (t == TA_NIL || ta_analysis_bit(reported, t) || !loops(lint, &search, s)) continue;
        ta_analysis_set(reported, t);
        ta_analysis_finding finding = transition_finding(lint, TA_ANALYSIS_EVENTLESS_LOOP, t);
        ta_status status = add(lint->lint, &finding);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

/* ---- sends and raises ---------------------------------------------------------------------- */

/* A raise or a send of a chart, and where its code is. */
typedef struct occurrence {
    uint32_t send;  /* chart-local send, or TA_NIL for a raise */
    uint32_t event; /* a raise's event node */
    uint32_t state, transition;
} occurrence;

typedef struct occurrences {
    occurrence *list;
    uint32_t count;
} occurrences;

typedef struct code_owners {
    uint32_t *states, *transitions; /* per word of the chart's code */
    const ta_chart_record *chart;
} code_owners;

static void own(code_owners *owners, uint32_t pc, finding_at at) {
    const ta_chart_record *chart = owners->chart;
    if (pc == TA_NIL || pc < chart->first_code || pc - chart->first_code >= chart->code_count) return;
    owners->states[pc - chart->first_code] = at.state;
    owners->transitions[pc - chart->first_code] = at.transition;
}

/* Every raise and send of chart `chart`, each with the state (and the
 * transition) whose code holds it: the entry point starting at or before
 * it (code is laid out entry point by entry point). */
static ta_status occurrences_of(linter *lint, uint32_t chart_index, occurrences *out) {
    const ta_image_view *view = lint->view;
    const ta_chart_record *chart = &view->charts[chart_index];
    ta_growable_arena *arena = lint->request->arena;
    uint32_t size = chart->code_count + 1u;
    code_owners owners = {(uint32_t *)ta_growable_arena_alloc(arena, sizeof(uint32_t) * size, 4),
                          (uint32_t *)ta_growable_arena_alloc(arena, sizeof(uint32_t) * size, 4), chart};
    out->list = (occurrence *)ta_growable_arena_alloc(arena, sizeof(occurrence) * size, 4);
    out->count = 0;
    if (!owners.states || !owners.transitions || !out->list) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < size; i++)
        owners.states[i] = owners.transitions[i] = TA_NIL;
    const ta_state_record *states = ta_chart_states(view, chart);
    const ta_transition_record *transitions = ta_chart_transitions(view, chart);
    own(&owners, chart->script, (finding_at){chart_index, 0, TA_NIL});
    for (uint32_t s = 0; s < chart->state_count; s++) {
        own(&owners, states[s].onentry, (finding_at){chart_index, s, TA_NIL});
        own(&owners, states[s].onexit, (finding_at){chart_index, s, TA_NIL});
    }
    for (uint32_t t = 0; t < chart->transition_count; t++)
        own(&owners, transitions[t].action, (finding_at){chart_index, transitions[t].source, t});
    for (uint32_t i = 0; i < chart->invoke_count; i++) {
        const ta_invoke_record *invoke = &view->invokes[chart->first_invoke + i];
        own(&owners, invoke->finalize, (finding_at){chart_index, invoke->state, TA_NIL});
    }
    uint32_t state = TA_NIL, transition = TA_NIL;
    for (uint32_t pc = chart->first_code; pc < chart->first_code + chart->code_count;) {
        uint32_t at = pc - chart->first_code, opcode = view->code[pc];
        if (owners.states[at] != TA_NIL) {
            state = owners.states[at];
            transition = owners.transitions[at];
        }
        if (opcode >= TA_OPCODE_MAX_) break; /* a verified image holds none */
        if (opcode == TA_OPCODE_RAISE || opcode == TA_OPCODE_SEND) {
            occurrence *item = &out->list[out->count++];
            item->send = opcode == TA_OPCODE_SEND ? view->code[pc + 1] : TA_NIL;
            item->event = opcode == TA_OPCODE_RAISE ? view->code[pc + 1] : TA_NIL;
            item->state = state;
            item->transition = transition;
        }
        pc += ta_opcode_length[opcode];
    }
    return TA_OK;
}

static const ta_send_record *send_of(const ta_image_view *view, uint32_t chart, uint32_t send) {
    return &view->sends[view->charts[chart].first_send + send];
}

/* A send to the SCXML processor (no type): the others leave for the
 * application's I/O processors. */
static int scxml_send(const ta_send_record *send) {
    return send->type == TA_NIL && send->typeexpr == TA_NIL;
}

/* Whether chart `chart` has a transition whose descriptor matches the
 * event node `event`. */
static int handles(const ta_image_view *view, uint32_t chart, uint32_t event) {
    const ta_chart_record *record = &view->charts[chart];
    const ta_transition_record *transitions = ta_chart_transitions(view, record);
    for (uint32_t t = 0; t < record->transition_count; t++) {
        const uint32_t *pairs = view->u32 + transitions[t].events_offset;
        for (uint32_t i = 0; i < transitions[t].event_count; i++)
            if (pairs[2 * i] <= event && event < pairs[2 * i + 1]) return 1;
    }
    return 0;
}

static int invokes_chart(const ta_image_view *view, uint32_t parent, uint32_t child) {
    const ta_chart_record *record = &view->charts[parent];
    for (uint32_t i = 0; i < record->invoke_count; i++)
        if (view->invokes[record->first_invoke + i].source_chart == child) return 1;
    return 0;
}

/* Where a send of chart `from` can arrive. */
typedef enum arrival {
    ARRIVES_KNOWN = 0,  /* at the charts `receives` says */
    ARRIVES_UNKNOWN = 1 /* anywhere: a target given at run time, an address, a parent not in the image */
} arrival;

typedef struct send_route {
    uint32_t from;
    const ta_send_record *send;
} send_route;

static uint32_t arrival_of(const ta_image_view *view, send_route route) {
    const ta_send_record *send = route.send;
    switch (send->target_kind) {
    case TA_SEND_TARGET_NONE:
    case TA_SEND_TARGET_INTERNAL: return ARRIVES_KNOWN;
    case TA_SEND_TARGET_PARENT:
        for (uint32_t c = 0; c < view->chart_count; c++)
            if (invokes_chart(view, c, route.from)) return ARRIVES_KNOWN;
        return ARRIVES_UNKNOWN;
    case TA_SEND_TARGET_INVOKE:
        return view->invokes[view->charts[route.from].first_invoke + send->target].source_chart == TA_NIL
                   ? ARRIVES_UNKNOWN
                   : ARRIVES_KNOWN;
    default: return ARRIVES_UNKNOWN;
    }
}

/* Whether a send whose arrival is known reaches chart `chart`. */
typedef struct send_receiver {
    send_route route;
    uint32_t chart;
} send_receiver;

static int receives(const ta_image_view *view, send_receiver receiver) {
    const ta_send_record *send = receiver.route.send;
    uint32_t from = receiver.route.from;
    switch (send->target_kind) {
    case TA_SEND_TARGET_NONE:
    case TA_SEND_TARGET_INTERNAL: return receiver.chart == from;
    case TA_SEND_TARGET_PARENT: return invokes_chart(view, receiver.chart, from);
    case TA_SEND_TARGET_INVOKE:
        return view->invokes[view->charts[from].first_invoke + send->target].source_chart == receiver.chart;
    default: return 0;
    }
}

/* A send's finding, at the line of its first expression when it has one. */
static ta_analysis_finding send_finding(const chart_lint *lint, uint32_t rule, const occurrence *item) {
    ta_analysis_finding finding = finding_of(rule, (finding_at){lint->chart, item->state, item->transition});
    finding.send = item->send;
    if (item->send == TA_NIL) return finding;
    const ta_send_record *send = send_of(lint->lint->view, lint->chart, item->send);
    const uint32_t expressions[] = {send->eventexpr, send->targetexpr, send->delayexpr, send->idlocation};
    for (uint32_t i = 0; i < sizeof expressions / sizeof expressions[0]; i++) {
        const ta_expression_record *expression = expression_of(lint, expressions[i]);
        if (!expression) continue;
        finding.line = expression->line;
        finding.column = expression->column;
        break;
    }
    return finding;
}

static ta_status unhandled_events(chart_lint *lint, const occurrences *found) {
    const ta_image_view *view = lint->lint->view;
    for (uint32_t i = 0; i < found->count; i++) {
        const occurrence *item = &found->list[i];
        uint32_t event = item->event;
        int handled = 0;
        if (item->send == TA_NIL) {
            handled = handles(view, lint->chart, event);
        } else {
            const ta_send_record *send = send_of(view, lint->chart, item->send);
            send_route route = {lint->chart, send};
            event = send->event;
            if (event == TA_NIL || !scxml_send(send) || arrival_of(view, route) != ARRIVES_KNOWN) continue;
            for (uint32_t c = 0; c < view->chart_count && !handled; c++)
                handled = receives(view, (send_receiver){route, c}) && handles(view, c, event);
        }
        if (handled) continue;
        ta_analysis_finding finding = send_finding(lint, TA_ANALYSIS_UNHANDLED_EVENT, item);
        finding.event = event;
        ta_status status = add(lint->lint, &finding);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

/* Whether a static target can never be reached: "#_x" with no
 * invocation x (the compiler resolves those it knows), or text that is
 * neither "#_..." nor an absolute URI (a scheme, then ':'). */
static int unreachable_target(ta_string_view target) {
    if (target.length >= 2 && target.pointer[0] == '#' && target.pointer[1] == '_') {
        ta_string_view session = {"#_scxml_", 8};
        return !(target.length >= session.length &&
                 equal((ta_string_view){target.pointer, session.length}, session));
    }
    for (uint32_t i = 0; i < target.length; i++) {
        char letter = target.pointer[i];
        int alpha = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z');
        if (letter == ':') return i == 0;
        if (!alpha && (!i || !((letter >= '0' && letter <= '9') || letter == '+' || letter == '-' || letter == '.')))
            return 1;
    }
    return 1;
}

static ta_status send_targets(chart_lint *lint, const occurrences *found) {
    const ta_image_view *view = lint->lint->view;
    for (uint32_t i = 0; i < found->count; i++) {
        const occurrence *item = &found->list[i];
        if (item->send == TA_NIL) continue;
        const ta_send_record *send = send_of(view, lint->chart, item->send);
        if (send->target_kind != TA_SEND_TARGET_STRING || !scxml_send(send) ||
            !unreachable_target(ta_image_view_string(view, send->target)))
            continue;
        ta_analysis_finding finding = send_finding(lint, TA_ANALYSIS_SEND_TARGET, item);
        ta_status status = add(lint->lint, &finding);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

/* ---- events between the charts of a system ------------------------------------------------- */

/* done.* and error.* are the platform's. */
static int platform_event(ta_string_view name) {
    static const ta_string_view prefixes[] = {{"done", 4}, {"error", 5}};
    for (uint32_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        ta_string_view prefix = prefixes[i];
        if (name.length >= prefix.length && equal((ta_string_view){name.pointer, prefix.length}, prefix) &&
            (name.length == prefix.length || name.pointer[prefix.length] == '.'))
            return 1;
    }
    return 0;
}

/* What can arrive at chart `chart`: one byte per event node; `unknown`
 * the first chart whose computed event (eventexpr) may arrive there. */
typedef struct arrivals {
    uint8_t *events;
    uint32_t unknown;
} arrivals;

static ta_status arrivals_at(chart_lint *lint, arrivals *out) {
    linter *lint_all = lint->lint;
    const ta_image_view *view = lint_all->view;
    out->events = (uint8_t *)ta_growable_arena_alloc(lint_all->request->arena, view->event_node_count + 1u, 1);
    out->unknown = TA_NIL;
    if (!out->events) return TA_ERROR_NO_MEMORY;
    ta_memset(out->events, 0, view->event_node_count + 1u);
    const ta_chart_record *chart = &view->charts[lint->chart];
    for (uint32_t s = 0; s < chart->state_count; s++) {
        uint32_t done = view->states[chart->first_state + s].done_event;
        if (done < view->event_node_count) out->events[done] = 1;
    }
    for (uint32_t c = 0; c < view->chart_count; c++) {
        occurrences found;
        ta_status status = occurrences_of(lint_all, c, &found);
        if (status != TA_OK) return status;
        for (uint32_t i = 0; i < found.count; i++) {
            const occurrence *item = &found.list[i];
            if (item->send == TA_NIL) {
                if (c == lint->chart && item->event < view->event_node_count) out->events[item->event] = 1;
                continue;
            }
            const ta_send_record *send = send_of(view, c, item->send);
            send_route route = {c, send};
            if (!scxml_send(send)) continue;
            if (arrival_of(view, route) == ARRIVES_KNOWN && !receives(view, (send_receiver){route, lint->chart}))
                continue;
            if (send->event == TA_NIL) {
                if (out->unknown == TA_NIL) out->unknown = c;
            } else if (send->event < view->event_node_count) {
                out->events[send->event] = 1;
            }
        }
    }
    return TA_OK;
}

static ta_status unsent_events(chart_lint *lint) {
    const ta_image_view *view = lint->lint->view;
    arrivals arrived;
    ta_status status = arrivals_at(lint, &arrived);
    if (status != TA_OK) return status;
    if (arrived.unknown != TA_NIL) {
        ta_analysis_finding finding = finding_of(TA_ANALYSIS_UNKNOWN_EVENTS, at_state(lint, TA_NIL));
        finding.other = arrived.unknown;
        return add(lint->lint, &finding);
    }
    uint8_t *reported = (uint8_t *)ta_growable_arena_alloc(lint->lint->request->arena, view->event_node_count + 1u, 1);
    if (!reported) return TA_ERROR_NO_MEMORY;
    ta_memset(reported, 0, view->event_node_count + 1u);
    const ta_analysis_machine *machine = &lint->machine;
    for (uint32_t s = 0; s < machine->state_count; s++) {
        const ta_state_record *state = &machine->states[s];
        for (uint32_t t = state->first_transition; t < state->first_transition + state->transition_count; t++) {
            const ta_transition_record *transition = &machine->transitions[t];
            const uint32_t *pairs = view->u32 + transition->events_offset;
            for (uint32_t i = 0; i < transition->event_count; i++) {
                uint32_t low = pairs[2 * i], high = pairs[2 * i + 1];
                if (low >= view->event_node_count || reported[low]) continue;
                ta_string_view name = ta_image_view_string(view, view->event_nodes[low].name);
                if (!name.length || platform_event(name)) continue; /* "*", done.*, error.* */
                int sent = 0;
                for (uint32_t e = low; e < high && e < view->event_node_count && !sent; e++)
                    sent = arrived.events[e];
                if (sent) continue;
                reported[low] = 1;
                ta_analysis_finding finding = transition_finding(lint, TA_ANALYSIS_UNSENT_EVENT, t);
                finding.event = low;
                status = add(lint->lint, &finding);
                if (status != TA_OK) return status;
            }
        }
    }
    return TA_OK;
}

/* ---- the request ----------------------------------------------------------------------------- */

static ta_status lint_chart(linter *lint, uint32_t chart) {
    chart_lint one;
    ta_memset(&one, 0, sizeof one);
    one.lint = lint;
    one.chart = chart;
    ta_status status = ta_analysis_machine_init(&one.machine, (ta_analysis_machine_setup){lint->view, chart, lint->request->arena});
    if (status != TA_OK) return status;
    one.reached = ta_analysis_words(&one.machine, one.machine.words);
    one.dead = ta_analysis_words(&one.machine, one.machine.words);
    one.scratch = ta_analysis_words(&one.machine, one.machine.words);
    if (!one.reached || !one.dead || !one.scratch) return TA_ERROR_NO_MEMORY;
    occurrences found;
    if ((status = reachable(&one)) != TA_OK || (status = unreachable(&one)) != TA_OK ||
        (status = dead_ends(&one)) != TA_OK || (status = never_ends(&one)) != TA_OK ||
        (status = shadowed(&one)) != TA_OK || (status = eventless_loops(&one)) != TA_OK ||
        (status = occurrences_of(lint, chart, &found)) != TA_OK || (status = unhandled_events(&one, &found)) != TA_OK)
        return status;
    if (lint->request->flags & TA_ANALYSIS_SYSTEM) {
        status = unsent_events(&one);
        if (status != TA_OK) return status;
    }
    return send_targets(&one, &found);
}

ta_status ta_analysis_lint(const ta_analysis_request *request, ta_analysis_findings *out) {
    out->list = 0;
    out->count = 0;
    if (!request || !request->view || !request->arena) return TA_ERROR_INVALID;
    linter lint = {request, request->view, 0, 0, 0};
    for (uint32_t c = 0; c < request->view->chart_count; c++) {
        if (request->charts && !request->charts[c]) continue;
        ta_status status = lint_chart(&lint, c);
        if (status != TA_OK) return status;
    }
    out->list = lint.list;
    out->count = lint.count;
    return TA_OK;
}
