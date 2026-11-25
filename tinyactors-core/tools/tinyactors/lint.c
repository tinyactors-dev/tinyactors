/* lint.c — tinyactors lint: what the analysis finds in the selected charts
 * (analysis/analysis.h), each finding with its rule, severity, chart,
 * state, transition, event and line, and a sentence saying what it means.
 * --deny makes a rule's findings fail the command, --allow silences one.
 * Freestanding. */
#include "tool.h"

#include "analysis/analysis.h"

static const ta_image_view *view_of(const tool_opened *opened) {
    return ta_image_view_of(&opened->image.image);
}

static ta_string_view string_of(const ta_image_view *view, uint32_t id) {
    return ta_image_view_string(view, id);
}

static const ta_chart_record *chart_of(const ta_image_view *view, uint32_t chart) {
    return &view->charts[chart];
}

static ta_string_view state_id(const ta_image_view *view, const ta_chart_record *chart, uint32_t state) {
    if (state == TA_NIL || state >= chart->state_count) return ta_string("");
    ta_string_view id = string_of(view, view->states[chart->first_state + state].id);
    return id.length ? id : ta_string("/scxml"); /* the root: its XPath, never an id */
}

static ta_string_view event_name(const ta_image_view *view, uint32_t node) {
    if (node == TA_NIL || node >= view->event_node_count) return ta_string("");
    ta_string_view name = string_of(view, view->event_nodes[node].name);
    return name.length ? name : ta_string("*");
}

/* "checkout → paying", "eventless → paid", "cancel → no target": a
 * transition as a finding names it. */
static ta_string_view transition_text(tool *t, const ta_image_view *view, const ta_analysis_finding *finding) {
    const ta_chart_record *chart = chart_of(view, finding->chart);
    if (finding->transition == TA_NIL || finding->transition >= chart->transition_count) return ta_string("");
    const ta_transition_record *transition = &view->transitions[chart->first_transition + finding->transition];
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t i = 0; i < transition->event_count; i++) {
        if (i) tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, event_name(view, view->u32[transition->events_offset + 2 * i]));
    }
    if (!transition->event_count) tool_text_append(&text, ta_string("eventless"));
    tool_text_append(&text, ta_string(" → "));
    for (uint32_t i = 0; i < transition->target_count; i++) {
        if (i) tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, state_id(view, chart, view->u32[transition->targets_offset + i]));
    }
    if (!transition->target_count) tool_text_append(&text, ta_string("no target"));
    return tool_text_view(&text);
}

/* ---- what a finding means ----------------------------------------------------------------- */

typedef struct finding_words {
    tool *t;
    const ta_image_view *view;
    const ta_analysis_finding *finding;
    tool_text text;
} finding_words;

static void say(finding_words *words, const char *piece) {
    tool_text_append(&words->text, ta_string(piece));
}

static void say_text(finding_words *words, ta_string_view piece) {
    tool_text_append(&words->text, piece);
}

static ta_string_view state_of(const finding_words *words) {
    return state_id(words->view, chart_of(words->view, words->finding->chart), words->finding->state);
}

static void say_send(finding_words *words) {
    const ta_analysis_finding *finding = words->finding;
    if (finding->send == TA_NIL) {
        say(words, "raised");
        return;
    }
    const ta_chart_record *chart = chart_of(words->view, finding->chart);
    const ta_send_record *send = &words->view->sends[chart->first_send + finding->send];
    switch (send->target_kind) {
    case TA_SEND_TARGET_PARENT: say(words, "sent to the chart's parent"); break;
    case TA_SEND_TARGET_INVOKE: {
        const ta_invoke_record *invoke = &words->view->invokes[chart->first_invoke + send->target];
        say(words, "sent to the invoked ");
        say_text(words, string_of(words->view, chart_of(words->view, invoke->source_chart)->name));
        break;
    }
    default: say(words, "sent to the chart itself"); break;
    }
}

static ta_string_view message(finding_words *words) {
    const ta_analysis_finding *finding = words->finding;
    const ta_image_view *view = words->view;
    tool_text_init(words->t, &words->text);
    switch (finding->rule) {
    case TA_ANALYSIS_UNREACHABLE:
        say(words, "no transition, initial state or history default ever enters it");
        break;
    case TA_ANALYSIS_DEAD_END:
        say(words, "it is not final, handles no event and nothing exits it: an actor that enters it stays there, doing nothing");
        break;
    case TA_ANALYSIS_NEVER_ENDS:
        say(words, finding->state ? "no final state of the chart is reachable once it is entered"
                                  : "no final state of the chart is reachable from its initial configuration");
        break;
    case TA_ANALYSIS_SHADOWED: {
        ta_analysis_finding earlier = *finding;
        earlier.transition = finding->other;
        const ta_chart_record *chart = chart_of(view, finding->chart);
        const ta_transition_record *other = &view->transitions[chart->first_transition + finding->other];
        say(words, "never taken: ");
        say_text(words, transition_text(words->t, view, &earlier));
        say(words, other->event_count ? " has no condition and its events cover this one's, so it is taken first"
                                      : " is eventless and has no condition, so it is taken before any event");
        break;
    }
    case TA_ANALYSIS_EVENTLESS_LOOP:
        say(words, "an eventless transition without a condition that is taken again and again: "
                   "the actor never rests and never takes an event");
        break;
    case TA_ANALYSIS_UNHANDLED_EVENT:
        say(words, "the event is ");
        say_send(words);
        say(words, ", and no transition there handles it");
        break;
    case TA_ANALYSIS_UNSENT_EVENT:
        say(words, "nothing in the image raises or sends it to ");
        say_text(words, string_of(view, chart_of(view, finding->chart)->name));
        say(words, ": only the application can");
        break;
    case TA_ANALYSIS_UNKNOWN_EVENTS:
        say_text(words, string_of(view, chart_of(view, finding->other)->name));
        say(words, " sends an event named at run time (eventexpr) that may arrive here: "
                   "which events arrive is unknown, so the events handled here are not checked");
        break;
    case TA_ANALYSIS_SEND_TARGET: {
        const ta_chart_record *chart = chart_of(view, finding->chart);
        ta_string_view target = string_of(view, view->sends[chart->first_send + finding->send].target);
        say(words, "the target ");
        say_text(words, target);
        say(words, target.length > 1 && target.pointer[0] == '#'
                       ? " names no invocation of the chart: the send fails with error.communication"
                       : " is neither #_<invokeid>, #_scxml_<session> nor an absolute URI: the send fails "
                         "with error.execution");
        break;
    }
    case TA_ANALYSIS_SEARCH_LIMIT:
        say(words, "the search of the chart's configurations stopped at ");
        tool_text_number(&words->text, finding->other);
        say(words, ": never-ends was not checked");
        break;
    default: break;
    }
    return tool_text_view(&words->text);
}

/* ---- --deny and --allow ------------------------------------------------------------------- */

/* One byte per rule: given with the option. TA_ERROR_INVALID: a name that
 * is no rule (told, exit 2). */
static ta_status rules_given(tool *t, uint32_t option, uint8_t *given) {
    for (uint32_t rule = 0; rule < TA_ANALYSIS_RULE_COUNT; rule++)
        given[rule] = 0;
    for (uint32_t i = 0; i < t->parsed->value_count; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        if (value->option.scope != TA_CLI_SCOPE_COMMAND || value->option.option != option) continue;
        if (tool_equal(value->text, ta_string("all"))) {
            for (uint32_t rule = 0; rule < TA_ANALYSIS_RULE_COUNT; rule++)
                given[rule] = 1;
            continue;
        }
        uint32_t rule = ta_analysis_rule_named(value->text);
        if (rule != TA_NIL) {
            given[rule] = 1;
            continue;
        }
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, ta_string("'"));
        tool_text_append(&text, value->text);
        tool_text_append(&text, ta_string("' is not a rule; the rules are all"));
        for (uint32_t r = 0; r < TA_ANALYSIS_RULE_COUNT; r++) {
            tool_text_append(&text, ta_string(", "));
            tool_text_append(&text, ta_analysis_rule_name(r));
        }
        tool_complain(t, tool_text_view(&text));
        t->exit = TOOL_EXIT_USAGE;
        return TA_ERROR_INVALID;
    }
    return TA_OK;
}

/* ---- the command ------------------------------------------------------------------------- */

static void out_of_memory(tool *t) {
    t->exit = TOOL_EXIT_FAILURE;
    tool_complain(t, ta_string("out of memory"));
}

void tool_lint(tool *t) {
    uint8_t denied[TA_ANALYSIS_RULE_COUNT], allowed[TA_ANALYSIS_RULE_COUNT];
    if (rules_given(t, TOOL_LINT_DENY, denied) != TA_OK || rules_given(t, TOOL_LINT_ALLOW, allowed) != TA_OK) return;
    tool_opened opened;
    tool_open_request open = {t->parsed->argument_count, t->parsed->arguments, 0};
    if (tool_open(t, open, &opened) != TA_OK) return;
    const ta_image_view *view = view_of(&opened);
    uint32_t flags = opened.image.kind == TOOL_INPUT_MANIFEST || view->chart_count > 1 ? TA_ANALYSIS_SYSTEM : 0u;
    ta_analysis_request request = {view, opened.charts.selected, flags, 0, t->arena};
    ta_analysis_findings findings;
    if (ta_analysis_lint(&request, &findings) != TA_OK) {
        out_of_memory(t);
        return;
    }
    uint32_t counts[2] = {0, 0}, failing = 0;
    for (uint32_t i = 0; i < findings.count; i++) {
        const ta_analysis_finding *finding = &findings.list[i];
        if (allowed[finding->rule]) continue;
        counts[finding->severity]++;
        if (denied[finding->rule]) failing++;
    }
    ta_report *report = &t->report;
    ta_report_number(report, ta_string("warnings"), counts[TA_ANALYSIS_WARNING]);
    ta_report_number(report, ta_string("notes"), counts[TA_ANALYSIS_NOTE]);
    ta_report_number(report, ta_string("denied"), failing);
    ta_report_list(report, (ta_report_list_names){ta_string("findings"), ta_string("finding")});
    for (uint32_t i = 0; i < findings.count; i++) {
        const ta_analysis_finding *finding = &findings.list[i];
        if (allowed[finding->rule]) continue;
        finding_words words = {t, view, finding, {{0}, {0}}};
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("rule"), ta_analysis_rule_name(finding->rule));
        ta_report_text(report, ta_string("severity"),
                       finding->severity == TA_ANALYSIS_WARNING ? ta_string("warning") : ta_string("note"));
        ta_report_text(report, ta_string("chart"), string_of(view, chart_of(view, finding->chart)->name));
        ta_report_text(report, ta_string("state"), state_of(&words));
        ta_report_text(report, ta_string("transition"), transition_text(t, view, finding));
        ta_report_text(report, ta_string("event"), event_name(view, finding->event));
        ta_report_number(report, ta_string("line"), finding->line);
        ta_report_text(report, ta_string("message"), message(&words));
        ta_report_end(report);
    }
    ta_report_end(report);
    if (opened.image.diagnostic_count) tool_report_diagnostics(t, &opened.image);
    if (failing) t->exit = TOOL_EXIT_REFUSED;
}
