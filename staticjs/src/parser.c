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
 * after a name, to tell `x => ...` from `x`, and after `(`, to tell an arrow
 * function's parameters from a parenthesized expression. That lookahead is
 * bounded by max_lookahead.
 */
#include "syntax.h"

typedef struct {
    uint8_t kind;
    uint8_t punctuator;
    uint8_t keyword;
    uint8_t newline_before;
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
    F_CONST,
    F_RETURN,
    F_PARAMS,
    F_PATTERN_ARRAY,
    F_PATTERN_OBJECT,
    F_IMPORT,
    F_NEW
};

/* States. */
enum {
    /* F_OBJECT */
    O_KEY = 0, O_KEY_NAME, O_COLON, O_VALUE,
    /* F_CONST */
    C_PATTERN = 0, C_ASSIGN, C_INIT,
    /* F_PARAMS */
    P_START = 0, P_AFTER,
    /* F_PATTERN_ARRAY */
    PA_ELEMENT = 0, PA_AFTER,
    /* F_PATTERN_OBJECT */
    PO_KEY = 0, PO_KEY_NAME, PO_AFTER, PO_VALUE,
    /* F_BLOCK */
    B_STATEMENTS = 0, B_AFTER_RETURN,
    /* F_MODULE */
    M_IMPORTS = 0, M_DECLARATIONS, M_EXPORT,
    /* F_IMPORT */
    I_OPEN = 0, I_NAME, I_AS, I_ALIAS, I_AFTER, I_FROM, I_SOURCE, I_END,
    /* F_NEW */
    N_NAME = 0, N_OPEN
};

/* How the current expression frame expects its next token. */
enum { E_OPERAND = 0, E_OPERATOR, E_AFTER_BLOCK_ARROW };

/* Segment flags: which short-circuit operators the current operand chain used. */
#define SEG_LOGICAL 1u
#define SEG_COALESCE 2u

typedef struct {
    uint8_t type;
    uint8_t state;
    uint8_t spread;
    uint8_t segment;
    uint8_t chain;
    uint8_t buffered;
    uint8_t after_comma;
    uint8_t is_new;
    uint32_t ops_base;
    uint32_t count;
    uint32_t held_base;    /* F_CONST: its pattern's first held event */
    uint32_t held_text_base;
    sjs_position at;
} frame;

/* Operators waiting for their right operand. */
enum { OP_UNARY = 1, OP_BINARY, OP_LOGICAL, OP_ARROW, OP_ELSE };

typedef struct {
    uint8_t kind;
    uint8_t op;
    uint8_t precedence;
    uint8_t saved_segment;
    sjs_position at;
} pending_op;

/* An event held until it can be emitted: a const's pattern, an import. */
typedef struct {
    uint8_t kind;
    uint32_t count;
    uint32_t text_offset;
    uint32_t text_length;
    uint32_t alias_offset;
    uint32_t alias_length;
    sjs_position at;
} held_event;

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
};

typedef enum { H_TAKEN = 0, H_AGAIN, H_WAIT, H_STOP } handled;

#define PRECEDENCE_ELSE 1u
#define PRECEDENCE_COALESCE 4u
#define PRECEDENCE_OR 4u
#define PRECEDENCE_AND 5u
#define PRECEDENCE_EQUALITY 9u
#define PRECEDENCE_RELATIONAL 10u
#define PRECEDENCE_ADDITIVE 12u
#define PRECEDENCE_MULTIPLICATIVE 13u
#define PRECEDENCE_UNARY 15u

/* ------------------------------------------------------------------------
 * Errors and events
 */

static handled parse_fail(sjs_parser *p, const queued_token *t, sjs_problem problem) {
    sjs_position at = t != NULL ? t->at : p->lexer.here;
    sjs_error_set(&p->error, at, problem);
    if (t != NULL && t->text_length > 0) {
        sjs_error_subject(&p->error, (sjs_str){ (const char *)p->queue_text.ptr + t->text_offset, t->text_length });
    }
    p->failed = true;
    return H_STOP;
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
    held_event h = { 0 };
    h.kind = (uint8_t)e->kind;
    h.count = e->count;
    h.at = e->at;
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

static handled send_kind(sjs_parser *p, uint8_t kind, sjs_position at) {
    sjs_event e = { 0 };
    e.kind = kind;
    e.at = at;
    return send(p, &e);
}

static sjs_event counted(uint8_t kind, uint32_t count, sjs_position at) {
    sjs_event e = { 0 };
    e.kind = kind;
    e.count = count;
    e.at = at;
    return e;
}

static sjs_event texted(uint8_t kind, sjs_str text, sjs_position at) {
    sjs_event e = { 0 };
    e.kind = kind;
    e.text = text;
    e.at = at;
    return e;
}

static handled send_event(sjs_parser *p, sjs_event e) {
    return send(p, &e);
}

/* Send the events the top frame held, in order, and forget them. A const
 * inside another's value holds its pattern above the outer one's. */
static handled flush_held(sjs_parser *p) {
    frame *f = top_frame(p);
    uint32_t n = SJS_BUF_COUNT(p->held, held_event);
    for (uint32_t i = f->held_base; i < n; i++) {
        held_event h = SJS_BUF_AT(p->held, held_event, i);
        sjs_event e = { 0 };
        e.kind = h.kind;
        e.count = h.count;
        e.at = h.at;
        e.text = (sjs_str){ (const char *)p->held_text.ptr + h.text_offset, h.text_length };
        e.alias = (sjs_str){ (const char *)p->held_text.ptr + h.alias_offset, h.alias_length };
        sjs_result r = p->sink.v->event(p->sink.self, &e);
        if (r != SJS_OK) {
            p->failed = true;
            p->error.error.code = SJS_ERROR_NONE;
            return H_STOP;
        }
    }
    p->held.length = f->held_base * (uint32_t)sizeof(held_event);
    p->held_text.length = f->held_text_base;
    return H_TAKEN;
}

/* The top frame's pattern starts here in the held events. */
static void hold_from_here(sjs_parser *p) {
    frame *f = top_frame(p);
    f->held_base = SJS_BUF_COUNT(p->held, held_event);
    f->held_text_base = p->held_text.length;
    f->buffered = 1;
}

/* ------------------------------------------------------------------------
 * Stacks
 */

static handled push_frame(sjs_parser *p, uint8_t type, sjs_position at) {
    if (frame_depth(p) >= p->limits.max_nesting) {
        sjs_error_set(&p->error, at, (sjs_problem){ SJS_ERROR_LIMIT, "the program nests deeper than max_nesting" });
        p->failed = true;
        return H_STOP;
    }
    frame f = { 0 };
    f.type = type;
    f.at = at;
    f.ops_base = SJS_BUF_COUNT(p->ops, pending_op);
    frame *parent = top_frame(p);
    if (parent != NULL && parent->buffered && (type == F_PATTERN_ARRAY || type == F_PATTERN_OBJECT)) f.buffered = 1;
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

/* Emit the event of the operator on top and pop it. */
static handled pop_op(sjs_parser *p) {
    pending_op op = *top_op(p);
    p->ops.length -= (uint32_t)sizeof(pending_op);
    switch (op.kind) {
    case OP_UNARY:
        return send_event(p, counted(SJS_EV_UNARY, op.op, op.at));
    case OP_BINARY:
        return send_event(p, counted(SJS_EV_BINARY, op.op, op.at));
    case OP_LOGICAL:
        return send_event(p, counted(SJS_EV_LOGICAL_END, op.op, op.at));
    case OP_ARROW:
        top_frame(p)->segment = op.saved_segment;
        return send_kind(p, SJS_EV_ARROW_END, op.at);
    default:
        top_frame(p)->segment = op.saved_segment;
        return send_kind(p, SJS_EV_CONDITIONAL_END, op.at);
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
    return send_event(p, counted(SJS_EV_CHAIN_END, frame_depth(p), f->at));
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

static bool is_binding_name(const queued_token *t) {
    return t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_NONE;
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

/* An arrow function may start only where an assignment expression may. */
static bool arrow_allowed(sjs_parser *p) {
    pending_op *op = top_op(p);
    return op == NULL || op->kind == OP_ARROW || op->kind == OP_ELSE;
}

/* After `=>`: a block, or an expression that runs as far as it can. */
static handled arrow_body(sjs_parser *p, const queued_token *t) {
    if (is_punctuator(t, SJS_P_LBRACE)) {
        if (push_frame(p, F_BLOCK, t->at) == H_STOP) return H_STOP;
        return send_kind(p, SJS_EV_BLOCK_BEGIN, t->at);
    }
    frame *f = top_frame(p);
    pending_op op = { OP_ARROW, 0, PRECEDENCE_ELSE, f->segment, t->at };
    f->segment = 0;
    if (push_op(p, op) == H_STOP) return H_STOP;
    p->expect = E_OPERAND;
    return H_AGAIN;
}

typedef enum { D_WAIT = 0, D_ARROW, D_EXPRESSION, D_FAILED } decision;

/* After `(`: are these an arrow function's parameters? */
static decision decide_paren(sjs_parser *p) {
    int32_t depth = 0;
    for (uint32_t i = 1;; i++) {
        if (i > p->limits.max_lookahead) {
            parse_fail(p, peek(p, 0), (sjs_problem){ SJS_ERROR_LIMIT, "an arrow function's parameters are longer than max_lookahead" });
            return D_FAILED;
        }
        const queued_token *t = peek(p, i);
        if (t == NULL) return D_WAIT;
        if (t->kind == SJS_TOKEN_END) return D_EXPRESSION;
        if (t->kind == SJS_TOKEN_NAME || t->kind == SJS_TOKEN_STRING) continue;
        if (t->kind != SJS_TOKEN_PUNCTUATOR) return D_EXPRESSION;
        switch (t->punctuator) {
        case SJS_P_RPAREN: {
            if (depth != 0) return D_EXPRESSION;
            const queued_token *after = peek(p, i + 1);
            if (after == NULL) return D_WAIT;
            return is_punctuator(after, SJS_P_ARROW) ? D_ARROW : D_EXPRESSION;
        }
        case SJS_P_LBRACKET:
        case SJS_P_LBRACE:
            depth++;
            continue;
        case SJS_P_RBRACKET:
        case SJS_P_RBRACE:
            if (--depth < 0) return D_EXPRESSION;
            continue;
        case SJS_P_COMMA:
        case SJS_P_COLON:
        case SJS_P_ELLIPSIS:
            continue;
        case SJS_P_ASSIGN:
            /* A default value, or an assignment: staticjs has neither, and
             * reading parameters says so with the clearer message. */
            return D_ARROW;
        default:
            return D_EXPRESSION;
        }
    }
}

static handled unsupported_word(sjs_parser *p, const queued_token *t) {
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs does not have this" });
}

static handled operand_name(sjs_parser *p, const queued_token *t) {
    switch (t->keyword) {
    case SJS_K_TRUE:
        p->expect = E_OPERATOR;
        return send_kind(p, SJS_EV_TRUE, t->at) == H_STOP ? H_STOP : H_TAKEN;
    case SJS_K_FALSE:
        p->expect = E_OPERATOR;
        return send_kind(p, SJS_EV_FALSE, t->at) == H_STOP ? H_STOP : H_TAKEN;
    case SJS_K_NULL:
        p->expect = E_OPERATOR;
        return send_kind(p, SJS_EV_NULL, t->at) == H_STOP ? H_STOP : H_TAKEN;
    case SJS_K_TYPEOF: {
        pending_op op = { OP_UNARY, SJS_OP_TYPEOF, PRECEDENCE_UNARY, 0, t->at };
        return push_op(p, op);
    }
    case SJS_K_NEW:
        return push_frame(p, F_NEW, t->at);
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
        if (send_kind(p, SJS_EV_ARROW_BEGIN, t->at) == H_STOP) return H_STOP;
        if (send_event(p, counted(SJS_EV_PARAM, 0, t->at)) == H_STOP) return H_STOP;
        if (send_event(p, texted(SJS_EV_BIND, name, t->at)) == H_STOP) return H_STOP;
        if (send_kind(p, SJS_EV_PARAMS_END, next->at) == H_STOP) return H_STOP;
        /* Consume the name and the arrow; the body comes next. */
        p->head++;
        p->expect = E_OPERAND;
        p->arrow_body_next = true;
        return H_TAKEN;
    }
    p->expect = E_OPERATOR;
    return send_event(p, texted(SJS_EV_NAME, token_text(p, t), t->at)) == H_STOP ? H_STOP : H_TAKEN;
}

static handled operand(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (t->kind) {
    case SJS_TOKEN_NUMBER: {
        sjs_event e = { 0 };
        e.kind = SJS_EV_NUMBER;
        e.number = t->number;
        e.at = t->at;
        p->expect = E_OPERATOR;
        return send(p, &e);
    }
    case SJS_TOKEN_STRING:
    case SJS_TOKEN_TEMPLATE:
        p->expect = E_OPERATOR;
        return send_event(p, texted(SJS_EV_STRING, token_text(p, t), t->at));
    case SJS_TOKEN_TEMPLATE_HEAD:
        if (send_kind(p, SJS_EV_TEMPLATE_BEGIN, t->at) == H_STOP) return H_STOP;
        if (send_event(p, texted(SJS_EV_STRING, token_text(p, t), t->at)) == H_STOP) return H_STOP;
        if (push_frame(p, F_TEMPLATE, t->at) == H_STOP) return H_STOP;
        top_frame(p)->count = 1;
        p->expect = E_OPERAND;
        return H_TAKEN;
    case SJS_TOKEN_NAME:
        return operand_name(p, t);
    case SJS_TOKEN_PUNCTUATOR:
        break;
    default:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an expression" });
    }
    switch (t->punctuator) {
    case SJS_P_LPAREN: {
        decision d = decide_paren(p);
        if (d == D_WAIT) return H_WAIT;
        if (d == D_FAILED) return H_STOP;
        if (d == D_ARROW) {
            if (!arrow_allowed(p)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "an arrow function cannot be an operand here" });
            if (send_kind(p, SJS_EV_ARROW_BEGIN, t->at) == H_STOP) return H_STOP;
            return push_frame(p, F_PARAMS, t->at);
        }
        if (push_frame(p, F_PAREN, t->at) == H_STOP) return H_STOP;
        p->expect = E_OPERAND;
        return H_TAKEN;
    }
    case SJS_P_LBRACKET:
        if (send_kind(p, SJS_EV_ARRAY_BEGIN, t->at) == H_STOP) return H_STOP;
        if (push_frame(p, F_ARRAY, t->at) == H_STOP) return H_STOP;
        p->expect = E_OPERAND;
        return H_TAKEN;
    case SJS_P_LBRACE:
        if (send_kind(p, SJS_EV_OBJECT_BEGIN, t->at) == H_STOP) return H_STOP;
        return push_frame(p, F_OBJECT, t->at);
    case SJS_P_BANG: {
        pending_op op = { OP_UNARY, SJS_OP_NOT, PRECEDENCE_UNARY, 0, t->at };
        return push_op(p, op);
    }
    case SJS_P_MINUS: {
        pending_op op = { OP_UNARY, SJS_OP_NEGATE, PRECEDENCE_UNARY, 0, t->at };
        return push_op(p, op);
    }
    case SJS_P_PLUS: {
        pending_op op = { OP_UNARY, SJS_OP_PLUS, PRECEDENCE_UNARY, 0, t->at };
        return push_op(p, op);
    }
    case SJS_P_ELLIPSIS:
        if ((f->type == F_ARRAY || f->type == F_CALL) && !f->spread && top_op(p) == NULL) {
            f->spread = 1;
            return H_TAKEN;
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "... spreads only into an array, a call or an object" });
    case SJS_P_SLASH:
    case SJS_P_ASSIGN_OPERATOR:
        if (t->punctuator == SJS_P_SLASH || sjs_str_equal(token_text(p, t), SJS_STR("/="))) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no regular expressions" });
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an expression" });
    case SJS_P_PLUS_PLUS:
    case SJS_P_MINUS_MINUS:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no ++ or --: values do not change" });
    case SJS_P_TILDE:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no bitwise operators" });
    case SJS_P_HASH:
    case SJS_P_AT:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs does not have this" });
    default:
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
    pending_op pending = { OP_BINARY, op, precedence, 0, { 0, 0, 0 } };
    return pending;
}

static handled parse_binary(sjs_parser *p, const queued_token *t, pending_op pending) {
    if (end_chain(p) == H_STOP) return H_STOP;
    if (pop_ops(p, pending.precedence) == H_STOP) return H_STOP;
    pending.at = t->at;
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
    if (send_event(p, counted(SJS_EV_LOGICAL, op, t->at)) == H_STOP) return H_STOP;
    pending.kind = OP_LOGICAL;
    pending.at = t->at;
    p->expect = E_OPERAND;
    return push_op(p, pending);
}

/* `.name` after a value. */
static handled member(sjs_parser *p, const queued_token *dot) {
    const queued_token *name = peek(p, 1);
    if (name == NULL) return H_WAIT;
    if (name->kind != SJS_TOKEN_NAME) {
        if (is_punctuator(name, SJS_P_HASH)) return parse_fail(p, name, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no private names" });
        return parse_fail(p, name, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a property name after ." });
    }
    SJS_UNUSED(dot);
    if (send_event(p, texted(SJS_EV_MEMBER, token_text(p, name), name->at)) == H_STOP) return H_STOP;
    p->head++;
    return H_TAKEN;
}

static handled open_call(sjs_parser *p, const queued_token *t) {
    if (send_kind(p, SJS_EV_CALL_BEGIN, t->at) == H_STOP) return H_STOP;
    if (push_frame(p, F_CALL, t->at) == H_STOP) return H_STOP;
    p->expect = E_OPERAND;
    return H_TAKEN;
}

static handled open_index(sjs_parser *p, const queued_token *t) {
    if (push_frame(p, F_INDEX, t->at) == H_STOP) return H_STOP;
    p->expect = E_OPERAND;
    return H_TAKEN;
}

static handled parse_optional(sjs_parser *p, const queued_token *t) {
    const queued_token *next = peek(p, 1);
    if (next == NULL) return H_WAIT;
    frame *f = top_frame(p);
    f->chain = 1;
    if (send_event(p, counted(SJS_EV_OPTIONAL, frame_depth(p), t->at)) == H_STOP) return H_STOP;
    if (next->kind == SJS_TOKEN_NAME) {
        if (send_event(p, texted(SJS_EV_MEMBER, token_text(p, next), next->at)) == H_STOP) return H_STOP;
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

static handled operator_token(sjs_parser *p, const queued_token *t) {
    if (t->kind == SJS_TOKEN_NAME) {
        if (t->keyword == SJS_K_IN) return parse_binary(p, t, infix(SJS_OP_IN, PRECEDENCE_RELATIONAL));
        if (sjs_str_equal(token_text(p, t), SJS_STR("instanceof"))) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no instanceof" });
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
    case SJS_P_QUESTION:
        if (end_chain(p) == H_STOP) return H_STOP;
        if (pop_ops(p, PRECEDENCE_ELSE + 1u) == H_STOP) return H_STOP;
        if (send_kind(p, SJS_EV_CONDITIONAL, t->at) == H_STOP) return H_STOP;
        if (push_frame(p, F_CONDITIONAL, t->at) == H_STOP) return H_STOP;
        p->expect = E_OPERAND;
        return H_TAKEN;
    case SJS_P_EQUAL:
    case SJS_P_NOT_EQUAL:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs compares with === and !== only" });
    case SJS_P_ASSIGN:
    case SJS_P_ASSIGN_OPERATOR:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no assignment: values do not change" });
    case SJS_P_PLUS_PLUS:
    case SJS_P_MINUS_MINUS:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no ++ or --: values do not change" });
    case SJS_P_STAR_STAR:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no ** operator" });
    case SJS_P_SHIFT_LEFT:
    case SJS_P_SHIFT_RIGHT:
    case SJS_P_SHIFT_RIGHT_UNSIGNED:
    case SJS_P_AMPERSAND:
    case SJS_P_PIPE:
    case SJS_P_CARET:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no bitwise operators" });
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
    f->count++;
    return send_kind(p, kind, f->at);
}

static handled list_terminator(sjs_parser *p, const queued_token *t, uint8_t closer) {
    frame *f = top_frame(p);
    bool empty = p->expect == E_OPERAND;
    bool is_array = f->type == F_ARRAY;
    if (empty && f->spread) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "... needs a value" });
    if (is_punctuator(t, SJS_P_COMMA)) {
        if (empty) {
            if (is_array) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs arrays have no holes" });
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an argument" });
        }
        if (end_expression(p) == H_STOP) return H_STOP;
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
        if (end_item(p, is_array ? SJS_EV_ELEMENT : SJS_EV_ARGUMENT,
                     is_array ? SJS_EV_SPREAD_ELEMENT : SJS_EV_SPREAD_ARGUMENT) == H_STOP) {
            return H_STOP;
        }
    }
    f = top_frame(p);
    uint8_t kind = is_array ? SJS_EV_ARRAY_END : (f->is_new ? SJS_EV_NEW_END : SJS_EV_CALL_END);
    uint32_t count = f->count;
    sjs_position at = f->at;
    pop_frame(p);
    p->expect = E_OPERATOR;
    return send_event(p, counted(kind, count, at));
}

static handled finish_const(sjs_parser *p) {
    if (flush_held(p) == H_STOP) return H_STOP;
    return send_kind(p, SJS_EV_CONST_END, top_frame(p)->at);
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
        if (f->type == F_RETURN && (is_punctuator(t, SJS_P_SEMICOLON) || is_punctuator(t, SJS_P_RBRACE))) {
            /* `return;` returns undefined. */
            pop_frame(p);
            top_frame(p)->state = B_AFTER_RETURN;
            if (send_event(p, counted(SJS_EV_RETURN, 0, t->at)) == H_STOP) return H_STOP;
            return is_punctuator(t, SJS_P_SEMICOLON) ? H_TAKEN : H_AGAIN;
        }
        if (f->type == F_PAREN && is_punctuator(t, SJS_P_RPAREN)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "() needs an expression or =>" });
        }
        if (t->kind == SJS_TOKEN_END) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "the source ends inside an expression" });
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected an expression" });
    }
    if (end_expression(p) == H_STOP) return H_STOP;
    f = top_frame(p);
    switch (f->type) {
    case F_EXPRESSION_GOAL:
        if (t->kind != SJS_TOKEN_END) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected the end of the expression" });
        p->done = true;
        return send_kind(p, SJS_EV_END, t->at) == H_STOP ? H_STOP : H_TAKEN;
    case F_PAREN:
        if (is_punctuator(t, SJS_P_COMMA)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no comma operator" });
        if (!is_punctuator(t, SJS_P_RPAREN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected )" });
        pop_frame(p);
        p->expect = E_OPERATOR;
        return H_TAKEN;
    case F_INDEX:
        if (!is_punctuator(t, SJS_P_RBRACKET)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ]" });
        pop_frame(p);
        p->expect = E_OPERATOR;
        return send_kind(p, SJS_EV_INDEX, t->at);
    case F_TEMPLATE:
        if (t->kind == SJS_TOKEN_TEMPLATE_MIDDLE) {
            f->count += 2;
            p->expect = E_OPERAND;
            return send_event(p, texted(SJS_EV_STRING, token_text(p, t), t->at));
        }
        if (t->kind == SJS_TOKEN_TEMPLATE_TAIL) {
            uint32_t count = f->count + 2;
            pop_frame(p);
            p->expect = E_OPERATOR;
            if (send_event(p, texted(SJS_EV_STRING, token_text(p, t), t->at)) == H_STOP) return H_STOP;
            return send_event(p, counted(SJS_EV_TEMPLATE_END, count, t->at));
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected } to close ${" });
    case F_CONDITIONAL: {
        if (!is_punctuator(t, SJS_P_COLON)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected : of ?:" });
        sjs_position at = f->at;
        pop_frame(p);
        if (send_kind(p, SJS_EV_CONDITIONAL_ELSE, t->at) == H_STOP) return H_STOP;
        frame *parent = top_frame(p);
        pending_op op = { OP_ELSE, 0, PRECEDENCE_ELSE, parent->segment, at };
        parent->segment = 0;
        p->expect = E_OPERAND;
        return push_op(p, op);
    }
    case F_OBJECT: {
        if (!is_punctuator(t, SJS_P_COMMA) && !is_punctuator(t, SJS_P_RBRACE)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected , or }" });
        }
        uint8_t kind = f->spread ? SJS_EV_SPREAD_PROPERTY : SJS_EV_PROPERTY;
        f->spread = 0;
        f->count++;
        if (send_kind(p, kind, t->at) == H_STOP) return H_STOP;
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->state = O_KEY;
            f->after_comma = 1;
            return H_TAKEN;
        }
        pop_frame(p);
        p->expect = E_OPERATOR;
        return send_kind(p, SJS_EV_OBJECT_END, t->at);
    }
    case F_COMPUTED_KEY:
        if (!is_punctuator(t, SJS_P_RBRACKET)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ]" });
        pop_frame(p);
        top_frame(p)->state = O_COLON;
        return send_kind(p, SJS_EV_COMPUTED_KEY, t->at);
    case F_CONST:
        if (is_punctuator(t, SJS_P_SEMICOLON) || is_punctuator(t, SJS_P_COMMA)) {
            if (finish_const(p) == H_STOP) return H_STOP;
            if (is_punctuator(t, SJS_P_COMMA)) {
                if (send_kind(p, SJS_EV_CONST_BEGIN, t->at) == H_STOP) return H_STOP;
                top_frame(p)->state = C_PATTERN;
                hold_from_here(p);
                return H_TAKEN;
            }
            pop_frame(p);
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_RBRACE) || t->kind == SJS_TOKEN_END) {
            /* A semicolon is inserted before } and at the end. */
            if (finish_const(p) == H_STOP) return H_STOP;
            pop_frame(p);
            return H_AGAIN;
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after a const declaration" });
    case F_RETURN:
        if (!is_punctuator(t, SJS_P_SEMICOLON) && !is_punctuator(t, SJS_P_RBRACE)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after return" });
        }
        pop_frame(p);
        top_frame(p)->state = B_AFTER_RETURN;
        if (send_event(p, counted(SJS_EV_RETURN, 1, t->at)) == H_STOP) return H_STOP;
        return is_punctuator(t, SJS_P_SEMICOLON) ? H_TAKEN : H_AGAIN;
    default:
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "unexpected token" });
    }
}

static handled expression_token(sjs_parser *p, const queued_token *t) {
    if (p->arrow_body_next) {
        p->arrow_body_next = false;
        return arrow_body(p, t);
    }
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
        f->state = C_ASSIGN;
        f->buffered = 0;
        break;
    default: /* F_PARAMS */
        f->state = P_AFTER;
        break;
    }
}

static handled pattern_start(sjs_parser *p, const queued_token *t) {
    if (t->kind == SJS_TOKEN_NAME) {
        if (t->keyword != SJS_K_NONE) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "this word cannot be bound" });
        if (send_event(p, texted(SJS_EV_BIND, token_text(p, t), t->at)) == H_STOP) return H_STOP;
        pattern_done(p);
        return H_TAKEN;
    }
    if (is_punctuator(t, SJS_P_LBRACKET)) {
        if (push_frame(p, F_PATTERN_ARRAY, t->at) == H_STOP) return H_STOP;
        return send_kind(p, SJS_EV_ARRAY_PATTERN_BEGIN, t->at);
    }
    if (is_punctuator(t, SJS_P_LBRACE)) {
        if (push_frame(p, F_PATTERN_OBJECT, t->at) == H_STOP) return H_STOP;
        return send_kind(p, SJS_EV_OBJECT_PATTERN_BEGIN, t->at);
    }
    if (is_punctuator(t, SJS_P_ELLIPSIS)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no rest elements" });
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name, [ or {" });
}

static handled close_pattern(sjs_parser *p, uint8_t kind, const queued_token *t) {
    if (send_kind(p, kind, t->at) == H_STOP) return H_STOP;
    pop_frame(p);
    pattern_done(p);
    return H_TAKEN;
}

static handled pattern_array_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (is_punctuator(t, SJS_P_ASSIGN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no default values" });
    if (f->state == PA_AFTER) {
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->count++;
            f->state = PA_ELEMENT;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_RBRACKET)) return close_pattern(p, SJS_EV_ARRAY_PATTERN_END, t);
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected , or ]" });
    }
    if (is_punctuator(t, SJS_P_COMMA)) {
        f->count++;
        return H_TAKEN;
    }
    if (is_punctuator(t, SJS_P_RBRACKET)) return close_pattern(p, SJS_EV_ARRAY_PATTERN_END, t);
    if (send_event(p, counted(SJS_EV_PATTERN_INDEX, f->count, t->at)) == H_STOP) return H_STOP;
    return pattern_start(p, t);
}

static handled pattern_object_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case PO_KEY:
        if (is_punctuator(t, SJS_P_RBRACE)) return close_pattern(p, SJS_EV_OBJECT_PATTERN_END, t);
        if (t->kind == SJS_TOKEN_NAME || t->kind == SJS_TOKEN_STRING) {
            if (hold_name(p, t) == H_STOP) return H_STOP;
            p->name_keyword = t->kind == SJS_TOKEN_STRING ? SJS_K_RESERVED : t->keyword;
            f->state = PO_KEY_NAME;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_ELLIPSIS)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no rest properties" });
        if (is_punctuator(t, SJS_P_LBRACKET)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs patterns have no computed keys" });
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a property name" });
    case PO_KEY_NAME:
        if (is_punctuator(t, SJS_P_COLON)) {
            if (send_event(p, texted(SJS_EV_PATTERN_KEY, held_name(p), p->name_at)) == H_STOP) return H_STOP;
            /* The next token starts the property's own pattern. */
            f->state = PO_VALUE;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_COMMA) || is_punctuator(t, SJS_P_RBRACE)) {
            if (p->name_keyword != SJS_K_NONE) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "this word cannot be bound" });
            if (send_event(p, texted(SJS_EV_PATTERN_KEY, held_name(p), p->name_at)) == H_STOP) return H_STOP;
            if (send_event(p, texted(SJS_EV_BIND, held_name(p), p->name_at)) == H_STOP) return H_STOP;
            f->state = PO_AFTER;
            return H_AGAIN;
        }
        if (is_punctuator(t, SJS_P_ASSIGN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no default values" });
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected : , or }" });
    case PO_AFTER:
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->state = PO_KEY;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_RBRACE)) return close_pattern(p, SJS_EV_OBJECT_PATTERN_END, t);
        if (is_punctuator(t, SJS_P_ASSIGN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no default values" });
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected , or }" });
    default: /* PO_VALUE, after `key:` */
        f->state = PO_AFTER;
        return pattern_start(p, t);
    }
}

/* An arrow function's parameter list, after the lookahead chose it. */
static handled params_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (is_punctuator(t, SJS_P_ASSIGN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs has no default values" });
    if (f->state == P_AFTER) {
        if (is_punctuator(t, SJS_P_COMMA)) {
            f->count++;
            f->state = P_START;
            f->after_comma = 1;
            return H_TAKEN;
        }
        if (!is_punctuator(t, SJS_P_RPAREN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected , or )" });
        f->count++;
    } else if (is_punctuator(t, SJS_P_RPAREN)) {
        if (f->count > 0 && !f->after_comma) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a parameter" });
    } else {
        if (send_event(p, counted(SJS_EV_PARAM, f->count, t->at)) == H_STOP) return H_STOP;
        return pattern_start(p, t);
    }
    /* `)`: the arrow follows, as the lookahead saw. */
    const queued_token *arrow = peek(p, 1);
    if (arrow == NULL) return H_WAIT;
    if (arrow->newline_before) return parse_fail(p, arrow, (sjs_problem){ SJS_ERROR_SYNTAX, "=> must be on the line of its parameters" });
    pop_frame(p);
    if (send_kind(p, SJS_EV_PARAMS_END, arrow->at) == H_STOP) return H_STOP;
    p->head++;
    p->expect = E_OPERAND;
    p->arrow_body_next = true;
    return H_TAKEN;
}

/* ------------------------------------------------------------------------
 * Objects
 */

static handled object_key_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case O_KEY:
        if (is_punctuator(t, SJS_P_RBRACE)) {
            if (f->count > 0 && !f->after_comma) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a property" });
            pop_frame(p);
            p->expect = E_OPERATOR;
            return send_kind(p, SJS_EV_OBJECT_END, t->at);
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
            return send_event(p, texted(SJS_EV_KEY, token_text(p, t), t->at));
        }
        if (t->kind == SJS_TOKEN_NUMBER) {
            sjs_event e = { 0 };
            e.kind = SJS_EV_NUMBER;
            e.number = t->number;
            e.at = t->at;
            if (send(p, &e) == H_STOP) return H_STOP;
            f->state = O_COLON;
            return send_kind(p, SJS_EV_COMPUTED_KEY, t->at);
        }
        if (is_punctuator(t, SJS_P_LBRACKET)) {
            if (push_frame(p, F_COMPUTED_KEY, t->at) == H_STOP) return H_STOP;
            p->expect = E_OPERAND;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_ELLIPSIS)) {
            f->spread = 1;
            f->state = O_VALUE;
            p->expect = E_OPERAND;
            return H_TAKEN;
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a property" });
    case O_KEY_NAME:
        if (is_punctuator(t, SJS_P_COLON)) {
            if (sjs_str_equal(held_name(p), SJS_STR("__proto__"))) {
                return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs objects have no prototype" });
            }
            if (send_event(p, texted(SJS_EV_KEY, held_name(p), p->name_at)) == H_STOP) return H_STOP;
            f->state = O_VALUE;
            p->expect = E_OPERAND;
            return H_TAKEN;
        }
        if (is_punctuator(t, SJS_P_COMMA) || is_punctuator(t, SJS_P_RBRACE)) {
            if (p->name_keyword != SJS_K_NONE) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "this word cannot stand for a value" });
            if (sjs_str_equal(held_name(p), SJS_STR("__proto__"))) {
                return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs objects have no prototype" });
            }
            if (send_event(p, texted(SJS_EV_KEY, held_name(p), p->name_at)) == H_STOP) return H_STOP;
            if (send_event(p, texted(SJS_EV_NAME, held_name(p), p->name_at)) == H_STOP) return H_STOP;
            f->state = O_VALUE;
            p->expect = E_OPERATOR;
            return H_AGAIN;
        }
        if (is_punctuator(t, SJS_P_LPAREN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs objects have no methods" });
        if (t->kind == SJS_TOKEN_NAME || t->kind == SJS_TOKEN_STRING || is_punctuator(t, SJS_P_LBRACKET)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs objects have no getters or setters" });
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected : after a property name" });
    default: /* O_COLON */
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
    if (push_frame(p, F_CONST, t->at) == H_STOP) return H_STOP;
    if (send_kind(p, SJS_EV_CONST_BEGIN, t->at) == H_STOP) return H_STOP;
    hold_from_here(p);
    return H_TAKEN;
}

static handled const_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f->state == C_PATTERN) return pattern_start(p, t);
    /* C_ASSIGN */
    if (!is_punctuator(t, SJS_P_ASSIGN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "a const declaration needs = and a value" });
    f->state = C_INIT;
    f->ops_base = SJS_BUF_COUNT(p->ops, pending_op);
    p->expect = E_OPERAND;
    return H_TAKEN;
}

static handled block_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (is_punctuator(t, SJS_P_SEMICOLON)) return H_TAKEN;
    if (is_punctuator(t, SJS_P_RBRACE)) {
        sjs_position at = f->at;
        pop_frame(p);
        if (send_kind(p, SJS_EV_BLOCK_END, t->at) == H_STOP) return H_STOP;
        if (send_kind(p, SJS_EV_ARROW_END, at) == H_STOP) return H_STOP;
        p->expect = E_AFTER_BLOCK_ARROW;
        return H_TAKEN;
    }
    if (f->state == B_AFTER_RETURN) {
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "in staticjs a block ends with its return" });
    }
    if (t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_CONST) return begin_const(p, t);
    if (t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_RETURN) {
        const queued_token *next = peek(p, 1);
        if (next == NULL) return H_WAIT;
        if (next->newline_before && !is_punctuator(next, SJS_P_SEMICOLON) && !is_punctuator(next, SJS_P_RBRACE)) {
            return parse_fail(p, next, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "put the value on the line of its return" });
        }
        if (push_frame(p, F_RETURN, t->at) == H_STOP) return H_STOP;
        p->expect = E_OPERAND;
        return H_TAKEN;
    }
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "an arrow function's block holds const declarations and one return" });
}

static handled import_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    switch (f->state) {
    case I_OPEN:
        if (!is_punctuator(t, SJS_P_LBRACE)) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs imports names only: import { a, b as c } from \"m\";" });
        }
        f->state = I_NAME;
        return H_TAKEN;
    case I_NAME:
        if (is_punctuator(t, SJS_P_RBRACE)) {
            f->state = I_FROM;
            return H_TAKEN;
        }
        if (t->kind != SJS_TOKEN_NAME) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name to import" });
        if (hold_name(p, t) == H_STOP) return H_STOP;
        f->state = I_AS;
        return H_TAKEN;
    case I_AS:
    case I_ALIAS: {
        if (f->state == I_AS && t->kind == SJS_TOKEN_NAME && sjs_str_equal(token_text(p, t), SJS_STR("as"))) {
            f->state = I_ALIAS;
            return H_TAKEN;
        }
        sjs_event e = { 0 };
        e.kind = SJS_EV_IMPORT;
        e.alias = held_name(p);
        e.at = p->name_at;
        if (f->state == I_ALIAS) {
            if (!is_binding_name(t)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected a name to import as" });
            e.text = token_text(p, t);
            if (hold_event(p, &e) == H_STOP) return H_STOP;
            f->state = I_AFTER;
            return H_TAKEN;
        }
        if (p->name_keyword != SJS_K_NONE) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "this word cannot be bound" });
        e.text = held_name(p);
        if (hold_event(p, &e) == H_STOP) return H_STOP;
        f->state = I_AFTER;
        return H_AGAIN;
    }
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
        if (t->kind != SJS_TOKEN_NAME || !sjs_str_equal(token_text(p, t), SJS_STR("from"))) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected from" });
        }
        f->state = I_SOURCE;
        return H_TAKEN;
    case I_SOURCE: {
        if (t->kind != SJS_TOKEN_STRING) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected the module's name as a string" });
        /* Each held import learns its module: text is the local name, held
         * as `name`; the module goes in text. */
        uint32_t n = SJS_BUF_COUNT(p->held, held_event);
        sjs_str module = token_text(p, t);
        for (uint32_t i = 0; i < n; i++) {
            held_event h = SJS_BUF_AT(p->held, held_event, i);
            sjs_event e = { 0 };
            e.kind = SJS_EV_IMPORT;
            e.at = h.at;
            e.text = module;
            e.alias = (sjs_str){ (const char *)p->held_text.ptr + h.alias_offset, h.alias_length };
            e.name = (sjs_str){ (const char *)p->held_text.ptr + h.text_offset, h.text_length };
            sjs_result r = p->sink.v->event(p->sink.self, &e);
            if (r != SJS_OK) {
                p->failed = true;
                p->error.error.code = SJS_ERROR_NONE;
                return H_STOP;
            }
        }
        p->held.length = 0;
        p->held_text.length = 0;
        f->state = I_END;
        return H_TAKEN;
    }
    default: /* I_END */
        if (is_punctuator(t, SJS_P_SEMICOLON)) {
            pop_frame(p);
            return H_TAKEN;
        }
        if (t->kind == SJS_TOKEN_END) {
            pop_frame(p);
            return H_AGAIN;
        }
        return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "expected ; after an import" });
    }
}

static handled module_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (t->kind == SJS_TOKEN_END) {
        if (f->state == M_EXPORT) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "export needs a const declaration" });
        p->done = true;
        return send_kind(p, SJS_EV_END, t->at) == H_STOP ? H_STOP : H_TAKEN;
    }
    if (f->state == M_EXPORT) {
        if (t->kind != SJS_TOKEN_NAME || t->keyword != SJS_K_CONST) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs exports const declarations only" });
        }
        f->state = M_DECLARATIONS;
        if (send_kind(p, SJS_EV_EXPORT, t->at) == H_STOP) return H_STOP;
        return begin_const(p, t);
    }
    if (is_punctuator(t, SJS_P_SEMICOLON)) return H_TAKEN;
    if (t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_IMPORT) {
        if (f->state != M_IMPORTS) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "in staticjs imports come first" });
        if (push_frame(p, F_IMPORT, t->at) == H_STOP) return H_STOP;
        return H_TAKEN;
    }
    if (t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_EXPORT) {
        f->state = M_EXPORT;
        return H_TAKEN;
    }
    if (t->kind == SJS_TOKEN_NAME && t->keyword == SJS_K_CONST) {
        f->state = M_DECLARATIONS;
        return begin_const(p, t);
    }
    return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "a staticjs module holds imports and const declarations" });
}

static handled new_token(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f->state == N_NAME) {
        if (t->kind != SJS_TOKEN_NAME || !sjs_str_equal(token_text(p, t), SJS_STR("Set"))) {
            return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "staticjs constructs only Set" });
        }
        f->state = N_OPEN;
        return H_TAKEN;
    }
    if (!is_punctuator(t, SJS_P_LPAREN)) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_UNSUPPORTED, "write new Set(...) with parentheses" });
    f->type = F_CALL;
    f->is_new = 1;
    f->state = 0;
    p->expect = E_OPERAND;
    return send_event(p, texted(SJS_EV_NEW_BEGIN, SJS_STR("Set"), f->at));
}

/* ------------------------------------------------------------------------
 * Dispatch
 */

static handled dispatch(sjs_parser *p, const queued_token *t) {
    frame *f = top_frame(p);
    if (f == NULL) return parse_fail(p, t, (sjs_problem){ SJS_ERROR_SYNTAX, "unexpected token after the end" });
    switch (f->type) {
    case F_MODULE:
        return module_token(p, t);
    case F_BLOCK:
        return block_token(p, t);
    case F_IMPORT:
        return import_token(p, t);
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

static sjs_result on_token(void *self, const sjs_token *token) {
    sjs_parser *p = self;
    queued_token q = { 0 };
    q.kind = token->kind;
    q.punctuator = token->punctuator;
    q.keyword = token->keyword;
    q.newline_before = token->newline_before;
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
    if (push_frame(p, p->goal == SJS_GOAL_MODULE ? F_MODULE : F_EXPRESSION_GOAL, start) == H_STOP) {
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
    sjs_mem_free(&allocator, parser, sizeof *parser);
}
