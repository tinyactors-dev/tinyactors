/*
 * The matcher (docs/matcher.md, section 5): iburg's labeller, run bottom-up
 * over the events as they arrive, with the tables src/shapes.rules compiles
 * to.
 *
 * Every node's last event comes after its children, so the matcher keeps a
 * stack of records. A cell is a complete node: for each nonterminal it
 * derives, the cheapest cost and that derivation's attributes. A marker is
 * an open bracket (an arrow, ...): for each bracket rule of its kind still
 * matching, the cost so far and the variables bound so far. A leaf pushes a
 * cell; a postfix event replaces the cells of its operands by one; an open
 * pushes a marker; a separator, a close and an item fold the cells above the
 * marker into it, left to right, so that neither a wide bracket nor a long
 * body holds more than a few records; a close turns the marker into a cell.
 *
 * Every rule wants each child of a bracket to derive a nonterminal or to be
 * a given separator, so a child that derives nothing kills every rule of
 * its bracket. Once a bracket can match no more, nothing inside it is a
 * child of a rule again, but for brackets rules look at, which may derive a
 * shape of their own. So the matcher keeps nothing of such a dead region
 * but its marker (and none for the program, which no rule matches), and
 * passes over its events, those of the brackets inside it included, but for
 * its own close and the opens of brackets rules look at. Most events of a
 * program are in a dead region, and cost a comparison or two.
 *
 * Which bracket an event belongs to is its node id: an event with the
 * innermost marker's id is that bracket's separator or close, whatever its
 * kind. A stream that cannot be such a tree, where a rule could still
 * match, stops the matching (5.5).
 *
 * Text an event carries lives only during the call, so a text a record
 * keeps is copied into m->text, in record order. When records are replaced
 * by one, the text the new record still refers to is moved down to where
 * theirs began (settle). Cells folded into a marker leave their text where
 * it is until the marker closes or dies.
 *
 * Nothing recurses: rules are table rows, children a loop, chain rules one
 * pass in an order the generator fixed.
 */
#include "matcher.h"
#include "shapes_table.h"

/* A slot: what its field says it is. A text is a range of m->text, or of
 * the event being matched, or unbound. An operand is a text too: a kind
 * byte (SJS_OPERAND_*) and the literal's value, a number's 8 bytes or a
 * string's bytes; while its event is matched, in the step's own bytes. */
typedef union {
    double number;
    uint32_t count;
    struct {
        uint32_t offset;
        uint32_t length;
    } text;
} shape_value;

#define SHAPE_TEXT_NONE UINT32_MAX
#define SHAPE_TEXT_EVENT (UINT32_MAX - 1u)
#define SHAPE_TEXT_OPERAND (UINT32_MAX - 2u)

/* A cell or a marker. Every record has the same stride. */
typedef struct {
    uint64_t derived;      /* cell: a bit per nonterminal it derives, whose cost is then set */
    uint32_t node;
    uint32_t text_base;    /* its first byte in m->text */
    uint32_t outer;        /* marker: the marker around it */
    uint32_t alive;        /* marker: a bit per bracket rule of its kind still matching */
    uint32_t children;     /* marker: children folded so far */
    uint8_t kind;          /* the event that made it: a leaf's, a postfix's, a bracket's open */
    uint8_t marker;
    uint8_t dead;          /* a cell deriving nothing, or a marker with no rule alive */
    uint16_t cost[SHAPE_COSTS]; /* cell: per nonterminal; marker: per bracket rule of its kind */
    shape_value slots[SHAPE_SLOTS]; /* cell: by shape_slot; marker: by each rule's first_slot */
} shape_record;

/* An unreadable stream (5.5): never returned from sjs_matcher_event. */
#define MATCHER_UNREADABLE 2

/* The record stack's first allocation: what fits in 1 KiB, which a guard's
 * compile rarely outgrows (and which an allocator serves from its small
 * sizes). */
#define MATCHER_FIRST_RECORDS (1024u / (uint32_t)sizeof(shape_record))
/* Past 1 KiB a record, the first allocation would hold none: share slots
 * (docs/matcher.md, 11.4, [slots]) before the rules grow that far. */
_Static_assert(MATCHER_FIRST_RECORDS >= 1, "a shape_record outgrew the first 1 KiB of records");

/* What one event's matching works with. */
typedef struct {
    sjs_matcher *m;
    const sjs_event *e;
    shape_value *vars;     /* the variables of the rule being tried */
    uint32_t base;         /* a postfix event's first operand */
    uint32_t count;        /* its operands */
    uint32_t cost;         /* the cost of the derivation being accepted */
    bool event_text;       /* a test read the event's text: settle may copy it */
    bool event_operand;    /* a test read the event as an operand, into `operand` */
    shape_value local[SHAPE_VARIABLES];
    uint8_t operand[1u + SHAPE_TEXT_MAX];
} matcher_step;

static uint32_t matcher_count(const sjs_matcher *m) {
    return SJS_BUF_COUNT(m->records, shape_record);
}

static shape_record *matcher_record(sjs_matcher *m, uint32_t i) {
    return &SJS_BUF_AT(m->records, shape_record, i);
}

static uint32_t matcher_add_cost(uint32_t a, uint32_t b) {
    uint32_t sum = a + b;
    return sum >= SHAPE_INFINITE ? SHAPE_INFINITE - 1u : sum;
}

static sjs_str matcher_text(const matcher_step *s, shape_value v) {
    if (v.text.offset == SHAPE_TEXT_EVENT) return s->e->text;
    if (v.text.offset == SHAPE_TEXT_OPERAND) return (sjs_str){ (const char *)s->operand, v.text.length };
    return (sjs_str){ (const char *)s->m->text.ptr + v.text.offset, v.text.length };
}

/* ------------------------------------------------------------------------
 * Tests and links: binding a rule's variables, or checking them
 */

/* Bind a variable at its first occurrence, or check it holds the same. */
typedef struct {
    uint8_t field;
    uint8_t bind;
    shape_value value;
} shape_use;

static bool matcher_use(const matcher_step *s, shape_value *variable, shape_use use) {
    if (use.bind) {
        *variable = use.value;
        return true;
    }
    switch (use.field) {
    case SHAPE_FIELD_COUNT: return variable->count == use.value.count;
    case SHAPE_FIELD_NUMBER: return variable->number == use.value.number;
    /* Texts, and operands by their bytes: a literal's number is never NaN
     * or -0 (those are a name and a negation), so equal bits are ==. */
    default: return sjs_str_equal(matcher_text(s, *variable), matcher_text(s, use.value));
    }
}

/* The event read as an operand, into the step's bytes: false for an event
 * that is not a literal, and for a string too long to keep. */
static bool matcher_operand(matcher_step *s, shape_value *out) {
    const sjs_event *e = s->e;
    uint32_t length = 1;
    switch (e->kind) {
    case SJS_EV_NUMBER:
        s->operand[0] = SJS_OPERAND_NUMBER;
        sjs_copy(&s->operand[1], &e->number, sizeof e->number);
        length += (uint32_t)sizeof e->number;
        break;
    case SJS_EV_STRING:
        if (e->text.length > SHAPE_TEXT_MAX) return false;
        s->operand[0] = SJS_OPERAND_TEXT;
        sjs_copy(&s->operand[1], e->text.ptr, e->text.length);
        length += e->text.length;
        break;
    case SJS_EV_TRUE:
        s->operand[0] = SJS_OPERAND_TRUE;
        break;
    case SJS_EV_FALSE:
        s->operand[0] = SJS_OPERAND_FALSE;
        break;
    case SJS_EV_NULL:
        s->operand[0] = SJS_OPERAND_NULL;
        break;
    default:
        return false;
    }
    out->text.offset = SHAPE_TEXT_OPERAND;
    out->text.length = length;
    s->event_operand = true;
    return true;
}

/* The field of the event a test reads; false for a text too long to keep,
 * which fails the test. */
static bool matcher_field(matcher_step *s, uint8_t field, shape_value *out) {
    switch (field) {
    case SHAPE_FIELD_COUNT:
        out->count = s->e->count;
        return true;
    case SHAPE_FIELD_NUMBER:
        out->number = s->e->number;
        return true;
    case SHAPE_FIELD_OPERAND:
        return matcher_operand(s, out);
    default:
        if (s->e->text.length > SHAPE_TEXT_MAX) return false;
        out->text.offset = SHAPE_TEXT_EVENT;
        out->text.length = s->e->text.length;
        s->event_text = true;
        return true;
    }
}

/* A test against a constant: the generator gives none for an operand. */
static bool matcher_constant(const sjs_event *e, const shape_test *t, shape_value v) {
    switch (t->field) {
    case SHAPE_FIELD_COUNT: return v.count == t->constant;
    case SHAPE_FIELD_NUMBER: return v.number == shape_numbers[t->constant];
    default: return sjs_str_equal(e->text, shape_texts[t->constant]);
    }
}

/* The tests of a rule's event, or of a separator it wants, on s->e. */
static bool matcher_tests(matcher_step *s, uint16_t first, uint8_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const shape_test *t = &shape_tests[first + i];
        shape_value v;
        if (!matcher_field(s, t->field, &v)) return false;
        if (t->variable == SHAPE_CONSTANT) {
            if (!matcher_constant(s->e, t, v)) return false;
        } else if (!matcher_use(s, &s->vars[t->variable], (shape_use){ t->field, t->bind, v })) {
            return false;
        }
    }
    return true;
}

/* The attributes `cell` derives for the child's nonterminal, linked to the
 * rule's variables in order. */
static bool matcher_links(const matcher_step *s, const shape_record *cell, const shape_child *child) {
    uint32_t slot = shape_slot[child->nonterminal];
    for (uint32_t i = 0; i < child->link_count; i++) {
        const shape_link *link = &shape_links[child->first_link + i];
        shape_use use = { shape_slot_field[slot + i], link->bind, cell->slots[slot + i] };
        if (!matcher_use(s, &s->vars[link->variable], use)) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Deriving: iburg's labeller at one node
 */

static bool matcher_derives(const shape_record *cell, uint32_t nonterminal) {
    return (cell->derived >> nonterminal) & 1u;
}

/* The rule derives its lhs at s->cost with s->vars: keep it if cheaper. */
static void matcher_accept(matcher_step *s, shape_record *cell, const shape_rule *r) {
    if (matcher_derives(cell, r->lhs) && s->cost >= cell->cost[r->lhs]) return;
    cell->derived |= (uint64_t)1u << r->lhs;
    cell->dead = 0;
    cell->cost[r->lhs] = (uint16_t)s->cost;
    uint32_t slot = shape_slot[r->lhs];
    for (uint32_t i = 0; i < shape_attribute_count[r->lhs]; i++) {
        const shape_result *result = &shape_results[r->first_result + i];
        shape_value v = { 0 };
        if (result->variable != SHAPE_CONSTANT) v = s->vars[result->variable];
        else if (shape_slot_field[slot + i] == SHAPE_FIELD_NUMBER) v.number = shape_numbers[result->constant];
        else v.count = result->constant;
        cell->slots[slot + i] = v;
    }
}

/* Chain rules, in the order the generator fixed. */
static void matcher_close_cell(matcher_step *s, shape_record *cell) {
    for (uint32_t k = 0; k < SHAPE_CHAIN_COUNT; k++) {
        const shape_rule *r = &shape_rules[shape_chain_rules[k]];
        const shape_child *child = &shape_children[r->first_child];
        if (!matcher_derives(cell, child->nonterminal)) continue;
        s->vars = s->local;
        if (!matcher_links(s, cell, child)) continue;
        s->cost = matcher_add_cost(cell->cost[child->nonterminal], r->cost);
        matcher_accept(s, cell, r);
    }
}

/* A cell that derives nothing yet. */
static void matcher_new_cell(shape_record *cell, uint32_t node, uint8_t kind) {
    cell->node = node;
    cell->kind = kind;
    cell->marker = 0;
    cell->dead = 1;
    cell->derived = 0;
}

/* A leaf or a postfix event over the s->count cells from s->base. */
static void matcher_derive(matcher_step *s, shape_record *cell) {
    const shape_event *ev = &shape_events[s->e->kind];
    matcher_new_cell(cell, s->e->node, (uint8_t)s->e->kind);
    for (uint32_t i = 0; i < ev->rule_count; i++) {
        const shape_rule *r = &shape_rules[ev->first_rule + i];
        if (r->child_count != s->count) continue;
        s->vars = s->local;
        if (!matcher_tests(s, r->first_test, r->test_count)) continue;
        uint32_t cost = r->cost;
        bool ok = true;
        for (uint32_t c = 0; c < r->child_count && ok; c++) {
            const shape_child *child = &shape_children[r->first_child + c];
            const shape_record *operand = matcher_record(s->m, s->base + c);
            ok = matcher_derives(operand, child->nonterminal) && matcher_links(s, operand, child);
            if (ok) cost = matcher_add_cost(cost, operand->cost[child->nonterminal]);
        }
        if (!ok) continue;
        s->cost = cost;
        matcher_accept(s, cell, r);
    }
    matcher_close_cell(s, cell);
}

/* ------------------------------------------------------------------------
 * Text: what a record refers to, moved down to where its region began
 */

/* A field kept in m->text: a text, or an operand. */
static bool matcher_is_text(uint8_t field) {
    return field == SHAPE_FIELD_TEXT || field == SHAPE_FIELD_OPERAND;
}

/* The text slots of `record` that may refer to m->text at `region` or
 * after, or to the event or its operand: a cell's, of each nonterminal it derives; a
 * marker's, of each bracket rule still alive. Answers how many. */
static uint32_t matcher_text_slots(shape_record *record, shape_value **out) {
    uint32_t n = 0;
    if (record->dead) return 0;
    if (!record->marker) {
        for (uint64_t derived = record->derived; derived != 0; derived &= derived - 1u) {
            uint32_t nt = (uint32_t)__builtin_ctzll(derived);
            for (uint32_t i = 0; i < shape_attribute_count[nt]; i++) {
                uint32_t slot = shape_slot[nt] + i;
                if (matcher_is_text(shape_slot_field[slot])) out[n++] = &record->slots[slot];
            }
        }
        return n;
    }
    const shape_event *ev = &shape_events[record->kind];
    for (uint32_t j = 0; j < ev->rule_count; j++) {
        if ((record->alive & (1u << j)) == 0) continue;
        const shape_rule *r = &shape_rules[ev->first_rule + j];
        for (uint32_t v = 0; v < r->variable_count; v++) {
            if (matcher_is_text(shape_variable_fields[r->first_variable + v])) out[n++] = &record->slots[r->first_slot + v];
        }
    }
    return n;
}

/* Room for `extra` more bytes of text: in the matcher's own bytes while
 * they suffice, then on the heap. */
static sjs_result matcher_text_reserve(sjs_matcher *m, uint32_t extra) {
    if (extra <= m->text.capacity - m->text.length) return SJS_OK;
    if (m->text.ptr != m->text_inline) return sjs_buf_reserve(m->allocator, &m->text, extra);
    sjs_buf heap = { 0 };
    sjs_result r = sjs_buf_reserve(m->allocator, &heap, m->text.length + extra);
    if (r != SJS_OK) return r;
    sjs_copy(heap.ptr, m->text.ptr, m->text.length);
    heap.length = m->text.length;
    m->text = heap;
    return SJS_OK;
}

static void matcher_free_stacks(sjs_matcher *m) {
    sjs_buf_free(m->allocator, &m->records);
    if (m->text.ptr != m->text_inline) sjs_buf_free(m->allocator, &m->text);
    m->text = (sjs_buf){ NULL, 0, 0 };
}

/* MUTATION: the anchored lane's cells. */
static void matcher_free_lane(sjs_matcher *m) {
    if (m->lane.ptr != (uint8_t *)m->lane_inline) sjs_buf_free(m->allocator, &m->lane);
    m->lane = (sjs_buf){ NULL, 0, 0 };
}

/* Nothing is above the record's region: the bytes of the event (its text,
 * or its operand) that the record refers to go after everything, once. */
static sjs_result matcher_settle_event(matcher_step *s, shape_record *record, uint32_t sentinel) {
    sjs_matcher *m = s->m;
    shape_value *all[SHAPE_SLOTS];
    uint32_t total = matcher_text_slots(record, all);
    uint32_t at = m->text.length;
    bool copied = false;
    for (uint32_t i = 0; i < total; i++) {
        if (all[i]->text.offset != sentinel) continue;
        if (!copied) {
            sjs_str bytes = matcher_text(s, *all[i]);
            sjs_result r = matcher_text_reserve(m, bytes.length);
            if (r != SJS_OK) return r;
            sjs_copy(m->text.ptr + at, bytes.ptr, bytes.length);
            m->text.length += bytes.length;
            copied = true;
        }
        all[i]->text.offset = at;
    }
    return SJS_OK;
}

/* The text of `record` from `region` on, and the event's, which it refers
 * to, laid out from `region` in the order it was (the event's last); the
 * text stack ends after it. Each move goes down, so a forward copy is safe. */
static sjs_result matcher_settle(matcher_step *s, shape_record *record, uint32_t region) {
    sjs_matcher *m = s->m;
    /* Nothing above the region, and no bytes of the event: nothing moves. */
    if (region == m->text.length && !s->event_text && !s->event_operand) return SJS_OK;
    if (region == m->text.length) {
        sjs_result r = s->event_text ? matcher_settle_event(s, record, SHAPE_TEXT_EVENT) : SJS_OK;
        if (r == SJS_OK && s->event_operand) r = matcher_settle_event(s, record, SHAPE_TEXT_OPERAND);
        return r;
    }
    shape_value *all[SHAPE_SLOTS];
    shape_value *list[SHAPE_SLOTS];
    uint32_t total = matcher_text_slots(record, all);
    uint32_t n = 0;
    uint32_t extra = 0;     /* the event's bytes, which go on top */
    bool event = false;
    bool operand = false;
    for (uint32_t i = 0; i < total; i++) {
        uint32_t offset = all[i]->text.offset;
        if (offset == SHAPE_TEXT_NONE || offset < region) continue;
        if (offset == SHAPE_TEXT_EVENT && !event) {
            event = true;
            extra += s->e->text.length;
        }
        if (offset == SHAPE_TEXT_OPERAND && !operand) {
            operand = true;
            extra += all[i]->text.length;
        }
        /* Insertion by offset: few slots. */
        uint32_t at = n++;
        while (at > 0 && list[at - 1]->text.offset > offset) {
            list[at] = list[at - 1];
            at--;
        }
        list[at] = all[i];
    }
    if (extra > 0) {
        sjs_result r = matcher_text_reserve(m, extra);
        if (r != SJS_OK) return r;
    }
    uint32_t write = region;
    uint32_t last_from = SHAPE_TEXT_NONE;
    uint32_t last_length = 0;
    uint32_t last_to = 0;
    for (uint32_t i = 0; i < n; i++) {
        shape_value *v = list[i];
        /* The same text, kept by two slots: moved once. */
        if (v->text.offset == last_from && v->text.length == last_length) {
            v->text.offset = last_to;
            continue;
        }
        last_from = v->text.offset;
        last_length = v->text.length;
        last_to = write;
        if (v->text.offset == SHAPE_TEXT_EVENT || v->text.offset == SHAPE_TEXT_OPERAND) {
            sjs_copy(m->text.ptr + write, matcher_text(s, *v).ptr, v->text.length);
        } else {
            for (uint32_t b = 0; b < v->text.length; b++) m->text.ptr[write + b] = m->text.ptr[v->text.offset + b];
        }
        v->text.offset = write;
        write += v->text.length;
    }
    m->text.length = write;
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * The stack
 */

/* MUTATION: room for one more cell of the anchored lane: in the matcher's
 * own bytes while they suffice, then on the heap, doubling. */
static sjs_result matcher_lane_reserve(sjs_matcher *m) {
    if (m->lane.ptr != (uint8_t *)m->lane_inline) return sjs_buf_reserve(m->allocator, &m->lane, m->lane.capacity);
    sjs_buf heap = { 0 };
    sjs_result r = sjs_buf_reserve(m->allocator, &heap, 2u * m->lane.capacity);
    if (r != SJS_OK) return r;
    sjs_copy(heap.ptr, m->lane.ptr, m->lane.length);
    heap.length = m->lane.length;
    m->lane = heap;
    return SJS_OK;
}

/* Room for one more record, within the bound. */
static sjs_result matcher_reserve(sjs_matcher *m) {
    uint32_t n = matcher_count(m);
    if (n + 1u > m->bound) return MATCHER_UNREADABLE;
    if (m->records.length + (uint32_t)sizeof(shape_record) <= m->records.capacity) return SJS_OK;
    if (m->records.capacity == 0) {
        return sjs_buf_reserve(m->allocator, &m->records, MATCHER_FIRST_RECORDS * (uint32_t)sizeof(shape_record));
    }
    return sjs_buf_reserve(m->allocator, &m->records, (uint32_t)sizeof(shape_record));
}

static void matcher_grew(sjs_matcher *m) {
    uint32_t n = matcher_count(m);
    if (n > m->peak) m->peak = n;
}

/* The innermost marker is now `index`, or the program's. */
static void matcher_set_top(sjs_matcher *m, uint32_t index) {
    m->top = index;
    if (index == SJS_MATCHER_PROGRAM) {
        m->top_node = 0;
        m->top_dead = true;
        return;
    }
    const shape_record *marker = matcher_record(m, index);
    m->top_node = marker->node;
    m->top_dead = marker->dead != 0;
}

/* The innermost marker can match no more: a child of it derives nothing,
 * or will. Every rule wants each child to derive a nonterminal or to be a
 * given separator, so a child that derives nothing kills every rule, and
 * nothing inside the bracket is a child of a rule any more. Keep only the
 * marker, dead: from now on it stands for a dead region, whose events,
 * those of the brackets inside it included, the matcher passes over, but
 * for its own close and the opens of brackets rules look at. */
static void matcher_doom(sjs_matcher *m) {
    shape_record *marker = matcher_record(m, m->top);
    marker->dead = 1;
    marker->alive = 0;
    m->top_dead = true;
    m->records.length = (m->top + 1u) * (uint32_t)sizeof(shape_record);
    m->text.length = marker->text_base;
}

/* Fold one child into the marker: a cell, or (NULL) the event as a
 * separator. Each rule still alive takes it as its next child, or dies. */
static void matcher_fold(matcher_step *s, shape_record *marker, const shape_record *cell) {
    const shape_event *ev = &shape_events[marker->kind];
    for (uint32_t alive = marker->alive; alive != 0; alive &= alive - 1u) {
        uint32_t j = (uint32_t)__builtin_ctz(alive);
        const shape_rule *r = &shape_rules[ev->first_rule + j];
        bool ok = marker->children < r->child_count;
        if (ok) {
            const shape_child *child = &shape_children[r->first_child + marker->children];
            s->vars = &marker->slots[r->first_slot];
            if (child->nonterminal != 0) {
                ok = cell != NULL && matcher_derives(cell, child->nonterminal) && matcher_links(s, cell, child);
                if (ok) marker->cost[j] = (uint16_t)matcher_add_cost(marker->cost[j], cell->cost[child->nonterminal]);
            } else {
                ok = cell == NULL && s->e->kind == child->kind && matcher_tests(s, child->first_test, child->test_count);
            }
        }
        if (!ok) marker->alive &= ~(1u << j);
    }
    marker->children++;
    if (marker->alive == 0) marker->dead = 1;
}

/* Every cell above the innermost marker, which can still match, is a whole
 * child of it: fold them, left to right, and keep only the marker. Their
 * text stays where it is, now the marker's, until the marker closes or can
 * match no more: a marker that can match has at most as many children as
 * its longest rule, so what it keeps is bounded. */
static void matcher_fold_above(matcher_step *s) {
    sjs_matcher *m = s->m;
    uint32_t n = matcher_count(m);
    if (n == m->top + 1u) return;
    shape_record *marker = matcher_record(m, m->top);
    for (uint32_t i = m->top + 1u; i < n && !marker->dead; i++) matcher_fold(s, marker, matcher_record(m, i));
    if (marker->dead) {
        matcher_doom(m);
        return;
    }
    m->records.length = (m->top + 1u) * (uint32_t)sizeof(shape_record);
}

/* An operand's bytes, as a report gives them: its kind, and a number's
 * value or a string's text. */
static void matcher_report_operand(sjs_str bytes, sjs_shape_value *arg) {
    arg->operand = (uint8_t)bytes.ptr[0];
    if (arg->operand == SJS_OPERAND_NUMBER) sjs_copy(&arg->number, bytes.ptr + 1, sizeof arg->number);
    if (arg->operand == SJS_OPERAND_TEXT) arg->text = (sjs_str){ bytes.ptr + 1, bytes.length - 1u };
}

/* A cell that derives something was made for the event's node, at `index`:
 * report a shape it derives; then keep it in the bracket it is in, folded
 * at once if it is an item, unless that bracket is a dead region. */
static sjs_result matcher_created(matcher_step *s, uint32_t index) {
    sjs_matcher *m = s->m;
    const shape_record *cell = matcher_record(m, index);
    uint32_t best = SJS_SHAPE_NONE;
    for (uint32_t shape = 1; shape < SJS_SHAPE_COUNT; shape++) {
        if (matcher_derives(cell, shape) && (best == SJS_SHAPE_NONE || cell->cost[shape] < cell->cost[best])) best = shape;
    }
    if (best != SJS_SHAPE_NONE) {
        sjs_shape_match match = { (sjs_shape)best, s->e->node, s->e->start, { { 0 } } };
        for (uint32_t i = 0; i < shape_attribute_count[best] && i < SJS_SHAPE_ARGS; i++) {
            shape_value v = cell->slots[shape_slot[best] + i];
            sjs_shape_value *arg = &match.args[i];
            arg->kind = sjs_shape_arg_kind(best, i);
            if (arg->kind == SJS_SHAPE_ARG_TEXT) arg->text = matcher_text(s, v);
            else if (arg->kind == SJS_SHAPE_ARG_NUMBER) arg->number = v.number;
            else if (arg->kind == SJS_SHAPE_ARG_OPERAND) matcher_report_operand(matcher_text(s, v), arg);
            else arg->count = v.count;
        }
        sjs_result r = m->reporter.v->report(m->reporter.self, &match);
        if (r != SJS_OK) return r;
    }
    if (m->top_dead) {
        m->text.length = cell->text_base;
        m->records.length = index * (uint32_t)sizeof(shape_record);
        return SJS_OK;
    }
    if (shape_events[cell->kind].item) matcher_fold_above(s);
    return SJS_OK;
}

/* A leaf, or a postfix event over the top s->count cells, in a bracket
 * that can still match. */
static sjs_result matcher_node(matcher_step *s) {
    sjs_matcher *m = s->m;
    uint32_t n = matcher_count(m);
    if (n - (m->top + 1u) < s->count) return MATCHER_UNREADABLE;
    const shape_event *ev = &shape_events[s->e->kind];
    if (ev->rule_count == 0) {
        /* No rule looks at this event: it derives nothing. */
        matcher_doom(m);
        return SJS_OK;
    }
    if (s->count == 0) {
        sjs_result r = matcher_reserve(m);
        if (r != SJS_OK) return r;
    }
    s->base = n - s->count;
    uint32_t region = s->count == 0 ? m->text.length : matcher_record(m, s->base)->text_base;
    shape_record *cell = matcher_record(m, s->base);
    if (s->count == 0) {
        /* A leaf reads no record: derive it in place, past the top. */
        matcher_derive(s, cell);
    } else {
        shape_record made;
        matcher_derive(s, &made);
        if (!made.dead) *cell = made;
        else cell->dead = 1;
    }
    if (cell->dead) {
        matcher_doom(m);
        return SJS_OK;
    }
    cell->text_base = region;
    m->records.length = (s->base + 1u) * (uint32_t)sizeof(shape_record);
    matcher_grew(m);
    sjs_result r = matcher_settle(s, cell, region);
    if (r != SJS_OK) return r;
    return matcher_created(s, s->base);
}

/* An open event. A bracket that rules look at, and that one of them can
 * still match, gets a marker. Any other will derive nothing: in a bracket
 * that can still match, that bracket can match no more; in a dead region it
 * is part of the region. A bracket that takes the node before it as its
 * first child (a call its callee: the generator allows one such node) takes
 * the cell on top: the marker replaces it, keeps its text, and folds it as
 * child 0. In a dead region that cell was never kept, so such a bracket is
 * part of the region (docs/matcher.md, 11.9.2). */
static sjs_result matcher_open(matcher_step *s) {
    sjs_matcher *m = s->m;
    const shape_event *ev = &shape_events[s->e->kind];
    if (!m->top_dead && matcher_count(m) - (m->top + 1u) < ev->arity) return MATCHER_UNREADABLE;
    if (ev->rule_count == 0 || (ev->arity > 0 && m->top_dead)) {
        if (!m->top_dead) matcher_doom(m);
        return SJS_OK;
    }
    if (ev->arity == 0) {
        sjs_result r = matcher_reserve(m);
        if (r != SJS_OK) return r;
    }
    uint32_t at = matcher_count(m) - ev->arity;
    shape_record made;
    made.alive = 0;
    for (uint32_t j = 0; j < ev->rule_count; j++) {
        const shape_rule *rule = &shape_rules[ev->first_rule + j];
        s->vars = &made.slots[rule->first_slot];
        for (uint32_t v = 0; v < rule->variable_count; v++) s->vars[v].text.offset = SHAPE_TEXT_NONE;
        if (!matcher_tests(s, rule->first_test, rule->test_count)) continue;
        made.alive |= 1u << j;
        made.cost[j] = 0;
    }
    made.node = s->e->node;
    made.kind = (uint8_t)s->e->kind;
    made.marker = 1;
    made.dead = 0;
    made.outer = m->top;
    made.children = 0;
    made.text_base = m->text.length;
    if (ev->arity > 0 && made.alive != 0) {
        /* The cell on top is the first child: its text stays where it is,
         * now the marker's. */
        const shape_record *first = matcher_record(m, at);
        made.text_base = first->text_base;
        matcher_fold(s, &made, first);
    }
    if (made.alive == 0) {
        if (!m->top_dead) matcher_doom(m);
        return SJS_OK;
    }
    *matcher_record(m, at) = made;
    m->records.length = (at + 1u) * (uint32_t)sizeof(shape_record);
    matcher_set_top(m, at);
    matcher_grew(m);
    return matcher_settle(s, matcher_record(m, at), m->text.length);
}

/* The bracket of the innermost marker closes, or the dead region a marker
 * that died stands for ends. */
static sjs_result matcher_finish(matcher_step *s) {
    sjs_matcher *m = s->m;
    uint32_t at = m->top;
    shape_record *marker = matcher_record(m, at);
    if (!marker->dead) {
        matcher_fold_above(s);
        marker = matcher_record(m, at);
    }
    shape_record made;
    matcher_new_cell(&made, marker->node, marker->kind);
    made.text_base = marker->text_base;
    if (!marker->dead) {
        const shape_event *ev = &shape_events[marker->kind];
        for (uint32_t j = 0; j < ev->rule_count; j++) {
            const shape_rule *rule = &shape_rules[ev->first_rule + j];
            if ((marker->alive & (1u << j)) == 0 || rule->child_count != marker->children) continue;
            s->vars = &marker->slots[rule->first_slot];
            s->cost = matcher_add_cost(marker->cost[j], rule->cost);
            matcher_accept(s, &made, rule);
        }
        matcher_close_cell(s, &made);
    }
    matcher_set_top(m, marker->outer);
    if (made.dead) {
        /* Its cell derives nothing: the bracket around it can match no more. */
        m->records.length = at * (uint32_t)sizeof(shape_record);
        m->text.length = made.text_base;
        if (!m->top_dead) matcher_doom(m);
        return SJS_OK;
    }
    *marker = made;
    sjs_result r = matcher_settle(s, marker, marker->text_base);
    if (r != SJS_OK) return r;
    return matcher_created(s, at);
}

static sjs_result matcher_step_event(sjs_matcher *m, const sjs_event *e) {
    matcher_step s;
    s.m = m;
    s.e = e;
    s.count = 0;
    s.event_text = false;
    s.event_operand = false;
    const shape_event *ev = &shape_events[e->kind];
    if (e->node == m->top_node) {
        /* An event of the open bracket: a separator, or its close. */
        if (ev->action == SHAPE_CLOSE) {
            if (m->top == SJS_MATCHER_PROGRAM) return e->kind == SJS_EV_END ? SJS_OK : MATCHER_UNREADABLE;
            if (e->kind != shape_events[matcher_record(m, m->top)->kind].close) return MATCHER_UNREADABLE;
            return matcher_finish(&s);
        }
        if (m->top_dead) return SJS_OK;
        matcher_fold_above(&s);
        if (m->top_dead) return SJS_OK;
        shape_record *top = matcher_record(m, m->top);
        matcher_fold(&s, top, NULL);
        if (top->dead) {
            matcher_doom(m);
            return SJS_OK;
        }
        return matcher_settle(&s, top, m->text.length);
    }
    if (ev->action == SHAPE_OPEN) return matcher_open(&s);
    /* In a dead region, every other event is passed over. */
    if (m->top_dead) return SJS_OK;
    switch (ev->action) {
    case SHAPE_LEAF:
        return matcher_node(&s);
    case SHAPE_POSTFIX:
        s.count = ev->arity;
        return matcher_node(&s);
    case SHAPE_POSTFIX_COUNT:
        s.count = e->count != 0 ? 1u : 0u;
        return matcher_node(&s);
    default:
        /* A separator or a close of no open bracket. */
        return MATCHER_UNREADABLE;
    }
}

/* ------------------------------------------------------------------------
 * MUTATION: the anchored lane (docs/matcher.md, 12.4), which matches the
 * rewrites' rules beside the labeller, in a dead region too.
 *
 * A rewrite's rule is anchored: a leaf; a postfix event whose first operand
 * derives a nonterminal and whose others are anything; or a bracket over
 * the node before it, with anything inside. Its first operand is the node
 * that ended last among those that start where the event's node starts
 * (sjs_event.start: an operator's, a member's, an index's or a call's node
 * starts with its first operand). So the lane keeps a stack of cells, each
 * a node's start and the nonterminals it derives, their starts increasing:
 *
 *   - an event's node holds every cell that starts after it does, which
 *     ended inside it: they go;
 *   - a cell that starts where the event's node does is its first operand,
 *     or inside its first operand: it goes, and is the operand;
 *   - a leaf, a postfix event or a bracket's close that derives something
 *     pushes a cell; a bracket over an operand that one of its rules wants
 *     pushes a marker, whose separators pass and whose close derives.
 *
 * Each event costs a comparison or two of starts (an event no rule looks at
 * that starts after the top cell, most of them, one: matcher_lane_idle),
 * and the lane holds one cell per node still open around the current one,
 * at most. Nothing is
 * copied: the rules bind no text, and a report names the node and its
 * start.
 */

/* A cell (a node that derives something) or a marker (a bracket that one
 * of its rules can still match: `derived` is then a bit per bracket rule of
 * its kind that wants its operand). */
typedef struct {
    uint32_t start;
    uint32_t node;
    uint32_t derived;      /* cell: a bit per anchored nonterminal; marker: per bracket rule alive */
    uint8_t marker;
    uint8_t kind;          /* marker: its open's kind */
} matcher_lane_cell;

_Static_assert(SHAPE_ANCHORED_NONTERMINALS <= 32u, "an anchored cell's nonterminals fit a uint32_t");

static uint32_t matcher_lane_count(const sjs_matcher *m) {
    return SJS_BUF_COUNT(m->lane, matcher_lane_cell);
}

static matcher_lane_cell *matcher_lane_at(sjs_matcher *m, uint32_t i) {
    return &SJS_BUF_AT(m->lane, matcher_lane_cell, i);
}

/* The rule's tests, on the event: constants only. */
static bool matcher_lane_tests(const sjs_event *e, const shape_anchored_rule *r) {
    for (uint32_t i = 0; i < r->test_count; i++) {
        const shape_test *t = &shape_tests[r->first_test + i];
        shape_value v = { 0 };
        if (t->field == SHAPE_FIELD_COUNT) v.count = e->count;
        else if (t->field == SHAPE_FIELD_NUMBER) v.number = e->number;
        if (!matcher_constant(e, t, v)) return false;
    }
    return true;
}

/* What the chain rules add to what a node derives, in their order. */
static uint32_t matcher_lane_chains(uint32_t derived) {
    for (uint32_t k = 0; k < SHAPE_ANCHORED_CHAIN_COUNT; k++) {
        const shape_anchored_rule *r = &shape_anchored_rules[shape_anchored_chain_rules[k]];
        if ((derived >> r->child) & 1u) derived |= 1u << r->lhs;
    }
    return derived;
}

/* The node `e` ends derives `derived`: report its rewrites, then keep it,
 * unless it derives nothing. */
static sjs_result matcher_lane_push(sjs_matcher *m, const sjs_event *e, matcher_lane_cell cell) {
    if (!cell.marker) {
        cell.derived = matcher_lane_chains(cell.derived);
        for (uint32_t rewrite = 1; rewrite < SJS_REWRITE_COUNT; rewrite++) {
            if (((cell.derived >> rewrite) & 1u) == 0 || m->reporter.v->rewrite == NULL) continue;
            sjs_rewrite_match match = { (sjs_rewrite)rewrite, e->node, e->start };
            sjs_result r = m->reporter.v->rewrite(m->reporter.self, &match);
            if (r != SJS_OK) return r;
        }
    }
    if (cell.derived == 0) return SJS_OK;
    uint32_t n = matcher_lane_count(m);
    if (n + 1u > m->bound) return MATCHER_UNREADABLE;
    if (m->lane.length + (uint32_t)sizeof cell > m->lane.capacity) {
        sjs_result r = matcher_lane_reserve(m);
        if (r != SJS_OK) return r;
    }
    *matcher_lane_at(m, n) = cell;
    m->lane.length += (uint32_t)sizeof cell;
    if (n + 1u > m->lane_peak) m->lane_peak = n + 1u;
    return SJS_OK;
}

/* An event that no anchored rule looks at and that starts after every cell:
 * it ends none and takes none as its operand, so it changes nothing. Most
 * events are such, leaves and the operators over them aside. */
static bool matcher_lane_idle(const sjs_matcher *m, const sjs_event *e) {
    if (shape_anchored_events[e->kind].rule_count != 0) return false;
    if (m->lane.length == 0) return true;
    const matcher_lane_cell *top = (const matcher_lane_cell *)(const void *)(m->lane.ptr + m->lane.length - sizeof *top);
    return top->start < e->start.offset;
}

/* One event, before the labeller and the compiler see it. */
static sjs_result matcher_lane_event(sjs_matcher *m, const sjs_event *e) {
    uint32_t start = e->start.offset;
    uint32_t n = matcher_lane_count(m);
    /* What started after this node ended inside it. */
    while (n > 0 && matcher_lane_at(m, n - 1u)->start > start) n--;
    m->lane.length = n * (uint32_t)sizeof(matcher_lane_cell);
    const shape_anchored_event *ae = &shape_anchored_events[e->kind];
    matcher_lane_cell *top = n > 0 ? matcher_lane_at(m, n - 1u) : NULL;
    if (top != NULL && top->marker && top->node == e->node) {
        /* A separator of the bracket, which `...` takes, or its close. */
        if (shape_events[e->kind].action != SHAPE_CLOSE) return SJS_OK;
        if (e->kind != shape_events[top->kind].close) return MATCHER_UNREADABLE;
        const shape_anchored_event *open = &shape_anchored_events[top->kind];
        matcher_lane_cell cell = { top->start, e->node, 0, 0, 0 };
        for (uint32_t j = 0; j < open->rule_count; j++) {
            if ((top->derived >> j) & 1u) cell.derived |= 1u << shape_anchored_rules[open->first_rule + j].lhs;
        }
        m->lane.length -= (uint32_t)sizeof(matcher_lane_cell);
        return matcher_lane_push(m, e, cell);
    }
    /* A node that starts where this one does, and ended before it, is its
     * first operand, or inside it: it goes either way. */
    uint32_t operand = 0;
    if (top != NULL && top->start == start) {
        if (!top->marker) operand = top->derived;
        m->lane.length -= (uint32_t)sizeof(matcher_lane_cell);
    }
    if (ae->rule_count == 0) return SJS_OK;
    uint8_t action = shape_events[e->kind].action;
    if (action != SHAPE_LEAF && operand == 0) return SJS_OK;
    matcher_lane_cell cell = { start, e->node, 0, action == SHAPE_OPEN ? 1u : 0u, (uint8_t)e->kind };
    for (uint32_t j = 0; j < ae->rule_count; j++) {
        const shape_anchored_rule *r = &shape_anchored_rules[ae->first_rule + j];
        if (r->form != SHAPE_FORM_LEAF && ((operand >> r->child) & 1u) == 0) continue;
        if (!matcher_lane_tests(e, r)) continue;
        cell.derived |= cell.marker ? 1u << j : 1u << r->lhs;
    }
    return matcher_lane_push(m, e, cell);
}

/* ------------------------------------------------------------------------
 * The interface
 */

void sjs_matcher_init(sjs_matcher *matcher, const sjs_matcher_config *config) {
    sjs_zero(matcher, sizeof *matcher);
    matcher->allocator = config->allocator;
    matcher->reporter = config->reporter;
    matcher->bound = 4u * config->max_nesting + 4u;
    matcher->text = (sjs_buf){ matcher->text_inline, 0, (uint32_t)sizeof matcher->text_inline };
    matcher_set_top(matcher, SJS_MATCHER_PROGRAM);
    /* MUTATION */
    matcher->rewrites_only = config->rewrites_only;
    matcher->lane = (sjs_buf){ (uint8_t *)matcher->lane_inline, 0, (uint32_t)sizeof matcher->lane_inline };
}

sjs_result sjs_matcher_event(sjs_matcher *matcher, const sjs_event *event) {
    bool readable = event->kind >= SJS_EV_NUMBER && event->kind <= SJS_EV_END && (event->node != 0 || event->kind == SJS_EV_END);
    /* MUTATION: the anchored lane first, which stops on its own. */
    if (!matcher->lane_stopped && !(readable && matcher_lane_idle(matcher, event))) {
        sjs_result lane = readable ? matcher_lane_event(matcher, event) : MATCHER_UNREADABLE;
        if (lane == MATCHER_UNREADABLE) {
            matcher->lane_stopped = true;
            matcher_free_lane(matcher);
        } else if (lane != SJS_OK) {
            return lane;
        }
    }
    if (matcher->stopped || matcher->rewrites_only) return SJS_OK;
    sjs_result r = MATCHER_UNREADABLE;
    if (readable) { /* MUTATION: what the lane reads too */
        /* Most events: in a dead region, and neither an open nor its end. */
        if (matcher->top_dead && event->node != matcher->top_node && shape_events[event->kind].action != SHAPE_OPEN) {
            return SJS_OK;
        }
        r = matcher_step_event(matcher, event);
    }
    if (r != MATCHER_UNREADABLE) return r;
    matcher->stopped = true;
    matcher_free_stacks(matcher);
    return SJS_OK;
}

void sjs_matcher_free(sjs_matcher *matcher) {
    matcher_free_stacks(matcher);
    matcher_free_lane(matcher); /* MUTATION */
}
