/* inspect.c — tinyactors inspect, states, transitions, events, data and
 * expressions: what an image holds, read from its records (tool.h).
 * Freestanding. */
#include "tool.h"

#define T TA_CLI_TEXT

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
    return tool_state_id(view, chart, state);
}

/* An expression's text ("" for none). */
static ta_string_view expression_text(const ta_image_view *view, const ta_chart_record *chart,
                                      uint32_t expression) {
    if (expression == TA_NIL || expression >= chart->expression_count) return ta_string("");
    return string_of(view, view->expressions[chart->first_expression + expression].source);
}

/* A transition's event descriptors, separated by spaces ("" when eventless;
 * "*" for the descriptor that matches every event). */
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

/* A transition's target states' ids, separated by spaces. */
static ta_string_view transition_targets(tool *t, const ta_image_view *view, const ta_chart_record *chart,
                                         const ta_transition_record *transition) {
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t i = 0; i < transition->target_count; i++) {
        if (i) tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, state_id(view, chart, view->u32[transition->targets_offset + i]));
    }
    return tool_text_view(&text);
}

static void open_list(tool *t, const char *name, const char *item) {
    ta_report_list(&t->report, (ta_report_list_names){ta_string(name), ta_string(item)});
}

/* A list "charts" of the selected charts, each an object with its name;
 * `each` adds the rest of the chart's members. */
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

static void diagnostics_if_any(tool *t, const tool_opened *opened) {
    if (opened->image.diagnostic_count) tool_report_diagnostics(t, &opened->image);
}

/* ---- inspect ------------------------------------------------------------------------- */

static const ta_string_view section_names[TA_SECTION_MAX_] = {
    [TA_SECTION_STRINGS] = T("strings"),         [TA_SECTION_CHARTS] = T("charts"),
    [TA_SECTION_STATES] = T("states"),           [TA_SECTION_TRANSITIONS] = T("transitions"),
    [TA_SECTION_U32] = T("words"),               [TA_SECTION_CODE] = T("code"),
    [TA_SECTION_EXPRESSIONS] = T("expressions"), [TA_SECTION_CAPTURES] = T("captures"),
    [TA_SECTION_SENDS] = T("sends"),             [TA_SECTION_INVOKES] = T("invokes"),
    [TA_SECTION_DATA] = T("data"),               [TA_SECTION_EVENT_NODES] = T("eventNames"),
    [TA_SECTION_ACTORS] = T("bootActors"),
};

static ta_string_view kind_name(uint32_t kind) {
    return kind == TOOL_INPUT_IMAGE ? ta_string("image")
           : kind == TOOL_INPUT_MANIFEST ? ta_string("system manifest")
                                         : ta_string("charts");
}

void tool_inspect(tool *t) {
    if (tool_inspect_stored(t)) return; /* a snapshot or a blob (snapshot.c) */
    tool_opened opened;
    if (tool_open(t, arguments_of(t), &opened) != TA_OK) return;
    const ta_image_view *view = view_of(&opened);
    const ta_header_record *header = (const ta_header_record *)(const void *)view->base;
    ta_report *report = &t->report;
    ta_report_text(report, ta_string("from"), kind_name(opened.image.kind));
    ta_report_number(report, ta_string("bytes"), view->size);
    ta_report_number(report, ta_string("format"), header->version);
    ta_report_text(report, ta_string("hash"), tool_hexadecimal(t, view->hash));
    ta_report_number(report, ta_string("charts"), view->chart_count);
    ta_report_number(report, ta_string("bootActors"), view->actor_count);
    ta_report_number(report, ta_string("eventNames"), view->event_node_count);
    open_list(t, "sections", "section");
    const ta_section_record *sections = (const ta_section_record *)(const void *)(view->base + sizeof *header);
    for (uint32_t i = 0; i < header->section_count; i++) {
        uint32_t kind = sections[i].kind;
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("section"),
                       kind < TA_SECTION_MAX_ && section_names[kind].length ? section_names[kind]
                                                                             : ta_string("unknown"));
        ta_report_number(report, ta_string("offset"), sections[i].offset);
        ta_report_number(report, ta_string("bytes"), sections[i].size);
        ta_report_number(report, ta_string("records"), sections[i].count);
        ta_report_end(report);
    }
    ta_report_end(report);
    open_list(t, "charts", "chart");
    for (uint32_t c = 0; c < view->chart_count; c++) {
        if (!opened.charts.selected[c]) continue;
        const ta_chart_record *chart = &view->charts[c];
        ta_report_object(report, ta_string(""));
        ta_report_number(report, ta_string("index"), c);
        ta_report_text(report, ta_string("name"), string_of(view, chart->name));
        ta_report_text(report, ta_string("source"), string_of(view, chart->source));
        ta_report_text(report, ta_string("datamodel"), string_of(view, chart->datamodel));
        ta_report_text(report, ta_string("binding"), chart->binding ? ta_string("late") : ta_string("early"));
        ta_report_number(report, ta_string("states"), chart->state_count);
        ta_report_number(report, ta_string("transitions"), chart->transition_count);
        ta_report_number(report, ta_string("data"), chart->data_count);
        ta_report_number(report, ta_string("expressions"), chart->expression_count);
        ta_report_number(report, ta_string("sends"), chart->send_count);
        ta_report_number(report, ta_string("invokes"), chart->invoke_count);
        ta_report_number(report, ta_string("codeWords"), chart->code_count);
        ta_report_end(report);
    }
    ta_report_end(report);
    open_list(t, "bootActors", "actor");
    for (uint32_t i = 0; i < view->actor_count; i++) {
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("name"), string_of(view, view->actors[i].name));
        ta_report_text(report, ta_string("chart"), string_of(view, view->charts[view->actors[i].chart].name));
        ta_report_end(report);
    }
    ta_report_end(report);
    diagnostics_if_any(t, &opened);
}

/* ---- states ---------------------------------------------------------------------------- */

static void state_members(tool *t, const tool_opened *opened, uint32_t c) {
    const ta_image_view *view = view_of(opened);
    const ta_chart_record *chart = &view->charts[c];
    ta_report *report = &t->report;
    open_list(t, "states", "state");
    for (uint32_t s = 0; s < chart->state_count; s++) {
        const ta_state_record *state = &view->states[chart->first_state + s];
        ta_report_object(report, ta_string(""));
        ta_report_number(report, ta_string("index"), s);
        ta_report_text(report, ta_string("id"), state_id(view, chart, s));
        ta_report_text(report, ta_string("kind"), tool_state_kind_name(state->kind));
        ta_report_text(report, ta_string("parent"), state_id(view, chart, state->parent));
        ta_report_number(report, ta_string("depth"), state->depth);
        ta_string_view initial = ta_string("");
        if (state->initial != TA_NIL && state->initial < chart->transition_count)
            initial = transition_targets(t, view, chart, &view->transitions[chart->first_transition + state->initial]);
        ta_report_text(report, ta_string("initial"), initial);
        ta_report_number(report, ta_string("transitions"), state->transition_count);
        ta_report_number(report, ta_string("invokes"), state->invoke_count);
        ta_report_number(report, ta_string("data"), state->data_count);
        ta_report_boolean(report, ta_string("onEntry"), state->onentry != TA_NIL);
        ta_report_boolean(report, ta_string("onExit"), state->onexit != TA_NIL);
        ta_report_text(report, ta_string("doneEvent"),
                       state->done_event != TA_NIL && state->done_event < view->event_node_count
                           ? string_of(view, view->event_nodes[state->done_event].name)
                           : ta_string(""));
        ta_report_end(report);
    }
    ta_report_end(report);
}

void tool_states(tool *t) {
    tool_opened opened;
    if (tool_open(t, arguments_of(t), &opened) != TA_OK) return;
    each_chart(t, &opened, state_members);
    diagnostics_if_any(t, &opened);
}

/* ---- transitions ------------------------------------------------------------------------- */

static ta_string_view transition_kind(const ta_transition_record *transition, const ta_state_record *source) {
    if (!(transition->flags & TA_TRANSITION_FLAG_INITIAL)) return ta_string("transition");
    return source->kind >= TA_STATE_HISTORY_SHALLOW ? ta_string("history default") : ta_string("initial");
}

static void transition_members(tool *t, const tool_opened *opened, uint32_t c) {
    const ta_image_view *view = view_of(opened);
    const ta_chart_record *chart = &view->charts[c];
    ta_report *report = &t->report;
    open_list(t, "transitions", "transition");
    for (uint32_t i = 0; i < chart->transition_count; i++) {
        const ta_transition_record *transition = &view->transitions[chart->first_transition + i];
        const ta_state_record *source = &view->states[chart->first_state + transition->source];
        ta_report_object(report, ta_string(""));
        ta_report_number(report, ta_string("index"), i);
        ta_report_text(report, ta_string("kind"), transition_kind(transition, source));
        ta_report_text(report, ta_string("source"), state_id(view, chart, transition->source));
        ta_report_text(report, ta_string("events"), transition_events(t, view, transition));
        ta_report_text(report, ta_string("condition"), expression_text(view, chart, transition->condition));
        ta_report_text(report, ta_string("targets"), transition_targets(t, view, chart, transition));
        ta_report_text(report, ta_string("type"), transition->flags & TA_TRANSITION_FLAG_INTERNAL
                                                      ? ta_string("internal")
                                                      : ta_string("external"));
        ta_report_boolean(report, ta_string("actions"), transition->action != TA_NIL);
        ta_report_end(report);
    }
    ta_report_end(report);
}

void tool_transitions(tool *t) {
    tool_opened opened;
    if (tool_open(t, arguments_of(t), &opened) != TA_OK) return;
    each_chart(t, &opened, transition_members);
    diagnostics_if_any(t, &opened);
}

/* ---- events ------------------------------------------------------------------------------ */

/* How the selected charts use each event name: the transitions whose
 * descriptors name it, and the raises and sends that make it by name
 * (an eventexpr's events are known only when it runs). */
typedef struct event_uses {
    uint32_t transitions, raises, sends;
} event_uses;

static void count_uses(const tool_opened *opened, event_uses *uses) {
    const ta_image_view *view = view_of(opened);
    for (uint32_t c = 0; c < view->chart_count; c++) {
        if (!opened->charts.selected[c]) continue;
        const ta_chart_record *chart = &view->charts[c];
        for (uint32_t i = 0; i < chart->transition_count; i++) {
            const ta_transition_record *transition = &view->transitions[chart->first_transition + i];
            for (uint32_t e = 0; e < transition->event_count; e++) {
                uint32_t node = view->u32[transition->events_offset + 2 * e];
                if (node < view->event_node_count) uses[node].transitions++;
            }
        }
        for (uint32_t s = 0; s < chart->send_count; s++) {
            uint32_t node = view->sends[chart->first_send + s].event;
            if (node < view->event_node_count) uses[node].sends++;
        }
        uint32_t end = chart->first_code + chart->code_count;
        for (uint32_t pc = chart->first_code; pc < end;) {
            uint32_t opcode = view->code[pc];
            if (opcode >= TA_OPCODE_MAX_) break; /* verified images hold none */
            if (opcode == TA_OPCODE_RAISE && view->code[pc + 1] < view->event_node_count)
                uses[view->code[pc + 1]].raises++;
            pc += ta_opcode_length[opcode];
        }
    }
}

void tool_events(tool *t) {
    tool_opened opened;
    if (tool_open(t, arguments_of(t), &opened) != TA_OK) return;
    const ta_image_view *view = view_of(&opened);
    event_uses *uses = (event_uses *)ta_growable_arena_alloc(t->arena, sizeof *uses * (view->event_node_count + 1u), 8);
    if (!uses) {
        t->exit = TOOL_EXIT_FAILURE;
        tool_complain(t, ta_string("out of memory"));
        return;
    }
    for (uint32_t i = 0; i < view->event_node_count; i++)
        uses[i] = (event_uses){0, 0, 0};
    count_uses(&opened, uses);
    open_list(t, "events", "event");
    for (uint32_t i = 0; i < view->event_node_count; i++) {
        /* The image keeps every prefix of a name ("order" of "order.paid")
         * and the done events of every state: only names something uses. */
        if (!uses[i].transitions && !uses[i].raises && !uses[i].sends) continue;
        ta_string_view name = string_of(view, view->event_nodes[i].name);
        ta_report_object(&t->report, ta_string(""));
        ta_report_text(&t->report, ta_string("name"), name.length ? name : ta_string("*"));
        ta_report_number(&t->report, ta_string("transitions"), uses[i].transitions);
        ta_report_number(&t->report, ta_string("raises"), uses[i].raises);
        ta_report_number(&t->report, ta_string("sends"), uses[i].sends);
        ta_report_end(&t->report);
    }
    ta_report_end(&t->report);
    diagnostics_if_any(t, &opened);
}

/* ---- data ---------------------------------------------------------------------------------- */

static void data_members(tool *t, const tool_opened *opened, uint32_t c) {
    const ta_image_view *view = view_of(opened);
    const ta_chart_record *chart = &view->charts[c];
    ta_report *report = &t->report;
    open_list(t, "data", "datum");
    for (uint32_t i = 0; i < chart->data_count; i++) {
        const ta_data_record *data = &view->data[chart->first_data + i];
        ta_report_object(report, ta_string(""));
        ta_report_text(report, ta_string("id"), string_of(view, data->id));
        ta_report_text(report, ta_string("state"), state_id(view, chart, data->state));
        ta_report_text(report, ta_string("expression"), expression_text(view, chart, data->expression));
        uint32_t line = data->expression != TA_NIL && data->expression < chart->expression_count
                            ? view->expressions[chart->first_expression + data->expression].line
                            : 0;
        ta_report_number(report, ta_string("line"), line);
        ta_report_end(report);
    }
    ta_report_end(report);
}

void tool_data(tool *t) {
    tool_opened opened;
    if (tool_open(t, arguments_of(t), &opened) != TA_OK) return;
    each_chart(t, &opened, data_members);
    diagnostics_if_any(t, &opened);
}

/* ---- expressions ---------------------------------------------------------------------------- */

static int kind_wanted(const tool *t, uint32_t kind) {
    if (!tool_count(t, TOOL_EXPRESSIONS_KIND)) return 1;
    for (uint32_t i = 0; i < t->parsed->value_count; i++) {
        const ta_cli_value *value = &t->parsed->values[i];
        if (value->option.scope == TA_CLI_SCOPE_COMMAND && value->option.option == TOOL_EXPRESSIONS_KIND &&
            value->choice + TA_EXPRESSION_CONDITION == kind)
            return 1;
    }
    return 0;
}

static void expression_members(tool *t, const tool_opened *opened, uint32_t c) {
    const ta_image_view *view = view_of(opened);
    const ta_chart_record *chart = &view->charts[c];
    ta_report *report = &t->report;
    open_list(t, "expressions", "expression");
    for (uint32_t i = 0; i < chart->expression_count; i++) {
        const ta_expression_record *expression = &view->expressions[chart->first_expression + i];
        if (!kind_wanted(t, expression->kind)) continue;
        ta_report_object(report, ta_string(""));
        ta_report_number(report, ta_string("index"), i);
        ta_report_text(report, ta_string("kind"), tool_expression_kind_name(expression->kind));
        ta_report_number(report, ta_string("line"), expression->line);
        ta_report_number(report, ta_string("column"), expression->column);
        ta_report_text(report, ta_string("source"), string_of(view, expression->source));
        ta_report_end(report);
    }
    ta_report_end(report);
}

void tool_expressions(tool *t) {
    tool_opened opened;
    if (tool_open(t, arguments_of(t), &opened) != TA_OK) return;
    each_chart(t, &opened, expression_members);
    diagnostics_if_any(t, &opened);
}
