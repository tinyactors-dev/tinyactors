/* image.c — open + verify an image once; public read accessors.
 *
 * Verification bounds-checks every offset, index, range, string and
 * bytecode operand so that the execution hot path needs no checks. It is
 * O(image size) and allocation-free.
 */
#include "image/records.h"
#include "base/memory.h"

TA_STATIC_ASSERT(sizeof(ta_image_view) <= sizeof(ta_image), view_fits_public_storage);
TA_STATIC_ASSERT(sizeof(ta_header_record) == 64, header_is_64_bytes);

#define NIL_OR_LT(x, n) ((x) == TA_NIL || (x) < (n))
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) return TA_ERROR_INVALID;                                                         \
    } while (0)

static int str_ok(const ta_image_view *view, uint32_t sid) {
    if (sid == TA_NIL) return 1;
    if ((sid & 3u) || sid > view->strings_size || view->strings_size - sid < 5) return 0;
    uint32_t length = *(const uint32_t *)(const void *)(view->strings + sid);
    if (length > view->strings_size - sid - 5) return 0;
    return view->strings[sid + 4 + length] == 0;
}

static int range_ok(uint32_t offset, uint32_t n, uint32_t total) {
    return offset <= total && n <= total - offset;
}

static int pc_ok(const ta_chart_record *chart, uint32_t pc) {
    return pc == TA_NIL || (pc >= chart->first_code && pc < chart->first_code + chart->code_count);
}

/* Is an offset the start of an instruction in the chart's code range? O(code): the
 * fallback for what the linear checks below cannot settle. */
static int insn_start(const ta_image_view *view, const ta_chart_record *chart, uint32_t pc) {
    uint32_t p = chart->first_code, e = chart->first_code + chart->code_count;
    while (p < e && p < pc)
        p += ta_opcode_length[view->code[p]];
    return p == pc && pc < e;
}

/* Entry points of one table (onentry, onexit, transition actions, invoke
 * finalizers) ascend in compiler output: each is found by decoding on from
 * the previous one, O(code) per table. */
typedef struct cursor {
    uint32_t at; /* an instruction start (or the chart's first_code) */
} cursor;

static int entry_ok(const ta_image_view *view, const ta_chart_record *chart, cursor *cur,
                    uint32_t pc) {
    if (pc == TA_NIL) return 1;
    if (pc < cur->at) return insn_start(view, chart, pc);
    uint32_t p = cur->at, e = chart->first_code + chart->code_count;
    while (p < e && p < pc)
        p += ta_opcode_length[view->code[p]];
    if (p != pc || pc >= e) return 0;
    cur->at = pc;
    return 1;
}

/* Jump targets must be instruction starts. A forward target is one iff the
 * sequential decode reaches it, so pending targets wait (sorted, nearest on
 * top) until it does; a <foreach>'s back jump goes to the body start its
 * FEBEGIN recorded. Compiler output never needs more room than these stacks
 * (TA_IMAGE_MAX_NESTING); anything else falls back to insn_start. */
#define PENDING_CAPACITY (2u * TA_IMAGE_MAX_NESTING + 2u)

static int pend_add(uint32_t *pend, uint32_t *np, uint32_t t) {
    uint32_t i = *np;
    while (i > 0 && pend[i - 1] < t) /* pend descends: find t's place */
        i--;
    if (i > 0 && pend[i - 1] == t) return 1;
    if (*np == PENDING_CAPACITY) return 0;
    for (uint32_t k = *np; k > i; k--)
        pend[k] = pend[k - 1];
    pend[i] = t;
    (*np)++;
    return 1;
}

static ta_status verify_code(const ta_image_view *view, const ta_chart_record *chart) {
    uint32_t p = chart->first_code, e = chart->first_code + chart->code_count;
    uint32_t pend[PENDING_CAPACITY], np = 0;
    uint32_t body[TA_IMAGE_MAX_NESTING], nb = 0, nb_lost = 0;
    while (p < e) {
        while (np && pend[np - 1] == p)
            np--;
        CHECK(!np || pend[np - 1] > p); /* decoding stepped over a target */
        uint32_t opcode = view->code[p];
        CHECK(opcode < TA_OPCODE_MAX_);
        CHECK(e - p >= ta_opcode_length[opcode]);
        const uint32_t *w = view->code + p + 1;
        uint32_t fwd = TA_NIL;
        switch (opcode) {
        case TA_OPCODE_END:
            break;
        case TA_OPCODE_BLOCK:
            CHECK(w[0] > p && w[0] < e);
            fwd = w[0];
            break;
        case TA_OPCODE_RAISE:
            CHECK(w[0] < view->event_node_count);
            break;
        case TA_OPCODE_SEND:
            CHECK(w[0] < chart->send_count);
            break;
        case TA_OPCODE_CANCEL:
            CHECK(w[0] != TA_NIL && str_ok(view, w[0]));
            break;
        case TA_OPCODE_CANCEL_EXPRESSION:
        case TA_OPCODE_SCRIPT:
            CHECK(w[0] < chart->expression_count);
            break;
        case TA_OPCODE_ASSIGN:
            CHECK(w[0] < chart->expression_count && w[1] < chart->expression_count);
            break;
        case TA_OPCODE_LOG:
            CHECK(str_ok(view, w[0]) && NIL_OR_LT(w[1], chart->expression_count));
            break;
        case TA_OPCODE_JUMP_IF_FALSE:
            CHECK(w[0] < chart->expression_count && w[1] > p && w[1] < e);
            fwd = w[1];
            break;
        case TA_OPCODE_JUMP:
            CHECK(w[0] > p && w[0] < e);
            fwd = w[0];
            break;
        case TA_OPCODE_FOREACH_BEGIN:
            CHECK(w[0] < chart->expression_count && w[1] < chart->expression_count &&
                  NIL_OR_LT(w[2], chart->expression_count));
            CHECK(w[3] > p && w[3] < e);
            fwd = w[3];
            if (nb < TA_IMAGE_MAX_NESTING)
                body[nb++] = p + ta_opcode_length[opcode];
            else
                nb_lost++;
            break;
        case TA_OPCODE_FOREACH_NEXT: /* body instruction; == p for an empty <foreach> body */
            CHECK(w[0] <= p && w[0] >= chart->first_code);
            if (!nb_lost && nb && body[nb - 1] == w[0])
                nb--;
            else
                CHECK(insn_start(view, chart, w[0]));
            if (nb_lost) nb_lost--;
            break;
        case TA_OPCODE_CUSTOM:
            break;
        }
        if (fwd != TA_NIL && !pend_add(pend, &np, fwd)) CHECK(insn_start(view, chart, fwd));
        p += ta_opcode_length[opcode];
    }
    CHECK(p == e && np == 0);
    return TA_OK;
}

static ta_status verify_capture(const ta_image_view *view, const ta_chart_record *chart,
                                uint32_t index) {
    if (index == TA_NIL) return TA_OK;
    CHECK(index < view->capture_count);
    const ta_capture_record *capture = &view->captures[index];
    CHECK(range_ok(capture->namelist_offset, capture->namelist_count, view->u32_count));
    for (uint32_t i = 0; i < capture->namelist_count; i++)
        CHECK(view->u32[capture->namelist_offset + i] < chart->expression_count);
    CHECK(capture->param_count <= view->u32_count / 2 &&
          range_ok(capture->params_offset, capture->param_count * 2, view->u32_count));
    for (uint32_t i = 0; i < capture->param_count; i++) {
        CHECK(str_ok(view, view->u32[capture->params_offset + 2 * i]));
        CHECK(view->u32[capture->params_offset + 2 * i + 1] < chart->expression_count);
    }
    CHECK(NIL_OR_LT(capture->content, chart->expression_count));
    return TA_OK;
}

static ta_status verify_chart(const ta_image_view *view, const ta_chart_record *chart) {
    CHECK(str_ok(view, chart->name) && str_ok(view, chart->datamodel) &&
          str_ok(view, chart->source));
    CHECK(chart->binding <= 1);
    CHECK(chart->state_count >= 1 &&
          range_ok(chart->first_state, chart->state_count, view->state_count));
    CHECK(range_ok(chart->first_transition, chart->transition_count, view->transition_count));
    CHECK(range_ok(chart->first_expression, chart->expression_count, view->expression_count));
    CHECK(range_ok(chart->first_send, chart->send_count, view->send_count));
    CHECK(range_ok(chart->first_invoke, chart->invoke_count, view->invoke_count));
    CHECK(chart->invoke_count <= TA_CHART_MAX_INVOKES);
    CHECK(range_ok(chart->first_data, chart->data_count, view->data_count));
    CHECK(range_ok(chart->first_code, chart->code_count, view->code_count));
    CHECK(range_ok(chart->eventless_mask, (chart->state_count + 31) / 32, view->u32_count));
    CHECK(chart->max_depth < 256 && chart->max_foreach < 256);
    if (verify_code(view, chart) != TA_OK) return TA_ERROR_INVALID; /* before any decoding below */
    CHECK(pc_ok(chart, chart->script) &&
          (chart->script == TA_NIL || insn_start(view, chart, chart->script)));

    const ta_state_record *state = view->states + chart->first_state;
    const ta_transition_record *transition_record = view->transitions + chart->first_transition;
    cursor onentry = {chart->first_code}, onexit = {chart->first_code},
           action = {chart->first_code}, fin = {chart->first_code};
    for (uint32_t i = 0; i < chart->state_count; i++) {
        const ta_state_record *s = &state[i];
        if (i == 0) {
            CHECK(s->parent == TA_NIL && s->end == chart->state_count &&
                  (s->flags & TA_STATE_FLAG_ROOT));
            CHECK(s->depth == 0 && s->kind == TA_STATE_COMPOUND);
        } else {
            CHECK(s->parent < i && s->end <= state[s->parent].end &&
                  !(s->flags & TA_STATE_FLAG_ROOT));
            CHECK(s->depth == state[s->parent].depth + 1 && s->depth <= chart->max_depth);
            uint8_t pk = state[s->parent].kind;
            CHECK(pk == TA_STATE_COMPOUND || pk == TA_STATE_PARALLEL);
        }
        CHECK(s->end > i && s->end <= chart->state_count);
        CHECK(s->kind <= TA_STATE_HISTORY_DEEP);
        int leaf = s->end == i + 1;
        if (s->kind == TA_STATE_COMPOUND) CHECK(!leaf && s->initial < chart->transition_count);
        if (s->kind == TA_STATE_PARALLEL) CHECK(!leaf && s->initial == TA_NIL);
        if (s->kind == TA_STATE_ATOMIC || s->kind == TA_STATE_FINAL)
            CHECK(leaf && s->initial == TA_NIL);
        if (s->kind >= TA_STATE_HISTORY_SHALLOW) {
            CHECK(leaf && i > 0 && NIL_OR_LT(s->initial, chart->transition_count));
            const ta_state_record *p = &state[s->parent];
            uint32_t words = (p->end - s->parent - 1 + 31) / 32;
            CHECK(s->history_offset != TA_NIL &&
                  range_ok(s->history_offset, words, chart->history_word_count));
        }
        CHECK(str_ok(view, s->id));
        CHECK(range_ok(s->first_transition, s->transition_count, chart->transition_count));
        CHECK(pc_ok(chart, s->onentry) && pc_ok(chart, s->onexit));
        CHECK(entry_ok(view, chart, &onentry, s->onentry) &&
              entry_ok(view, chart, &onexit, s->onexit));
        CHECK(range_ok(s->first_invoke, s->invoke_count, chart->invoke_count));
        CHECK(range_ok(s->first_data, s->data_count, chart->data_count));
        if (verify_capture(view, chart, s->donedata) != TA_OK) return TA_ERROR_INVALID;
        CHECK(NIL_OR_LT(s->done_event, view->event_node_count));
    }
    for (uint32_t i = 0; i < chart->transition_count; i++) {
        const ta_transition_record *transition = &transition_record[i];
        CHECK(transition->source < chart->state_count &&
              NIL_OR_LT(transition->domain, chart->state_count));
        CHECK(NIL_OR_LT(transition->condition, chart->expression_count));
        CHECK(range_ok(transition->events_offset, (uint32_t)transition->event_count * 2,
                       view->u32_count));
        for (uint32_t k = 0; k < transition->event_count; k++) {
            uint32_t lo = view->u32[transition->events_offset + 2 * k],
                     hi = view->u32[transition->events_offset + 2 * k + 1];
            CHECK(lo <= hi && hi <= view->event_node_count);
        }
        CHECK(range_ok(transition->targets_offset, transition->target_count, view->u32_count));
        for (uint32_t k = 0; k < transition->target_count; k++)
            CHECK(view->u32[transition->targets_offset + k] < chart->state_count &&
                  view->u32[transition->targets_offset + k] > 0);
        CHECK(transition->target_count > 0 || transition->domain == TA_NIL);
        CHECK(pc_ok(chart, transition->action) &&
              entry_ok(view, chart, &action, transition->action));
    }
    for (uint32_t i = 0; i < chart->expression_count; i++) {
        const ta_expression_record *expression = &view->expressions[chart->first_expression + i];
        CHECK(expression->kind >= TA_EXPRESSION_CONDITION &&
              expression->kind <= TA_EXPRESSION_GLOBAL_SCRIPT && str_ok(view, expression->source));
    }
    for (uint32_t i = 0; i < chart->send_count; i++) {
        const ta_send_record *s = &view->sends[chart->first_send + i];
        CHECK(NIL_OR_LT(s->event, view->event_node_count) &&
              NIL_OR_LT(s->eventexpr, chart->expression_count));
        CHECK(s->target_kind <= TA_SEND_TARGET_EXPRESSION);
        if (s->target_kind == TA_SEND_TARGET_INVOKE) CHECK(s->target < chart->invoke_count);
        if (s->target_kind == TA_SEND_TARGET_STRING)
            CHECK(s->target != TA_NIL && str_ok(view, s->target));
        if (s->target_kind == TA_SEND_TARGET_EXPRESSION)
            CHECK(s->targetexpr < chart->expression_count);
        CHECK(str_ok(view, s->type) && NIL_OR_LT(s->typeexpr, chart->expression_count));
        CHECK(str_ok(view, s->id) && NIL_OR_LT(s->idlocation, chart->expression_count));
        CHECK(NIL_OR_LT(s->delayexpr, chart->expression_count));
        if (verify_capture(view, chart, s->capture) != TA_OK) return TA_ERROR_INVALID;
    }
    for (uint32_t i = 0; i < chart->invoke_count; i++) {
        const ta_invoke_record *invoke = &view->invokes[chart->first_invoke + i];
        CHECK(invoke->state < chart->state_count && invoke->state > 0);
        CHECK(str_ok(view, invoke->type) && NIL_OR_LT(invoke->typeexpr, chart->expression_count));
        CHECK(NIL_OR_LT(invoke->source_chart, view->chart_count) && str_ok(view, invoke->source));
        CHECK(NIL_OR_LT(invoke->srcexpr, chart->expression_count));
        CHECK(str_ok(view, invoke->id) && NIL_OR_LT(invoke->idlocation, chart->expression_count));
        CHECK(invoke->autoforward <= 1 && NIL_OR_LT(invoke->docexpr, chart->expression_count));
        CHECK(pc_ok(chart, invoke->finalize) && entry_ok(view, chart, &fin, invoke->finalize));
        if (verify_capture(view, chart, invoke->capture) != TA_OK) return TA_ERROR_INVALID;
    }
    for (uint32_t i = 0; i < chart->data_count; i++) {
        const ta_data_record *data = &view->data[chart->first_data + i];
        CHECK(data->id != TA_NIL && str_ok(view, data->id) &&
              NIL_OR_LT(data->expression, chart->expression_count));
        CHECK(data->state < chart->state_count);
    }
    return TA_OK;
}

#define SECTION(kind_, field, type, nfield)                                                        \
    case kind_:                                                                                    \
        CHECK(section->size == (uint64_t)section->count * sizeof(type));                           \
        view->field = (const type *)(const void *)(b + section->offset);                           \
        view->nfield = section->count;                                                             \
        break

ta_bytes ta_image_bytes(const ta_image *image) {
    const ta_image_view *view = (const ta_image_view *)(const void *)image;
    return (ta_bytes){view->base, view->size};
}

ta_status ta_image_open(ta_bytes image, ta_image *out) {
    const void *bytes = image.pointer;
    size_t length = image.length;
    ta_image_view *view = (ta_image_view *)(void *)out;
    const unsigned char *b = (const unsigned char *)bytes;
    ta_memset(out, 0, sizeof *out);
    CHECK(b && ((uintptr_t)b & 7u) == 0);
    CHECK(length >= sizeof(ta_header_record) && length <= 0x7FFFFFFFu);
    const ta_header_record *header = (const ta_header_record *)bytes;
    CHECK(header->magic == TA_IMAGE_MAGIC && header->version == TA_IMAGE_VERSION &&
          header->total_size == length);
    CHECK(header->section_count <= 64);
    CHECK(sizeof(ta_header_record) + (size_t)header->section_count * sizeof(ta_section_record) <=
          length);
    CHECK(ta_image_hash(b, (uint32_t)length) == header->hash);

    static const uint32_t empty_u32[1] = {0};
    view->base = b;
    view->size = (uint32_t)length;
    view->hash = header->hash;
    view->u32 = empty_u32;
    view->code = empty_u32;
    unsigned seen = 0;
    const ta_section_record *sections =
        (const ta_section_record *)(const void *)(b + sizeof(ta_header_record));
    for (uint32_t i = 0; i < header->section_count; i++) {
        const ta_section_record *section = &sections[i];
        CHECK(section->kind > 0 && section->kind < TA_SECTION_MAX_ &&
              !(seen & (1u << section->kind)));
        seen |= 1u << section->kind;
        CHECK((section->offset & 7u) == 0 &&
              range_ok(section->offset, section->size, (uint32_t)length));
        switch (section->kind) {
        case TA_SECTION_STRINGS:
            view->strings = b + section->offset;
            view->strings_size = section->size;
            break;
            SECTION(TA_SECTION_CHARTS, charts, ta_chart_record, chart_count);
            SECTION(TA_SECTION_STATES, states, ta_state_record, state_count);
            SECTION(TA_SECTION_TRANSITIONS, transitions, ta_transition_record, transition_count);
            SECTION(TA_SECTION_U32, u32, uint32_t, u32_count);
            SECTION(TA_SECTION_CODE, code, uint32_t, code_count);
            SECTION(TA_SECTION_EXPRESSIONS, expressions, ta_expression_record, expression_count);
            SECTION(TA_SECTION_CAPTURES, captures, ta_capture_record, capture_count);
            SECTION(TA_SECTION_SENDS, sends, ta_send_record, send_count);
            SECTION(TA_SECTION_INVOKES, invokes, ta_invoke_record, invoke_count);
            SECTION(TA_SECTION_DATA, data, ta_data_record, data_count);
            SECTION(TA_SECTION_EVENT_NODES, event_nodes, ta_event_node_record, event_node_count);
            SECTION(TA_SECTION_ACTORS, actors, ta_actor_record, actor_count);
        }
    }
    CHECK(view->strings && view->chart_count > 0 && view->chart_count == header->chart_count &&
          view->event_node_count > 0);

    for (uint32_t i = 0; i < view->event_node_count; i++) {
        const ta_event_node_record *node = &view->event_nodes[i];
        CHECK(node->end > i && node->end <= view->event_node_count && str_ok(view, node->token) &&
              str_ok(view, node->name));
        CHECK(node->token != TA_NIL && node->name != TA_NIL);
        if (i == 0)
            CHECK(node->parent == TA_NIL && node->end == view->event_node_count);
        else
            CHECK(node->parent < i && node->end <= view->event_nodes[node->parent].end);
    }
    for (uint32_t i = 0; i < view->chart_count; i++)
        if (verify_chart(view, &view->charts[i]) != TA_OK) goto bad;
    for (uint32_t i = 0; i < view->actor_count; i++)
        if (!str_ok(view, view->actors[i].name) || view->actors[i].chart >= view->chart_count)
            goto bad;
    return TA_OK;
bad:
    ta_memset(out, 0, sizeof *out);
    return TA_ERROR_INVALID;
}

/* ---- public accessors ---------------------------------------------------- */

static int string_equal(ta_string_view a, ta_string_view b) {
    return a.length == b.length &&
           (a.length == 0 || ta_memcmp(a.pointer, b.pointer, a.length) == 0);
}

void ta_image_get_info(const ta_image *image, ta_image_info *out) {
    const ta_image_view *view = ta_image_view_of(image);
    out->size = sizeof *out;
    out->version = TA_IMAGE_VERSION;
    out->chart_count = view->chart_count;
    out->boot_actor_count = view->actor_count;
    out->datamodel_count = 0;
    out->event_name_count = view->event_node_count;
    out->content_hash = view->hash;
}

/* Matches the chart name, its source name, or the source's last path segment. */
uint32_t ta_image_chart_by_name(const ta_image *image, ta_string_view name) {
    const ta_image_view *view = ta_image_view_of(image);
    for (uint32_t i = 0; i < view->chart_count; i++) {
        if (string_equal(ta_image_view_string(view, view->charts[i].name), name)) return i;
        ta_string_view source = ta_image_view_string(view, view->charts[i].source);
        if (string_equal(source, name)) return i;
        uint32_t b = source.length;
        while (b > 0 && source.pointer[b - 1] != '/' && source.pointer[b - 1] != '\\')
            b--;
        ta_string_view base = {source.pointer + b, source.length - b};
        if (b && string_equal(base, name)) return i;
    }
    return TA_NIL;
}

ta_string_view ta_image_chart_name(const ta_image *image, uint32_t chart) {
    const ta_image_view *view = ta_image_view_of(image);
    ta_string_view none = {(const char *)0, 0};
    return chart < view->chart_count ? ta_image_view_string(view, view->charts[chart].name) : none;
}

ta_string_view ta_image_chart_source(const ta_image *image, uint32_t chart) {
    const ta_image_view *view = ta_image_view_of(image);
    ta_string_view none = {(const char *)0, 0};
    return chart < view->chart_count ? ta_image_view_string(view, view->charts[chart].source)
                                     : none;
}

ta_string_view ta_image_chart_datamodel(const ta_image *image, uint32_t chart) {
    const ta_image_view *view = ta_image_view_of(image);
    ta_string_view none = {(const char *)0, 0};
    return chart < view->chart_count ? ta_image_view_string(view, view->charts[chart].datamodel)
                                     : none;
}

uint32_t ta_image_state_count(const ta_image *image, uint32_t chart) {
    const ta_image_view *view = ta_image_view_of(image);
    return chart < view->chart_count ? view->charts[chart].state_count : 0;
}

ta_string_view ta_image_state_name(const ta_image *image, uint32_t chart, uint32_t state) {
    const ta_image_view *view = ta_image_view_of(image);
    ta_string_view none = {(const char *)0, 0};
    if (chart >= view->chart_count || state >= view->charts[chart].state_count) return none;
    return ta_image_view_string(view, view->states[view->charts[chart].first_state + state].id);
}

ta_status ta_image_state_info(const ta_image *image, ta_chart_item item, ta_state_info *out) {
    uint32_t chart = item.chart, state = item.index;
    const ta_image_view *view = ta_image_view_of(image);
    if (chart >= view->chart_count || state >= view->charts[chart].state_count)
        return TA_ERROR_INVALID;
    const ta_state_record *state_record = &view->states[view->charts[chart].first_state + state];
    out->size = sizeof *out;
    out->parent = state_record->parent;
    out->kind = state_record->kind;
    out->depth = state_record->depth;
    out->id = ta_image_view_string(view, state_record->id);
    return TA_OK;
}

ta_status ta_image_boot_actor(const ta_image *image, uint32_t i, ta_boot_actor *out) {
    const ta_image_view *view = ta_image_view_of(image);
    if (i >= view->actor_count) return TA_ERROR_INVALID;
    out->name = ta_image_view_string(view, view->actors[i].name);
    out->chart = view->actors[i].chart;
    out->pad = 0;
    return TA_OK;
}

uint32_t ta_image_state_by_name(const ta_image *image, uint32_t chart, ta_string_view name) {
    const ta_image_view *view = ta_image_view_of(image);
    if (chart >= view->chart_count) return TA_NIL;
    const ta_chart_record *chart_record = &view->charts[chart];
    for (uint32_t i = 0; i < chart_record->state_count; i++)
        if (string_equal(ta_image_view_string(view, view->states[chart_record->first_state + i].id),
                         name))
            return i;
    return TA_NIL;
}

ta_string_view ta_image_event_name(const ta_image *image, uint32_t name_id) {
    const ta_image_view *view = ta_image_view_of(image);
    ta_string_view none = {(const char *)0, 0};
    return name_id < view->event_node_count
               ? ta_image_view_string(view, view->event_nodes[name_id].name)
               : none;
}

ta_string_view ta_image_string(const ta_image *image, uint32_t sid) {
    return ta_image_view_string(ta_image_view_of(image), sid);
}

uint32_t ta_image_expression_count(const ta_image *image, uint32_t chart) {
    const ta_image_view *view = ta_image_view_of(image);
    return chart < view->chart_count ? view->charts[chart].expression_count : 0;
}

ta_status ta_image_expression(const ta_image *image, ta_chart_item item, ta_expression_info *out) {
    uint32_t chart = item.chart;
    ta_expression expression = item.index;
    const ta_image_view *view = ta_image_view_of(image);
    if (chart >= view->chart_count || expression >= view->charts[chart].expression_count)
        return TA_ERROR_INVALID;
    const ta_expression_record *expression_record =
        &view->expressions[view->charts[chart].first_expression + expression];
    out->kind = expression_record->kind;
    out->line = expression_record->line;
    out->column = expression_record->column;
    out->source = ta_image_view_string(view, expression_record->source);
    return TA_OK;
}

ta_string_view ta_image_ahead_of_time_section(const ta_image *image, uint32_t chart,
                                              ta_string_view datamodel) {
    (void)image;
    (void)chart;
    (void)datamodel;
    ta_string_view none = {(const char *)0, 0};
    return none; /* no AOT sections in format version 1 */
}

uint32_t ta_image_data_count(const ta_image *image, uint32_t chart) {
    const ta_image_view *view = ta_image_view_of(image);
    return chart < view->chart_count ? view->charts[chart].data_count : 0;
}

ta_status ta_image_data(const ta_image *image, ta_chart_item item, ta_data_info *out) {
    uint32_t chart = item.chart, index = item.index;
    const ta_image_view *view = ta_image_view_of(image);
    if (chart >= view->chart_count || index >= view->charts[chart].data_count)
        return TA_ERROR_INVALID;
    const ta_data_record *data = &view->data[view->charts[chart].first_data + index];
    out->id = ta_image_view_string(view, data->id);
    out->expression = data->expression;
    out->state = data->state;
    return TA_OK;
}
