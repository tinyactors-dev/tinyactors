/* diff.c — tinyactors diff: what changed between two versions of an image,
 * chart by chart, as a statechart and not as XML text, and which changes
 * break the actors that run the old version when they migrate (tool.h,
 * docs/cli.md "Diff"). Freestanding.
 *
 * What a migration does, and so what "breaking" means here
 * (src/system/hibernate.c, ta_blob_migrate, and src/host/host.c,
 * migrate_one):
 *
 *   - states are mapped by id, the root to the root (hibernate.c:1124-1129,
 *     map_state); the mapped states and their new ancestors are set
 *     (:1329-1335) and the set is completed to a legal configuration of the
 *     new chart (:1162-1209, legalize): a compound state with no active
 *     child enters its initial targets, one with two keeps the first, an
 *     atomic or final state drops its descendants, a history state is
 *     dropped. No onentry runs for a state entered so: the blob's
 *     configuration words are written directly, and core runs onentry only
 *     while it enters a state in a microstep (src/core/core.c:1813). Only
 *     the invokes of newly entered states start (hibernate.c:1337-1343).
 *   - the datamodel deserializes the old variables, its migrate entry drops
 *     the variables the new chart does not declare and hands the old
 *     bindings over as error.platform.upgrade's data, and core initializes
 *     the <data> the new chart adds (include/tinyactors/datamodel.h:213-221,
 *     hibernate.c:1098-1106, :1360-1375);
 *   - an actor whose invocations run is not translated at all: it moves
 *     once they finished, or fails with TA_ERROR_STATE when the migration
 *     has no timeout (hibernate.c:1290-1291; host.c:1757-1761, :1788). So
 *     a migration never cancels a running invocation.
 *
 * Every comparison is at most O(n·m) in the records of one chart; memory
 * comes from the invocation's arena, and nothing recurses. */
#include "tool.h"

#include "analysis/analysis.h"

#include <tinyactors/datamodel.h>

enum diff_limits {
    DIFF_TEXT_LIMIT = 200 /* bytes of an action's text shown before and after */
};

static ta_string_view string_of(const ta_image_view *view, uint32_t id) {
    return ta_image_view_string(view, id);
}

/* ---- one side of a chart ----------------------------------------------------------------- */

/* A transition's key: its source state's id and its event descriptors. */
typedef struct transition_key {
    ta_string_view source, events;
} transition_key;

/* A chart of one side, and what it is matched with on the other: `chart`
 * is null when that side has no chart of the name. */
typedef struct chart_side {
    const ta_image_view *view;
    const ta_chart_record *chart;
    uint32_t *state_partners;      /* per state: the other side's state of the same id, or TA_NIL */
    transition_key *keys;          /* per transition */
    uint32_t *transition_partners; /* per transition: the other side's, or TA_NIL */
} chart_side;

static uint32_t state_count_of(const chart_side *side) {
    return side->chart ? side->chart->state_count : 0;
}

static uint32_t transition_count_of(const chart_side *side) {
    return side->chart ? side->chart->transition_count : 0;
}

static const ta_state_record *state_of(const chart_side *side, uint32_t state) {
    return &side->view->states[side->chart->first_state + state];
}

static const ta_transition_record *transition_of(const chart_side *side, uint32_t index) {
    return &side->view->transitions[side->chart->first_transition + index];
}

/* A state as the report names it: its id, "/scxml" for the root (which no
 * id can be), "" for none. */
static ta_string_view state_name(const chart_side *side, uint32_t state) {
    if (!side->chart || state == TA_NIL || state >= side->chart->state_count) return ta_string("");
    if (state == 0) return ta_string("/scxml");
    return string_of(side->view, state_of(side, state)->id);
}

/* The state named `name`, or TA_NIL: what a migration maps a state to
 * (hibernate.c:1124-1129, map_state: the root to the root, every other
 * state by its id). */
static uint32_t state_named(const chart_side *side, ta_string_view name) {
    for (uint32_t s = 0; s < state_count_of(side); s++)
        if (tool_equal(state_name(side, s), name)) return s;
    return TA_NIL;
}

static ta_string_view expression_text(const chart_side *side, uint32_t expression) {
    if (expression == TA_NIL || expression >= side->chart->expression_count) return ta_string("");
    return string_of(side->view, side->view->expressions[side->chart->first_expression + expression].source);
}

/* An event's full dotted name; "*" for every event. */
static ta_string_view event_name(const ta_image_view *view, uint32_t node) {
    if (node == TA_NIL || node >= view->event_node_count) return ta_string("");
    ta_string_view name = string_of(view, view->event_nodes[node].name);
    return name.length ? name : ta_string("*");
}

/* Descriptor `index` of a transition. */
static ta_string_view descriptor(const chart_side *side, const ta_transition_record *transition, uint32_t index) {
    return event_name(side->view, side->view->u32[transition->events_offset + 2 * index]);
}

static int is_initial(const ta_transition_record *transition) {
    return (transition->flags & TA_TRANSITION_FLAG_INITIAL) != 0;
}

static void separate(tool_text *text, const char *separator) {
    if (ta_output_fits(&text->output) && text->output.length) tool_text_append(text, ta_string(separator));
}

/* "<words><text>" in the arena. */
static ta_string_view prefixed(tool *t, const char *words, ta_string_view text) {
    tool_text out;
    tool_text_init(t, &out);
    tool_text_append(&out, ta_string(words));
    tool_text_append(&out, text);
    return tool_text_view(&out);
}

/* "<before> → <after>" in the arena. */
static ta_string_view arrow(tool *t, ta_string_view before, ta_string_view after) {
    tool_text out;
    tool_text_init(t, &out);
    tool_text_append(&out, before);
    tool_text_append(&out, ta_string(" \xe2\x86\x92 "));
    tool_text_append(&out, after);
    return tool_text_view(&out);
}

static ta_string_view events_text(tool *t, const chart_side *side, const ta_transition_record *transition) {
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t e = 0; e < transition->event_count; e++) {
        separate(&text, " ");
        tool_text_append(&text, descriptor(side, transition, e));
    }
    return tool_text_view(&text);
}

static ta_string_view targets_text(tool *t, const chart_side *side, const ta_transition_record *transition) {
    tool_text text;
    tool_text_init(t, &text);
    for (uint32_t i = 0; i < transition->target_count; i++) {
        separate(&text, " ");
        tool_text_append(&text, state_name(side, side->view->u32[transition->targets_offset + i]));
    }
    return tool_text_view(&text);
}

/* The two sides of one chart. */
typedef struct chart_pair {
    chart_side before, after;
} chart_pair;

static void *arena_array(tool *t, uint32_t count, uint32_t size) {
    return ta_growable_arena_alloc(t->arena, (size_t)size * (count + 1u), 8);
}

static int same_key(transition_key a, transition_key b) {
    return tool_equal(a.source, b.source) && tool_equal(a.events, b.events);
}

/* Transition `index` of `from` is matched with the transition of `to` of
 * the same key that has as many of that key before it: ties go by order.
 * Initial transitions are their state's, not transitions of their own. */
static uint32_t transition_partner(const chart_side *from, const chart_side *to, uint32_t index) {
    if (is_initial(transition_of(from, index))) return TA_NIL;
    transition_key key = from->keys[index];
    uint32_t rank = 0;
    for (uint32_t i = 0; i < index; i++)
        if (!is_initial(transition_of(from, i)) && same_key(from->keys[i], key)) rank++;
    for (uint32_t j = 0; j < transition_count_of(to); j++) {
        if (is_initial(transition_of(to, j)) || !same_key(to->keys[j], key)) continue;
        if (!rank--) return j;
    }
    return TA_NIL;
}

static ta_status prepare_side(tool *t, chart_side *side) {
    if (!side->chart) return TA_OK;
    side->state_partners = (uint32_t *)arena_array(t, side->chart->state_count, sizeof(uint32_t));
    side->keys = (transition_key *)arena_array(t, side->chart->transition_count, sizeof(transition_key));
    side->transition_partners = (uint32_t *)arena_array(t, side->chart->transition_count, sizeof(uint32_t));
    if (!side->state_partners || !side->keys || !side->transition_partners) return TA_ERROR_NO_MEMORY;
    for (uint32_t s = 0; s < side->chart->state_count; s++)
        side->state_partners[s] = TA_NIL;
    for (uint32_t i = 0; i < side->chart->transition_count; i++) {
        const ta_transition_record *transition = transition_of(side, i);
        side->keys[i] = (transition_key){state_name(side, transition->source), events_text(t, side, transition)};
        side->transition_partners[i] = TA_NIL;
    }
    return TA_OK;
}

/* States by id, transitions by key, both ways. */
static ta_status prepare(tool *t, chart_pair *pair) {
    if (prepare_side(t, &pair->before) != TA_OK || prepare_side(t, &pair->after) != TA_OK)
        return TA_ERROR_NO_MEMORY;
    if (!pair->before.chart || !pair->after.chart) return TA_OK;
    for (uint32_t s = 0; s < state_count_of(&pair->before); s++) {
        uint32_t other = state_named(&pair->after, state_name(&pair->before, s));
        pair->before.state_partners[s] = other;
        if (other != TA_NIL) pair->after.state_partners[other] = s;
    }
    for (uint32_t i = 0; i < transition_count_of(&pair->before); i++) {
        uint32_t other = transition_partner(&pair->before, &pair->after, i);
        pair->before.transition_partners[i] = other;
        if (other != TA_NIL) pair->after.transition_partners[other] = i;
    }
    return TA_OK;
}

/* ---- executable content as text ------------------------------------------------------------ */

/* An entry point's instructions, decoded into words as disassemble does,
 * with expressions and event names as text and jumps relative to their
 * instruction, so code that only moved elsewhere in the chart reads the
 * same. */
typedef struct code_text {
    tool_text text;
    const chart_side *side;
    uint32_t pc; /* the instruction's own, for relative jumps */
} code_text;

static void put(code_text *code, const char *piece) {
    tool_text_append(&code->text, ta_string(piece));
}

static void put_expression(code_text *code, uint32_t expression) {
    put(code, "`");
    tool_text_append(&code->text, expression_text(code->side, expression));
    put(code, "`");
}

static void put_jump(code_text *code, uint32_t target) {
    if (target >= code->pc) {
        put(code, "+");
        tool_text_number(&code->text, target - code->pc);
    } else {
        put(code, "-");
        tool_text_number(&code->text, code->pc - target);
    }
}

static const ta_invoke_record *invoke_of(const chart_side *side, uint32_t index) {
    return &side->view->invokes[side->chart->first_invoke + index];
}

static void put_send_target(code_text *code, const ta_send_record *send) {
    const chart_side *side = code->side;
    put(code, " to ");
    switch (send->target_kind) {
    case TA_SEND_TARGET_NONE: put(code, "itself"); break;
    case TA_SEND_TARGET_INTERNAL: put(code, "#_internal"); break;
    case TA_SEND_TARGET_PARENT: put(code, "#_parent"); break;
    case TA_SEND_TARGET_INVOKE: {
        put(code, "#_");
        ta_string_view id = send->target < side->chart->invoke_count
                                ? string_of(side->view, invoke_of(side, send->target)->id)
                                : ta_string("");
        if (id.length) tool_text_append(&code->text, id);
        else tool_text_number(&code->text, send->target);
        break;
    }
    case TA_SEND_TARGET_STRING: tool_text_append(&code->text, string_of(side->view, send->target)); break;
    case TA_SEND_TARGET_EXPRESSION: put_expression(code, send->targetexpr); break;
    default: put(code, "?"); break;
    }
}

/* What a send, an invoke or a donedata carries: namelist, params, content. */
static void put_capture(code_text *code, uint32_t index) {
    const ta_image_view *view = code->side->view;
    if (index == TA_NIL || index >= view->capture_count) return;
    const ta_capture_record *capture = &view->captures[index];
    for (uint32_t i = 0; i < capture->namelist_count; i++) {
        put(code, i ? " " : " namelist ");
        put_expression(code, view->u32[capture->namelist_offset + i]);
    }
    for (uint32_t i = 0; i < capture->param_count; i++) {
        put(code, i ? ", " : " with ");
        tool_text_append(&code->text, string_of(view, view->u32[capture->params_offset + 2 * i]));
        put(code, "=");
        put_expression(code, view->u32[capture->params_offset + 2 * i + 1]);
    }
    if (capture->content != TA_NIL) {
        put(code, " content ");
        put_expression(code, capture->content);
    }
}

static void put_send(code_text *code, uint32_t index) {
    const chart_side *side = code->side;
    if (index >= side->chart->send_count) {
        tool_text_number(&code->text, index);
        return;
    }
    const ta_send_record *send = &side->view->sends[side->chart->first_send + index];
    if (send->event != TA_NIL) {
        put(code, "event ");
        tool_text_append(&code->text, event_name(side->view, send->event));
    } else {
        put(code, "eventexpr ");
        put_expression(code, send->eventexpr);
    }
    put_send_target(code, send);
    if (send->type != TA_NIL) {
        put(code, " type ");
        tool_text_append(&code->text, string_of(side->view, send->type));
    } else if (send->typeexpr != TA_NIL) {
        put(code, " typeexpr ");
        put_expression(code, send->typeexpr);
    }
    if (send->id != TA_NIL) {
        put(code, " id ");
        tool_text_append(&code->text, string_of(side->view, send->id));
    } else if (send->idlocation != TA_NIL) {
        put(code, " idlocation ");
        put_expression(code, send->idlocation);
    }
    if (send->delayexpr != TA_NIL) {
        put(code, " delayexpr ");
        put_expression(code, send->delayexpr);
    } else if (send->delay_ns) {
        put(code, " after ");
        tool_text_duration(&code->text, send->delay_ns);
    }
    put_capture(code, send->capture);
}

static void put_operands(code_text *code, const uint32_t *words) {
    const ta_image_view *view = code->side->view;
    switch (words[0]) {
    case TA_OPCODE_RAISE:
        put(code, " ");
        tool_text_append(&code->text, event_name(view, words[1]));
        break;
    case TA_OPCODE_SEND: put(code, " "); put_send(code, words[1]); break;
    case TA_OPCODE_CANCEL:
        put(code, " ");
        tool_text_append(&code->text, string_of(view, words[1]));
        break;
    case TA_OPCODE_CANCEL_EXPRESSION:
    case TA_OPCODE_SCRIPT: put(code, " "); put_expression(code, words[1]); break;
    case TA_OPCODE_ASSIGN:
        put(code, " ");
        put_expression(code, words[1]);
        put(code, " = ");
        put_expression(code, words[2]);
        break;
    case TA_OPCODE_LOG:
        if (words[1] != TA_NIL) {
            put(code, " label ");
            tool_text_append(&code->text, string_of(view, words[1]));
        }
        if (words[2] != TA_NIL) {
            put(code, " ");
            put_expression(code, words[2]);
        }
        break;
    case TA_OPCODE_JUMP_IF_FALSE:
        put(code, " unless ");
        put_expression(code, words[1]);
        put(code, " to ");
        put_jump(code, words[2]);
        break;
    case TA_OPCODE_JUMP: put(code, " to "); put_jump(code, words[1]); break;
    case TA_OPCODE_FOREACH_BEGIN:
        put(code, " ");
        put_expression(code, words[1]);
        put(code, " as ");
        put_expression(code, words[2]);
        if (words[3] != TA_NIL) {
            put(code, ", ");
            put_expression(code, words[3]);
        }
        put(code, " until ");
        put_jump(code, words[4]);
        break;
    case TA_OPCODE_FOREACH_NEXT: put(code, " back to "); put_jump(code, words[1]); break;
    case TA_OPCODE_CUSTOM:
        put(code, " ");
        tool_text_number(&code->text, words[1]);
        break;
    default: break;
    }
}

/* The code of the entry point at `pc` (global; TA_NIL: none, ""), its
 * instructions "; "-joined. The block that opens it and the end that
 * closes it are left out: every entry point has them. */
static ta_string_view code_of(tool *t, const chart_side *side, uint32_t pc) {
    code_text code = {.side = side};
    tool_text_init(t, &code.text);
    if (pc == TA_NIL || !side->chart) return tool_text_view(&code.text);
    uint32_t end = side->chart->first_code + side->chart->code_count;
    while (pc >= side->chart->first_code && pc < end) {
        uint32_t opcode = side->view->code[pc];
        if (opcode >= TA_OPCODE_MAX_ || pc + ta_opcode_length[opcode] > end || opcode == TA_OPCODE_END) break;
        if (opcode != TA_OPCODE_BLOCK) {
            separate(&code.text, "; ");
            tool_text_append(&code.text, tool_opcode_name(opcode));
            code.pc = pc;
            put_operands(&code, &side->view->code[pc]);
        }
        pc += ta_opcode_length[opcode];
    }
    return tool_text_view(&code.text);
}

/* At most DIFF_TEXT_LIMIT bytes of `text`, cut at a character, then "…". */
static ta_string_view shortened(tool *t, ta_string_view text) {
    if (text.length <= DIFF_TEXT_LIMIT) return text;
    uint32_t length = DIFF_TEXT_LIMIT;
    while (length && ((unsigned char)text.pointer[length] & 0xC0u) == 0x80u)
        length--;
    tool_text out;
    tool_text_init(t, &out);
    tool_text_append(&out, (ta_string_view){text.pointer, length});
    tool_text_append(&out, ta_string("\xe2\x80\xa6"));
    return tool_text_view(&out);
}

/* ---- descriptions ------------------------------------------------------------------------- */

/* "compound in /scxml, initial open". */
static ta_string_view state_description(tool *t, const chart_side *side, uint32_t state) {
    const ta_state_record *record = state_of(side, state);
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, tool_state_kind_name(record->kind));
    if (state) {
        tool_text_append(&text, ta_string(" in "));
        tool_text_append(&text, state_name(side, record->parent));
    }
    if (record->initial != TA_NIL && record->initial < side->chart->transition_count) {
        ta_string_view targets = targets_text(t, side, transition_of(side, record->initial));
        if (targets.length) {
            tool_text_append(&text, ta_string(", initial "));
            tool_text_append(&text, targets);
        }
    }
    return tool_text_view(&text);
}

/* "cond `total > 100` → paid (external)"; "cond `x`, no target" for a
 * targetless one. */
static ta_string_view transition_description(tool *t, const chart_side *side, uint32_t index) {
    const ta_transition_record *transition = transition_of(side, index);
    tool_text text;
    tool_text_init(t, &text);
    if (transition->condition != TA_NIL) {
        tool_text_append(&text, ta_string("cond `"));
        tool_text_append(&text, expression_text(side, transition->condition));
        tool_text_append(&text, ta_string(transition->target_count ? "` " : "`, "));
    }
    if (!transition->target_count) {
        tool_text_append(&text, ta_string("no target"));
        return tool_text_view(&text);
    }
    tool_text_append(&text, ta_string("\xe2\x86\x92 "));
    tool_text_append(&text, targets_text(t, side, transition));
    tool_text_append(&text, transition->flags & TA_TRANSITION_FLAG_INTERNAL ? ta_string(" (internal)")
                                                                           : ta_string(" (external)"));
    return tool_text_view(&text);
}

static const ta_data_record *datum_of(const chart_side *side, uint32_t index) {
    return &side->view->data[side->chart->first_data + index];
}

static ta_string_view datum_id(const chart_side *side, uint32_t index) {
    return string_of(side->view, datum_of(side, index)->id);
}

/* "`0` in /scxml". */
static ta_string_view datum_description(tool *t, const chart_side *side, uint32_t index) {
    const ta_data_record *datum = datum_of(side, index);
    tool_text text;
    tool_text_init(t, &text);
    if (datum->expression != TA_NIL) {
        tool_text_append(&text, ta_string("`"));
        tool_text_append(&text, expression_text(side, datum->expression));
        tool_text_append(&text, ta_string("` in "));
    } else {
        tool_text_append(&text, ta_string("unset in "));
    }
    tool_text_append(&text, state_name(side, datum->state));
    return tool_text_view(&text);
}

/* An invoke as its state's: its id, or "#<position>" among its state's
 * invokes. */
static ta_string_view invoke_label(tool *t, const chart_side *side, uint32_t index) {
    const ta_invoke_record *invoke = invoke_of(side, index);
    ta_string_view id = string_of(side->view, invoke->id);
    if (id.length) return id;
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, ta_string("#"));
    tool_text_number(&text, index - state_of(side, invoke->state)->first_invoke + 1);
    return tool_text_view(&text);
}

/* "type scxml src payment.scxml, autoforward". */
static ta_string_view invoke_description(tool *t, const chart_side *side, uint32_t index) {
    const ta_invoke_record *invoke = invoke_of(side, index);
    code_text code = {.side = side};
    tool_text_init(t, &code.text);
    if (invoke->type != TA_NIL) {
        put(&code, "type ");
        tool_text_append(&code.text, string_of(side->view, invoke->type));
    } else if (invoke->typeexpr != TA_NIL) {
        put(&code, "typeexpr ");
        put_expression(&code, invoke->typeexpr);
    } else {
        put(&code, "type scxml");
    }
    if (invoke->source != TA_NIL) {
        put(&code, " src ");
        tool_text_append(&code.text, string_of(side->view, invoke->source));
    } else if (invoke->srcexpr != TA_NIL) {
        put(&code, " srcexpr ");
        put_expression(&code, invoke->srcexpr);
    } else if (invoke->docexpr != TA_NIL) {
        put(&code, " content ");
        put_expression(&code, invoke->docexpr);
    }
    if (invoke->autoforward) put(&code, ", autoforward");
    return tool_text_view(&code.text);
}

/* The invoke of `to`'s state `state` matched with invoke `index` of
 * `from`: the same id, or the same position among id-less ones. */
typedef struct invoke_match {
    const chart_side *from, *to;
    uint32_t state; /* `to`'s */
} invoke_match;

static uint32_t invoke_partner(tool *t, const invoke_match *match, uint32_t index) {
    ta_string_view label = invoke_label(t, match->from, index);
    const ta_state_record *state = state_of(match->to, match->state);
    for (uint32_t i = state->first_invoke; i < state->first_invoke + state->invoke_count; i++)
        if (tool_equal(invoke_label(t, match->to, i), label)) return i;
    return TA_NIL;
}

/* ---- the differences of one chart ---------------------------------------------------------- */

typedef enum diff_list {
    DIFF_STATES = 0,
    DIFF_TRANSITIONS,
    DIFF_DATA,
    DIFF_ACTIONS,
    DIFF_INVOKES,
    DIFF_EVENTS,
    DIFF_LIST_COUNT
} diff_list;

typedef enum diff_columns {
    DIFF_COLUMN_BREAKING = 1u << 0, /* items say whether they break running actors */
    DIFF_COLUMN_BEFORE_AFTER = 1u << 1
} diff_columns;

/* A list's names: the list, its item, the members naming what an item is
 * about (the second may be absent), and its other columns. */
typedef struct list_shape {
    const char *name, *item, *first, *second;
    uint32_t columns; /* diff_columns */
} list_shape;

static const list_shape shapes[DIFF_LIST_COUNT] = {
    [DIFF_STATES] = {"states", "state", "state", 0, DIFF_COLUMN_BREAKING | DIFF_COLUMN_BEFORE_AFTER},
    [DIFF_TRANSITIONS] = {"transitions", "transition", "source", "events",
                          DIFF_COLUMN_BREAKING | DIFF_COLUMN_BEFORE_AFTER},
    [DIFF_DATA] = {"data", "datum", "id", 0, DIFF_COLUMN_BREAKING | DIFF_COLUMN_BEFORE_AFTER},
    [DIFF_ACTIONS] = {"actions", "action", "where", 0, DIFF_COLUMN_BEFORE_AFTER},
    [DIFF_INVOKES] = {"invokes", "invoke", "state", "invoke", DIFF_COLUMN_BREAKING | DIFF_COLUMN_BEFORE_AFTER},
    [DIFF_EVENTS] = {"events", "event", "state", "event", 0},
};

/* One difference, as a list's item. */
typedef struct difference {
    ta_string_view first, second; /* what it is about */
    ta_string_view change, before, after;
    int breaking;
} difference;

/* What the breaking changes of a chart mean for its running actors, in
 * words, gathered while it is compared (the counting pass only). */
typedef struct migration_notes {
    tool_text removed, kinds, moved, data, events;
} migration_notes;

/* A chart being compared: counted (`writing` 0) or written into the report. */
typedef struct chart_diff {
    tool *t;
    chart_pair pair;
    ta_string_view name;
    uint32_t list;      /* diff_list the items go to */
    int writing;        /* write the items; else only count them */
    int breaking_only;  /* --breaking: write only the breaking ones */
    uint32_t differences, breaking;
    migration_notes *notes; /* the counting pass's, or null */
} chart_diff;

static void note(tool_text *text, ta_string_view piece) {
    separate(text, ", ");
    tool_text_append(text, piece);
}

static void emit(chart_diff *diff, const difference *item) {
    diff->differences++;
    if (item->breaking) diff->breaking++;
    if (!diff->writing || (diff->breaking_only && !item->breaking)) return;
    const list_shape *shape = &shapes[diff->list];
    ta_report *report = &diff->t->report;
    ta_report_object(report, ta_string(""));
    ta_report_text(report, ta_string(shape->first), item->first);
    if (shape->second) ta_report_text(report, ta_string(shape->second), item->second);
    ta_report_text(report, ta_string("change"), item->change);
    if (shape->columns & DIFF_COLUMN_BREAKING) ta_report_boolean(report, ta_string("breaking"), item->breaking);
    if (shape->columns & DIFF_COLUMN_BEFORE_AFTER) {
        ta_report_text(report, ta_string("before"), item->before);
        ta_report_text(report, ta_string("after"), item->after);
    }
    ta_report_end(report);
}

static int both_sides(const chart_diff *diff) {
    return diff->pair.before.chart && diff->pair.after.chart;
}

static void open_list(chart_diff *diff, uint32_t list) {
    diff->list = list;
    if (diff->writing)
        ta_report_list(&diff->t->report,
                       (ta_report_list_names){ta_string(shapes[list].name), ta_string(shapes[list].item)});
}

static void close_list(chart_diff *diff) {
    if (diff->writing) ta_report_end(&diff->t->report);
}

/* ---- states ------------------------------------------------------------------------------- */

/* The invokes of two kept states, as one text each: whether they differ. */
static int invokes_differ(chart_diff *diff, uint32_t before, uint32_t after) {
    const chart_side *old = &diff->pair.before, *next = &diff->pair.after;
    const ta_state_record *from = state_of(old, before), *to = state_of(next, after);
    if (from->invoke_count != to->invoke_count) return 1;
    invoke_match match = {old, next, after};
    for (uint32_t i = from->first_invoke; i < from->first_invoke + from->invoke_count; i++) {
        uint32_t other = invoke_partner(diff->t, &match, i);
        if (other == TA_NIL ||
            !tool_equal(invoke_description(diff->t, old, i), invoke_description(diff->t, next, other)) ||
            !tool_equal(code_of(diff->t, old, invoke_of(old, i)->finalize),
                        code_of(diff->t, next, invoke_of(next, other)->finalize)))
            return 1;
    }
    return 0;
}

static ta_string_view initial_targets(tool *t, const chart_side *side, uint32_t state) {
    uint32_t initial = state_of(side, state)->initial;
    if (initial == TA_NIL || initial >= side->chart->transition_count) return ta_string("");
    return targets_text(t, side, transition_of(side, initial));
}

/* A state both versions have. A new kind, or a new parent, changes the
 * configuration a running actor gets (hibernate.c:1162-1209, legalize: an
 * atomic or final state drops its children, a compound one with none
 * enters its initial targets, a history state is dropped, :1332; a moved
 * state's new ancestors are set, :1333, and a compound ancestor that then
 * has two active children keeps the first, :1184-1189): breaking. */
static void kept_state(chart_diff *diff, uint32_t before, uint32_t after) {
    tool *t = diff->t;
    const chart_side *old = &diff->pair.before, *next = &diff->pair.after;
    const ta_state_record *from = state_of(old, before), *to = state_of(next, after);
    ta_string_view name = state_name(old, before);
    tool_text change;
    tool_text_init(t, &change);
    int kind = from->kind != to->kind;
    int moved = before && !tool_equal(state_name(old, from->parent), state_name(next, to->parent));
    if (kind) note(&change, ta_string("kind changed"));
    if (moved) note(&change, ta_string("moved"));
    if (!tool_equal(initial_targets(t, old, before), initial_targets(t, next, after)))
        note(&change, ta_string("initial changed"));
    if (!tool_equal(code_of(t, old, from->onentry), code_of(t, next, to->onentry)))
        note(&change, ta_string("onentry changed"));
    if (!tool_equal(code_of(t, old, from->onexit), code_of(t, next, to->onexit)))
        note(&change, ta_string("onexit changed"));
    if (invokes_differ(diff, before, after)) note(&change, ta_string("invokes changed"));
    if (!change.output.length) return;
    difference item = {name, ta_string(""), tool_text_view(&change), state_description(t, old, before),
                       state_description(t, next, after), kind || moved};
    emit(diff, &item);
    if (!diff->notes) return;
    if (kind) {
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, name);
        tool_text_append(&text, ta_string(" ("));
        tool_text_append(&text, arrow(t, tool_state_kind_name(from->kind), tool_state_kind_name(to->kind)));
        tool_text_append(&text, ta_string(")"));
        note(&diff->notes->kinds, tool_text_view(&text));
    }
    if (moved) {
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, name);
        tool_text_append(&text, ta_string(" (now in "));
        tool_text_append(&text, state_name(next, to->parent));
        tool_text_append(&text, ta_string(")"));
        note(&diff->notes->moved, tool_text_view(&text));
    }
}

/* "paying → open": a removed state, and where a migration puts an actor
 * in it when it can tell. Its ancestors stay active and are mapped by id
 * (hibernate.c:1331-1335); legalize then has the nearest one the new
 * version keeps, a compound state with no active child, enter its initial
 * targets (:1191-1203). Under a parallel state the actor only loses the
 * region: no single destination. */
static ta_string_view removed_state(tool *t, const chart_pair *pair, uint32_t state) {
    const chart_side *old = &pair->before, *next = &pair->after;
    ta_string_view name = state_name(old, state);
    uint32_t ancestor = state_of(old, state)->parent;
    while (ancestor != TA_NIL && old->state_partners[ancestor] == TA_NIL)
        ancestor = state_of(old, ancestor)->parent;
    if (ancestor == TA_NIL) return name;
    uint32_t kept = old->state_partners[ancestor];
    if (state_of(next, kept)->kind != TA_STATE_COMPOUND) return name;
    ta_string_view targets = initial_targets(t, next, kept);
    return targets.length ? arrow(t, name, targets) : name;
}

static void compare_states(chart_diff *diff) {
    const chart_side *old = &diff->pair.before, *next = &diff->pair.after;
    open_list(diff, DIFF_STATES);
    for (uint32_t s = 0; both_sides(diff) && s < state_count_of(old); s++) {
        uint32_t other = old->state_partners[s];
        if (other != TA_NIL) {
            kept_state(diff, s, other);
            continue;
        }
        /* an actor in it is completed to a legal configuration of the new
         * version (hibernate.c:1331-1336: an unmapped state is skipped, then
         * legalize): breaking */
        difference item = {state_name(old, s), ta_string(""), ta_string("removed"),
                           state_description(diff->t, old, s), ta_string(""), 1};
        emit(diff, &item);
        if (diff->notes) note(&diff->notes->removed, removed_state(diff->t, &diff->pair, s));
    }
    for (uint32_t s = 0; both_sides(diff) && s < state_count_of(next); s++) {
        if (next->state_partners[s] != TA_NIL) continue;
        difference item = {state_name(next, s), ta_string(""), ta_string("added"), ta_string(""),
                           state_description(diff->t, next, s), 0};
        emit(diff, &item);
    }
    close_list(diff);
}

/* ---- transitions -------------------------------------------------------------------------- */

/* A condition, targets, type or actions changed: what an actor does on its
 * next event, never what it holds: behaviour, not breaking. */
static void compare_transitions(chart_diff *diff) {
    tool *t = diff->t;
    const chart_side *old = &diff->pair.before, *next = &diff->pair.after;
    open_list(diff, DIFF_TRANSITIONS);
    for (uint32_t i = 0; both_sides(diff) && i < transition_count_of(old); i++) {
        const ta_transition_record *from = transition_of(old, i);
        if (is_initial(from)) continue;
        uint32_t other = old->transition_partners[i];
        difference item = {old->keys[i].source, old->keys[i].events, ta_string("removed"),
                           transition_description(t, old, i), ta_string(""), 0};
        if (other != TA_NIL) {
            const ta_transition_record *to = transition_of(next, other);
            tool_text change;
            tool_text_init(t, &change);
            if (!tool_equal(expression_text(old, from->condition), expression_text(next, to->condition)))
                note(&change, ta_string("condition changed"));
            if (!tool_equal(targets_text(t, old, from), targets_text(t, next, to)))
                note(&change, ta_string("targets changed"));
            if ((from->flags & TA_TRANSITION_FLAG_INTERNAL) != (to->flags & TA_TRANSITION_FLAG_INTERNAL))
                note(&change, ta_string("type changed"));
            if (!tool_equal(code_of(t, old, from->action), code_of(t, next, to->action)))
                note(&change, ta_string("actions changed"));
            if (!change.output.length) continue;
            item.change = tool_text_view(&change);
            item.after = transition_description(t, next, other);
        }
        emit(diff, &item);
    }
    for (uint32_t j = 0; both_sides(diff) && j < transition_count_of(next); j++) {
        if (is_initial(transition_of(next, j)) || next->transition_partners[j] != TA_NIL) continue;
        difference item = {next->keys[j].source, next->keys[j].events, ta_string("added"), ta_string(""),
                           transition_description(t, next, j), 0};
        emit(diff, &item);
    }
    close_list(diff);
}

/* ---- data --------------------------------------------------------------------------------- */

static uint32_t datum_named(const chart_side *side, ta_string_view id) {
    for (uint32_t d = 0; d < side->chart->data_count; d++)
        if (tool_equal(datum_id(side, d), id)) return d;
    return TA_NIL;
}

/* A datum removed: the datamodel's migrate drops the variables the new
 * chart does not declare (datamodel.h:213-219), so a running actor loses
 * its value but for error.platform.upgrade's data (hibernate.c:1098-1106):
 * breaking. One added is initialized for running actors (hibernate.c:
 * 1360-1375, under late binding once its state is active). A new
 * expression or declaring state changes only how it is initialized, which
 * running actors did already: the value they hold stays. */
static void compare_data(chart_diff *diff) {
    tool *t = diff->t;
    const chart_side *old = &diff->pair.before, *next = &diff->pair.after;
    open_list(diff, DIFF_DATA);
    for (uint32_t d = 0; both_sides(diff) && d < old->chart->data_count; d++) {
        ta_string_view id = datum_id(old, d);
        uint32_t other = datum_named(next, id);
        difference item = {id, ta_string(""), ta_string("removed"), datum_description(t, old, d), ta_string(""), 1};
        if (other != TA_NIL) {
            const ta_data_record *from = datum_of(old, d), *to = datum_of(next, other);
            tool_text change;
            tool_text_init(t, &change);
            if (!tool_equal(expression_text(old, from->expression), expression_text(next, to->expression)))
                note(&change, ta_string("expression changed"));
            if (!tool_equal(state_name(old, from->state), state_name(next, to->state)))
                note(&change, ta_string("moved"));
            if (!change.output.length) continue;
            item.change = tool_text_view(&change);
            item.after = datum_description(t, next, other);
            item.breaking = 0;
        } else if (diff->notes) {
            note(&diff->notes->data, id);
        }
        emit(diff, &item);
    }
    for (uint32_t d = 0; both_sides(diff) && d < next->chart->data_count; d++) {
        ta_string_view id = datum_id(next, d);
        if (datum_named(old, id) != TA_NIL) continue;
        difference item = {id, ta_string(""), ta_string("added"), ta_string(""), datum_description(t, next, d), 0};
        emit(diff, &item);
    }
    close_list(diff);
}

/* ---- actions ------------------------------------------------------------------------------ */

/* One entry point on both sides, compared as text. */
typedef struct entry_pair {
    ta_string_view where;
    uint32_t before, after; /* global pcs, TA_NIL: none */
} entry_pair;

static void compare_entry(chart_diff *diff, entry_pair entry) {
    tool *t = diff->t;
    ta_string_view before = code_of(t, &diff->pair.before, entry.before);
    ta_string_view after = code_of(t, &diff->pair.after, entry.after);
    if (tool_equal(before, after)) return;
    difference item = {entry.where, ta_string(""),
                       !before.length  ? ta_string("added")
                       : !after.length ? ta_string("removed")
                                       : ta_string("changed"),
                       shortened(t, before), shortened(t, after), 0};
    emit(diff, &item);
}

/* "transition open on checkout", "#2" after it for the second of that key. */
static ta_string_view transition_where(tool *t, const chart_side *side, uint32_t index) {
    transition_key key = side->keys[index];
    uint32_t rank = 1;
    for (uint32_t i = 0; i < index; i++)
        if (!is_initial(transition_of(side, i)) && same_key(side->keys[i], key)) rank++;
    tool_text text;
    tool_text_init(t, &text);
    tool_text_append(&text, ta_string("transition "));
    tool_text_append(&text, key.source);
    if (key.events.length) {
        tool_text_append(&text, ta_string(" on "));
        tool_text_append(&text, key.events);
    } else {
        tool_text_append(&text, ta_string(" without event"));
    }
    if (rank > 1) {
        tool_text_append(&text, ta_string(" #"));
        tool_text_number(&text, rank);
    }
    return tool_text_view(&text);
}

/* The executable content of what both versions have: the chart's script,
 * kept states' onentry and onexit, matched transitions' actions, matched
 * invokes' finalize. Behaviour, not breaking: a running actor runs the new
 * code from its next event on. */
static void compare_actions(chart_diff *diff) {
    tool *t = diff->t;
    const chart_side *old = &diff->pair.before, *next = &diff->pair.after;
    open_list(diff, DIFF_ACTIONS);
    if (both_sides(diff))
        compare_entry(diff, (entry_pair){ta_string("script"), old->chart->script, next->chart->script});
    for (uint32_t s = 0; both_sides(diff) && s < state_count_of(old); s++) {
        uint32_t other = old->state_partners[s];
        if (other == TA_NIL) continue;
        const ta_state_record *from = state_of(old, s), *to = state_of(next, other);
        ta_string_view name = state_name(old, s);
        compare_entry(diff, (entry_pair){prefixed(t, "onentry ", name), from->onentry, to->onentry});
        compare_entry(diff, (entry_pair){prefixed(t, "onexit ", name), from->onexit, to->onexit});
    }
    for (uint32_t i = 0; both_sides(diff) && i < transition_count_of(old); i++) {
        uint32_t other = old->transition_partners[i];
        if (other == TA_NIL) continue;
        compare_entry(diff, (entry_pair){transition_where(t, old, i), transition_of(old, i)->action,
                                         transition_of(next, other)->action});
    }
    for (uint32_t s = 0; both_sides(diff) && s < state_count_of(old); s++) {
        uint32_t other = old->state_partners[s];
        if (other == TA_NIL) continue;
        const ta_state_record *state = state_of(old, s);
        invoke_match match = {old, next, other};
        for (uint32_t i = state->first_invoke; i < state->first_invoke + state->invoke_count; i++) {
            uint32_t partner = invoke_partner(t, &match, i);
            if (partner == TA_NIL) continue;
            compare_entry(diff, (entry_pair){prefixed(t, "finalize ", invoke_label(t, old, i)),
                                             invoke_of(old, i)->finalize, invoke_of(next, partner)->finalize});
        }
    }
    close_list(diff);
}

/* ---- invokes ------------------------------------------------------------------------------ */

/* The invokes of the states both versions have. None is breaking: a
 * migration never cancels a running invocation, since an actor with one
 * is not translated until it finished (hibernate.c:1290-1291; host.c:
 * 1757-1761, :1788). An invoke added to a kept state does not start for
 * the actors already in it: only newly entered states invoke
 * (hibernate.c:1337-1343). */
static void compare_invokes(chart_diff *diff) {
    tool *t = diff->t;
    const chart_side *old = &diff->pair.before, *next = &diff->pair.after;
    open_list(diff, DIFF_INVOKES);
    for (uint32_t s = 0; both_sides(diff) && s < state_count_of(old); s++) {
        uint32_t other = old->state_partners[s];
        if (other == TA_NIL) continue;
        const ta_state_record *from = state_of(old, s), *to = state_of(next, other);
        ta_string_view name = state_name(old, s);
        invoke_match forward = {old, next, other}, backward = {next, old, s};
        for (uint32_t i = from->first_invoke; i < from->first_invoke + from->invoke_count; i++) {
            uint32_t partner = invoke_partner(t, &forward, i);
            difference item = {name, invoke_label(t, old, i), ta_string("removed"), invoke_description(t, old, i),
                               ta_string(""), 0};
            if (partner != TA_NIL) {
                item.after = invoke_description(t, next, partner);
                if (tool_equal(item.before, item.after)) continue;
                item.change = ta_string("changed");
            }
            emit(diff, &item);
        }
        for (uint32_t i = to->first_invoke; i < to->first_invoke + to->invoke_count; i++) {
            if (invoke_partner(t, &backward, i) != TA_NIL) continue;
            difference item = {name, invoke_label(t, next, i), ta_string("added"), ta_string(""),
                               invoke_description(t, next, i), 0};
            emit(diff, &item);
        }
    }
    close_list(diff);
}

/* ---- events a state handles ------------------------------------------------------------- */

/* The descriptors of the transitions an actor in `state` takes: those
 * whose source is the state or one of its ancestors (a source's
 * descendants are the interval (source, end), records.h). */
typedef struct descriptor_walk {
    const chart_side *side;
    uint32_t state, transition, event;
} descriptor_walk;

static int walk_next(descriptor_walk *walk, ta_string_view *name) {
    const chart_side *side = walk->side;
    for (; walk->transition < side->chart->transition_count; walk->transition++, walk->event = 0) {
        const ta_transition_record *transition = transition_of(side, walk->transition);
        uint32_t source = transition->source;
        if (is_initial(transition) || source > walk->state || walk->state >= state_of(side, source)->end) continue;
        if (walk->event < transition->event_count) {
            *name = descriptor(side, transition, walk->event++);
            return 1;
        }
    }
    return 0;
}

/* Whether an actor in `state` of `side` handles `event` (a descriptor
 * covers it); with `before`, whether one of the first `before`
 * descriptors names it exactly (it was listed already). */
typedef struct handled_query {
    const chart_side *side;
    uint32_t state;
    uint32_t before; /* TA_NIL: every descriptor, matched by ta_analysis_descriptor_covers */
} handled_query;

static int handles(handled_query query, ta_string_view event) {
    descriptor_walk walk = {query.side, query.state, 0, 0};
    ta_string_view name;
    for (uint32_t k = 0; walk_next(&walk, &name); k++) {
        if (query.before != TA_NIL) {
            if (k >= query.before) return 0;
            if (tool_equal(name, event)) return 1;
        } else if (ta_analysis_descriptor_covers(name, event)) {
            return 1;
        }
    }
    return 0;
}

/* The events of one side an actor in `state` handles that the other side's
 * actor in `other` no longer (or not yet) does. */
typedef struct handled_pair {
    const chart_side *from, *to;
    uint32_t state, other;
} handled_pair;

static void handled_only(chart_diff *diff, const handled_pair *pair, int lost) {
    descriptor_walk walk = {pair->from, pair->state, 0, 0};
    ta_string_view event;
    ta_string_view state = state_name(&diff->pair.before, lost ? pair->state : pair->other);
    tool_text sentence;
    tool_text_init(diff->t, &sentence);
    for (uint32_t k = 0; walk_next(&walk, &event); k++) {
        if (handles((handled_query){pair->from, pair->state, k}, event)) continue; /* listed already */
        if (handles((handled_query){pair->to, pair->other, TA_NIL}, event)) continue;
        difference item = {state, event, lost ? ta_string("no longer handled") : ta_string("newly handled"),
                           ta_string(""), ta_string(""), lost};
        emit(diff, &item);
        if (lost && diff->notes) note(&sentence, event);
    }
    if (!sentence.output.length) return;
    /* an actor waiting there ignores the event: nothing in the new chart
     * takes it */
    tool_text *events = &diff->notes->events;
    separate(events, " ");
    tool_text_append(events, ta_string("Actors in "));
    tool_text_append(events, state);
    tool_text_append(events, ta_string(" no longer handle "));
    tool_text_append(events, tool_text_view(&sentence));
    tool_text_append(events, ta_string("; they now ignore such events."));
}

/* Per kept state an actor can rest in (atomic or final: the leaves of a
 * configuration), the events it handles before and after. */
static void compare_events(chart_diff *diff) {
    const chart_side *old = &diff->pair.before, *next = &diff->pair.after;
    open_list(diff, DIFF_EVENTS);
    for (uint32_t s = 1; both_sides(diff) && s < state_count_of(old); s++) {
        uint32_t kind = state_of(old, s)->kind, other = old->state_partners[s];
        if (other == TA_NIL || (kind != TA_STATE_ATOMIC && kind != TA_STATE_FINAL)) continue;
        handled_pair forward = {old, next, s, other}, backward = {next, old, other, s};
        handled_only(diff, &forward, 1);
        handled_only(diff, &backward, 0);
    }
    close_list(diff);
}

/* ---- one chart ---------------------------------------------------------------------------- */

static ta_string_view binding_name(uint32_t binding) {
    return binding == TA_BINDING_LATE ? ta_string("late") : ta_string("early");
}

/* The datamodel and the binding: "ecmascript", or "ecmascript → python"
 * when they changed, counted. A new datamodel is given the old one's
 * serialized state (hibernate.c:697-746, restore: the new chart's binding
 * deserializes the blob's datamodel bytes); whether it refuses them or
 * misreads them is the datamodel's (unverified), so: breaking. Late
 * binding to early: early binding initializes <data> only when the actor
 * starts (core.c:1929-1941) and late binding when a state is first entered
 * (core.c:1802-1809), so the data of states a running actor never entered
 * stays unset: breaking. Early to late marks every state initialized
 * (hibernate.c:1345-1356): not breaking. */
typedef struct chart_settings {
    ta_string_view datamodel, binding;
    int datamodel_changed, binding_changed;
} chart_settings;

static ta_string_view datamodel_name(const chart_side *side) {
    ta_string_view name = string_of(side->view, side->chart->datamodel);
    return name.length ? name : ta_string("null");
}

static chart_settings compare_settings(chart_diff *diff) {
    chart_settings settings = {ta_string(""), ta_string(""), 0, 0};
    const chart_side *old = &diff->pair.before, *next = &diff->pair.after;
    const chart_side *latest = next->chart ? next : old;
    settings.datamodel = datamodel_name(latest);
    settings.binding = binding_name(latest->chart->binding);
    if (!both_sides(diff)) return settings;
    tool *t = diff->t;
    ta_string_view from = datamodel_name(old), to = datamodel_name(next);
    if (!tool_equal(from, to)) {
        settings.datamodel = arrow(t, from, to);
        settings.datamodel_changed = 1;
        diff->differences++;
        diff->breaking++;
    }
    if (old->chart->binding != next->chart->binding) {
        settings.binding = arrow(t, binding_name(old->chart->binding), binding_name(next->chart->binding));
        settings.binding_changed = 1;
        diff->differences++;
        if (old->chart->binding == TA_BINDING_LATE) diff->breaking++;
    }
    return settings;
}

static ta_string_view change_of(const chart_diff *diff) {
    if (!diff->pair.before.chart) return ta_string("added");
    if (!diff->pair.after.chart) return ta_string("removed");
    return diff->differences ? ta_string("changed") : ta_string("unchanged");
}

static void compare_lists(chart_diff *diff) {
    compare_states(diff);
    compare_transitions(diff);
    compare_data(diff);
    compare_actions(diff);
    compare_invokes(diff);
    compare_events(diff);
}

/* "<chart>: <what>" sentences for the migration text. */
typedef struct migration_sentence {
    const char *before, *after; /* around the names */
} migration_sentence;

static void sentence(tool_text *out, const tool_text *names, migration_sentence words) {
    if (!names->output.length) return;
    separate(out, " ");
    tool_text_append(out, ta_string(words.before));
    tool_text_append(out, tool_text_view(names));
    tool_text_append(out, ta_string(words.after));
}

/* The migration text of one chart, from what its counting pass noted. */
static void explain(chart_diff *diff, chart_settings settings, tool_text *out) {
    const migration_notes *notes = diff->notes;
    if (!diff->breaking) return;
    separate(out, " ");
    tool_text_append(out, ta_string("Chart "));
    tool_text_append(out, diff->name);
    tool_text_append(out, ta_string(":"));
    sentence(out, &notes->removed,
             (migration_sentence){"Actors in removed states are completed to a legal configuration of the "
                                  "new version, the nearest kept ancestor entering its initial state, and no "
                                  "onentry runs: ",
                                  "."});
    sentence(out, &notes->kinds,
             (migration_sentence){"Actors in ", " keep the state, but their configuration is completed for its "
                                               "new kind, and no onentry runs."});
    sentence(out, &notes->moved,
             (migration_sentence){"Actors in ", " enter its new ancestors without their onentry; a compound "
                                               "ancestor that then has two active children keeps the first."});
    sentence(out, &notes->data,
             (migration_sentence){"Running actors lose ", "; the old values are only in the data of the "
                                                         "error.platform.upgrade event each one gets first."});
    if (settings.datamodel_changed) {
        separate(out, " ");
        tool_text_append(out, ta_string("The datamodel changes ("));
        tool_text_append(out, settings.datamodel);
        tool_text_append(out, ta_string("): the new one is given the old one's state, which it may refuse; "
                                        "let running actors finish on the old version."));
    }
    if (diff->pair.before.chart->binding == TA_BINDING_LATE && settings.binding_changed) {
        separate(out, " ");
        tool_text_append(out, ta_string("Binding changes from late to early: the data of states a running "
                                        "actor never entered stays unset."));
    }
    if (notes->events.output.length) {
        separate(out, " ");
        tool_text_append(out, tool_text_view(&notes->events));
    }
}

/* ---- the two images ---------------------------------------------------------------------- */

typedef struct diff_side {
    ta_string_view path;
    tool_opened opened;
} diff_side;

typedef struct diff_sides {
    const diff_side *before, *after;
} diff_sides;

static const ta_image_view *view_of(const diff_side *side) {
    return ta_image_view_of(&side->opened.image.image);
}

/* The whole comparison: counted first (the summary and the migration text
 * come before the charts), then written. */
typedef struct diff_run {
    tool *t;
    diff_sides sides;
    int all, breaking_only;
    uint32_t charts_added, charts_removed, charts_changed, charts_unchanged;
    uint32_t differences, breaking;
    tool_text migration;
    int failed; /* out of memory */
} diff_run;

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
    chart_side found;
    int counts;
} chart_lookup;

static chart_lookup look_up(const diff_side *side, ta_string_view name) {
    const ta_image_view *view = view_of(side);
    uint32_t chart = ta_image_chart_by_name(&side->opened.image.image, name);
    chart_lookup lookup = {{view, 0, 0, 0, 0}, 1};
    if (chart == TA_NIL || chart >= view->chart_count) return lookup;
    lookup.found.chart = &view->charts[chart];
    lookup.counts = side->opened.charts.selected[chart] != 0;
    return lookup;
}

typedef enum diff_pass {
    DIFF_PASS_COUNT = 0, /* the summary and the migration text */
    DIFF_PASS_WRITE = 1  /* the chart's item in the report */
} diff_pass;

/* One chart, by its name, in one pass. */
static void chart_by_name(diff_run *run, ta_string_view name, uint32_t pass) {
    tool *t = run->t;
    chart_lookup before = look_up(run->sides.before, name), after = look_up(run->sides.after, name);
    if (!before.counts || !after.counts) return;
    migration_notes notes;
    chart_diff diff = {t, {before.found, after.found}, name, 0, 0, run->breaking_only, 0, 0, 0};
    if (prepare(t, &diff.pair) != TA_OK) {
        run->failed = 1;
        return;
    }
    if (pass == DIFF_PASS_COUNT) {
        tool_text_init(t, &notes.removed);
        tool_text_init(t, &notes.kinds);
        tool_text_init(t, &notes.moved);
        tool_text_init(t, &notes.data);
        tool_text_init(t, &notes.events);
        diff.notes = &notes;
    }
    chart_settings settings = compare_settings(&diff);
    compare_lists(&diff);
    /* a chart added or removed is one difference, breaking no running
     * actor: those of a chart the new version lacks stay on theirs */
    if (!both_sides(&diff)) diff.differences = 1;
    ta_string_view change = change_of(&diff);
    if (pass == DIFF_PASS_COUNT) {
        run->differences += diff.differences;
        run->breaking += diff.breaking;
        uint32_t *count = !diff.pair.before.chart  ? &run->charts_added
                          : !diff.pair.after.chart ? &run->charts_removed
                          : diff.differences       ? &run->charts_changed
                                                   : &run->charts_unchanged;
        (*count)++;
        explain(&diff, settings, &run->migration);
        return;
    }
    int listed = run->all || (run->breaking_only ? diff.breaking != 0 : diff.differences != 0);
    if (!listed) return;
    ta_report *report = &t->report;
    ta_report_object(report, ta_string(""));
    ta_report_text(report, ta_string("name"), name);
    ta_report_text(report, ta_string("change"), change);
    ta_report_text(report, ta_string("datamodel"), settings.datamodel);
    ta_report_text(report, ta_string("binding"), settings.binding);
    diff.writing = 1;
    diff.differences = diff.breaking = 0;
    compare_lists(&diff);
    ta_report_end(report);
}

/* Every chart of either image, by name: the old image's first. */
static void each_chart(diff_run *run, uint32_t pass) {
    const ta_image_view *before = view_of(run->sides.before), *after = view_of(run->sides.after);
    for (uint32_t c = 0; c < before->chart_count && !run->failed; c++)
        chart_by_name(run, string_of(before, before->charts[c].name), pass);
    for (uint32_t c = 0; c < after->chart_count && !run->failed; c++) {
        ta_string_view name = string_of(after, after->charts[c].name);
        if (ta_image_chart_by_name(&run->sides.before->opened.image.image, name) == TA_NIL)
            chart_by_name(run, name, pass);
    }
}

/* Which event nodes of an image are names its charts use: a transition's
 * descriptor, a send's or a raise's event, a state's done event. The other
 * nodes are only the prefixes of names ("payment" of "payment.timeout").
 * One flag per node, in the arena; null when it refused. */
static uint8_t *named_events(tool *t, const ta_image_view *view) {
    uint8_t *named = (uint8_t *)ta_growable_arena_alloc(t->arena, view->event_node_count + 1u, 1);
    if (!named) return 0;
    uint32_t count = view->event_node_count;
    for (uint32_t i = 0; i < count; i++)
        named[i] = 0;
    for (uint32_t i = 0; i < view->transition_count; i++) {
        const ta_transition_record *transition = &view->transitions[i];
        for (uint32_t e = 0; e < transition->event_count; e++) {
            uint32_t node = view->u32[transition->events_offset + 2 * e];
            if (node < count) named[node] = 1;
        }
    }
    for (uint32_t i = 0; i < view->send_count; i++)
        if (view->sends[i].event < count) named[view->sends[i].event] = 1;
    for (uint32_t i = 0; i < view->state_count; i++)
        if (view->states[i].done_event < count) named[view->states[i].done_event] = 1;
    for (uint32_t pc = 0; pc < view->code_count;) {
        uint32_t opcode = view->code[pc];
        if (opcode >= TA_OPCODE_MAX_ || pc + ta_opcode_length[opcode] > view->code_count) break;
        if (opcode == TA_OPCODE_RAISE && view->code[pc + 1] < count) named[view->code[pc + 1]] = 1;
        pc += ta_opcode_length[opcode];
    }
    return named;
}

/* The image-wide event names `before` uses and `after` does not, as a
 * list. */
static void events_list(tool *t, const char *name, diff_sides sides) {
    const ta_image_view *from = view_of(sides.before), *to = view_of(sides.after);
    const uint8_t *named = named_events(t, from), *named_there = named_events(t, to);
    ta_report_list(&t->report, (ta_report_list_names){ta_string(name), ta_string("event")});
    for (uint32_t i = 0; named && named_there && i < from->event_node_count; i++) {
        ta_string_view event = string_of(from, from->event_nodes[i].name);
        if (!event.length || !named[i]) continue;
        int exact = 0;
        uint32_t there = ta_event_name_lookup(to, event.pointer, event.length, &exact);
        if (!exact || there >= to->event_node_count || !named_there[there])
            ta_report_text(&t->report, ta_string(""), event);
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

static void summary_object(const diff_run *run) {
    ta_report *report = &run->t->report;
    ta_report_object(report, ta_string("summary"));
    ta_report_number(report, ta_string("chartsAdded"), run->charts_added);
    ta_report_number(report, ta_string("chartsRemoved"), run->charts_removed);
    ta_report_number(report, ta_string("chartsChanged"), run->charts_changed);
    ta_report_number(report, ta_string("chartsUnchanged"), run->charts_unchanged);
    ta_report_number(report, ta_string("differences"), run->differences);
    ta_report_number(report, ta_string("breaking"), run->breaking);
    ta_report_end(report);
}

void tool_diff(tool *t) {
    const ta_string_view *arguments = t->parsed->arguments;
    diff_side before = {.path = arguments[0]}, after = {.path = arguments[1]};
    if (tool_open(t, (tool_open_request){1, &arguments[0], TOOL_SELECT_NAMES_ELSEWHERE}, &before.opened) != TA_OK)
        return;
    if (tool_open(t, (tool_open_request){1, &arguments[1], TOOL_SELECT_NAMES_ELSEWHERE}, &after.opened) != TA_OK)
        return;
    diff_sides sides = {&before, &after};
    if (!names_exist(t, sides)) return;
    diff_run run = {.t = t, .sides = sides};
    run.all = tool_count(t, TOOL_DIFF_ALL) != 0;
    run.breaking_only = tool_count(t, TOOL_DIFF_BREAKING) != 0;
    tool_text_init(t, &run.migration);
    each_chart(&run, DIFF_PASS_COUNT);
    ta_report *report = &t->report;
    side_object(t, "old", &before);
    side_object(t, "new", &after);
    ta_report_boolean(report, ta_string("identical"), view_of(&before)->hash == view_of(&after)->hash);
    summary_object(&run);
    ta_report_list(report, (ta_report_list_names){ta_string("charts"), ta_string("chart")});
    each_chart(&run, DIFF_PASS_WRITE);
    ta_report_end(report);
    events_list(t, "eventsAdded", (diff_sides){&after, &before});
    events_list(t, "eventsRemoved", (diff_sides){&before, &after});
    ta_string_view migration = tool_text_view(&run.migration);
    if (migration.length) ta_report_text(report, ta_string("migration"), migration);
    if (run.failed) {
        tool_complain(t, ta_string("out of memory"));
        t->exit = TOOL_EXIT_FAILURE;
        return;
    }
    uint32_t counted = run.breaking_only ? run.breaking : run.differences;
    if (tool_count(t, TOOL_DIFF_EXIT_CODE) && counted) t->exit = TOOL_EXIT_REFUSED;
}
