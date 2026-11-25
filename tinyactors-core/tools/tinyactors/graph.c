/* graph.c — tinyactors graph: each selected chart drawn (diagram.h). With
 * -f svg the answer is the picture: the charts one above the other, each
 * under its name. In the other formats it is the drawing as data: each
 * chart's boxes (its states, where they are drawn), its edges (one per
 * target of each routed transition) and the transitions a box lists
 * inside it. Freestanding. */
#include "tool.h"

#include "diagram/svg.h"

static tool_open_request arguments_of(const tool *t) {
    tool_open_request request = {t->parsed->argument_count, t->parsed->arguments, 0};
    return request;
}

static ta_string_view string_of(const ta_image_view *view, uint32_t id) {
    return ta_image_view_string(view, id);
}

static ta_string_view state_id(const ta_image_view *view, const ta_chart_record *chart, uint32_t state) {
    if (state == TA_NIL || state >= chart->state_count) return ta_string("");
    ta_string_view id = string_of(view, view->states[chart->first_state + state].id);
    return id.length ? id : ta_string("/scxml"); /* the root: its XPath, never an id */
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

/* A chart, drawn: what the diagram reads and what it made. */
typedef struct drawn {
    ta_diagram_chart chart;
    uint8_t *portals;
    ta_diagram_layout layout;
} drawn;

/* The image's chart as a diagram's: its states in document order, each
 * compound state's initial transition as its initial states, every other
 * transition (a history state's default among them) as a transition. */
static ta_status draw(tool *t, const ta_image_view *view, const ta_chart_record *chart, drawn *out) {
    uint32_t n = chart->state_count, transitions = chart->transition_count, targets = 0;
    for (uint32_t i = 0; i < transitions; i++)
        targets += view->transitions[chart->first_transition + i].target_count;
    ta_diagram_state *states =
        (ta_diagram_state *)ta_growable_arena_alloc(t->arena, sizeof(ta_diagram_state) * (n + 1u), 8);
    ta_diagram_transition *list = (ta_diagram_transition *)ta_growable_arena_alloc(
        t->arena, sizeof(ta_diagram_transition) * (transitions + 1u), 8);
    uint32_t *target_list = (uint32_t *)ta_growable_arena_alloc(t->arena, sizeof(uint32_t) * (targets + 1u), 8);
    if (!states || !list || !target_list) return TA_ERROR_NO_MEMORY;
    uint32_t count = 0, target_count = 0;
    for (uint32_t s = 0; s < n; s++) {
        const ta_state_record *state = &view->states[chart->first_state + s];
        states[s] = (ta_diagram_state){state->parent, diagram_kind(state, s), string_of(view, state->id), 0, 0};
    }
    for (uint32_t i = 0; i < transitions; i++) {
        const ta_transition_record *transition = &view->transitions[chart->first_transition + i];
        uint32_t first = target_count;
        for (uint32_t j = 0; j < transition->target_count; j++)
            target_list[target_count++] = view->u32[transition->targets_offset + j];
        const ta_state_record *source = &view->states[chart->first_state + transition->source];
        if ((transition->flags & TA_TRANSITION_FLAG_INITIAL) && source->kind < TA_STATE_HISTORY_SHALLOW) {
            states[transition->source].first_initial = first;
            states[transition->source].initial_count = transition->target_count;
            continue;
        }
        list[count++] = (ta_diagram_transition){transition->source, first, transition->target_count,
                                                transition_events(t, view, transition),
                                                expression_text(view, chart, transition->condition)};
    }
    out->chart = (ta_diagram_chart){states, n, list, count, target_list};
    out->portals = ta_diagram_portals(&out->chart, t->arena);
    if (!out->portals) return TA_ERROR_NO_MEMORY;
    ta_diagram_measures measures;
    ta_diagram_measure_request measure = {&out->chart, out->portals, t->arena};
    if (ta_diagram_measure(&measure, &measures) != TA_OK) return TA_ERROR_NO_MEMORY;
    ta_diagram_request request = {&out->chart, TA_DIAGRAM_RIGHT, 0, measures.states, measures.labels,
                                  out->portals, t->arena};
    return ta_diagram_layout_chart(&request, &out->layout);
}

/* ---- the answer as data ---------------------------------------------------------------------- */

static const char *const box_kinds[] = {"root", "atomic", "compound", "parallel", "region", "final", "history"};
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
} chart_view;

static void boxes_of(tool *t, const chart_view *c) {
    ta_report *report = &t->report;
    const ta_diagram_layout *layout = &c->drawing->layout;
    open_list(t, "boxes", "box");
    for (uint32_t i = 0; i < layout->box_count; i++) {
        const ta_diagram_box *box = &layout->boxes[i];
        const ta_state_record *state = &c->view->states[c->chart->first_state + box->state];
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("state"), state_id(c->view, c->chart, box->state));
        ta_report_text(report, ta_string("kind"), tool_state_kind_name(state->kind));
        ta_report_text(report, ta_string("parent"), state_id(c->view, c->chart, state->parent));
        ta_report_text(report, ta_string("drawnAs"), ta_string(box_kinds[box->kind]));
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
    const ta_diagram_chart *chart = &c->drawing->chart;
    for (uint32_t j = 0; j < transition->target_count; j++) {
        if (j) tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, state_id(c->view, c->chart, chart->targets[transition->first_target + j]));
    }
    return tool_text_view(&text);
}

static void edges_of(tool *t, const chart_view *c) {
    ta_report *report = &t->report;
    const ta_diagram_layout *layout = &c->drawing->layout;
    const ta_diagram_chart *chart = &c->drawing->chart;
    open_list(t, "edges", "edge");
    for (uint32_t i = 0; i < layout->edge_count; i++) {
        const ta_diagram_edge *edge = &layout->edges[i];
        const ta_diagram_transition *transition = &chart->transitions[edge->transition];
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("source"), state_id(c->view, c->chart, transition->source));
        ta_report_text(report, ta_string("target"), state_id(c->view, c->chart, edge->target));
        ta_report_text(report, ta_string("events"), transition->events);
        ta_report_text(report, ta_string("condition"), transition->condition);
        ta_report_text(report, ta_string("drawnAs"), ta_string(edge_kinds[edge->kind]));
        ta_report_text(report, ta_string("route"), route_text(t, layout, edge));
        ta_report_end(report);
    }
    ta_report_end(report);
    /* what each box lists inside: transitions without a target, and those not routed */
    open_list(t, "inside", "transition");
    for (uint32_t s = 0; s < chart->state_count; s++)
        for (uint32_t i = 0; i < layout->inside_count[s]; i++) {
            const ta_diagram_transition *transition = &chart->transitions[layout->inside[layout->inside_first[s] + i]];
            ta_report_object(report, ta_string(""));
            ta_report_text(report, ta_string("state"), state_id(c->view, c->chart, s));
            ta_report_text(report, ta_string("events"), transition->events);
            ta_report_text(report, ta_string("condition"), transition->condition);
            ta_report_text(report, ta_string("targets"), targets_text(t, c, transition));
            ta_report_end(report);
        }
    ta_report_end(report);
}

/* What the layout must never show (ta_diagram_check): each problem, the edge or the box it is
 * about, and what it touches. A chart whose list is not empty is drawn wrong. */
static ta_string_view edge_text(tool *t, const chart_view *c, uint32_t edge) {
    const ta_diagram_layout *layout = &c->drawing->layout;
    const ta_diagram_transition *transition = &c->drawing->chart.transitions[layout->edges[edge].transition];
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, state_id(c->view, c->chart, transition->source));
    tool_text_append(&text, ta_string(" \xE2\x86\x92 "));
    tool_text_append(&text, state_id(c->view, c->chart, layout->edges[edge].target));
    if (transition->events.length) {
        tool_text_append(&text, ta_string(" on "));
        tool_text_append(&text, transition->events);
    }
    return tool_text_view(&text);
}

static ta_status problems_of(tool *t, const chart_view *c) {
    ta_diagram_problems problems;
    ta_diagram_check_request request = {&c->drawing->chart, &c->drawing->layout, t->arena};
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
        ta_report_text(report, ta_string("subject"),
                       state ? state_id(c->view, c->chart, problem->subject) : edge_text(t, c, problem->subject));
        ta_report_text(report, ta_string("touches"),
                       problem->other == TA_NIL ? ta_string("")
                       : kind == TA_DIAGRAM_PROBLEM_LABEL_OVERLAP || kind == TA_DIAGRAM_PROBLEM_LABEL_ON_EDGE
                           ? edge_text(t, c, problem->other)
                                                                  : state_id(c->view, c->chart, problem->other));
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
static void picture(tool *t, const tool_opened *opened, const drawn *drawings) {
    const ta_image_view *view = ta_image_view_of(&opened->image.image);
    ta_diagram_size page = {0, page_margin};
    for (uint32_t c = 0; c < opened->charts.count; c++) {
        if (!opened->charts.selected[c]) continue;
        ta_string_view name = string_of(view, view->charts[c].name);
        double width = drawings[c].layout.width + 2 * page_margin;
        if (width > page.w) page.w = width;
        page.h += ta_diagram_title_height(name) + drawings[c].layout.height + 2 * page_margin;
    }
    ta_allocator allocator = ta_growable_arena_allocator(t->arena);
    ta_xml_writer *writer;
    if (ta_xml_writer_new(&allocator, &writer) != TA_OK) {
        out_of_memory(t);
        return;
    }
    ta_diagram_svg_open(writer, page);
    double top = page_margin;
    for (uint32_t c = 0; c < opened->charts.count; c++) {
        if (!opened->charts.selected[c]) continue;
        ta_string_view name = string_of(view, view->charts[c].name);
        ta_diagram_drawing drawing = {&drawings[c].chart, &drawings[c].layout, drawings[c].portals, name, top, t->arena};
        if (ta_diagram_svg(writer, &drawing) == TA_ERROR_NO_MEMORY) {
            ta_xml_writer_free(writer);
            out_of_memory(t);
            return;
        }
        top += ta_diagram_title_height(name) + drawings[c].layout.height + 2 * page_margin;
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

void tool_graph(tool *t) {
    tool_opened opened;
    if (tool_open(t, arguments_of(t), &opened) != TA_OK) return;
    const ta_image_view *view = ta_image_view_of(&opened.image.image);
    drawn *drawings = (drawn *)ta_growable_arena_alloc(t->arena, sizeof(drawn) * (opened.charts.count + 1u), 8);
    if (!drawings) {
        out_of_memory(t);
        return;
    }
    for (uint32_t c = 0; c < opened.charts.count; c++) {
        if (!opened.charts.selected[c]) continue;
        if (draw(t, view, &view->charts[c], &drawings[c]) != TA_OK) {
            out_of_memory(t);
            return;
        }
    }
    if (t->format == TOOL_FORMAT_SVG) {
        picture(t, &opened, drawings);
        return;
    }
    open_list(t, "charts", "chart");
    for (uint32_t c = 0; c < opened.charts.count; c++) {
        if (!opened.charts.selected[c]) continue;
        chart_view where = {view, &view->charts[c], &drawings[c]};
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
