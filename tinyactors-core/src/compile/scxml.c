/* scxml.c — compile one <scxml> document into the global IR tables.
 *
 * States are collected in document order (preorder); transitions per state
 * are contiguous; executable content is emitted directly as flattened
 * bytecode. Event names are recorded as provisional event name indices and
 * patched to trie ids by ta_compiler_link_names() once all charts are known.
 */
#include "compile/compile_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 200

TA_STATIC_ASSERT(TA_IMAGE_MAX_NESTING == TA_COMPILE_MAX_CONTENT_NESTING, content_nesting_limit);

typedef struct chart_builder {
    ta_compiler *compiler;
    uint32_t index;
    const char *file;
    const char *text;
    uint32_t length; /* of text */
    uint32_t first_state, first_transition, first_expression, first_send, first_invoke, first_data,
        first_code;
    ta_vector nodes;              /* ta_xml_node* per local state */
    ta_string_map ids;            /* state id -> local index */
    ta_string_map invoke_ids;     /* static invoke id -> slot */
    ta_string_map expression_map; /* kind+source -> local expr */
    uint32_t foreach_depth, max_foreach, max_depth;
    uint32_t nesting; /* <if>/<foreach> nesting of the executable content being emitted */
    uint32_t invoke_id_count;
    uint32_t inline_sequence;
} chart_builder;

/* ---- small helpers ---------------------------------------------------------- */

static int is_scxml(const ta_xml_node *node) {
    return node->namespace_uri[0] == 0 || strcmp(node->namespace_uri, TA_SCXML_NAMESPACE) == 0;
}
static int el(const ta_xml_node *node, const char *name) {
    return is_scxml(node) && strcmp(node->name, name) == 0;
}
static int is_state_el(const ta_xml_node *node) {
    return el(node, "state") || el(node, "parallel") || el(node, "final") || el(node, "history");
}

typedef struct compile_message {
    const char *format, *argument;
} compile_message;

static void compile_error(chart_builder *builder, const ta_xml_node *node, compile_message message) {
    ta_diagnostics_add(&builder->compiler->diagnostics, &(ta_diagnostic_input){
        TA_DIAGNOSTIC_ERROR, builder->file, node ? node->line : 0, node ? node->column : 0,
        message.format}, message.argument ? message.argument : "");
}
static void compile_warning(chart_builder *builder, const ta_xml_node *node, compile_message message) {
    ta_diagnostics_add(&builder->compiler->diagnostics, &(ta_diagnostic_input){
        TA_DIAGNOSTIC_WARNING, builder->file, node ? node->line : 0, node ? node->column : 0,
        message.format}, message.argument ? message.argument : "");
}

static ta_state_record *state_at(chart_builder *builder, uint32_t i) {
    return &TA_VECTOR_AT(&builder->compiler->states, ta_state_record, builder->first_state + i);
}
static ta_transition_record *transition_at(chart_builder *builder, uint32_t i) {
    return &TA_VECTOR_AT(&builder->compiler->transitions, ta_transition_record,
                         builder->first_transition + i);
}
static uint32_t state_count(chart_builder *builder) {
    return builder->compiler->states.length - builder->first_state;
}
static uint32_t transition_count(chart_builder *builder) {
    return builder->compiler->transitions.length - builder->first_transition;
}
static uint32_t pc(chart_builder *builder) {
    return builder->compiler->code.length;
}
static void emit(chart_builder *builder, uint32_t w) {
    ta_vector_push_u32(&builder->compiler->code, w);
}
static void patch(chart_builder *builder, uint32_t at, uint32_t v) {
    TA_VECTOR_AT(&builder->compiler->code, uint32_t, at) = v;
}
static uint32_t str(chart_builder *builder, const char *s) {
    return ta_compiler_string(builder->compiler, s, (uint32_t)strlen(s));
}

static int is_whitespace(char child) {
    return child == ' ' || child == '\t' || child == '\n' || child == '\r';
}

/* Iterate whitespace-separated tokens. */
static int next_token(const char **p, const char **tok, uint32_t *length) {
    const char *s = *p;
    while (*s && is_whitespace(*s))
        s++;
    if (!*s) return 0;
    const char *b = s;
    while (*s && !is_whitespace(*s))
        s++;
    *tok = b;
    *length = (uint32_t)(s - b);
    *p = s;
    return 1;
}

typedef struct expression_input {
    uint32_t kind;
    ta_string_view source;
    const ta_xml_node *at;
} expression_input;

static uint32_t intern_expression(chart_builder *builder, const expression_input *input) {
    uint32_t kind = input->kind, length = input->source.length;
    const char *source = input->source.pointer;
    const ta_xml_node *at = input->at;
    char *key = (char *)ta_compile_alloc(&builder->compiler->arena, length + 2, &builder->compiler->out_of_memory);
    if (!key) return 0;
    key[0] = (char)('0' + kind);
    memcpy(key + 1, source, length);
    uint32_t hit = ta_string_map_get(&builder->expression_map, key, length + 1);
    if (hit != TA_NIL) return hit;
    ta_expression_record *expression =
        (ta_expression_record *)ta_vector_push(&builder->compiler->expressions);
    if (!expression) return 0;
    expression->kind = kind;
    expression->source = ta_compiler_string(builder->compiler, source, length);
    expression->line = at ? at->line : 0;
    expression->column = at ? at->column : 0;
    uint32_t id = builder->compiler->expressions.length - 1 - builder->first_expression;
    ta_string_map_put(&builder->expression_map, key, length + 1, id);
    return id;
}

/* An event name is a path in the image's event trie, which stores the full
 * name of every prefix: the parts are limited so that stays linear in the
 * document. */
#define MAX_EVENT_PARTS 64

static uint32_t event_name(chart_builder *builder, const ta_xml_node *at, const char *s,
                           uint32_t length) {
    uint32_t parts = 1;
    for (uint32_t i = 0; i < length; i++)
        parts += s[i] == '.';
    if (parts > MAX_EVENT_PARTS) {
        compile_error(builder, at, (compile_message){"event name with more than 64 dot-separated parts%s", NULL});
        length = 0;
    }
    return ta_compiler_event_name(builder->compiler, s, length);
}

static uint32_t attribute_expression(chart_builder *builder, const ta_xml_node *node,
                                     const char *name, uint32_t kind) {
    const ta_xml_attribute *attribute = ta_xml_find_attribute(node, name);
    return attribute
               ? intern_expression(builder, &(expression_input){kind, {attribute->value, attribute->value_length}, node})
               : TA_NIL;
}

static uint32_t attr_str(chart_builder *builder, const ta_xml_node *node, const char *name) {
    const ta_xml_attribute *attribute = ta_xml_find_attribute(node, name);
    return attribute
               ? ta_compiler_string(builder->compiler, attribute->value, attribute->value_length)
               : TA_NIL;
}

static int has_element_child(const ta_xml_node *node) {
    return node->first != NULL;
}

/* An SCXML document written literally inside <assign>/<data> content may be
 * invoked later via <content expr>: compile it as a chart named by its
 * normalized text so the runtime can resolve the dynamic document. */
static void precompile_literal_document(chart_builder *builder, const ta_xml_node *node) {
    if (!node->first || !el(node->first, "scxml")) return;
    const char *s = builder->text + node->inner_start;
    uint32_t length = node->inner_end - node->inner_start;
    char key[4096];
    uint32_t kl = ta_compiler_document_key(s, length, key, sizeof key);
    if (!kl) return;
    ta_compiler_queue(builder->compiler, &(ta_pending_document){(ta_xml_node *)node->first, key, builder->text, builder->length});
}

/* Content of <content>/<data>/<assign>: raw markup if it has element
 * children, else unescaped text; trimmed. */
static uint32_t content_expression(chart_builder *builder, const ta_xml_node *node) {
    const char *s;
    uint32_t length;
    if (has_element_child(node)) {
        s = builder->text + node->inner_start;
        length = node->inner_end - node->inner_start;
    } else {
        s = node->text;
        length = node->text_length;
    }
    while (length && is_whitespace(*s)) {
        s++;
        length--;
    }
    while (length && is_whitespace(s[length - 1]))
        length--;
    return intern_expression(builder, &(expression_input){TA_EXPRESSION_CONTENT, {s, length}, node});
}

static const ta_xml_node *child(const ta_xml_node *node, const char *name) {
    for (const ta_xml_node *child_node = node->first; child_node; child_node = child_node->next)
        if (el(child_node, name)) return child_node;
    return NULL;
}

/* Fetch an external src at compile time through the resolver. */
typedef struct source_fetch {
    const ta_xml_node *at;
    const char *source;
} source_fetch;

static int fetch_source(chart_builder *builder, const source_fetch *fetch, ta_source *out) {
    const ta_compile_options *options = builder->compiler->options;
    ta_resolve_request request = {{builder->file, (uint32_t)strlen(builder->file)},
                                  {fetch->source, (uint32_t)strlen(fetch->source)},
                                  &builder->compiler->arena};
    if (!options || !options->resolve) {
        compile_error(builder, fetch->at,
                      (compile_message){"src=\"%s\" requires a resolver", fetch->source});
        return 0;
    }
    return ta_compiler_resolve(builder->compiler, &request, out) == TA_OK;
}

static int is_scxml_type(const char *t) {
    return !t || strcmp(t, "scxml") == 0 || strcmp(t, "http://www.w3.org/TR/scxml/") == 0 ||
           strcmp(t, "http://www.w3.org/TR/scxml") == 0 ||
           strcmp(t, "http://www.w3.org/TR/scxml/#SCXMLEventProcessor") == 0;
}

/* ---- durations ---------------------------------------------------------------- */

int ta_parse_duration_ns(const char *s, uint64_t *out) {
    while (is_whitespace(*s))
        s++;
    uint64_t ip = 0, frac = 0, scale = 1;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        ip = ip * 10 + (uint64_t)(*s++ - '0');
        digits++;
        if (ip > 1000000000ull * 3600) return 0;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (scale < 1000000000ull) {
                frac = frac * 10 + (uint64_t)(*s - '0');
                scale *= 10;
            }
            s++;
            digits++;
        }
    }
    if (!digits) return 0;
    uint64_t unit;
    if (s[0] == 'm' && s[1] == 's') {
        unit = 1000000ull;
        s += 2;
    } else if (s[0] == 's') {
        unit = 1000000000ull;
        s += 1;
    } else {
        return 0;
    }
    while (is_whitespace(*s))
        s++;
    if (*s) return 0;
    /* beyond what 64 bits of nanoseconds hold (~584 years): never, not a wrapped soon */
    *out = ip > (UINT64_MAX - unit) / unit ? UINT64_MAX : ip * unit + frac * unit / scale;
    return 1;
}

/* ---- state collection ----------------------------------------------------------- */

static void state_defaults(ta_state_record *state) {
    memset(state, 0, sizeof *state);
    state->parent = state->id = state->initial = state->onentry = state->onexit = TA_NIL;
    state->donedata = state->done_event = state->history_offset = TA_NIL;
}

static uint32_t enter_state(chart_builder *builder, ta_xml_node *node, uint32_t parent,
                            uint32_t depth) {
    uint32_t index = state_count(builder);
    ta_state_record *state = (ta_state_record *)ta_vector_push(&builder->compiler->states);
    ta_xml_node **np = (ta_xml_node **)ta_vector_push(&builder->nodes);
    if (!state || !np) return TA_NIL;
    state_defaults(state);
    *np = node;
    state->parent = parent;
    state->depth = (uint8_t)depth;
    if (depth > builder->max_depth) builder->max_depth = depth;
    return index;
}

static void leave_state(chart_builder *builder, ta_xml_node *node, uint32_t index) {
    ta_state_record *state = state_at(builder, index);
    state->end = state_count(builder);
    int has_children = state->end > index + 1;
    int has_real_child = 0;
    for (uint32_t k = index + 1; k < state->end; k = state_at(builder, k)->end)
        if (!el(((ta_xml_node **)(void *)builder->nodes.data)[k], "history")) has_real_child = 1;
    if (state->parent == TA_NIL) {
        state->kind = TA_STATE_COMPOUND;
        state->flags |= TA_STATE_FLAG_ROOT;
        if (!has_real_child)
            compile_error(builder, node, (compile_message){"<scxml> must contain at least one state%s", NULL});
    } else if (el(node, "state")) {
        state->kind = has_children ? TA_STATE_COMPOUND : TA_STATE_ATOMIC;
        if (has_children && !has_real_child)
            compile_error(builder, node, (compile_message){"compound state needs a non-history child state%s", NULL});
    } else if (el(node, "parallel")) {
        state->kind = has_children ? TA_STATE_PARALLEL : TA_STATE_ATOMIC;
    } else if (el(node, "final")) {
        state->kind = TA_STATE_FINAL;
    } else {
        const char *t = ta_xml_attribute_value(node, "type");
        state->kind =
            (t && strcmp(t, "deep") == 0) ? TA_STATE_HISTORY_DEEP : TA_STATE_HISTORY_SHALLOW;
    }
}

/* Collect the states in document order (preorder) with an explicit stack:
 * native stack use does not depend on the nesting. */
typedef struct cframe {
    ta_xml_node *count, *next; /* the state's element, its next child to visit */
    uint32_t index;
} cframe;

static void collect(chart_builder *builder, ta_xml_node *root) {
    uint32_t index = enter_state(builder, root, TA_NIL, 0);
    if (index == TA_NIL) return;
    ta_vector stack;
    ta_vector_init(&stack, builder->compiler->allocator, sizeof(cframe));
    cframe *f = (cframe *)ta_vector_push(&stack);
    if (f) {
        f->count = root;
        f->next = root->first;
        f->index = index;
    }
    while (stack.length && !ta_compiler_stop(builder->compiler)) {
        f = &TA_VECTOR_AT(&stack, cframe, stack.length - 1);
        ta_xml_node *child = f->next, *node = f->count;
        if (!child) {
            leave_state(builder, node, f->index);
            stack.length--;
            continue;
        }
        f->next = child->next;
        if (!is_state_el(child)) continue;
        if (el(node, "history") || el(node, "final")) {
            compile_error(builder, child, (compile_message){"<%s> cannot contain states", node->name});
            continue;
        }
        uint32_t depth = stack.length; /* the root is at depth 0 */
        if (depth > MAX_DEPTH) {
            compile_error(builder, child, (compile_message){"state nesting too deep%s", NULL});
            continue;
        }
        uint32_t parent = f->index;
        uint32_t k = enter_state(builder, child, parent, depth);
        cframe *nf = k == TA_NIL ? NULL : (cframe *)ta_vector_push(&stack);
        if (!nf) break; /* out of memory: reported by the caller */
        nf->count = child;
        nf->next = child->first;
        nf->index = k;
    }
    ta_vector_free(&stack);
}

static ta_xml_node *node_at(chart_builder *builder, uint32_t i) {
    return ((ta_xml_node **)(void *)builder->nodes.data)[i];
}

static int is_descendant(chart_builder *builder, uint32_t s, uint32_t anc) { /* proper descendant */
    return s > anc && s < state_at(builder, anc)->end;
}

static uint32_t resolve_id(chart_builder *builder, const ta_xml_node *at, const char *id,
                           uint32_t length) {
    uint32_t i = ta_string_map_get(&builder->ids, id, length);
    if (i == TA_NIL) {
        char buffer[128];
        uint32_t l = length < sizeof buffer - 1 ? length : (uint32_t)sizeof buffer - 1;
        memcpy(buffer, id, l);
        buffer[l] = 0;
        compile_error(builder, at, (compile_message){"unknown state id '%s'", buffer});
    }
    return i;
}

/* ---- captures, sends ------------------------------------------------------------- */

static uint32_t capture(chart_builder *builder, const ta_xml_node *node, const char *namelist,
                        int skip_content) {
    ta_capture_record capture_record;
    memset(&capture_record, 0, sizeof capture_record);
    capture_record.content = TA_NIL;
    capture_record.namelist_offset = builder->compiler->u32.length;
    if (namelist) {
        const char *p = namelist, *tok;
        uint32_t length;
        while (next_token(&p, &tok, &length)) {
            ta_vector_push_u32(
                &builder->compiler->u32,
                intern_expression(builder, &(expression_input){TA_EXPRESSION_LOCATION, {tok, length}, node}));
            capture_record.namelist_count++;
        }
    }
    capture_record.params_offset = builder->compiler->u32.length;
    for (const ta_xml_node *child = node->first; child; child = child->next) {
        if (el(child, "param")) {
            const ta_xml_attribute *nm = ta_xml_find_attribute(child, "name");
            if (!nm) {
                compile_error(builder, child, (compile_message){"<param> requires 'name'%s", NULL});
                continue;
            }
            uint32_t e = attribute_expression(builder, child, "expr", TA_EXPRESSION_VALUE);
            if (e == TA_NIL)
                e = attribute_expression(builder, child, "location", TA_EXPRESSION_LOCATION);
            if (e == TA_NIL) {
                compile_error(builder, child, (compile_message){"<param> requires 'expr' or 'location'%s", NULL});
                continue;
            }
            ta_vector_push_u32(&builder->compiler->u32,
                               ta_compiler_string(builder->compiler, nm->value, nm->value_length));
            ta_vector_push_u32(&builder->compiler->u32, e);
            capture_record.param_count++;
        } else if (el(child, "content") && !skip_content) {
            if (capture_record.content != TA_NIL)
                compile_error(builder, child, (compile_message){"duplicate <content>%s", NULL});
            uint32_t e = attribute_expression(builder, child, "expr", TA_EXPRESSION_VALUE);
            capture_record.content = e != TA_NIL ? e : content_expression(builder, child);
        }
    }
    if (!capture_record.namelist_count && !capture_record.param_count &&
        capture_record.content == TA_NIL)
        return TA_NIL;
    if (capture_record.content != TA_NIL &&
        (capture_record.namelist_count || capture_record.param_count))
        compile_error(builder, node, (compile_message){"<content> cannot be combined with namelist/<param>%s", NULL});
    ta_capture_record *dst = (ta_capture_record *)ta_vector_push(&builder->compiler->captures);
    if (!dst) return TA_NIL;
    *dst = capture_record;
    return builder->compiler->captures.length - 1;
}

static uint32_t send_def(chart_builder *builder, const ta_xml_node *node) {
    ta_send_record send;
    memset(&send, 0, sizeof send);
    send.event = send.eventexpr = send.target = send.targetexpr = send.type = send.typeexpr =
        TA_NIL;
    send.id = send.idlocation = send.delayexpr = send.capture = TA_NIL;
    const char *event = ta_xml_attribute_value(node, "event");
    if (event) send.event = event_name(builder, node, event, (uint32_t)strlen(event));
    send.eventexpr = attribute_expression(builder, node, "eventexpr", TA_EXPRESSION_STRING);
    if (event && send.eventexpr != TA_NIL)
        compile_error(builder, node, (compile_message){"<send> has both event and eventexpr%s", NULL});
    const char *tg = ta_xml_attribute_value(node, "target");
    send.targetexpr = attribute_expression(builder, node, "targetexpr", TA_EXPRESSION_STRING);
    if (tg && send.targetexpr != TA_NIL)
        compile_error(builder, node, (compile_message){"<send> has both target and targetexpr%s", NULL});
    if (send.targetexpr != TA_NIL) {
        send.target_kind = TA_SEND_TARGET_EXPRESSION;
    } else if (!tg) {
        send.target_kind = TA_SEND_TARGET_NONE;
    } else if (strcmp(tg, "#_internal") == 0) {
        send.target_kind = TA_SEND_TARGET_INTERNAL;
    } else if (strcmp(tg, "#_parent") == 0) {
        send.target_kind = TA_SEND_TARGET_PARENT;
    } else if (strncmp(tg, "#_", 2) == 0 && strncmp(tg, "#_scxml_", 8) != 0 &&
               ta_string_map_get(&builder->invoke_ids, tg + 2, (uint32_t)strlen(tg + 2)) !=
                   TA_NIL) {
        send.target_kind = TA_SEND_TARGET_INVOKE;
        send.target = ta_string_map_get(&builder->invoke_ids, tg + 2, (uint32_t)strlen(tg + 2));
    } else {
        send.target_kind = TA_SEND_TARGET_STRING;
        send.target = str(builder, tg);
    }
    const char *ty = ta_xml_attribute_value(node, "type");
    if (ty && !is_scxml_type(ty)) send.type = str(builder, ty);
    send.typeexpr = attribute_expression(builder, node, "typeexpr", TA_EXPRESSION_STRING);
    send.id = attr_str(builder, node, "id");
    send.idlocation = attribute_expression(builder, node, "idlocation", TA_EXPRESSION_LOCATION);
    if (send.id != TA_NIL && send.idlocation != TA_NIL)
        compile_error(builder, node, (compile_message){"<send> has both id and idlocation%s", NULL});
    const char *delay = ta_xml_attribute_value(node, "delay");
    if (delay && !ta_parse_duration_ns(delay, &send.delay_ns))
        compile_error(builder, node, (compile_message){"invalid delay '%s'", delay});
    send.delayexpr = attribute_expression(builder, node, "delayexpr", TA_EXPRESSION_STRING);
    if ((delay || send.delayexpr != TA_NIL) && send.target_kind == TA_SEND_TARGET_INTERNAL)
        compile_error(builder, node, (compile_message){"delay is not allowed with target #_internal%s", NULL});
    send.capture = capture(builder, node, ta_xml_attribute_value(node, "namelist"), 0);
    if (send.event == TA_NIL && send.eventexpr == TA_NIL && send.type == TA_NIL &&
        send.typeexpr == TA_NIL)
        compile_error(builder, node, (compile_message){"<send> to the SCXML processor requires event or eventexpr%s", NULL});
    ta_send_record *dst = (ta_send_record *)ta_vector_push(&builder->compiler->sends);
    if (!dst) return 0;
    *dst = send;
    return builder->compiler->sends.length - 1 - builder->first_send;
}

/* ---- executable content ---------------------------------------------------------- */

static void exec_el(chart_builder *c, const ta_xml_node *n);

static void exec_children(chart_builder *builder, const ta_xml_node *container) {
    for (const ta_xml_node *child = container->first; child; child = child->next)
        exec_el(builder, child);
}

static void exec_if(chart_builder *builder, const ta_xml_node *node) {
    uint32_t condition = attribute_expression(builder, node, "cond", TA_EXPRESSION_CONDITION);
    if (condition == TA_NIL) {
        compile_error(builder, node, (compile_message){"<if> requires 'cond'%s", NULL});
        condition = 0;
    }
    ta_vector jmps;
    ta_vector_init(&jmps, builder->compiler->allocator, sizeof(uint32_t));
    emit(builder, TA_OPCODE_JUMP_IF_FALSE);
    emit(builder, condition);
    uint32_t pending = pc(builder);
    emit(builder, 0);
    int seen_else = 0;
    for (const ta_xml_node *child = node->first; child; child = child->next) {
        if (el(child, "elseif") || el(child, "else")) {
            if (seen_else) compile_error(builder, child, (compile_message){"<%s> after <else>", child->name});
            emit(builder, TA_OPCODE_JUMP);
            ta_vector_push_u32(&jmps, pc(builder));
            emit(builder, 0);
            if (pending != TA_NIL) patch(builder, pending, pc(builder));
            pending = TA_NIL;
            if (el(child, "elseif")) {
                uint32_t c2 = attribute_expression(builder, child, "cond", TA_EXPRESSION_CONDITION);
                if (c2 == TA_NIL) {
                    compile_error(builder, child, (compile_message){"<elseif> requires 'cond'%s", NULL});
                    c2 = 0;
                }
                emit(builder, TA_OPCODE_JUMP_IF_FALSE);
                emit(builder, c2);
                pending = pc(builder);
                emit(builder, 0);
            } else {
                seen_else = 1;
            }
        } else {
            exec_el(builder, child);
        }
    }
    if (pending != TA_NIL) patch(builder, pending, pc(builder));
    for (uint32_t i = 0; i < jmps.length; i++)
        patch(builder, TA_VECTOR_AT(&jmps, uint32_t, i), pc(builder));
    ta_vector_free(&jmps);
}

/* <if>/<foreach> bodies recurse: the nesting is bounded (and so is the
 * native stack) by TA_IMAGE_MAX_NESTING, which also keeps image verification of
 * the jumps linear. */
static int nest_enter(chart_builder *builder, const ta_xml_node *node) {
    if (builder->nesting >= TA_IMAGE_MAX_NESTING) {
        compile_error(builder, node, (compile_message){"<%s> nested too deep in executable content", node->name});
        return 0;
    }
    builder->nesting++;
    return 1;
}

static void exec_el(chart_builder *builder, const ta_xml_node *node) {
    if (ta_compiler_stop(builder->compiler)) return;
    if (!is_scxml(node)) {
        compile_warning(builder, node, (compile_message){"ignoring foreign executable content <%s>", node->name});
        return;
    }
    const char *nm = node->name;
    if (!strcmp(nm, "raise")) {
        const char *event = ta_xml_attribute_value(node, "event");
        if (!event) {
            compile_error(builder, node, (compile_message){"<raise> requires 'event'%s", NULL});
            return;
        }
        emit(builder, TA_OPCODE_RAISE);
        emit(builder, event_name(builder, node, event, (uint32_t)strlen(event)));
    } else if (!strcmp(nm, "log")) {
        emit(builder, TA_OPCODE_LOG);
        emit(builder, attr_str(builder, node, "label"));
        emit(builder, attribute_expression(builder, node, "expr", TA_EXPRESSION_VALUE));
    } else if (!strcmp(nm, "assign")) {
        uint32_t loc = attribute_expression(builder, node, "location", TA_EXPRESSION_LOCATION);
        uint32_t value = attribute_expression(builder, node, "expr", TA_EXPRESSION_VALUE);
        if (loc == TA_NIL) {
            compile_error(builder, node, (compile_message){"<assign> requires 'location'%s", NULL});
            return;
        }
        if (value == TA_NIL) {
            value = content_expression(builder, node);
            precompile_literal_document(builder, node);
        }
        emit(builder, TA_OPCODE_ASSIGN);
        emit(builder, loc);
        emit(builder, value);
    } else if (!strcmp(nm, "script")) {
        const char *source = ta_xml_attribute_value(node, "src");
        uint32_t e, kind = node->parent && el(node->parent, "scxml") ? TA_EXPRESSION_GLOBAL_SCRIPT /* <scxml>'s */
                                                                  : TA_EXPRESSION_SCRIPT;
        if (source) {
            ta_source s;
            if (!fetch_source(builder, &(source_fetch){node, source}, &s)) return;
            e = intern_expression(builder, &(expression_input){kind, {s.text.pointer, s.text.length}, node});
        } else {
            e = intern_expression(builder, &(expression_input){kind, {node->text, node->text_length}, node});
        }
        emit(builder, TA_OPCODE_SCRIPT);
        emit(builder, e);
    } else if (!strcmp(nm, "if")) {
        if (!nest_enter(builder, node)) return;
        exec_if(builder, node);
        builder->nesting--;
    } else if (!strcmp(nm, "foreach")) {
        uint32_t arr = attribute_expression(builder, node, "array", TA_EXPRESSION_VALUE);
        uint32_t item = attribute_expression(builder, node, "item", TA_EXPRESSION_ITEM);
        uint32_t index = attribute_expression(builder, node, "index", TA_EXPRESSION_ITEM);
        if (arr == TA_NIL || item == TA_NIL) {
            compile_error(builder, node, (compile_message){"<foreach> requires 'array' and 'item'%s", NULL});
            return;
        }
        if (!nest_enter(builder, node)) return;
        emit(builder, TA_OPCODE_FOREACH_BEGIN);
        emit(builder, arr);
        emit(builder, item);
        emit(builder, index);
        uint32_t end_at = pc(builder);
        emit(builder, 0);
        uint32_t body = pc(builder);
        builder->foreach_depth++;
        if (builder->foreach_depth > builder->max_foreach)
            builder->max_foreach = builder->foreach_depth;
        exec_children(builder, node);
        builder->foreach_depth--;
        builder->nesting--;
        emit(builder, TA_OPCODE_FOREACH_NEXT);
        emit(builder, body);
        patch(builder, end_at, pc(builder));
    } else if (!strcmp(nm, "send")) {
        uint32_t s = send_def(builder, node);
        emit(builder, TA_OPCODE_SEND);
        emit(builder, s);
    } else if (!strcmp(nm, "cancel")) {
        const char *sid = ta_xml_attribute_value(node, "sendid");
        uint32_t e = attribute_expression(builder, node, "sendidexpr", TA_EXPRESSION_STRING);
        if (sid) {
            emit(builder, TA_OPCODE_CANCEL);
            emit(builder, str(builder, sid));
        } else if (e != TA_NIL) {
            emit(builder, TA_OPCODE_CANCEL_EXPRESSION);
            emit(builder, e);
        } else {
            compile_error(builder, node, (compile_message){"<cancel> requires 'sendid' or 'sendidexpr'%s", NULL});
        }
    } else if (!strcmp(nm, "elseif") || !strcmp(nm, "else")) {
        compile_error(builder, node, (compile_message){"<%s> outside <if>", nm});
    } else {
        compile_error(builder, node, (compile_message){"unknown executable content <%s>", nm});
    }
}

/* One BLOCK holding the executable children of `container`; NIL if empty. */
static uint32_t block_one(chart_builder *builder, const ta_xml_node *container) {
    if (!container || !container->first) return TA_NIL;
    uint32_t start = pc(builder);
    emit(builder, TA_OPCODE_BLOCK);
    uint32_t end_at = pc(builder);
    emit(builder, 0);
    exec_children(builder, container);
    patch(builder, end_at, pc(builder));
    emit(builder, TA_OPCODE_END);
    return start;
}

/* One BLOCK per child element named `name` (e.g. each <onentry>). */
static uint32_t block_seq(chart_builder *builder, const ta_xml_node *parent, const char *name) {
    uint32_t start = TA_NIL;
    for (const ta_xml_node *child = parent->first; child; child = child->next) {
        if (!el(child, name)) continue;
        if (start == TA_NIL) start = pc(builder);
        emit(builder, TA_OPCODE_BLOCK);
        uint32_t end_at = pc(builder);
        emit(builder, 0);
        exec_children(builder, child);
        patch(builder, end_at, pc(builder));
    }
    if (start != TA_NIL) emit(builder, TA_OPCODE_END);
    return start;
}

/* One BLOCK per child element named `name`, each block being that element
 * itself (top-level <script>). */
static uint32_t block_each(chart_builder *builder, const ta_xml_node *parent, const char *name) {
    uint32_t start = TA_NIL;
    for (const ta_xml_node *child = parent->first; child; child = child->next) {
        if (!el(child, name)) continue;
        if (start == TA_NIL) start = pc(builder);
        emit(builder, TA_OPCODE_BLOCK);
        uint32_t end_at = pc(builder);
        emit(builder, 0);
        exec_el(builder, child);
        patch(builder, end_at, pc(builder));
    }
    if (start != TA_NIL) emit(builder, TA_OPCODE_END);
    return start;
}

/* ---- transitions ----------------------------------------------------------------- */

static void add_descriptor(chart_builder *builder, const ta_xml_node *at, const char *tok,
                           uint32_t length) {
    if (length == 1 && tok[0] == '*') length = 0;
    if (length >= 2 && tok[length - 1] == '*' && tok[length - 2] == '.') length -= 2;
    while (length && tok[length - 1] == '.')
        length--;
    ta_vector_push_u32(&builder->compiler->u32, event_name(builder, at, tok, length));
    ta_vector_push_u32(&builder->compiler->u32,
                       TA_NIL); /* patched to the interval end by the linker */
}

static uint32_t least_common_compound_ancestor(chart_builder *builder, uint32_t source,
                                               const uint32_t *tg, uint32_t n) {
    for (uint32_t a = state_at(builder, source)->parent; a != TA_NIL;
         a = state_at(builder, a)->parent) {
        if (state_at(builder, a)->kind != TA_STATE_COMPOUND) continue;
        uint32_t k = 0;
        while (k < n && is_descendant(builder, tg[k], a))
            k++;
        if (k == n) return a;
    }
    return 0;
}

static uint32_t add_targets(chart_builder *builder, const ta_xml_node *at, const char *targets,
                            uint16_t *out_count, uint16_t *flags) {
    uint32_t offset = builder->compiler->u32.length;
    const char *p = targets, *tok;
    uint32_t length;
    uint16_t n = 0;
    while (next_token(&p, &tok, &length)) {
        if (n == UINT16_MAX) {
            compile_error(builder, at, (compile_message){"more than 65535 targets%s", NULL});
            break;
        }
        uint32_t s = resolve_id(builder, at, tok, length);
        if (s == TA_NIL) continue;
        if (s == 0) {
            compile_error(builder, at, (compile_message){"the <scxml> element is not a valid target%s", NULL});
            continue;
        }
        if (state_at(builder, s)->kind >= TA_STATE_HISTORY_SHALLOW)
            *flags |= TA_TRANSITION_FLAG_HISTORY_TARGET;
        ta_vector_push_u32(&builder->compiler->u32, s);
        n++;
    }
    *out_count = n;
    return offset;
}

static void transition(chart_builder *builder, const ta_xml_node *node, uint32_t source) {
    if (ta_compiler_stop(builder->compiler)) return;
    ta_transition_record transition_record;
    memset(&transition_record, 0, sizeof transition_record);
    transition_record.source = source;
    transition_record.domain = TA_NIL;
    transition_record.condition =
        attribute_expression(builder, node, "cond", TA_EXPRESSION_CONDITION);
    const char *event = ta_xml_attribute_value(node, "event");
    transition_record.events_offset = builder->compiler->u32.length;
    if (event) {
        const char *p = event, *tok;
        uint32_t length;
        while (next_token(&p, &tok, &length)) {
            if (transition_record.event_count == UINT16_MAX) {
                compile_error(builder, node, (compile_message){"more than 65535 event descriptors%s", NULL});
                break;
            }
            add_descriptor(builder, node, tok, length);
            transition_record.event_count++;
        }
        if (!transition_record.event_count)
            compile_error(builder, node, (compile_message){"empty event attribute%s", NULL});
    }
    const char *type = ta_xml_attribute_value(node, "type");
    if (type && strcmp(type, "internal") == 0)
        transition_record.flags |= TA_TRANSITION_FLAG_INTERNAL;
    else if (type && strcmp(type, "external") != 0)
        compile_error(builder, node, (compile_message){"invalid transition type '%s'", type});
    const char *tg = ta_xml_attribute_value(node, "target");
    if (tg)
        transition_record.targets_offset = add_targets(
            builder, node, tg, &transition_record.target_count, &transition_record.flags);
    else
        transition_record.targets_offset = builder->compiler->u32.length;
    transition_record.action = block_one(builder, node);
    if (transition_record.target_count &&
        !(transition_record.flags & TA_TRANSITION_FLAG_HISTORY_TARGET)) {
        const uint32_t *tgs =
            &TA_VECTOR_AT(&builder->compiler->u32, uint32_t, transition_record.targets_offset);
        uint32_t k = 0;
        if ((transition_record.flags & TA_TRANSITION_FLAG_INTERNAL) &&
            state_at(builder, source)->kind == TA_STATE_COMPOUND) {
            while (k < transition_record.target_count && is_descendant(builder, tgs[k], source))
                k++;
        }
        if ((transition_record.flags & TA_TRANSITION_FLAG_INTERNAL) &&
            state_at(builder, source)->kind == TA_STATE_COMPOUND &&
            k == transition_record.target_count)
            transition_record.domain = source;
        else
            transition_record.domain = least_common_compound_ancestor(
                builder, source, tgs, transition_record.target_count);
    }
    ta_transition_record *dst =
        (ta_transition_record *)ta_vector_push(&builder->compiler->transitions);
    if (dst) *dst = transition_record;
}

/* Initial / history-default pseudo transition; `domain` = entry ancestor. */
static uint32_t pseudo_transition(chart_builder *builder, const ta_xml_node *at, uint32_t source,
                                  uint32_t domain, const char *targets, uint32_t single_target,
                                  const ta_xml_node *content) {
    ta_transition_record transition;
    memset(&transition, 0, sizeof transition);
    transition.source = source;
    transition.domain = domain;
    transition.condition = TA_NIL;
    transition.events_offset = builder->compiler->u32.length;
    transition.flags = TA_TRANSITION_FLAG_INITIAL;
    if (targets) {
        transition.targets_offset =
            add_targets(builder, at, targets, &transition.target_count, &transition.flags);
    } else {
        transition.targets_offset = builder->compiler->u32.length;
        if (single_target != TA_NIL) {
            ta_vector_push_u32(&builder->compiler->u32, single_target);
            transition.target_count = 1;
            if (state_at(builder, single_target)->kind >= TA_STATE_HISTORY_SHALLOW)
                transition.flags |= TA_TRANSITION_FLAG_HISTORY_TARGET;
        }
    }
    for (uint32_t k = 0; k < transition.target_count; k++) {
        uint32_t s = TA_VECTOR_AT(&builder->compiler->u32, uint32_t, transition.targets_offset + k);
        if (!is_descendant(builder, s, domain))
            compile_error(builder, at, (compile_message){"initial/default target outside its state%s", NULL});
    }
    if (!transition.target_count)
        compile_error(builder, at, (compile_message){"initial/default transition without target%s", NULL});
    transition.action = block_one(builder, content);
    ta_vector_push(&builder->compiler->transitions);
    *transition_at(builder, transition_count(builder) - 1) = transition;
    return transition_count(builder) - 1;
}

/* ---- invokes, data -------------------------------------------------------------- */

static uint32_t resolve_chart(chart_builder *builder, const ta_xml_node *at, const char *source) {
    uint32_t index =
        ta_string_map_get(&builder->compiler->source_map, source, (uint32_t)strlen(source));
    if (index != TA_NIL) return index;
    ta_source s;
    if (!fetch_source(builder, &(source_fetch){at, source}, &s)) return TA_NIL;
    /* key by the resolver's canonical name so nested src resolve relative to
     * the real location and repeated references dedupe */
    char name[1024];
    const char *key = source;
    if (s.name.pointer && s.name.length && s.name.length < sizeof name) {
        memcpy(name, s.name.pointer, s.name.length);
        name[s.name.length] = 0;
        key = name;
    }
    uint32_t q = ta_compiler_queue(builder->compiler, &(ta_pending_document){NULL, key, s.text.pointer, s.text.length});
    if (q != TA_NIL && key != source) {
        char *alias = ta_compile_copy(&builder->compiler->arena, (ta_string_view){source, (uint32_t)(strlen(source))}, &builder->compiler->out_of_memory);
        if (alias)
            ta_string_map_put(&builder->compiler->source_map, alias, (uint32_t)strlen(alias), q);
    }
    return q;
}

static void invoke(chart_builder *builder, const ta_xml_node *node, uint32_t state) {
    ta_invoke_record invoke_record;
    memset(&invoke_record, 0, sizeof invoke_record);
    invoke_record.state = state;
    invoke_record.type = invoke_record.typeexpr = invoke_record.source_chart =
        invoke_record.source = invoke_record.srcexpr = invoke_record.id = TA_NIL;
    invoke_record.idlocation = invoke_record.capture = invoke_record.finalize =
        invoke_record.docexpr = TA_NIL;
    const char *ty = ta_xml_attribute_value(node, "type");
    int scxml = is_scxml_type(ty);
    if (!scxml) invoke_record.type = str(builder, ty);
    invoke_record.typeexpr = attribute_expression(builder, node, "typeexpr", TA_EXPRESSION_STRING);
    const char *source = ta_xml_attribute_value(node, "src");
    invoke_record.srcexpr = attribute_expression(builder, node, "srcexpr", TA_EXPRESSION_STRING);
    if (source) {
        invoke_record.source = str(builder, source);
        if (scxml && invoke_record.typeexpr == TA_NIL)
            invoke_record.source_chart = resolve_chart(builder, node, source);
    }
    invoke_record.id = attr_str(builder, node, "id");
    invoke_record.idlocation =
        attribute_expression(builder, node, "idlocation", TA_EXPRESSION_LOCATION);
    const char *af = ta_xml_attribute_value(node, "autoforward");
    invoke_record.autoforward = af && strcmp(af, "true") == 0;
    const ta_xml_node *content = child(node, "content");
    if (content && content->first && el(content->first, "scxml")) {
        char name[256];
        int nl =
            snprintf(name, sizeof name, "%s#inline%u", builder->file, ++builder->inline_sequence);
        if (nl < 0 || (size_t)nl >= sizeof name) {
            /* deeply nested inline documents: a bounded name that is still unique
             * (a truncated one would alias another chart) */
            uint32_t k = builder->compiler->pending.length;
            do
                snprintf(name, sizeof name, "%.200s#inline@%u", builder->file, k++);
            while (ta_string_map_get(&builder->compiler->source_map, name,
                                     (uint32_t)strlen(name)) != TA_NIL);
        }
        invoke_record.source_chart = ta_compiler_queue(builder->compiler, &(ta_pending_document){(ta_xml_node *)content->first, name, builder->text, builder->length});
    }
    if (invoke_record.source_chart == TA_NIL && source == NULL && invoke_record.srcexpr == TA_NIL &&
        !content)
        compile_error(builder, node, (compile_message){"<invoke> needs src, srcexpr or <content>%s", NULL});
    /* <content> of an invoke is the child document, never a payload: inline
     * <scxml> is compiled statically; <content expr> yields document text at
     * runtime (resolved against documents precompiled into the image). */
    int document = content != NULL;
    if (content && !(content->first && el(content->first, "scxml"))) {
        invoke_record.docexpr = attribute_expression(builder, content, "expr", TA_EXPRESSION_VALUE);
        if (invoke_record.docexpr == TA_NIL)
            invoke_record.docexpr = content_expression(builder, content);
    }
    invoke_record.capture =
        capture(builder, node, ta_xml_attribute_value(node, "namelist"), document);
    invoke_record.finalize = block_one(builder, child(node, "finalize"));
    if (invoke_record.id != TA_NIL) {
        ta_string_view id = ta_image_view_string(
            &(ta_image_view){.strings = builder->compiler->strings.data}, invoke_record.id);
        char buffer[512];
        snprintf(buffer, sizeof buffer, "done.invoke.%.*s", (int)id.length, id.pointer);
        event_name(builder, node, buffer, (uint32_t)strlen(buffer));
    } else {
        ta_string_view sid =
            ta_image_view_string(&(ta_image_view){.strings = builder->compiler->strings.data},
                                 state_at(builder, state)->id);
        char buffer[512];
        snprintf(buffer, sizeof buffer, "done.invoke.%.*s", (int)sid.length, sid.pointer);
        event_name(builder, node, buffer, (uint32_t)strlen(buffer));
    }
    ta_invoke_record *dst = (ta_invoke_record *)ta_vector_push(&builder->compiler->invokes);
    if (dst) *dst = invoke_record;
}

static void data_declarations(chart_builder *builder, const ta_xml_node *datamodel,
                              uint32_t state) {
    for (const ta_xml_node *d = datamodel->first; d; d = d->next) {
        if (!el(d, "data")) {
            compile_error(builder, d, (compile_message){"unexpected <%s> in <datamodel>", d->name});
            continue;
        }
        ta_data_record data;
        data.state = state;
        data.id = attr_str(builder, d, "id");
        if (data.id == TA_NIL) compile_error(builder, d, (compile_message){"<data> requires 'id'%s", NULL});
        data.expression = attribute_expression(builder, d, "expr", TA_EXPRESSION_VALUE);
        const char *source = ta_xml_attribute_value(d, "src");
        if (source) {
            ta_source s;
            if (fetch_source(builder, &(source_fetch){d, source}, &s))
                data.expression = intern_expression(builder, &(expression_input){TA_EXPRESSION_CONTENT, {s.text.pointer, s.text.length}, d});
        } else if (data.expression == TA_NIL && (d->first || d->text_length)) {
            data.expression = content_expression(builder, d);
            precompile_literal_document(builder, d);
        }
        ta_data_record *dst = (ta_data_record *)ta_vector_push(&builder->compiler->data);
        if (dst) *dst = data;
    }
}

/* ---- chart ------------------------------------------------------------------------ */

void ta_compiler_compile_chart(ta_compiler *compiler, uint32_t index) {
    ta_pending_document *pending = &TA_VECTOR_AT(&compiler->pending, ta_pending_document, index);
    chart_builder context, *c = &context;
    memset(c, 0, sizeof *c);
    c->compiler = compiler;
    c->index = index;
    c->file = pending->name;
    c->text = pending->text;
    c->length = pending->length;
    c->first_state = compiler->states.length;
    c->first_transition = compiler->transitions.length;
    c->first_expression = compiler->expressions.length;
    c->first_send = compiler->sends.length;
    c->first_invoke = compiler->invokes.length;
    c->first_data = compiler->data.length;
    c->first_code = compiler->code.length;
    ta_vector_init(&c->nodes, compiler->allocator, sizeof(ta_xml_node *));
    ta_string_map_init(&c->ids, compiler->allocator);
    ta_string_map_init(&c->invoke_ids, compiler->allocator);
    ta_string_map_init(&c->expression_map, compiler->allocator);

    ta_chart_record chart;
    memset(&chart, 0, sizeof chart);
    if (!pending->root)
        pending->root =
            ta_xml_parse(&compiler->arena, &(ta_xml_parse_request){
                {pending->text, pending->length}, &compiler->diagnostics, compiler->allocator,
                pending->name, compiler->abort_flag, &compiler->aborted, &compiler->out_of_memory});
    ta_xml_node *root = pending->root;
    if (!root) goto done;
    if (!el(root, "scxml")) {
        compile_error(c, root, (compile_message){"root element must be <scxml>, got <%s>", root->name});
        goto done;
    }
    const char *dmname = ta_xml_attribute_value(root, "datamodel");
    const char *binding = ta_xml_attribute_value(root, "binding");
    const char *nm = ta_xml_attribute_value(root, "name");
    chart.name = str(c, nm ? nm : pending->name);
    chart.source = str(c, pending->name);
    chart.datamodel = str(c, dmname ? dmname : "null");
    chart.binding = binding && strcmp(binding, "late") == 0;
    if (binding && strcmp(binding, "late") && strcmp(binding, "early"))
        compile_error(c, root, (compile_message){"invalid binding '%s'", binding});

    collect(c, root);
    uint32_t ns = state_count(c);
    if (compiler->diagnostics.errors || ta_compiler_stop(compiler)) goto done;

    /* ids */
    state_at(c, 0)->id = str(c, "");
    for (uint32_t i = 1; i < ns; i++) {
        if ((i & 1023) == 0 && ta_compiler_stop(compiler)) goto done;
        const ta_xml_attribute *id = ta_xml_find_attribute(node_at(c, i), "id");
        if (!id) continue;
        if (ta_string_map_get(&c->ids, id->value, id->value_length) != TA_NIL)
            compile_error(c, node_at(c, i), (compile_message){"duplicate state id '%s'", id->value});
        ta_string_map_put(&c->ids, id->value, id->value_length, i);
        state_at(c, i)->id = ta_compiler_string(compiler, id->value, id->value_length);
    }
    /* state i without an id gets _ta_s<k>, the smallest free k >= i. Every k
     * below the previous pick (and >= i) is taken, so the search resumes
     * there: linear in the number of states, whatever ids the document uses */
    for (uint32_t i = 1, next = 1; i < ns; i++) {
        if ((i & 1023) == 0 && ta_compiler_stop(compiler)) goto done;
        if (state_at(c, i)->id != TA_NIL) continue;
        char buffer[48];
        uint32_t k = i > next ? i : next;
        do
            snprintf(buffer, sizeof buffer, "_ta_s%u", k++);
        while (ta_string_map_get(&c->ids, buffer, (uint32_t)strlen(buffer)) != TA_NIL);
        next = k;
        char *key = ta_compile_copy(&compiler->arena, (ta_string_view){buffer, (uint32_t)(strlen(buffer))}, &compiler->out_of_memory);
        if (!key) goto done;
        ta_string_map_put(&c->ids, key, (uint32_t)strlen(key), i);
        state_at(c, i)->id = str(c, key);
    }
    /* static invoke ids -> slots (slot order == invoke table order) */
    for (uint32_t i = 0; i < ns; i++)
        for (const ta_xml_node *chn = node_at(c, i)->first; chn; chn = chn->next)
            if (el(chn, "invoke")) {
                const ta_xml_attribute *id = ta_xml_find_attribute(chn, "id");
                if (id)
                    ta_string_map_put(&c->invoke_ids, id->value, id->value_length,
                                      c->invoke_id_count);
                c->invoke_id_count++;
            }

    /* per-state content, in document order */
    for (uint32_t i = 0; i < ns; i++) {
        if (ta_compiler_stop(compiler)) goto done;
        ta_xml_node *node = node_at(c, i);
        uint32_t t0 = transition_count(c), inv0 = compiler->invokes.length - c->first_invoke,
                 d0 = compiler->data.length - c->first_data;
        uint8_t kind = state_at(c, i)->kind;
        for (const ta_xml_node *chn = node->first; chn; chn = chn->next) {
            if (!is_scxml(chn)) continue;
            if (el(chn, "transition")) {
                if (kind >= TA_STATE_HISTORY_SHALLOW) continue; /* history default: below */
                if (i == 0)
                    compile_error(c, chn, (compile_message){"<transition> not allowed in <scxml>%s", NULL});
                else if (kind == TA_STATE_FINAL)
                    compile_error(c, chn, (compile_message){"<transition> not allowed in <final>%s", NULL});
                else
                    transition(c, chn, i);
            } else if (el(chn, "datamodel")) {
                data_declarations(c, chn, i);
            } else if (el(chn, "invoke")) {
                if (i == 0)
                    compile_error(c, chn, (compile_message){"<invoke> not allowed in <scxml>%s", NULL});
                else
                    invoke(c, chn, i);
            }
        }
        uint32_t onentry = i ? block_seq(c, node, "onentry") : TA_NIL;
        uint32_t onexit = i ? block_seq(c, node, "onexit") : TA_NIL;
        uint32_t donedata = TA_NIL;
        const ta_xml_node *dd = child(node, "donedata");
        if (dd) donedata = capture(c, dd, NULL, 0);
        ta_state_record *state = state_at(c, i);
        state->first_transition = t0;
        state->transition_count = transition_count(c) - t0;
        state->first_invoke = inv0;
        state->invoke_count = compiler->invokes.length - c->first_invoke - inv0;
        state->first_data = d0;
        state->data_count = compiler->data.length - c->first_data - d0;
        state->onentry = onentry;
        state->onexit = onexit;
        state->donedata = donedata;
        for (uint32_t k = t0; k < transition_count(c); k++)
            state->flags |= transition_at(c, k)->event_count
                                ? TA_STATE_FLAG_HAS_EVENT_TRANSITIONS
                                : TA_STATE_FLAG_HAS_EVENTLESS_TRANSITIONS;
    }
    /* initial transitions and history defaults */
    for (uint32_t i = 0; i < ns; i++) {
        if (ta_compiler_stop(compiler)) goto done;
        ta_xml_node *node = node_at(c, i);
        uint8_t kind = state_at(c, i)->kind;
        if (kind == TA_STATE_COMPOUND) {
            const char *ia = ta_xml_attribute_value(node, "initial");
            const ta_xml_node *ie = child(node, "initial");
            uint32_t t;
            if (ia && ie)
                compile_error(c, node, (compile_message){"both 'initial' attribute and <initial> element%s", NULL});
            if (ie) {
                const ta_xml_node *it = child(ie, "transition");
                if (!it) {
                    compile_error(c, ie, (compile_message){"<initial> requires a <transition>%s", NULL});
                    continue;
                }
                t = pseudo_transition(c, it, i, i, ta_xml_attribute_value(it, "target"), TA_NIL,
                                      it);
            } else if (ia) {
                t = pseudo_transition(c, node, i, i, ia, TA_NIL, NULL);
            } else {
                uint32_t first = TA_NIL;
                for (uint32_t k = i + 1; k < state_at(c, i)->end; k = state_at(c, k)->end)
                    if (state_at(c, k)->kind < TA_STATE_HISTORY_SHALLOW) {
                        first = k;
                        break;
                    }
                t = pseudo_transition(c, node, i, i, NULL, first, NULL);
            }
            state_at(c, i)->initial = t;
        } else if (kind >= TA_STATE_HISTORY_SHALLOW) {
            const ta_xml_node *ht = child(node, "transition");
            if (!ht) {
                compile_error(c, node, (compile_message){"<history> requires a default <transition>%s", NULL});
                continue;
            }
            uint32_t parent = state_at(c, i)->parent;
            state_at(c, i)->initial = pseudo_transition(
                c, ht, i, parent, ta_xml_attribute_value(ht, "target"), TA_NIL, ht);
        } else if (child(node, "initial")) {
            compile_error(c, node, (compile_message){"<initial> only allowed in compound states%s", NULL});
        }
    }
    /* top-level scripts */
    chart.script = block_each(c, root, "script");

    /* done.state names, history storage, eventless mask */
    uint32_t history = 0;
    for (uint32_t i = 1; i < ns; i++) {
        if ((i & 1023) == 0 && ta_compiler_stop(compiler)) goto done;
        ta_state_record *state = state_at(c, i);
        if (state->kind == TA_STATE_COMPOUND || state->kind == TA_STATE_PARALLEL) {
            ta_string_view id = ta_image_view_string(
                &(ta_image_view){.strings = compiler->strings.data}, state->id);
            char buffer[512];
            snprintf(buffer, sizeof buffer, "done.state.%.*s", (int)id.length, id.pointer);
            uint32_t en = event_name(c, node_at(c, i), buffer, (uint32_t)strlen(buffer));
            state_at(c, i)->done_event = en;
        } else if (state->kind >= TA_STATE_HISTORY_SHALLOW) {
            const ta_state_record *p = state_at(c, state->parent);
            state->history_offset = history;
            history += (p->end - state->parent - 1 + 31) / 32;
        }
    }
    chart.eventless_mask = compiler->u32.length;
    uint32_t words = (ns + 31) / 32;
    uint32_t *mask = (uint32_t *)ta_vector_push_many(&compiler->u32, words);
    if (mask)
        for (uint32_t i = 0; i < ns; i++)
            if (state_at(c, i)->flags & TA_STATE_FLAG_HAS_EVENTLESS_TRANSITIONS)
                mask[i >> 5] |= 1u << (i & 31);

    chart.history_word_count = history;
    chart.max_depth = c->max_depth;
    chart.max_foreach = c->max_foreach;

done:
    chart.first_state = c->first_state;
    chart.state_count = compiler->states.length - c->first_state;
    chart.first_transition = c->first_transition;
    chart.transition_count = compiler->transitions.length - c->first_transition;
    chart.first_expression = c->first_expression;
    chart.expression_count = compiler->expressions.length - c->first_expression;
    chart.first_send = c->first_send;
    chart.send_count = compiler->sends.length - c->first_send;
    chart.first_invoke = c->first_invoke;
    chart.invoke_count = compiler->invokes.length - c->first_invoke;
    if (chart.invoke_count > TA_CHART_MAX_INVOKES)
        compile_error(c, root, (compile_message){"a chart may have at most 65534 invocations%s", ""});
    chart.first_data = c->first_data;
    chart.data_count = compiler->data.length - c->first_data;
    chart.first_code = c->first_code;
    chart.code_count = compiler->code.length - c->first_code;
    if (!chart.state_count) chart.script = TA_NIL;
    ta_chart_record *dst = (ta_chart_record *)ta_vector_push(&compiler->charts);
    if (dst) *dst = chart;
    ta_vector_free(&c->nodes);
    ta_string_map_free(&c->ids);
    ta_string_map_free(&c->invoke_ids);
    ta_string_map_free(&c->expression_map);
}
