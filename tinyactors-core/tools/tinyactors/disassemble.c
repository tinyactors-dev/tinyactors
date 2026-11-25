/* disassemble.c — tinyactors disassemble: each selected chart's executable
 * content, one instruction per item, with the entry points that start at
 * it and its operands as text (tool.h). Freestanding. */
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
    return id.length ? id : ta_string("/scxml"); /* the root: its XPath, never an id */
}

static ta_string_view expression_text(const ta_image_view *view, const ta_chart_record *chart,
                                      uint32_t expression) {
    if (expression == TA_NIL || expression >= chart->expression_count) return ta_string("");
    return string_of(view, view->expressions[chart->first_expression + expression].source);
}

static ta_string_view event_name(const ta_image_view *view, uint32_t node) {
    if (node == TA_NIL || node >= view->event_node_count) return ta_string("");
    ta_string_view name = string_of(view, view->event_nodes[node].name);
    return name.length ? name : ta_string("*");
}

static ta_string_view transition_events(tool *t, const ta_image_view *view,
                                        const ta_transition_record *transition) {
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t i = 0; i < transition->event_count; i++) {
        if (i) tool_text_append(&text, ta_string(" "));
        tool_text_append(&text, event_name(view, view->u32[transition->events_offset + 2 * i]));
    }
    return tool_text_view(&text);
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

/* ---- the entry points ------------------------------------------------------------------ */

/* One label per word of the chart's code (chart-relative), "" where no
 * entry point starts. */
typedef struct entry_labels {
    tool *t;
    const ta_chart_record *chart;
    ta_string_view *labels;
} entry_labels;

static void add_label(entry_labels *entries, uint32_t pc, ta_string_view label) {
    const ta_chart_record *chart = entries->chart;
    if (pc == TA_NIL || pc < chart->first_code || pc - chart->first_code >= chart->code_count) return;
    ta_string_view *slot = &entries->labels[pc - chart->first_code];
    if (!slot->length) {
        *slot = label;
        return;
    }
    tool_text text;
    tool_text_init(entries->t, &text);
    tool_text_append(&text, *slot);
    tool_text_append(&text, ta_string("; "));
    tool_text_append(&text, label);
    *slot = tool_text_view(&text);
}

/* "<words><id>" in the arena. */
static ta_string_view prefixed(tool *t, const char *words, ta_string_view id) {
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, ta_string(words));
    tool_text_append(&text, id);
    return tool_text_view(&text);
}

static void label_transitions(entry_labels *entries, const ta_image_view *view) {
    const ta_chart_record *chart = entries->chart;
    for (uint32_t i = 0; i < chart->transition_count; i++) {
        const ta_transition_record *transition = &view->transitions[chart->first_transition + i];
        if (transition->action == TA_NIL) continue;
        ta_string_view source = state_id(view, chart, transition->source);
        if (transition->flags & TA_TRANSITION_FLAG_INITIAL) {
            add_label(entries, transition->action, prefixed(entries->t, "initial of ", source));
            continue;
        }
        tool_text text;
        tool_text_init(entries->t, &text);
        tool_text_append(&text, ta_string("transition "));
        tool_text_number(&text, i);
        tool_text_append(&text, ta_string(": "));
        tool_text_append(&text, source);
        tool_text_append(&text, ta_string(" on "));
        ta_string_view events = transition_events(entries->t, view, transition);
        tool_text_append(&text, events.length ? events : ta_string("no event"));
        add_label(entries, transition->action, tool_text_view(&text));
    }
}

static void label_invokes(entry_labels *entries, const ta_image_view *view) {
    const ta_chart_record *chart = entries->chart;
    for (uint32_t i = 0; i < chart->invoke_count; i++) {
        const ta_invoke_record *invoke = &view->invokes[chart->first_invoke + i];
        if (invoke->finalize == TA_NIL) continue;
        tool_text text;
        tool_text_init(entries->t, &text);
        tool_text_append(&text, ta_string("finalize "));
        ta_string_view id = string_of(view, invoke->id);
        if (id.length) {
            tool_text_append(&text, id);
        } else {
            tool_text_append(&text, ta_string("#"));
            tool_text_number(&text, i);
        }
        add_label(entries, invoke->finalize, tool_text_view(&text));
    }
}

static ta_status label_entries(entry_labels *entries, const ta_image_view *view) {
    const ta_chart_record *chart = entries->chart;
    entries->labels = (ta_string_view *)ta_growable_arena_alloc(
        entries->t->arena, sizeof *entries->labels * (chart->code_count + 1u), 8);
    if (!entries->labels) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < chart->code_count; i++)
        entries->labels[i] = ta_string("");
    add_label(entries, chart->script, ta_string("script"));
    for (uint32_t s = 0; s < chart->state_count; s++) {
        const ta_state_record *state = &view->states[chart->first_state + s];
        ta_string_view id = state_id(view, chart, s);
        if (state->onentry != TA_NIL) add_label(entries, state->onentry, prefixed(entries->t, "onentry ", id));
        if (state->onexit != TA_NIL) add_label(entries, state->onexit, prefixed(entries->t, "onexit ", id));
    }
    label_transitions(entries, view);
    label_invokes(entries, view);
    return TA_OK;
}

/* ---- the operands ------------------------------------------------------------------------ */

typedef struct operand_text {
    tool_text text;
    const ta_image_view *view;
    const ta_chart_record *chart;
} operand_text;

static void put(operand_text *operands, const char *piece) {
    tool_text_append(&operands->text, ta_string(piece));
}

/* An expression in backticks. */
static void put_expression(operand_text *operands, uint32_t expression) {
    put(operands, "`");
    tool_text_append(&operands->text, expression_text(operands->view, operands->chart, expression));
    put(operands, "`");
}

/* A global pc, chart-relative. */
static void put_pc(operand_text *operands, uint32_t pc) {
    tool_text_number(&operands->text, pc - operands->chart->first_code);
}

static void put_send_target(operand_text *operands, const ta_send_record *send) {
    const ta_image_view *view = operands->view;
    put(operands, " to ");
    switch (send->target_kind) {
    case TA_SEND_TARGET_NONE: put(operands, "itself"); break;
    case TA_SEND_TARGET_INTERNAL: put(operands, "#_internal"); break;
    case TA_SEND_TARGET_PARENT: put(operands, "#_parent"); break;
    case TA_SEND_TARGET_INVOKE: {
        put(operands, "#_");
        ta_string_view id = send->target < operands->chart->invoke_count
                                ? string_of(view, view->invokes[operands->chart->first_invoke + send->target].id)
                                : ta_string("");
        if (id.length) tool_text_append(&operands->text, id);
        else tool_text_number(&operands->text, send->target);
        break;
    }
    case TA_SEND_TARGET_STRING: tool_text_append(&operands->text, string_of(view, send->target)); break;
    case TA_SEND_TARGET_EXPRESSION: put_expression(operands, send->targetexpr); break;
    default: put(operands, "?"); break;
    }
}

static void put_send(operand_text *operands, uint32_t index) {
    const ta_image_view *view = operands->view;
    if (index >= operands->chart->send_count) {
        put(operands, "send ");
        tool_text_number(&operands->text, index);
        return;
    }
    const ta_send_record *send = &view->sends[operands->chart->first_send + index];
    if (send->event != TA_NIL) {
        put(operands, "event ");
        tool_text_append(&operands->text, event_name(view, send->event));
    } else {
        put(operands, "eventexpr ");
        put_expression(operands, send->eventexpr);
    }
    put_send_target(operands, send);
    if (send->type != TA_NIL) {
        put(operands, " type ");
        tool_text_append(&operands->text, string_of(view, send->type));
    }
    if (send->delayexpr != TA_NIL) {
        put(operands, " delayexpr ");
        put_expression(operands, send->delayexpr);
    } else if (send->delay_ns) {
        put(operands, " after ");
        tool_text_duration(&operands->text, send->delay_ns);
    }
}

static void put_log(operand_text *operands, const uint32_t *words) {
    ta_string_view label = string_of(operands->view, words[1]);
    if (words[1] != TA_NIL) {
        put(operands, "label ");
        tool_text_append(&operands->text, label);
    }
    if (words[2] != TA_NIL) {
        if (words[1] != TA_NIL) put(operands, " ");
        put_expression(operands, words[2]);
    }
}

static void put_foreach(operand_text *operands, const uint32_t *words) {
    put_expression(operands, words[1]);
    put(operands, " as ");
    put_expression(operands, words[2]);
    if (words[3] != TA_NIL) {
        put(operands, ", ");
        put_expression(operands, words[3]);
    }
    put(operands, " until ");
    put_pc(operands, words[4]);
}

/* The operands of the instruction whose words start at `words`. */
static void put_operands(operand_text *operands, const uint32_t *words) {
    switch (words[0]) {
    case TA_OPCODE_BLOCK: put(operands, "to "); put_pc(operands, words[1]); break;
    case TA_OPCODE_RAISE:
        put(operands, "event ");
        tool_text_append(&operands->text, event_name(operands->view, words[1]));
        break;
    case TA_OPCODE_SEND: put_send(operands, words[1]); break;
    case TA_OPCODE_CANCEL:
        put(operands, "sendid ");
        tool_text_append(&operands->text, string_of(operands->view, words[1]));
        break;
    case TA_OPCODE_CANCEL_EXPRESSION: put_expression(operands, words[1]); break;
    case TA_OPCODE_ASSIGN:
        put_expression(operands, words[1]);
        put(operands, " = ");
        put_expression(operands, words[2]);
        break;
    case TA_OPCODE_SCRIPT: put_expression(operands, words[1]); break;
    case TA_OPCODE_LOG: put_log(operands, words); break;
    case TA_OPCODE_JUMP_IF_FALSE:
        put(operands, "unless ");
        put_expression(operands, words[1]);
        put(operands, " to ");
        put_pc(operands, words[2]);
        break;
    case TA_OPCODE_JUMP: put(operands, "to "); put_pc(operands, words[1]); break;
    case TA_OPCODE_FOREACH_BEGIN: put_foreach(operands, words); break;
    case TA_OPCODE_FOREACH_NEXT: put(operands, "back to "); put_pc(operands, words[1]); break;
    case TA_OPCODE_CUSTOM:
        put(operands, "custom ");
        tool_text_number(&operands->text, words[1]);
        break;
    default: break; /* end: none */
    }
}

/* ---- disassemble ------------------------------------------------------------------------- */

static void instruction_members(tool *t, const tool_opened *opened, uint32_t c) {
    const ta_image_view *view = view_of(opened);
    const ta_chart_record *chart = &view->charts[c];
    ta_report *report = &t->report;
    entry_labels entries = {t, chart, 0};
    if (label_entries(&entries, view) != TA_OK) {
        t->exit = TOOL_EXIT_FAILURE;
        tool_complain(t, ta_string("out of memory"));
        return;
    }
    open_list(t, "instructions", "instruction");
    uint32_t end = chart->first_code + chart->code_count;
    for (uint32_t pc = chart->first_code; pc < end;) {
        uint32_t opcode = view->code[pc];
        if (opcode >= TA_OPCODE_MAX_ || pc + ta_opcode_length[opcode] > end) break; /* verified images hold none */
        operand_text operands = {.view = view, .chart = chart};
        tool_text_init(t, &operands.text);
        put_operands(&operands, &view->code[pc]);
        ta_report_object(report, ta_string(""));
        ta_report_number(report, ta_string("pc"), pc - chart->first_code);
        ta_report_text(report, ta_string("label"), entries.labels[pc - chart->first_code]);
        ta_report_text(report, ta_string("opcode"), tool_opcode_name(opcode));
        ta_report_text(report, ta_string("operands"), tool_text_view(&operands.text));
        ta_report_end(report);
        pc += ta_opcode_length[opcode];
    }
    ta_report_end(report);
}

void tool_disassemble(tool *t) {
    tool_opened opened;
    if (tool_open(t, arguments_of(t), &opened) != TA_OK) return;
    each_chart(t, &opened, instruction_members);
    if (opened.image.diagnostic_count) tool_report_diagnostics(t, &opened.image);
}
