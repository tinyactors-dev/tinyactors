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
    bool cached;              /* a test's loop: the cache of its steps follows the search's words ("The cache") */
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
    uint32_t pending;         /* an edge of the cache whose step is taken and whose start is not, or 0 (edge 0 is none) */
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

/* Inline: rx_search_run's step calls it for each thread at each position,
 * and it has another caller (rx_advance). */
__attribute__((always_inline)) static inline bool rx_consumes(const rx_search *s, const rx_ins *ins, uint32_t c) {
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

/*
 * Skipping. Where a search has no thread and no match, a program led by an
 * ASCII character (RX_LEAD_CHAR, RX_LEAD_FOLD) starts a thread at each
 * position that its closure takes over the leading SAVEs to the character
 * and that dies there unless the input has that character; one led by ^
 * (RX_LEAD_START), a thread that dies at the ^ at every position but the
 * first, whether or not other threads are alive. Over a run of ASCII bytes
 * that cannot begin a match, the search passes each position without
 * following it, and counts what following it would have visited and
 * copied, so that fuel and the budget's stopping points are the same
 * (docs/regex.md, 12.1).
 */

/* Positions to pass, and what passing each costs. */
typedef struct {
    uint32_t positions;
    uint64_t visits;
    uint64_t copies;
} rx_cost;

/* The positions rx_search_run passes before its budget stops it, when each
 * visits `per` instructions: it checks after each one. */
static uint64_t rx_budget_positions(const rx_search *s, uint64_t per, uint64_t until) {
    if (until <= s->visits) return 1;
    return (until - s->visits + per - 1u) / per;
}

/* The ASCII bytes from `byte` on, at most `most`, that cannot begin a
 * match: any but the leading character (either case of it with i), and
 * after the start with ^ any. */
static uint32_t rx_lead_run(const rx_search *s, uint32_t byte, uint64_t most) {
    const uint8_t *t = s->in.text;
    uint32_t end = s->in.length - byte < most ? s->in.length : byte + (uint32_t)most;
    uint32_t lead = s->re->lead == RX_LEAD_START ? 0x100u : s->re->lead_char;
    uint32_t fold = s->re->lead == RX_LEAD_FOLD ? 0x20u : 0u;
    uint32_t i = byte;
    while (i < end && t[i] < 0x80u && (t[i] | fold) != lead) i++;
    return i - byte;
}

/* Pass positions, each an ASCII byte: true when the budget stops the
 * search there. */
static bool rx_pass(rx_search *s, const rx_cost *pass, uint64_t until) {
    s->visits += pass->positions * pass->visits;
    s->copies += pass->positions * pass->copies;
    uint32_t byte = (s->pos >> 1) + pass->positions;
    s->prev = s->in.text[byte - 1u];
    s->pos = byte << 1;
    s->unit = rx_unit_at(&s->in, s->pos, &s->next);
    s->generation += 2u;
    return s->visits >= until;
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
    s->pending = 0;
}

/*
 * The automaton's start thread and step, as rx_search_run takes them at a
 * position, for the cache's learning (rx_dfa_learn). rx_search_run keeps
 * its own copy of both inline: its code generation needs it (helpers there,
 * called or inlined, cost the methods that do not use the cache 1 to 9%
 * more instructions). spec_test, regex_test and api_test compare a test
 * with the cache and without on every input they run, fuel and pauses
 * included, so the copies cannot drift apart unseen.
 */

/* A thread from the start at s->pos, after every thread from earlier; led
 * by ^, past the first position, only its count: it dies at the ^, after
 * the SAVEs before it, which only the start reaches. Where no match is
 * found yet (and a sticky search only at its start). */
static void rx_start_thread(rx_search *s) {
    uint32_t n = s->re->slots;
    if (s->re->lead == RX_LEAD_START && s->pos > 0) {
        s->copies += n;
        s->visits += s->re->lead_saves + 1u;
        return;
    }
    for (uint32_t k = 0; k < n; k++) s->cur[k] = RX_UNSET;
    s->copies += n;
    s->at_pos = s->pos;
    s->at_prev = s->prev;
    s->at_unit = s->unit;
    rx_threads now = { s->cpc, s->cslots, &s->ccount };
    rx_closure(s, &now, 0);
}

/* The step over the character at s->pos: each thread consumes it, or
 * matches and ends the threads after it; the threads it gives are the list
 * at the next position, where the search then is. False at the end of the
 * input, where the search ends. */
static bool rx_advance(rx_search *s) {
    uint32_t n = s->re->slots;
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
    if (s->unit == RX_NONE) return false;
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
    return true;
}

/* Whether rx_search_run's skip passes the position the search is at. */
static bool rx_skips(const rx_search *s) {
    uint8_t lead = s->re->lead;
    if (lead == RX_LEAD_NONE || s->ccount != 0 || s->matched || s->sticky || (lead == RX_LEAD_START && s->pos == 0)) return false;
    return rx_lead_run(s, s->pos >> 1, 1) > 0;
}

/*
 * The cache. A test() over an ASCII input, run as a loop, needs of its
 * threads only their program counters: the result is whether any thread
 * matched, and the slots bear on what a thread does only through CHECK and
 * the registers' mask, which compare a register with the position, and a
 * register is set to the position only within the position's closure. So
 * a list of threads (after its position's start thread), as program
 * counters, and whether a match is found, is a state of a deterministic
 * automaton, and the step over a character (rx_advance) with the next
 * position's start thread (rx_start_thread) its edge. What an edge does
 * depends on the state, the character's class and the next character's
 * (or the end): every instruction that consumes, every assertion and the
 * skip treat the characters of a class alike (sjs_regex_dfa_classes,
 * sorted once when the pattern compiles: rx_dfa_map). Of the next
 * character only the assertions and the skip ask, so its class is a
 * coarser one, of those predicates alone (dfa_next): a row of edges is
 * the classes by the coarser ones and the end. An edge is learned
 * by running the automaton itself once, and keeps what it visited
 * and copied in the step and in the start, so a search that takes it counts
 * the same work, and fuel, as the automaton (docs/regex.md, 9.1), and stops
 * for its budget at the same place: after the step, where the start
 * remains to take when the search goes on (s->pending). An edge to a
 * position where the search skips (no thread, no match, and a character
 * that cannot begin one) hands the search back at that position.
 *
 * The cache lives in the loop's own block, after the job, in a fixed
 * budget (RX_DFA_BYTES); a pattern with more classes than RX_DFA_CLASSES
 * has none, and a search that fills it goes on by the automaton alone.
 */

#define RX_DFA_BYTES 8192u
#define RX_DFA_CLASSES 32u
#define RX_DFA_BUCKETS 256u
/* The most instructions a pattern has for the cache, and distinct ones
 * among them that consume: sorting the characters into classes reads each
 * of the instructions, and asks each distinct one of every character. */
#define RX_DFA_CODE 256u
#define RX_DFA_CONSUMERS 32u
/* The state an edge goes to where the search skips the position after: it
 * stops a run of the cache, as does a state with no thread and a match. */
#define RX_DFA_SKIP 0u
#define RX_DFA_STOPS UINT32_MAX

typedef struct {
    uint32_t pcs;             /* its threads' program counters, from here in the pool */
    uint32_t count;
    uint32_t row;             /* its edges by class pair, from here in the rows */
    uint32_t hash;
    bool matched;
    bool stops;               /* the search ends here (no thread, a match), or skips */
} rx_dfa_state;

typedef struct {
    uint32_t target;          /* a state, or RX_DFA_SKIP */
    uint32_t row;             /* the target's row, or RX_DFA_STOPS when it stops */
    uint32_t step_visits;     /* in the step */
    uint32_t step_copies;
    uint32_t visits;          /* in the step and the start */
    uint32_t copies;
} rx_dfa_edge;

typedef struct {
    uint32_t k;               /* classes (rx_dfa_map); 0 before set up */
    uint32_t next;            /* the coarser classes of the character after (dfa_next); the end is one more */
    uint32_t states;
    uint32_t state_cap;
    uint32_t edges;
    uint32_t edge_cap;
    uint32_t pcs;
    uint32_t pc_cap;
    uint16_t buckets[RX_DFA_BUCKETS];  /* states by hash, plus one */
    rx_dfa_state *state;
    uint32_t *pc;
    uint16_t *row;            /* an edge, or 0 for one not learned (edge 0 is none) */
    rx_dfa_edge *edge;
} rx_dfa;

#define RX_DFA_EDGES 128u
#define RX_DFA_PCS 256u

/* The cache of a search that has one: after the search's words, on a
 * boundary of 16 bytes (sjs_regex_loop_start). */
static rx_dfa *rx_dfa_of(const rx_search *s) {
    uintptr_t end = (uintptr_t)(s->best + s->re->slots);
    return (rx_dfa *)(void *)((end + 15u) & ~(uintptr_t)15u);
}

/* Add the characters from `low` to `high`, those below 128, to a set of
 * 128 bits. */
static void rx_dfa_add(uint64_t *set, uint32_t low, uint32_t high) {
    for (uint32_t half = 0; half < 2u; half++) {
        uint32_t from = low > 64u * half ? low : 64u * half;
        uint32_t to = high < 64u * half + 63u ? high : 64u * half + 63u;
        if (from > to) continue;
        uint32_t width = to - from + 1u;
        uint64_t bits = width == 64u ? UINT64_MAX : (((uint64_t)1 << width) - 1u);
        set[half] |= bits << (from - 64u * half);
    }
}

/* The ASCII characters a predicate of sjs_regex_dfa_classes holds for, as
 * a set of 128 bits: the instruction `which` consumes them, or past the
 * program: they are word characters, line terminators, or (led by a
 * character) not ones that can begin a match. What rx_consumes, rx_word,
 * rx_line_terminator and rx_skips say of them; a search's fold_unicode
 * bears only on characters past ASCII. */
static void rx_dfa_holds(const sjs_regexp *re, uint32_t which, uint64_t *set) {
    set[0] = 0;
    set[1] = 0;
    if (which < re->count) {
        uint32_t x = re->code[which].x;
        switch (re->code[which].op) {
        case RX_CHAR: rx_dfa_add(set, x, x); break;
        case RX_CHAR_FOLD:
            /* rx_fold: A to Z to a to z, the others to themselves. */
            if (!(x >= 'A' && x <= 'Z')) rx_dfa_add(set, x, x);
            if (x >= 'a' && x <= 'z') rx_dfa_add(set, x - 0x20u, x - 0x20u);
            break;
        case RX_ANY:
            rx_dfa_add(set, 0, 0x09u);
            rx_dfa_add(set, 0x0Bu, 0x0Cu);
            rx_dfa_add(set, 0x0Eu, 0x7Fu);
            break;
        case RX_ANY_ALL: rx_dfa_add(set, 0, 0x7Fu); break;
        case RX_CLASS: {
            const rx_class *k = &re->classes[x];
            const uint32_t *r = re->ranges + 2u * k->start;
            for (uint32_t i = 0; i < k->count && r[2u * i] < 0x80u; i++) rx_dfa_add(set, r[2u * i], r[2u * i + 1u]);
            break;
        }
        default: break;
        }
    } else if (which == re->count) {
        rx_dfa_add(set, '0', '9');
        rx_dfa_add(set, 'A', 'Z');
        rx_dfa_add(set, '_', '_');
        rx_dfa_add(set, 'a', 'z');
    } else if (which == re->count + 1u) {
        rx_dfa_add(set, 0x0Au, 0x0Au);
        rx_dfa_add(set, 0x0Du, 0x0Du);
    } else if (re->lead == RX_LEAD_CHAR || re->lead == RX_LEAD_FOLD) {
        /* All but those whose (c | fold) is the leading character: it,
         * and with fold, it without 0x20. */
        uint32_t fold = re->lead == RX_LEAD_FOLD ? 0x20u : 0u;
        uint32_t lead = re->lead_char;
        set[0] = UINT64_MAX;
        set[1] = UINT64_MAX;
        uint32_t begins[2] = { lead, lead & ~fold };
        for (uint32_t i = 0; i < 2u; i++) {
            if ((begins[i] | fold) == lead) set[begins[i] >> 6] &= ~((uint64_t)1 << (begins[i] & 63u));
        }
    }
}

/* Split each of the `*k` classes in `sets` (128 bits each) in two where
 * `holds` holds for some of its characters and not for the rest: false
 * when that would make more than RX_DFA_CLASSES. */
static bool rx_dfa_split(uint64_t *sets, uint32_t *k, const uint64_t *holds) {
    uint32_t before = *k;
    for (uint32_t i = 0; i < before; i++) {
        uint64_t in[2] = { sets[2u * i] & holds[0], sets[2u * i + 1u] & holds[1] };
        uint64_t out[2] = { sets[2u * i] & ~holds[0], sets[2u * i + 1u] & ~holds[1] };
        if ((in[0] | in[1]) == 0 || (out[0] | out[1]) == 0) continue;
        if (*k == RX_DFA_CLASSES) return false;
        sets[2u * i] = in[0];
        sets[2u * i + 1u] = in[1];
        sets[2u * *k] = out[0];
        sets[2u * *k + 1u] = out[1];
        (*k)++;
    }
    return true;
}

/* Number the `k` classes in `sets` by the order of their first characters
 * (the classes whose first characters come before it), each character's
 * in `classes`. */
static void rx_dfa_number(const uint64_t *sets, uint32_t k, uint8_t *classes) {
    uint32_t first[RX_DFA_CLASSES];
    for (uint32_t i = 0; i < k; i++) {
        first[i] = sets[2u * i] != 0 ? (uint32_t)__builtin_ctzll(sets[2u * i]) : 64u + (uint32_t)__builtin_ctzll(sets[2u * i + 1u]);
    }
    for (uint32_t i = 0; i < k; i++) {
        uint8_t rank = 0;
        for (uint32_t m = 0; m < k; m++) rank = (uint8_t)(rank + (first[m] < first[i] ? 1u : 0u));
        for (uint32_t half = 0; half < 2u; half++) {
            uint64_t bits = sets[2u * i + half];
            while (bits != 0) {
                classes[64u * half + (uint32_t)__builtin_ctzll(bits)] = rank;
                bits &= bits - 1u;
            }
        }
    }
}

/* Sort the ASCII characters into classes that every predicate treats
 * alike, for the program `re` (whose code, classes, ranges and lead are
 * set), once when it compiles, into its dfa_classes, with each
 * character's class in `map`; and into the coarser classes that the
 * predicates past the program alone treat alike (word characters, line
 * terminators, the skip's), which are all that bear on the character after
 * an edge's (rx_dfa_run), into its dfa_next, with each character's in
 * `map` + 128. Classes are numbered in the order of their first
 * characters. dfa_classes is 0 when the program has more than RX_DFA_CODE
 * instructions, more distinct instructions that consume than
 * RX_DFA_CONSUMERS, or more classes than RX_DFA_CLASSES. An instruction
 * like one before it splits nothing more, so each distinct one is read
 * once. Each class is a set of 128 bits, which each predicate splits in
 * two where it holds for some of the class and not for the rest; the
 * classes after all the predicates do not depend on their order, so the
 * coarser ones come first. */
void sjs_regex_dfa_classes(sjs_regexp *re, uint16_t *map) {
    re->dfa_classes = 0;
    re->dfa_next = 0;
    if (re->count > RX_DFA_CODE) return;
    uint64_t sets[2u * RX_DFA_CLASSES];
    sets[0] = UINT64_MAX;
    sets[1] = UINT64_MAX;
    uint32_t k = 1;
    uint64_t holds[2];
    for (uint32_t which = re->count; which < re->count + 3u; which++) {
        rx_dfa_holds(re, which, holds);
        if (!rx_dfa_split(sets, &k, holds)) return;
    }
    uint32_t next = k;
    uint8_t coarse[RX_DFA_ASCII];
    rx_dfa_number(sets, k, coarse);
    uint32_t seen[2u * RX_DFA_CONSUMERS];
    uint32_t seen_count = 0;
    for (uint32_t which = 0; which < re->count; which++) {
        const rx_ins *ins = &re->code[which];
        if (ins->op > RX_CLASS) continue;
        uint32_t i = 0;
        while (i < seen_count && (seen[2u * i] != ins->op || seen[2u * i + 1u] != ins->x)) i++;
        if (i < seen_count) continue;
        if (seen_count == RX_DFA_CONSUMERS) return;
        seen[2u * seen_count] = ins->op;
        seen[2u * seen_count + 1u] = ins->x;
        seen_count++;
        rx_dfa_holds(re, which, holds);
        if (!rx_dfa_split(sets, &k, holds)) return;
    }
    uint8_t fine[RX_DFA_ASCII];
    rx_dfa_number(sets, k, fine);
    for (uint32_t c = 0; c < RX_DFA_ASCII; c++) map[c] = (uint16_t)((fine[c] * (next + 1u)) << RX_DFA_NEXT_BITS | coarse[c]);
    re->dfa_classes = (uint8_t)k;
    re->dfa_next = (uint8_t)next;
}

/* Set the cache up when the search first runs from it: its arrays over
 * RX_DFA_BYTES, for the pattern's classes (rx_dfa_map). */
static void rx_dfa_setup(rx_search *s) {
    rx_dfa *d = rx_dfa_of(s);
    uint32_t k = s->re->dfa_classes;
    uint8_t *at = (uint8_t *)(void *)d + ((sizeof(rx_dfa) + 15u) & ~(size_t)15u);
    uint8_t *end = (uint8_t *)(void *)d + RX_DFA_BYTES;
    d->k = k;
    d->next = s->re->dfa_next;
    d->states = 1;
    d->edges = 1;
    d->pcs = 0;
    for (uint32_t i = 0; i < RX_DFA_BUCKETS; i++) d->buckets[i] = 0;
    d->edge = (rx_dfa_edge *)(void *)at;
    d->edge_cap = RX_DFA_EDGES + 1u;
    at += (RX_DFA_EDGES + 1u) * sizeof(rx_dfa_edge);
    d->pc = (uint32_t *)(void *)at;
    d->pc_cap = RX_DFA_PCS;
    at += RX_DFA_PCS * sizeof(uint32_t);
    /* Each state, and its row of edges: k classes by the next + 1. */
    size_t row = (size_t)k * (d->next + 1u) * sizeof(uint16_t);
    size_t each = sizeof(rx_dfa_state) + row;
    uint32_t cap = (uint32_t)((size_t)(end - at) / each);
    d->state_cap = cap < 0xFFFFu ? cap : 0xFFFEu;
    d->state = (rx_dfa_state *)(void *)at;
    at += (size_t)d->state_cap * sizeof(rx_dfa_state);
    d->row = (uint16_t *)(void *)at;
    d->state[RX_DFA_SKIP] = (rx_dfa_state){ 0, 0, 0, 0, false, true };
}

static uint32_t rx_dfa_hash(const uint32_t *pcs, uint32_t count, bool matched) {
    uint32_t h = matched ? 0x9E3779B9u : 0x811C9DC5u;
    for (uint32_t i = 0; i < count; i++) h = (h ^ pcs[i]) * 0x01000193u;
    return h;
}

/* The state of the search's list of threads, made when new: false when
 * the cache has no room for it. */
static bool rx_dfa_intern(rx_search *s, uint32_t *out) {
    rx_dfa *d = rx_dfa_of(s);
    uint32_t count = s->ccount;
    uint32_t hash = rx_dfa_hash(s->cpc, count, s->matched);
    uint32_t b = hash & (RX_DFA_BUCKETS - 1u);
    for (uint32_t probe = 0; probe < RX_DFA_BUCKETS; probe++) {
        uint32_t at = d->buckets[b];
        if (at == 0) break;
        const rx_dfa_state *st = &d->state[at - 1u];
        if (st->hash == hash && st->count == count && st->matched == s->matched) {
            uint32_t i = 0;
            while (i < count && d->pc[st->pcs + i] == s->cpc[i]) i++;
            if (i == count) {
                *out = at - 1u;
                return true;
            }
        }
        b = (b + 1u) & (RX_DFA_BUCKETS - 1u);
    }
    if (d->buckets[b] != 0 || d->states == d->state_cap || count > d->pc_cap - d->pcs) return false;
    uint32_t row = d->k * (d->next + 1u);
    rx_dfa_state *st = &d->state[d->states];
    st->pcs = d->pcs;
    st->count = count;
    st->row = d->states * row;
    st->hash = hash;
    st->matched = s->matched;
    st->stops = count == 0 && s->matched;
    for (uint32_t i = 0; i < count; i++) d->pc[d->pcs + i] = s->cpc[i];
    d->pcs += count;
    for (uint32_t i = 0; i < row; i++) d->row[st->row + i] = 0;
    *out = d->states++;
    d->buckets[b] = (uint16_t)(*out + 1u);
    return true;
}

/* Put the search at byte `byte` in the state `id`, its threads' slots
 * unset (only the program counters bear on a test's result). */
static void rx_dfa_load(rx_search *s, uint32_t id, uint32_t byte) {
    const rx_dfa *d = rx_dfa_of(s);
    const rx_dfa_state *st = &d->state[id];
    uint32_t n = s->re->slots;
    for (uint32_t i = 0; i < st->count; i++) {
        s->cpc[i] = d->pc[st->pcs + i];
        for (uint32_t k = 0; k < n; k++) s->cslots[(size_t)i * n + k] = RX_UNSET;
    }
    s->ccount = st->count;
    s->matched = st->matched;
    s->pos = byte << 1;
    s->prev = s->in.text[byte - 1u];
    s->unit = rx_unit_at(&s->in, s->pos, &s->next);
}

/* Where a run of the cache leaves the search: at a position after its
 * start thread, to go on by the automaton; at the top of a position, where
 * it skips; or stopped by its budget. */
enum { RX_DFA_LISTED = 0, RX_DFA_TOP, RX_DFA_PAUSE };

/* Learn the edge of the search's state `*id` for the class pair at
 * `entry` of its row by taking the step and the start by the automaton:
 * RX_DFA_PAUSE when the budget stops the search after the step, as it would
 * stop it; RX_DFA_TOP when the search skips the position after;
 * RX_DFA_LISTED with `*id` the state after, or with the cache given up. */
typedef struct {
    uint32_t id;
    uint32_t entry;
} rx_dfa_at;

static uint8_t rx_dfa_learn(rx_search *s, rx_dfa_at *at, uint64_t until) {
    rx_dfa *d = rx_dfa_of(s);
    uint64_t visits = s->visits;
    uint64_t copies = s->copies;
    (void)rx_advance(s);
    uint64_t step_visits = s->visits - visits;
    uint64_t step_copies = s->copies - copies;
    if (s->visits >= until) return RX_DFA_PAUSE;
    bool skips = rx_skips(s);
    if (!skips && !s->matched) rx_start_thread(s);
    uint64_t all_visits = s->visits - visits;
    uint64_t all_copies = s->copies - copies;
    uint32_t target = RX_DFA_SKIP;
    bool fits = d->edges < d->edge_cap && all_visits <= UINT32_MAX && all_copies <= UINT32_MAX;
    if (fits && !skips) fits = rx_dfa_intern(s, &target);
    if (!fits) {
        s->cached = false;
        return skips ? RX_DFA_TOP : RX_DFA_LISTED;
    }
    const rx_dfa_state *to = &d->state[target];
    d->edge[d->edges] = (rx_dfa_edge){ target, to->stops ? RX_DFA_STOPS : to->row, (uint32_t)step_visits, (uint32_t)step_copies,
                                       (uint32_t)all_visits, (uint32_t)all_copies };
    d->row[d->state[at->id].row + at->entry] = (uint16_t)d->edges++;
    at->id = target;
    return skips ? RX_DFA_TOP : RX_DFA_LISTED;
}

/* Run the search by the cache, from the state the budget stopped it after
 * the step to (s->pending), or else from its list of threads after its
 * start thread, until it ends, its budget stops it (after a step:
 * s->pending), it skips, or an edge is not learned and cannot be. Apart
 * from rx_search_run, which the other methods run. */
__attribute__((noinline)) static uint8_t rx_dfa_run(rx_search *s, uint64_t until) {
    uint32_t id;
    /* Whether the search's own list holds the state. */
    bool holds = s->pending == 0;
    if (!holds) {
        /* The start the step left, then on from the edge's target. */
        const rx_dfa_edge *edge = &rx_dfa_of(s)->edge[s->pending];
        s->pending = 0;
        s->visits += edge->visits - edge->step_visits;
        s->copies += edge->copies - edge->step_copies;
        if (edge->target == RX_DFA_SKIP) {
            s->matched = false;
            return RX_DFA_TOP;
        }
        id = edge->target;
    } else {
        /* From the search's list: not at the end, where nothing is left to
         * run from it; the cache set up when first run from. */
        if ((s->pos >> 1) >= s->in.length) return RX_DFA_LISTED;
        if (rx_dfa_of(s)->k == 0) rx_dfa_setup(s);
        if (!rx_dfa_intern(s, &id)) {
            s->cached = false;
            return RX_DFA_LISTED;
        }
    }
    const rx_dfa *d = rx_dfa_of(s);
    const uint16_t *classes = rx_dfa_map(s->re);
    const uint32_t mask = (1u << RX_DFA_NEXT_BITS) - 1u;
    const uint8_t *text = s->in.text;
    uint32_t length = s->in.length;
    uint32_t byte = s->pos >> 1;
    uint32_t from = byte;
    uint64_t visits = s->visits;
    uint64_t copies = s->copies;
    uint32_t pending = 0;
    /* The class of the character at `byte`, times the width of a row. */
    uint32_t here = byte < length ? (uint32_t)classes[text[byte]] >> RX_DFA_NEXT_BITS : 0;
    for (;;) {
        const rx_dfa_state *st = &d->state[id];
        if (st->stops || byte >= length) break;
        /* The edges learned over characters with one after them, while the
         * budget lasts and no state stops; what is left, below. */
        uint32_t row = st->row;
        while (byte + 1u < length) {
            uint32_t c = classes[text[byte + 1u]];
            uint32_t e = d->row[row + here + (c & mask)];
            if (e == 0) break;
            const rx_dfa_edge *edge = &d->edge[e];
            if (visits + edge->step_visits >= until) break;
            visits += edge->visits;
            copies += edge->copies;
            byte++;
            holds = false;
            here = c >> RX_DFA_NEXT_BITS;
            id = edge->target;
            row = edge->row;
            if (row == RX_DFA_STOPS) break;
        }
        st = &d->state[id];
        if (st->stops || byte >= length) break;
        uint32_t c = byte + 1u < length ? classes[text[byte + 1u]] : 0;
        uint32_t after = byte + 1u < length ? (c & mask) : d->next;
        /* The class of the character after, times the width of a row. */
        uint32_t there = c >> RX_DFA_NEXT_BITS;
        uint32_t e = d->row[st->row + here + after];
        if (e == 0) {
            if (!holds) rx_dfa_load(s, id, byte);
            s->generation += 2u * (byte - from);
            s->visits = visits;
            s->copies = copies;
            rx_dfa_at at = { id, here + after };
            uint8_t learned = rx_dfa_learn(s, &at, until);
            if (learned != RX_DFA_LISTED || !s->cached) return learned;
            id = at.id;
            byte = s->pos >> 1;
            from = byte;
            visits = s->visits;
            copies = s->copies;
            here = there;
            holds = true;
            continue;
        }
        const rx_dfa_edge *edge = &d->edge[e];
        byte++;
        holds = false;
        if (visits + edge->step_visits >= until) {
            /* Stopped after the step, as the automaton stops: the start
             * comes when the search goes on. */
            visits += edge->step_visits;
            copies += edge->step_copies;
            pending = e;
            break;
        }
        visits += edge->visits;
        copies += edge->copies;
        here = there;
        id = edge->target;
    }
    s->generation += 2u * (byte - from);
    s->visits = visits;
    s->copies = copies;
    if (holds) return RX_DFA_LISTED;
    if (pending == 0 && id != RX_DFA_SKIP) {
        rx_dfa_load(s, id, byte);
        return RX_DFA_LISTED;
    }
    /* Between the step and the start: no thread was there to skip from; or
     * the start comes (s->pending, whose target is loaded then), and until
     * then the search has no thread and stands as if matched, so that it
     * neither skips nor starts a thread when it goes on (rx_search_run). */
    s->pending = pending;
    s->ccount = 0;
    s->matched = pending != 0;
    s->pos = byte << 1;
    s->prev = text[byte - 1u];
    s->unit = rx_unit_at(&s->in, s->pos, &s->next);
    return pending != 0 ? RX_DFA_PAUSE : RX_DFA_TOP;
}

/* Run the search until it ends (true; s->matched says whether s->best
 * holds a match) or it has visited `budget` instructions more (false),
 * finishing the position it is at. */
static bool rx_search_run(rx_search *s, uint64_t budget) {
    uint64_t until = s->visits + budget;
    uint32_t n = s->re->slots;
    uint8_t lead = s->re->lead;
    for (;;) {
        if (lead != RX_LEAD_NONE && s->ccount == 0 && !s->matched && !s->sticky && (lead != RX_LEAD_START || s->pos > 0)) {
            /* Each position: the start's closure (its SAVEs and the
             * character or the ^) and, led by a character, its thread's
             * step, which fails; or, after ^, nothing more. */
            rx_cost pass = { 0, s->re->lead_saves + 2u, 2u * (uint64_t)n };
            if (lead == RX_LEAD_START) pass = (rx_cost){ 0, s->re->lead_saves + 1u, n };
            pass.positions = rx_lead_run(s, s->pos >> 1, rx_budget_positions(s, pass.visits, until));
            if (pass.positions > 0) {
                if (rx_pass(s, &pass, until)) return false;
                continue;
            }
        }
        if (!s->matched && (!s->sticky || s->pos == s->start) && lead == RX_LEAD_START && s->pos > 0) {
            /* A thread from here dies at ^, after the SAVEs before it: only
             * the start reaches them, so it changes nothing but the count. */
            s->copies += n;
            s->visits += s->re->lead_saves + 1u;
        } else if (!s->matched && (!s->sticky || s->pos == s->start)) {
            /* A thread from here, after every thread from earlier. */
            for (uint32_t k = 0; k < n; k++) s->cur[k] = RX_UNSET;
            s->copies += n;
            s->at_pos = s->pos;
            s->at_prev = s->prev;
            s->at_unit = s->unit;
            rx_threads now = { s->cpc, s->cslots, &s->ccount };
            rx_closure(s, &now, 0);
        }
        if (s->cached) {
            uint8_t cached = rx_dfa_run(s, until);
            if (cached == RX_DFA_PAUSE) return false;
            if (cached == RX_DFA_TOP) continue;
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
        /* rx_method_check accepted the limit. */
        (void)sjs_split_limit(m->operands.second, &j->limit);
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
    /* Before the y check: replaceAll without g throws a TypeError in
     * JavaScript, whatever its other flags. */
    if ((m->all || m->op == RX_MATCH_ALL) && !global) {
        sjs_vm_fault(vm, "JavaScript throws a TypeError for a regular expression without g here");
        return false;
    }
    if ((m->op == RX_MATCH || m->op == RX_REPLACE) && sticky && !global) {
        sjs_vm_fault(vm, "a regular expression with y and without g changes its lastIndex when it matches, and staticjs values do not change");
        return false;
    }
    uint32_t limit;
    if (m->op == RX_SPLIT && !sjs_split_limit(o->second, &limit)) {
        sjs_vm_fault(vm, "staticjs splits with a limit that is a number, a boolean, null or undefined");
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

/* Whether a method's search keeps the cache of its steps: only a test, not
 * sticky, over an ASCII input, of a program the cache takes (dfa_classes);
 * and not with the switch off. A test runs as a loop only when its work is long
 * (sjs_regex_call), which setting the cache up adds little to. */
static bool rx_dfa_wanted(const sjs_vm *vm, const rx_method *m) {
    const sjs_regexp *re = SJS_AS_REGEXP(m->operands.regexp);
    const sjs_string *input = SJS_AS_STRING(m->operands.input);
    return !vm->no_regex_dfa && m->op == RX_TEST && (input->h.flags & SJS_STRING_ASCII) != 0 && (re->flags & SJS_REGEX_STICKY) == 0 &&
           re->dfa_classes != 0;
}

/* The loop's state: a block of values' size, as sjs_loop_free frees it. */
sjs_result sjs_regex_loop_start(sjs_vm *vm, vm_frame *frame) {
    sjs_value *slots = &vm->stack[frame->base];
    rx_method m = { (uint8_t)(frame->builtin - SJS_REGEX_LOOP_FIRST), false,
                    rx_operands_of((rx_call_site){ (uint8_t)(frame->builtin - SJS_REGEX_LOOP_FIRST), slots[LOOP_RECEIVER],
                                                   { &slots[LOOP_CALLBACK], frame->argc } }) };
    bool cached = rx_dfa_wanted(vm, &m);
    /* The job, then the cache, if any, on a boundary of 16 bytes
     * (rx_dfa_of); it is set up when the search first runs from it. */
    uint64_t job = (rx_job_words(SJS_AS_REGEXP(m.operands.regexp)) * 4u + 15u) & ~(uint64_t)15u;
    uint64_t bytes = job + (cached ? RX_DFA_BYTES : 0u);
    uint64_t values = (bytes + sizeof(sjs_value) - 1u) / sizeof(sjs_value);
    if (values * sizeof(sjs_value) > UINT32_MAX) return sjs_vm_fail(vm, SJS_ERR_LIMIT);
    frame->scratch = sjs_mem_alloc(&vm->context->counting, (size_t)values * sizeof(sjs_value));
    if (frame->scratch == NULL) return sjs_vm_fail(vm, SJS_ERR_NOMEM);
    frame->scratch_count = (uint32_t)values;
    frame->phase = 0;
    sjs_zero(frame->scratch, (size_t)job);
    uint32_t *block = (uint32_t *)(void *)frame->scratch;
    rx_work w = { vm, (rx_job *)(void *)block, &slots[LOOP_OUTPUT] };
    if (rx_job_start(&w, &m, block + (sizeof(rx_job) + 3u) / 4u) != SJS_OK) return SJS_ERR_PROGRAM;
    if (cached) {
        w.job->s.cached = true;
        rx_dfa_of(&w.job->s)->k = 0;
    }
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
