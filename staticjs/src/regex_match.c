/*
 * Regular expressions when a program runs (docs/regex.md, sections 4, 5, 7,
 * 8 and 9): their properties, the automaton that matches them, and the
 * methods that use it.
 *
 * The automaton is a Pike VM: every thread runs in step over the input, in
 * priority order, so a match costs time linear in the input. Two threads at
 * one program counter are one only when the same loop registers equal the
 * position (section 5.3), which is all CHECK can see of them. A search
 * advances over a budget of instructions visited, finishing the position it
 * is at, and returns, so a method runs as a loop frame whose steps pause
 * like any loop's; a short one runs whole as a leaf, in a buffer of the
 * evaluator's, and charges the fuel its steps would have (section 9.1).
 *
 * Positions are encoded: a byte offset times two, plus one between the two
 * UTF-16 code units of a character outside the BMP (without u, where the
 * input is code units). An index a program sees is in code units.
 */
#include "vm.h"
#include "regex_program.h"

static sjs_result regex_fault(sjs_vm *vm, const char *message, sjs_str subject) {
    sjs_vm_fault(vm, message);
    sjs_error_subject(&vm->error, subject);
    return SJS_ERR_PROGRAM;
}

/* ------------------------------------------------------------------------
 * Properties
 */

/* RegExp.prototype's getters of the flags, by name. */
static const struct {
    const char *name;
    uint32_t flag;
} FLAG_GETTERS[] = {
    { "hasIndices", SJS_REGEX_HAS_INDICES }, { "global", SJS_REGEX_GLOBAL },
    { "ignoreCase", SJS_REGEX_IGNORE_CASE }, { "multiline", SJS_REGEX_MULTILINE },
    { "dotAll", SJS_REGEX_DOT_ALL },         { "unicode", SJS_REGEX_UNICODE },
    { "unicodeSets", SJS_REGEX_UNICODE_SETS }, { "sticky", SJS_REGEX_STICKY },
};

static sjs_result new_text(sjs_vm *vm, sjs_str text, sjs_value *out) {
    sjs_string *s;
    sjs_result r = sjs_string_make(vm->context, text, &s);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    *out = sjs_heap_value(SJS_STRING, &s->h);
    return SJS_OK;
}

sjs_result sjs_regex_property(sjs_vm *vm, sjs_member member, sjs_value *out) {
    const sjs_regexp *re = SJS_AS_REGEXP(member.object);
    sjs_str name = sjs_string_view(member.key);
    *out = sjs_undefined();
    if (sjs_str_equal(name, SJS_STR("source"))) return new_text(vm, sjs_regex_source(&re->h), out);
    if (sjs_str_equal(name, SJS_STR("flags"))) {
        char flags[8];
        return new_text(vm, (sjs_str){ flags, sjs_regex_flag_text(re->flags, flags) }, out);
    }
    if (sjs_str_equal(name, SJS_STR("lastIndex"))) {
        *out = sjs_num(0);
        return SJS_OK;
    }
    for (uint32_t i = 0; i < sizeof FLAG_GETTERS / sizeof FLAG_GETTERS[0]; i++) {
        if (sjs_str_equal(name, sjs_cstr(FLAG_GETTERS[i].name))) {
            *out = sjs_boolean((re->flags & FLAG_GETTERS[i].flag) != 0);
            return SJS_OK;
        }
    }
    const sjs_name *n = sjs_name_of(member.key);
    if (n != NULL && (n->flags & SJS_NAME_ON_REGEXP) != 0) {
        return regex_fault(vm, "staticjs calls methods; it does not read them as values", name);
    }
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * The input: code units (or code points, with u) at encoded positions
 */

/* No character: before the start, or at the end. */
#define RX_NONE UINT32_MAX
/* A position past the end, where no search starts. */
#define RX_BEYOND (UINT32_MAX - 1u)

typedef struct {
    const uint8_t *text;
    uint32_t length;      /* bytes */
    bool ascii;
    bool unicode;         /* code points, not code units */
} rx_input;

/* The character at encoded position `p`, or RX_NONE at the end; `*next`
 * is the position after it. */
static uint32_t rx_unit_at(const rx_input *in, uint32_t p, uint32_t *next) {
    uint32_t byte = p >> 1;
    if (byte >= in->length) {
        *next = p;
        return RX_NONE;
    }
    if (in->ascii) {
        *next = p + 2u;
        return in->text[byte];
    }
    uint32_t i = byte;
    uint32_t cp = (uint32_t)sjs_utf8_next((sjs_str){ (const char *)in->text, in->length }, &i);
    if (cp < 0x10000u || in->unicode) {
        *next = i << 1;
        return cp;
    }
    if ((p & 1u) == 0) {
        *next = p + 1u;
        return 0xD800u + ((cp - 0x10000u) >> 10);
    }
    *next = i << 1;
    return 0xDC00u + ((cp - 0x10000u) & 0x3FFu);
}

/* The character before position `p`, or RX_NONE at the start. */
static uint32_t rx_unit_before(const rx_input *in, uint32_t p) {
    if (p == 0) return RX_NONE;
    uint32_t byte = p >> 1;
    if ((p & 1u) != 0) {
        uint32_t next;
        return rx_unit_at(in, byte << 1, &next);
    }
    uint32_t start = byte - 1u;
    while (start > 0 && (in->text[start] & 0xC0u) == 0x80u) start--;
    uint32_t i = start;
    uint32_t cp = (uint32_t)sjs_utf8_next((sjs_str){ (const char *)in->text, in->length }, &i);
    if (cp < 0x10000u || in->unicode) return cp;
    return 0xDC00u + ((cp - 0x10000u) & 0x3FFu);
}

/* ------------------------------------------------------------------------
 * A search: the automaton over the input from one position
 */

typedef struct {
    const sjs_regexp *re;
    rx_input in;
    bool sticky;              /* matches only at `start` */
    bool mid_starts;          /* with u, starts between a pair's halves too, as V8 (rx_mid_start) */
    bool fold_unicode;        /* i with u: U+017F folds to s, U+212A to k */
    bool matched;
    bool running;
    uint32_t start;
    /* Where the automaton is: the position, the characters before and at
     * it, and the position after that one. */
    uint32_t pos;
    uint32_t prev;
    uint32_t unit;
    uint32_t next;
    /* Where a closure is computed (the position, the characters around). */
    uint32_t at_pos;
    uint32_t at_prev;
    uint32_t at_unit;
    uint32_t generation;
    uint32_t ccount;
    uint32_t ncount;
    uint32_t *cpc;
    uint32_t *npc;
    uint32_t *cslots;
    uint32_t *nslots;
    uint32_t *stamps;         /* by pc << registers | mask: the generation it was last reached in */
    uint32_t *stack;          /* the closure's: three words an entry */
    uint32_t *saved;          /* slots a CLEAR cleared, to restore */
    uint32_t *cur;            /* the thread being followed */
    uint32_t *best;           /* the match */
    uint64_t visits;          /* instructions visited */
    uint64_t copies;          /* slots copied */
} rx_search;

/* The words a search needs after its header. */
static uint64_t rx_search_words(const sjs_regexp *re) {
    uint64_t k = re->threads;
    uint64_t s = re->slots;
    return 2u * k + 2u * k * s + k + 3u * (k + 1u) + ((uint64_t)re->clear_slots << re->registers) + 2u * s;
}

/* Lay the search's arrays out in `words`, which rx_search_words long, are
 * zeroed. */
static void rx_search_layout(rx_search *s, uint32_t *words) {
    uint32_t k = s->re->threads;
    uint32_t n = s->re->slots;
    s->cpc = words;
    words += k;
    s->npc = words;
    words += k;
    s->cslots = words;
    words += (size_t)k * n;
    s->nslots = words;
    words += (size_t)k * n;
    s->stamps = words;
    words += k;
    s->stack = words;
    words += 3u * ((size_t)k + 1u);
    s->saved = words;
    words += (size_t)s->re->clear_slots << s->re->registers;
    s->cur = words;
    words += n;
    s->best = words;
}

static bool rx_line_terminator(uint32_t c) {
    return c == 0x0Au || c == 0x0Du || c == 0x2028u || c == 0x2029u;
}

static bool rx_word(const rx_search *s, uint32_t c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') return true;
    return s->fold_unicode && (c == 0x17Fu || c == 0x212Au);
}

static uint32_t rx_fold(const rx_search *s, uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32u;
    if (s->fold_unicode) {
        if (c == 0x17Fu) return 's';
        if (c == 0x212Au) return 'k';
    }
    return c;
}

static bool rx_in_class(const sjs_regexp *re, uint32_t index, uint32_t c) {
    const rx_class *k = &re->classes[index];
    const uint32_t *r = re->ranges + 2u * k->start;
    uint32_t low = 0;
    uint32_t high = k->count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2u;
        if (c < r[2u * mid]) high = mid;
        else if (c > r[2u * mid + 1u]) low = mid + 1u;
        else return true;
    }
    return false;
}

static bool rx_consumes(const rx_search *s, const rx_ins *ins, uint32_t c) {
    if (c == RX_NONE) return false;
    switch (ins->op) {
    case RX_CHAR: return c == ins->x;
    case RX_CHAR_FOLD: return rx_fold(s, c) == ins->x;
    case RX_ANY: return !rx_line_terminator(c);
    case RX_ANY_ALL: return true;
    case RX_CLASS: return rx_in_class(s->re, ins->x, c);
    default: return false;
    }
}

static bool rx_holds(const rx_search *s, uint32_t kind) {
    switch (kind) {
    case RX_ASSERT_START: return s->at_pos == 0;
    case RX_ASSERT_LINE_START: return s->at_pos == 0 || rx_line_terminator(s->at_prev);
    case RX_ASSERT_END: return s->at_unit == RX_NONE;
    case RX_ASSERT_LINE_END: return s->at_unit == RX_NONE || rx_line_terminator(s->at_unit);
    case RX_ASSERT_WORD: return rx_word(s, s->at_prev) != rx_word(s, s->at_unit);
    default: return rx_word(s, s->at_prev) == rx_word(s, s->at_unit);
    }
}

/* The thread list a closure adds to. */
typedef struct {
    uint32_t *pcs;
    uint32_t *slots;
    uint32_t *count;
} rx_threads;

enum { RX_FOLLOW = 0, RX_RESTORE, RX_RESTORE_RANGE };

/* Follow every instruction that consumes nothing from `pc`, with the slots
 * in s->cur, at s->at_*, in priority order, over an explicit stack; each
 * thread that reaches a consuming instruction or MATCH joins the list,
 * unless a thread of the same pc and the same registers at the position is
 * there (docs/regex.md, 5.3). s->cur is as it was when it returns. */
static void rx_closure(rx_search *s, const rx_threads *list, uint32_t pc) {
    const sjs_regexp *re = s->re;
    uint32_t d = re->registers;
    uint32_t base = 2u * (re->groups + 1u);
    uint32_t n = re->slots;
    uint32_t pos = s->at_pos;
    uint32_t *st = s->stack;
    uint32_t sp = 0;
    uint32_t vp = 0;
    st[sp++] = RX_FOLLOW;
    st[sp++] = pc;
    st[sp++] = 0;
    while (sp > 0) {
        sp -= 3u;
        uint32_t kind = st[sp];
        uint32_t a = st[sp + 1u];
        uint32_t b = st[sp + 2u];
        if (kind == RX_RESTORE) {
            s->cur[a] = b;
            continue;
        }
        if (kind == RX_RESTORE_RANGE) {
            vp -= b - a + 1u;
            for (uint32_t k = a; k <= b; k++) s->cur[k] = s->saved[vp + k - a];
            continue;
        }
        pc = a;
        bool alive = true;
        while (alive) {
            const rx_ins *ins = &re->code[pc];
            s->visits++;
            uint32_t mask = 0;
            for (uint32_t j = 0; j < ins->relevant; j++) {
                if (s->cur[base + j] == pos) mask |= 1u << j;
            }
            uint32_t key = pc << d | mask;
            if (s->stamps[key] == s->generation) break;
            s->stamps[key] = s->generation;
            switch (ins->op) {
            case RX_JMP:
                pc = ins->x;
                break;
            case RX_SPLIT:
                st[sp++] = RX_FOLLOW;
                st[sp++] = ins->y;
                st[sp++] = 0;
                pc = ins->x;
                break;
            case RX_SAVE:
            case RX_MARK:
                st[sp++] = RX_RESTORE;
                st[sp++] = ins->x;
                st[sp++] = s->cur[ins->x];
                s->cur[ins->x] = pos;
                pc++;
                break;
            case RX_CLEAR:
                for (uint32_t k = ins->x; k <= ins->y; k++) {
                    s->saved[vp++] = s->cur[k];
                    s->cur[k] = RX_UNSET;
                }
                st[sp++] = RX_RESTORE_RANGE;
                st[sp++] = ins->x;
                st[sp++] = ins->y;
                pc++;
                break;
            case RX_CHECK:
                if (s->cur[ins->x] == pos) alive = false;
                pc++;
                break;
            case RX_ASSERT:
                if (!rx_holds(s, ins->x)) alive = false;
                pc++;
                break;
            default: {
                uint32_t at = *list->count;
                list->pcs[at] = pc;
                for (uint32_t k = 0; k < n; k++) list->slots[(size_t)at * n + k] = s->cur[k];
                s->copies += n;
                *list->count = at + 1u;
                alive = false;
                break;
            }
            }
        }
    }
}

/*
 * With u, V8 also tries a search's start between the two halves of a pair
 * (Node 22: "a\u{1F600}".search(/\B/u) is 2), where the halves are the
 * characters around it for an assertion and nothing can be consumed: only
 * an empty match can start there. Its thread comes after every thread of
 * an earlier start, which are now one character on, and has priority over
 * none of them; so its match, if it has one, stands until one of theirs
 * matches. Its closure is computed in the thread list just used, at a
 * generation of its own (the step's generation plus one).
 */
static void rx_mid_start(rx_search *s) {
    uint32_t point = s->unit;
    uint32_t count = 0;
    uint32_t n = s->re->slots;
    s->generation++;
    for (uint32_t k = 0; k < n; k++) s->cur[k] = RX_UNSET;
    s->copies += n;
    s->at_pos = s->pos + 1u;
    s->at_prev = 0xD800u + ((point - 0x10000u) >> 10);
    s->at_unit = 0xDC00u + ((point - 0x10000u) & 0x3FFu);
    rx_threads there = { s->cpc, s->cslots, &count };
    rx_closure(s, &there, 0);
    s->generation--;
    for (uint32_t i = 0; i < count; i++) {
        if (s->re->code[s->cpc[i]].op != RX_MATCH) continue;
        for (uint32_t k = 0; k < n; k++) s->best[k] = s->cslots[(size_t)i * n + k];
        s->copies += n;
        s->matched = true;
        break;
    }
}

static void rx_search_begin(rx_search *s, uint32_t start) {
    s->start = start;
    s->pos = start;
    s->prev = rx_unit_before(&s->in, start);
    s->unit = rx_unit_at(&s->in, start, &s->next);
    s->generation += 2u;
    s->ccount = 0;
    s->matched = false;
    s->running = true;
}

/* Run the search until it ends (true; s->matched says whether s->best
 * holds a match) or it has visited `budget` instructions more (false),
 * finishing the position it is at. */
static bool rx_search_run(rx_search *s, uint64_t budget) {
    uint64_t until = s->visits + budget;
    uint32_t n = s->re->slots;
    for (;;) {
        if (!s->matched && (!s->sticky || s->pos == s->start)) {
            /* A thread from here, after every thread from earlier. */
            for (uint32_t k = 0; k < n; k++) s->cur[k] = RX_UNSET;
            s->copies += n;
            s->at_pos = s->pos;
            s->at_prev = s->prev;
            s->at_unit = s->unit;
            rx_threads now = { s->cpc, s->cslots, &s->ccount };
            rx_closure(s, &now, 0);
        }
        if (s->ccount == 0 && (s->matched || s->sticky || s->unit == RX_NONE)) {
            s->running = false;
            return true;
        }
        uint32_t after = s->next;
        uint32_t after_next = after;
        uint32_t after_unit = s->unit == RX_NONE ? RX_NONE : rx_unit_at(&s->in, after, &after_next);
        s->generation += 2u;
        s->ncount = 0;
        s->at_pos = after;
        s->at_prev = s->unit;
        s->at_unit = after_unit;
        rx_threads next = { s->npc, s->nslots, &s->ncount };
        for (uint32_t i = 0; i < s->ccount; i++) {
            const rx_ins *ins = &s->re->code[s->cpc[i]];
            s->visits++;
            const uint32_t *slots = &s->cslots[(size_t)i * n];
            if (ins->op == RX_MATCH) {
                /* The threads after it have lower priority: they end. */
                for (uint32_t k = 0; k < n; k++) s->best[k] = slots[k];
                s->copies += n;
                s->matched = true;
                break;
            }
            if (!rx_consumes(s, ins, s->unit)) continue;
            for (uint32_t k = 0; k < n; k++) s->cur[k] = slots[k];
            s->copies += n;
            rx_closure(s, &next, s->cpc[i] + 1u);
        }
        if (s->unit == RX_NONE) {
            s->running = false;
            return true;
        }
        if (s->unit >= 0x10000u && s->mid_starts && !s->matched && !s->sticky) rx_mid_start(s);
        uint32_t *swap = s->cpc;
        s->cpc = s->npc;
        s->npc = swap;
        swap = s->cslots;
        s->cslots = s->nslots;
        s->nslots = swap;
        s->ccount = s->ncount;
        s->prev = s->unit;
        s->pos = after;
        s->unit = after_unit;
        s->next = after_next;
        if (s->visits >= until) return false;
    }
}

/* ------------------------------------------------------------------------
 * A method's work: its searches, and what it makes of their matches
 */

/* What a method computes (vm.h). */
#define RX_TEST SJS_REGEX_OP_TEST
#define RX_SEARCH SJS_REGEX_OP_SEARCH
#define RX_MATCH SJS_REGEX_OP_MATCH
#define RX_SPLIT SJS_REGEX_OP_SPLIT
#define RX_REPLACE SJS_REGEX_OP_REPLACE
#define RX_EXEC SJS_REGEX_OP_EXEC
#define RX_MATCH_ALL SJS_REGEX_OP_MATCH_ALL

/* What a step of the work comes to: RX_CALL, a replacement's function to
 * call with the match (a loop's only). */
enum { RX_MORE = 0, RX_DONE, RX_FAULT, RX_CALL };

/* Instructions a loop step visits, finishing the position it is at. */
#define RX_STEP_VISITS 256u

typedef struct {
    rx_search s;
    uint8_t op;
    bool global;
    bool done;
    bool started;             /* a loop's first step is done */
    bool callback;            /* replace: with a function, in LOOP_CALLBACK */
    bool awaiting;            /* replace: the function was called with the match, its result is on the stack */
    uint32_t from;            /* where the next search starts */
    uint32_t last;            /* the text copied up to (replace), the piece's start (split) */
    uint32_t limit;           /* split: the pieces it may give */
    uint32_t count;           /* split: the pieces it gave */
    uint32_t high;            /* replace: the first half of a pair, waiting for its second */
    uint32_t cursor_byte;     /* the last UTF-16 index found, and its byte */
    uint32_t cursor_units;
    const sjs_string *input;
    sjs_str replacement;      /* replace: the template */
    char room[32];            /* replace: the template's text, when the replacement is not a string */
    uint64_t extra;           /* fuel for what was built */
    uint64_t charged;         /* fuel charged so far */
} rx_job;

typedef struct {
    sjs_vm *vm;
    rx_job *job;
    sjs_value *out;           /* the result being built: a loop's output slot, or a leaf's result */
} rx_work;

/* Fuel for the work so far (docs/regex.md, 9.1), charged as it is done. */
static void rx_charge(rx_work *w) {
    rx_job *j = w->job;
    uint64_t total = j->s.visits / 8u + j->s.copies / 16u + j->extra;
    w->vm->fuel_used += total - j->charged;
    j->charged = total;
}

static uint64_t rx_job_words(const sjs_regexp *re) {
    return (sizeof(rx_job) + 3u) / 4u + rx_search_words(re);
}

/* The UTF-16 index of position `p`. */
static uint32_t rx_index(rx_job *j, uint32_t p) {
    uint32_t byte = p >> 1;
    if (j->s.in.ascii) return byte;
    if (byte < j->cursor_byte) {
        j->cursor_byte = 0;
        j->cursor_units = 0;
    }
    j->extra += (byte - j->cursor_byte) / 16u;
    j->cursor_units += sjs_utf16_length((sjs_str){ (const char *)j->s.in.text + j->cursor_byte, byte - j->cursor_byte });
    j->cursor_byte = byte;
    return j->cursor_units + (p & 1u);
}

/* The position after the one at `p`: a code unit on, or with u a code
 * point (AdvanceStringIndex); RX_BEYOND past the end. */
static uint32_t rx_advance_from(const rx_job *j, uint32_t p) {
    uint32_t next;
    if (rx_unit_at(&j->s.in, p, &next) == RX_NONE) return RX_BEYOND;
    return next;
}

static sjs_result rx_lone(sjs_vm *vm) {
    return sjs_vm_fault(vm, "staticjs strings cannot hold a lone surrogate");
}

/* Positions from one to another. */
typedef struct {
    uint32_t from;
    uint32_t to;
} rx_span;

/* The input over a span as a new string; a fault when it would begin or end
 * inside a pair. */
static sjs_result rx_piece(rx_work *w, rx_span span, sjs_value *out) {
    uint32_t a = span.from;
    uint32_t b = span.to;
    if (a == b) {
        /* Empty, even between the halves of a pair. */
        a &= ~1u;
        b = a;
    }
    if ((a & 1u) != 0 || (b & 1u) != 0) return rx_lone(w->vm);
    sjs_str text = { (const char *)w->job->s.in.text + (a >> 1), (b >> 1) - (a >> 1) };
    w->job->extra += 1u + text.length / 16u;
    return new_text(w->vm, text, out);
}

/* Append to the array being built. */
static sjs_result rx_push(rx_work *w, sjs_value v) {
    sjs_array *a = SJS_AS_ARRAY(*w->out);
    sjs_result r = sjs_array_grow(w->vm->context, &a, 1);
    w->out->as.heap = &a->h;
    if (r != SJS_OK) {
        sjs_unref(w->vm->context, v);
        return sjs_vm_fail(w->vm, r);
    }
    a->items[a->length++] = v;
    return SJS_OK;
}

static sjs_result rx_push_piece(rx_work *w, uint32_t a, uint32_t b) {
    sjs_value v;
    if (rx_piece(w, (rx_span){ a, b }, &v) != SJS_OK) return SJS_ERR_PROGRAM;
    return rx_push(w, v);
}

/* ------------------------------------------------------------------------
 * Match results (docs/regex.md, 7.2): an array of the match and each
 * group's text, with index, input and groups, and with d indices, as
 * properties of its own besides its elements (SJS_ARRAY_EXTRA).
 */

/* Put `value` (taken over) under the key `name` of the object at `*o`. */
typedef struct {
    sjs_str name;
    sjs_value value;
} rx_property;

static sjs_result rx_put(sjs_vm *vm, sjs_object **o, rx_property p) {
    sjs_string *key;
    sjs_result r = sjs_string_intern(vm->context, p.name, &key);
    if (r == SJS_OK) {
        r = sjs_object_put_key(vm->context, o, (sjs_put){ key, p.value });
        if (r != SJS_OK) sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &key->h));
    }
    if (r != SJS_OK) {
        sjs_unref(vm->context, p.value);
        return sjs_vm_fail(vm, r);
    }
    return SJS_OK;
}

/* What a group gives: its text, or with d its [start, end] in code units;
 * undefined when it did not match. */
typedef struct {
    uint32_t group;
    bool indices;
} rx_group_part;

static sjs_result rx_group_value(rx_work *w, rx_group_part part, sjs_value *out) {
    const uint32_t *best = w->job->s.best;
    uint32_t a = best[2u * part.group];
    uint32_t b = best[2u * part.group + 1u];
    *out = sjs_undefined();
    if (a == RX_UNSET) return SJS_OK;
    if (!part.indices) return rx_piece(w, (rx_span){ a, b }, out);
    sjs_array *pair;
    sjs_result r = sjs_array_make(w->vm->context, 2, &pair);
    if (r != SJS_OK) return sjs_vm_fail(w->vm, r);
    pair->items[0] = sjs_num((double)rx_index(w->job, a));
    pair->items[1] = sjs_num((double)rx_index(w->job, b));
    pair->length = 2;
    w->job->extra += 2u;
    *out = sjs_heap_value(SJS_ARRAY, &pair->h);
    return SJS_OK;
}

/* Each builder below gives its value, or on failure releases what it made
 * and faults. */

/* The groups object: each named group's part, in the groups' order; or
 * undefined when no group has a name. */
static sjs_result rx_groups(rx_work *w, bool indices, sjs_value *out) {
    const sjs_regexp *re = w->job->s.re;
    *out = sjs_undefined();
    if (!re->named) return SJS_OK;
    sjs_object *o;
    sjs_result r = sjs_object_make(w->vm->context, re->groups, &o);
    if (r != SJS_OK) return sjs_vm_fail(w->vm, r);
    for (uint32_t g = 1; g <= re->groups; g++) {
        const rx_group_name *n = &re->names[g - 1u];
        if (n->length == 0) continue;
        sjs_value v;
        r = rx_group_value(w, (rx_group_part){ g, indices }, &v);
        if (r == SJS_OK) r = rx_put(w->vm, &o, (rx_property){ { re->text + n->offset, n->length }, v });
        if (r != SJS_OK) {
            sjs_unref(w->vm->context, sjs_heap_value(SJS_OBJECT, &o->h));
            return SJS_ERR_PROGRAM;
        }
        w->job->extra++;
    }
    *out = sjs_heap_value(SJS_OBJECT, &o->h);
    return SJS_OK;
}

/* An array of each group's part, the whole match first. */
static sjs_result rx_parts(rx_work *w, bool indices, sjs_value *out) {
    const sjs_regexp *re = w->job->s.re;
    uint32_t n = re->groups + 1u;
    sjs_array *a;
    sjs_result r = sjs_array_make(w->vm->context, n, &a);
    if (r != SJS_OK) return sjs_vm_fail(w->vm, r);
    for (uint32_t g = 0; g < n; g++) {
        sjs_value v;
        if (rx_group_value(w, (rx_group_part){ g, indices }, &v) != SJS_OK) {
            sjs_unref(w->vm->context, sjs_heap_value(SJS_ARRAY, &a->h));
            return SJS_ERR_PROGRAM;
        }
        a->items[a->length++] = v;
    }
    w->job->extra += n;
    *out = sjs_heap_value(SJS_ARRAY, &a->h);
    return SJS_OK;
}

/* Give an array an empty object of extra properties, with room for
 * `capacity`; releasing the array releases it. */
typedef struct {
    sjs_value array;
    uint32_t capacity;
} rx_extra_room;

static sjs_result rx_extra_new(sjs_vm *vm, rx_extra_room room) {
    sjs_object *o;
    sjs_result r = sjs_object_make(vm->context, room.capacity, &o);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    sjs_array *a = SJS_AS_ARRAY(room.array);
    a->h.flags |= SJS_ARRAY_EXTRA;
    a->extra = &o->h;
    return SJS_OK;
}

/* Put a property among an array's extra ones; takes over the value. */
typedef struct {
    sjs_value array;
    rx_property property;
} rx_extra_put;

static sjs_result rx_put_extra(sjs_vm *vm, rx_extra_put put) {
    sjs_array *a = SJS_AS_ARRAY(put.array);
    sjs_object *o = (sjs_object *)(void *)a->extra;
    sjs_result r = rx_put(vm, &o, put.property);
    a->extra = &o->h;
    return r;
}

/* indices with d: each group's [start, end], and groups of them. */
static sjs_result rx_indices(rx_work *w, sjs_value *out) {
    sjs_value indices;
    sjs_value groups;
    if (rx_parts(w, true, &indices) != SJS_OK) return SJS_ERR_PROGRAM;
    if (rx_extra_new(w->vm, (rx_extra_room){ indices, 1 }) != SJS_OK || rx_groups(w, true, &groups) != SJS_OK ||
        rx_put_extra(w->vm, (rx_extra_put){ indices, { SJS_STR("groups"), groups } }) != SJS_OK) {
        sjs_unref(w->vm->context, indices);
        return SJS_ERR_PROGRAM;
    }
    *out = indices;
    return SJS_OK;
}

/* The match result of the last search, as exec gives it. */
static sjs_result rx_result(rx_work *w, sjs_value *out) {
    rx_job *j = w->job;
    sjs_vm *vm = w->vm;
    bool d = (j->s.re->flags & SJS_REGEX_HAS_INDICES) != 0;
    sjs_value result;
    if (rx_parts(w, false, &result) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_value input = sjs_heap_value(SJS_STRING, (sjs_heap *)(uintptr_t)&j->input->h);
    sjs_value groups;
    sjs_value indices;
    sjs_result r = rx_extra_new(vm, (rx_extra_room){ result, d ? 4u : 3u });
    if (r == SJS_OK) r = rx_put_extra(vm, (rx_extra_put){ result, { SJS_STR("index"), sjs_num((double)rx_index(j, j->s.best[0])) } });
    if (r == SJS_OK) {
        sjs_ref(input);
        r = rx_put_extra(vm, (rx_extra_put){ result, { SJS_STR("input"), input } });
    }
    if (r == SJS_OK) r = rx_groups(w, false, &groups);
    if (r == SJS_OK) r = rx_put_extra(vm, (rx_extra_put){ result, { SJS_STR("groups"), groups } });
    if (r == SJS_OK && d) {
        r = rx_indices(w, &indices);
        if (r == SJS_OK) r = rx_put_extra(vm, (rx_extra_put){ result, { SJS_STR("indices"), indices } });
    }
    if (r != SJS_OK) {
        sjs_unref(vm->context, result);
        return SJS_ERR_PROGRAM;
    }
    *out = result;
    return SJS_OK;
}

/* The text being built (replace): appended in place to the string the
 * output holds alone. A half of a pair waits in job->high for its second. */
static sjs_result rx_build_text(rx_work *w, sjs_str text) {
    if (text.length == 0) return SJS_OK;
    if (w->job->high != 0) return rx_lone(w->vm);
    sjs_string *s = SJS_AS_STRING(*w->out);
    sjs_result r = sjs_string_reserve(w->vm->context, &s, text.length);
    w->out->as.heap = &s->h;
    if (r != SJS_OK) return sjs_vm_fail(w->vm, r);
    sjs_string_extend(s, text);
    return SJS_OK;
}

static sjs_result rx_build_unit(rx_work *w, uint32_t unit) {
    uint32_t point = unit;
    if (unit >= 0xD800u && unit <= 0xDBFFu) {
        if (w->job->high != 0) return rx_lone(w->vm);
        w->job->high = unit;
        return SJS_OK;
    }
    if (unit >= 0xDC00u && unit <= 0xDFFFu) {
        if (w->job->high == 0) return rx_lone(w->vm);
        point = 0x10000u + ((w->job->high - 0xD800u) << 10) + (unit - 0xDC00u);
        w->job->high = 0;
    }
    uint8_t bytes[4];
    uint32_t n = sjs_utf8_encode(point, bytes);
    return rx_build_text(w, (sjs_str){ (const char *)bytes, n });
}

/* The input from `a` to `b`, appended: halves of pairs at either end as
 * code units. */
static sjs_result rx_build_range(rx_work *w, uint32_t a, uint32_t b) {
    if (a >= b) return SJS_OK;
    /* By code units, with u too: a match of V8's between the halves of a
     * pair (rx_mid_start) ends or begins there. */
    rx_input units = w->job->s.in;
    units.unicode = false;
    const rx_input *in = &units;
    uint32_t next;
    if ((a & 1u) != 0) {
        if (rx_build_unit(w, rx_unit_at(in, a, &next)) != SJS_OK) return SJS_ERR_PROGRAM;
        a = next;
        if (a >= b) return SJS_OK;
    }
    uint32_t end = b & ~1u;
    if (end > a && rx_build_text(w, (sjs_str){ (const char *)in->text + (a >> 1), (end >> 1) - (a >> 1) }) != SJS_OK) {
        return SJS_ERR_PROGRAM;
    }
    if ((b & 1u) != 0) return rx_build_unit(w, rx_unit_at(in, end, &next));
    return SJS_OK;
}

/* A capture group's text, appended: nothing when it did not match. */
static sjs_result rx_build_group(rx_work *w, uint32_t group) {
    const uint32_t *best = w->job->s.best;
    if (best[2u * group] == RX_UNSET) return SJS_OK;
    return rx_build_range(w, best[2u * group], best[2u * group + 1u]);
}

/* The group named by `name`, or 0. */
static uint32_t rx_group_of(const sjs_regexp *re, sjs_str name) {
    for (uint32_t g = 0; g < re->groups; g++) {
        const rx_group_name *n = &re->names[g];
        if (n->length != 0 && sjs_str_equal((sjs_str){ re->text + n->offset, n->length }, name)) return g + 1u;
    }
    return 0;
}

/* GetSubstitution: the replacement template, with $$, $&, $`, $', $n, $nn
 * and $<name> standing for what the match gave. */
static sjs_result rx_substitute(rx_work *w) {
    sjs_str t = w->job->replacement;
    const sjs_regexp *re = w->job->s.re;
    const uint32_t *best = w->job->s.best;
    uint32_t end = (uint32_t)w->job->input->length << 1;
    uint32_t plain = 0;
    uint32_t i = 0;
    while (i + 1u < t.length) {
        char c = t.ptr[i + 1u];
        if (t.ptr[i] != '$') {
            i++;
            continue;
        }
        sjs_str before = { t.ptr + plain, i - plain };
        sjs_result r = SJS_OK;
        uint32_t used = 0;
        if (c == '$' || c == '&' || c == '`' || c == '\'') {
            r = rx_build_text(w, before);
            if (r == SJS_OK) {
                if (c == '$') r = rx_build_text(w, SJS_STR("$"));
                else if (c == '&') r = rx_build_range(w, best[0], best[1]);
                else if (c == '`') r = rx_build_range(w, 0, best[0]);
                else r = rx_build_range(w, best[1], end);
            }
            used = 2;
        } else if (c >= '0' && c <= '9') {
            uint32_t digits = 1;
            uint32_t index = (uint32_t)(c - '0');
            if (i + 2u < t.length && t.ptr[i + 2u] >= '0' && t.ptr[i + 2u] <= '9') {
                uint32_t two = index * 10u + (uint32_t)(t.ptr[i + 2u] - '0');
                if (two <= re->groups) {
                    digits = 2;
                    index = two;
                } else if (index == 0) {
                    /* $0x: index 0 with two digits is no group either. */
                    index = 0;
                }
            }
            if (index >= 1u && index <= re->groups) {
                r = rx_build_text(w, before);
                if (r == SJS_OK) r = rx_build_group(w, index);
                used = 1u + digits;
            }
        } else if (c == '<' && re->named) {
            uint32_t close = i + 2u;
            while (close < t.length && t.ptr[close] != '>') close++;
            if (close < t.length) {
                r = rx_build_text(w, before);
                uint32_t group = rx_group_of(re, (sjs_str){ t.ptr + i + 2u, close - i - 2u });
                if (r == SJS_OK && group != 0) r = rx_build_group(w, group);
                used = close + 1u - i;
            }
        }
        if (r != SJS_OK) return SJS_ERR_PROGRAM;
        if (used == 0) {
            i++;
            continue;
        }
        i += used;
        plain = i;
    }
    return rx_build_text(w, (sjs_str){ t.ptr + plain, t.length - plain });
}

/* The end of the text being built. */
static sjs_result rx_build_end(rx_work *w) {
    if (w->job->high != 0) return rx_lone(w->vm);
    w->job->extra += SJS_AS_STRING(*w->out)->length / 16u;
    return SJS_OK;
}

static uint8_t rx_finish(rx_work *w, sjs_value result) {
    sjs_vm_release(w->vm, *w->out);
    *w->out = result;
    w->job->done = true;
    return RX_DONE;
}

/* After a match's replacement: the rest of the input once, or the next
 * search with g. */
static uint8_t rx_replacement_done(rx_work *w) {
    rx_job *j = w->job;
    const uint32_t *best = j->s.best;
    uint32_t end = (uint32_t)j->input->length << 1;
    j->last = best[1];
    if (!j->global) {
        if (rx_build_range(w, j->last, end) != SJS_OK || rx_build_end(w) != SJS_OK) return RX_FAULT;
        j->done = true;
        return RX_DONE;
    }
    j->from = best[1] == best[0] ? rx_advance_from(j, best[1]) : best[1];
    if (j->from == RX_BEYOND) {
        if (rx_build_end(w) != SJS_OK) return RX_FAULT;
        j->done = true;
        return RX_DONE;
    }
    return RX_MORE;
}

/* The replacement function's result (taken over), as ToString gives it of a
 * primitive; an object or a BigInt faults (docs/regex.md, 8.5). */
static uint8_t rx_replaced(rx_work *w, sjs_value result) {
    char room[32];
    sjs_result r = SJS_OK;
    if (!sjs_is_primitive(result)) {
        r = sjs_vm_fault(w->vm, "staticjs turns only primitives into strings");
    } else if (result.kind == SJS_BIGINT) {
        r = sjs_vm_fault(w->vm, "staticjs turns a BigInt into a string only with String, toString, + or a template");
    } else {
        r = rx_build_text(w, sjs_primitive_text(result, room));
    }
    sjs_vm_release(w->vm, result);
    if (r != SJS_OK) return RX_FAULT;
    return rx_replacement_done(w);
}

/* What a method makes of a search that ended: RX_MORE to search again. */
static uint8_t rx_searched(rx_work *w) {
    rx_job *j = w->job;
    rx_search *s = &j->s;
    const uint32_t *best = s->best;
    uint32_t end = (uint32_t)j->input->length << 1;
    switch (j->op) {
    case RX_TEST:
        return rx_finish(w, sjs_boolean(s->matched));
    case RX_SEARCH:
        return rx_finish(w, sjs_num(s->matched ? (double)rx_index(j, best[0]) : -1.0));
    case RX_EXEC: {
        sjs_value result = sjs_null();
        if (s->matched && rx_result(w, &result) != SJS_OK) return RX_FAULT;
        return rx_finish(w, result);
    }
    case RX_MATCH_ALL: {
        if (!s->matched) {
            /* An iterator of the results, read once, as an iterator is. */
            sjs_value it;
            sjs_result r = sjs_iterator_make(w->vm->context, (sjs_iterator_spec){ sjs_undefined(), *w->out, SJS_ITERATE_VALUES }, &it);
            if (r != SJS_OK) {
                sjs_vm_fail(w->vm, r);
                return RX_FAULT;
            }
            *w->out = it;
            j->done = true;
            return RX_DONE;
        }
        sjs_value result;
        if (rx_result(w, &result) != SJS_OK || rx_push(w, result) != SJS_OK) return RX_FAULT;
        j->from = best[1] == best[0] ? rx_advance_from(j, best[1]) : best[1];
        return RX_MORE;
    }
    case RX_MATCH:
        if (!s->matched) {
            if (SJS_AS_ARRAY(*w->out)->length != 0) {
                j->done = true;
                return RX_DONE;
            }
            return rx_finish(w, sjs_null());
        }
        if (rx_push_piece(w, best[0], best[1]) != SJS_OK) return RX_FAULT;
        j->from = best[1] == best[0] ? rx_advance_from(j, best[1]) : best[1];
        return RX_MORE;
    case RX_SPLIT:
        /* A match found from `from` on (ECMA-262's @@split tries each
         * position with a sticky match; the leftmost match from `from` is
         * the first of those). One that begins at the end, or is empty
         * where the last piece ended, splits nothing. */
        if (!s->matched || best[0] >= end) {
            if (rx_push_piece(w, j->last, end) != SJS_OK) return RX_FAULT;
            j->done = true;
            return RX_DONE;
        }
        if (best[1] == j->last) {
            j->from = rx_advance_from(j, best[0]);
            if (j->from == RX_BEYOND || j->from >= end) {
                if (rx_push_piece(w, j->last, end) != SJS_OK) return RX_FAULT;
                j->done = true;
                return RX_DONE;
            }
            return RX_MORE;
        }
        if (rx_push_piece(w, j->last, best[0]) != SJS_OK) return RX_FAULT;
        if (++j->count == j->limit) {
            j->done = true;
            return RX_DONE;
        }
        for (uint32_t g = 1; g <= s->re->groups; g++) {
            sjs_result r = best[2u * g] == RX_UNSET ? rx_push(w, sjs_undefined()) : rx_push_piece(w, best[2u * g], best[2u * g + 1u]);
            if (r != SJS_OK) return RX_FAULT;
            if (++j->count == j->limit) {
                j->done = true;
                return RX_DONE;
            }
        }
        j->last = best[1];
        j->from = best[1];
        if (j->from >= end) {
            if (rx_push_piece(w, j->last, end) != SJS_OK) return RX_FAULT;
            j->done = true;
            return RX_DONE;
        }
        return RX_MORE;
    default: /* RX_REPLACE */
        if (!s->matched) {
            if (!j->global && j->last == 0) {
                sjs_value input = sjs_heap_value(SJS_STRING, (sjs_heap *)(uintptr_t)&j->input->h);
                sjs_ref(input);
                return rx_finish(w, input);
            }
            if (rx_build_range(w, j->last, end) != SJS_OK || rx_build_end(w) != SJS_OK) return RX_FAULT;
            j->done = true;
            return RX_DONE;
        }
        if (rx_build_range(w, j->last, best[0]) != SJS_OK) return RX_FAULT;
        if (j->callback) {
            /* The function's result comes back on the stack (rx_replaced). */
            j->awaiting = true;
            return RX_CALL;
        }
        if (rx_substitute(w) != SJS_OK) return RX_FAULT;
        return rx_replacement_done(w);
    }
}

/* One step: searches and what they give, until the work is done or it has
 * visited RX_STEP_VISITS instructions. */
static uint8_t rx_step(rx_work *w) {
    rx_job *j = w->job;
    rx_search *s = &j->s;
    uint64_t until = s->visits + RX_STEP_VISITS;
    uint8_t outcome = RX_MORE;
    while (!j->done && outcome == RX_MORE) {
        if (!s->running) {
            if (s->visits >= until) break;
            if (j->from == RX_BEYOND) {
                /* Past the end, the next search finds nothing. */
                s->matched = false;
                outcome = rx_searched(w);
                continue;
            }
            rx_search_begin(s, j->from);
        }
        if (!rx_search_run(s, until > s->visits ? until - s->visits : 0)) break;
        outcome = rx_searched(w);
    }
    rx_charge(w);
    if (outcome == RX_FAULT || outcome == RX_CALL) return outcome;
    return j->done ? RX_DONE : RX_MORE;
}

/* ------------------------------------------------------------------------
 * The methods: checked, then run whole as a leaf, or handed to a loop
 */

/* A method's operands, where its call has them. */
typedef struct {
    sjs_value regexp;
    sjs_value input;
    sjs_value second;
} rx_operands;

/* The receiver, and the method's arguments. */
typedef struct {
    uint8_t op;
    sjs_value receiver;
    sjs_values args;
} rx_call_site;

static rx_operands rx_operands_of(rx_call_site site) {
    sjs_value first = site.args.count > 0 ? site.args.ptr[0] : sjs_undefined();
    sjs_value second = site.args.count > 1 ? site.args.ptr[1] : sjs_undefined();
    if (site.op == RX_TEST || site.op == RX_EXEC) return (rx_operands){ site.receiver, first, second };
    return (rx_operands){ first, site.receiver, second };
}

/* Set up a job over `words` (zeroed, rx_job_words long) for the method `op`
 * on its operands; the output starts as what the method builds. */
typedef struct {
    uint8_t op;
    bool all;              /* replaceAll, which needs g */
    rx_operands operands;
} rx_method;

static sjs_result rx_job_start(rx_work *w, const rx_method *m, uint32_t *words) {
    rx_job *j = w->job;
    const sjs_regexp *re = SJS_AS_REGEXP(m->operands.regexp);
    const sjs_string *input = SJS_AS_STRING(m->operands.input);
    /* match without g is exec. */
    j->op = m->op == RX_MATCH && (re->flags & SJS_REGEX_GLOBAL) == 0 ? RX_EXEC : m->op;
    j->input = input;
    j->s.re = re;
    j->s.in = (rx_input){ (const uint8_t *)input->bytes, input->length, (input->h.flags & SJS_STRING_ASCII) != 0,
                          (re->flags & (SJS_REGEX_UNICODE | SJS_REGEX_UNICODE_SETS)) != 0 };
    j->s.fold_unicode = (re->flags & SJS_REGEX_IGNORE_CASE) != 0 && j->s.in.unicode;
    j->global = (re->flags & SJS_REGEX_GLOBAL) != 0 && (m->op == RX_MATCH || m->op == RX_REPLACE || m->op == RX_MATCH_ALL);
    j->s.sticky = (re->flags & SJS_REGEX_STICKY) != 0 && m->op != RX_SPLIT;
    /* split too: ECMA-262's @@split tries a sticky match at each code
     * point's start, but V8 searches as exec does (Node 22:
     * "a\u{1F600}".split(/\B/u) cuts the pair). */
    j->s.mid_starts = j->s.in.unicode;
    rx_search_layout(&j->s, words);
    sjs_result r = SJS_OK;
    if (j->op == RX_MATCH || j->op == RX_SPLIT || j->op == RX_MATCH_ALL) {
        sjs_array *a;
        r = sjs_array_make(w->vm->context, 4, &a);
        if (r == SJS_OK) *w->out = sjs_heap_value(SJS_ARRAY, &a->h);
    } else if (m->op == RX_REPLACE) {
        /* A function, called with each match; or a primitive's text, as
         * ToString gives it. */
        j->callback = m->operands.second.kind == SJS_FUNCTION;
        if (!j->callback) j->replacement = sjs_primitive_text(m->operands.second, j->room);
        sjs_string *s;
        r = sjs_string_make(w->vm->context, SJS_STR(""), &s);
        if (r == SJS_OK) {
            *w->out = sjs_heap_value(SJS_STRING, &s->h);
            r = sjs_string_reserve(w->vm->context, &s, input->length);
            w->out->as.heap = &s->h;
        }
    }
    if (r != SJS_OK) return sjs_vm_fail(w->vm, r);
    if (m->op == RX_SPLIT) {
        j->limit = UINT32_MAX;
        if (m->operands.second.kind == SJS_NUMBER) j->limit = sjs_number_to_uint32(m->operands.second.as.number);
        if (j->limit == 0) {
            j->done = true;
        } else if (input->length == 0) {
            /* An empty input splits into nothing when the whole pattern
             * matches it there, else into itself. */
            j->s.sticky = true;
        }
    }
    return SJS_OK;
}

/* The empty input of split: decided by a sticky search at 0. */
static uint8_t rx_split_empty(rx_work *w) {
    rx_job *j = w->job;
    if (j->done || j->input->length != 0 || j->op != RX_SPLIT) return RX_MORE;
    rx_search_begin(&j->s, 0);
    while (!rx_search_run(&j->s, UINT64_MAX)) {}
    rx_charge(w);
    if (!j->s.matched && rx_push_piece(w, 0, 0) != SJS_OK) return RX_FAULT;
    j->done = true;
    return RX_DONE;
}

/* The checks every call makes before it matches, so that the leaf and the
 * loop fault alike: false after a fault. */
static bool rx_method_check(sjs_vm *vm, const rx_method *m) {
    const rx_operands *o = &m->operands;
    if (o->regexp.kind != SJS_REGEXP) {
        sjs_vm_fault(vm, "staticjs matches only with a regular expression literal");
        return false;
    }
    if (o->input.kind != SJS_STRING) {
        sjs_vm_fault(vm, "staticjs matches a regular expression against a string only");
        return false;
    }
    uint32_t flags = SJS_AS_REGEXP(o->regexp)->flags;
    bool global = (flags & SJS_REGEX_GLOBAL) != 0;
    bool sticky = (flags & SJS_REGEX_STICKY) != 0;
    if ((m->op == RX_TEST || m->op == RX_EXEC) && (global || sticky)) {
        sjs_vm_fault(vm, "a regular expression with g or y changes its lastIndex when it matches, and staticjs values do not change");
        return false;
    }
    if ((m->op == RX_MATCH || m->op == RX_REPLACE) && sticky && !global) {
        sjs_vm_fault(vm, "a regular expression with y and without g changes its lastIndex when it matches, and staticjs values do not change");
        return false;
    }
    if ((m->all || m->op == RX_MATCH_ALL) && !global) {
        sjs_vm_fault(vm, "JavaScript throws a TypeError for a regular expression without g here");
        return false;
    }
    if (m->op == RX_SPLIT && o->second.kind != SJS_UNDEFINED && o->second.kind != SJS_NUMBER) {
        sjs_vm_fault(vm, "staticjs splits with a limit that is a number");
        return false;
    }
    if (m->op == RX_REPLACE && o->second.kind == SJS_FUNCTION && o->second.internal == SJS_INTERNAL_NONE) return true;
    if (m->op == RX_REPLACE && !sjs_is_primitive(o->second)) {
        sjs_vm_fault(vm, "staticjs turns only primitives into strings");
        return false;
    }
    if (m->op == RX_REPLACE && o->second.kind == SJS_BIGINT) {
        sjs_vm_fault(vm, "staticjs turns a BigInt into a string only with String, toString, + or a template");
        return false;
    }
    return true;
}

/* Work a leaf may do whole: instructions visited, about. */
#define RX_LEAF_WORK 8192u

sjs_result sjs_regex_call(sjs_vm *vm, sjs_regex_call_params *params) {
    sjs_leaf_call *call = params->call;
    rx_method m = { params->op, params->all, rx_operands_of((rx_call_site){ params->op, call->receiver, { call->args, call->count } }) };
    if (!rx_method_check(vm, &m)) return SJS_ERR_PROGRAM;
    const sjs_regexp *re = SJS_AS_REGEXP(m.operands.regexp);
    uint64_t units = (uint64_t)SJS_AS_STRING(m.operands.input)->utf16_length + 1u;
    bool repeats = ((re->flags & SJS_REGEX_GLOBAL) != 0 && (m.op == RX_MATCH || m.op == RX_REPLACE || m.op == RX_MATCH_ALL)) ||
                   m.op == RX_SPLIT;
    uint64_t work = units * re->threads * 2u * (repeats ? units : 1u);
    uint64_t words = rx_job_words(re);
    /* A replacement function is called back, which only a loop can. */
    bool callback = m.op == RX_REPLACE && m.operands.second.kind == SJS_FUNCTION;
    if (vm->no_regex_leaf || callback || work > RX_LEAF_WORK || words > SJS_REGEX_LEAF_WORDS) {
        call->loop = (uint16_t)(SJS_REGEX_LOOP_FIRST + m.op);
        return SJS_LEAF_LOOP;
    }
    uint32_t *block = (uint32_t *)(void *)vm->regex_leaf;
    sjs_zero(block, (size_t)words * 4u);
    rx_job *j = (rx_job *)(void *)block;
    rx_work w = { vm, j, &call->result };
    if (rx_job_start(&w, &m, block + (sizeof(rx_job) + 3u) / 4u) != SJS_OK) return SJS_ERR_PROGRAM;
    /* The steps a loop would take, each a unit of fuel, as a loop's are. */
    uint64_t steps = 1;
    uint8_t outcome = rx_split_empty(&w);
    while (outcome == RX_MORE) {
        outcome = rx_step(&w);
        if (outcome == RX_MORE) steps++;
    }
    rx_charge(&w);
    if (outcome == RX_FAULT) {
        /* As the loop's steps up to the fault would have cost. */
        vm->fuel_used += steps;
        return SJS_ERR_PROGRAM;
    }
    call->cost += steps;
    return SJS_OK;
}

/* The loop's state: a block of values' size, as sjs_loop_free frees it. */
sjs_result sjs_regex_loop_start(sjs_vm *vm, vm_frame *frame) {
    sjs_value *slots = &vm->stack[frame->base];
    rx_method m = { (uint8_t)(frame->builtin - SJS_REGEX_LOOP_FIRST), false,
                    rx_operands_of((rx_call_site){ (uint8_t)(frame->builtin - SJS_REGEX_LOOP_FIRST), slots[LOOP_RECEIVER],
                                                   { &slots[LOOP_CALLBACK], frame->argc } }) };
    uint64_t bytes = rx_job_words(SJS_AS_REGEXP(m.operands.regexp)) * 4u;
    uint64_t values = (bytes + sizeof(sjs_value) - 1u) / sizeof(sjs_value);
    if (values * sizeof(sjs_value) > UINT32_MAX) return sjs_vm_fail(vm, SJS_ERR_LIMIT);
    frame->scratch = sjs_mem_alloc(&vm->context->counting, (size_t)values * sizeof(sjs_value));
    if (frame->scratch == NULL) return sjs_vm_fail(vm, SJS_ERR_NOMEM);
    frame->scratch_count = (uint32_t)values;
    frame->phase = 0;
    sjs_zero(frame->scratch, (size_t)values * sizeof(sjs_value));
    uint32_t *block = (uint32_t *)(void *)frame->scratch;
    rx_work w = { vm, (rx_job *)(void *)block, &slots[LOOP_OUTPUT] };
    if (rx_job_start(&w, &m, block + (sizeof(rx_job) + 3u) / 4u) != SJS_OK) return SJS_ERR_PROGRAM;
    if (w.job->callback) {
        /* The function where a loop's callback is (sjs_vm_call_back), the
         * regular expression, which the job reads, beside it. */
        sjs_value swap = slots[LOOP_CALLBACK];
        slots[LOOP_CALLBACK] = slots[LOOP_SECOND];
        slots[LOOP_SECOND] = swap;
    }
    return SJS_OK;
}

/* Call the replacement function with the match: its text, each group's
 * (undefined for one that did not match), its index, the input, and the
 * groups object when a group has a name. */
static loop_step rx_call_back(rx_work *w, vm_frame *frame) {
    rx_job *j = w->job;
    sjs_value args;
    if (rx_parts(w, false, &args) != SJS_OK) return LOOP_FAULT;
    sjs_array *a = SJS_AS_ARRAY(args);
    sjs_result r = sjs_array_grow(w->vm->context, &a, 3);
    args.as.heap = &a->h;
    sjs_value groups = sjs_undefined();
    if (r == SJS_OK && j->s.re->named) r = rx_groups(w, false, &groups);
    if (r != SJS_OK) {
        sjs_unref(w->vm->context, args);
        if (w->vm->status != VM_FAULT) sjs_vm_fail(w->vm, r);
        return LOOP_FAULT;
    }
    a->items[a->length++] = sjs_num((double)rx_index(j, j->s.best[0]));
    a->items[a->length++] = sjs_heap_value(SJS_STRING, (sjs_heap *)(uintptr_t)&j->input->h);
    sjs_ref(a->items[a->length - 1u]);
    if (j->s.re->named) a->items[a->length++] = groups;
    bool called = sjs_vm_call_back(w->vm, frame, (sjs_values){ a->items, a->length });
    sjs_unref(w->vm->context, args);
    return called ? LOOP_CALL : LOOP_FAULT;
}

loop_step sjs_regex_loop_step(sjs_vm *vm, vm_frame *frame) {
    rx_work w = { vm, (rx_job *)(void *)frame->scratch, &vm->stack[frame->base + LOOP_OUTPUT] };
    /* frame->phase, index and length stay 0, so that sjs_loop_step hands
     * the frame here from its end (builtins.c). */
    uint8_t outcome = w.job->started ? RX_MORE : rx_split_empty(&w);
    w.job->started = true;
    if (w.job->awaiting) {
        /* The replacement function returned: its result is on top. */
        w.job->awaiting = false;
        outcome = rx_replaced(&w, sjs_vm_pop(vm));
    }
    if (outcome == RX_MORE) outcome = rx_step(&w);
    rx_charge(&w);
    if (outcome == RX_CALL) return rx_call_back(&w, frame);
    if (outcome == RX_FAULT) return LOOP_FAULT;
    return outcome == RX_DONE ? LOOP_DONE : LOOP_AGAIN;
}
