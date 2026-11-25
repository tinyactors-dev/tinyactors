/* graph.c — tinyactors graph: each selected chart drawn (diagram.h). With
 * -f svg the answer is the picture: the charts one above the other, each
 * under its name. In the other formats it is the drawing as data: each
 * chart's boxes (its states, where they are drawn), its edges (one per
 * target of each routed transition) and the transitions a box lists
 * inside it. Its options choose the direction, the theme, states and a
 * path to mark, and containers to fold into one box; every format answers
 * the same layout. Freestanding. */
#include "tool.h"

#include "analysis/analysis.h"
#include "diagram/svg.h"

static tool_open_request arguments_of(const tool *t) {
    tool_open_request request = {t->parsed->argument_count, t->parsed->arguments, 0};
    return request;
}

static ta_string_view string_of(const ta_image_view *view, uint32_t id) {
    return ta_image_view_string(view, id);
}

static ta_string_view expression_text(const ta_image_view *view, const ta_chart_record *chart, uint32_t expression) {
    if (expression == TA_NIL || expression >= chart->expression_count) return ta_string("");
    return string_of(view, view->expressions[chart->first_expression + expression].source);
}

/* A transition's event descriptors, separated by spaces ("*" for the one
 * that matches every event). */
static ta_string_view transition_events(tool *t, const ta_image_view *view, const ta_transition_record *transition) {
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t i = 0; i < transition->event_count; i++) {
        uint32_t node = view->u32[transition->events_offset + 2 * i];
        ta_string_view name = node < view->event_node_count ? string_of(view, view->event_nodes[node].name)
                                                            : ta_string("");
        if (i) tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, name.length ? name : ta_string("*"));
    }
    return tool_text_view(&text);
}

/* ---- what the command line asks ------------------------------------------------------------- */

typedef struct graph_options {
    uint32_t direction; /* ta_diagram_direction */
    uint32_t theme;     /* ta_diagram_theme */
    int depth_given;
    uint64_t depth;
    int folding;  /* --depth or --fold: the data formats say what each box hides */
    int marking;  /* --highlight or --path: the data formats say what is marked */
    int path;     /* --path */
} graph_options;

static void usage(tool *t, ta_string_view message) {
    tool_complain(t, message);
    t->exit = TOOL_EXIT_USAGE;
}

/* The options, read and checked against each other; 0 told when they are wrong (exit 2). */
static int options_of(tool *t, graph_options *out) {
    const ta_cli_value *direction = tool_last(t, TOOL_GRAPH_DIRECTION), *theme = tool_last(t, TOOL_GRAPH_THEME);
    out->direction = direction && direction->choice == TOOL_GRAPH_RIGHT ? TA_DIAGRAM_RIGHT : TA_DIAGRAM_DOWN;
    uint32_t chosen = theme ? theme->choice : TOOL_GRAPH_THEME_AUTO;
    out->theme = chosen == TOOL_GRAPH_THEME_LIGHT  ? TA_DIAGRAM_THEME_LIGHT
                 : chosen == TOOL_GRAPH_THEME_DARK ? TA_DIAGRAM_THEME_DARK
                                                   : TA_DIAGRAM_THEME_AUTO;
    const ta_cli_value *depth = tool_last(t, TOOL_GRAPH_DEPTH);
    out->depth_given = depth != 0;
    out->depth = 0;
    if (depth && tool_decimal(depth->text, &out->depth) != TA_OK) {
        usage(t, ta_string("--depth takes a number of levels: 0, 1, 2, ..."));
        return 0;
    }
    out->folding = out->depth_given || tool_count(t, TOOL_GRAPH_FOLD);
    out->path = tool_count(t, TOOL_GRAPH_PATH) > 0;
    out->marking = out->path || tool_count(t, TOOL_GRAPH_HIGHLIGHT);
    if (out->path && !tool_count(t, TOOL_GRAPH_TO)) {
        usage(t, ta_string("--path needs --to STATE: where the path goes"));
        return 0;
    }
    if (!out->path && (tool_count(t, TOOL_GRAPH_TO) || tool_count(t, TOOL_GRAPH_FROM))) {
        usage(t, ta_string("--to and --from say where the path goes and starts: give --path with them"));
        return 0;
    }
    return 1;
}

/* ---- the chart as the diagram reads it ------------------------------------------------------ */

static uint32_t diagram_kind(const ta_state_record *state, uint32_t index) {
    if (index == 0) return TA_DIAGRAM_STATE_ROOT;
    switch (state->kind) {
    case TA_STATE_PARALLEL: return TA_DIAGRAM_STATE_PARALLEL;
    case TA_STATE_FINAL: return TA_DIAGRAM_STATE_FINAL;
    case TA_STATE_HISTORY_SHALLOW: return TA_DIAGRAM_STATE_HISTORY_SHALLOW;
    case TA_STATE_HISTORY_DEEP: return TA_DIAGRAM_STATE_HISTORY_DEEP;
    default: return TA_DIAGRAM_STATE_STATE;
    }
}

/* A chart, drawn: what the diagram reads, its folding, and what it made. The chart's states are
 * the image's chart-local states, in order; `folding.chart` is what is drawn. */
typedef struct drawn {
    ta_diagram_chart chart;
    uint32_t *transition_of; /* per image transition: the chart's, TA_NIL for an initial one */
    ta_diagram_folding folding;
    uint8_t *portals;
    ta_diagram_layout layout;
    uint8_t *marks;  /* per drawn state, ta_diagram_mark; NULL when nothing is marked */
    uint32_t *path;  /* drawn transitions, in the order the path takes them */
    uint32_t path_length;
} drawn;

/* The image's chart as a diagram's: its states in document order, each
 * compound state's initial transition as its initial states, every other
 * transition (a history state's default among them) as a transition. */
static ta_status read_chart(tool *t, const ta_image_view *view, const ta_chart_record *chart, drawn *out) {
    uint32_t n = chart->state_count, transitions = chart->transition_count, targets = 0;
    for (uint32_t i = 0; i < transitions; i++)
        targets += view->transitions[chart->first_transition + i].target_count;
    ta_diagram_state *states =
        (ta_diagram_state *)ta_growable_arena_alloc(t->arena, sizeof(ta_diagram_state) * (n + 1u), 8);
    ta_diagram_transition *list = (ta_diagram_transition *)ta_growable_arena_alloc(
        t->arena, sizeof(ta_diagram_transition) * (transitions + 1u), 8);
    uint32_t *target_list = (uint32_t *)ta_growable_arena_alloc(t->arena, sizeof(uint32_t) * (targets + 1u), 8);
    out->transition_of = (uint32_t *)ta_growable_arena_alloc(t->arena, sizeof(uint32_t) * (transitions + 1u), 8);
    if (!states || !list || !target_list || !out->transition_of) return TA_ERROR_NO_MEMORY;
    uint32_t count = 0, target_count = 0;
    for (uint32_t s = 0; s < n; s++) {
        const ta_state_record *state = &view->states[chart->first_state + s];
        states[s] = (ta_diagram_state){state->parent, diagram_kind(state, s), string_of(view, state->id), 0, 0, 0};
    }
    for (uint32_t i = 0; i < transitions; i++) {
        const ta_transition_record *transition = &view->transitions[chart->first_transition + i];
        uint32_t first = target_count;
        for (uint32_t j = 0; j < transition->target_count; j++)
            target_list[target_count++] = view->u32[transition->targets_offset + j];
        const ta_state_record *source = &view->states[chart->first_state + transition->source];
        out->transition_of[i] = TA_NIL;
        if ((transition->flags & TA_TRANSITION_FLAG_INITIAL) && source->kind < TA_STATE_HISTORY_SHALLOW) {
            states[transition->source].first_initial = first;
            states[transition->source].initial_count = transition->target_count;
            continue;
        }
        out->transition_of[i] = count;
        list[count++] = (ta_diagram_transition){transition->source, first, transition->target_count,
                                                transition_events(t, view, transition),
                                                expression_text(view, chart, transition->condition)};
    }
    out->chart = (ta_diagram_chart){states, n, list, count, target_list};
    return TA_OK;
}

/* The chart's state named `name` ("/scxml": the root), or TA_NIL. */
static uint32_t state_named(const ta_image_view *view, const ta_chart_record *chart, ta_string_view name) {
    for (uint32_t s = 0; s < chart->state_count; s++)
        if (tool_equal(tool_state_id(view, chart, s), name)) return s;
    return TA_NIL;
}

static int is_container(const ta_diagram_chart *chart, uint32_t s) {
    for (uint32_t c = s + 1; c < chart->state_count; c++) {
        if (chart->states[c].parent != s) continue;
        uint32_t kind = chart->states[c].kind;
        if (kind != TA_DIAGRAM_STATE_HISTORY_SHALLOW && kind != TA_DIAGRAM_STATE_HISTORY_DEEP) return 1;
    }
    return 0;
}

/* Which containers fold: those nested deeper than --depth, and those --fold names. */
static uint8_t *folded_states(tool *t, const graph_options *options, const ta_image_view *view,
                              const ta_chart_record *record, const ta_diagram_chart *chart) {
    uint32_t n = chart->state_count;
    uint8_t *folded = (uint8_t *)ta_growable_arena_alloc(t->arena, n + 1u, 8);
    uint32_t *depth = (uint32_t *)ta_growable_arena_alloc(t->arena, sizeof(uint32_t) * (n + 1u), 8);
    if (!folded || !depth) return 0;
    for (uint32_t s = 0; s < n; s++) {
        uint32_t parent = chart->states[s].parent;
        depth[s] = parent == TA_NIL ? 0 : depth[parent] + 1;
        folded[s] = options->depth_given && depth[s] > options->depth;
    }
    for (uint32_t i = 0; i < t->parsed->value_count; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        if (value->option.scope != TA_CLI_SCOPE_COMMAND || value->option.option != TOOL_GRAPH_FOLD) continue;
        uint32_t s = state_named(view, record, value->text);
        if (s != TA_NIL) folded[s] = 1;
    }
    return folded;
}

/* Every state an option names (--highlight, --fold, --to, --from) is a state of some chart drawn;
 * a folded one is a container, a path's end not a history state. 0 told when one is not (exit 2). */
typedef struct named_check {
    const ta_image_view *view;
    const tool_opened *opened;
    const drawn *drawings;
} named_check;

static int names_known(tool *t, named_check c) {
    static const uint32_t options[] = {TOOL_GRAPH_HIGHLIGHT, TOOL_GRAPH_FOLD, TOOL_GRAPH_TO, TOOL_GRAPH_FROM};
    for (uint32_t i = 0; i < t->parsed->value_count; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        uint32_t which = TA_NIL;
        for (uint32_t k = 0; k < sizeof options / sizeof options[0]; k++)
            if (value->option.scope == TA_CLI_SCOPE_COMMAND && value->option.option == options[k]) which = options[k];
        if (which == TA_NIL) continue;
        int found = 0, foldable = 0, endpoint = 0;
        for (uint32_t ci = 0; ci < c.opened->charts.count; ci++) {
            if (!c.opened->charts.selected[ci]) continue;
            const ta_diagram_chart *chart = &c.drawings[ci].chart;
            uint32_t s = state_named(c.view, &c.view->charts[ci], value->text);
            if (s == TA_NIL) continue;
            found = 1;
            foldable |= s != 0 && is_container(chart, s);
            uint32_t kind = chart->states[s].kind;
            endpoint |= kind != TA_DIAGRAM_STATE_HISTORY_SHALLOW && kind != TA_DIAGRAM_STATE_HISTORY_DEEP;
        }
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, ta_string("'"));
        tool_text_append(&text, value->text);
        if (!found)
            tool_text_append(&text, ta_string("' is no state of a chart drawn"));
        else if (which == TOOL_GRAPH_FOLD && !foldable)
            tool_text_append(&text, ta_string("' is not a compound or parallel state: only those fold"));
        else if ((which == TOOL_GRAPH_TO || which == TOOL_GRAPH_FROM) && !endpoint)
            tool_text_append(&text, ta_string("' is a history state, never active itself: name a state, a parallel or a final"));
        else
            continue;
        usage(t, tool_text_view(&text));
        return 0;
    }
    return 1;
}

/* Lays one chart out: folded as asked, in the direction asked. */
static ta_status lay_out(tool *t, const graph_options *options, const uint8_t *folded, drawn *out) {
    ta_diagram_fold_request fold = {&out->chart, folded, t->arena};
    ta_status status = ta_diagram_fold(&fold, &out->folding);
    if (status != TA_OK) return status;
    const ta_diagram_chart *shown = &out->folding.chart;
    out->portals = ta_diagram_portals(shown, t->arena);
    if (!out->portals) return TA_ERROR_NO_MEMORY;
    ta_diagram_measures measures;
    ta_diagram_measure_request measure = {shown, out->portals, t->arena};
    if (ta_diagram_measure(&measure, &measures) != TA_OK) return TA_ERROR_NO_MEMORY;
    ta_diagram_request request = {shown, options->direction, 0, measures.states, measures.labels, out->portals, t->arena};
    return ta_diagram_layout_chart(&request, &out->layout);
}

static void mark(drawn *d, uint32_t state, uint32_t how) {
    if (state != TA_NIL && state < d->chart.state_count) d->marks[d->folding.drawn_state[state]] |= (uint8_t)how;
}

/* The path the path command finds, as drawn transitions and marked states: TA_OK and
 * `*found` 1 when there is one; 0 when the chart lacks --to or --from, or none reaches it. */
typedef struct path_place {
    const ta_image_view *view;
    uint32_t chart; /* the image's chart index */
    uint32_t *found;
} path_place;

static ta_status find_path(tool *t, path_place where, drawn *d) {
    const ta_chart_record *record = &where.view->charts[where.chart];
    *where.found = 0;
    const ta_cli_value *to_value = tool_last(t, TOOL_GRAPH_TO), *from_value = tool_last(t, TOOL_GRAPH_FROM);
    uint32_t to = state_named(where.view, record, to_value->text);
    uint32_t from = from_value ? state_named(where.view, record, from_value->text) : TA_NIL;
    if (to == TA_NIL || (from_value && from == TA_NIL)) return TA_OK;
    ta_analysis_path_request request = {where.view, where.chart, from, to, 0, t->arena};
    ta_analysis_path path;
    ta_status status = ta_analysis_path_find(&request, &path);
    if (status == TA_ERROR_INVALID) return TA_OK; /* a history state: told before */
    if (status != TA_OK) return status;
    if (path.result != TA_ANALYSIS_PATH_FOUND) return TA_OK;
    *where.found = 1;
    d->path = (uint32_t *)ta_growable_arena_alloc(t->arena, sizeof(uint32_t) * path.step_count, 8);
    if (!d->path) return TA_ERROR_NO_MEMORY;
    mark(d, from, TA_DIAGRAM_MARK_PATH);
    mark(d, to, TA_DIAGRAM_MARK_PATH);
    for (uint32_t i = 1; i < path.step_count; i++) {
        uint32_t transition = d->transition_of[path.steps[i].transition];
        uint32_t shown = transition == TA_NIL ? TA_NIL : d->folding.drawn_transition[transition];
        d->path[d->path_length++] = shown;
        if (transition == TA_NIL) continue;
        const ta_diagram_transition *taken = &d->chart.transitions[transition];
        mark(d, taken->source, TA_DIAGRAM_MARK_PATH);
        for (uint32_t j = 0; j < taken->target_count; j++)
            mark(d, d->chart.targets[taken->first_target + j], TA_DIAGRAM_MARK_PATH);
    }
    return TA_OK;
}

/* The states --highlight names, and the path, marked. */
static ta_status marks_of(tool *t, path_place where, drawn *d, const graph_options *options) {
    d->marks = (uint8_t *)ta_growable_arena_alloc(t->arena, d->folding.chart.state_count + 1u, 8);
    if (!d->marks) return TA_ERROR_NO_MEMORY;
    for (uint32_t s = 0; s <= d->folding.chart.state_count; s++)
        d->marks[s] = 0;
    const ta_chart_record *record = &where.view->charts[where.chart];
    for (uint32_t i = 0; i < t->parsed->value_count; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        if (value->option.scope != TA_CLI_SCOPE_COMMAND || value->option.option != TOOL_GRAPH_HIGHLIGHT) continue;
        mark(d, state_named(where.view, record, value->text), TA_DIAGRAM_MARK_HIGHLIGHT);
    }
    return options->path ? find_path(t, where, d) : TA_OK;
}

/* ---- the answer as data ---------------------------------------------------------------------- */

static const char *const box_kinds[] = {"root", "atomic", "compound", "parallel", "region", "final", "history", "folded"};
static const char *const edge_kinds[] = {"forward", "back", "self", "toAncestor", "toDescendant"};

static void open_list(tool *t, const char *name, const char *item) {
    ta_report_list(&t->report, (ta_report_list_names){ta_string(name), ta_string(item)});
}

static uint64_t whole(double value) { /* a coordinate, to the pixel */
    return value <= 0 ? 0 : (uint64_t)(value + 0.5);
}

typedef struct chart_view {
    const ta_image_view *view;
    const ta_chart_record *chart;
    const drawn *drawing;
    const graph_options *options;
} chart_view;

/* The id of a state of the chart drawn ("/scxml" for the root). */
static ta_string_view shown_id(const chart_view *c, uint32_t state) {
    if (state == TA_NIL) return ta_string("");
    return tool_state_id(c->view, c->chart, c->drawing->folding.original_state[state]);
}

static ta_string_view marks_text(uint32_t marks) {
    static const char *const names[] = {"", "highlight", "path", "highlight path"};
    return ta_string(names[marks & 3u]);
}

static void boxes_of(tool *t, const chart_view *c) {
    ta_report *report = &t->report;
    const ta_diagram_layout *layout = &c->drawing->layout;
    const ta_diagram_chart *shown = &c->drawing->folding.chart;
    open_list(t, "boxes", "box");
    for (uint32_t i = 0; i < layout->box_count; i++) {
        const ta_diagram_box *box = &layout->boxes[i];
        uint32_t original = c->drawing->folding.original_state[box->state];
        const ta_state_record *state = &c->view->states[c->chart->first_state + original];
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("state"), shown_id(c, box->state));
        ta_report_text(report, ta_string("kind"), tool_state_kind_name(state->kind));
        ta_report_text(report, ta_string("parent"), tool_state_id(c->view, c->chart, state->parent));
        ta_report_text(report, ta_string("drawnAs"), ta_string(box_kinds[box->kind]));
        if (c->options->folding) ta_report_number(report, ta_string("hides"), shown->states[box->state].hidden);
        if (c->options->marking) ta_report_text(report, ta_string("marks"), marks_text(c->drawing->marks[box->state]));
        ta_report_number(report, ta_string("x"), whole(box->rect.x));
        ta_report_number(report, ta_string("y"), whole(box->rect.y));
        ta_report_number(report, ta_string("width"), whole(box->rect.w));
        ta_report_number(report, ta_string("height"), whole(box->rect.h));
        ta_report_end(report);
    }
    ta_report_end(report);
}

/* "x,y x,y ...": an edge's route */
static ta_string_view route_text(tool *t, const ta_diagram_layout *layout, const ta_diagram_edge *edge) {
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t i = 0; i < edge->point_count; i++) {
        const ta_diagram_point *p = &layout->points[edge->first_point + i];
        if (i) tool_text_append(&text, ta_string(" "));
        tool_text_number(&text, whole(p->x));
        tool_text_append(&text, ta_string(","));
        tool_text_number(&text, whole(p->y));
    }
    return tool_text_view(&text);
}

static ta_string_view targets_text(tool *t, const chart_view *c, const ta_diagram_transition *transition) {
    tool_text text;
    tool_text_init(t, &text);
    const ta_diagram_chart *chart = &c->drawing->folding.chart;
    for (uint32_t j = 0; j < transition->target_count; j++) {
        if (j) tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, shown_id(c, chart->targets[transition->first_target + j]));
    }
    return tool_text_view(&text);
}

/* The path's steps taking a transition, numbered from 1: "2 5"; empty for none. */
static ta_string_view steps_text(tool *t, const drawn *d, uint32_t transition) {
    tool_text text;
    tool_text_init(t, &text);
    uint32_t written = 0;
    for (uint32_t i = 0; i < d->path_length; i++) {
        if (d->path[i] != transition) continue;
        if (written++) tool_text_append(&text, ta_string(" "));
        tool_text_number(&text, i + 1u);
    }
    return tool_text_view(&text);
}

static void edges_of(tool *t, const chart_view *c) {
    ta_report *report = &t->report;
    const ta_diagram_layout *layout = &c->drawing->layout;
    const ta_diagram_chart *chart = &c->drawing->folding.chart;
    open_list(t, "edges", "edge");
    for (uint32_t i = 0; i < layout->edge_count; i++) {
        const ta_diagram_edge *edge = &layout->edges[i];
        const ta_diagram_transition *transition = &chart->transitions[edge->transition];
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("source"), shown_id(c, transition->source));
        ta_report_text(report, ta_string("target"), shown_id(c, edge->target));
        ta_report_text(report, ta_string("events"), transition->events);
        ta_report_text(report, ta_string("condition"), transition->condition);
        ta_report_text(report, ta_string("drawnAs"), ta_string(edge_kinds[edge->kind]));
        if (c->options->path) ta_report_text(report, ta_string("steps"), steps_text(t, c->drawing, edge->transition));
        ta_report_text(report, ta_string("route"), route_text(t, layout, edge));
        ta_report_end(report);
    }
    ta_report_end(report);
    /* what each box lists inside: transitions without a target, and those not routed */
    open_list(t, "inside", "transition");
    for (uint32_t s = 0; s < chart->state_count; s++)
        for (uint32_t i = 0; i < layout->inside_count[s]; i++) {
            uint32_t index = layout->inside[layout->inside_first[s] + i];
            const ta_diagram_transition *transition = &chart->transitions[index];
            ta_report_object(report, ta_string(""));
            ta_report_text(report, ta_string("state"), shown_id(c, s));
            ta_report_text(report, ta_string("events"), transition->events);
            ta_report_text(report, ta_string("condition"), transition->condition);
            ta_report_text(report, ta_string("targets"), targets_text(t, c, transition));
            if (c->options->path) ta_report_text(report, ta_string("steps"), steps_text(t, c->drawing, index));
            ta_report_end(report);
        }
    ta_report_end(report);
}

/* What the layout must never show (ta_diagram_check): each problem, the edge or the box it is
 * about, and what it touches. A chart whose list is not empty is drawn wrong. */
static ta_string_view edge_text(tool *t, const chart_view *c, uint32_t edge) {
    const ta_diagram_layout *layout = &c->drawing->layout;
    const ta_diagram_transition *transition = &c->drawing->folding.chart.transitions[layout->edges[edge].transition];
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, shown_id(c, transition->source));
    tool_text_append(&text, ta_string(" \xE2\x86\x92 "));
    tool_text_append(&text, shown_id(c, layout->edges[edge].target));
    if (transition->events.length) {
        tool_text_append(&text, ta_string(" on "));
        tool_text_append(&text, transition->events);
    }
    return tool_text_view(&text);
}

static ta_status problems_of(tool *t, const chart_view *c) {
    ta_diagram_problems problems;
    ta_diagram_check_request request = {&c->drawing->folding.chart, &c->drawing->layout, t->arena};
    ta_status status = ta_diagram_check(&request, &problems);
    if (status != TA_OK) return status;
    ta_report *report = &t->report;
    open_list(t, "problems", "problem");
    for (uint32_t i = 0; i < problems.count; i++) {
        const ta_diagram_problem *problem = &problems.list[i];
        uint32_t kind = problem->kind;
        int state = kind == TA_DIAGRAM_PROBLEM_OUTSIDE_PARENT || kind == TA_DIAGRAM_PROBLEM_BOXES_OVERLAP ||
                    kind == TA_DIAGRAM_PROBLEM_INITIAL_DETACHED;
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("kind"), ta_diagram_problem_name(kind));
        ta_report_text(report, ta_string("subject"), state ? shown_id(c, problem->subject) : edge_text(t, c, problem->subject));
        ta_report_text(report, ta_string("touches"),
                       problem->other == TA_NIL ? ta_string("")
                       : kind == TA_DIAGRAM_PROBLEM_LABEL_OVERLAP || kind == TA_DIAGRAM_PROBLEM_LABEL_ON_EDGE ||
                                 kind == TA_DIAGRAM_PROBLEM_EDGES_OVERLAP
                           ? edge_text(t, c, problem->other)
                           : shown_id(c, problem->other));
        ta_report_end(report);
    }
    ta_report_end(report);
    return TA_OK;
}

/* ---- graph ---------------------------------------------------------------------------------- */

static void out_of_memory(tool *t) {
    t->exit = TOOL_EXIT_FAILURE;
    tool_complain(t, ta_string("out of memory"));
}

static const double page_margin = 12;

/* The picture: every selected chart, one above the other, each under its name, written
 * through one ta_xml_writer. */
typedef struct picture_request {
    const tool_opened *opened;
    const drawn *drawings;
    const graph_options *options;
} picture_request;

static void picture(tool *t, picture_request p) {
    const tool_opened *opened = p.opened;
    const drawn *drawings = p.drawings;
    const ta_image_view *view = ta_image_view_of(&opened->image.image);
    ta_diagram_page page = {{0, page_margin}, p.options->theme};
    for (uint32_t c = 0; c < opened->charts.count; c++) {
        if (!opened->charts.selected[c]) continue;
        ta_string_view name = string_of(view, view->charts[c].name);
        double width = drawings[c].layout.width + 2 * page_margin;
        if (width > page.size.w) page.size.w = width;
        page.size.h += ta_diagram_title_height(name) + drawings[c].layout.height + 2 * page_margin;
    }
    ta_allocator allocator = ta_growable_arena_allocator(t->arena);
    ta_xml_writer *writer;
    if (ta_xml_writer_new(&allocator, &writer) != TA_OK) {
        out_of_memory(t);
        return;
    }
    ta_diagram_svg_open(writer, &page);
    double top = page_margin;
    for (uint32_t c = 0; c < opened->charts.count; c++) {
        if (!opened->charts.selected[c]) continue;
        const drawn *d = &drawings[c];
        ta_string_view name = string_of(view, view->charts[c].name);
        ta_diagram_drawing drawing = {&d->folding.chart, &d->layout, d->portals, name,          top,
                                      t->arena,          d->marks,   d->path,    d->path_length};
        if (ta_diagram_svg(writer, &drawing) == TA_ERROR_NO_MEMORY) {
            ta_xml_writer_free(writer);
            out_of_memory(t);
            return;
        }
        top += ta_diagram_title_height(name) + d->layout.height + 2 * page_margin;
    }
    ta_diagram_svg_close(writer);
    ta_string_view document;
    ta_status status = ta_xml_writer_finish(writer, &document);
    if (status != TA_OK) {
        t->exit = TOOL_EXIT_FAILURE;
        tool_complain(t, status == TA_ERROR_NO_MEMORY ? ta_string("out of memory")
                                                      : ta_string("the drawing is not well-formed XML (a bug)"));
        ta_xml_writer_free(writer);
        return;
    }
    tool_write request = {TOOL_STANDARD_OUTPUT, {0, 0}, {document.pointer, document.length}};
    if (t->platform->write(t->platform->context, &request) != TA_OK) t->exit = TOOL_EXIT_FAILURE;
    tool_write newline = {TOOL_STANDARD_OUTPUT, {0, 0}, {"\n", 1}};
    if (t->exit == TOOL_EXIT_OK && t->platform->write(t->platform->context, &newline) != TA_OK) t->exit = TOOL_EXIT_FAILURE;
    ta_xml_writer_free(writer);
    t->answered = 1;
}

/* Every selected chart read, folded, laid out and marked; 0 told when it could not be (exit 1, 2
 * or 3). */
static int draw_all(tool *t, const tool_opened *opened, const graph_options *options, drawn *drawings) {
    const ta_image_view *view = ta_image_view_of(&opened->image.image);
    for (uint32_t c = 0; c < opened->charts.count; c++) {
        if (!opened->charts.selected[c]) continue;
        drawings[c] = (drawn){0};
        if (read_chart(t, view, &view->charts[c], &drawings[c]) != TA_OK) {
            out_of_memory(t);
            return 0;
        }
    }
    if (!names_known(t, (named_check){view, opened, drawings})) return 0;
    uint32_t paths = 0;
    for (uint32_t c = 0; c < opened->charts.count; c++) {
        if (!opened->charts.selected[c]) continue;
        drawn *d = &drawings[c];
        uint8_t *folded = folded_states(t, options, view, &view->charts[c], &d->chart);
        uint32_t found = 0;
        if (!folded || lay_out(t, options, folded, d) != TA_OK ||
            (options->marking && marks_of(t, (path_place){view, c, &found}, d, options) != TA_OK)) {
            out_of_memory(t);
            return 0;
        }
        paths += found;
    }
    if (options->path && !paths) {
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, ta_string("no path reaches '"));
        tool_text_append(&text, tool_last(t, TOOL_GRAPH_TO)->text);
        tool_text_append(&text, ta_string("' in a chart drawn (tinyactors path says why)"));
        tool_complain(t, tool_text_view(&text));
        t->exit = TOOL_EXIT_REFUSED;
        return 0;
    }
    return 1;
}

void tool_graph(tool *t) {
    graph_options options;
    if (!options_of(t, &options)) return;
    tool_opened opened;
    if (tool_open(t, arguments_of(t), &opened) != TA_OK) return;
    const ta_image_view *view = ta_image_view_of(&opened.image.image);
    drawn *drawings = (drawn *)ta_growable_arena_alloc(t->arena, sizeof(drawn) * (opened.charts.count + 1u), 8);
    if (!drawings) {
        out_of_memory(t);
        return;
    }
    if (!draw_all(t, &opened, &options, drawings)) return;
    if (t->format == TOOL_FORMAT_SVG) {
        picture(t, (picture_request){&opened, drawings, &options});
        return;
    }
    open_list(t, "charts", "chart");
    for (uint32_t c = 0; c < opened.charts.count; c++) {
        if (!opened.charts.selected[c]) continue;
        chart_view where = {view, &view->charts[c], &drawings[c], &options};
        ta_report_object(&t->report, ta_string(""));
        ta_report_text(&t->report, ta_string("name"), string_of(view, view->charts[c].name));
        ta_report_number(&t->report, ta_string("width"), whole(drawings[c].layout.width));
        ta_report_number(&t->report, ta_string("height"), whole(drawings[c].layout.height));
        boxes_of(t, &where);
        edges_of(t, &where);
        if (problems_of(t, &where) != TA_OK) {
            out_of_memory(t);
            return;
        }
        ta_report_end(&t->report);
    }
    ta_report_end(&t->report);
    if (opened.image.diagnostic_count) tool_report_diagnostics(t, &opened.image);
}
