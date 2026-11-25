/* path.c — tinyactors path: the shortest sequence of transitions that
 * takes a chart from its initial configuration (or from a state) to a
 * configuration holding a state (analysis/analysis.h), each step with the
 * events that take it, the condition it needs, its source and targets and
 * the atomic states active after it. Freestanding. */
#include "tool.h"

#include "analysis/analysis.h"

static ta_string_view string_of(const ta_image_view *view, uint32_t id) {
    return ta_image_view_string(view, id);
}

typedef struct path_chart {
    const ta_image_view *view;
    const ta_chart_record *chart;
} path_chart;

static ta_string_view state_id(path_chart in, uint32_t state) {
    if (state == TA_NIL || state >= in.chart->state_count) return ta_string("");
    ta_string_view id = string_of(in.view, in.view->states[in.chart->first_state + state].id);
    return id.length ? id : ta_string("/scxml"); /* the root: its XPath, never an id */
}

static uint32_t state_named(path_chart in, ta_string_view name) {
    for (uint32_t s = 0; s < in.chart->state_count; s++)
        if (tool_equal(state_id(in, s), name)) return s;
    return TA_NIL;
}

static const ta_transition_record *transition_of(path_chart in, uint32_t transition) {
    return &in.view->transitions[in.chart->first_transition + transition];
}

static ta_string_view events_text(tool *t, path_chart in, const ta_transition_record *transition) {
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t i = 0; i < transition->event_count; i++) {
        uint32_t node = in.view->u32[transition->events_offset + 2 * i];
        ta_string_view name = node < in.view->event_node_count ? string_of(in.view, in.view->event_nodes[node].name)
                                                               : ta_string("");
        if (i) tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, name.length ? name : ta_string("*"));
    }
    return tool_text_view(&text);
}

static ta_string_view targets_text(tool *t, path_chart in, const ta_transition_record *transition) {
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t i = 0; i < transition->target_count; i++) {
        if (i) tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, state_id(in, in.view->u32[transition->targets_offset + i]));
    }
    return tool_text_view(&text);
}

static ta_string_view condition_text(path_chart in, const ta_transition_record *transition) {
    if (transition->condition == TA_NIL || transition->condition >= in.chart->expression_count) return ta_string("");
    return string_of(in.view, in.view->expressions[in.chart->first_expression + transition->condition].source);
}

/* The atomic and final states a configuration holds, separated by spaces. */
static ta_string_view active_text(tool *t, path_chart in, const uint32_t *configuration) {
    tool_text text;
    tool_text_init(t, &text);
    uint32_t written = 0;
    for (uint32_t s = 0; s < in.chart->state_count; s++) {
        uint32_t kind = in.view->states[in.chart->first_state + s].kind;
        if (!((configuration[s >> 5] >> (s & 31u)) & 1u) || (kind != TA_STATE_ATOMIC && kind != TA_STATE_FINAL)) continue;
        if (written++) tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, state_id(in, s));
    }
    return tool_text_view(&text);
}

/* The state an option names; TA_NIL told when it names none (exit 1). */
static uint32_t named_option(tool *t, path_chart in, uint32_t option) {
    const ta_cli_value *value = tool_last(t, option);
    if (!value) return TA_NIL;
    uint32_t state = state_named(in, value->text);
    if (state != TA_NIL) return state;
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, ta_string("chart '"));
    tool_text_append(&text, string_of(in.view, in.chart->name));
    tool_text_append(&text, ta_string("' has no state '"));
    tool_text_append(&text, value->text);
    tool_text_append(&text, ta_string("'"));
    tool_complain(t, tool_text_view(&text));
    t->exit = TOOL_EXIT_USAGE;
    return TA_NIL;
}

static ta_string_view result_name(uint32_t result) {
    return result == TA_ANALYSIS_PATH_FOUND         ? ta_string("found")
           : result == TA_ANALYSIS_PATH_UNREACHABLE ? ta_string("unreachable")
                                                   : ta_string("unknown");
}

static ta_string_view result_sentence(uint32_t result) {
    return result == TA_ANALYSIS_PATH_FOUND
               ? ta_string("the steps below reach it, if their events come and their conditions hold")
           : result == TA_ANALYSIS_PATH_UNREACHABLE
               ? ta_string("no sequence of transitions reaches it, whatever events come and conditions hold")
               : ta_string("the search stopped at its limit of configurations before reaching it");
}

static void report_steps(tool *t, path_chart in, const ta_analysis_path *path) {
    ta_report *report = &t->report;
    ta_report_list(report, (ta_report_list_names){ta_string("steps"), ta_string("step")});
    for (uint32_t i = 1; i < path->step_count; i++) {
        const ta_analysis_step *step = &path->steps[i];
        const ta_transition_record *transition = transition_of(in, step->transition);
        ta_report_object(report, ta_string(""));
        ta_report_number(report, ta_string("step"), i);
        ta_report_text(report, ta_string("events"), events_text(t, in, transition));
        ta_report_boolean(report, ta_string("eventless"), !transition->event_count);
        ta_report_text(report, ta_string("condition"), condition_text(in, transition));
        ta_report_text(report, ta_string("source"), state_id(in, transition->source));
        ta_report_text(report, ta_string("targets"), targets_text(t, in, transition));
        ta_report_text(report, ta_string("active"), active_text(t, in, step->configuration));
        ta_report_end(report);
    }
    ta_report_end(report);
}

void tool_path(tool *t) {
    tool_opened opened;
    tool_open_request open = {t->parsed->argument_count, t->parsed->arguments, 0};
    if (tool_count(t, TOOL_OPTION_CHART) > 1) {
        tool_complain(t, ta_string("path looks at one chart: give --chart once"));
        t->exit = TOOL_EXIT_USAGE;
        return;
    }
    if (tool_open(t, open, &opened) != TA_OK) return;
    const ta_image_view *view = ta_image_view_of(&opened.image.image);
    uint32_t chart = 0; /* the first selected: without --chart, the default boot chart */
    while (chart < opened.charts.count && !opened.charts.selected[chart]) chart++;
    path_chart in = {view, &view->charts[chart]};
    uint32_t to = named_option(t, in, TOOL_PATH_TO);
    if (to == TA_NIL) return;
    uint32_t from = TA_NIL;
    if (tool_count(t, TOOL_PATH_FROM) && (from = named_option(t, in, TOOL_PATH_FROM)) == TA_NIL) return;
    ta_analysis_path_request request = {view, chart, from, to, 0, t->arena};
    ta_analysis_path path;
    ta_status status = ta_analysis_path_find(&request, &path);
    if (status == TA_ERROR_INVALID) {
        tool_complain(t, ta_string("a history state is never active itself: name a state, a parallel or a final"));
        t->exit = TOOL_EXIT_USAGE;
        return;
    }
    if (status != TA_OK) {
        t->exit = TOOL_EXIT_FAILURE;
        tool_complain(t, ta_string("out of memory"));
        return;
    }
    ta_report *report = &t->report;
    ta_report_text(report, ta_string("chart"), string_of(view, in.chart->name));
    ta_report_text(report, ta_string("from"), from == TA_NIL ? ta_string("") : state_id(in, from));
    ta_report_text(report, ta_string("to"), state_id(in, to));
    ta_report_text(report, ta_string("result"), result_name(path.result));
    ta_report_text(report, ta_string("meaning"), result_sentence(path.result));
    ta_report_number(report, ta_string("transitions"), path.step_count - 1u);
    ta_report_number(report, ta_string("explored"), path.explored);
    ta_report_text(report, ta_string("start"), active_text(t, in, path.steps[0].configuration));
    report_steps(t, in, &path);
    if (opened.image.diagnostic_count) tool_report_diagnostics(t, &opened.image);
    int wanted = tool_count(t, TOOL_PATH_UNREACHABLE) ? path.result == TA_ANALYSIS_PATH_UNREACHABLE
                                                      : path.result == TA_ANALYSIS_PATH_FOUND;
    if (!wanted) t->exit = TOOL_EXIT_REFUSED;
}
