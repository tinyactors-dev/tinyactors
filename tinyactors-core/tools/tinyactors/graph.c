/* graph.c — tinyactors graph: each selected chart's states and transitions
 * as nodes and edges, and as a Mermaid state diagram (tool.h).
 * Freestanding. */
#include "tool.h"

static tool_open_request arguments_of(const tool *t) {
    tool_open_request request = {t->parsed->argument_count, t->parsed->arguments, 0};
    return request;
}

static const ta_image_view *view_of(const tool_opened *opened) {
    return ta_image_view_of(&opened->image.image);
}

static ta_string_view string_of(const ta_image_view *view, uint32_t id) {
    return ta_image_view_string(view, id);
}

static ta_string_view state_id(const ta_image_view *view, const ta_chart_record *chart, uint32_t state) {
    if (state == TA_NIL || state >= chart->state_count) return ta_string("");
    ta_string_view id = string_of(view, view->states[chart->first_state + state].id);
    return id.length ? id : ta_string("<scxml>"); /* the root: its element, never an id */
}

static ta_string_view expression_text(const ta_image_view *view, const ta_chart_record *chart,
                                      uint32_t expression) {
    if (expression == TA_NIL || expression >= chart->expression_count) return ta_string("");
    return string_of(view, view->expressions[chart->first_expression + expression].source);
}

static ta_string_view transition_events(tool *t, const ta_image_view *view,
                                        const ta_transition_record *transition) {
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

static ta_string_view transition_kind(const ta_transition_record *transition, const ta_state_record *source) {
    if (!(transition->flags & TA_TRANSITION_FLAG_INITIAL)) return ta_string("transition");
    return source->kind >= TA_STATE_HISTORY_SHALLOW ? ta_string("history default") : ta_string("initial");
}

static void open_list(tool *t, const char *name, const char *item) {
    ta_report_list(&t->report, (ta_report_list_names){ta_string(name), ta_string(item)});
}

typedef void (*chart_members)(tool *t, const tool_opened *opened, uint32_t chart);
static void each_chart(tool *t, const tool_opened *opened, chart_members each) {
    const ta_image_view *view = view_of(opened);
    open_list(t, "charts", "chart");
    for (uint32_t c = 0; c < opened->charts.count; c++) {
        if (!opened->charts.selected[c]) continue;
        ta_report_object(&t->report, ta_string(""));
        ta_report_text(&t->report, ta_string("name"), string_of(view, view->charts[c].name));
        each(t, opened, c);
        ta_report_end(&t->report);
    }
    ta_report_end(&t->report);
}

/* ---- nodes and edges ---------------------------------------------------------------------- */

static void node_members(tool *t, const ta_image_view *view, const ta_chart_record *chart) {
    ta_report *report = &t->report;
    open_list(t, "nodes", "node");
    for (uint32_t s = 0; s < chart->state_count; s++) {
        const ta_state_record *state = &view->states[chart->first_state + s];
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("id"), state_id(view, chart, s));
        ta_report_text(report, ta_string("kind"), tool_state_kind_name(state->kind));
        ta_report_text(report, ta_string("parent"), state_id(view, chart, state->parent));
        ta_report_end(report);
    }
    ta_report_end(report);
}

/* A chart of the image. */
typedef struct chart_view {
    const ta_image_view *view;
    const ta_chart_record *chart;
} chart_view;

/* One edge per target of transition `index`; a targetless one is an edge
 * to "". */
static void edges_of(tool *t, const chart_view *where, uint32_t index) {
    const ta_image_view *view = where->view;
    const ta_chart_record *chart = where->chart;
    const ta_transition_record *transition = &view->transitions[chart->first_transition + index];
    const ta_state_record *source = &view->states[chart->first_state + transition->source];
    ta_report *report = &t->report;
    ta_string_view events = transition_events(t, view, transition);
    ta_string_view condition = expression_text(view, chart, transition->condition);
    ta_string_view kind = transition_kind(transition, source);
    uint32_t count = transition->target_count ? transition->target_count : 1u;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t target = transition->target_count ? view->u32[transition->targets_offset + i] : TA_NIL;
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("source"), state_id(view, chart, transition->source));
        ta_report_text(report, ta_string("target"), state_id(view, chart, target));
        ta_report_text(report, ta_string("events"), events);
        ta_report_text(report, ta_string("condition"), condition);
        ta_report_text(report, ta_string("kind"), kind);
        ta_report_end(report);
    }
}

/* ---- the diagram -------------------------------------------------------------------------- */

typedef struct diagram {
    tool_text text;
    const ta_image_view *view;
    const ta_chart_record *chart;
} diagram;

typedef enum diagram_escape {
    DIAGRAM_QUOTED = 0, /* a state's description, in quotes */
    DIAGRAM_LABEL = 1   /* a transition's label, after ':' */
} diagram_escape;

static void put(diagram *d, const char *piece) {
    tool_text_append(&d->text, ta_string(piece));
}

static void indent(diagram *d, uint32_t level) {
    for (uint32_t i = 0; i < level; i++)
        put(d, "    ");
}

/* Text inside a label: '"' as #quot;, newlines as spaces, and in a
 * transition's label ':' as a space. */
static void put_escaped(diagram *d, ta_string_view text, uint32_t escape) {
    uint32_t start = 0;
    for (uint32_t i = 0; i < text.length; i++) {
        char letter = text.pointer[i];
        const char *replacement = letter == '"'                                    ? "#quot;"
                                  : letter == '\n' || letter == '\r'               ? " "
                                  : letter == ':' && escape == DIAGRAM_LABEL ? " "
                                                                                   : 0;
        if (!replacement) continue;
        tool_text_append(&d->text, (ta_string_view){text.pointer + start, i - start});
        put(d, replacement);
        start = i + 1;
    }
    tool_text_append(&d->text, (ta_string_view){text.pointer + start, text.length - start});
}

static void put_state(diagram *d, uint32_t state) {
    put(d, "s");
    tool_text_number(&d->text, state);
}

/* "[*] --> s<x>" for each target of the initial transition of `state`. */
static void put_initial(diagram *d, uint32_t state, uint32_t level) {
    const ta_state_record *record = &d->view->states[d->chart->first_state + state];
    if (record->initial == TA_NIL || record->initial >= d->chart->transition_count) return;
    const ta_transition_record *initial = &d->view->transitions[d->chart->first_transition + record->initial];
    for (uint32_t i = 0; i < initial->target_count; i++) {
        indent(d, level);
        put(d, "[*] --> ");
        put_state(d, d->view->u32[initial->targets_offset + i]);
        put(d, "\n");
    }
}

static void put_transition(diagram *d, const ta_transition_record *transition, tool *t) {
    ta_string_view events = transition_events(t, d->view, transition);
    ta_string_view condition = expression_text(d->view, d->chart, transition->condition);
    for (uint32_t i = 0; i < transition->target_count; i++) {
        indent(d, 1);
        put_state(d, transition->source);
        put(d, " --> ");
        put_state(d, d->view->u32[transition->targets_offset + i]);
        if (events.length || condition.length) put(d, " : ");
        put_escaped(d, events, DIAGRAM_LABEL);
        if (condition.length) {
            if (events.length) put(d, " ");
            put(d, "[");
            put_escaped(d, condition, DIAGRAM_LABEL);
            put(d, "]");
        }
        put(d, "\n");
    }
}

/* The states in document order; a compound or parallel state is a block,
 * closed when the walk leaves its descendants (no recursion). */
static void put_states(diagram *d, uint32_t *open) {
    const ta_state_record *states = &d->view->states[d->chart->first_state];
    uint32_t depth = 0;
    for (uint32_t s = 1; s < d->chart->state_count; s++) {
        while (depth && states[open[depth - 1]].end <= s) {
            depth--;
            indent(d, depth + 1);
            put(d, "}\n");
        }
        uint32_t level = depth + 1;
        uint32_t parent = states[s].parent;
        if (parent != 0 && parent != TA_NIL && states[parent].kind == TA_STATE_PARALLEL && s != parent + 1) {
            indent(d, level);
            put(d, "--\n");
        }
        indent(d, level);
        put(d, "state \"");
        put_escaped(d, state_id(d->view, d->chart, s), DIAGRAM_QUOTED);
        put(d, "\" as ");
        put_state(d, s);
        put(d, "\n");
        if (states[s].kind == TA_STATE_FINAL) {
            indent(d, level);
            put_state(d, s);
            put(d, " --> [*]\n");
        }
        if (states[s].kind == TA_STATE_COMPOUND || states[s].kind == TA_STATE_PARALLEL) {
            indent(d, level);
            put(d, "state ");
            put_state(d, s);
            put(d, " {\n");
            open[depth++] = s;
            put_initial(d, s, level + 1);
        }
    }
    while (depth) {
        depth--;
        indent(d, depth + 1);
        put(d, "}\n");
    }
}

static ta_string_view mermaid(tool *t, const ta_image_view *view, const ta_chart_record *chart) {
    diagram d = {.view = view, .chart = chart};
    tool_text_init(t, &d.text);
    uint32_t *open = (uint32_t *)ta_growable_arena_alloc(t->arena, sizeof *open * (chart->state_count + 1u), 4);
    if (!open) return ta_string("");
    put(&d, "stateDiagram-v2\n");
    put_initial(&d, 0, 1);
    put_states(&d, open);
    for (uint32_t i = 0; i < chart->transition_count; i++) {
        const ta_transition_record *transition = &view->transitions[chart->first_transition + i];
        const ta_state_record *source = &view->states[chart->first_state + transition->source];
        if ((transition->flags & TA_TRANSITION_FLAG_INITIAL) && source->kind < TA_STATE_HISTORY_SHALLOW)
            continue; /* drawn as [*] inside its state's block */
        put_transition(&d, transition, t);
    }
    return tool_text_view(&d.text);
}

/* ---- graph -------------------------------------------------------------------------------- */

static void graph_members(tool *t, const tool_opened *opened, uint32_t c) {
    const ta_image_view *view = view_of(opened);
    const ta_chart_record *chart = &view->charts[c];
    node_members(t, view, chart);
    open_list(t, "edges", "edge");
    chart_view where = {view, chart};
    for (uint32_t i = 0; i < chart->transition_count; i++)
        edges_of(t, &where, i);
    ta_report_end(&t->report);
    ta_report_block(&t->report, ta_string("diagram"),
                    (ta_report_code){ta_string("mermaid"), mermaid(t, view, chart)});
}

void tool_graph(tool *t) {
    tool_opened opened;
    if (tool_open(t, arguments_of(t), &opened) != TA_OK) return;
    each_chart(t, &opened, graph_members);
    if (opened.image.diagnostic_count) tool_report_diagnostics(t, &opened.image);
}
