/*
 * The parser: tokens in, syntax events out, in evaluation order.
 *
 * Expressions are read by operator precedence over two explicit stacks: a
 * stack of frames (one per open bracket, template, conditional, pattern or
 * statement) and a stack of operators waiting for their right operand. An
 * operator's event is emitted when it is popped, after its operands; `&&`,
 * `?:`, `??` and `?.` also emit a marker where they branch. Nothing recurses.
 *
 * Tokens wait in a queue only while the parser needs to see further ahead:
 * after a name, to tell `x => ...` from `x`; after `(`, to tell an arrow
 * function's parameters from a parenthesized expression; after `import`,
 * to tell a declaration from import() and import.meta; and after the first
 * token of a statement or a word staticjs refuses, to tell JavaScript it
 * leaves out (unsupported) from what is not JavaScript (syntax). That
 * lookahead is bounded by max_lookahead.
 *
 * The lookahead after `(` is one scan that resumes where it stopped: in a
 * pattern it decides as soon as a token no pattern takes appears, so `(a +
 * b)` is an expression at `+`; a default value or a computed key it only
 * balances, to the `,` or the bracket that ends it. There a `/` that cannot
 * divide begins a regular expression: the scan says so while the lexer hands
 * it the `/`, before the lexer reads on, and the lexer reads the literal as
 * one token, which takes the slash's place (regex_starts). It keeps what
 * each bracket it balances is, as the end of an if's condition or of a block
 * statement is the start of a statement, where a `/` cannot divide.
 *
 * Wherever else a `/` begins an operand, the parser decides the same way,
 * when the `/` arrives, and waits for the literal's token. A literal is
 * classified when it arrives (regex.c): a syntax error, JavaScript staticjs
 * leaves out (refused, and read on), or an SJS_EV_REGEXP.
 *
 * Events wait in a held list where the source and the evaluation order
 * differ. A const holds its pattern until its value has been sent. A
 * parameter's, an element's or a property's target is held until the token
 * after it: at `=`, its default value's events go first, and the target's
 * follow. Inside something already held, nothing is sent: the events are
 * rotated in place instead, so a default or a const nested in a held
 * pattern ends up in evaluation order when the outermost hold is sent.
 *
 * A module's import and export statements hold their specifiers until the
 * statement's end says what they are; then each one is an event. A line
 * break ends a statement where JavaScript would insert a semicolon.
 *
 * Statements are read by a frame per block (a function's body, a block
 * statement) and per if; an else-if chain is one frame, whatever its length.
 * When a statement ends, the if whose consequent it was waits for its else,
 * and the if whose alternative it was ends, with any if around it that ends
 * with it: a loop over the frames, not a recursion. So a dangling else binds
 * to the nearest if. A declaration cannot be the statement of an if; which
 * one is, the parser tells from at most two tokens (`let [`, `async
 * function`), or past a run of labels.
 *
 * JavaScript staticjs leaves out is refused, but where the parser can read
 * on (an assignment, the comma operator, an operator staticjs does not
 * have, `this`, a method, an expression statement, a statement at a
 * module's top level) it does, sending no more events, so that a syntax
 * error further on is reported as one: "unsupported" must be JavaScript.
 * The refusal is reported at the end, or at the first construct the parser
 * cannot read past.
 */
#include "syntax.h"
#include "regex.h"

typedef struct {
    uint8_t kind;
    uint8_t punctuator;
    uint8_t keyword;
    uint8_t newline_before;
    uint8_t lone_surrogate;
    uint8_t escaped;
    uint8_t bigint;
    sjs_position at;
    double number;
    uint32_t text_offset;
    uint32_t text_length;
} queued_token;

/* Frames. */
enum {
    F_EXPRESSION_GOAL = 1, /* the whole program is one expression */
    F_MODULE,
    F_PAREN,
    F_ARRAY,
    F_CALL,                /* arguments of a call, or of new Set */
    F_INDEX,
    F_TEMPLATE,
    F_CONDITIONAL,         /* the consequent of ?: */
    F_OBJECT,
    F_COMPUTED_KEY,
    F_BLOCK,               /* an arrow function's block body */
    F_INNER_BLOCK,         /* a block statement */
    F_IF,                  /* an if, or a whole else-if chain */
    F_LABEL,               /* `name:`, refused and read on to its statement's end */
    F_CONST,
    F_RETURN,
    F_EXPRESSION_STATEMENT, /* MUTATION: sent if it is a call, else refused at its end */
    F_PARAMS,
    F_PATTERN_ARRAY,
    F_PATTERN_OBJECT,
    F_IMPORT,
    F_EXPORT,              /* export { ... }, export * */
    F_DEFAULT,             /* the expression of export default */
    F_NEW,
    F_INITIALIZER          /* a default value in a pattern: `= value` */
};

/* States. */
enum {
    /* F_OBJECT */
    O_KEY = 0, O_KEY_NAME, O_COLON, O_VALUE,
    /* F_CONST */
    C_PATTERN = 0, C_ASSIGN, C_INIT,
    /* F_PARAMS */
    P_START = 0, P_REST, P_AFTER, P_END,
    /* F_PATTERN_ARRAY */
    PA_ELEMENT = 0, PA_REST, PA_AFTER,
    /* F_PATTERN_OBJECT */
    PO_KEY = 0, PO_KEY_NAME, PO_COLON, PO_VALUE, PO_REST, PO_AFTER,
    /* F_IF */
    IF_OPEN = 0, IF_TEST, IF_CONSEQUENT, IF_AFTER_CONSEQUENT, IF_ALTERNATIVE,
    /* F_MODULE */
    M_ITEMS = 0, M_EXPORT,
    /* F_IMPORT */
    I_OPEN = 0, I_DEFAULT_AFTER, I_SECOND, I_STAR, I_STAR_NAME, I_NAME, I_AS, I_ALIAS, I_AFTER, I_FROM, I_SOURCE, I_END,
    /* F_EXPORT */
    X_NAME = 0, X_AS, X_ALIAS, X_AFTER, X_CLOSE, X_STAR, X_STAR_NAME, X_FROM, X_SOURCE, X_END,
    /* F_DEFAULT */
    DF_FIRST = 0, DF_VALUE,
    /* F_IMPORT, F_EXPORT: `with { key: "value" }` after the module's name */
    WA_OPEN = 32, WA_KEY, WA_COLON, WA_VALUE, WA_AFTER, WA_END,
    /* F_NEW */
    N_NAME = 0, N_OPEN,
    /* MUTATION: F_EXPRESSION_STATEMENT: an expression, an assignment, `++x`, `x++`,
     * `delete x.k` */
    ES_EXPRESSION = 0, ES_ASSIGN, ES_PREFIX, ES_UPDATE, ES_DELETE
};

/* How the current expression frame expects its next token. */
enum { E_OPERAND = 0, E_OPERATOR, E_AFTER_BLOCK_ARROW };

/* p->name_keyword of a string held where a name may be. */
#define NAME_STRING 0xFFu

/* What an import or export statement's specifiers held (frame.names). */
#define SPEC_RESERVED_LOCAL 1u /* export { if }: only a re-export may name it */
#define SPEC_STRING_LOCAL 2u   /* { "a" as b } */
#define SPEC_STRING_NAME 4u    /* { a as "b" }, * as "b" */

/* Segment flags: which short-circuit operators the current operand chain used. */
#define SEG_LOGICAL 1u
#define SEG_COALESCE 2u

typedef struct {
    uint8_t type;
    uint8_t state;
    uint8_t spread;
    uint8_t segment;
    uint8_t chain;
    uint8_t buffered;      /* its events are held, not sent */
    uint8_t holds;         /* something outside it holds, so it holds too */
    uint8_t after_comma;
    uint8_t is_new;
    uint8_t names;         /* F_IMPORT, F_EXPORT: SPEC_* flags */
    uint8_t exported;      /* F_CONST: export const, so every declarator is exported */
    uint8_t in_target;     /* F_PARAMS, F_PATTERN_*: a target is held, from target_base */
    uint8_t rest;          /* F_PARAMS, F_PATTERN_*: the last target was `...` */
    uint8_t nonsimple;     /* F_PARAMS, F_BLOCK: the parameters are not all plain names */
    uint8_t not_pattern;   /* F_ARRAY, F_OBJECT: an item no assignment pattern takes */
    uint8_t refused_before; /* MUTATION: F_EXPRESSION_STATEMENT: the parser was refusing when it began */
    uint8_t declares_let;  /* MUTATION: F_CONST: a let declaration */
    uint8_t bare;          /* MUTATION: F_CONST: its declarator has no value (`let x;`) */
    uint32_t ops_base;
    uint32_t count;
    uint32_t held_base;    /* F_CONST: its pattern's first held event; F_IMPORT, F_EXPORT: its specifiers';
                            * F_LABEL: where its name begins in sjs_parser.labels (count: its length) */
    uint32_t held_mid;     /* F_CONST: its value's first; F_INITIALIZER: its default value's */
    uint32_t target_base;  /* F_PARAMS, F_PATTERN_*: the held target's first */
    sjs_position at;
    /* The node its events belong to (sjs_event.node, .start); 0 for a frame
     * that sends no events of its own (F_PAREN, F_LABEL, F_COMPUTED_KEY, ...).
     * F_IMPORT and F_EXPORT keep only the statement's start. */
    uint32_t node;
    sjs_position start;
    sjs_position chain_start; /* where the optional chain open in it starts */
} frame;

/* Operators waiting for their right operand. */
/* OP_ASSIGN is an assignment refused (refuse) and read on: its `op` is 1
 * when it can be a pattern's target with a default value. MUTATION:
 * OP_STORE is an assignment statement's, whose `op` is its operator (0 for
 * `=`), and OP_UPDATE a statement's prefix ++ or -- (SJS_OP_ADD or
 * SJS_OP_SUBTRACT). */
enum { OP_UNARY = 1, OP_BINARY, OP_LOGICAL, OP_ARROW, OP_ELSE, OP_ASSIGN, OP_STORE, OP_UPDATE,
       OP_DELETE /* MUTATION: `delete`; `op` is 1 for a statement's, 0 for one refused as a value */ };

typedef struct {
    uint8_t kind;
    uint8_t op;
    uint8_t precedence;
    uint8_t saved_segment;
    sjs_position at;
    uint32_t node;         /* the node of its events (sjs_event.node, .start) */
    sjs_position start;
} pending_op;

/* The lookahead after `(` (decide_paren), as far as it has read. */
typedef struct {
    uint32_t paren;        /* the `(` it decides: its offset + 1; 0 for none */
    uint32_t next;         /* the next token to read, counted from the `(` */
    uint32_t levels;       /* pattern levels open inside the parentheses (sjs_parser.scan_levels) */
    uint32_t inner;        /* brackets open inside a default value or a computed key */
    uint32_t commas;       /* at level 0 */
    uint8_t region;        /* SCAN_* */
    uint8_t key;           /* in an object level: SK_* */
    uint8_t any;           /* a token at level 0 */
    uint8_t trailing;      /* the last token at level 0 was a comma */
    uint8_t closed;        /* BR_*: the bracket a default value or a computed key closed last */
} paren_scan;

/* An event held until it can be emitted: a const's pattern, a target, an
 * import or export specifier. */
typedef struct {
    uint8_t kind;
    uint32_t count;
    double number;
    uint32_t text_offset;
    uint32_t text_length;
    uint32_t alias_offset;
    uint32_t alias_length;
    sjs_position at;
    uint32_t node;
    sjs_position start;
} held_event;

/* Where an event is: its own token, and the node it belongs to
 * (sjs_event.at, .node and .start). */
typedef struct {
    sjs_position at;
    uint32_t node;
    sjs_position start;
} event_place;

struct sjs_parser {
    sjs_allocator allocator;
    sjs_limits limits;
    sjs_goal goal;
    sjs_sink sink;
    sjs_lexer lexer;
    sjs_error_slot error;
    bool failed;
    bool finished;
    bool done;
    uint8_t expect;
    sjs_buf queue;
    uint32_t head;
    sjs_buf queue_text;
    sjs_buf frames;
    sjs_buf ops;
    sjs_buf held;          /* held_event records */
    sjs_buf held_text;
    sjs_buf name;          /* a name the parser holds across tokens */
    sjs_position name_at;
    uint8_t name_keyword;
    bool arrow_body_next;  /* the next token begins an arrow function's body */
    bool params_nonsimple; /* ... whose parameters are not all plain names */
    uint8_t last_operand;  /* LO_*: what the operand just read can be assigned as */
    uint32_t next_node;    /* the id the next node gets (sjs_event.node), from 1 */
    sjs_position last_start; /* the start of the last event sent: of the operand just read */
    uint32_t arrow_node;   /* the arrow function whose body begins next (arrow_body_next) */
    sjs_position arrow_start;
    sjs_position export_token; /* the `export` of the statement being read */
    bool default_exported; /* a module exported a name `default` */
    paren_scan scan;
    sjs_buf scan_levels;   /* a byte per pattern level the scan has open: 1 for an object */
    sjs_buf scan_brackets; /* a byte per bracket open in what a lookahead balances: BR_* */
    sjs_buf labels;        /* the names of the F_LABEL frames open */
    /* The first JavaScript staticjs leaves out, while the parser reads on to
     * find a syntax error after it; no event is sent after it. */
    sjs_error_slot refusal;
    bool refusing;
    uint8_t skip;          /* SKIP_*: reading past a method staticjs refused */
    uint32_t skip_depth;   /* brackets open in that method */
    bool statement_ends;   /* MUTATION: an expression statement's end closes its operators */
    bool last_name;        /* MUTATION: the last event sent was a NAME, for delete */
};

typedef enum { H_TAKEN = 0, H_AGAIN, H_WAIT, H_STOP } handled;

/* sjs_parser.skip: a method's head, before its body's `{`, or its body. */
enum { SKIP_NONE = 0, SKIP_HEAD, SKIP_BODY };

/* What an operand is to the `=` after it, by its last event: a name or a
 * member (a simple target), a call (JavaScript reads `f() = 1`, and throws
 * when it runs), an array or object literal that can be read as a pattern,
 * or anything else, which cannot be assigned to. LO_DEFAULTED is `target =
 * value`: not a target, but an element of a pattern with a default value. */
enum { LO_OTHER = 0, LO_SIMPLE, LO_CALL, LO_PATTERN, LO_DEFAULTED };

#define PRECEDENCE_ELSE 1u
#define PRECEDENCE_COALESCE 4u
#define PRECEDENCE_OR 4u
#define PRECEDENCE_AND 5u
#define PRECEDENCE_BITWISE_OR 6u
#define PRECEDENCE_BITWISE_XOR 7u
#define PRECEDENCE_BITWISE_AND 8u
#define PRECEDENCE_EQUALITY 9u
#define PRECEDENCE_RELATIONAL 10u
#define PRECEDENCE_SHIFT 11u
#define PRECEDENCE_ADDITIVE 12u
#define PRECEDENCE_MULTIPLICATIVE 13u
#define PRECEDENCE_EXPONENT 14u
#define PRECEDENCE_UNARY 15u

/* ------------------------------------------------------------------------
 * Errors and events
 */

/* Report the refusal the parser read on after. */
static handled report_refusal(sjs_parser *p) {
    p->error = p->refusal;
    p->error.error.subject.ptr = p->error.subject;
    p->failed = true;
    return H_STOP;
}

/* A syntax error after a refusal is still the program's error, as JavaScript
 * rejects the whole program for it; any other stop reports the refusal. */
static handled parse_fail(sjs_parser *p, const queued_token *t, sjs_problem problem) {
    if (p->refusing && problem.code != SJS_ERROR_SYNTAX) return report_refusal(p);
    sjs_position at = t != NULL ? t->at : p->lexer.here;
    sjs_error_set(&p->error, at, problem);
    if (t != NULL && t->text_length > 0) {
        sjs_error_subject(&p->error, (sjs_str){ (const char *)p->queue_text.ptr + t->text_offset, t->text_length });
    }
    p->failed = true;
    return H_STOP;
}

/* JavaScript staticjs leaves out, where the parser can read on: the first
 * one is reported when the source ends without a syntax error, since
 * JavaScript would refuse the whole program for one ("unsupported" must be
 * JavaScript). From here on no event is sent. */
static handled refuse(sjs_parser *p, const queued_token *t, sjs_problem problem) {
    if (p->refusing) return H_TAKEN;
    sjs_error_set(&p->refusal, t->at, problem);
    if (t->text_length > 0) {
        sjs_error_subject(&p->refusal, (sjs_str){ (const char *)p->queue_text.ptr + t->text_offset, t->text_length });
    }
    p->refusing = true;
    return H_TAKEN;
}

static handled fail_memory(sjs_parser *p) {
    sjs_error_set(&p->error, p->lexer.here, (sjs_problem){ SJS_ERROR_MEMORY, "out of memory" });
    p->failed = true;
    return H_STOP;
}

static sjs_str token_text(const sjs_parser *p, const queued_token *t) {
    return (sjs_str){ (const char *)p->queue_text.ptr + t->text_offset, t->text_length };
}

static frame *top_frame(sjs_parser *p) {
    uint32_t n = SJS_BUF_COUNT(p->frames, frame);
    return n == 0 ? NULL : &SJS_BUF_AT(p->frames, frame, n - 1);
}

static uint32_t frame_depth(const sjs_parser *p) {
    return SJS_BUF_COUNT(p->frames, frame);
}

static bool buffering(sjs_parser *p) {
    frame *f = top_frame(p);
    return f != NULL && f->buffered;
}

static handled hold_event(sjs_parser *p, const sjs_event *e) {
    if (SJS_BUF_COUNT(p->held, held_event) >= p->limits.max_held_events) {
        return parse_fail(p, NULL, (sjs_problem){ SJS_ERROR_LIMIT, "the parser holds more events than max_held_events" });
    }
    held_event h = { 0 };
    h.kind = (uint8_t)e->kind;
    h.count = e->count;
    h.number = e->number;
    h.at = e->at;
    h.node = e->node;
    h.start = e->start;
    h.text_offset = p->held_text.length;
    h.text_length = e->text.length;
    if (sjs_buf_append(&p->allocator, &p->held_text, (sjs_bytes){ (const uint8_t *)e->text.ptr, e->text.length }) != SJS_OK) {
        return fail_memory(p);
    }
    h.alias_offset = p->held_text.length;
    h.alias_length = e->alias.length;
    if (sjs_buf_append(&p->allocator, &p->held_text, (sjs_bytes){ (const uint8_t *)e->alias.ptr, e->alias.length }) != SJS_OK) {
        return fail_memory(p);
    }
    if (sjs_buf_append(&p->allocator, &p->held, (sjs_bytes){ (const uint8_t *)&h, sizeof h }) != SJS_OK) {
        return fail_memory(p);
    }
    return H_TAKEN;
}

static handled send(sjs_parser *p, const sjs_event *e) {
    bool chain = top_frame(p) != NULL && top_frame(p)->chain;
    p->last_name = e->kind == SJS_EV_NAME; /* MUTATION */
    switch (e->kind) {
    case SJS_EV_NAME:
        p->last_operand = LO_SIMPLE;
        break;
    case SJS_EV_MEMBER:
    case SJS_EV_INDEX:
        p->last_operand = chain ? LO_OTHER : LO_SIMPLE;
        break;
    case SJS_EV_CALL_END:
        p->last_operand = chain ? LO_OTHER : LO_CALL;
        break;
    default:
        p->last_operand = LO_OTHER;
        break;
    }
    p->last_start = e->start;
    if (p->refusing) return H_TAKEN;
    if (buffering(p)) return hold_event(p, e);
    sjs_result r = p->sink.v->event(p->sink.self, e);
    if (r != SJS_OK) {
        /* The sink keeps its own error; the parser records only that it stopped. */
        p->failed = true;
        p->error.error.code = SJS_ERROR_NONE;
        return H_STOP;
    }
    return H_TAKEN;
}

/* A node of its own, new, whose source starts at its token `at`. */
static event_place own_place(sjs_parser *p, sjs_position at) {
    event_place place = { at, p->next_node++, at };
    return place;
}

/* A node, new, whose first operand is the one just read: an operator, a
 * member, an index, a call, a conditional. */
static event_place operand_place(sjs_parser *p, sjs_position at) {
    event_place place = { at, p->next_node++, p->last_start };
    return place;
}

/* An event at `at` of the node a frame or an operator reads. */
static event_place frame_place(const frame *f, sjs_position at) {
    event_place place = { at, f->node, f->start };
    return place;
}

static event_place op_place(const pending_op *op) {
    event_place place = { op->at, op->node, op->start };
    return place;
}

/* A unary operator at `at`: a node of its own, from the operator. */
static pending_op unary(sjs_parser *p, uint8_t op, sjs_position at) {
    pending_op pending = { OP_UNARY, op, PRECEDENCE_UNARY, 0, at, p->next_node++, at };
    return pending;
}

/* END: the program's, node 0, which starts where the source does. */
static event_place program_end(sjs_position at) {
    event_place place = { at, 0, { 0, 1, 1 } };
    return place;
}

/* A frame that sends no events of its own. */
static event_place no_node(sjs_position at) {
    event_place place = { at, 0, at };
    return place;
}

static sjs_event counted(uint8_t kind, uint32_t count, event_place place) {
    sjs_event e = { 0 };
    e.kind = kind;
    e.count = count;
    e.at = place.at;
    e.node = place.node;
    e.start = place.start;
    return e;
}

static sjs_event texted(uint8_t kind, sjs_str text, event_place place) {
    sjs_event e = counted(kind, 0, place);
    e.text = text;
    return e;
}

/* A number literal's event: NUMBER, or BIGINT with its digits and radix. */
static sjs_event number_event(const sjs_parser *p, const queued_token *t, event_place place) {
    if (t->bigint != 0) {
        sjs_event e = texted(SJS_EV_BIGINT, token_text(p, t), place);
        e.count = t->bigint;
        return e;
    }
    sjs_event e = counted(SJS_EV_NUMBER, 0, place);
    e.number = t->number;
    return e;
}

static handled send_kind(sjs_parser *p, uint8_t kind, event_place place) {
    sjs_event e = counted(kind, 0, place);
    return send(p, &e);
}

static handled send_event(sjs_parser *p, sjs_event e) {
    return send(p, &e);
}

static uint32_t held_count(const sjs_parser *p) {
    return SJS_BUF_COUNT(p->held, held_event);
}

/* Where the text of the held events from `from` on starts. They were all
 * held after the ones before `from`, though perhaps reordered since. */
static uint32_t held_text_start(const sjs_parser *p, uint32_t from) {
    uint32_t start = p->held_text.length;
    uint32_t n = held_count(p);
    for (uint32_t i = from; i < n; i++) {
        uint32_t offset = SJS_BUF_AT(p->held, held_event, i).text_offset;
        if (offset < start) start = offset;
    }
    return start;
}

/* Forget the held events from `from` on. */
static void drop_held(sjs_parser *p, uint32_t from) {
    p->held_text.length = held_text_start(p, from);
    p->held.length = from * (uint32_t)sizeof(held_event);
}

/* Send the held events from `from` on, in order, and forget them. */
static handled send_held(sjs_parser *p, uint32_t from) {
    uint32_t n = held_count(p);
    if (p->refusing) n = from;
    for (uint32_t i = from; i < n; i++) {
        held_event h = SJS_BUF_AT(p->held, held_event, i);
        sjs_event e = { 0 };
        e.kind = h.kind;
        e.count = h.count;
        e.number = h.number;
        e.at = h.at;
        e.node = h.node;
        e.start = h.start;
        e.text = (sjs_str){ (const char *)p->held_text.ptr + h.text_offset, h.text_length };
        e.alias = (sjs_str){ (const char *)p->held_text.ptr + h.alias_offset, h.alias_length };
        sjs_result r = p->sink.v->event(p->sink.self, &e);
        if (r != SJS_OK) {
            p->failed = true;
            p->error.error.code = SJS_ERROR_NONE;
            return H_STOP;
        }
    }
    drop_held(p, from);
    return H_TAKEN;
}

static void reverse_held(sjs_parser *p, uint32_t from, uint32_t to) {
    held_event *h = (held_event *)(void *)p->held.ptr;
    while (from + 1u < to) {
        held_event swap = h[from];
        h[from++] = h[--to];
        h[to] = swap;
    }
}

/* The held events [from, mid) and [mid, end) trade places, in place. Their
 * text stays where it is. */
static void rotate_held(sjs_parser *p, uint32_t from, uint32_t mid) {
    uint32_t n = held_count(p);
    reverse_held(p, from, mid);
    reverse_held(p, mid, n);
    reverse_held(p, from, n);
}

/* The held events [from, mid) come after [mid, end) in evaluation order:
 * a pattern after its value, a target after its default value. Send them
 * so, or, where something outside the top frame holds, reorder them. */
static handled settle_held(sjs_parser *p, uint32_t from, uint32_t mid) {
    rotate_held(p, from, mid);
    if (top_frame(p)->holds) return H_TAKEN;
    return send_held(p, from);
}

/* The top frame's pattern starts here in the held events. */
static void hold_from_here(sjs_parser *p) {
    frame *f = top_frame(p);
    f->held_base = held_count(p);
    f->buffered = 1;
}

/* ------------------------------------------------------------------------
 * Stacks
 */

/* A frame at `place.at`, whose events are of `place`'s node. */
static handled push_frame(sjs_parser *p, uint8_t type, event_place place) {
    if (frame_depth(p) >= p->limits.max_nesting) {
        sjs_error_set(&p->error, place.at, (sjs_problem){ SJS_ERROR_LIMIT, "the program nests deeper than max_nesting" });
        p->failed = true;
        return H_STOP;
    }
    frame f = { 0 };
    f.type = type;
    f.at = place.at;
    f.node = place.node;
    f.start = place.start;
    f.ops_base = SJS_BUF_COUNT(p->ops, pending_op);
    frame *parent = top_frame(p);
    if (parent != NULL && parent->buffered) f.holds = f.buffered = 1;
    if (sjs_buf_append(&p->allocator, &p->frames, (sjs_bytes){ (const uint8_t *)&f, sizeof f }) != SJS_OK) {
        return fail_memory(p);
    }
    return H_TAKEN;
}

static void pop_frame(sjs_parser *p) {
    p->frames.length -= (uint32_t)sizeof(frame);
}

static handled push_op(sjs_parser *p, pending_op op) {
    if (SJS_BUF_COUNT(p->ops, pending_op) >= p->limits.max_nesting) {
        sjs_error_set(&p->error, op.at, (sjs_problem){ SJS_ERROR_LIMIT, "the program nests deeper than max_nesting" });
        p->failed = true;
        return H_STOP;
    }
    if (sjs_buf_append(&p->allocator, &p->ops, (sjs_bytes){ (const uint8_t *)&op, sizeof op }) != SJS_OK) {
        return fail_memory(p);
    }
    return H_TAKEN;
}

static pending_op *top_op(sjs_parser *p) {
    frame *f = top_frame(p);
    uint32_t n = SJS_BUF_COUNT(p->ops, pending_op);
    if (f == NULL || n <= f->ops_base) return NULL;
    return &SJS_BUF_AT(p->ops, pending_op, n - 1);
}

static handled prefix_update(sjs_parser *p, const pending_op *op); /* MUTATION */
static handled delete_end(sjs_parser *p, const pending_op *op);    /* MUTATION */

/* Emit the event of the operator on top and pop it. */
static handled pop_op(sjs_parser *p) {
    pending_op op = *top_op(p);
    p->ops.length -= (uint32_t)sizeof(pending_op);
    switch (op.kind) {
    case OP_UNARY:
        return send_event(p, counted(SJS_EV_UNARY, op.op, op_place(&op)));
    case OP_BINARY:
        return send_event(p, counted(SJS_EV_BINARY, op.op, op_place(&op)));
    case OP_LOGICAL:
        return send_event(p, counted(SJS_EV_LOGICAL_END, op.op, op_place(&op)));
    case OP_ARROW:
        top_frame(p)->segment = op.saved_segment;
        return send_kind(p, SJS_EV_ARROW_END, op_place(&op));
    case OP_ASSIGN:
        top_frame(p)->segment = op.saved_segment;
        p->last_operand = op.op ? LO_DEFAULTED : LO_OTHER;
        return H_TAKEN;
    case OP_STORE: /* MUTATION: an assignment statement's value has ended */
        top_frame(p)->segment = op.saved_segment;
        return send_event(p, counted(SJS_EV_ASSIGN_END, op.op, op_place(&op)));
    case OP_UPDATE: /* MUTATION */
        return prefix_update(p, &op);
    case OP_DELETE: /* MUTATION */
        return delete_end(p, &op);
    default:
        top_frame(p)->segment = op.saved_segment;
        return send_kind(p, SJS_EV_CONDITIONAL_END, op_place(&op));
    }
}

/* Pop every operator that binds at least as tightly as `precedence`. */
static handled pop_ops(sjs_parser *p, uint8_t precedence) {
    pending_op *op;
    while ((op = top_op(p)) != NULL && op->precedence >= precedence) {
        if (pop_op(p) == H_STOP) return H_STOP;
    }
    return H_TAKEN;
}

static handled end_chain(sjs_parser *p) {
    frame *f = top_frame(p);
    if (!f->chain) return H_TAKEN;
    f->chain = 0;
    event_place place = { f->at, p->next_node++, f->chain_start };
    return send_event(p, counted(SJS_EV_CHAIN_END, frame_depth(p), place));
}

/* The current expression is complete: close its chain and its operators. */
static handled end_expression(sjs_parser *p) {
    if (end_chain(p) == H_STOP) return H_STOP;
    return pop_ops(p, 0);
}

/* ------------------------------------------------------------------------
 * The token queue
 */

static const queued_token *peek(sjs_parser *p, uint32_t ahead) {
    uint32_t n = SJS_BUF_COUNT(p->queue, queued_token);
    uint32_t at = p->head + ahead;
    if (at < n) return &SJS_BUF_AT(p->queue, queued_token, at);
    if (p->finished && n > 0) return &SJS_BUF_AT(p->queue, queued_token, n - 1);
    return NULL;
}

static bool is_punctuator(const queued_token *t, uint8_t punctuator) {
    return t->kind == SJS_TOKEN_PUNCTUATOR && t->punctuator == punctuator;
}

static handled hold_name(sjs_parser *p, const queued_token *t) {
    p->name.length = 0;
    p->name_at = t->at;
    p->name_keyword = t->keyword;
    sjs_str text = token_text(p, t);
    if (sjs_buf_append(&p->allocator, &p->name, (sjs_bytes){ (const uint8_t *)text.ptr, text.length }) != SJS_OK) {
        return fail_memory(p);
    }
    return H_TAKEN;
}

static sjs_str held_name(const sjs_parser *p) {
    return (sjs_str){ (const char *)p->name.ptr, p->name.length };
}

/* ------------------------------------------------------------------------
 * Expressions: operands
 */

/* Does the operator `op` wait for an assignment expression? */
static bool takes_assignment(const pending_op *op) {
    return op == NULL || op->kind == OP_ARROW || op->kind == OP_ELSE || op->kind == OP_ASSIGN ||
           op->kind == OP_STORE; /* MUTATION */
}

/* An arrow function may start only where an assignment expression may. */
static bool arrow_allowed(sjs_parser *p) {
    return takes_assignment(top_op(p));
}

/* After `=>`: a block, or an expression that runs as far as it can. */
static handled arrow_body(sjs_parser *p, const queued_token *t) {
    event_place place = { t->at, p->arrow_node, p->arrow_start };
    if (is_punctuator(t, SJS_P_LBRACE)) {
        if (push_frame(p, F_BLOCK, place) == H_STOP) return H_STOP;
        top_frame(p)->nonsimple = p->params_nonsimple ? 1u : 0u;
        return send_kind(p, SJS_EV_BLOCK_BEGIN, place);
    }
    frame *f = top_frame(p);
    pending_op op = { OP_ARROW, 0, PRECEDENCE_ELSE, f->segment, t->at, place.node, place.start };
    f->segment = 0;
    if (push_op(p, op) == H_STOP) return H_STOP;
    p->expect = E_OPERAND;
    return H_AGAIN;
}

typedef enum { D_WAIT = 0, D_ARROW, D_EXPRESSION, D_FAILED, D_NEXT, D_AGAIN } decision;

/* paren_scan.region: what the scan reads. A pattern, token by token; or a
 * default value or a computed key, which it only balances. */
enum { SCAN_PATTERN = 0, SCAN_DEFAULT, SCAN_KEY };
/* paren_scan.key: where an object pattern is. */
enum { SK_KEY = 0, SK_VALUE, SK_AFTER };

static bool scan_opens(const queued_token *t) {
    if (t->kind == SJS_TOKEN_TEMPLATE_HEAD) return true;
    return t->kind == SJS_TOKEN_PUNCTUATOR &&
           (t->punctuator == SJS_P_LPAREN || t->punctuator == SJS_P_LBRACKET || t->punctuator == SJS_P_LBRACE);
}

static bool scan_closes(const queued_token *t) {
    if (t->kind == SJS_TOKEN_TEMPLATE_TAIL) return true;
    return t->kind == SJS_TOKEN_PUNCTUATOR &&
           (t->punctuator == SJS_P_RPAREN || t->punctuator == SJS_P_RBRACKET || t->punctuator == SJS_P_RBRACE);
}

/* Can `t` end an operand, so that a `/` after it divides? Where the parser
 * reads ahead without parsing, this tells a division from a regular
 * expression, which the lexer must read as one: `(a = /'/) => a`. */
static bool ends_operand(const queued_token *t) {
    switch (t->kind) {
    case SJS_TOKEN_NUMBER:
    case SJS_TOKEN_STRING:
    case SJS_TOKEN_TEMPLATE:
    case SJS_TOKEN_TEMPLATE_TAIL:
    case SJS_TOKEN_REGEXP:
        return true;
    case SJS_TOKEN_NAME:
        return t->keyword == SJS_K_NONE || t->keyword == SJS_K_TRUE || t->keyword == SJS_K_FALSE ||
               t->keyword == SJS_K_NULL || t->keyword == SJS_K_UNSUPPORTED_NAME;
    case SJS_TOKEN_PUNCTUATOR:
        return t->punctuator == SJS_P_RPAREN || t->punctuator == SJS_P_RBRACKET || t->punctuator == SJS_P_RBRACE ||
               t->punctuator == SJS_P_PLUS_PLUS || t->punctuator == SJS_P_MINUS_MINUS;
    default:
        return false;
    }
}

/* Is `t` a slash, which may begin a regular expression? */
static bool is_slash(const sjs_parser *p, const queued_token *t) {
    return is_punctuator(t, SJS_P_SLASH) ||
           (is_punctuator(t, SJS_P_ASSIGN_OPERATOR) && sjs_str_equal(token_text(p, t), SJS_STR("/=")));
}

/* The slash `t` begins a regular expression. The lexer has read no byte past
 * it, as the parser decides while the lexer hands it over (the newest token
 * queued): the lexer reads the literal next, and its token takes the
 * slash's place in the queue (on_token). Until then the parser waits. */
static handled regex_starts(sjs_parser *p, const queued_token *t) {
    uint32_t n = SJS_BUF_COUNT(p->queue, queued_token);
    if (n == 0 || SJS_BUF_AT(p->queue, queued_token, n - 1u).at.offset != t->at.offset) {
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs cannot tell where this regular expression ends" });
    }
    p->lexer.regex_next = is_punctuator(t, SJS_P_SLASH) ? 1u : 2u;
    return H_WAIT;
}

static bool word_listed(sjs_str word, const char *const *list, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (sjs_str_equal(word, sjs_cstr(list[i]))) return true;
    }
    return false;
}

#define WORD_LIST(list) (list), (uint32_t)(sizeof(list) / sizeof((list)[0]))

static bool is_word(const sjs_parser *p, const queued_token *t, sjs_str word) {
    return t->kind == SJS_TOKEN_NAME && sjs_str_equal(token_text(p, t), word);
}

/* What a bracket the lookahead balances is, where it reads without parsing:
 * an if's condition, an arrow function's body, a block statement, or
 * anything else. A `/` after the end of a condition or of a block statement
 * begins a statement, so a regular expression: `() => { if (x) /'/ }`. */
enum { BR_OTHER = 0, BR_CONDITION, BR_BODY, BR_BLOCK };

/* What the lookahead knows of the token before the one it reads. */
typedef struct {
    const queued_token *before;  /* the token before prev, or NULL */
    const queued_token *prev;
    uint8_t open;          /* BR_*: the innermost bracket open */
    uint8_t closed;        /* BR_*: the last bracket closed */
} bracket_seen;

/* The kind of bracket `t` opens. The statements staticjs leaves out count
 * too: the lookahead reads JavaScript, and must not mistake their regular
 * expressions for a division. A keyword after `.` or `?.` is a property's
 * name, as in `o.if(4) / 2`, and heads nothing. */
static uint8_t bracket_kind(const sjs_parser *p, const queued_token *t, const bracket_seen *seen) {
    static const char *const headed[] = { "if", "while", "for", "with", "switch", "catch" };
    static const char *const blocks[] = { "else", "do", "try", "finally" };
    const queued_token *prev = seen->prev;
    bool property = seen->before != NULL && (is_punctuator(seen->before, SJS_P_DOT) || is_punctuator(seen->before, SJS_P_OPTIONAL));
    bool reserved = prev->kind == SJS_TOKEN_NAME && prev->keyword == SJS_K_RESERVED && !property;
    if (is_punctuator(t, SJS_P_LPAREN)) return reserved && word_listed(token_text(p, prev), WORD_LIST(headed)) ? BR_CONDITION : BR_OTHER;
    if (!is_punctuator(t, SJS_P_LBRACE)) return BR_OTHER;
    if (is_punctuator(prev, SJS_P_ARROW)) return BR_BODY;
    if (is_punctuator(prev, SJS_P_SEMICOLON) || (reserved && word_listed(token_text(p, prev), WORD_LIST(blocks)))) return BR_BLOCK;
    if (is_punctuator(prev, SJS_P_RPAREN)) return seen->closed == BR_CONDITION ? BR_BLOCK : BR_OTHER;
    if (is_punctuator(prev, SJS_P_RBRACE)) return seen->closed == BR_BODY || seen->closed == BR_BLOCK ? BR_BLOCK : BR_OTHER;
    if (is_punctuator(prev, SJS_P_LBRACE)) return seen->open == BR_BODY || seen->open == BR_BLOCK ? BR_BLOCK : BR_OTHER;
    return BR_OTHER;
}

/* Does `t` begin a regular expression? A `/` divides only after what ends an
 * operand, and the end of a condition or a block statement does not. */
static bool scan_starts_regex(const sjs_parser *p, const queued_token *t, const bracket_seen *seen) {
    if (!is_slash(p, t)) return false;
    if (!ends_operand(seen->prev)) return true;
    bool closer = is_punctuator(seen->prev, SJS_P_RPAREN) || is_punctuator(seen->prev, SJS_P_RBRACE);
    return closer && (seen->closed == BR_CONDITION || seen->closed == BR_BLOCK);
}

/* Note the bracket opened at `depth` (from 0) in scan_brackets. As many can
 * be open as tokens are read ahead: max_lookahead. */
static bool scan_bracket(sjs_parser *p, uint32_t depth, uint8_t kind) {
    p->scan_brackets.length = depth;
    if (sjs_buf_append(&p->allocator, &p->scan_brackets, (sjs_bytes){ &kind, 1 }) != SJS_OK) {
        fail_memory(p);
        return false;
    }
    return true;
}

/* What the paren scan has seen before the token at s->next. */
static bracket_seen scan_seen(sjs_parser *p) {
    const paren_scan *s = &p->scan;
    const queued_token *before = s->next >= 2u ? peek(p, s->next - 2u) : NULL;
    bracket_seen seen = { before, peek(p, s->next - 1u), BR_OTHER, s->closed };
    if (s->inner > 0) seen.open = SJS_BUF_AT(p->scan_brackets, uint8_t, s->inner - 1u);
    return seen;
}

/* A token of a default value or a computed key: D_NEXT, or D_AGAIN when it
 * ends the region and the pattern reads it. */
static decision scan_region(sjs_parser *p, const queued_token *t) {
    paren_scan *s = &p->scan;
    if (t->kind == SJS_TOKEN_END) return D_EXPRESSION;
    if (scan_opens(t)) {
        bracket_seen seen = scan_seen(p);
        if (!scan_bracket(p, s->inner, bracket_kind(p, t, &seen))) return D_FAILED;
        s->inner++;
        return D_NEXT;
    }
    if (scan_closes(t)) {
        if (s->inner > 0) {
            s->inner--;
            s->closed = SJS_BUF_AT(p->scan_brackets, uint8_t, s->inner);
            return D_NEXT;
        }
        s->closed = BR_OTHER;
        if (s->region == SCAN_KEY) {
            if (!is_punctuator(t, SJS_P_RBRACKET)) return D_EXPRESSION;
            s->region = SCAN_PATTERN;
            s->key = SK_AFTER;
            return D_NEXT;
        }
        s->region = SCAN_PATTERN;
        return D_AGAIN;
    }
    if (s->inner == 0 && s->region == SCAN_DEFAULT && is_punctuator(t, SJS_P_COMMA)) {
        s->region = SCAN_PATTERN;
        return D_AGAIN;
    }
    return D_NEXT;
}

/* Open a level of array (object 0) or object (1) pattern. As many can be
 * open as the parser could then read: max_nesting. */
static decision scan_open(sjs_parser *p, uint8_t object) {
    paren_scan *s = &p->scan;
    if (s->levels >= p->limits.max_nesting) {
        parse_fail(p, peek(p, s->next), (sjs_problem){ SJS_ERROR_LIMIT, "the program nests deeper than max_nesting" });
        return D_FAILED;
    }
    p->scan_levels.length = s->levels;
    if (sjs_buf_append(&p->allocator, &p->scan_levels, (sjs_bytes){ &object, 1 }) != SJS_OK) {
        fail_memory(p);
        return D_FAILED;
    }
    s->levels++;
    s->key = SK_KEY;
    return D_NEXT;
}

/* A token in a pattern. Any token no pattern takes there decides at once:
 * the parentheses hold an expression. */
static decision scan_pattern(sjs_parser *p, const queued_token *t) {
    paren_scan *s = &p->scan;
    uint32_t level = s->levels;
    bool object = level > 0 && SJS_BUF_AT(p->scan_levels, uint8_t, level - 1u) != 0;
    if (t->kind == SJS_TOKEN_NAME) {
        if (object) {
            if (s->key == SK_AFTER) return D_EXPRESSION;
            s->key = SK_AFTER;
        }
        return D_NEXT;
    }
    if (t->kind == SJS_TOKEN_STRING || t->kind == SJS_TOKEN_NUMBER) {
        if (!object || s->key != SK_KEY) return D_EXPRESSION;
        s->key = SK_AFTER;
        return D_NEXT;
    }
    if (t->kind != SJS_TOKEN_PUNCTUATOR) return D_EXPRESSION;
    switch (t->punctuator) {
    case SJS_P_LBRACKET:
        if (object && s->key == SK_KEY) {
            s->region = SCAN_KEY;
            s->inner = 0;
            s->closed = BR_OTHER;
            return D_NEXT;
        }
        if (object && s->key != SK_VALUE) return D_EXPRESSION;
        return scan_open(p, 0);
    case SJS_P_LBRACE:
        if (object && s->key != SK_VALUE) return D_EXPRESSION;
        return scan_open(p, 1);
    case SJS_P_RBRACKET:
    case SJS_P_RBRACE:
        if (level == 0 || object != (t->punctuator == SJS_P_RBRACE)) return D_EXPRESSION;
        s->levels--;
        s->key = SK_AFTER;
        return D_NEXT;
    case SJS_P_COMMA:
        if (object) s->key = SK_KEY;
        if (level == 0) {
            s->commas++;
            s->trailing = 1;
        }
        return D_NEXT;
    case SJS_P_COLON:
        if (!object || s->key != SK_AFTER) return D_EXPRESSION;
        s->key = SK_VALUE;
        return D_NEXT;
    case SJS_P_ELLIPSIS:
        if (object) s->key = SK_VALUE;
        return D_NEXT;
    case SJS_P_ASSIGN:
        s->region = SCAN_DEFAULT;
        s->inner = 0;
        s->closed = BR_OTHER;
        return D_NEXT;
    default:
        return D_EXPRESSION;
    }
}

/* After `(`: are these an arrow function's parameters? Then `params` is how
 * many. The scan resumes where it stopped for want of tokens, so it reads
 * each token once. */
static decision decide_paren(sjs_parser *p, const queued_token *paren, uint32_t *params) {
    paren_scan *s = &p->scan;
    if (s->paren != paren->at.offset + 1u) {
        sjs_zero(s, sizeof *s);
        s->paren = paren->at.offset + 1u;
        s->next = 1;
    }
    for (;;) {
        if (s->next > p->limits.max_lookahead) {
            s->paren = 0;
            parse_fail(p, paren, (sjs_problem){ SJS_ERROR_LIMIT, "an arrow function's parameters are longer than max_lookahead" });
            return D_FAILED;
        }
        const queued_token *t = peek(p, s->next);
        if (t == NULL) return D_WAIT;
        decision d;
        if (s->region != SCAN_PATTERN) {
            bracket_seen seen = scan_seen(p);
            if (scan_starts_regex(p, t, &seen)) {
                /* Its token takes the slash's place, and the scan reads it
                 * as it reads a string. */
                if (regex_starts(p, t) == H_STOP) {
                    s->paren = 0;
                    return D_FAILED;
                }
                return D_WAIT;
            }
            s->trailing = 0;
            d = scan_region(p, t);
            if (d == D_AGAIN) continue;
        } else if (s->levels == 0 && is_punctuator(t, SJS_P_RPAREN)) {
            /* `(a, b,)` has two parameters. */
            const queued_token *after = peek(p, s->next + 1u);
            if (after == NULL) return D_WAIT;
            *params = s->any ? s->commas + 1u - s->trailing : 0;
            d = is_punctuator(after, SJS_P_ARROW) ? D_ARROW : D_EXPRESSION;
        } else {
            s->trailing = 0;
            if (s->levels == 0) s->any = 1;
            d = scan_pattern(p, t);
        }
        if (d != D_NEXT) {
            s->paren = 0;
            return d;
        }
        s->next++;
    }
}

/* Reserved words no expression, and no name, starts with. */
static const char *const never_words[] = { "break", "case", "catch", "continue", "debugger", "default", "do",
                                           "else", "enum", "extends", "finally", "for", "if", "instanceof",
                                           "super", "switch", "throw", "try", "var", "while", "with" };
/* Names in a script, reserved in a module, which is strict. */
static const char *const strict_words[] = { "let", "static", "implements", "interface", "package",
                                            "private", "protected", "public", "yield", "await" };


/* How a reserved word would be used as a name. */
enum { USE_PARAM = 0, USE_CONST, USE_REFERENCE };

/* Why a reserved word cannot be bound or read as a name: JavaScript refuses
 * most; some are names JavaScript allows (async anywhere; eval and arguments,
 * read; in a script, which is sloppy, the strict ones) and staticjs leaves out. */
static sjs_problem word_problem(const sjs_parser *p, sjs_str word, uint8_t use) {
    bool module = p->goal == SJS_GOAL_MODULE;
    sjs_problem syntax = { SJS_ERROR_SYNTAX, use == USE_REFERENCE ? "expected an expression" : "this word cannot be bound" };
    sjs_problem unsupported = { SJS_ERROR_UNSUPPORTED, use == USE_REFERENCE ? "staticjs does not have this" : "staticjs does not bind this name" };
    if (sjs_str_equal(word, SJS_STR("async"))) return unsupported;
    if (sjs_str_equal(word, SJS_STR("eval")) || sjs_str_equal(word, SJS_STR("arguments"))) {
        return module && use != USE_REFERENCE ? syntax : unsupported;
    }
    if (module || !word_listed(word, WORD_LIST(strict_words))) return syntax;
    /* `let` is never the name of a lexical declaration. */
    if (use == USE_CONST && sjs_str_equal(word, SJS_STR("let"))) return syntax;
    return unsupported;
}

/* Is the parser inside an arrow function, its parameters or its body? */
static bool in_function(const sjs_parser *p) {
    uint32_t frames = SJS_BUF_COUNT(p->frames, frame);
    for (uint32_t i = 0; i < frames; i++) {
        uint8_t type = SJS_BUF_AT(p->frames, frame, i).type;
        if (type == F_BLOCK || type == F_PARAMS) return true;
    }
    uint32_t ops = SJS_BUF_COUNT(p->ops, pending_op);
    for (uint32_t i = 0; i < ops; i++) {
        if (SJS_BUF_AT(p->ops, pending_op, i).kind == OP_ARROW) return true;
    }
    return false;
}

/* Can an operand start with the name `t`? */
static bool word_starts_operand(sjs_parser *p, const queued_token *t) {
    switch (t->keyword) {
    case SJS_K_CONST:
    case SJS_K_RETURN:
    case SJS_K_EXPORT:
    case SJS_K_IN:
        return false;
    case SJS_K_RESERVED:
        break;
    default:
        return true;
    }
    sjs_str word = token_text(p, t);
    if (word_listed(word, WORD_LIST(never_words))) return false;
    if (p->goal != SJS_GOAL_MODULE) return true;
    if (sjs_str_equal(word, SJS_STR("await"))) return !in_function(p);
    return !word_listed(word, WORD_LIST(strict_words));
}

/* Can an operand (a unary expression) start with `t`? */
static bool starts_operand(sjs_parser *p, const queued_token *t) {
    switch (t->kind) {
    case SJS_TOKEN_NUMBER:
    case SJS_TOKEN_STRING:
    case SJS_TOKEN_TEMPLATE:
    case SJS_TOKEN_TEMPLATE_HEAD:
    case SJS_TOKEN_REGEXP:
        return true;
    case SJS_TOKEN_NAME:
        return word_starts_operand(p, t);
    case SJS_TOKEN_PUNCTUATOR:
        break;
    default:
        return false;
    }
    switch (t->punctuator) {
    case SJS_P_LPAREN:
    case SJS_P_LBRACKET:
    case SJS_P_LBRACE:
    case SJS_P_BANG:
    case SJS_P_TILDE:
    case SJS_P_PLUS:
    case SJS_P_MINUS:
    case SJS_P_PLUS_PLUS:
    case SJS_P_MINUS_MINUS:
    case SJS_P_SLASH:
        return true;
    case SJS_P_ASSIGN_OPERATOR:
        return sjs_str_equal(token_text(p, t), SJS_STR("/="));
    default:
        return false;
    }
}

/* Can a statement start with `t`? A statement staticjs leaves out is
 * unsupported; a token no statement starts with is a syntax error. */
static bool starts_statement(sjs_parser *p, const queued_token *t) {
    static const char *const statements[] = { "debugger", "do", "for", "if", "let", "switch", "throw", "try", "var", "while" };
    if (starts_operand(p, t) || is_punctuator(t, SJS_P_SEMICOLON)) return true;
    if (t->kind != SJS_TOKEN_NAME || t->keyword != SJS_K_RESERVED) return false;
    sjs_str word = token_text(p, t);
    if (word_listed(word, WORD_LIST(statements))) return true;
    return p->goal != SJS_GOAL_MODULE && sjs_str_equal(word, SJS_STR("with"));
}

/* `await` in a module: reserved in its functions, which are never async; at
 * the top level, an await expression, which staticjs leaves out. */
static handled await_word(sjs_parser *p, const queued_token *t) {
    if (in_function(p)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "await is reserved in a module" });
    const queued_token *next = peek(p, 1);
    if (next == NULL) return H_WAIT;
    if (!starts_operand(p, next)) return parse_fail(p, next, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an expression after await" });
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no await" });
}

/* Can what `new` constructs start with `t`? A member expression: an operand,
 * but not one a unary operator starts. */
static bool starts_member(sjs_parser *p, const queued_token *t) {
    if (!starts_operand(p, t)) return false;
    if (t->kind == SJS_TOKEN_PUNCTUATOR) {
        return t->punctuator == SJS_P_LPAREN || t->punctuator == SJS_P_LBRACKET || t->punctuator == SJS_P_LBRACE ||
               t->punctuator == SJS_P_SLASH || t->punctuator == SJS_P_ASSIGN_OPERATOR;
    }
    return t->keyword != SJS_K_TYPEOF && !is_word(p, t, SJS_STR("void")) && !is_word(p, t, SJS_STR("delete")) &&
           !(p->goal == SJS_GOAL_MODULE && is_word(p, t, SJS_STR("await")));
}

/* Does the statement `t` starts go on with `next`? Some tell by their next
 * token: a unary operator, typeof, void and delete need an operand; new what
 * it constructs, or .target; if (, try {, throw a value on its line, var a
 * name or a pattern; in a module, let as var, and await an operand. */
static bool statement_goes_on(sjs_parser *p, const queued_token *t, const queued_token *next) {
    if (t->kind == SJS_TOKEN_PUNCTUATOR) {
        switch (t->punctuator) {
        case SJS_P_BANG:
        case SJS_P_TILDE:
        case SJS_P_PLUS:
        case SJS_P_MINUS:
        case SJS_P_PLUS_PLUS:
        case SJS_P_MINUS_MINUS:
            return starts_operand(p, next);
        default:
            return true;
        }
    }
    if (t->kind != SJS_TOKEN_NAME) return true;
    if (t->keyword == SJS_K_TYPEOF || is_word(p, t, SJS_STR("void")) || is_word(p, t, SJS_STR("delete"))) return starts_operand(p, next);
    if (t->keyword == SJS_K_NEW) return starts_member(p, next) || (p->goal != SJS_GOAL_MODULE && is_punctuator(next, SJS_P_DOT));
    static const char *const parenthesized[] = { "if", "while", "switch", "with" };
    bool module = p->goal == SJS_GOAL_MODULE;
    sjs_str word = token_text(p, t);
    bool binds = next->kind == SJS_TOKEN_NAME || is_punctuator(next, SJS_P_LBRACKET) || is_punctuator(next, SJS_P_LBRACE);
    if (word_listed(word, WORD_LIST(parenthesized))) return is_punctuator(next, SJS_P_LPAREN);
    if (sjs_str_equal(word, SJS_STR("for"))) {
        return is_punctuator(next, SJS_P_LPAREN) || (module && !in_function(p) && is_word(p, next, SJS_STR("await")));
    }
    if (sjs_str_equal(word, SJS_STR("try"))) return is_punctuator(next, SJS_P_LBRACE);
    if (sjs_str_equal(word, SJS_STR("throw"))) return !next->newline_before && starts_operand(p, next);
    if (sjs_str_equal(word, SJS_STR("var"))) return binds;
    if (module && sjs_str_equal(word, SJS_STR("let"))) return binds;
    /* A declaration names what it declares. */
    if (sjs_str_equal(word, SJS_STR("function"))) {
        return is_punctuator(next, SJS_P_STAR) ||
               (next->kind == SJS_TOKEN_NAME &&
                (next->keyword == SJS_K_NONE || word_problem(p, token_text(p, next), USE_CONST).code != SJS_ERROR_SYNTAX));
    }
    if (sjs_str_equal(word, SJS_STR("class"))) return next->kind == SJS_TOKEN_NAME && (next->keyword == SJS_K_NONE || is_word(p, next, SJS_STR("async")));
    if (module && sjs_str_equal(word, SJS_STR("await"))) return starts_operand(p, next);
    return true;
}

/* A statement staticjs leaves out: unsupported, with `message`. A token no
 * statement starts with is a syntax error. */
static handled refuse_statement(sjs_parser *p, const queued_token *t, const char *message) {
    if (!starts_statement(p, t)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a statement" });
    /* A statement a slash begins begins with a regular expression. */
    if (is_slash(p, t)) return regex_starts(p, t);
    if (t->kind == SJS_TOKEN_NAME || t->kind == SJS_TOKEN_PUNCTUATOR) {
        const queued_token *next = peek(p, 1);
        if (next == NULL) return H_WAIT;
        if (!statement_goes_on(p, t, next)) return parse_fail(p, next, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a statement" });
    }
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, message });
}

/* MUTATION: `delete`, a statement of its own (docs/mutation.md, "delete"):
 * DELETE after its operand, a member or an index, when the statement ends
 * with it. As a value it is refused, and read on as a unary operator. In
 * strict code, a module, the operand may not be a name, even in
 * parentheses, as in JavaScript. */
static const sjs_problem delete_value = { SJS_ERROR_UNSUPPORTED, "staticjs has no delete as a value: delete is a statement of its own" };
static const sjs_problem delete_target = { SJS_ERROR_UNSUPPORTED, "staticjs deletes only a property of an object, on a path from a variable" };
static const sjs_problem delete_name = { SJS_ERROR_SYNTAX, "delete of a name is not allowed in strict mode" };

static handled refuse_at(sjs_parser *p, sjs_position at, sjs_problem problem);

static handled delete_word(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    bool statement = f->type == F_EXPRESSION_STATEMENT && f->state == ES_EXPRESSION && top_op(p) == NULL;
    pending_op op = unary(p, SJS_OP_NOT, t->at);
    op.kind = OP_DELETE;
    op.op = statement ? 1u : 0u;
    if (statement) {
        f->state = ES_DELETE;
    } else if (refuse(p, t, delete_value) == H_STOP) {
        return H_STOP;
    }
    return push_op(p, op);
}

/* The operand of `delete` has ended. */
static handled delete_end(sjs_parser *p, const pending_op *op) {
    bool name = p->last_name;
    uint8_t operand = p->last_operand;
    p->last_operand = LO_OTHER;
    p->last_name = false;
    if (name && p->goal == SJS_GOAL_MODULE) {
        queued_token at = { 0 };
        at.at = op->at;
        return parse_fail(p, &at, delete_name);
    }
    if (op->op == 0) return H_TAKEN; /* refused as a value already */
    if (!p->statement_ends) return refuse_at(p, op->at, delete_value);
    if (name || operand != LO_SIMPLE) return refuse_at(p, op->at, delete_target);
    return send_event(p, counted(SJS_EV_DELETE, 0, op_place(op)));
}

/* A reserved word where an operand belongs. JavaScript has expressions that
 * start with some (this, function, class, void, ...), and none that start
 * with the others; nor, in a module, with the strict ones. Before `=>`, it
 * is a parameter's name. */
static handled unsupported_word(sjs_parser *p, const queued_token *t) {
    static const char *const starters[] = { "this", "function", "class", "void" };
    sjs_str word = token_text(p, t);
    if (word_listed(word, WORD_LIST(never_words))) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an expression" });
    const queued_token *next = peek(p, 1);
    if (next == NULL) return H_WAIT;
    if (is_punctuator(next, SJS_P_ARROW)) return parse_fail(p, t, word_problem(p, word, USE_PARAM));
    if (p->goal == SJS_GOAL_MODULE && sjs_str_equal(word, SJS_STR("await"))) return await_word(p, t);
    /* MUTATION: restore "delete" to the starters refused below. */
    if (sjs_str_equal(word, SJS_STR("delete"))) return delete_word(p, t);
    if (sjs_str_equal(word, SJS_STR("this"))) {
        /* Refused, and read on as an operand no pattern takes: `[this] = x`. */
        if (refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs does not have this" }) == H_STOP) return H_STOP;
        p->expect = E_OPERATOR;
        p->last_operand = LO_OTHER;
        return H_TAKEN;
    }
    if (word_listed(word, WORD_LIST(starters))) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs does not have this" });
    return parse_fail(p, t, word_problem(p, word, USE_REFERENCE));
}

/* The reserved word `word` where a name is bound. */
static handled refuse_binding(sjs_parser *p, const queued_token *t, sjs_str word) {
    uint8_t use = USE_CONST;
    if (p->goal != SJS_GOAL_MODULE) {
        /* A parameter's pattern, or a const's. */
        uint32_t n = frame_depth(p);
        while (n > 0 && (SJS_BUF_AT(p->frames, frame, n - 1).type == F_PATTERN_ARRAY ||
                         SJS_BUF_AT(p->frames, frame, n - 1).type == F_PATTERN_OBJECT)) {
            n--;
        }
        if (n > 0 && SJS_BUF_AT(p->frames, frame, n - 1).type == F_PARAMS) use = USE_PARAM;
    }
    return parse_fail(p, t, word_problem(p, word, use));
}

/* `import` where a declaration cannot be: import() and, in a module,
 * import.meta are JavaScript staticjs leaves out; anything else is not
 * JavaScript. */
static handled import_word(sjs_parser *p, const queued_token *t) {
    const queued_token *next = peek(p, 1);
    if (next == NULL) return H_WAIT;
    sjs_problem unsupported = { SJS_ERROR_UNSUPPORTED, "staticjs has no import() or import.meta" };
    if (is_punctuator(next, SJS_P_LPAREN)) return parse_fail(p, t, unsupported);
    if (is_punctuator(next, SJS_P_DOT)) {
        const queued_token *meta = peek(p, 2);
        if (meta == NULL) return H_WAIT;
        if (p->goal == SJS_GOAL_MODULE && is_word(p, meta, SJS_STR("meta"))) return parse_fail(p, t, unsupported);
    }
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "import declares only at the top level of a module" });
}

static handled operand_name(sjs_parser *p, const queued_token *t) {
    switch (t->keyword) {
    case SJS_K_TRUE:
        p->expect = E_OPERATOR;
        return send_kind(p, SJS_EV_TRUE, own_place(p, t->at)) == H_STOP ? H_STOP : H_TAKEN;
    case SJS_K_FALSE:
        p->expect = E_OPERATOR;
        return send_kind(p, SJS_EV_FALSE, own_place(p, t->at)) == H_STOP ? H_STOP : H_TAKEN;
    case SJS_K_NULL:
        p->expect = E_OPERATOR;
        return send_kind(p, SJS_EV_NULL, own_place(p, t->at)) == H_STOP ? H_STOP : H_TAKEN;
    case SJS_K_TYPEOF:
        return push_op(p, unary(p, SJS_OP_TYPEOF, t->at));
    case SJS_K_NEW:
        return push_frame(p, F_NEW, own_place(p, t->at));
    case SJS_K_IMPORT:
        return import_word(p, t);
    case SJS_K_CONST:
    case SJS_K_RETURN:
    case SJS_K_EXPORT:
    case SJS_K_IN:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an expression" });
    case SJS_K_NONE:
        break;
    default:
        return unsupported_word(p, t);
    }
    const queued_token *next = peek(p, 1);
    if (next == NULL) return H_WAIT;
    if (is_punctuator(next, SJS_P_ARROW)) {
        if (next->newline_before) return parse_fail(p, next, (sjs_problem){ SJS_ERROR_SYNTAX, "=> must be on the line of its parameters" });
        if (!arrow_allowed(p)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "an arrow function cannot be an operand here" });
        sjs_str name = token_text(p, t);
        event_place arrow = own_place(p, t->at);
        if (send_event(p, counted(SJS_EV_ARROW_BEGIN, 1, arrow)) == H_STOP) return H_STOP;
        if (send_event(p, counted(SJS_EV_PARAM, 0, arrow)) == H_STOP) return H_STOP;
        if (send_event(p, texted(SJS_EV_BIND, name, own_place(p, t->at))) == H_STOP) return H_STOP;
        arrow.at = next->at;
        if (send_kind(p, SJS_EV_PARAMS_END, arrow) == H_STOP) return H_STOP;
        p->arrow_node = arrow.node;
        p->arrow_start = arrow.start;
        /* Consume the name and the arrow; the body comes next. */
        p->head++;
        p->expect = E_OPERAND;
        p->arrow_body_next = true;
        p->params_nonsimple = false;
        return H_TAKEN;
    }
    p->expect = E_OPERATOR;
    return send_event(p, texted(SJS_EV_NAME, token_text(p, t), own_place(p, t->at))) == H_STOP ? H_STOP : H_TAKEN;
}

/* MUTATION: ++ and --, as statements of their own. */
static const sjs_problem update_value = { SJS_ERROR_UNSUPPORTED, "staticjs has no ++ or -- as a value: each is a statement of its own" };
static const sjs_problem update_target = { SJS_ERROR_UNSUPPORTED, "staticjs assigns only to a name, a property or an element" };
static const sjs_problem update_syntax = { SJS_ERROR_SYNTAX, "the operand of ++ or -- cannot be assigned to" };

/* JavaScript staticjs leaves out, at `at`, read on as refuse() does. */
static handled refuse_at(sjs_parser *p, sjs_position at, sjs_problem problem) {
    if (p->refusing) return H_TAKEN;
    sjs_error_set(&p->refusal, at, problem);
    p->refusing = true;
    return H_TAKEN;
}

/* The prefix ++ or -- of a statement, popped: UPDATE, when the statement
 * ends with its operand; anything after the operand makes it a value. */
static handled prefix_update(sjs_parser *p, const pending_op *op) {
    if (!p->statement_ends) return refuse_at(p, op->at, update_value);
    if (p->last_operand == LO_CALL) return refuse_at(p, op->at, update_target);
    if (p->last_operand != LO_SIMPLE) {
        queued_token at = { 0 };
        at.at = op->at;
        return parse_fail(p, &at, update_syntax);
    }
    return send_event(p, counted(SJS_EV_UPDATE, op->op, op_place(op)));
}

/* `x++` or `x--`: UPDATE, when it is the whole statement. */
static handled postfix_update(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    uint8_t left = p->last_operand;
    if (left != LO_SIMPLE && left != LO_CALL) return parse_fail(p, t, update_syntax);
    if (f->type != F_EXPRESSION_STATEMENT || f->state != ES_EXPRESSION || top_op(p) != NULL) return parse_fail(p, t, update_value);
    const queued_token *next = peek(p, 1);
    if (next == NULL) return H_WAIT;
    bool ends = is_punctuator(next, SJS_P_SEMICOLON) || is_punctuator(next, SJS_P_RBRACE) || next->kind == SJS_TOKEN_END ||
                next->newline_before;
    p->expect = E_OPERATOR;
    f->state = ES_UPDATE;
    if (left == LO_CALL || !ends) {
        p->last_operand = LO_OTHER;
        return refuse(p, t, left == LO_CALL ? update_target : update_value);
    }
    uint8_t op = t->punctuator == SJS_P_PLUS_PLUS ? SJS_OP_ADD : SJS_OP_SUBTRACT;
    return send_event(p, counted(SJS_EV_UPDATE, op, operand_place(p, t->at)));
}

/* A regular expression literal: its flags and body read and classified
 * (regex.c). Exact, it is a REGEXP event; unsupported, it is refused and
 * read on, as JavaScript staticjs leaves out; a syntax error stops. */
static handled regex_literal(sjs_parser *p, const queued_token *t) {
    sjs_str text = token_text(p, t);
    uint32_t slash = text.length;
    while (slash > 0 && text.ptr[slash - 1u] != '/') slash--;
    sjs_regex_literal literal = { { text.ptr, slash > 0 ? slash - 1u : 0 }, 0 };
    if (!sjs_regex_flags((sjs_str){ text.ptr + slash, text.length - slash }, &literal.flags)) {
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "invalid regular expression flags" });
    }
    sjs_regex_verdict verdict;
    if (sjs_regex_check((sjs_regex_env){ &p->allocator, p->limits }, literal, &verdict) != SJS_OK) return fail_memory(p);
    switch (verdict.outcome) {
    case SJS_REGEX_SYNTAX:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, verdict.message });
    case SJS_REGEX_LIMIT:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_LIMIT, verdict.message });
    case SJS_REGEX_UNSUPPORTED:
        return refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, verdict.message });
    default: {
        sjs_event e = texted(SJS_EV_REGEXP, literal.body, own_place(p, t->at));
        e.count = literal.flags;
        return send(p, &e);
    }
    }
}

static handled operand(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (t->kind) {
    case SJS_TOKEN_NUMBER: {
        sjs_event e = number_event(p, t, own_place(p, t->at));
        p->expect = E_OPERATOR;
        return send(p, &e);
    }
    case SJS_TOKEN_STRING:
    case SJS_TOKEN_TEMPLATE:
        p->expect = E_OPERATOR;
        return send_event(p, texted(SJS_EV_STRING, token_text(p, t), own_place(p, t->at)));
    case SJS_TOKEN_TEMPLATE_HEAD: {
        event_place head = own_place(p, t->at);
        if (send_kind(p, SJS_EV_TEMPLATE_BEGIN, head) == H_STOP) return H_STOP;
        if (send_event(p, texted(SJS_EV_STRING, token_text(p, t), head)) == H_STOP) return H_STOP;
        if (push_frame(p, F_TEMPLATE, head) == H_STOP) return H_STOP;
        top_frame(p)->count = 1;
        p->expect = E_OPERAND;
        return H_TAKEN;
    }
    case SJS_TOKEN_NAME:
        return operand_name(p, t);
    case SJS_TOKEN_REGEXP:
        p->expect = E_OPERATOR;
        return regex_literal(p, t);
    case SJS_TOKEN_PUNCTUATOR:
        break;
    default:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an expression" });
    }
    switch (t->punctuator) {
    case SJS_P_LPAREN: {
        uint32_t params = 0;
        decision d = decide_paren(p, t, &params);
        if (d == D_WAIT) return H_WAIT;
        if (d == D_FAILED) return H_STOP;
        if (d == D_ARROW) {
            if (!arrow_allowed(p)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "an arrow function cannot be an operand here" });
            event_place arrow = own_place(p, t->at);
            if (send_event(p, counted(SJS_EV_ARROW_BEGIN, params, arrow)) == H_STOP) return H_STOP;
            return push_frame(p, F_PARAMS, arrow);
        }
        if (push_frame(p, F_PAREN, no_node(t->at)) == H_STOP) return H_STOP;
        p->expect = E_OPERAND;
        return H_TAKEN;
    }
    case SJS_P_LBRACKET: {
        event_place array = own_place(p, t->at);
        if (send_kind(p, SJS_EV_ARRAY_BEGIN, array) == H_STOP) return H_STOP;
        if (push_frame(p, F_ARRAY, array) == H_STOP) return H_STOP;
        p->expect = E_OPERAND;
        return H_TAKEN;
    }
    case SJS_P_LBRACE: {
        event_place object = own_place(p, t->at);
        if (send_kind(p, SJS_EV_OBJECT_BEGIN, object) == H_STOP) return H_STOP;
        return push_frame(p, F_OBJECT, object);
    }
    case SJS_P_BANG:
        return push_op(p, unary(p, SJS_OP_NOT, t->at));
    case SJS_P_MINUS:
        return push_op(p, unary(p, SJS_OP_NEGATE, t->at));
    case SJS_P_PLUS:
        return push_op(p, unary(p, SJS_OP_PLUS, t->at));
    case SJS_P_ELLIPSIS:
        if ((f->type == F_ARRAY || f->type == F_CALL) && !f->spread && top_op(p) == NULL) {
            f->spread = 1;
            return H_TAKEN;
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "... spreads only into an array, a call or an object" });
    case SJS_P_SLASH:
    case SJS_P_ASSIGN_OPERATOR:
        if (is_slash(p, t)) return regex_starts(p, t);
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an expression" });
    case SJS_P_PLUS_PLUS:
    case SJS_P_MINUS_MINUS:
        /* MUTATION: a statement's prefix ++ or --, which prefix_update sends
         * when the statement ends. Restore the refusal alone, "staticjs has
         * no ++ or --: values do not change". */
        if (f->type == F_EXPRESSION_STATEMENT && f->state == ES_EXPRESSION && top_op(p) == NULL) {
            pending_op update = unary(p, t->punctuator == SJS_P_PLUS_PLUS ? SJS_OP_ADD : SJS_OP_SUBTRACT, t->at);
            update.kind = OP_UPDATE;
            f->state = ES_PREFIX;
            return push_op(p, update);
        }
        return parse_fail(p, t, update_value);
    case SJS_P_TILDE: {
        /* Refused, and read on as a unary operator. */
        if (refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no bitwise operators" }) == H_STOP) return H_STOP;
        return push_op(p, unary(p, SJS_OP_NOT, t->at));
    }
    default:
        /* `#name` and `@` too: outside a class, which staticjs has none of, and a decorator are not JavaScript. */
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an expression" });
    }
}

/* ------------------------------------------------------------------------
 * Expressions: operators
 */

static bool is_terminator(const queued_token *t) {
    if (t->kind == SJS_TOKEN_END || t->kind == SJS_TOKEN_TEMPLATE_MIDDLE || t->kind == SJS_TOKEN_TEMPLATE_TAIL) return true;
    if (t->kind != SJS_TOKEN_PUNCTUATOR) return false;
    switch (t->punctuator) {
    case SJS_P_RPAREN:
    case SJS_P_RBRACKET:
    case SJS_P_RBRACE:
    case SJS_P_COMMA:
    case SJS_P_COLON:
    case SJS_P_SEMICOLON:
        return true;
    default:
        return false;
    }
}

/* An infix operator and how tightly it binds. */
static pending_op infix(uint8_t op, uint8_t precedence) {
    pending_op pending = { OP_BINARY, op, precedence, 0, { 0, 0, 0 }, 0, { 0, 0, 0 } };
    return pending;
}

static handled parse_binary(sjs_parser *p, const queued_token *t, pending_op pending) {
    if (end_chain(p) == H_STOP) return H_STOP;
    if (pop_ops(p, pending.precedence) == H_STOP) return H_STOP;
    event_place place = operand_place(p, t->at);
    pending.at = place.at;
    pending.node = place.node;
    pending.start = place.start;
    p->expect = E_OPERAND;
    return push_op(p, pending);
}

static handled parse_logical(sjs_parser *p, const queued_token *t, pending_op pending) {
    uint8_t op = pending.op;
    uint8_t precedence = pending.precedence;
    if (end_chain(p) == H_STOP) return H_STOP;
    if (pop_ops(p, precedence) == H_STOP) return H_STOP;
    frame *f = top_frame(p);
    f->segment |= op == SJS_OP_COALESCE ? SEG_COALESCE : SEG_LOGICAL;
    if (f->segment == (SEG_LOGICAL | SEG_COALESCE)) {
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "?? cannot be mixed with && or || without parentheses" });
    }
    event_place place = operand_place(p, t->at);
    if (send_event(p, counted(SJS_EV_LOGICAL, op, place)) == H_STOP) return H_STOP;
    pending.kind = OP_LOGICAL;
    pending.at = place.at;
    pending.node = place.node;
    pending.start = place.start;
    p->expect = E_OPERAND;
    return push_op(p, pending);
}

/* `.name` after a value. */
static handled member(sjs_parser *p, const queued_token *dot) {
    const queued_token *name = peek(p, 1);
    if (name == NULL) return H_WAIT;
    if (name->kind != SJS_TOKEN_NAME) {
        return parse_fail(p, name, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a property name after ." });
    }
    SJS_UNUSED(dot);
    if (send_event(p, texted(SJS_EV_MEMBER, token_text(p, name), operand_place(p, name->at))) == H_STOP) return H_STOP;
    p->head++;
    return H_TAKEN;
}

static handled open_call(sjs_parser *p, const queued_token *t) {
    event_place call = operand_place(p, t->at);
    if (send_kind(p, SJS_EV_CALL_BEGIN, call) == H_STOP) return H_STOP;
    if (push_frame(p, F_CALL, call) == H_STOP) return H_STOP;
    p->expect = E_OPERAND;
    return H_TAKEN;
}

static handled open_index(sjs_parser *p, const queued_token *t) {
    if (push_frame(p, F_INDEX, operand_place(p, t->at)) == H_STOP) return H_STOP;
    p->expect = E_OPERAND;
    return H_TAKEN;
}

static handled parse_optional(sjs_parser *p, const queued_token *t) {
    const queued_token *next = peek(p, 1);
    if (next == NULL) return H_WAIT;
    frame *f = top_frame(p);
    if (!f->chain) f->chain_start = p->last_start;
    f->chain = 1;
    if (send_event(p, counted(SJS_EV_OPTIONAL, frame_depth(p), operand_place(p, t->at))) == H_STOP) return H_STOP;
    if (next->kind == SJS_TOKEN_NAME) {
        if (send_event(p, texted(SJS_EV_MEMBER, token_text(p, next), operand_place(p, next->at))) == H_STOP) return H_STOP;
        p->head++;
        return H_TAKEN;
    }
    if (is_punctuator(next, SJS_P_LBRACKET)) {
        p->head++;
        return open_index(p, next);
    }
    if (is_punctuator(next, SJS_P_LPAREN)) {
        p->head++;
        return open_call(p, next);
    }
    return parse_fail(p, next, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name, [ or ( after ?." });
}

/* Can the item of an array or object literal just read be a target, were
 * the literal an assignment pattern? A simple target or a pattern. */
static bool assignable_item(const sjs_parser *p) {
    return p->last_operand == LO_SIMPLE || p->last_operand == LO_PATTERN || p->last_operand == LO_DEFAULTED;
}

static handled close_object(sjs_parser *p, const queued_token *t) {
    bool pattern = !top_frame(p)->not_pattern;
    event_place place = frame_place(top_frame(p), t->at);
    pop_frame(p);
    p->expect = E_OPERATOR;
    if (send_kind(p, SJS_EV_OBJECT_END, place) == H_STOP) return H_STOP;
    if (pattern) p->last_operand = LO_PATTERN;
    return H_TAKEN;
}

/* MUTATION: `target = value`, or `target op= value`, as a statement: ASSIGN,
 * whose count is the operator (0 for `=`), the value, and at the
 * statement's end ASSIGN_END (pop_op). An operator staticjs does not have
 * is refused (H_TAKEN), as its binary operator is; H_AGAIN: the caller
 * refuses it. */
static handled assignment_statement(sjs_parser *p, const queued_token *t) {
    static const struct { const char *text; uint8_t op; } operators[] = {
        { "=", 0 }, { "+=", SJS_OP_ADD }, { "-=", SJS_OP_SUBTRACT }, { "*=", SJS_OP_MULTIPLY },
        { "/=", SJS_OP_DIVIDE }, { "%=", SJS_OP_REMAINDER }, { "&&=", SJS_OP_AND }, { "||=", SJS_OP_OR },
        { "?\?=", SJS_OP_COALESCE }, { "**=", SJS_OP_EXPONENT },
    };
    sjs_str text = token_text(p, t);
    if (t->punctuator == SJS_P_ASSIGN) text = SJS_STR("=");
    uint8_t op = UINT8_MAX;
    for (uint32_t i = 0; i < sizeof operators / sizeof operators[0]; i++) {
        if (sjs_str_equal(text, sjs_cstr(operators[i].text))) op = operators[i].op;
    }
    if (op == UINT8_MAX) {
        if (refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no bitwise operators" }) == H_STOP) {
            return H_STOP;
        }
        return H_AGAIN;
    }
    frame *f = top_frame(p);
    event_place place = operand_place(p, t->at);
    if (send_event(p, counted(SJS_EV_ASSIGN, op, place)) == H_STOP) return H_STOP;
    pending_op store = { OP_STORE, op, PRECEDENCE_ELSE, f->segment, place.at, place.node, place.start };
    f->segment = 0;
    f->state = ES_ASSIGN;
    p->expect = E_OPERAND;
    return push_op(p, store);
}

/* `=` or an operator such as `+=` after an operand: JavaScript that staticjs
 * leaves out when the operand can be assigned to there, and not JavaScript
 * when it cannot. A refusal: the parser reads the value on, as an
 * operator, for a syntax error after it (`a = 1 = 2`, `([a.b = 1]) => 0`). */
static handled assignment(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    bool position = takes_assignment(top_op(p));
    bool plain = t->punctuator == SJS_P_ASSIGN;
    sjs_str text = token_text(p, t);
    bool logical = sjs_str_equal(text, SJS_STR("&&=")) || sjs_str_equal(text, SJS_STR("||=")) || sjs_str_equal(text, SJS_STR("?\?="));
    uint8_t left = p->last_operand;
    bool target = left == LO_SIMPLE || (left == LO_CALL && !logical) || (left == LO_PATTERN && plain);
    if (!position || !target) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "the left side of this assignment cannot be assigned to" });
    /* MUTATION: an assignment that is a statement of its own is sent; any
     * other is refused. Restore the one refusal, "staticjs has no
     * assignment: values do not change". */
    bool statement = f->type == F_EXPRESSION_STATEMENT && f->state == ES_EXPRESSION && top_op(p) == NULL;
    if (statement && left == LO_SIMPLE) {
        handled h = assignment_statement(p, t);
        if (h != H_AGAIN) return h;
    }
    const char *refused = statement ? "staticjs assigns only to a name, a property or an element"
                                    : "staticjs has no assignment as a value: an assignment is a statement of its own";
    if (refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, refused }) == H_STOP) return H_STOP;
    if (statement) f->state = ES_ASSIGN; /* the statement's refusal is this one */
    uint8_t defaulted = plain && (left == LO_SIMPLE || left == LO_PATTERN) ? 1u : 0u;
    pending_op op = { OP_ASSIGN, defaulted, PRECEDENCE_ELSE, f->segment, t->at, 0, t->at };
    f->segment = 0;
    p->expect = E_OPERAND;
    return push_op(p, op);
}

/* `**`: right-associative, so `a ** b ** c` is `a ** (b ** c)`: an earlier
 * `**` waits for the later one. `-a ** 2` is not JavaScript: a unary
 * operand needs parentheses. */
static handled parse_power(sjs_parser *p, const queued_token *t) {
    if (end_chain(p) == H_STOP) return H_STOP;
    if (top_op(p) != NULL && top_op(p)->kind == OP_UNARY) {
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "** cannot follow a unary operator without parentheses" });
    }
    if (pop_ops(p, PRECEDENCE_EXPONENT + 1u) == H_STOP) return H_STOP;
    pending_op pending = infix(SJS_OP_EXPONENT, PRECEDENCE_EXPONENT);
    event_place place = operand_place(p, t->at);
    pending.at = place.at;
    pending.node = place.node;
    pending.start = place.start;
    p->expect = E_OPERAND;
    return push_op(p, pending);
}

/* A binary operator staticjs leaves out (`problem`): a refusal, read on as
 * the operator JavaScript reads. */
static handled refused_binary(sjs_parser *p, const queued_token *t, sjs_problem problem) {
    uint8_t precedence;
    switch (t->punctuator) {
    case SJS_P_EQUAL:
    case SJS_P_NOT_EQUAL:
        precedence = PRECEDENCE_EQUALITY;
        break;
    case SJS_P_SHIFT_LEFT:
    case SJS_P_SHIFT_RIGHT:
    case SJS_P_SHIFT_RIGHT_UNSIGNED:
        precedence = PRECEDENCE_SHIFT;
        break;
    case SJS_P_AMPERSAND:
        precedence = PRECEDENCE_BITWISE_AND;
        break;
    case SJS_P_CARET:
        precedence = PRECEDENCE_BITWISE_XOR;
        break;
    case SJS_P_PIPE:
        precedence = PRECEDENCE_BITWISE_OR;
        break;
    default: /* instanceof */
        precedence = PRECEDENCE_RELATIONAL;
        break;
    }
    if (refuse(p, t, problem) == H_STOP) return H_STOP;
    return parse_binary(p, t, infix(SJS_OP_ADD, precedence));
}

static handled operator_token(sjs_parser *p, const queued_token *t) {
    if (t->kind == SJS_TOKEN_NAME) {
        if (t->keyword == SJS_K_IN) return parse_binary(p, t, infix(SJS_OP_IN, PRECEDENCE_RELATIONAL));
        if (sjs_str_equal(token_text(p, t), SJS_STR("instanceof"))) {
            return refused_binary(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no instanceof" });
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an operator" });
    }
    if (t->kind == SJS_TOKEN_TEMPLATE || t->kind == SJS_TOKEN_TEMPLATE_HEAD) {
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no tagged templates" });
    }
    if (t->kind != SJS_TOKEN_PUNCTUATOR) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an operator" });
    switch (t->punctuator) {
    case SJS_P_DOT:
        return member(p, t);
    case SJS_P_OPTIONAL:
        return parse_optional(p, t);
    case SJS_P_LBRACKET:
        return open_index(p, t);
    case SJS_P_LPAREN:
        return open_call(p, t);
    case SJS_P_PLUS:
        return parse_binary(p, t, infix(SJS_OP_ADD, PRECEDENCE_ADDITIVE));
    case SJS_P_MINUS:
        return parse_binary(p, t, infix(SJS_OP_SUBTRACT, PRECEDENCE_ADDITIVE));
    case SJS_P_STAR:
        return parse_binary(p, t, infix(SJS_OP_MULTIPLY, PRECEDENCE_MULTIPLICATIVE));
    case SJS_P_SLASH:
        return parse_binary(p, t, infix(SJS_OP_DIVIDE, PRECEDENCE_MULTIPLICATIVE));
    case SJS_P_PERCENT:
        return parse_binary(p, t, infix(SJS_OP_REMAINDER, PRECEDENCE_MULTIPLICATIVE));
    case SJS_P_LESS:
        return parse_binary(p, t, infix(SJS_OP_LESS, PRECEDENCE_RELATIONAL));
    case SJS_P_LESS_EQUAL:
        return parse_binary(p, t, infix(SJS_OP_LESS_EQUAL, PRECEDENCE_RELATIONAL));
    case SJS_P_GREATER:
        return parse_binary(p, t, infix(SJS_OP_GREATER, PRECEDENCE_RELATIONAL));
    case SJS_P_GREATER_EQUAL:
        return parse_binary(p, t, infix(SJS_OP_GREATER_EQUAL, PRECEDENCE_RELATIONAL));
    case SJS_P_STRICT_EQUAL:
        return parse_binary(p, t, infix(SJS_OP_STRICT_EQUAL, PRECEDENCE_EQUALITY));
    case SJS_P_STRICT_NOT_EQUAL:
        return parse_binary(p, t, infix(SJS_OP_STRICT_NOT_EQUAL, PRECEDENCE_EQUALITY));
    case SJS_P_AND:
        return parse_logical(p, t, infix(SJS_OP_AND, PRECEDENCE_AND));
    case SJS_P_OR:
        return parse_logical(p, t, infix(SJS_OP_OR, PRECEDENCE_OR));
    case SJS_P_COALESCE:
        return parse_logical(p, t, infix(SJS_OP_COALESCE, PRECEDENCE_COALESCE));
    case SJS_P_QUESTION: {
        if (end_chain(p) == H_STOP) return H_STOP;
        if (pop_ops(p, PRECEDENCE_ELSE + 1u) == H_STOP) return H_STOP;
        event_place place = operand_place(p, t->at);
        if (send_kind(p, SJS_EV_CONDITIONAL, place) == H_STOP) return H_STOP;
        if (push_frame(p, F_CONDITIONAL, place) == H_STOP) return H_STOP;
        p->expect = E_OPERAND;
        return H_TAKEN;
    }
    case SJS_P_EQUAL:
    case SJS_P_NOT_EQUAL:
        return refused_binary(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs compares with === and !== only" });
    case SJS_P_ASSIGN:
    case SJS_P_ASSIGN_OPERATOR:
        return assignment(p, t);
    case SJS_P_PLUS_PLUS:
    case SJS_P_MINUS_MINUS:
        /* MUTATION: restore the refusal, "staticjs has no ++ or --: values do not change". */
        return postfix_update(p, t);
    case SJS_P_STAR_STAR:
        return parse_power(p, t);
    case SJS_P_SHIFT_LEFT:
    case SJS_P_SHIFT_RIGHT:
    case SJS_P_SHIFT_RIGHT_UNSIGNED:
    case SJS_P_AMPERSAND:
    case SJS_P_PIPE:
    case SJS_P_CARET:
        return refused_binary(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no bitwise operators" });
    case SJS_P_ARROW:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "=> follows a name or a parameter list" });
    default:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an operator" });
    }
}

/* ------------------------------------------------------------------------
 * Expression frames: what ends them
 */

/* Close an element of an array or an argument list. */
static handled end_item(sjs_parser *p, uint8_t plain, uint8_t spread) {
    frame *f = top_frame(p);
    uint8_t kind = f->spread ? spread : plain;
    f->spread = 0;
    /* The next element or argument is an expression of its own: `??` there
     * may follow `||` here. */
    f->segment = 0;
    f->count++;
    return send_kind(p, kind, frame_place(f, f->at));
}

static handled list_terminator(sjs_parser *p, const queued_token *t, uint8_t closer) {
    frame *f = top_frame(p);
    bool empty = p->expect == E_OPERAND;
    bool is_array = f->type == F_ARRAY;
    if (empty && f->spread) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "... needs a value" });
    /* `[!]`: an operator waits for its operand. */
    if (empty && top_op(p) != NULL) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an expression" });
    if (is_punctuator(t, SJS_P_COMMA)) {
        if (empty) {
            if (is_array) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs arrays have no holes" });
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an argument" });
        }
        if (end_expression(p) == H_STOP) return H_STOP;
        /* A rest element is the last, with no comma after it. */
        if (is_array && (top_frame(p)->spread || !assignable_item(p))) top_frame(p)->not_pattern = 1;
        if (end_item(p, is_array ? SJS_EV_ELEMENT : SJS_EV_ARGUMENT,
                     is_array ? SJS_EV_SPREAD_ELEMENT : SJS_EV_SPREAD_ARGUMENT) == H_STOP) {
            return H_STOP;
        }
        top_frame(p)->after_comma = 1;
        p->expect = E_OPERAND;
        return H_TAKEN;
    }
    if (!is_punctuator(t, closer)) {
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, is_array ? "expected , or ]" : "expected , or )" });
    }
    if (empty && f->count > 0 && !f->after_comma) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a value" });
    if (!empty) {
        if (end_expression(p) == H_STOP) return H_STOP;
        /* `[...a = 1]`: a rest element has no default value. */
        if (is_array && (!assignable_item(p) || (top_frame(p)->spread && p->last_operand == LO_DEFAULTED))) {
            top_frame(p)->not_pattern = 1;
        }
        if (end_item(p, is_array ? SJS_EV_ELEMENT : SJS_EV_ARGUMENT,
                     is_array ? SJS_EV_SPREAD_ELEMENT : SJS_EV_SPREAD_ARGUMENT) == H_STOP) {
            return H_STOP;
        }
    }
    f = top_frame(p);
    uint8_t kind = is_array ? SJS_EV_ARRAY_END : (f->is_new ? SJS_EV_NEW_END : SJS_EV_CALL_END);
    uint32_t count = f->count;
    event_place place = frame_place(f, f->at);
    bool pattern = !f->not_pattern;
    pop_frame(p);
    p->expect = E_OPERATOR;
    if (send_event(p, counted(kind, count, place)) == H_STOP) return H_STOP;
    if (is_array && pattern) p->last_operand = LO_PATTERN;
    return H_TAKEN;
}

static handled end_initializer(sjs_parser *p, const queued_token *t);
static handled statement_done(sjs_parser *p);
static handled let_declarator(sjs_parser *p, uint32_t i, uint8_t *bare); /* MUTATION */

/* MUTATION: an expression statement ends at `t`. A call is sent, for the
 * compiler to accept if it changes a variable; anything else is refused, as
 * it can have no effect, from where the statement began. That is before any
 * refusal inside it, which it replaces; a refusal before it stands. */
static const sjs_problem expression_statement = { SJS_ERROR_UNSUPPORTED, "an expression statement has no effect in staticjs" };

static handled end_expression_statement(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    event_place place = frame_place(f, t->at);
    sjs_position at = f->at;
    bool refused_before = f->refused_before != 0;
    bool call = p->last_operand == LO_CALL;
    uint8_t state = f->state;
    pop_frame(p);
    if (state != ES_EXPRESSION) return H_TAKEN; /* an assignment or an update, sent already */
    if (call) return send_event(p, counted(SJS_EV_EXPRESSION_STATEMENT, 0, place));
    if (!refused_before) {
        sjs_error_set(&p->refusal, at, expression_statement);
        p->refusing = true;
    }
    return H_TAKEN;
}

static handled finish_const(sjs_parser *p) {
    frame *f = top_frame(p);
    if (settle_held(p, f->held_base, f->held_mid) == H_STOP) return H_STOP;
    return send_kind(p, SJS_EV_CONST_END, frame_place(top_frame(p), top_frame(p)->at));
}

/* `a, b` where an expression may be: a refusal, read on. */
static handled comma_operator(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no comma operator" }) == H_STOP) return H_STOP;
    f->after_comma = 1;
    f->segment = 0;
    p->expect = E_OPERAND;
    return H_TAKEN;
}

static handled terminator(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    bool empty = p->expect == E_OPERAND;
    switch (f->type) {
    case F_ARRAY:
        return list_terminator(p, t, SJS_P_RBRACKET);
    case F_CALL:
        return list_terminator(p, t, SJS_P_RPAREN);
    default:
        break;
    }
    if (empty) {
        if (f->type == F_RETURN && top_op(p) == NULL && (is_punctuator(t, SJS_P_SEMICOLON) || is_punctuator(t, SJS_P_RBRACE))) {
            /* `return;` returns undefined. */
            event_place place = frame_place(f, t->at);
            pop_frame(p);
            if (send_event(p, counted(SJS_EV_RETURN, 0, place)) == H_STOP) return H_STOP;
            if (statement_done(p) == H_STOP) return H_STOP;
            return is_punctuator(t, SJS_P_SEMICOLON) ? H_TAKEN : H_AGAIN;
        }
        if (f->type == F_PAREN && is_punctuator(t, SJS_P_RPAREN)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "() needs an expression or =>" });
        }
        if (t->kind == SJS_TOKEN_END) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "the source ends inside an expression" });
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an expression" });
    }
    p->statement_ends = f->type == F_EXPRESSION_STATEMENT; /* MUTATION: prefix_update */
    handled ended = end_expression(p);
    p->statement_ends = false;
    if (ended == H_STOP) return H_STOP;
    f = top_frame(p);
    /* Where JavaScript reads an Expression, not an AssignmentExpression. */
    bool sequence = f->type == F_EXPRESSION_GOAL || f->type == F_PAREN || f->type == F_INDEX || f->type == F_TEMPLATE ||
                    f->type == F_RETURN || f->type == F_IF || f->type == F_EXPRESSION_STATEMENT;
    if (sequence && is_punctuator(t, SJS_P_COMMA)) return comma_operator(p, t);
    switch (f->type) {
    case F_EXPRESSION_GOAL:
        if (t->kind != SJS_TOKEN_END) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected the end of the expression" });
        if (p->refusing) return report_refusal(p);
        p->done = true;
        return send_kind(p, SJS_EV_END, program_end(t->at)) == H_STOP ? H_STOP : H_TAKEN;
    case F_PAREN: {
        if (!is_punctuator(t, SJS_P_RPAREN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected )" });
        bool comma = f->after_comma != 0;
        pop_frame(p);
        p->expect = E_OPERATOR;
        /* `(a) = 1` is an assignment; `([a]) = 1`, `[(a = 1)] = x` and
         * `(a, b) = 1` are not JavaScript. */
        if (comma || p->last_operand == LO_PATTERN || p->last_operand == LO_DEFAULTED) p->last_operand = LO_OTHER;
        return H_TAKEN;
    }
    case F_INDEX: {
        if (!is_punctuator(t, SJS_P_RBRACKET)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ]" });
        event_place place = frame_place(f, t->at);
        pop_frame(p);
        p->expect = E_OPERATOR;
        return send_kind(p, SJS_EV_INDEX, place);
    }
    case F_TEMPLATE:
        if (t->kind == SJS_TOKEN_TEMPLATE_MIDDLE) {
            f->count += 2;
            f->segment = 0;
            p->expect = E_OPERAND;
            return send_event(p, texted(SJS_EV_STRING, token_text(p, t), frame_place(f, t->at)));
        }
        if (t->kind == SJS_TOKEN_TEMPLATE_TAIL) {
            uint32_t count = f->count + 2;
            event_place place = frame_place(f, t->at);
            pop_frame(p);
            p->expect = E_OPERATOR;
            if (send_event(p, texted(SJS_EV_STRING, token_text(p, t), place)) == H_STOP) return H_STOP;
            return send_event(p, counted(SJS_EV_TEMPLATE_END, count, place));
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected } to close ${" });
    case F_CONDITIONAL: {
        if (!is_punctuator(t, SJS_P_COLON)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected : of ?:" });
        sjs_position at = f->at;
        event_place place = frame_place(f, t->at);
        pop_frame(p);
        if (send_kind(p, SJS_EV_CONDITIONAL_ELSE, place) == H_STOP) return H_STOP;
        frame *parent = top_frame(p);
        pending_op op = { OP_ELSE, 0, PRECEDENCE_ELSE, parent->segment, at, place.node, place.start };
        parent->segment = 0;
        p->expect = E_OPERAND;
        return push_op(p, op);
    }
    case F_OBJECT: {
        if (!is_punctuator(t, SJS_P_COMMA) && !is_punctuator(t, SJS_P_RBRACE)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected , or }" });
        }
        /* A rest property is a simple target, the last, with no comma after it. */
        bool target = f->spread ? p->last_operand == LO_SIMPLE && !is_punctuator(t, SJS_P_COMMA) : assignable_item(p);
        if (!target) f->not_pattern = 1;
        uint8_t kind = f->spread ? SJS_EV_SPREAD_PROPERTY : SJS_EV_PROPERTY;
        f->spread = 0;
        f->segment = 0;
        f->count++;
        if (send_kind(p, kind, frame_place(f, t->at)) == H_STOP) return H_STOP;
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->state = O_KEY;
            f->after_comma = 1;
            return H_TAKEN;
        }
        return close_object(p, t);
    }
    case F_COMPUTED_KEY:
        if (!is_punctuator(t, SJS_P_RBRACKET)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ]" });
        pop_frame(p);
        if (top_frame(p)->type == F_PATTERN_OBJECT) {
            top_frame(p)->state = PO_COLON;
            return send_kind(p, SJS_EV_PATTERN_COMPUTED_KEY, frame_place(top_frame(p), t->at));
        }
        top_frame(p)->state = O_COLON;
        return send_kind(p, SJS_EV_COMPUTED_KEY, frame_place(top_frame(p), t->at));
    case F_INITIALIZER:
        return end_initializer(p, t);
    case F_CONST:
        if (is_punctuator(t, SJS_P_SEMICOLON) || is_punctuator(t, SJS_P_COMMA)) {
            f->segment = 0;
            /* MUTATION: whether a let's next declarator has a value. */
            uint8_t bare = 0;
            if (is_punctuator(t, SJS_P_COMMA) && f->declares_let) {
                handled h = let_declarator(p, 1, &bare);
                if (h != H_TAKEN) return h;
            }
            if (finish_const(p) == H_STOP) return H_STOP;
            if (is_punctuator(t, SJS_P_COMMA)) {
                /* `export const a = 1, b = 2` exports both; each is a node,
                 * from the statement's `const`. */
                if (f->exported) {
                    event_place exported = { t->at, p->next_node++, p->export_token };
                    if (send_kind(p, SJS_EV_EXPORT, exported) == H_STOP) return H_STOP;
                }
                f->node = p->next_node++;
                /* MUTATION: and the count; restore send_kind(p, SJS_EV_CONST_BEGIN, ...). */
                f->bare = bare;
                uint32_t count = f->declares_let ? (bare ? 2u : 1u) : 0u;
                if (send_event(p, counted(SJS_EV_CONST_BEGIN, count, frame_place(f, t->at))) == H_STOP) return H_STOP;
                top_frame(p)->state = C_PATTERN;
                hold_from_here(p);
                return H_TAKEN;
            }
            pop_frame(p);
            return statement_done(p);
        }
        if (is_punctuator(t, SJS_P_RBRACE) || t->kind == SJS_TOKEN_END) {
            /* A semicolon is inserted before } and at the end. */
            if (finish_const(p) == H_STOP) return H_STOP;
            pop_frame(p);
            return statement_done(p) == H_STOP ? H_STOP : H_AGAIN;
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after a const declaration" });
    case F_RETURN: {
        if (!is_punctuator(t, SJS_P_SEMICOLON) && !is_punctuator(t, SJS_P_RBRACE)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after return" });
        }
        event_place place = frame_place(f, t->at);
        pop_frame(p);
        if (send_event(p, counted(SJS_EV_RETURN, 1, place)) == H_STOP) return H_STOP;
        if (statement_done(p) == H_STOP) return H_STOP;
        return is_punctuator(t, SJS_P_SEMICOLON) ? H_TAKEN : H_AGAIN;
    }
    case F_EXPRESSION_STATEMENT:
        if (!is_punctuator(t, SJS_P_SEMICOLON) && !is_punctuator(t, SJS_P_RBRACE) && t->kind != SJS_TOKEN_END) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after an expression" });
        }
        /* MUTATION: restore `pop_frame(p);` (refused when it began). */
        if (end_expression_statement(p, t) == H_STOP) return H_STOP;
        if (statement_done(p) == H_STOP) return H_STOP;
        return is_punctuator(t, SJS_P_SEMICOLON) ? H_TAKEN : H_AGAIN;
    case F_IF:
        /* The condition ends. */
        if (!is_punctuator(t, SJS_P_RPAREN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ) after the condition" });
        f->state = IF_CONSEQUENT;
        return send_kind(p, SJS_EV_IF, frame_place(f, t->at));
    case F_DEFAULT: {
        if (is_punctuator(t, SJS_P_COMMA)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "export default takes one expression" });
        if (!is_punctuator(t, SJS_P_SEMICOLON) && t->kind != SJS_TOKEN_END) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after export default" });
        }
        event_place place = frame_place(f, f->at);
        pop_frame(p);
        if (send_kind(p, SJS_EV_EXPORT_DEFAULT, place) == H_STOP) return H_STOP;
        return is_punctuator(t, SJS_P_SEMICOLON) ? H_TAKEN : H_AGAIN;
    }
    default:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "unexpected token" });
    }
}

/* Automatic semicolon insertion at the end of a statement's expression: a
 * line break before a token that cannot continue the expression ends the
 * statement (ECMAScript 12.10). */
/* Can `t` not continue an expression it follows on a new line? */
static bool cannot_continue(sjs_parser *p, const queued_token *t) {
    switch (t->kind) {
    case SJS_TOKEN_NAME:
        return t->keyword != SJS_K_IN && !is_word(p, t, SJS_STR("instanceof"));
    case SJS_TOKEN_NUMBER:
    case SJS_TOKEN_STRING:
        return true;
    case SJS_TOKEN_PUNCTUATOR:
        /* None of these continues an expression; ++ and -- do not after a line break. */
        return t->punctuator == SJS_P_LBRACE || t->punctuator == SJS_P_PLUS_PLUS || t->punctuator == SJS_P_MINUS_MINUS ||
               t->punctuator == SJS_P_BANG || t->punctuator == SJS_P_TILDE;
    default:
        return false;
    }
}

static bool inserts_semicolon(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (!t->newline_before) return false;
    if (!((f->type == F_CONST && f->state == C_INIT) || f->type == F_DEFAULT || f->type == F_RETURN ||
          f->type == F_EXPRESSION_STATEMENT)) {
        return false;
    }
    /* Nothing continues an arrow function with a block body. */
    if (p->expect == E_AFTER_BLOCK_ARROW) return !is_terminator(t);
    if (p->expect != E_OPERATOR) return false;
    return cannot_continue(p, t);
}

/* End the statement as `;` would, then read the token again after it. */
static handled insert_semicolon(sjs_parser *p, const queued_token *t) {
    queued_token semicolon = *t;
    semicolon.kind = SJS_TOKEN_PUNCTUATOR;
    semicolon.punctuator = SJS_P_SEMICOLON;
    semicolon.keyword = SJS_K_NONE;
    semicolon.text_length = 0;
    p->expect = E_OPERATOR;
    handled h = terminator(p, &semicolon);
    return h == H_TAKEN ? H_AGAIN : h;
}

static handled expression_token(sjs_parser *p, const queued_token *t) {
    if (p->arrow_body_next) {
        p->arrow_body_next = false;
        return arrow_body(p, t);
    }
    if (inserts_semicolon(p, t)) return insert_semicolon(p, t);
    switch (p->expect) {
    case E_OPERAND:
        if (is_terminator(t)) return terminator(p, t);
        return operand(p, t);
    case E_OPERATOR:
        if (is_terminator(t)) return terminator(p, t);
        return operator_token(p, t);
    default:
        if (is_terminator(t)) {
            p->expect = E_OPERATOR;
            return terminator(p, t);
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "an arrow function with a block body ends here" });
    }
}

/* ------------------------------------------------------------------------
 * Patterns
 */

/* A pattern is complete: tell the frame that held it. */
static void pattern_done(sjs_parser *p) {
    frame *f = top_frame(p);
    switch (f->type) {
    case F_PATTERN_ARRAY:
        f->state = PA_AFTER;
        break;
    case F_PATTERN_OBJECT:
        f->state = PO_AFTER;
        break;
    case F_CONST:
        /* The value comes next; it goes before the pattern. */
        f->state = C_ASSIGN;
        f->buffered = f->holds;
        f->held_mid = held_count(p);
        break;
    default: /* F_PARAMS */
        f->state = P_AFTER;
        break;
    }
}

/* A parameter's, an element's or a property's target begins: it is held
 * until the token after it says whether a default value goes first. */
static void begin_target(sjs_parser *p) {
    frame *f = top_frame(p);
    f->target_base = held_count(p);
    f->buffered = 1;
    f->in_target = 1;
}

/* The target has no default value: it goes on in the order it came. */
static handled end_target(sjs_parser *p) {
    frame *f = top_frame(p);
    if (!f->in_target) return H_TAKEN;
    f->in_target = 0;
    f->buffered = f->holds;
    return settle_held(p, f->target_base, held_count(p));
}

/* `=` after a target: its default value, whose events go before the
 * target's. */
static handled begin_initializer(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f->rest) {
        const char *message = f->type == F_PARAMS ? "a rest parameter has no default value" : "a rest element has no default value";
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, message });
    }
    if (f->type == F_PARAMS) f->nonsimple = 1;
    uint8_t holds = f->holds;
    if (push_frame(p, F_INITIALIZER, own_place(p, t->at)) == H_STOP) return H_STOP;
    frame *init = top_frame(p);
    init->holds = init->buffered = holds;
    init->held_mid = held_count(p);
    p->expect = E_OPERAND;
    return send_kind(p, SJS_EV_DEFAULT, frame_place(init, t->at));
}

/* The default value ends at `,` or at the pattern's closing bracket, which
 * the pattern then reads. */
static handled end_initializer(sjs_parser *p, const queued_token *t) {
    if (!is_punctuator(t, SJS_P_COMMA) && !is_punctuator(t, SJS_P_RBRACKET) && !is_punctuator(t, SJS_P_RBRACE) &&
        !is_punctuator(t, SJS_P_RPAREN)) {
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "a default value ends with , or the pattern's end" });
    }
    if (send_kind(p, SJS_EV_DEFAULT_END, frame_place(top_frame(p), t->at)) == H_STOP) return H_STOP;
    uint32_t n = frame_depth(p);
    frame *target = &SJS_BUF_AT(p->frames, frame, n - 2);
    if (settle_held(p, target->target_base, top_frame(p)->held_mid) == H_STOP) return H_STOP;
    pop_frame(p);
    frame *f = top_frame(p);
    f->in_target = 0;
    f->buffered = f->holds;
    return H_AGAIN;
}

static handled pattern_start(sjs_parser *p, const queued_token *t) {
    if (t->kind == SJS_TOKEN_NAME) {
        if (t->keyword != SJS_K_NONE) return refuse_binding(p, t, token_text(p, t));
        if (send_event(p, texted(SJS_EV_BIND, token_text(p, t), own_place(p, t->at))) == H_STOP) return H_STOP;
        pattern_done(p);
        return H_TAKEN;
    }
    if (top_frame(p)->type == F_PARAMS) top_frame(p)->nonsimple = 1;
    if (is_punctuator(t, SJS_P_LBRACKET)) {
        if (push_frame(p, F_PATTERN_ARRAY, own_place(p, t->at)) == H_STOP) return H_STOP;
        return send_kind(p, SJS_EV_ARRAY_PATTERN_BEGIN, frame_place(top_frame(p), t->at));
    }
    if (is_punctuator(t, SJS_P_LBRACE)) {
        if (push_frame(p, F_PATTERN_OBJECT, own_place(p, t->at)) == H_STOP) return H_STOP;
        return send_kind(p, SJS_EV_OBJECT_PATTERN_BEGIN, frame_place(top_frame(p), t->at));
    }
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name, [ or {" });
}

/* A target of the top frame, after the event that names its place. */
static handled target_start(sjs_parser *p, const queued_token *t) {
    begin_target(p);
    return pattern_start(p, t);
}

static handled close_pattern(sjs_parser *p, uint8_t kind, const queued_token *t) {
    if (send_kind(p, kind, frame_place(top_frame(p), t->at)) == H_STOP) return H_STOP;
    pop_frame(p);
    pattern_done(p);
    return H_TAKEN;
}

/* The token after a target: `=` begins its default value; any other token
 * ends the target, and the caller reads it. H_AGAIN: the caller goes on. */
static handled after_target(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f->in_target && is_punctuator(t, SJS_P_ASSIGN)) return begin_initializer(p, t);
    if (end_target(p) == H_STOP) return H_STOP;
    if (is_punctuator(t, SJS_P_COMMA) && top_frame(p)->rest) {
        const char *message = f->type == F_PARAMS ? "a rest parameter must be last" : "a rest element must be last";
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, message });
    }
    return H_AGAIN;
}

static handled pattern_array_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case PA_AFTER: {
        handled h = after_target(p, t);
        if (h != H_AGAIN) return h;
        f = top_frame(p);
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->count++;
            f->state = PA_ELEMENT;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_RBRACKET)) return close_pattern(p, SJS_EV_ARRAY_PATTERN_END, t);
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected , or ]" });
    }
    case PA_REST:
        return target_start(p, t);
    default: /* PA_ELEMENT */
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->count++;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_RBRACKET)) return close_pattern(p, SJS_EV_ARRAY_PATTERN_END, t);
        if (is_punctuator(t, SJS_P_ELLIPSIS)) {
            f->rest = 1;
            f->state = PA_REST;
            return send_event(p, counted(SJS_EV_PATTERN_REST, f->count, frame_place(f, t->at)));
        }
        if (send_event(p, counted(SJS_EV_PATTERN_INDEX, f->count, frame_place(f, t->at))) == H_STOP) return H_STOP;
        return target_start(p, t);
    }
}

/* `{ name }` and `{ name = value }`: the name is the key and the target. */
static handled shorthand_property(sjs_parser *p, const queued_token *t) {
    if (p->name_keyword == NAME_STRING) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "this word cannot be bound" });
    if (p->name_keyword != SJS_K_NONE) return refuse_binding(p, t, held_name(p));
    if (send_event(p, texted(SJS_EV_PATTERN_KEY, held_name(p), frame_place(top_frame(p), p->name_at))) == H_STOP) return H_STOP;
    begin_target(p);
    if (send_event(p, texted(SJS_EV_BIND, held_name(p), own_place(p, p->name_at))) == H_STOP) return H_STOP;
    pattern_done(p);
    return H_AGAIN;
}

static handled pattern_key_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (is_punctuator(t, SJS_P_RBRACE)) return close_pattern(p, SJS_EV_OBJECT_PATTERN_END, t);
    if (t->kind == SJS_TOKEN_NAME || t->kind == SJS_TOKEN_STRING) {
        if (hold_name(p, t) == H_STOP) return H_STOP;
        if (t->kind == SJS_TOKEN_STRING) p->name_keyword = NAME_STRING;
        f->state = PO_KEY_NAME;
        return H_TAKEN;
    }
    if (t->kind == SJS_TOKEN_NUMBER && t->bigint == 0) {
        /* The key is the number's text: `{1.50: a}` reads "1.5". */
        char text[32];
        uint32_t length = sjs_number_format(t->number, text);
        f->state = PO_COLON;
        return send_event(p, texted(SJS_EV_PATTERN_KEY, (sjs_str){ text, length }, frame_place(f, t->at)));
    }
    if (t->kind == SJS_TOKEN_NUMBER) {
        /* A decimal BigInt's digits are its key's text: `{10n: a}` reads
         * "10" (a decimal literal has no leading 0). */
        if (t->bigint != 10u) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs reads a BigInt key of a pattern in decimal only" });
        f->state = PO_COLON;
        return send_event(p, texted(SJS_EV_PATTERN_KEY, token_text(p, t), frame_place(f, t->at)));
    }
    if (is_punctuator(t, SJS_P_LBRACKET)) {
        if (push_frame(p, F_COMPUTED_KEY, no_node(t->at)) == H_STOP) return H_STOP;
        p->expect = E_OPERAND;
        return H_TAKEN;
    }
    if (is_punctuator(t, SJS_P_ELLIPSIS)) {
        f->state = PO_REST;
        return H_TAKEN;
    }
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a property name" });
}

static handled pattern_object_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case PO_KEY:
        return pattern_key_token(p, t);
    case PO_KEY_NAME:
        if (is_punctuator(t, SJS_P_COLON)) {
            if (send_event(p, texted(SJS_EV_PATTERN_KEY, held_name(p), frame_place(f, p->name_at))) == H_STOP) return H_STOP;
            /* The next token starts the property's own pattern. */
            f->state = PO_VALUE;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_COMMA) || is_punctuator(t, SJS_P_RBRACE) || is_punctuator(t, SJS_P_ASSIGN)) {
            return shorthand_property(p, t);
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected : , = or }" });
    case PO_COLON:
        if (!is_punctuator(t, SJS_P_COLON)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected : after a property's key" });
        f->state = PO_VALUE;
        return H_TAKEN;
    case PO_VALUE:
        return target_start(p, t);
    case PO_REST:
        if (t->kind != SJS_TOKEN_NAME) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "... in an object pattern takes a name" });
        if (t->keyword != SJS_K_NONE) return refuse_binding(p, t, token_text(p, t));
        f->rest = 1;
        if (send_event(p, counted(SJS_EV_PATTERN_REST, 0, frame_place(f, t->at))) == H_STOP) return H_STOP;
        return target_start(p, t);
    default: { /* PO_AFTER */
        handled h = after_target(p, t);
        if (h != H_AGAIN) return h;
        f = top_frame(p);
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->state = PO_KEY;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_RBRACE)) return close_pattern(p, SJS_EV_OBJECT_PATTERN_END, t);
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected , or }" });
    }
    }
}

/* `)`: the arrow follows, as the lookahead saw. */
static handled end_params(sjs_parser *p) {
    frame *f = top_frame(p);
    f->state = P_END;
    const queued_token *arrow = peek(p, 1);
    if (arrow == NULL) return H_WAIT;
    if (arrow->newline_before) return parse_fail(p, arrow, (sjs_problem){ SJS_ERROR_SYNTAX, "=> must be on the line of its parameters" });
    p->params_nonsimple = f->nonsimple != 0;
    p->arrow_node = f->node;
    p->arrow_start = f->start;
    event_place place = frame_place(f, arrow->at);
    pop_frame(p);
    if (send_kind(p, SJS_EV_PARAMS_END, place) == H_STOP) return H_STOP;
    p->head++;
    p->expect = E_OPERAND;
    p->arrow_body_next = true;
    return H_TAKEN;
}

/* An arrow function's parameter list, after the lookahead chose it. */
static handled params_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case P_END:
        return end_params(p);
    case P_REST:
        return target_start(p, t);
    case P_AFTER: {
        handled h = after_target(p, t);
        if (h != H_AGAIN) return h;
        f = top_frame(p);
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->count++;
            f->state = P_START;
            f->after_comma = 1;
            return H_TAKEN;
        }
        if (!is_punctuator(t, SJS_P_RPAREN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected , or )" });
        return end_params(p);
    }
    default: /* P_START */
        if (is_punctuator(t, SJS_P_RPAREN)) {
            if (f->count > 0 && !f->after_comma) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a parameter" });
            return end_params(p);
        }
        if (is_punctuator(t, SJS_P_ELLIPSIS)) {
            f->rest = 1;
            f->nonsimple = 1;
            f->state = P_REST;
            return send_event(p, counted(SJS_EV_REST_PARAM, f->count, frame_place(f, t->at)));
        }
        if (send_event(p, counted(SJS_EV_PARAM, f->count, frame_place(f, t->at))) == H_STOP) return H_STOP;
        return target_start(p, t);
    }
}

/* ------------------------------------------------------------------------
 * Objects
 */

/* The operator waiting in frame `outer` for the literal that frame `inner`
 * reads; NULL when the literal is a whole operand. */
static const pending_op *waiting_op(const sjs_parser *p, uint32_t inner) {
    const frame *f = &SJS_BUF_AT(p->frames, frame, inner);
    const frame *outer = &SJS_BUF_AT(p->frames, frame, inner - 1u);
    if (f->ops_base <= outer->ops_base) return NULL;
    return &SJS_BUF_AT(p->ops, pending_op, f->ops_base - 1u);
}

static handled cover_assigned(sjs_parser *p, const queued_token *t);

/* `{ a = 1 }` in an object literal: JavaScript reads it only as the cover of
 * an assignment pattern, when the literal, or an array or object literal it
 * is an element of, is assigned to; staticjs has no assignment. Anywhere
 * else it is a syntax error. The tokens after the literal tell which. */
static handled cover_initializer(sjs_parser *p, const queued_token *t) {
    sjs_problem syntax = { SJS_ERROR_SYNTAX, "a shorthand property takes = only in a pattern" };
    uint32_t level = frame_depth(p) - 1u;  /* the literal's frame */
    uint32_t i = 1;                        /* tokens ahead of `=` */
    uint32_t depth = 1;                    /* brackets open in the literal */
    bracket_seen seen = { NULL, NULL, BR_OTHER, BR_OTHER };
    if (!scan_bracket(p, 0, BR_OTHER)) return H_STOP;
    for (;;) {
        for (; depth > 0; i++) {
            if (i > p->limits.max_lookahead) {
                return parse_fail(p, t, (sjs_problem){ SJS_ERROR_LIMIT, "the parser needs to see further ahead than max_lookahead" });
            }
            const queued_token *next = peek(p, i);
            if (next == NULL) return H_WAIT;
            if (next->kind == SJS_TOKEN_END) return parse_fail(p, t, syntax);
            seen.before = i >= 2u ? peek(p, i - 2u) : NULL;
            seen.prev = peek(p, i - 1u);
            seen.open = SJS_BUF_AT(p->scan_brackets, uint8_t, depth - 1u);
            if (scan_starts_regex(p, next, &seen)) return regex_starts(p, next);
            if (scan_opens(next)) {
                if (!scan_bracket(p, depth, bracket_kind(p, next, &seen))) return H_STOP;
                depth++;
            } else if (scan_closes(next)) {
                depth--;
                seen.closed = SJS_BUF_AT(p->scan_brackets, uint8_t, depth);
            }
        }
        const queued_token *after = peek(p, i);
        if (after == NULL) return H_WAIT;
        const pending_op *op = waiting_op(p, level);
        if (is_punctuator(after, SJS_P_ASSIGN)) {
            /* Assigned to, where an assignment can be. */
            if (!takes_assignment(op)) return parse_fail(p, t, syntax);
            return cover_assigned(p, t);
        }
        const frame *outer = &SJS_BUF_AT(p->frames, frame, level - 1u);
        bool element = outer->type == F_ARRAY && !(outer->spread && is_punctuator(after, SJS_P_COMMA));
        bool value = outer->type == F_OBJECT && outer->state == O_VALUE && !outer->spread;
        if (op != NULL || !(element || value)) return parse_fail(p, t, syntax);
        uint8_t closer = outer->type == F_ARRAY ? SJS_P_RBRACKET : SJS_P_RBRACE;
        /* An element of a literal that may be a pattern: read on to its end. */
        if (is_punctuator(after, closer)) {
            i++;
        } else if (is_punctuator(after, SJS_P_COMMA)) {
            i++;
            depth = 1;
        } else {
            return parse_fail(p, t, syntax);
        }
        level--;
    }
}

/* Read past a method staticjs refused (refuse), to the `}` that ends its
 * body, for a syntax error after it: its brackets balanced, as the parser does
 * not read its statements. A `/` may begin a regular expression, whose body
 * the lexer cannot read, so the refusal is reported there. */
static handled skip_token(sjs_parser *p, const queued_token *t) {
    if (t->kind == SJS_TOKEN_END) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "the source ends inside a method" });
    if (is_slash(p, t)) return report_refusal(p);
    if (scan_opens(t)) {
        if (p->skip_depth == 0 && is_punctuator(t, SJS_P_LBRACE)) p->skip = SKIP_BODY;
        if (p->skip_depth >= p->limits.max_nesting) return report_refusal(p);
        p->skip_depth++;
        return H_TAKEN;
    }
    if (!scan_closes(t)) return H_TAKEN;
    if (p->skip_depth == 0) return report_refusal(p);
    p->skip_depth--;
    if (p->skip_depth == 0 && p->skip == SKIP_BODY) {
        /* The method is the property's value. */
        p->skip = SKIP_NONE;
        top_frame(p)->state = O_VALUE;
        p->expect = E_OPERATOR;
        p->last_operand = LO_OTHER;
    }
    return H_TAKEN;
}

/* After the name `get`, `set` or `async`: does `t` begin a method's name? */
static bool method_prefix(sjs_parser *p, const queued_token *t) {
    sjs_str word = held_name(p);
    bool name = t->kind == SJS_TOKEN_NAME || t->kind == SJS_TOKEN_STRING || t->kind == SJS_TOKEN_NUMBER ||
                is_punctuator(t, SJS_P_LBRACKET);
    if (sjs_str_equal(word, SJS_STR("get")) || sjs_str_equal(word, SJS_STR("set"))) return name;
    /* `async` and the method's name on one line; `async *g() {}` too. */
    return sjs_str_equal(word, SJS_STR("async")) && !t->newline_before && (name || is_punctuator(t, SJS_P_STAR));
}

/* A method, getter, setter or generator in an object literal, from `t` on. */
static handled refuse_method(sjs_parser *p, const queued_token *t, const char *message) {
    if (refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, message }) == H_STOP) return H_STOP;
    p->skip = SKIP_HEAD;
    p->skip_depth = 0;
    return skip_token(p, t);
}

/* A shorthand property's name, read as a reference: a reserved word cannot be. */
static handled shorthand_word(sjs_parser *p, const queued_token *t) {
    if (p->name_keyword == SJS_K_NONE) return H_TAKEN;
    sjs_problem problem = word_problem(p, held_name(p), USE_REFERENCE);
    if (problem.code == SJS_ERROR_SYNTAX) problem.message = "this word cannot stand for a value";
    return parse_fail(p, t, problem);
}

/* `{ a = 1 }` assigned to: refused, and read on as `a` and an assignment. */
static handled cover_assigned(sjs_parser *p, const queued_token *t) {
    if (refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no assignment: values do not change" }) == H_STOP) return H_STOP;
    if (shorthand_word(p, t) == H_STOP) return H_STOP;
    if (send_event(p, texted(SJS_EV_KEY, held_name(p), frame_place(top_frame(p), p->name_at))) == H_STOP) return H_STOP;
    if (send_event(p, texted(SJS_EV_NAME, held_name(p), own_place(p, p->name_at))) == H_STOP) return H_STOP;
    top_frame(p)->state = O_VALUE;
    p->expect = E_OPERATOR;
    return H_AGAIN;
}

static handled object_key_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case O_KEY:
        if (is_punctuator(t, SJS_P_RBRACE)) {
            if (f->count > 0 && !f->after_comma) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a property" });
            return close_object(p, t);
        }
        f->after_comma = 0;
        if (t->kind == SJS_TOKEN_NAME) {
            if (hold_name(p, t) == H_STOP) return H_STOP;
            f->state = O_KEY_NAME;
            return H_TAKEN;
        }
        if (t->kind == SJS_TOKEN_STRING) {
            if (sjs_str_equal(token_text(p, t), SJS_STR("__proto__"))) {
                return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs objects have no prototype" });
            }
            f->state = O_COLON;
            return send_event(p, texted(SJS_EV_KEY, token_text(p, t), frame_place(f, t->at)));
        }
        if (t->kind == SJS_TOKEN_NUMBER) {
            sjs_event e = number_event(p, t, own_place(p, t->at));
            if (send(p, &e) == H_STOP) return H_STOP;
            f->state = O_COLON;
            return send_kind(p, SJS_EV_COMPUTED_KEY, frame_place(f, t->at));
        }
        if (is_punctuator(t, SJS_P_LBRACKET)) {
            if (push_frame(p, F_COMPUTED_KEY, no_node(t->at)) == H_STOP) return H_STOP;
            p->expect = E_OPERAND;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_ELLIPSIS)) {
            f->spread = 1;
            f->state = O_VALUE;
            p->expect = E_OPERAND;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_STAR)) return refuse_method(p, t, "staticjs objects have no methods");
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a property" });
    case O_KEY_NAME:
        if (is_punctuator(t, SJS_P_COLON)) {
            if (sjs_str_equal(held_name(p), SJS_STR("__proto__"))) {
                return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs objects have no prototype" });
            }
            if (send_event(p, texted(SJS_EV_KEY, held_name(p), frame_place(f, p->name_at))) == H_STOP) return H_STOP;
            f->state = O_VALUE;
            p->expect = E_OPERAND;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_COMMA) || is_punctuator(t, SJS_P_RBRACE)) {
            if (shorthand_word(p, t) == H_STOP) return H_STOP;
            if (sjs_str_equal(held_name(p), SJS_STR("__proto__"))) {
                return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs objects have no prototype" });
            }
            if (send_event(p, texted(SJS_EV_KEY, held_name(p), frame_place(f, p->name_at))) == H_STOP) return H_STOP;
            if (send_event(p, texted(SJS_EV_NAME, held_name(p), own_place(p, p->name_at))) == H_STOP) return H_STOP;
            f->state = O_VALUE;
            p->expect = E_OPERATOR;
            return H_AGAIN;
        }
        if (is_punctuator(t, SJS_P_LPAREN)) return refuse_method(p, t, "staticjs objects have no methods");
        if (is_punctuator(t, SJS_P_ASSIGN)) return cover_initializer(p, t);
        if (p->name_keyword != NAME_STRING && method_prefix(p, t)) {
            bool async = sjs_str_equal(held_name(p), SJS_STR("async"));
            return refuse_method(p, t, async ? "staticjs objects have no methods" : "staticjs objects have no getters or setters");
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected : after a property name" });
    default: /* O_COLON */
        if (is_punctuator(t, SJS_P_LPAREN)) return refuse_method(p, t, "staticjs objects have no methods");
        if (!is_punctuator(t, SJS_P_COLON)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected :" });
        f->state = O_VALUE;
        p->expect = E_OPERAND;
        return H_TAKEN;
    }
}

/* ------------------------------------------------------------------------
 * Statements
 */

/* The pattern comes before the value in the source and after it in the
 * events, so its events are held until the value has been sent. */
static handled begin_const(sjs_parser *p, const queued_token *t) {
    if (push_frame(p, F_CONST, own_place(p, t->at)) == H_STOP) return H_STOP;
    if (send_kind(p, SJS_EV_CONST_BEGIN, frame_place(top_frame(p), t->at)) == H_STOP) return H_STOP;
    hold_from_here(p);
    return H_TAKEN;
}

/* MUTATION: `let`, a const whose names can be assigned to: CONST_BEGIN's
 * count is 1, or 2 for a declarator without a value (`let x;`), whose
 * value is undefined. That the parser tells from the tokens at `i` and
 * after: a name without `=` after it. */
static handled let_declarator(sjs_parser *p, uint32_t i, uint8_t *bare) {
    const queued_token *name = peek(p, i);
    if (name == NULL) return H_WAIT;
    *bare = 0;
    if (name->kind != SJS_TOKEN_NAME) return H_TAKEN;
    const queued_token *next = peek(p, i + 1u);
    if (next == NULL) return H_WAIT;
    *bare = is_punctuator(next, SJS_P_ASSIGN) ? 0u : 1u;
    return H_TAKEN;
}

static handled begin_let(sjs_parser *p, const queued_token *t) {
    uint8_t bare;
    handled h = let_declarator(p, 1, &bare);
    if (h != H_TAKEN) return h;
    if (push_frame(p, F_CONST, own_place(p, t->at)) == H_STOP) return H_STOP;
    frame *f = top_frame(p);
    f->declares_let = 1;
    f->bare = bare;
    if (send_event(p, counted(SJS_EV_CONST_BEGIN, bare ? 2u : 1u, frame_place(f, t->at))) == H_STOP) return H_STOP;
    hold_from_here(p);
    return H_TAKEN;
}

/* After the name of a let declarator without a value: the declarator
 * ends, at a `;`, a `,`, a `}`, the end or a line break. */
static handled end_bare_let(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    f->state = C_INIT;
    f->ops_base = SJS_BUF_COUNT(p->ops, pending_op);
    p->expect = E_OPERATOR;
    if (is_punctuator(t, SJS_P_SEMICOLON) || is_punctuator(t, SJS_P_COMMA) || is_punctuator(t, SJS_P_RBRACE) || t->kind == SJS_TOKEN_END) {
        return terminator(p, t);
    }
    if (t->newline_before) return insert_semicolon(p, t);
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after a let declaration" });
}

static handled const_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f->state == C_PATTERN) return pattern_start(p, t);
    /* C_ASSIGN */
    if (f->bare) return end_bare_let(p, t); /* MUTATION */
    if (!is_punctuator(t, SJS_P_ASSIGN)) {
        /* MUTATION: let's message; restore the const's alone. */
        const char *message = f->declares_let ? "a let declaration of a pattern needs = and a value" : "a const declaration needs = and a value";
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, message });
    }
    f->state = C_INIT;
    f->ops_base = SJS_BUF_COUNT(p->ops, pending_op);
    p->expect = E_OPERAND;
    return H_TAKEN;
}

/* A block's first statement is a string: it begins the directive prologue,
 * strings each alone in a statement. JavaScript refuses "use strict" there,
 * unescaped, when the function's parameters are not all plain names; any
 * other string statement is one staticjs leaves out. H_TAKEN: no such
 * directive. */
static handled strict_directive(sjs_parser *p) {
    for (uint32_t i = 0;;) {
        const queued_token *t = peek(p, i);
        if (t == NULL) return H_WAIT;
        if (t->kind != SJS_TOKEN_STRING) return H_TAKEN;
        const queued_token *next = peek(p, i + 1u);
        if (next == NULL) return H_WAIT;
        bool ends = is_punctuator(next, SJS_P_SEMICOLON) || is_punctuator(next, SJS_P_RBRACE) || next->kind == SJS_TOKEN_END ||
                    (next->newline_before && cannot_continue(p, next));
        if (!ends) return H_TAKEN;
        if (!t->escaped && sjs_str_equal(token_text(p, t), SJS_STR("use strict"))) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "\"use strict\" cannot be in a function whose parameters are not plain names" });
        }
        if (!is_punctuator(next, SJS_P_SEMICOLON)) {
            if (!next->newline_before) return H_TAKEN;
            i += 1u;
        } else {
            i += 2u;
        }
    }
}

/* The if on top ends, with its chain: one IF_END. */
static handled end_if_chain(sjs_parser *p) {
    if (send_kind(p, SJS_EV_IF_END, frame_place(top_frame(p), top_frame(p)->at)) == H_STOP) return H_STOP;
    pop_frame(p);
    return H_TAKEN;
}

/* A statement has ended. The labels it had end; the if whose consequent it
 * was waits for an else; the if whose alternative it was ends, and with it,
 * perhaps, the if whose branch that if was. */
static handled statement_done(sjs_parser *p) {
    for (;;) {
        frame *f = top_frame(p);
        if (f != NULL && f->type == F_LABEL) {
            p->labels.length = f->held_base;
            pop_frame(p);
            continue;
        }
        if (f == NULL || f->type != F_IF) return H_TAKEN;
        if (f->state == IF_CONSEQUENT) {
            f->state = IF_AFTER_CONSEQUENT;
            return H_TAKEN;
        }
        if (f->state != IF_ALTERNATIVE) return H_TAKEN;
        if (end_if_chain(p) == H_STOP) return H_STOP;
    }
}

/* `return`. A line break after it ends it, as JavaScript inserts a semicolon
 * there: the function returns undefined, and the next line is a statement
 * of its own. A value there would be an expression statement, which is
 * refused, as a hint that it belongs on the line of its return. */
static handled return_token(sjs_parser *p, const queued_token *t) {
    if (!in_function(p)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "return is only in a function" });
    const queued_token *next = peek(p, 1);
    if (next == NULL) return H_WAIT;
    if (!next->newline_before || is_punctuator(next, SJS_P_SEMICOLON) || is_punctuator(next, SJS_P_RBRACE)) {
        if (push_frame(p, F_RETURN, own_place(p, t->at)) == H_STOP) return H_STOP;
        p->expect = E_OPERAND;
        return H_TAKEN;
    }
    if (starts_operand(p, next) && !is_punctuator(next, SJS_P_LBRACE)) {
        if (refuse(p, next, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "put the value on the line of its return" }) == H_STOP) return H_STOP;
    }
    if (send_event(p, counted(SJS_EV_RETURN, 0, own_place(p, t->at))) == H_STOP) return H_STOP;
    return statement_done(p);
}

/* What a statement declares (SD_*), from peek(i) on: SD_WAIT until the
 * tokens that tell have arrived. A lexical declaration (const, and let with
 * what it binds), a declaration no statement context takes (class, async
 * function, function*), or a function declaration, which a script takes as
 * the body of an if (Annex B). In a single-statement context (`single`),
 * `let` and a line break are an expression statement, unless `[` follows. */
enum { SD_NONE = 0, SD_WAIT, SD_LEXICAL, SD_DECLARATION, SD_FUNCTION };

static uint8_t declaration_at(sjs_parser *p, uint32_t i, bool single) {
    const queued_token *t = peek(p, i);
    if (t == NULL) return SD_WAIT;
    if (t->kind != SJS_TOKEN_NAME) return SD_NONE;
    if (t->keyword == SJS_K_CONST) return SD_LEXICAL;
    if (t->keyword != SJS_K_RESERVED) return SD_NONE;
    if (is_word(p, t, SJS_STR("class"))) return SD_DECLARATION;
    bool let = is_word(p, t, SJS_STR("let"));
    bool function = is_word(p, t, SJS_STR("function"));
    if (!let && !function && !is_word(p, t, SJS_STR("async"))) return SD_NONE;
    const queued_token *next = peek(p, i + 1u);
    if (next == NULL) return SD_WAIT;
    if (function) return is_punctuator(next, SJS_P_STAR) ? SD_DECLARATION : SD_FUNCTION;
    if (!let) return !next->newline_before && is_word(p, next, SJS_STR("function")) ? SD_DECLARATION : SD_NONE;
    /* `let`: in a module, a reserved word, so what follows is a declaration or nothing. */
    if (p->goal == SJS_GOAL_MODULE || is_punctuator(next, SJS_P_LBRACKET)) return SD_LEXICAL;
    bool binds = is_punctuator(next, SJS_P_LBRACE) ||
                 (next->kind == SJS_TOKEN_NAME && next->keyword != SJS_K_IN && !is_word(p, next, SJS_STR("instanceof")));
    if (!binds || (single && next->newline_before)) return SD_NONE;
    return SD_LEXICAL;
}

static const sjs_problem lexical_single = { SJS_ERROR_SYNTAX, "a lexical declaration cannot be the statement of an if, an else or a label" };
static const sjs_problem declaration_single = { SJS_ERROR_SYNTAX, "a declaration cannot be the statement of an if, an else or a label" };

/* Can the name `t` be a label? A name a reference could be; staticjs has
 * none, but it must tell `l: x` from what is not JavaScript. */
static bool label_word(const sjs_parser *p, const queued_token *t) {
    if (t->kind != SJS_TOKEN_NAME) return false;
    if (t->keyword == SJS_K_NONE || t->keyword == SJS_K_UNSUPPORTED_NAME) return true;
    return t->keyword == SJS_K_RESERVED && word_problem(p, token_text(p, t), USE_REFERENCE).code == SJS_ERROR_UNSUPPORTED;
}

/* Is a label named `t` open around the statement being read, in the same
 * function? JavaScript rejects a label inside one of the same name, and
 * takes `break name` only inside it. */
static bool label_open(sjs_parser *p, const queued_token *t) {
    sjs_str name = token_text(p, t);
    for (uint32_t i = frame_depth(p); i > 0; i--) {
        const frame *f = &SJS_BUF_AT(p->frames, frame, i - 1u);
        if (f->type == F_BLOCK || f->type == F_MODULE) return false;
        if (f->type != F_LABEL) continue;
        sjs_str open = { (const char *)p->labels.ptr + f->held_base, f->count };
        if (sjs_str_equal(open, name)) return true;
    }
    return false;
}

/* Open the label `t`: refused already, and read on to its statement's end. */
static handled open_label(sjs_parser *p, const queued_token *t) {
    if (label_open(p, t)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "a label of this name is already open around it" });
    uint32_t start = p->labels.length;
    if (sjs_buf_append(&p->allocator, &p->labels, (sjs_bytes){ (const uint8_t *)p->queue_text.ptr + t->text_offset, t->text_length }) != SJS_OK) {
        return fail_memory(p);
    }
    if (push_frame(p, F_LABEL, no_node(t->at)) == H_STOP) return H_STOP;
    frame *f = top_frame(p);
    f->held_base = start;
    f->count = t->text_length;
    return H_TAKEN;
}

/* `break name` inside the label `name`: JavaScript, but staticjs has no
 * labels. Anything else after break is a syntax error, as staticjs has no
 * loop or switch for a break to leave. */
static handled break_word(sjs_parser *p, const queued_token *t) {
    const queued_token *label = peek(p, 1);
    if (label == NULL) return H_WAIT;
    if (label->kind != SJS_TOKEN_NAME || label->newline_before || !label_open(p, label)) {
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a statement: break leaves only a loop, a switch or a label it names" });
    }
    const queued_token *after = peek(p, 2);
    if (after == NULL) return H_WAIT;
    bool semicolon = is_punctuator(after, SJS_P_SEMICOLON);
    if (!semicolon && !is_punctuator(after, SJS_P_RBRACE) && after->kind != SJS_TOKEN_END && !after->newline_before) {
        return parse_fail(p, after, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after break" });
    }
    if (refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no labels" }) == H_STOP) return H_STOP;
    p->head += semicolon ? 2u : 1u;
    return statement_done(p);
}

/* `name:` labels a statement. staticjs has no labels, and so nothing to
 * break or continue to: refused, and read on. The statement after the
 * labels is in a single-statement context, but for a function declaration
 * in a script's block (Annex B). */
static handled labelled_statement(sjs_parser *p, const queued_token *t, bool single) {
    uint32_t i = 0;
    for (;;) {
        if (i + 2u > p->limits.max_lookahead) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_LIMIT, "the parser needs to see further ahead than max_lookahead" });
        }
        /* The name first: what follows a token that is not one (a slash
         * that begins a regular expression) is not read ahead. */
        const queued_token *name = peek(p, i);
        if (name == NULL) return H_WAIT;
        if (!label_word(p, name)) break;
        const queued_token *colon = peek(p, i + 1u);
        if (colon == NULL) return H_WAIT;
        if (!is_punctuator(colon, SJS_P_COLON)) break;
        i += 2u;
    }
    uint8_t declares = declaration_at(p, i, true);
    if (declares == SD_WAIT) return H_WAIT;
    const queued_token *item = peek(p, i);
    if (declares == SD_LEXICAL) return parse_fail(p, item, lexical_single);
    if (declares == SD_DECLARATION || (declares == SD_FUNCTION && (single || p->goal == SJS_GOAL_MODULE))) {
        return parse_fail(p, item, declaration_single);
    }
    if (refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no labels" }) == H_STOP) return H_STOP;
    for (uint32_t k = 0; k < i; k += 2u) {
        if (open_label(p, peek(p, k)) == H_STOP) return H_STOP;
    }
    /* The labels and their colons are read; the statement is read under
     * them as a single statement (a function declaration there, which the
     * checks above let through, is refused as one). */
    p->head += i - 1u;
    return H_TAKEN;
}

/* Why staticjs leaves out the statement `t` begins. */
static const char *statement_problem(const sjs_parser *p, const queued_token *t) {
    static const struct { const char *word; const char *message; } problems[] = {
        { "for", "staticjs has no loops: use map, filter, reduce or some" },
        { "while", "staticjs has no loops: use map, filter, reduce or some" },
        { "do", "staticjs has no loops: use map, filter, reduce or some" },
        { "switch", "staticjs has no switch: use if or ?:" },
        { "throw", "staticjs has no exceptions" },
        { "try", "staticjs has no exceptions" },
        { "let", "staticjs declares with const" },
        { "var", "staticjs declares with const" },
        { "function", "staticjs has no function or class declarations: use const and =>" },
        { "class", "staticjs has no function or class declarations: use const and =>" },
        { "async", "staticjs has no function or class declarations: use const and =>" },
    };
    for (uint32_t i = 0; i < sizeof problems / sizeof problems[0]; i++) {
        if (is_word(p, t, sjs_cstr(problems[i].word))) return problems[i].message;
    }
    return "staticjs does not have this statement";
}

/* The start of a statement in a function's body, a block or a branch of an
 * if (`single`: a single-statement context, which takes no declaration). */
static handled statement_token(sjs_parser *p, const queued_token *t, bool single) {
    if (is_punctuator(t, SJS_P_SEMICOLON)) return statement_done(p);
    if (is_punctuator(t, SJS_P_LBRACE)) {
        if (push_frame(p, F_INNER_BLOCK, own_place(p, t->at)) == H_STOP) return H_STOP;
        return send_kind(p, SJS_EV_BLOCK_STATEMENT_BEGIN, frame_place(top_frame(p), t->at));
    }
    if (t->kind == SJS_TOKEN_NAME) {
        switch (t->keyword) {
        case SJS_K_CONST:
            if (single) return parse_fail(p, t, lexical_single);
            return begin_const(p, t);
        case SJS_K_RETURN:
            return return_token(p, t);
        case SJS_K_IMPORT:
            return import_word(p, t);
        case SJS_K_EXPORT:
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "export declares only at the top level of a module" });
        case SJS_K_RESERVED:
            if (is_word(p, t, SJS_STR("if"))) return push_frame(p, F_IF, own_place(p, t->at));
            break;
        default:
            break;
        }
    }
    uint8_t declares = declaration_at(p, 0, single);
    if (declares == SD_WAIT) return H_WAIT;
    if (declares != SD_NONE) {
        /* MUTATION: let declares; restore its refusal below. */
        if (!single && declares == SD_LEXICAL && is_word(p, t, SJS_STR("let"))) {
            const queued_token *next = peek(p, 1);
            if (next == NULL) return H_WAIT;
            if (statement_goes_on(p, t, next)) return begin_let(p, t);
        }
        if (!single) return refuse_statement(p, t, statement_problem(p, t));
        if (declares == SD_LEXICAL) return parse_fail(p, t, lexical_single);
        if (declares == SD_DECLARATION || p->goal == SJS_GOAL_MODULE) return parse_fail(p, t, declaration_single);
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs does not have this: declare a function with const and =>" });
    }
    if (label_word(p, t)) {
        const queued_token *next = peek(p, 1);
        if (next == NULL) return H_WAIT;
        if (is_punctuator(next, SJS_P_COLON)) return labelled_statement(p, t, single);
    }
    if (t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_RESERVED && is_word(p, t, SJS_STR("break"))) return break_word(p, t);
    if (!starts_operand(p, t)) return refuse_statement(p, t, statement_problem(p, t));
    /* An expression statement. MUTATION: its events are sent, and at its
     * end it is refused unless its expression, as a whole, is a call, which
     * the compiler accepts if it changes a variable (end_expression_statement).
     * Restore: refuse here, as `refuse(p, t, expression_statement)`, and push
     * the frame with no_node(t->at). A slash begins a regular expression,
     * read before the token after it is looked at. */
    if (is_slash(p, t)) return regex_starts(p, t);
    if (t->kind == SJS_TOKEN_NAME || t->kind == SJS_TOKEN_PUNCTUATOR) {
        const queued_token *next = peek(p, 1);
        if (next == NULL) return H_WAIT;
        if (!statement_goes_on(p, t, next)) return parse_fail(p, next, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a statement" });
    }
    bool refusing = p->refusing;
    if (push_frame(p, F_EXPRESSION_STATEMENT, own_place(p, t->at)) == H_STOP) return H_STOP;
    top_frame(p)->refused_before = refusing ? 1u : 0u;
    p->expect = E_OPERAND;
    return H_AGAIN;
}

/* Can `t` not continue an if's condition, after an operand? Then the
 * condition lacks its `)`. */
static bool missing_close_paren(sjs_parser *p, const queued_token *t) {
    if (t->kind == SJS_TOKEN_NUMBER || t->kind == SJS_TOKEN_STRING || is_punctuator(t, SJS_P_LBRACE)) return true;
    return t->kind == SJS_TOKEN_NAME && t->keyword != SJS_K_IN && !is_word(p, t, SJS_STR("instanceof"));
}

/* An if, and an else-if chain in the same frame: `else if` opens the next
 * condition, so a chain of any length nests once. */
static handled if_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case IF_OPEN:
        if (!is_punctuator(t, SJS_P_LPAREN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ( after if" });
        f->state = IF_TEST;
        f->segment = 0;
        f->chain = 0;
        f->after_comma = 0;
        f->ops_base = SJS_BUF_COUNT(p->ops, pending_op);
        p->expect = E_OPERAND;
        return H_TAKEN;
    case IF_TEST:
        if (p->expect == E_OPERATOR && !p->arrow_body_next && missing_close_paren(p, t)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ) after the condition" });
        }
        return expression_token(p, t);
    case IF_AFTER_CONSEQUENT: {
        if (t->keyword == SJS_K_RESERVED && is_word(p, t, SJS_STR("else"))) {
            const queued_token *next = peek(p, 1);
            if (next == NULL) return H_WAIT;
            if (next->keyword == SJS_K_RESERVED && is_word(p, next, SJS_STR("if"))) {
                if (send_event(p, counted(SJS_EV_ELSE, 1, frame_place(f, t->at))) == H_STOP) return H_STOP;
                top_frame(p)->state = IF_OPEN;
                p->head++;
                return H_TAKEN;
            }
            if (send_event(p, counted(SJS_EV_ELSE, 0, frame_place(f, t->at))) == H_STOP) return H_STOP;
            top_frame(p)->state = IF_ALTERNATIVE;
            return H_TAKEN;
        }
        if (end_if_chain(p) == H_STOP) return H_STOP;
        return statement_done(p) == H_STOP ? H_STOP : H_AGAIN;
    }
    default: /* IF_CONSEQUENT, IF_ALTERNATIVE */
        return statement_token(p, t, true);
    }
}

/* A function's block body, or a block statement in it. */
static handled block_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f->type == F_BLOCK && f->count == 0 && f->nonsimple && t->kind == SJS_TOKEN_STRING) {
        handled h = strict_directive(p);
        if (h != H_TAKEN) return h;
    }
    f->count = 1;
    if (t->kind == SJS_TOKEN_END) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "the source ends early, inside a block" });
    if (!is_punctuator(t, SJS_P_RBRACE)) return statement_token(p, t, false);
    event_place place = frame_place(f, t->at);
    sjs_position at = f->at;
    bool body = f->type == F_BLOCK;
    pop_frame(p);
    if (!body) {
        if (send_kind(p, SJS_EV_BLOCK_STATEMENT_END, place) == H_STOP) return H_STOP;
        return statement_done(p);
    }
    if (send_kind(p, SJS_EV_BLOCK_END, place) == H_STOP) return H_STOP;
    place.at = at;
    if (send_kind(p, SJS_EV_ARROW_END, place) == H_STOP) return H_STOP;
    p->expect = E_AFTER_BLOCK_ARROW;
    return H_TAKEN;
}

/* An import or export statement. Its specifiers are held until the
 * statement's end says what they are. */
static handled push_statement(sjs_parser *p, uint8_t type, event_place place) {
    if (push_frame(p, type, place) == H_STOP) return H_STOP;
    frame *f = top_frame(p);
    f->held_base = held_count(p);
    return H_TAKEN;
}

/* Hold the specifier `alias as name` of the top statement. */
static handled hold_specifier(sjs_parser *p, const sjs_event *spec, sjs_str name) {
    sjs_event e = *spec;
    e.text = name;
    return hold_event(p, &e);
}

static sjs_event specifier(uint32_t count, sjs_str alias, sjs_position at) {
    sjs_event e = counted(0, count, no_node(at));
    e.alias = alias;
    return e;
}

/* Send the top statement's specifiers as events of `kind`, each with the
 * module's name, and forget them. An import or re-export that holds none
 * still names a module that must exist. */
static handled send_specifiers(sjs_parser *p, uint8_t kind, sjs_str module) {
    frame *f = top_frame(p);
    uint32_t n = SJS_BUF_COUNT(p->held, held_event);
    if (n == f->held_base && kind != SJS_EV_EXPORT_NAME) {
        event_place place = { f->at, p->next_node++, f->start };
        sjs_event e = counted(kind, SJS_IMPORT_MODULE, place);
        e.text = module;
        return send(p, &e);
    }
    for (uint32_t i = f->held_base; i < n; i++) {
        held_event h = SJS_BUF_AT(p->held, held_event, i);
        event_place place = { h.at, p->next_node++, f->start };
        sjs_event e = counted(kind, h.count, place);
        e.text = module;
        e.alias = (sjs_str){ (const char *)p->held_text.ptr + h.alias_offset, h.alias_length };
        e.name = (sjs_str){ (const char *)p->held_text.ptr + h.text_offset, h.text_length };
        if (kind != SJS_EV_IMPORT && sjs_str_equal(e.name, SJS_STR("default"))) p->default_exported = true;
        if (send(p, &e) == H_STOP) return H_STOP;
    }
    drop_held(p, f->held_base);
    return H_TAKEN;
}

static bool ends_statement(const queued_token *t) {
    return is_punctuator(t, SJS_P_SEMICOLON) || t->kind == SJS_TOKEN_END || t->newline_before;
}

/* `;`, a line break or the end ends an import or export statement. */
static handled statement_end(sjs_parser *p, const queued_token *t) {
    if (!ends_statement(t)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after an import or export" });
    pop_frame(p);
    return is_punctuator(t, SJS_P_SEMICOLON) ? H_TAKEN : H_AGAIN;
}

/* After the module's name (held): import attributes, or the statement's end,
 * which sends the specifiers as events of `kind`. */
static handled source_end(sjs_parser *p, const queued_token *t, uint8_t kind) {
    frame *f = top_frame(p);
    if (is_word(p, t, SJS_STR("with"))) {
        /* Even after a line break: the statement cannot end before it. */
        f->state = WA_OPEN;
        f->count = SJS_BUF_COUNT(p->held, held_event);
        return H_TAKEN;
    }
    if (!ends_statement(t)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after an import or export" });
    if (send_specifiers(p, kind, held_name(p)) == H_STOP) return H_STOP;
    return statement_end(p, t);
}

/* `with { key: "value", ... }`: JavaScript staticjs leaves out, once it is
 * read to its end. Its keys are held to find one given twice. */
static handled attributes_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case WA_OPEN:
        if (!is_punctuator(t, SJS_P_LBRACE)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected { after with" });
        f->state = WA_KEY;
        return H_TAKEN;
    case WA_KEY: {
        if (is_punctuator(t, SJS_P_RBRACE)) {
            f->state = WA_END;
            return H_TAKEN;
        }
        if (t->kind != SJS_TOKEN_NAME && t->kind != SJS_TOKEN_STRING) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an import attribute's key" });
        }
        sjs_str key = token_text(p, t);
        uint32_t n = SJS_BUF_COUNT(p->held, held_event);
        for (uint32_t i = f->count; i < n; i++) {
            const held_event *h = &SJS_BUF_AT(p->held, held_event, i);
            if (sjs_str_equal((sjs_str){ (const char *)p->held_text.ptr + h->text_offset, h->text_length }, key)) {
                return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "an import attribute's key is given twice" });
            }
        }
        if (hold_event(p, &(sjs_event){ .text = key, .at = t->at }) == H_STOP) return H_STOP;
        f->state = WA_COLON;
        return H_TAKEN;
    }
    case WA_COLON:
        if (!is_punctuator(t, SJS_P_COLON)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected : after an import attribute's key" });
        f->state = WA_VALUE;
        return H_TAKEN;
    case WA_VALUE:
        if (t->kind != SJS_TOKEN_STRING) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "an import attribute's value is a string" });
        f->state = WA_AFTER;
        return H_TAKEN;
    case WA_AFTER:
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->state = WA_KEY;
            return H_TAKEN;
        }
        if (!is_punctuator(t, SJS_P_RBRACE)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected , or }" });
        f->state = WA_END;
        return H_TAKEN;
    default: /* WA_END */
        if (!ends_statement(t)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after an import or export" });
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no import attributes" });
    }
}

static handled expect_from(sjs_parser *p, const queued_token *t) {
    if (!is_word(p, t, SJS_STR("from"))) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected from" });
    top_frame(p)->state = top_frame(p)->type == F_IMPORT ? I_SOURCE : X_SOURCE;
    return H_TAKEN;
}

static handled import_name_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f->state == I_NAME) {
        if (is_punctuator(t, SJS_P_RBRACE)) {
            f->state = I_FROM;
            return H_TAKEN;
        }
        if (t->kind != SJS_TOKEN_NAME && t->kind != SJS_TOKEN_STRING) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name to import" });
        }
        if (hold_name(p, t) == H_STOP) return H_STOP;
        if (t->kind == SJS_TOKEN_STRING) {
            /* `import { "a" as b }` needs its `as`. */
            p->name_keyword = NAME_STRING;
            f->names |= SPEC_STRING_LOCAL;
        }
        f->state = I_AS;
        return H_TAKEN;
    }
    if (f->state == I_AS && is_word(p, t, SJS_STR("as"))) {
        f->state = I_ALIAS;
        return H_TAKEN;
    }
    sjs_event spec = specifier(SJS_IMPORT_NAME, held_name(p), p->name_at);
    if (f->state == I_ALIAS) {
        if (t->kind != SJS_TOKEN_NAME) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name to import as" });
        if (t->keyword != SJS_K_NONE) return refuse_binding(p, t, token_text(p, t));
        if (hold_specifier(p, &spec, token_text(p, t)) == H_STOP) return H_STOP;
        f->state = I_AFTER;
        return H_TAKEN;
    }
    if (p->name_keyword == NAME_STRING) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "this word cannot be bound" });
    if (p->name_keyword != SJS_K_NONE) return refuse_binding(p, t, held_name(p));
    if (hold_specifier(p, &spec, held_name(p)) == H_STOP) return H_STOP;
    f->state = I_AFTER;
    return H_AGAIN;
}

/* `{` of named imports or `*` of a namespace. */
static handled import_braces(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (is_punctuator(t, SJS_P_LBRACE)) {
        f->state = I_NAME;
        return H_TAKEN;
    }
    if (is_punctuator(t, SJS_P_STAR)) {
        f->state = I_STAR;
        return H_TAKEN;
    }
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name, { or * to import" });
}

static handled import_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case I_OPEN:
        if (t->kind == SJS_TOKEN_STRING) {
            /* import "m"; */
            if (hold_name(p, t) == H_STOP) return H_STOP;
            f->state = I_END;
            return H_TAKEN;
        }
        if (t->kind == SJS_TOKEN_NAME) {
            if (t->keyword != SJS_K_NONE) return refuse_binding(p, t, token_text(p, t));
            sjs_event spec = specifier(SJS_IMPORT_NAME, SJS_STR("default"), t->at);
            if (hold_specifier(p, &spec, token_text(p, t)) == H_STOP) return H_STOP;
            f->state = I_DEFAULT_AFTER;
            return H_TAKEN;
        }
        return import_braces(p, t);
    case I_SECOND:
        return import_braces(p, t);
    case I_DEFAULT_AFTER:
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->state = I_SECOND;
            return H_TAKEN;
        }
        return expect_from(p, t);
    case I_STAR:
        if (!is_word(p, t, SJS_STR("as"))) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected as after *" });
        f->state = I_STAR_NAME;
        return H_TAKEN;
    case I_STAR_NAME: {
        if (t->kind != SJS_TOKEN_NAME) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name for the namespace" });
        if (t->keyword != SJS_K_NONE) return refuse_binding(p, t, token_text(p, t));
        sjs_event spec = specifier(SJS_IMPORT_NAMESPACE, SJS_STR(""), t->at);
        if (hold_specifier(p, &spec, token_text(p, t)) == H_STOP) return H_STOP;
        f->state = I_FROM;
        return H_TAKEN;
    }
    case I_NAME:
    case I_AS:
    case I_ALIAS:
        return import_name_token(p, t);
    case I_AFTER:
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->state = I_NAME;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_RBRACE)) {
            f->state = I_FROM;
            return H_TAKEN;
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected , or }" });
    case I_FROM:
        return expect_from(p, t);
    case I_SOURCE:
        if (t->kind != SJS_TOKEN_STRING) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected the module's name as a string" });
        if (f->names & SPEC_STRING_LOCAL) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs names exports with names, not strings" });
        }
        if (hold_name(p, t) == H_STOP) return H_STOP;
        f->state = I_END;
        return H_TAKEN;
    case I_END:
        return source_end(p, t, SJS_EV_IMPORT);
    default:
        return attributes_token(p, t);
    }
}

/* `export { ... }`: the names, until `}`. */
static handled export_name_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case X_NAME:
        if (is_punctuator(t, SJS_P_RBRACE)) {
            f->state = X_CLOSE;
            return H_TAKEN;
        }
        if (t->kind != SJS_TOKEN_NAME && t->kind != SJS_TOKEN_STRING) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name to export" });
        }
        if (hold_name(p, t) == H_STOP) return H_STOP;
        if (t->kind == SJS_TOKEN_STRING) f->names |= SPEC_STRING_LOCAL;
        else if (t->keyword != SJS_K_NONE) f->names |= SPEC_RESERVED_LOCAL;
        f->state = X_AS;
        return H_TAKEN;
    case X_AS: {
        if (is_word(p, t, SJS_STR("as"))) {
            f->state = X_ALIAS;
            return H_TAKEN;
        }
        if (!is_punctuator(t, SJS_P_COMMA) && !is_punctuator(t, SJS_P_RBRACE)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected as, , or }" });
        }
        sjs_event spec = specifier(0, held_name(p), p->name_at);
        if (hold_specifier(p, &spec, held_name(p)) == H_STOP) return H_STOP;
        f->state = X_AFTER;
        return H_AGAIN;
    }
    case X_ALIAS: {
        if (t->kind != SJS_TOKEN_NAME && t->kind != SJS_TOKEN_STRING) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name to export as" });
        }
        /* A local list's EXPORT_NAME says in its count that the name was a string. */
        uint32_t string = t->kind == SJS_TOKEN_STRING ? 1u : 0u;
        if (string) f->names |= SPEC_STRING_NAME;
        sjs_event spec = specifier(string, held_name(p), p->name_at);
        if (hold_specifier(p, &spec, token_text(p, t)) == H_STOP) return H_STOP;
        f->state = X_AFTER;
        return H_TAKEN;
    }
    default: /* X_AFTER */
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->state = X_NAME;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_RBRACE)) {
            f->state = X_CLOSE;
            return H_TAKEN;
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected , or }" });
    }
}

static handled export_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case X_CLOSE:
        if (is_word(p, t, SJS_STR("from"))) {
            /* Even after a line break: `export { a }` cannot end before it. */
            f->state = X_SOURCE;
            return H_TAKEN;
        }
        if (!is_punctuator(t, SJS_P_SEMICOLON) && t->kind != SJS_TOKEN_END && !t->newline_before) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected from or ; after an export list" });
        }
        /* A local list: its names must be declared ones. */
        if (f->names & (SPEC_RESERVED_LOCAL | SPEC_STRING_LOCAL)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "only a re-export, with from, can export this name" });
        }
        if (send_specifiers(p, SJS_EV_EXPORT_NAME, SJS_STR("")) == H_STOP) return H_STOP;
        return statement_end(p, t);
    case X_STAR:
        if (is_word(p, t, SJS_STR("as"))) {
            f->state = X_STAR_NAME;
            return H_TAKEN;
        }
        if (is_word(p, t, SJS_STR("from"))) {
            sjs_event spec = specifier(SJS_IMPORT_STAR, SJS_STR(""), f->at);
            if (hold_specifier(p, &spec, SJS_STR("")) == H_STOP) return H_STOP;
            f->state = X_SOURCE;
            return H_TAKEN;
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected as or from after export *" });
    case X_STAR_NAME: {
        if (t->kind != SJS_TOKEN_NAME && t->kind != SJS_TOKEN_STRING) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name for the namespace" });
        }
        if (t->kind == SJS_TOKEN_STRING) f->names |= SPEC_STRING_NAME;
        sjs_event spec = specifier(SJS_IMPORT_NAMESPACE, SJS_STR(""), t->at);
        if (hold_specifier(p, &spec, token_text(p, t)) == H_STOP) return H_STOP;
        f->state = X_FROM;
        return H_TAKEN;
    }
    case X_FROM:
        return expect_from(p, t);
    case X_SOURCE:
        if (t->kind != SJS_TOKEN_STRING) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected the module's name as a string" });
        if (f->names & (SPEC_STRING_LOCAL | SPEC_STRING_NAME)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs names exports with names, not strings" });
        }
        if (hold_name(p, t) == H_STOP) return H_STOP;
        f->state = X_END;
        return H_TAKEN;
    case X_END:
        return source_end(p, t, SJS_EV_EXPORT_FROM);
    default:
        if (f->state >= WA_OPEN) return attributes_token(p, t);
        return export_name_token(p, t);
    }
}

/* The token after `export`. */
static handled export_start(sjs_parser *p, const queued_token *t) {
    top_frame(p)->state = M_ITEMS;
    if (t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_CONST) {
        event_place exported = { t->at, p->next_node++, p->export_token };
        if (send_kind(p, SJS_EV_EXPORT, exported) == H_STOP) return H_STOP;
        if (begin_const(p, t) == H_STOP) return H_STOP;
        top_frame(p)->exported = 1;
        return H_TAKEN;
    }
    if (t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_RESERVED && is_word(p, t, SJS_STR("default"))) {
        /* Known before the value, which may hold what staticjs leaves out;
         * the compiler finds every other name exported twice. */
        if (p->default_exported) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "this name is exported twice" });
        p->default_exported = true;
        event_place place = { t->at, p->next_node++, p->export_token };
        if (push_frame(p, F_DEFAULT, place) == H_STOP) return H_STOP;
        p->expect = E_OPERAND;
        return H_TAKEN;
    }
    if (is_punctuator(t, SJS_P_LBRACE) || is_punctuator(t, SJS_P_STAR)) {
        event_place place = { t->at, 0, p->export_token };
        if (push_statement(p, F_EXPORT, place) == H_STOP) return H_STOP;
        top_frame(p)->state = is_punctuator(t, SJS_P_STAR) ? X_STAR : X_NAME;
        return H_TAKEN;
    }
    static const char *const declarations[] = { "let", "var", "function", "class", "async" };
    for (uint32_t i = 0; i < sizeof declarations / sizeof declarations[0]; i++) {
        if (is_word(p, t, sjs_cstr(declarations[i]))) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs exports const declarations, default, lists and re-exports" });
        }
    }
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a declaration, default, { or * after export" });
}

/* `export default`: an expression, not a declaration. */
static handled default_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f->state == DF_FIRST) {
        f->state = DF_VALUE;
        if (t->kind == SJS_TOKEN_NAME && (t->keyword == SJS_K_CONST || is_word(p, t, SJS_STR("let")) || is_word(p, t, SJS_STR("var")))) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "export default takes an expression, not a declaration" });
        }
    }
    return expression_token(p, t);
}

static handled module_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f->state == M_EXPORT) return export_start(p, t);
    if (t->kind == SJS_TOKEN_END) {
        if (p->refusing) return report_refusal(p);
        p->done = true;
        return send_kind(p, SJS_EV_END, program_end(t->at)) == H_STOP ? H_STOP : H_TAKEN;
    }
    if (is_punctuator(t, SJS_P_SEMICOLON)) return H_TAKEN;
    if (t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_IMPORT) {
        const queued_token *next = peek(p, 1);
        if (next == NULL) return H_WAIT;
        if (is_punctuator(next, SJS_P_LPAREN) || is_punctuator(next, SJS_P_DOT)) return import_word(p, t);
        return push_statement(p, F_IMPORT, no_node(t->at));
    }
    if (t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_EXPORT) {
        f->state = M_EXPORT;
        p->export_token = t->at;
        return H_TAKEN;
    }
    if (t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_CONST) return begin_const(p, t);
    if (is_punctuator(t, SJS_P_HASH) && t->at.offset == 0) {
        /* `#!` on the first line is a comment. */
        const queued_token *next = peek(p, 1);
        if (next == NULL) return H_WAIT;
        if (is_punctuator(next, SJS_P_BANG) && next->at.offset == 1) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs reads no #! line" });
        }
    }
    /* A statement: refused, and read on where it can be, for a syntax error
     * further on (`if (a) return 1;`, a return outside a function). */
    if (refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "a staticjs module holds imports, const declarations and exports" }) == H_STOP) {
        return H_STOP;
    }
    return statement_token(p, t, false);
}

/* What a `new` frame constructs (frame.is_new): Set, Map, Date, or a
 * Temporal type, NEW_TEMPORAL plus its index in TEMPORAL_CONSTRUCTORS. The
 * parser reads every type JavaScript has; the compiler refuses one staticjs
 * does not construct. */
enum { NEW_SET = 1, NEW_MAP, NEW_DATE, NEW_TEMPORAL };

static const char *const TEMPORAL_CONSTRUCTORS[] = {
    "Temporal.Duration", "Temporal.Instant", "Temporal.PlainDate", "Temporal.PlainDateTime",
    "Temporal.PlainMonthDay", "Temporal.PlainTime", "Temporal.PlainYearMonth", "Temporal.ZonedDateTime"
};

/* 1 plus the index of Temporal's type `name`, or 0. */
static uint8_t temporal_constructor(sjs_str name) {
    for (uint8_t i = 0; i < sizeof TEMPORAL_CONSTRUCTORS / sizeof TEMPORAL_CONSTRUCTORS[0]; i++) {
        if (sjs_str_equal(name, sjs_cstr(TEMPORAL_CONSTRUCTORS[i] + 9))) return (uint8_t)(i + 1u);
    }
    return 0;
}

static handled new_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f->state == N_NAME) {
        if (is_punctuator(t, SJS_P_DOT)) {
            /* new.target: only in a function, and an arrow function has none
             * of its own. A script's expression is a function's body. */
            const queued_token *next = peek(p, 1);
            if (next == NULL) return H_WAIT;
            if (p->goal == SJS_GOAL_MODULE || !is_word(p, next, SJS_STR("target"))) {
                return parse_fail(p, next, (sjs_problem){ SJS_ERROR_SYNTAX, "new. is only new.target, in a function" });
            }
            /* Refused, and read on as an operand no pattern takes. */
            if (refuse(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no new.target" }) == H_STOP) return H_STOP;
            pop_frame(p);
            p->expect = E_OPERATOR;
            p->last_operand = LO_OTHER;
            p->head++;
            return H_TAKEN;
        }
        if (t->kind == SJS_TOKEN_NAME && sjs_str_equal(token_text(p, t), SJS_STR("Temporal"))) {
            /* new Temporal.PlainDate(...): the three tokens at once. */
            const queued_token *dot = peek(p, 1);
            if (dot == NULL) return H_WAIT;
            const queued_token *type = is_punctuator(dot, SJS_P_DOT) ? peek(p, 2) : NULL;
            if (is_punctuator(dot, SJS_P_DOT) && type == NULL) return H_WAIT;
            uint8_t which = type != NULL && type->kind == SJS_TOKEN_NAME ? temporal_constructor(token_text(p, type)) : 0u;
            if (which != 0) {
                p->head += 2;
                f->state = N_OPEN;
                f->is_new = (uint8_t)(NEW_TEMPORAL + which - 1u);
                return H_TAKEN;
            }
        }
        bool map = t->kind == SJS_TOKEN_NAME && sjs_str_equal(token_text(p, t), SJS_STR("Map"));
        bool date = t->kind == SJS_TOKEN_NAME && sjs_str_equal(token_text(p, t), SJS_STR("Date"));
        if (!map && !date && (t->kind != SJS_TOKEN_NAME || !sjs_str_equal(token_text(p, t), SJS_STR("Set")))) {
            /* What new constructs is a member expression: no unary operator,
             * and of import only import.meta. */
            bool member = starts_member(p, t);
            if (member && t->keyword == SJS_K_IMPORT) {
                const queued_token *next = peek(p, 1);
                if (next == NULL) return H_WAIT;
                member = p->goal == SJS_GOAL_MODULE && is_punctuator(next, SJS_P_DOT);
            }
            if (!member) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected what new constructs" });
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs constructs only Set and Map, Date, and the Temporal types it has" });
        }
        f->state = N_OPEN;
        f->is_new = map ? NEW_MAP : date ? NEW_DATE : NEW_SET;   /* which, until the ( */
        return H_TAKEN;
    }
    if (!is_punctuator(t, SJS_P_LPAREN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "write new Set(...), new Map(...), new Date(...) and new Temporal.PlainDate(...) with parentheses" });
    sjs_str constructor = f->is_new == NEW_MAP ? SJS_STR("Map") : f->is_new == NEW_SET ? SJS_STR("Set")
                        : f->is_new == NEW_DATE ? SJS_STR("Date") : sjs_cstr(TEMPORAL_CONSTRUCTORS[f->is_new - NEW_TEMPORAL]);
    f->type = F_CALL;
    f->state = 0;
    p->expect = E_OPERAND;
    return send_event(p, texted(SJS_EV_NEW_BEGIN, constructor, frame_place(f, f->at)));
}

/* ------------------------------------------------------------------------
 * Dispatch
 */

/* Does the top frame read its next token as an operator? Then a string is an
 * error, or, after a line break, the start of the next statement; it is
 * not read as a string yet. */
static bool expects_operator(const sjs_parser *p, const frame *f) {
    if (p->expect == E_OPERAND || p->arrow_body_next) return false;
    switch (f->type) {
    case F_MODULE:
    case F_BLOCK:
    case F_INNER_BLOCK:
    case F_LABEL:
    case F_IMPORT:
    case F_EXPORT:
    case F_NEW:
    case F_PARAMS:
    case F_PATTERN_ARRAY:
    case F_PATTERN_OBJECT:
        return false;
    case F_CONST:
        return f->state == C_INIT;
    case F_OBJECT:
        return f->state == O_VALUE;
    case F_IF:
        return f->state == IF_TEST;
    default:
        return true;
    }
}

/* A string with a surrogate that has no partner. JavaScript requires a module
 * export name to be well-formed Unicode; anywhere else, it is a string
 * staticjs cannot hold. */
static handled lone_surrogate_string(sjs_parser *p, const queued_token *t) {
    const frame *f = top_frame(p);
    bool export_name = (f->type == F_IMPORT && f->state == I_NAME) ||
                       (f->type == F_EXPORT && (f->state == X_NAME || f->state == X_ALIAS || f->state == X_STAR_NAME));
    if (export_name) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "a module export name cannot hold a lone surrogate" });
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs strings cannot hold a lone surrogate" });
}

static handled dispatch(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f == NULL) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "unexpected token after the end" });
    if (p->skip != SKIP_NONE) return skip_token(p, t);
    if (t->kind == SJS_TOKEN_STRING && t->lone_surrogate && !expects_operator(p, f)) {
        /* Unless it begins a directive prologue that holds "use strict". */
        if (f->type == F_BLOCK && f->count == 0 && f->nonsimple) {
            handled h = strict_directive(p);
            if (h != H_TAKEN) return h;
        }
        return lone_surrogate_string(p, t);
    }
    switch (f->type) {
    case F_MODULE:
        return module_token(p, t);
    case F_BLOCK:
    case F_INNER_BLOCK:
        return block_token(p, t);
    case F_IF:
        return if_token(p, t);
    case F_LABEL:
        return statement_token(p, t, true);
    case F_IMPORT:
        return import_token(p, t);
    case F_EXPORT:
        return export_token(p, t);
    case F_DEFAULT:
        return default_token(p, t);
    case F_NEW:
        return new_token(p, t);
    case F_PARAMS:
        return params_token(p, t);
    case F_PATTERN_ARRAY:
        return pattern_array_token(p, t);
    case F_PATTERN_OBJECT:
        return pattern_object_token(p, t);
    case F_CONST:
        if (f->state != C_INIT) return const_token(p, t);
        return expression_token(p, t);
    case F_OBJECT:
        if (f->state != O_VALUE) return object_key_token(p, t);
        return expression_token(p, t);
    default:
        return expression_token(p, t);
    }
}

/* Read every queued token the parser can decide on now. */
static sjs_result run(sjs_parser *p) {
    while (!p->failed) {
        uint32_t n = SJS_BUF_COUNT(p->queue, queued_token);
        if (p->head >= n) break;
        if (p->done) {
            const queued_token *t = &SJS_BUF_AT(p->queue, queued_token, p->head);
            if (t->kind != SJS_TOKEN_END) {
                parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "unexpected token after the end" });
                break;
            }
            p->head++;
            continue;
        }
        queued_token t = SJS_BUF_AT(p->queue, queued_token, p->head);
        handled h = dispatch(p, &t);
        if (h == H_STOP) break;
        if (h == H_WAIT) break;
        if (h == H_TAKEN) p->head++;
    }
    if (p->failed) return SJS_ERR_PROGRAM;
    if (p->head >= SJS_BUF_COUNT(p->queue, queued_token)) {
        /* Nothing waits: reuse the queue from the start. */
        p->queue.length = 0;
        p->queue_text.length = 0;
        p->head = 0;
    }
    return SJS_OK;
}

static sjs_result on_token(void *self, const sjs_token *arrived) {
    sjs_parser *p = self;
    sjs_token merged = *arrived;
    const sjs_token *token = arrived;
    if (arrived->kind == SJS_TOKEN_REGEXP) {
        /* It takes the place of the slash that began it, the newest token
         * queued (regex_starts), with the slash's position and line break. */
        uint32_t n = SJS_BUF_COUNT(p->queue, queued_token);
        if (n > p->head) {
            queued_token slash = SJS_BUF_AT(p->queue, queued_token, n - 1u);
            p->queue.length -= (uint32_t)sizeof(queued_token);
            p->queue_text.length = slash.text_offset;
            merged.newline_before = slash.newline_before;
            merged.at = slash.at;
            token = &merged;
        }
    }
    queued_token q = { 0 };
    q.kind = token->kind;
    q.punctuator = token->punctuator;
    q.keyword = token->keyword;
    q.newline_before = token->newline_before;
    q.lone_surrogate = token->lone_surrogate;
    q.escaped = token->escaped;
    q.bigint = token->bigint;
    q.at = token->at;
    q.number = token->number;
    q.text_offset = p->queue_text.length;
    q.text_length = token->text.length;
    if (SJS_BUF_COUNT(p->queue, queued_token) - p->head > p->limits.max_lookahead + 2u) {
        sjs_error_set(&p->error, token->at, (sjs_problem){ SJS_ERROR_LIMIT, "the parser needs to see further ahead than max_lookahead" });
        p->failed = true;
        return SJS_ERR_PROGRAM;
    }
    if (sjs_buf_append(&p->allocator, &p->queue_text, (sjs_bytes){ (const uint8_t *)token->text.ptr, token->text.length }) != SJS_OK ||
        sjs_buf_append(&p->allocator, &p->queue, (sjs_bytes){ (const uint8_t *)&q, sizeof q }) != SJS_OK) {
        fail_memory(p);
        return SJS_ERR_PROGRAM;
    }
    if (token->kind == SJS_TOKEN_END) p->finished = true;
    return run(p);
}

/* ------------------------------------------------------------------------
 * The public API
 */

sjs_result sjs_parser_init(sjs_parser **out, const sjs_parser_config *config) {
    if (out == NULL || config == NULL || config->allocator.v == NULL || config->sink.v == NULL ||
        config->sink.v->event == NULL) {
        return SJS_ERR_INVALID;
    }
    sjs_parser *p = sjs_mem_alloc(&config->allocator, sizeof *p);
    if (p == NULL) return SJS_ERR_NOMEM;
    sjs_zero(p, sizeof *p);
    p->allocator = config->allocator;
    p->limits = config->limits;
    p->goal = config->goal;
    p->sink = config->sink;
    p->lexer.allocator = config->allocator;
    p->lexer.limits = config->limits;
    p->lexer.handler = on_token;
    p->lexer.handler_self = p;
    p->lexer.error = &p->error;
    sjs_lexer_init(&p->lexer);
    sjs_position start = { 0, 1, 1 };
    p->next_node = 1;
    p->last_start = start;
    if (push_frame(p, p->goal == SJS_GOAL_MODULE ? F_MODULE : F_EXPRESSION_GOAL, no_node(start)) == H_STOP) {
        sjs_parser_destroy(p);
        return SJS_ERR_NOMEM;
    }
    p->expect = E_OPERAND;
    *out = p;
    return SJS_OK;
}

sjs_result sjs_parser_push(sjs_parser *parser, sjs_bytes chunk) {
    if (parser == NULL || (chunk.ptr == NULL && chunk.length != 0)) return SJS_ERR_INVALID;
    if (parser->failed) return SJS_ERR_PROGRAM;
    if (parser->finished) return SJS_ERR_INVALID;
    sjs_result r = sjs_lexer_push(&parser->lexer, chunk);
    if (r != SJS_OK) parser->failed = true;
    return r;
}

sjs_result sjs_parser_finish(sjs_parser *parser) {
    if (parser == NULL) return SJS_ERR_INVALID;
    if (parser->failed) return SJS_ERR_PROGRAM;
    if (parser->finished) return SJS_ERR_INVALID;
    sjs_result r = sjs_lexer_finish(&parser->lexer);
    if (r != SJS_OK) {
        parser->failed = true;
        return r;
    }
    if (!parser->done) {
        /* Tokens that never got their lookahead: the end decides them. */
        r = run(parser);
        if (r != SJS_OK) return r;
        if (!parser->done) {
            sjs_error_set(&parser->error, parser->lexer.here, (sjs_problem){ SJS_ERROR_SYNTAX, "the source ends early" });
            parser->failed = true;
            return SJS_ERR_PROGRAM;
        }
    }
    return SJS_OK;
}

sjs_result sjs_parser_error(const sjs_parser *parser, sjs_error *out) {
    if (parser == NULL || out == NULL) return SJS_ERR_INVALID;
    if (!parser->failed || parser->error.error.code == SJS_ERROR_NONE) return SJS_ERR_NOT_FOUND;
    *out = parser->error.error;
    return SJS_OK;
}

void sjs_parser_destroy(sjs_parser *parser) {
    if (parser == NULL) return;
    sjs_allocator allocator = parser->allocator;
    sjs_lexer_destroy(&parser->lexer);
    sjs_buf_free(&allocator, &parser->queue);
    sjs_buf_free(&allocator, &parser->queue_text);
    sjs_buf_free(&allocator, &parser->frames);
    sjs_buf_free(&allocator, &parser->ops);
    sjs_buf_free(&allocator, &parser->held);
    sjs_buf_free(&allocator, &parser->held_text);
    sjs_buf_free(&allocator, &parser->name);
    sjs_buf_free(&allocator, &parser->scan_levels);
    sjs_buf_free(&allocator, &parser->scan_brackets);
    sjs_buf_free(&allocator, &parser->labels);
    sjs_mem_free(&allocator, parser, sizeof *parser);
}
