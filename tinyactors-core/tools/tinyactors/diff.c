/* diff.c — tinyactors diff: what changed between two versions of an image,
 * chart by chart, and what a migration does with it (tool.h).
 * Freestanding. */
#include "tool.h"

static ta_string_view string_of(const ta_image_view *view, uint32_t id) {
    return ta_image_view_string(view, id);
}

static void open_list(tool *t, const char *name, const char *item) {
    ta_report_list(&t->report, (ta_report_list_names){ta_string(name), ta_string(item)});
}

/* A chart of one side: `chart` is null when that side has no chart of the
 * name. */
typedef struct chart_view {
    const ta_image_view *view;
    const ta_chart_record *chart;
} chart_view;

/* The same chart on both sides. */
typedef struct chart_pair {
    chart_view before, after;
} chart_pair;

/* What one side has that the other lacks, written as text. */
typedef struct comparison {
    tool *t;
    const chart_view *from; /* the side whose items are listed */
    const chart_view *to;   /* the side they are looked for in */
} comparison;

static uint32_t state_count_of(const chart_view *side) {
    return side->chart ? side->chart->state_count : 0;
}

static const ta_state_record *state_of(const chart_view *side, uint32_t state) {
    return &side->view->states[side->chart->first_state + state];
}

/* A state's id; "" for the root and for none. */
static ta_string_view state_id(const chart_view *side, uint32_t state) {
    if (!side->chart || state == TA_NIL || state >= side->chart->state_count) return ta_string("");
    return string_of(side->view, state_of(side, state)->id);
}

/* The state whose id is `id`, or TA_NIL (the root's empty id is never found). */
static uint32_t state_named(const chart_view *side, ta_string_view id) {
    if (!id.length) return TA_NIL;
    for (uint32_t s = 0; s < state_count_of(side); s++)
        if (tool_equal(state_id(side, s), id)) return s;
    return TA_NIL;
}

static int has_data(const chart_view *side, ta_string_view id) {
    if (!side->chart) return 0;
    for (uint32_t i = 0; i < side->chart->data_count; i++)
        if (tool_equal(string_of(side->view, side->view->data[side->chart->first_data + i].id), id)) return 1;
    return 0;
}

static ta_string_view expression_text(const chart_view *side, uint32_t expression) {
    if (expression == TA_NIL || expression >= side->chart->expression_count) return ta_string("");
    return string_of(side->view, side->view->expressions[side->chart->first_expression + expression].source);
}

static const ta_transition_record *transition_of(const chart_view *side, uint32_t index) {
    return &side->view->transitions[side->chart->first_transition + index];
}

/* The full name of a transition's descriptor `index` ("*": every event). */
static ta_string_view descriptor(const chart_view *side, const ta_transition_record *transition, uint32_t index) {
    uint32_t node = side->view->u32[transition->events_offset + 2 * index];
    ta_string_view name =
        node < side->view->event_node_count ? string_of(side->view, side->view->event_nodes[node].name) : ta_string("");
    return name.length ? name : ta_string("*");
}

/* Whether a transition of the chart names `name`. */
static int handles(const chart_view *side, ta_string_view name) {
    if (!side->chart) return 0;
    for (uint32_t i = 0; i < side->chart->transition_count; i++) {
        const ta_transition_record *transition = transition_of(side, i);
        for (uint32_t e = 0; e < transition->event_count; e++)
            if (tool_equal(descriptor(side, transition, e), name)) return 1;
    }
    return 0;
}

static void separate(tool_text *text, const char *separator) {
    if (ta_output_fits(&text->output) && text->output.length) tool_text_append(text, ta_string(separator));
}

/* ---- what one side has that the other lacks ------------------------------------------------ */

static ta_string_view states_only_in(const comparison *pair) {
    tool_text text;
    tool_text_init(pair->t, &text);
    for (uint32_t s = 0; s < state_count_of(pair->from); s++) {
        ta_string_view id = state_id(pair->from, s);
        if (!id.length || state_named(pair->to, id) != TA_NIL) continue;
        separate(&text, " ");
        tool_text_append(&text, id);
    }
    return tool_text_view(&text);
}

static ta_string_view data_only_in(const comparison *pair) {
    tool_text text;
    tool_text_init(pair->t, &text);
    const chart_view *from = pair->from;
    for (uint32_t i = 0; from->chart && i < from->chart->data_count; i++) {
        ta_string_view id = string_of(from->view, from->view->data[from->chart->first_data + i].id);
        if (has_data(pair->to, id)) continue;
        separate(&text, " ");
        tool_text_append(&text, id);
    }
    return tool_text_view(&text);
}

/* Whether descriptor `e` of transition `index` names an event an earlier
 * descriptor of the chart named. */
typedef struct descriptor_at {
    uint32_t transition, event;
} descriptor_at;
static int named_earlier(const chart_view *side, descriptor_at at, ta_string_view name) {
    for (uint32_t i = 0; i <= at.transition; i++) {
        const ta_transition_record *transition = transition_of(side, i);
        uint32_t end = i == at.transition ? at.event : transition->event_count;
        for (uint32_t e = 0; e < end; e++)
            if (tool_equal(descriptor(side, transition, e), name)) return 1;
    }
    return 0;
}

static ta_string_view events_only_in(const comparison *pair) {
    tool_text text;
    tool_text_init(pair->t, &text);
    const chart_view *from = pair->from;
    for (uint32_t i = 0; from->chart && i < from->chart->transition_count; i++) {
        const ta_transition_record *transition = transition_of(from, i);
        for (uint32_t e = 0; e < transition->event_count; e++) {
            ta_string_view name = descriptor(from, transition, e);
            if (handles(pair->to, name) || named_earlier(from, (descriptor_at){i, e}, name)) continue;
            separate(&text, " ");
            tool_text_append(&text, name);
        }
    }
    return tool_text_view(&text);
}

/* "id: oldkind→newkind" for the states both sides have, "; "-joined. */
static ta_string_view kinds_changed(tool *t, const chart_pair *pair) {
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t s = 0; s < state_count_of(&pair->before); s++) {
        ta_string_view id = state_id(&pair->before, s);
        uint32_t other = state_named(&pair->after, id);
        if (other == TA_NIL) continue;
        uint32_t before = state_of(&pair->before, s)->kind, after = state_of(&pair->after, other)->kind;
        if (before == after) continue;
        separate(&text, "; ");
        tool_text_append(&text, id);
        tool_text_append(&text, ta_string(": "));
        tool_text_append(&text, tool_state_kind_name(before));
        tool_text_append(&text, ta_string("\xe2\x86\x92"));
        tool_text_append(&text, tool_state_kind_name(after));
    }
    return tool_text_view(&text);
}

/* ---- what changed without adding or removing ------------------------------------------------- */

static int parents_differ(const chart_pair *pair) {
    for (uint32_t s = 0; s < state_count_of(&pair->before); s++) {
        uint32_t other = state_named(&pair->after, state_id(&pair->before, s));
        if (other == TA_NIL) continue;
        if (!tool_equal(state_id(&pair->before, state_of(&pair->before, s)->parent),
                        state_id(&pair->after, state_of(&pair->after, other)->parent)))
            return 1;
    }
    return 0;
}

static int transition_differs(const chart_pair *pair, uint32_t index) {
    const ta_transition_record *before = transition_of(&pair->before, index);
    const ta_transition_record *after = transition_of(&pair->after, index);
    if (before->event_count != after->event_count || before->target_count != after->target_count ||
        (before->flags & TA_TRANSITION_FLAG_INITIAL) != (after->flags & TA_TRANSITION_FLAG_INITIAL))
        return 1;
    if (!tool_equal(state_id(&pair->before, before->source), state_id(&pair->after, after->source))) return 1;
    if (!tool_equal(expression_text(&pair->before, before->condition), expression_text(&pair->after, after->condition)))
        return 1;
    for (uint32_t e = 0; e < before->event_count; e++)
        if (!tool_equal(descriptor(&pair->before, before, e), descriptor(&pair->after, after, e))) return 1;
    for (uint32_t i = 0; i < before->target_count; i++)
        if (!tool_equal(state_id(&pair->before, pair->before.view->u32[before->targets_offset + i]),
                        state_id(&pair->after, pair->after.view->u32[after->targets_offset + i])))
            return 1;
    return 0;
}

static int transitions_differ(const chart_pair *pair) {
    if (pair->before.chart->transition_count != pair->after.chart->transition_count) return 1;
    for (uint32_t i = 0; i < pair->before.chart->transition_count; i++)
        if (transition_differs(pair, i)) return 1;
    return 0;
}

/* ---- one chart ------------------------------------------------------------------------------ */

typedef struct chart_findings {
    ta_string_view states_added, states_removed, kinds, data_added, data_removed, events_added, events_removed;
} chart_findings;

static chart_findings find(tool *t, const chart_pair *pair) {
    comparison forward = {t, &pair->after, &pair->before};
    comparison backward = {t, &pair->before, &pair->after};
    chart_findings findings;
    findings.states_added = states_only_in(&forward);
    findings.states_removed = states_only_in(&backward);
    findings.kinds = kinds_changed(t, pair);
    findings.data_added = data_only_in(&forward);
    findings.data_removed = data_only_in(&backward);
    findings.events_added = events_only_in(&forward);
    findings.events_removed = events_only_in(&backward);
    return findings;
}

static ta_string_view change_of(const chart_pair *pair, const chart_findings *findings) {
    if (!pair->before.chart) return ta_string("added");
    if (!pair->after.chart) return ta_string("removed");
    int changed = findings->states_added.length || findings->states_removed.length || findings->kinds.length ||
                  findings->data_added.length || findings->data_removed.length ||
                  findings->events_added.length || findings->events_removed.length ||
                  !tool_equal(string_of(pair->before.view, pair->before.chart->datamodel),
                              string_of(pair->after.view, pair->after.chart->datamodel)) ||
                  parents_differ(pair) || transitions_differ(pair);
    return changed ? ta_string("changed") : ta_string("unchanged");
}

/* What the report's charts list and the migration text are built with. */
typedef struct diff_state {
    tool *t;
    tool_text migration; /* "<chart>: <states>; …" of the charts that lost states */
} diff_state;

static void chart_item(diff_state *diff, const chart_pair *pair, ta_string_view name) {
    tool *t = diff->t;
    ta_report *report = &t->report;
    chart_findings findings = find(t, pair);
    ta_string_view change = change_of(pair, &findings);
    const chart_view *latest = pair->after.chart ? &pair->after : &pair->before;
    ta_report_object(report, ta_string(""));
    ta_report_text(report, ta_string("name"), name);
    ta_report_text(report, ta_string("change"), change);
    ta_report_text(report, ta_string("datamodel"), string_of(latest->view, latest->chart->datamodel));
    ta_report_text(report, ta_string("statesAdded"), findings.states_added);
    ta_report_text(report, ta_string("statesRemoved"), findings.states_removed);
    ta_report_text(report, ta_string("kindsChanged"), findings.kinds);
    ta_report_text(report, ta_string("dataAdded"), findings.data_added);
    ta_report_text(report, ta_string("dataRemoved"), findings.data_removed);
    ta_report_number(report, ta_string("transitionsBefore"),
                     pair->before.chart ? pair->before.chart->transition_count : 0);
    ta_report_number(report, ta_string("transitionsAfter"),
                     pair->after.chart ? pair->after.chart->transition_count : 0);
    ta_report_text(report, ta_string("eventsAdded"), findings.events_added);
    ta_report_text(report, ta_string("eventsRemoved"), findings.events_removed);
    ta_report_end(report);
    /* an actor migrates only between two versions of its chart */
    if (pair->before.chart && pair->after.chart && findings.states_removed.length) {
        separate(&diff->migration, "; ");
        tool_text_append(&diff->migration, name);
        tool_text_append(&diff->migration, ta_string(": "));
        tool_text_append(&diff->migration, findings.states_removed);
    }
}

/* ---- the two images -------------------------------------------------------------------------- */

typedef struct diff_side {
    ta_string_view path;
    tool_opened opened;
} diff_side;

static const ta_image_view *view_of(const diff_side *side) {
    return ta_image_view_of(&side->opened.image.image);
}

static void side_object(tool *t, const char *name, const diff_side *side) {
    const ta_image_view *view = view_of(side);
    ta_report_object(&t->report, ta_string(name));
    ta_report_text(&t->report, ta_string("path"), side->path);
    ta_report_text(&t->report, ta_string("hash"), tool_hexadecimal(t, view->hash));
    ta_report_number(&t->report, ta_string("charts"), view->chart_count);
    if (side->opened.image.diagnostic_count) tool_report_diagnostics(t, &side->opened.image);
    ta_report_end(&t->report);
}

/* The chart of the name on a side, and whether --chart lets it count. */
typedef struct chart_lookup {
    chart_view found;
    int counts;
} chart_lookup;

static chart_lookup look_up(const diff_side *side, ta_string_view name) {
    const ta_image_view *view = view_of(side);
    uint32_t chart = ta_image_chart_by_name(&side->opened.image.image, name);
    chart_lookup lookup = {{view, 0}, 1};
    if (chart == TA_NIL || chart >= view->chart_count) return lookup;
    lookup.found.chart = &view->charts[chart];
    lookup.counts = side->opened.charts.selected[chart] != 0;
    return lookup;
}

typedef struct diff_sides {
    const diff_side *before, *after;
} diff_sides;

static void chart_by_name(diff_state *diff, const diff_sides *sides, ta_string_view name) {
    chart_lookup before = look_up(sides->before, name), after = look_up(sides->after, name);
    if (!before.counts || !after.counts) return;
    chart_pair pair = {before.found, after.found};
    chart_item(diff, &pair, name);
}

static void charts_list(diff_state *diff, const diff_sides *sides) {
    const ta_image_view *before = view_of(sides->before), *after = view_of(sides->after);
    open_list(diff->t, "charts", "chart");
    for (uint32_t c = 0; c < before->chart_count; c++)
        chart_by_name(diff, sides, string_of(before, before->charts[c].name));
    for (uint32_t c = 0; c < after->chart_count; c++) {
        ta_string_view name = string_of(after, after->charts[c].name);
        if (ta_image_chart_by_name(&sides->before->opened.image.image, name) == TA_NIL)
            chart_by_name(diff, sides, name);
    }
    ta_report_end(&diff->t->report);
}

/* The image-wide event names `from` has and `to` lacks, as a list. */
static void events_list(tool *t, const char *name, diff_sides sides) {
    const ta_image_view *from = view_of(sides.before), *to = view_of(sides.after);
    open_list(t, name, "event");
    for (uint32_t i = 0; i < from->event_node_count; i++) {
        ta_string_view event = string_of(from, from->event_nodes[i].name);
        if (!event.length) continue;
        int exact = 0;
        ta_event_name_lookup(to, event.pointer, event.length, &exact);
        if (!exact) ta_report_text(&t->report, ta_string(""), event);
    }
    ta_report_end(&t->report);
}

/* Each --chart name is a chart of one image or the other. */
static int names_exist(tool *t, diff_sides sides) {
    for (uint32_t i = 0; i < t->parsed->value_count; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        if (value->option.scope != TA_CLI_SCOPE_COMMAND || value->option.option != TOOL_OPTION_CHART) continue;
        if (ta_image_chart_by_name(&sides.before->opened.image.image, value->text) != TA_NIL ||
            ta_image_chart_by_name(&sides.after->opened.image.image, value->text) != TA_NIL)
            continue;
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, ta_string("no chart '"));
        tool_text_append(&text, value->text);
        tool_text_append(&text, ta_string("' in either image"));
        tool_complain(t, tool_text_view(&text));
        t->exit = TOOL_EXIT_USAGE;
        return 0;
    }
    return 1;
}

void tool_diff(tool *t) {
    const ta_string_view *arguments = t->parsed->arguments;
    diff_side before = {.path = arguments[0]}, after = {.path = arguments[1]};
    if (tool_open(t, (tool_open_request){1, &arguments[0], TOOL_SELECT_NAMES_ELSEWHERE}, &before.opened) != TA_OK)
        return;
    if (tool_open(t, (tool_open_request){1, &arguments[1], TOOL_SELECT_NAMES_ELSEWHERE}, &after.opened) != TA_OK)
        return;
    if (!names_exist(t, (diff_sides){&before, &after})) return;
    ta_report *report = &t->report;
    int identical = view_of(&before)->hash == view_of(&after)->hash;
    side_object(t, "old", &before);
    side_object(t, "new", &after);
    ta_report_boolean(report, ta_string("identical"), identical);
    diff_state diff;
    diff.t = t;
    tool_text_init(t, &diff.migration);
    diff_sides sides = {&before, &after};
    charts_list(&diff, &sides);
    events_list(t, "eventsAdded", (diff_sides){&after, &before});
    events_list(t, "eventsRemoved", (diff_sides){&before, &after});
    ta_string_view states = tool_text_view(&diff.migration);
    if (states.length) {
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, ta_string("An actor migrated from the old version keeps its state by id; one in a "
                                          "state the new version removed is completed to a legal configuration of "
                                          "the new version: "));
        tool_text_append(&text, states);
        ta_report_text(report, ta_string("migration"), tool_text_view(&text));
    }
    if (tool_count(t, TOOL_DIFF_EXIT_CODE) && !identical) t->exit = TOOL_EXIT_REFUSED;
}
