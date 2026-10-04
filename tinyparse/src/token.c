/*
** token.c -- the token/lexical engine.
**
** Two jobs live here. First, compiling token patterns into IR: literal strings,
** the small regex subset (character classes with ranges and negation, `+'/`*'
** postfix, `.' for any), and the lexical combinators seq/repeat/not/any/choice.
** Second, matching that IR against input bytes -- the scanner's inner loop.
**
** The regex engine is owned outright: no libc regex, no locale, no
** backreferences, no lookbehind. Freestanding throughout; all IR is arena-born.
*/
#include "internal.h"

/* ** Allocate and zero-initialise an IR node of the given opcode. */
static tp_ir *tp_ir_new(tp_arena *arena, tp_op op)
{
    tp_ir *ir = (tp_ir *)tp_alloc(arena, sizeof *ir);
    if (ir == 0)
        return 0;
    ir->op          = op;
    ir->text        = tp_text("");
    ir->symbol      = TP_NO_SYMBOL;
    ir->min         = 0;
    ir->max         = 0;
    ir->children    = 0;
    ir->child_count = 0;
    {
        uint32_t i;
        ir->cls.negated = 0;
        for (i = 0; i < 32; i++)
            ir->cls.bits[i] = 0;
    }
    return ir;
}

/* ** Set the bit for one byte in a class. */
static void tp_class_set(tp_class *cls, uint8_t byte)
{
    cls->bits[byte >> 3] |= (uint8_t)(1u << (byte & 7));
}

/* ** Test membership in a class, honouring negation. */
static int tp_class_has(const tp_class *cls, uint8_t byte)
{
    int present = (cls->bits[byte >> 3] >> (byte & 7)) & 1;
    return cls->negated ? !present : present;
}

/* ** Decode one hex digit, or return 16 if the byte is not a hex digit. */
static uint32_t tp_hex_digit(uint8_t c)
{
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint32_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint32_t)(c - 'A' + 10);
    return 16;
}

/* ** Decode one regex byte, handling the escapes the token grammar uses inside
** classes and literals: \n \t \r \f \v \\ \] \^ \- a two-digit \xHH raw byte
** (so a class can name a high byte such as \x80 -- how a UTF-8 lead/continuation
** byte enters an identifier class, which the .scm string syntax cannot hold
** directly) and a literal-through default. Advances *pos past what it consumed. */
static uint8_t tp_regex_escape(const uint8_t *p, uint32_t length, uint32_t *pos)
{
    uint8_t c = p[*pos];
    *pos += 1;
    if (c != '\\')
        return c;
    if (*pos >= length)
        return '\\';
    {
        uint8_t e = p[*pos];
        *pos += 1;
        switch (e) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case 'f': return '\f';
        case 'v': return '\v';
        case '0': return '\0';
        case 'x':
            if (*pos + 1 < length) {
                uint32_t hi = tp_hex_digit(p[*pos]);
                uint32_t lo = tp_hex_digit(p[*pos + 1]);
                if (hi < 16 && lo < 16) {
                    *pos += 2;
                    return (uint8_t)((hi << 4) | lo);
                }
            }
            return 'x'; /* malformed \x: treat as a literal x */
        default:  return e; /* \\ \] \^ \- and the rest pass through */
        }
    }
}

/* ** Parse a `[...]' character class starting at *pos (which points just past
** the `['). Fills cls and advances *pos past the closing `]'. */
static int tp_regex_class(const uint8_t *p, uint32_t length, uint32_t *pos,
                          tp_class *cls, const char **error)
{
    uint32_t i = *pos;
    cls->negated = 0;
    if (i < length && p[i] == '^') {
        cls->negated = 1;
        i++;
    }
    while (i < length && p[i] != ']') {
        uint8_t lo = tp_regex_escape(p, length, &i);
        if (i + 1 < length && p[i] == '-' && p[i + 1] != ']') {
            uint8_t hi;
            i++; /* consume '-' */
            hi = tp_regex_escape(p, length, &i);
            {
                uint32_t b;
                for (b = lo; b <= hi; b++)
                    tp_class_set(cls, (uint8_t)b);
            }
        } else {
            tp_class_set(cls, lo);
        }
    }
    if (i >= length) {
        *error = "unterminated character class in regex";
        return 0;
    }
    i++; /* consume ']' */
    *pos = i;
    return 1;
}

/* ** Parse a regex-subset pattern into an IR sequence of atoms. Each atom is a
** class, a literal byte, or `.' (any), optionally followed by `+' or `*'. */
tp_ir *tp_compile_regex(tp_arena *arena, tinyparse_string pattern,
                        const char **error)
{
    const uint8_t *p = pattern.pointer;
    uint32_t length = pattern.length;
    uint32_t pos = 0;
    tp_ir   *seq = tp_ir_new(arena, TP_OP_SEQ);
    tp_ir  **atoms = 0;
    uint32_t count = 0, capacity = 0;

    if (seq == 0) {
        *error = "out of memory compiling regex";
        return 0;
    }

    while (pos < length) {
        tp_ir *atom;
        if (p[pos] == '[') {
            pos++;
            atom = tp_ir_new(arena, TP_OP_CLASS);
            if (atom == 0) { *error = "out of memory"; return 0; }
            if (!tp_regex_class(p, length, &pos, &atom->cls, error))
                return 0;
        } else if (p[pos] == '.') {
            pos++;
            atom = tp_ir_new(arena, TP_OP_ANY);
            if (atom == 0) { *error = "out of memory"; return 0; }
        } else {
            uint8_t byte = tp_regex_escape(p, length, &pos);
            atom = tp_ir_new(arena, TP_OP_CLASS);
            if (atom == 0) { *error = "out of memory"; return 0; }
            tp_class_set(&atom->cls, byte);
        }

        /* postfix quantifier */
        if (pos < length && (p[pos] == '+' || p[pos] == '*')) {
            tp_ir *repeat = tp_ir_new(arena, TP_OP_REPEAT);
            if (repeat == 0) { *error = "out of memory"; return 0; }
            repeat->min = (p[pos] == '+') ? 1 : 0;
            repeat->max = 0; /* unbounded */
            repeat->children = (tp_ir **)tp_alloc(arena, sizeof(tp_ir *));
            if (repeat->children == 0) { *error = "out of memory"; return 0; }
            repeat->children[0] = atom;
            repeat->child_count = 1;
            atom = repeat;
            pos++;
        }

        if (count == capacity) {
            uint32_t nc = capacity == 0 ? 8 : capacity * 2;
            tp_ir **grown = (tp_ir **)tp_alloc(arena, nc * sizeof *grown);
            uint32_t i;
            if (grown == 0) { *error = "out of memory"; return 0; }
            for (i = 0; i < count; i++)
                grown[i] = atoms[i];
            atoms = grown;
            capacity = nc;
        }
        atoms[count++] = atom;
    }

    seq->children    = atoms;
    seq->child_count = count;
    return seq;
}

/* ** A (when PARAM: X) whose parameter is unset compiles to this marker: an empty
** TP_OP_SEQ (zero children). An empty seq matches zero bytes and always succeeds,
** so a gated element wrapped in seq/choice/not simply vanishes -- and the children
** collector drops the marker outright, so it leaves no trace in the IR. */
static int tp_ir_is_vanished(const tp_ir *ir)
{
    return ir != 0 && ir->op == TP_OP_SEQ && ir->child_count == 0;
}

/* ** Build a 1-child IR node from a single sub-form. */
static tp_ir *tp_compile_unary(const tinyparse_engine *engine, tp_arena *arena,
                               tp_interner *interner, tp_op op,
                               const tp_form *child, const char **error,
                               uint32_t *error_byte)
{
    tp_ir *ir = tp_ir_new(arena, op);
    tp_ir *sub;
    if (ir == 0) { *error = "out of memory"; *error_byte = 0; return 0; }
    sub = tp_compile_pattern(engine, arena, interner, child, error, error_byte);
    if (sub == 0)
        return 0;
    ir->children = (tp_ir **)tp_alloc(arena, sizeof(tp_ir *));
    if (ir->children == 0) { *error = "out of memory"; return 0; }
    ir->children[0] = sub;
    ir->child_count = 1;
    return ir;
}

/* ** Compile a run of sub-forms [from, count) into an IR child vector. A child
** that vanished -- a (when PARAM: X) whose parameter is unset -- is dropped, so it
** truly leaves no element in the surrounding seq/choice/not, exactly as the design
** requires ("it vanishes from the sequence/choice"). */
static tp_ir **tp_compile_children(const tinyparse_engine *engine,
                                   tp_arena *arena, tp_interner *interner,
                                   tp_form *const *forms, uint32_t from,
                                   uint32_t count, uint32_t *out_count,
                                   const char **error, uint32_t *error_byte)
{
    uint32_t n = count - from;
    tp_ir  **out;
    uint32_t i;
    uint32_t kept = 0;
    out = (tp_ir **)tp_alloc(arena, (n == 0 ? 1 : n) * sizeof *out);
    if (out == 0) { *error = "out of memory"; *error_byte = 0; return 0; }
    for (i = 0; i < n; i++) {
        tp_ir *child = tp_compile_pattern(engine, arena, interner,
                                          forms[from + i], error, error_byte);
        if (child == 0)
            return 0;
        if (tp_ir_is_vanished(child))
            continue;
        out[kept++] = child;
    }
    *out_count = kept;
    return out;
}

/* ** Read an optional `min:'/`max:' keyword pair following a repeat's child.
** Returns 1 on success, writing min/max (max 0 means unbounded). */
static int tp_read_repeat_bounds(const tp_form *list, uint32_t child_index,
                                 uint32_t *out_min, uint32_t *out_max,
                                 const char **error, uint32_t *error_byte)
{
    uint32_t i = child_index;
    *out_min = 0;
    *out_max = 0;
    while (i + 1 < list->count) {
        const tp_form *key = list->items[i];
        const tp_form *val = list->items[i + 1];
        if (key->kind != TP_FORM_KEYWORD) {
            *error = "unexpected form in repeat";
            *error_byte = key->start_byte;
            return 0;
        }
        if (val->kind != TP_FORM_INTEGER) {
            *error = "repeat bound must be an integer";
            *error_byte = val->start_byte;
            return 0;
        }
        if (tp_string_equal_c(key->text, "min"))
            *out_min = (uint32_t)val->integer;
        else if (tp_string_equal_c(key->text, "max"))
            *out_max = (uint32_t)val->integer;
        else {
            *error = "unknown repeat keyword";
            *error_byte = key->start_byte;
            return 0;
        }
        i += 2;
    }
    return 1;
}

/* ** Compile a pattern form into IR. A string or bytes form is a literal; a
** symbol is a reference (or the special EOF/any); a list is a combinator head
** plus operands. Shared by token bodies and rule bodies. */
tp_ir *tp_compile_pattern(const tinyparse_engine *engine, tp_arena *arena,
                          tp_interner *interner, const tp_form *form,
                          const char **error, uint32_t *error_byte)
{
    *error = "";
    *error_byte = form->start_byte;

    if (form->kind == TP_FORM_STRING || form->kind == TP_FORM_BYTES) {
        tp_ir *ir = tp_ir_new(arena, TP_OP_LITERAL);
        if (ir == 0) { *error = "out of memory"; return 0; }
        ir->text = form->text;
        /* A bare literal's kind name is its own text. Resolve the intern id
        ** once here so the executor never interns on the parse hot path. */
        ir->symbol = tp_intern(interner, form->text);
        return ir;
    }

    if (form->kind == TP_FORM_SYMBOL) {
        if (tp_string_equal_c(form->text, "any"))
            return tp_ir_new(arena, TP_OP_ANY);
        if (tp_string_equal_c(form->text, "EOF"))
            return tp_ir_new(arena, TP_OP_EOF);
        {
            tp_ir *ir = tp_ir_new(arena, TP_OP_REF);
            if (ir == 0) { *error = "out of memory"; return 0; }
            ir->symbol = tp_intern(interner, form->text);
            return ir;
        }
    }

    if (form->kind != TP_FORM_LIST || form->count == 0) {
        *error = "expected a pattern";
        return 0;
    }

    {
        const tp_form *head = form->items[0];
        if (head->kind != TP_FORM_SYMBOL) {
            *error = "pattern head must be a symbol";
            *error_byte = head->start_byte;
            return 0;
        }

        if (tp_string_equal_c(head->text, "regex")) {
            if (form->count != 2 || form->items[1]->kind != TP_FORM_STRING) {
                *error = "regex expects one string";
                return 0;
            }
            return tp_compile_regex(arena, form->items[1]->text, error);
        }

        if (tp_string_equal_c(head->text, "class")) {
            /* (class NAME): match one byte iff the host's CLASS binding NAME
            ** accepts it. The class name is held in the IR's text slice -- it is
            ** the binding name the matcher resolves against the engine, not a
            ** byte literal -- so it round-trips through pack.c with every other
            ** op's text and needs no new on-disk field. */
            tp_ir *ir;
            const tp_form *named;
            if (form->count != 2) {
                *error = "class expects one name";
                return 0;
            }
            named = form->items[1];
            if (named->kind != TP_FORM_SYMBOL) {
                *error = "class name must be a symbol";
                *error_byte = named->start_byte;
                return 0;
            }
            ir = tp_ir_new(arena, TP_OP_CLASS_NAME);
            if (ir == 0) { *error = "out of memory"; return 0; }
            ir->text = named->text;
            return ir;
        }

        if (tp_string_equal_c(head->text, "seq") ||
            tp_string_equal_c(head->text, "choice")) {
            tp_op op = tp_string_equal_c(head->text, "seq")
                           ? TP_OP_SEQ : TP_OP_CHOICE;
            tp_ir *ir = tp_ir_new(arena, op);
            if (ir == 0) { *error = "out of memory"; return 0; }
            ir->children = tp_compile_children(engine, arena, interner,
                                               form->items, 1, form->count,
                                               &ir->child_count,
                                               error, error_byte);
            if (ir->children == 0)
                return 0;
            return ir;
        }

        if (tp_string_equal_c(head->text, "when")) {
            /* (when PARAM: X): contribute X only when the compile-time parameter
            ** PARAM is set; otherwise vanish. An unset gate compiles to an empty
            ** seq -- a zero-width always-match -- which the children collector
            ** drops, so the gated element disappears from its seq/choice. The
            ** grammar still compiles and validates with the parameter unset. */
            const tp_form *key;
            if (form->count != 3) {
                *error = "when expects PARAM: and one element";
                return 0;
            }
            key = form->items[1];
            if (key->kind != TP_FORM_KEYWORD) {
                *error = "when parameter must be a keyword";
                *error_byte = key->start_byte;
                return 0;
            }
            if (!tp_engine_param_set(engine, key->text))
                return tp_ir_new(arena, TP_OP_SEQ); /* vanish */
            return tp_compile_pattern(engine, arena, interner, form->items[2],
                                      error, error_byte);
        }

        if (tp_string_equal_c(head->text, "repeat") ||
            tp_string_equal_c(head->text, "repeat1")) {
            tp_ir *ir = tp_ir_new(arena, TP_OP_REPEAT);
            tp_ir *child;
            if (ir == 0) { *error = "out of memory"; return 0; }
            if (form->count < 2) { *error = "repeat needs a child"; return 0; }
            child = tp_compile_pattern(engine, arena, interner, form->items[1],
                                       error, error_byte);
            if (child == 0)
                return 0;
            ir->children = (tp_ir **)tp_alloc(arena, sizeof(tp_ir *));
            if (ir->children == 0) { *error = "out of memory"; return 0; }
            ir->children[0] = child;
            ir->child_count = 1;
            if (!tp_read_repeat_bounds(form, 2, &ir->min, &ir->max,
                                       error, error_byte))
                return 0;
            if (tp_string_equal_c(head->text, "repeat1") && ir->min == 0)
                ir->min = 1;
            return ir;
        }

        if (tp_string_equal_c(head->text, "goal")) {
            /* (goal NAME X): select NAME as the active lexical goal while
            ** matching X. A rule combinator, but it may nest inside seq/choice/
            ** repeat (e.g. the binary `/` operator below a seq), so it lives in
            ** the shared pattern compiler. NAME rides in the IR's symbol; X is the
            ** sole child. The executor sets/restores the goal around the child. */
            tp_ir *ir;
            tp_ir *child;
            if (form->count != 3 || form->items[1]->kind != TP_FORM_SYMBOL) {
                *error = "goal expects a goal name and one child";
                *error_byte = head->start_byte;
                return 0;
            }
            ir = tp_ir_new(arena, TP_OP_GOAL);
            if (ir == 0) { *error = "out of memory"; return 0; }
            child = tp_compile_pattern(engine, arena, interner, form->items[2],
                                       error, error_byte);
            if (child == 0)
                return 0;
            ir->symbol = tp_intern(interner, form->items[1]->text);
            ir->children = (tp_ir **)tp_alloc(arena, sizeof(tp_ir *));
            if (ir->children == 0) { *error = "out of memory"; return 0; }
            ir->children[0] = child;
            ir->child_count = 1;
            return ir;
        }

        if (tp_string_equal_c(head->text, "optional")) {
            if (form->count != 2) { *error = "optional needs one child"; return 0; }
            return tp_compile_unary(engine, arena, interner, TP_OP_OPTIONAL,
                                    form->items[1], error, error_byte);
        }

        if (tp_string_equal_c(head->text, "not")) {
            /* (not X any): match `any' but not when X matches. */
            tp_ir *ir = tp_ir_new(arena, TP_OP_NOT);
            if (ir == 0) { *error = "out of memory"; return 0; }
            if (form->count < 2) { *error = "not needs operands"; return 0; }
            ir->children = tp_compile_children(engine, arena, interner,
                                               form->items, 1, form->count,
                                               &ir->child_count,
                                               error, error_byte);
            if (ir->children == 0)
                return 0;
            return ir;
        }

        if (tp_string_equal_c(head->text, "reserved")) {
            /* (reserved INNER "w1" "w2" ...): match INNER, then reject the
            ** match if its byte slice exactly spells one of the reserved words.
            ** This is keyword reservation for a word token: an identifier
            ** pattern wrapped here will not swallow a reserved spelling, so a
            ** keyword-led rule is reached instead of being eaten as a plain
            ** identifier. INNER is child[0]; each reserved word is a literal
            ** child, so the node round-trips through the IR serializer unchanged.
            */
            tp_ir   *ir;
            tp_ir   *inner;
            tp_ir  **kids;
            uint32_t words = form->count - 2;
            uint32_t k;
            if (form->count < 3) {
                *error = "reserved expects an inner pattern and word(s)";
                *error_byte = head->start_byte;
                return 0;
            }
            ir = tp_ir_new(arena, TP_OP_NOT_KEYWORD);
            if (ir == 0) { *error = "out of memory"; return 0; }
            inner = tp_compile_pattern(engine, arena, interner, form->items[1],
                                       error, error_byte);
            if (inner == 0)
                return 0;
            kids = (tp_ir **)tp_alloc(arena, (words + 1) * sizeof *kids);
            if (kids == 0) { *error = "out of memory"; return 0; }
            kids[0] = inner;
            for (k = 0; k < words; k++) {
                const tp_form *w = form->items[2 + k];
                tp_ir *lit;
                if (w->kind != TP_FORM_STRING && w->kind != TP_FORM_SYMBOL) {
                    *error = "reserved word must be a string or symbol";
                    *error_byte = w->start_byte;
                    return 0;
                }
                lit = tp_ir_new(arena, TP_OP_LITERAL);
                if (lit == 0) { *error = "out of memory"; return 0; }
                lit->text = w->text;
                kids[1 + k] = lit;
            }
            ir->children    = kids;
            ir->child_count = words + 1;
            return ir;
        }

        if (tp_string_equal_c(head->text, "inject")) {
            /* (inject grammar: NAME content: REGION): match REGION (the span
            ** delimiter), then re-parse the matched span with the grammar named
            ** NAME and splice that tree under an injection node. NAME is resolved
            ** and compiled ONCE here, at compile time, through the engine's module
            ** resolver -- the same seam (import NAME) uses -- so the parse path
            ** never reads the filesystem. The compiled embedded grammar and the
            ** language tag (NAME interned into the host interner) ride on the IR
            ** node; the executor (parse.c / vm.c) runs the inline sub-VM. */
            const tp_form *grammar_key, *grammar_name, *content_key, *region_form;
            tp_ir        *ir;
            tp_ir        *region;
            tinyparse_grammar *embedded = 0;
            if (form->count != 5) {
                *error = "inject expects grammar: NAME content: REGION";
                *error_byte = head->start_byte;
                return 0;
            }
            grammar_key  = form->items[1];
            grammar_name = form->items[2];
            content_key  = form->items[3];
            region_form  = form->items[4];
            if (grammar_key->kind != TP_FORM_KEYWORD ||
                !tp_string_equal_c(grammar_key->text, "grammar") ||
                grammar_name->kind != TP_FORM_SYMBOL) {
                *error = "inject needs grammar: NAME";
                *error_byte = grammar_key->start_byte;
                return 0;
            }
            if (content_key->kind != TP_FORM_KEYWORD ||
                !tp_string_equal_c(content_key->text, "content")) {
                *error = "inject needs content: REGION";
                *error_byte = content_key->start_byte;
                return 0;
            }
            /* Resolve and compile the embedded grammar. The engine is logically
            ** const here, but resolving a grammar by name is the same mutating
            ** compile (import NAME) performs, on the same engine and arena, so the
            ** const is cast away for this one call -- as elsewhere the engine's
            ** arena and fault are mutable. */
            if (!tp_resolve_embedded_grammar(
                    (tinyparse_engine *)engine, grammar_name->text,
                    &embedded, error)) {
                *error_byte = grammar_name->start_byte;
                return 0;
            }
            region = tp_compile_pattern(engine, arena, interner, region_form,
                                        error, error_byte);
            if (region == 0)
                return 0;
            ir = tp_ir_new(arena, TP_OP_INJECT);
            if (ir == 0) { *error = "out of memory"; return 0; }
            ir->children = (tp_ir **)tp_alloc(arena, sizeof(tp_ir *));
            if (ir->children == 0) { *error = "out of memory"; return 0; }
            ir->children[0]      = region;
            ir->child_count      = 1;
            ir->embedded_grammar = embedded;
            ir->language_symbol  = tp_intern(interner, grammar_name->text);
            return ir;
        }

        if (tp_string_equal_c(head->text, "field")) {
            const tp_form *key;
            tp_ir *ir;
            if (form->count != 3) { *error = "field expects name: and a child"; return 0; }
            key = form->items[1];
            if (key->kind != TP_FORM_KEYWORD) {
                *error = "field name must be a keyword";
                *error_byte = key->start_byte;
                return 0;
            }
            ir = tp_compile_unary(engine, arena, interner, TP_OP_FIELD,
                                  form->items[2], error, error_byte);
            if (ir == 0)
                return 0;
            ir->symbol = tp_intern(interner, key->text);
            return ir;
        }

        if (tp_string_equal_c(head->text, "alias")) {
            const tp_form *key;
            tp_ir *ir;
            if (form->count != 3) { *error = "alias expects a name and a child"; return 0; }
            key = form->items[1];
            if (key->kind != TP_FORM_KEYWORD && key->kind != TP_FORM_SYMBOL) {
                *error = "alias name must be a symbol or keyword";
                *error_byte = key->start_byte;
                return 0;
            }
            ir = tp_compile_unary(engine, arena, interner, TP_OP_ALIAS,
                                  form->items[2], error, error_byte);
            if (ir == 0)
                return 0;
            ir->symbol = tp_intern(interner, key->text);
            return ir;
        }

        if (tp_string_equal_c(head->text, "deferred-slot")) {
            /* (deferred-slot NAME): declare a per-invocation FIFO slot named NAME.
            ** Zero-width: it matches no input and emits no node. NAME rides in the
            ** IR's symbol; the node carries no children
            ** (lex-goal-implementation, "Grammar forms"). */
            tp_ir *ir;
            if (form->count != 2 || form->items[1]->kind != TP_FORM_SYMBOL) {
                *error = "deferred-slot needs a slot name";
                *error_byte = head->start_byte;
                return 0;
            }
            ir = tp_ir_new(arena, TP_OP_DEFERRED_SLOT);
            if (ir == 0) { *error = "out of memory"; return 0; }
            ir->symbol = tp_intern(interner, form->items[1]->text);
            return ir;
        }

        if (tp_string_equal_c(head->text, "defer")) {
            /* (defer SLOT EXPR): match EXPR, capture its byte span, enqueue it on
            ** the named SLOT of the innermost enclosing rule invocation. SLOT rides
            ** in the IR's symbol; EXPR compiles to the sole child so it links
            ** through the normal IR machinery (lex-goal-implementation). */
            tp_ir *ir;
            tp_ir *child;
            if (form->count != 3 || form->items[1]->kind != TP_FORM_SYMBOL) {
                *error = "defer needs a slot name and a pattern";
                *error_byte = head->start_byte;
                return 0;
            }
            ir = tp_ir_new(arena, TP_OP_DEFER);
            if (ir == 0) { *error = "out of memory"; return 0; }
            child = tp_compile_pattern(engine, arena, interner, form->items[2],
                                       error, error_byte);
            if (child == 0)
                return 0;
            ir->symbol = tp_intern(interner, form->items[1]->text);
            ir->children = (tp_ir **)tp_alloc(arena, sizeof(tp_ir *));
            if (ir->children == 0) { *error = "out of memory"; return 0; }
            ir->children[0] = child;
            ir->child_count = 1;
            return ir;
        }

        if (tp_string_equal_c(head->text, "parse-deferred")) {
            /* (parse-deferred SLOT RULE): drain the named SLOT's FIFO, invoking RULE
            ** once per item. SLOT rides in the IR's symbol. The RULE name cannot be
            ** resolved to a rule index here -- the rule table is still being built --
            ** so the unresolved name is STASHED in the IR's text slice and the
            ** grammar finalizer (tinyparse_compile_scheme) later resolves it into
            ** min (rule_index) and max (the compact parametrized goal index). Until
            ** then min/max stay zero; no children
            ** (lex-goal-implementation, "Grammar forms: parse-deferred"). */
            tp_ir *ir;
            if (form->count != 3 ||
                form->items[1]->kind != TP_FORM_SYMBOL ||
                form->items[2]->kind != TP_FORM_SYMBOL) {
                *error = "parse-deferred needs a slot name and a rule name";
                *error_byte = head->start_byte;
                return 0;
            }
            ir = tp_ir_new(arena, TP_OP_PARSE_DEFERRED);
            if (ir == 0) { *error = "out of memory"; return 0; }
            ir->symbol = tp_intern(interner, form->items[1]->text);
            ir->text   = form->items[2]->text; /* pending rule name; resolved later */
            ir->min    = 0;
            ir->max    = 0;
            return ir;
        }

        *error = "unknown pattern combinator";
        *error_byte = head->start_byte;
        return 0;
    }
}

/* ** Recursively copy a (param NAME BODY) template node into `arena', replacing
** every TP_OP_PARAM_REF leaf with a freshly minted TP_OP_LITERAL of the captured
** bytes. The template lives in the immutable grammar arena (compiled once); the
** copy lives in the parse arena for the whole parse and is mutable, so the lazy
** token-program / first-byte lowering writes back into it safely. tp_ir_new zeroes
** the cache fields, so the copy starts with a clean lowering state and lowers its
** own concrete program. A TP_OP_PARAM_REF becomes a literal whose bytes are COPIED
** into the parse arena (so they survive a sliding stream window); its symbol is
** TP_NO_SYMBOL -- the emitted leaf KIND comes from the goal trigger at the redirect
** site, not from this literal. Returns 0 on any allocation failure (the caller
** degrades to no-pattern -> no-match). See tp_param_instantiate. */
static tp_ir *tp_param_copy(tp_arena *arena, const tp_ir *node,
                            const uint8_t *text, uint32_t text_len)
{
    tp_ir   *copy;
    uint32_t i;

    if (node == 0)
        return 0;

    if (node->op == TP_OP_PARAM_REF) {
        /* Mint a literal of the captured bytes. text_len may be 0 (a degenerate
        ** empty delimiter); copy the bytes into the parse arena so they outlive any
        ** input buffer that may move (streaming). tp_ir_new sets text to "" already;
        ** overwrite it with the captured slice. */
        tp_ir   *lit = tp_ir_new(arena, TP_OP_LITERAL);
        uint8_t *bytes;
        if (lit == 0)
            return 0;
        bytes = (uint8_t *)tp_alloc(arena, text_len == 0 ? 1 : text_len);
        if (bytes == 0)
            return 0;
        for (i = 0; i < text_len; i++)
            bytes[i] = text[i];
        lit->text.pointer = bytes;
        lit->text.length  = text_len;
        lit->symbol       = TP_NO_SYMBOL;
        lit->min          = 0;
        lit->max          = 0;
        return lit;
    }

    copy = tp_ir_new(arena, node->op);
    if (copy == 0)
        return 0;
    /* Template text/cls live in the immutable grammar arena and are never mutated;
    ** aliasing the pointer/len (rather than copying) is safe. */
    copy->text   = node->text;
    copy->cls    = node->cls;
    copy->symbol = node->symbol;
    copy->min    = node->min;
    copy->max    = node->max;
    if (node->child_count > 0) {
        copy->children = (tp_ir **)tp_alloc(
            arena, node->child_count * sizeof *copy->children);
        if (copy->children == 0)
            return 0;
        copy->child_count = node->child_count;
        for (i = 0; i < node->child_count; i++) {
            copy->children[i] = tp_param_copy(arena, node->children[i],
                                              text, text_len);
            if (copy->children[i] == 0)
                return 0;
        }
    }
    return copy;
}

/* ** Build a concrete pattern from a (param NAME BODY) template by copying the
** template rule's IR and replacing every TP_OP_PARAM_REF leaf with a TP_OP_LITERAL
** of the captured bytes. `rule_index' names the internal template rule the (param
** ...) compiler appended to grammar->rules[]; its body is the template IR. `arena'
** is the parse arena, so the result lives for the whole parse and is mutable
** (token_program lowers into it lazily). Returns 0 on a bad index or any allocation
** failure -- callers degrade to no-pattern -> no-match (lex-goal-implementation,
** "Pattern instantiation"). */
const tp_ir *tp_param_instantiate(tp_arena *arena, const tinyparse_grammar *grammar,
                                  uint32_t rule_index, const uint8_t *text,
                                  uint32_t text_len)
{
    if (grammar == 0 || rule_index >= grammar->rule_count)
        return 0;
    return tp_param_copy(arena, grammar->rules[rule_index].body, text, text_len);
}

/* ** Match a literal: the input must hold the exact bytes at `start'. */
static int tp_match_literal(const tp_ir *ir, const uint8_t *input,
                            uint32_t length, uint32_t start, uint32_t *out_end)
{
    uint32_t i;
    if (start + ir->text.length > length)
        return 0;
    for (i = 0; i < ir->text.length; i++)
        if (input[start + i] != ir->text.pointer[i])
            return 0;
    *out_end = start + ir->text.length;
    return 1;
}

/* ** First-byte dispatch (Phase 10). A token retried at the same offset by an
** ordered choice re-runs the whole IR interpreter over bytes that cannot match.
** A cheap pre-filter prunes most of those: compute, once per pattern node, the
** set of bytes the pattern can legally begin with, and skip the match outright
** when the current input byte is not in that set.
**
** tp_first_bytes accumulates a pattern's lead-byte set into `out' and returns 1
** when the pattern is "open": either it can match the empty string (so any byte,
** including none, may follow and pruning would be unsound) or its lead byte
** cannot be pinned down (host (class NAME), `any', a `not' body). An open
** pattern disables pruning. The result is cached on the node so the walk runs
** at most once per distinct pattern. */
static int tp_first_bytes(const tp_ir *ir, uint8_t out[32]);

/* ** OR every byte the class accepts into the lead-byte set. A class is a finite
** 256-entry test, so honour negation by probing each byte. */
static void tp_first_bytes_class(const tp_class *cls, uint8_t out[32])
{
    uint32_t b;
    for (b = 0; b < 256; b++)
        if (tp_class_has(cls, (uint8_t)b))
            out[b >> 3] |= (uint8_t)(1u << (b & 7));
}

/* ** Mark every byte as possible (pruning off). */
static void tp_first_bytes_all(uint8_t out[32])
{
    uint32_t i;
    for (i = 0; i < 32; i++)
        out[i] = 0xFF;
}

static int tp_first_bytes(const tp_ir *ir, uint8_t out[32])
{
    switch (ir->op) {
    case TP_OP_LITERAL:
        if (ir->text.length == 0)
            return 1; /* empty literal matches zero bytes: open */
        out[ir->text.pointer[0] >> 3] |=
            (uint8_t)(1u << (ir->text.pointer[0] & 7));
        return 0;

    case TP_OP_CLASS:
        tp_first_bytes_class(&ir->cls, out);
        return 0;

    case TP_OP_ANY:
    case TP_OP_CLASS_NAME:
    case TP_OP_NOT:        /* matches a byte outside the guard: lead unknown */
    case TP_OP_EOF:
    case TP_OP_REF:        /* never a top-level token pattern; be conservative */
        tp_first_bytes_all(out);
        return 1;

    case TP_OP_SEQ: {
        uint32_t i;
        for (i = 0; i < ir->child_count; i++)
            if (!tp_first_bytes(ir->children[i], out))
                return 0; /* first non-nullable child fixes the lead set */
        return 1; /* every child was nullable: the seq is nullable */
    }

    case TP_OP_CHOICE: {
        uint32_t i;
        int open = 0;
        for (i = 0; i < ir->child_count; i++)
            if (tp_first_bytes(ir->children[i], out))
                open = 1; /* a nullable branch makes the choice nullable */
        return open;
    }

    case TP_OP_REPEAT:
        if (ir->child_count >= 1)
            tp_first_bytes(ir->children[0], out);
        return ir->min == 0; /* min 0 can match nothing: open */

    case TP_OP_OPTIONAL:
        if (ir->child_count >= 1)
            tp_first_bytes(ir->children[0], out);
        return 1; /* optional matches zero bytes: open */

    case TP_OP_NOT_KEYWORD:
        /* Reserved-word rejection only narrows; the lead set is the inner
        ** pattern's, the nullability the inner pattern's. */
        if (ir->child_count >= 1)
            return tp_first_bytes(ir->children[0], out);
        tp_first_bytes_all(out);
        return 1;

    case TP_OP_GOAL:
    case TP_OP_FIELD:
    case TP_OP_ALIAS:
        if (ir->child_count >= 1)
            return tp_first_bytes(ir->children[0], out);
        tp_first_bytes_all(out);
        return 1;

    default:
        tp_first_bytes_all(out);
        return 1;
    }
}

/* ** Decide whether the byte at `start' could begin a match of `ir', using the
** cached lead-byte table. Returns 1 when a match is possible (so the caller must
** run the interpreter), 0 when it is provably impossible. The table is computed
** lazily on first use and cached on the node. An "open" pattern (nullable or
** lead-unknown) always returns 1, so behaviour is unchanged where pruning would
** be unsound. */
static int tp_first_byte_admits(tp_ir *ir, const uint8_t *input,
                                uint32_t length, uint32_t start)
{
    if (ir->first_byte_state == 0) {
        uint8_t set[32];
        uint32_t i;
        int open;
        for (i = 0; i < 32; i++)
            set[i] = 0;
        open = tp_first_bytes(ir, set);
        if (open) {
            ir->first_byte_any = 1;
        } else {
            ir->first_byte_any = 0;
            for (i = 0; i < 32; i++)
                ir->first_byte_set[i] = set[i];
        }
        ir->first_byte_state = 1;
    }
    if (ir->first_byte_any)
        return 1;
    if (start >= length)
        return 0; /* a non-nullable pattern needs at least one byte */
    {
        uint8_t b = input[start];
        return (ir->first_byte_set[b >> 3] >> (b & 7)) & 1;
    }
}

/* ** Build the grammar's extras lead-byte fast-path set (W16-1), caching it on the
** grammar. tp_vm_consume_extras runs before every terminal, so at the dominant
** "no extra at this byte" position it should reject in O(1) rather than looping
** over every extras entry and running a full token match for each. This unions
** each extra token's first-byte lead set; if any extra's lead byte cannot be
** pinned down (a nullable or lead-unknown pattern -- the same "open" notion the
** node-level first-byte cache uses) the fast path is disabled (`extras_lead_open')
** and the scanner keeps its full loop. Computed once, then reused: the set is a
** pure function of the extras table + token patterns. Modifies the grammar's
** cache fields (the grammar arena owns them), like tp_first_byte_admits caches on
** its node. */
void tp_extras_lead_build(tinyparse_grammar *grammar)
{
    uint32_t i;
    uint8_t set[32];
    if (grammar->extras_lead_state)
        return;
    for (i = 0; i < 32; i++)
        set[i] = 0;
    grammar->extras_lead_open = 0;
    for (i = 0; i < grammar->extra_count && !grammar->extras_lead_open; i++) {
        uint32_t sym = grammar->extras[i].token_symbol;
        const tp_token_def *token = 0;
        uint32_t t;
        tp_ir *pattern;
        for (t = 0; t < grammar->token_count; t++)
            if (grammar->tokens[t].symbol == sym) {
                token = &grammar->tokens[t];
                break;
            }
        if (token == 0)
            continue; /* an unresolved extra contributes no lead byte */
        pattern = token->pattern;
        /* Reuse the node-level lead cache so the union shares its computation and
        ** its "open" verdict. An open extra means some byte it begins with cannot
        ** be pinned down, so no lead-byte pre-filter is sound: fall back. */
        if (pattern->first_byte_state == 0) {
            uint8_t one[32];
            uint32_t j;
            int open;
            for (j = 0; j < 32; j++)
                one[j] = 0;
            open = tp_first_bytes(pattern, one);
            if (open) {
                pattern->first_byte_any = 1;
            } else {
                pattern->first_byte_any = 0;
                for (j = 0; j < 32; j++)
                    pattern->first_byte_set[j] = one[j];
            }
            pattern->first_byte_state = 1;
        }
        if (pattern->first_byte_any) {
            grammar->extras_lead_open = 1;
        } else {
            uint32_t j;
            for (j = 0; j < 32; j++)
                set[j] |= pattern->first_byte_set[j];
        }
    }
    if (!grammar->extras_lead_open) {
        for (i = 0; i < 32; i++)
            grammar->extras_lead[i] = set[i];
    }
    grammar->extras_lead_state = 1;
}

/* ** Resolving FIRST-set computation for ordered-choice prediction (W46).
**
** tp_first_bytes above is local: it treats every (ref ...) as open because it
** has no grammar to resolve the reference against. For choice prediction that is
** too coarse -- a `statement' rule is a choice over ~19 sub-rules, each beginning
** with a distinct keyword, and pruning the ones whose lead byte cannot match is
** the whole win. So this variant carries the grammar and follows references into
** the referenced rule's body or token pattern, accumulating their lead bytes.
**
** SOUNDNESS is the contract: the function returns 1 ("open") whenever it cannot
** prove an exact, closed lead set, and an open node disables pruning. It returns
** open for nullability (the element can match nothing, so the next thing decides
** the lead byte), for any lead-unknown primitive (any/class-name/not), and -- the
** subtle one -- for any reference it cannot resolve or that it is already
** resolving (left recursion). The `visiting' stack carries the chain of rule
** symbols currently being expanded; re-entering one means a cycle, which we treat
** as open rather than looping. A reference whose symbol names neither a rule nor a
** token is unresolved, hence open. With every uncertainty mapped to open, a
** closed set is exact: every byte that can begin a match is in it, so a lookahead
** byte outside it provably cannot start this alternative. */
static int tp_first_set(const tinyparse_grammar *grammar, const tp_ir *ir,
                        uint8_t out[32], const uint32_t *visiting,
                        uint32_t visiting_count);

/* ** Resolve a reference's FIRST set by expanding the named rule or token. The
** symbol is looked up among rules first, then tokens; an unresolved symbol is
** open. A rule already on the `visiting' stack is a cycle -- open. */
static int tp_first_set_ref(const tinyparse_grammar *grammar, uint32_t symbol,
                            uint8_t out[32], const uint32_t *visiting,
                            uint32_t visiting_count)
{
    uint32_t i;
    const tp_rule_def  *rule  = 0;
    const tp_token_def *token = 0;
    for (i = 0; i < grammar->rule_count; i++)
        if (grammar->rules[i].symbol == symbol) { rule = &grammar->rules[i]; break; }
    if (rule != 0) {
        uint32_t *stack;
        uint32_t  n;
        for (i = 0; i < visiting_count; i++)
            if (visiting[i] == symbol)
                return 1; /* left recursion: cannot bound, stay open */
        /* The visiting chain grows one symbol per resolved reference, so a deeply
        ** nested grammar can chain arbitrarily deep before hitting left recursion.
        ** Carve the extended chain from the grammar arena rather than a fixed
        ** stack -- a fixed cap would silently fall back to "open" (no pruning) on a
        ** deep grammar. The chain is bounded by the rule count (a symbol repeats
        ** only on a cycle, which returns above), so the allocation is bounded; the
        ** block is reclaimed with the arena. */
        stack = (uint32_t *)tp_alloc(grammar->arena,
                                     (visiting_count + 1) * sizeof *stack);
        if (stack == 0)
            return 1; /* arena exhausted: stay open, which is always sound */
        for (n = 0; n < visiting_count; n++)
            stack[n] = visiting[n];
        stack[visiting_count] = symbol;
        return tp_first_set(grammar, rule->body, out, stack, visiting_count + 1);
    }
    for (i = 0; i < grammar->token_count; i++)
        if (grammar->tokens[i].symbol == symbol) { token = &grammar->tokens[i]; break; }
    if (token != 0)
        return tp_first_set(grammar, token->pattern, out, visiting, visiting_count);
    return 1; /* names neither rule nor token: unresolved, open */
}

static int tp_first_set(const tinyparse_grammar *grammar, const tp_ir *ir,
                        uint8_t out[32], const uint32_t *visiting,
                        uint32_t visiting_count)
{
    switch (ir->op) {
    case TP_OP_LITERAL:
        if (ir->text.length == 0)
            return 1; /* empty literal: nullable, open */
        out[ir->text.pointer[0] >> 3] |=
            (uint8_t)(1u << (ir->text.pointer[0] & 7));
        return 0;

    case TP_OP_CLASS:
        tp_first_bytes_class(&ir->cls, out);
        return 0;

    case TP_OP_REF:
        return tp_first_set_ref(grammar, ir->symbol, out, visiting,
                                visiting_count);

    case TP_OP_ANY:
    case TP_OP_CLASS_NAME:
    case TP_OP_NOT:
    case TP_OP_EOF:
        tp_first_bytes_all(out);
        return 1; /* lead byte cannot be pinned down: open */

    case TP_OP_SEQ: {
        uint32_t i;
        for (i = 0; i < ir->child_count; i++)
            if (!tp_first_set(grammar, ir->children[i], out, visiting,
                              visiting_count))
                return 0; /* first non-nullable child fixes the lead set */
        return 1; /* every child nullable: the seq is nullable, open */
    }

    case TP_OP_CHOICE: {
        uint32_t i;
        int open = 0;
        for (i = 0; i < ir->child_count; i++)
            if (tp_first_set(grammar, ir->children[i], out, visiting,
                             visiting_count))
                open = 1; /* a nullable branch makes the whole choice nullable */
        return open;
    }

    case TP_OP_REPEAT:
        if (ir->child_count >= 1)
            tp_first_set(grammar, ir->children[0], out, visiting, visiting_count);
        return ir->min == 0; /* min 0 can match nothing: open */

    case TP_OP_OPTIONAL:
        if (ir->child_count >= 1)
            tp_first_set(grammar, ir->children[0], out, visiting, visiting_count);
        return 1; /* matches zero bytes: open */

    case TP_OP_RECOVER:
        /* The body may synthesize a missing terminal at end of input, so it is
        ** effectively nullable there. Its lead set is the body's; nullable. */
        if (ir->child_count >= 1)
            tp_first_set(grammar, ir->children[0], out, visiting, visiting_count);
        return 1;

    case TP_OP_NOT_KEYWORD:
    case TP_OP_GOAL:
    case TP_OP_FIELD:
    case TP_OP_ALIAS:
    case TP_OP_NO_EXTRAS:
    case TP_OP_INJECT:
        /* INJECT begins by matching its REGION sub-pattern (child[0]), so its lead
        ** set is the region's -- exactly like the other single-child wrappers. */
        if (ir->child_count >= 1)
            return tp_first_set(grammar, ir->children[0], out, visiting,
                                visiting_count);
        tp_first_bytes_all(out);
        return 1;

    default:
        tp_first_bytes_all(out);
        return 1; /* unknown op: conservatively open */
    }
}

/* ** Decide whether the byte at `start' could begin a match of choice alternative
** `ir', using the resolving FIRST set cached on the node (computed lazily on first
** use). Returns 1 when the alternative might match (so the caller must try it), 0
** only when its lead byte provably excludes the lookahead. An open alternative
** (nullable or lead-unknown) always returns 1, so pruning is sound by
** construction. `start' is the position AFTER extras have been consumed, so the
** lookahead byte is the first significant byte the alternative would see. */
int tp_choice_first_admits(const tinyparse_grammar *grammar, tp_ir *ir,
                           const uint8_t *input, uint32_t length, uint32_t start)
{
    if (ir->choice_first_state == 0) {
        uint8_t set[32];
        uint32_t i;
        int open;
        for (i = 0; i < 32; i++)
            set[i] = 0;
        open = tp_first_set(grammar, ir, set, 0, 0);
        if (open) {
            ir->choice_first_open = 1;
        } else {
            ir->choice_first_open = 0;
            for (i = 0; i < 32; i++)
                ir->choice_first_set[i] = set[i];
        }
        ir->choice_first_state = 1;
    }
    if (ir->choice_first_open)
        return 1;
    if (start >= length)
        return 0; /* a non-nullable alternative needs at least one byte */
    {
        uint8_t b = input[start];
        return (ir->choice_first_set[b >> 3] >> (b & 7)) & 1;
    }
}

/* ** Match one IR atom for the lexer, greedily. Token IR is a restricted
** subset (literals, classes, any, seq, repeat, not, choice), all left-to-right
** and greedy, which is sufficient for the V1 token language. The engine is
** threaded through so a (class NAME) atom can resolve NAME to its host CLASS
** binding and call it; it may be NULL when no engine is in scope, in which case a
** named class matches nothing. */
static int tp_match_ir(const tinyparse_engine *engine, const tp_ir *ir,
                       const uint8_t *input, uint32_t length,
                       uint32_t start, uint32_t *out_end)
{
    switch (ir->op) {
    case TP_OP_LITERAL:
        return tp_match_literal(ir, input, length, start, out_end);

    case TP_OP_ANY:
        if (start >= length)
            return 0;
        *out_end = start + 1;
        return 1;

    case TP_OP_CLASS:
        if (start >= length)
            return 0;
        if (!tp_class_has(&ir->cls, input[start]))
            return 0;
        *out_end = start + 1;
        return 1;

    case TP_OP_CLASS_NAME: {
        /* (class NAME): one byte the host CLASS binding NAME accepts. Resolve
        ** the binding by name against the engine and call its membership test
        ** for the byte at `start'; an absent binding matches nothing. */
        const tp_binding    *binding;
        tinyparse_class_fn classify;
        if (start >= length || engine == 0)
            return 0;
        binding = tp_engine_find_class(engine, ir->text);
        if (binding == 0 || binding->function == 0)
            return 0;
        classify = (tinyparse_class_fn)binding->function;
        if (!classify(binding->user, (uint32_t)input[start]))
            return 0;
        *out_end = start + 1;
        return 1;
    }

    case TP_OP_SEQ: {
        uint32_t pos = start;
        uint32_t i;
        for (i = 0; i < ir->child_count; i++) {
            uint32_t next;
            if (!tp_match_ir(engine, ir->children[i], input, length, pos, &next))
                return 0;
            pos = next;
        }
        *out_end = pos;
        return 1;
    }

    case TP_OP_CHOICE: {
        uint32_t i;
        for (i = 0; i < ir->child_count; i++) {
            uint32_t next;
            if (tp_match_ir(engine, ir->children[i], input, length, start,
                            &next)) {
                *out_end = next;
                return 1;
            }
        }
        return 0;
    }

    case TP_OP_REPEAT: {
        uint32_t pos = start;
        uint32_t reps = 0;
        for (;;) {
            uint32_t next;
            if (ir->max != 0 && reps >= ir->max)
                break;
            if (!tp_match_ir(engine, ir->children[0], input, length, pos, &next))
                break;
            if (next == pos) /* zero-width guard */
                break;
            pos = next;
            reps++;
        }
        if (reps < ir->min)
            return 0;
        *out_end = pos;
        return 1;
    }

    case TP_OP_OPTIONAL: {
        uint32_t next;
        if (tp_match_ir(engine, ir->children[0], input, length, start, &next))
            *out_end = next;
        else
            *out_end = start;
        return 1;
    }

    case TP_OP_NOT_KEYWORD: {
        /* Match the inner pattern (child[0]); then fail if its byte slice is
        ** exactly one of the reserved words (children[1..]). Equality is
        ** whole-slice: a reserved word matches only when its length and bytes
        ** cover the entire match, so `class` is rejected but `classy` is not. */
        uint32_t end;
        uint32_t i;
        if (ir->child_count < 1)
            return 0;
        if (!tp_match_ir(engine, ir->children[0], input, length, start, &end))
            return 0;
        for (i = 1; i < ir->child_count; i++) {
            const tp_ir *w = ir->children[i];
            if (w->text.length == end - start) {
                uint32_t j;
                int same = 1;
                for (j = 0; j < w->text.length; j++)
                    if (input[start + j] != w->text.pointer[j]) {
                        same = 0;
                        break;
                    }
                if (same)
                    return 0;
            }
        }
        *out_end = end;
        return 1;
    }

    case TP_OP_NOT: {
        /* (not GUARD BODY...): fail if GUARD matches here; else match BODY. */
        uint32_t scratch;
        uint32_t pos = start;
        uint32_t i;
        if (ir->child_count >= 1 &&
            tp_match_ir(engine, ir->children[0], input, length, start, &scratch))
            return 0;
        for (i = 1; i < ir->child_count; i++) {
            uint32_t next;
            if (!tp_match_ir(engine, ir->children[i], input, length, pos, &next))
                return 0;
            pos = next;
        }
        if (ir->child_count < 2) {
            /* (not X) with no body matches a single byte not starting X */
            if (start >= length)
                return 0;
            pos = start + 1;
        }
        *out_end = pos;
        return 1;
    }

    default:
        return 0;
    }
}

/* ======================================================================== */
/* W15-2: the compiled token lexer.                                          */
/*                                                                           */
/* tp_match_ir above is the recursive token-IR interpreter -- the lexer's     */
/* inner loop, and (W15-1 profile) the single largest interpreted cost.      */
/* W15-2 lowers each token pattern, once, to a flat JUMP-THREADED program     */
/* (tp_token_program) and matches it on the hot path with an ITERATIVE        */
/* register loop (tp_token_run): one program counter, one cursor, and a tiny  */
/* fixed save-stack for the backtrack points an ordered choice / optional /   */
/* repeat-exit needs. This (a) takes the recursive tree-walk off the hot path */
/* and (b) bounds it -- a deeply nested pattern overflows no C stack -- while  */
/* the linear spine (the common keyword / identifier / number) runs as        */
/* straight-line code, the moves a hand-written lexer makes.                  */
/*                                                                           */
/* The matcher is PURE -- (input, length, start) -> matched + end -- and token */
/* matching is POSSESSIVE PEG (greedy, no cross-element backtracking:          */
/* tp_match_ir's repeat never gives bytes back, its choice commits to the      */
/* first matching alternative, its seq never retries an earlier element). So   */
/* the save-stack only ever holds "resume here if THIS alternative fails"      */
/* points: success drops the point (a COMMIT), a leaf failure pops it,         */
/* restores its cursor and jumps to its handler. The lowering mirrors          */
/* tp_match_ir node-for-node, so the end offset is byte-identical -- the        */
/* differential harness (cc-diff / vm-diff) and conformance suite verify it.   */
/* tp_match_ir stays as the cold reference for query.c and as the fallback for */
/* the few token shapes the lowering declines (a `not' guard, an overdeep      */
/* pattern), but it is off the parser hot path for the common tokens.          */
/* ======================================================================== */

/* ** Append one instruction to a token program under construction, returning its
** pc, or TP_NO_INDEX on allocation failure. Grows geometrically; arenas never
** realloc, so growth copies into a fresh, larger block. */
static uint32_t tp_token_emit(tp_arena *arena, tp_token_program *program,
                              tp_token_op op)
{
    tp_token_instruction *slot;
    uint32_t index;
    if (program->instruction_count == program->instruction_capacity) {
        uint32_t nc = program->instruction_capacity == 0
                          ? 16 : program->instruction_capacity * 2;
        tp_token_instruction *grown = (tp_token_instruction *)tp_alloc(
            arena, nc * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return TP_NO_INDEX;
        for (i = 0; i < program->instruction_count; i++)
            grown[i] = program->instructions[i];
        program->instructions         = grown;
        program->instruction_capacity = nc;
    }
    index = program->instruction_count++;
    slot = &program->instructions[index];
    slot->op     = op;
    slot->source = 0;
    slot->target = TP_NO_INDEX;
    slot->op2    = TP_TOK_MATCH;
    slot->min    = 0;
    slot->max    = 0;
    return index;
}

/* ** Map a token-IR leaf op to its threaded leaf opcode, or return -1 when the IR
** node is not a single-byte leaf (so the STAR fusion knows to decline). */
static int tp_token_leaf_op(const tp_ir *ir)
{
    switch (ir->op) {
    case TP_OP_LITERAL:    return TP_TOK_LITERAL;
    case TP_OP_ANY:        return TP_TOK_ANY;
    case TP_OP_CLASS:      return TP_TOK_CLASS;
    case TP_OP_CLASS_NAME: return TP_TOK_CLASS_NAME;
    default:               return -1;
    }
}

/* ** Lower one token-IR node into the threaded stream (appending instructions in
** execution order), returning 1 on success or 0 when the pattern cannot be lowered
** (the caller then falls back to tp_match_ir, byte-identical). The lowering is
** recursive over the AUTHORED pattern tree -- a one-time, compile-time cost bounded
** by the grammar, like the rule lowering in vmcompile.c; the RUNTIME matcher it
** produces is iterative. Possessive semantics make the encoding direct: a SEQ is
** its children back to back, a CHOICE is SAVE/<alt>/COMMIT chains, a REPEAT of a
** leaf is one STAR op, a general REPEAT/OPTIONAL is a SAVE/body/COMMIT loop. A
** `not' guard declines (returns 0) -- it appears only in comment/string/regex
** tokens, which then keep using tp_match_ir; the common tokens are all covered. */
static int tp_token_lower(tp_arena *arena, tp_token_program *program,
                          const tp_ir *ir)
{
    switch (ir->op) {
    case TP_OP_LITERAL:
    case TP_OP_ANY:
    case TP_OP_CLASS:
    case TP_OP_CLASS_NAME: {
        int leaf = tp_token_leaf_op(ir);
        uint32_t in = tp_token_emit(arena, program, (tp_token_op)leaf);
        if (in == TP_NO_INDEX) return 0;
        program->instructions[in].source = ir;
        return 1;
    }

    case TP_OP_SEQ: {
        uint32_t i;
        for (i = 0; i < ir->child_count; i++)
            if (!tp_token_lower(arena, program, ir->children[i]))
                return 0;
        return 1; /* an empty seq emits nothing: matches zero bytes, succeeds */
    }

    case TP_OP_CHOICE: {
        /* Ordered, possessive: try each alternative from the same cursor; the first
        ** to match commits and jumps past the rest. Each alternative but the last is
        ** guarded by a SAVE whose target is the next alternative's entry; the leaf
        ** failures inside it pop that save and resume there. A COMMIT after a
        ** matching alternative drops its save and jumps to the join. The last
        ** alternative needs no SAVE: its failure propagates to the enclosing save
        ** point (or fails the whole match), exactly as tp_match_ir's choice returns
        ** 0 when no alternative matched. */
        uint32_t i;
        uint32_t commits[64];
        uint32_t commit_count = 0;
        if (ir->child_count == 0)
            return 1; /* empty choice: matches nothing, succeeds (as tp_match_ir) */
        if (ir->child_count > 64)
            return 0; /* wider than the patch buffer: fall back (never happens) */
        for (i = 0; i < ir->child_count; i++) {
            int last = (i + 1 == ir->child_count);
            uint32_t save = TP_NO_INDEX;
            if (!last) {
                save = tp_token_emit(arena, program, TP_TOK_SAVE);
                if (save == TP_NO_INDEX) return 0;
            }
            if (!tp_token_lower(arena, program, ir->children[i]))
                return 0;
            if (!last) {
                uint32_t commit = tp_token_emit(arena, program, TP_TOK_COMMIT);
                if (commit == TP_NO_INDEX) return 0;
                commits[commit_count++] = commit;
                /* the SAVE resumes at the NEXT alternative's entry (now) */
                program->instructions[save].target = program->instruction_count;
            }
        }
        {
            uint32_t join = program->instruction_count;
            uint32_t k;
            for (k = 0; k < commit_count; k++)
                program->instructions[commits[k]].target = join;
        }
        return 1;
    }

    case TP_OP_REPEAT: {
        /* Greedy, possessive. A repeat of a single LEAF -- the dominant shape (an
        ** identifier's class run, a digit run, whitespace) -- fuses into ONE STAR op
        ** that loops in tight C. Anything else lowers to mandatory min copies, then a
        ** self-looping SAVE/body/COMMIT optional tail (unbounded) or unrolled
        ** optional copies (bounded). */
        if (ir->child_count < 1) return 0;
        {
            int leaf = tp_token_leaf_op(ir->children[0]);
            if (leaf >= 0) {
                uint32_t in = tp_token_emit(arena, program, TP_TOK_STAR);
                if (in == TP_NO_INDEX) return 0;
                program->instructions[in].source = ir->children[0];
                program->instructions[in].op2    = (tp_token_op)leaf;
                program->instructions[in].min    = ir->min;
                program->instructions[in].max    = ir->max;
                return 1;
            }
        }
        {
            /* (repeat (choice LEAF alt1 alt2 ...)) min:0 max:0 -- the identifier /
            ** identifier_name body, the single most common token shape. The first
            ** alternative is a leaf (a character class); the rest are the rare \u
            ** escape forms. Fuse the leaf run into a tight STAR and only fall to the
            ** remaining alternatives when the leaf stops matching:
            **
            **  Lloop: STAR leaf0           ; greedily consume the leaf run (min 0)
            **         SAVE Ldone           ; rest fails -> the repeat is done
            **         <choice alt1..altN>  ; try the rare alternatives
            **         COMMIT Lloop         ; one matched -> consume more leaf0
            **  Ldone:
            **
            ** Byte-identical to the general loop below: each "position" still tries the
            ** alternatives in order (leaf0 first via STAR, then the rest), greedily and
            ** possessively. Restricted to an unbounded, no-minimum repeat so no
            ** per-iteration count is needed; other bounds fall through. */
            const tp_ir *body = ir->children[0];
            if (ir->min == 0 && ir->max == 0 &&
                body->op == TP_OP_CHOICE && body->child_count >= 2 &&
                tp_token_leaf_op(body->children[0]) >= 0) {
                uint32_t loop = program->instruction_count;
                uint32_t star = tp_token_emit(arena, program, TP_TOK_STAR);
                uint32_t save, commit, i;
                uint32_t commits[64];
                uint32_t commit_count = 0;
                if (star == TP_NO_INDEX) return 0;
                program->instructions[star].source = body->children[0];
                program->instructions[star].op2 =
                    (tp_token_op)tp_token_leaf_op(body->children[0]);
                program->instructions[star].min = 0;
                program->instructions[star].max = 0;
                save = tp_token_emit(arena, program, TP_TOK_SAVE);
                if (save == TP_NO_INDEX) return 0;
                /* Lower (choice alt1..altN): SAVE/<alt>/COMMIT chain, last alt bare,
                ** mirroring the TP_OP_CHOICE arm but over children[1..]. */
                if (body->child_count - 1 > 64) return 0;
                for (i = 1; i < body->child_count; i++) {
                    int last = (i + 1 == body->child_count);
                    uint32_t alt_save = TP_NO_INDEX;
                    if (!last) {
                        alt_save = tp_token_emit(arena, program, TP_TOK_SAVE);
                        if (alt_save == TP_NO_INDEX) return 0;
                    }
                    if (!tp_token_lower(arena, program, body->children[i]))
                        return 0;
                    if (!last) {
                        uint32_t c = tp_token_emit(arena, program, TP_TOK_COMMIT);
                        if (c == TP_NO_INDEX) return 0;
                        commits[commit_count++] = c;
                        program->instructions[alt_save].target =
                            program->instruction_count;
                    }
                }
                /* All inner-choice COMMITs land here: an alternative matched, so loop
                ** back to consume more of the leaf run. */
                {
                    uint32_t join = program->instruction_count;
                    uint32_t k;
                    for (k = 0; k < commit_count; k++)
                        program->instructions[commits[k]].target = join;
                }
                commit = tp_token_emit(arena, program, TP_TOK_COMMIT);
                if (commit == TP_NO_INDEX) return 0;
                program->instructions[commit].target = loop; /* loop back */
                /* The outer SAVE (rest failed) resumes after the loop: Ldone. */
                program->instructions[save].target = program->instruction_count;
                return 1;
            }
        }
        {
            uint32_t m;
            for (m = 0; m < ir->min; m++)
                if (!tp_token_lower(arena, program, ir->children[0]))
                    return 0;
            if (ir->max != 0 && ir->max <= ir->min)
                return 1;
            if (ir->max == 0) {
                uint32_t loop = program->instruction_count;
                uint32_t save = tp_token_emit(arena, program, TP_TOK_SAVE);
                uint32_t commit;
                if (save == TP_NO_INDEX) return 0;
                if (!tp_token_lower(arena, program, ir->children[0]))
                    return 0;
                commit = tp_token_emit(arena, program, TP_TOK_COMMIT);
                if (commit == TP_NO_INDEX) return 0;
                program->instructions[commit].target = loop; /* loop back */
                program->instructions[save].target = program->instruction_count;
                return 1;
            }
            {
                uint32_t saves[256];
                uint32_t sc = 0;
                uint32_t remaining = ir->max - ir->min;
                uint32_t r;
                if (remaining > 256) return 0;
                for (r = 0; r < remaining; r++) {
                    uint32_t s = tp_token_emit(arena, program, TP_TOK_SAVE);
                    if (s == TP_NO_INDEX) return 0;
                    saves[sc++] = s;
                    if (!tp_token_lower(arena, program, ir->children[0]))
                        return 0;
                }
                for (r = 0; r < sc; r++)
                    program->instructions[saves[r]].target =
                        program->instruction_count;
            }
            return 1;
        }
    }

    case TP_OP_OPTIONAL: {
        /* SAVE Lexit; <body>; COMMIT Lexit. A body failure pops the save and resumes
        ** at Lexit (cursor restored); a match falls through the COMMIT, which drops
        ** the save. Either way OPTIONAL succeeds. */
        uint32_t save;
        uint32_t commit;
        if (ir->child_count < 1) return 0;
        save = tp_token_emit(arena, program, TP_TOK_SAVE);
        if (save == TP_NO_INDEX) return 0;
        if (!tp_token_lower(arena, program, ir->children[0]))
            return 0;
        commit = tp_token_emit(arena, program, TP_TOK_COMMIT);
        if (commit == TP_NO_INDEX) return 0;
        program->instructions[save].target   = program->instruction_count;
        program->instructions[commit].target = program->instruction_count;
        return 1;
    }

    case TP_OP_NOT_KEYWORD: {
        /* Match child[0], then reject if the matched slice equals a reserved word
        ** (children[1..], read at run time). A MARK before the inner pattern records
        ** the match start in the mark register (NOT a save point, so an inner failure
        ** backtracks past it normally and fails the whole construct, exactly as
        ** tp_match_ir does). The NOT_KEYWORD op, reached only when the inner matched,
        ** reads [mark,pos) and fails the match if it is a reserved word. */
        uint32_t mark;
        uint32_t check;
        if (ir->child_count < 1) return 0;
        mark = tp_token_emit(arena, program, TP_TOK_MARK);
        if (mark == TP_NO_INDEX) return 0;
        if (!tp_token_lower(arena, program, ir->children[0]))
            return 0;
        check = tp_token_emit(arena, program, TP_TOK_NOT_KEYWORD);
        if (check == TP_NO_INDEX) return 0;
        program->instructions[check].source = ir;
        return 1;
    }

    case TP_OP_NOT: {
        /* (not GUARD BODY...): a negative lookahead followed by a body. Mirror
        ** tp_match_ir's TP_OP_NOT exactly so the end offset is byte-identical, but
        ** as a flat guard region rather than a recursive call -- this is the scan
        ** the comment/string/regex tokens spend their bytes in, so lowering it takes
        ** the last token shape off the recursive tp_match_ir fallback.
        **
        **   NOT_BEGIN Lcont    ; push a negative save resuming at Lcont
        **   <GUARD>            ; child[0]; a leaf failure pops the save -> Lcont
        **   NOT_END           ; reached iff GUARD matched -> fail the (not ...)
        **  Lcont:             ; GUARD failed: match the body (or one ANY byte)
        **   <BODY...>         ; children[1..], in sequence
        **
        ** The negative save is gone by Lcont on both paths (popped by the guard's
        ** leaf failure, or by NOT_END), so a later body failure backtracks past the
        ** whole construct, never into the guard -- matching the recursive semantics. */
        uint32_t begin;
        if (ir->child_count < 1)
            return 0;
        begin = tp_token_emit(arena, program, TP_TOK_NOT_BEGIN);
        if (begin == TP_NO_INDEX) return 0;
        if (!tp_token_lower(arena, program, ir->children[0]))
            return 0;
        if (tp_token_emit(arena, program, TP_TOK_NOT_END) == TP_NO_INDEX)
            return 0;
        /* Lcont is the next pc: the guard's negative save resumes here. */
        program->instructions[begin].target = program->instruction_count;
        if (ir->child_count >= 2) {
            uint32_t i;
            for (i = 1; i < ir->child_count; i++)
                if (!tp_token_lower(arena, program, ir->children[i]))
                    return 0;
        } else {
            /* (not GUARD) with no body matches a single byte not starting GUARD. */
            if (tp_token_emit(arena, program, TP_TOK_ANY) == TP_NO_INDEX)
                return 0;
        }
        return 1;
    }

    default:
        /* TP_OP_EOF, TP_OP_REF and the rule-only ops decline: the caller falls back
        ** to tp_match_ir (byte-identical). With TP_OP_NOT now lowered, the common
        ** comment/string/regex tokens no longer touch the recursive interpreter. */
        return 0;
    }
}

/* ** Lower a token pattern to a threaded program and cache it on the IR node, or
** return NULL when it cannot be lowered (so the caller falls back to tp_match_ir).
** Built lazily on the first match attempt -- the node is arena-owned and mutable,
** like the first-byte cache. token_program_tried makes the failure path cheap. The
** arena is the engine arena (the IR's lifetime). */
static tp_token_program *tp_token_program_get(const tinyparse_engine *engine,
                                              tp_ir *ir)
{
    tp_token_program *program;
    tp_arena *arena;
    if (ir->token_program != 0)
        return ir->token_program->ok ? ir->token_program : 0;
    if (ir->token_program_tried)
        return 0;
    ir->token_program_tried = 1;
    if (engine == 0)
        return 0;
    arena = engine->arena;
    program = (tp_token_program *)tp_alloc(arena, sizeof *program);
    if (program == 0)
        return 0;
    program->instructions         = 0;
    program->instruction_count    = 0;
    program->instruction_capacity = 0;
    program->start_program        = 0;
    program->ok                   = 0;
    if (!tp_token_lower(arena, program, ir))
        return 0; /* unlowerable: fall back to tp_match_ir */
    if (tp_token_emit(arena, program, TP_TOK_MATCH) == TP_NO_INDEX)
        return 0;
    program->ok = 1;
    ir->token_program = program;
    return program;
}

/* ** One save-stack entry: the pc to resume at when the alternative/body whose entry
** pushed it fails, and the cursor to restore there. The stack is small and fixed;
** its depth is the number of NESTED choice/optional/repeat-tail alternatives in
** flight, bounded by the authored pattern, never by input. */
typedef struct tp_token_save {
    uint32_t pc;
    uint32_t pos;
    uint32_t negative; /* 1: a negative-lookahead guard save (TP_TOK_NOT_BEGIN);
                       ** a guard failure pops it and resumes at pc (the (not ...)
                       ** succeeds), so it behaves like an ordinary save on a leaf
                       ** failure. TP_TOK_NOT_END uses the flag only to assert it
                       ** is unwinding the matching guard. 0: an ordinary save. */
} tp_token_save;

#define TP_TOKEN_SAVE_MAX 64u

/* ** Match a threaded token program iteratively: one pc, one cursor, a small fixed
** save-stack. Returns 1 and writes the end offset on a match, 0 on no-match, and -1
** only when the save-stack would overflow (deeper than any real pattern -- the
** caller then retries with tp_match_ir, byte-identical). No C recursion, no
** per-match allocation. The control flow mirrors tp_match_ir, so the end offset is
** byte-identical. */
static int tp_token_run(const tinyparse_engine *engine,
                        const tp_token_program *program,
                        const uint8_t *input, uint32_t length, uint32_t start,
                        uint32_t *out_end)
{
    const tp_token_instruction *code = program->instructions;
    tp_token_save saves[TP_TOKEN_SAVE_MAX];
    uint32_t sp = 0;
    uint32_t pc = 0;
    uint32_t pos = start;
    uint32_t mark = start; /* not-keyword span start, set by TP_TOK_MARK */

    for (;;) {
        const tp_token_instruction *in = &code[pc];
        int matched; /* leaf verdict: 1 matched (pos advanced), 0 failed */

        switch (in->op) {
        case TP_TOK_LITERAL: {
            uint32_t end;
            matched = tp_match_literal(in->source, input, length, pos, &end);
            if (matched) pos = end;
            break;
        }
        case TP_TOK_ANY:
            matched = (pos < length);
            if (matched) pos++;
            break;
        case TP_TOK_CLASS:
            matched = (pos < length && tp_class_has(&in->source->cls, input[pos]));
            if (matched) pos++;
            break;
        case TP_TOK_CLASS_NAME: {
            const tp_binding    *binding;
            tinyparse_class_fn classify;
            matched = 0;
            if (pos < length && engine != 0) {
                binding = tp_engine_find_class(engine, in->source->text);
                if (binding != 0 && binding->function != 0) {
                    classify = (tinyparse_class_fn)binding->function;
                    if (classify(binding->user, (uint32_t)input[pos])) {
                        pos++;
                        matched = 1;
                    }
                }
            }
            break;
        }
        case TP_TOK_STAR: {
            /* The fused greedy possessive leaf loop -- the hottest lexer path. */
            uint32_t reps = 0;
            const tp_ir *src = in->source;
            switch (in->op2) {
            case TP_TOK_CLASS:
                while ((in->max == 0 || reps < in->max) &&
                       pos < length && tp_class_has(&src->cls, input[pos])) {
                    pos++; reps++;
                }
                break;
            case TP_TOK_ANY:
                while ((in->max == 0 || reps < in->max) && pos < length) {
                    pos++; reps++;
                }
                break;
            case TP_TOK_LITERAL: {
                uint32_t end;
                while ((in->max == 0 || reps < in->max) &&
                       tp_match_literal(src, input, length, pos, &end) &&
                       end != pos) {
                    pos = end; reps++;
                }
                break;
            }
            case TP_TOK_CLASS_NAME: {
                const tp_binding *binding =
                    (engine != 0) ? tp_engine_find_class(engine, src->text) : 0;
                tinyparse_class_fn classify =
                    (binding != 0) ? (tinyparse_class_fn)binding->function : 0;
                while (classify != 0 && (in->max == 0 || reps < in->max) &&
                       pos < length &&
                       classify(binding->user, (uint32_t)input[pos])) {
                    pos++; reps++;
                }
                break;
            }
            default:
                break;
            }
            matched = (reps >= in->min); /* possessive: succeed iff min met */
            break;
        }
        case TP_TOK_SAVE:
            if (sp >= TP_TOKEN_SAVE_MAX)
                return -1; /* deeper than the fixed save-stack: fall back */
            saves[sp].pc  = in->target;
            saves[sp].pos = pos;
            saves[sp].negative = 0;
            sp++;
            pc++;
            continue;
        case TP_TOK_NOT_BEGIN:
            /* Open a negative-lookahead guard: push a save whose target is the
            ** continuation reached when the guard fails (the (not ...) succeeds).
            ** A leaf failure inside the guard pops it and resumes there normally;
            ** only TP_TOK_NOT_END (guard matched) treats it specially. */
            if (sp >= TP_TOKEN_SAVE_MAX)
                return -1;
            saves[sp].pc  = in->target;
            saves[sp].pos = pos;
            saves[sp].negative = 1;
            sp++;
            pc++;
            continue;
        case TP_TOK_NOT_END:
            /* The guard matched, so the (not GUARD ...) fails. Drop the negative
            ** save (the guard consumed nothing for the construct), then backtrack to
            ** the enclosing save -- the generic failure path below, entered with
            ** matched == 0. */
            if (sp == 0) return -1;
            sp--; /* discard the negative-lookahead save */
            matched = 0;
            break;
        case TP_TOK_COMMIT: {
            uint32_t loop = in->target;
            uint32_t entry_pos;
            if (sp == 0) return -1;
            entry_pos = saves[sp - 1].pos;
            sp--; /* the alternative/body matched: drop its backtrack point */
            /* A COMMIT that loops back (a repeat tail) but made no progress exits the
            ** loop instead of spinning -- the zero-width guard, matching tp_match_ir's
            ** `if (next == pos) break'. A forward COMMIT (choice/optional join) always
            ** takes the jump. */
            if (loop <= pc && pos == entry_pos) {
                pc++; /* zero-width: stop looping, continue after the COMMIT */
                continue;
            }
            pc = loop;
            continue;
        }
        case TP_TOK_JMP:
            pc = in->target;
            continue;
        case TP_TOK_MARK:
            mark = pos;
            pc++;
            continue;
        case TP_TOK_NOT_KEYWORD: {
            /* The mark register holds the inner match start. Reject the slice
            ** [mark,pos) if it equals a reserved word (children[1..]). */
            const tp_ir *ir = in->source;
            uint32_t mstart = mark;
            uint32_t k;
            matched = 1;
            for (k = 1; k < ir->child_count; k++) {
                const tp_ir *w = ir->children[k];
                if (w->text.length == pos - mstart) {
                    uint32_t j;
                    int same = 1;
                    for (j = 0; j < w->text.length; j++)
                        if (input[mstart + j] != w->text.pointer[j]) {
                            same = 0;
                            break;
                        }
                    if (same) { pos = mstart; matched = 0; break; }
                }
            }
            break;
        }
        case TP_TOK_MATCH:
            *out_end = pos;
            return 1;
        default:
            matched = 0;
            break;
        }

        if (matched) {
            pc++;
            continue;
        }
        /* Failure: backtrack to the nearest save point (restore its cursor, jump to
        ** its handler). With no save point, the whole match fails. */
        if (sp == 0)
            return 0;
        sp--;
        pos = saves[sp].pos;
        pc  = saves[sp].pc;
    }
}


/* ** Public entry: match a token pattern at `start', writing the end offset. The
** engine carries the host CLASS bindings a (class NAME) atom resolves against;
** it may be NULL when matching IR with no named classes. Cold path (query.c): the
** recursive interpreter is fine here -- no parse position is retried. */
int tp_token_match(const tinyparse_engine *engine, const tp_ir *pattern,
                   const uint8_t *input, uint32_t length, uint32_t start,
                   uint32_t *out_end)
{
    return tp_match_ir(engine, pattern, input, length, start, out_end);
}

/* ** Round a count up to the next power of two, with a small floor. The memo's
** capacity is a power of two so the hash maps to a slot with a single mask. */
static uint32_t tp_round_pow2(uint32_t n)
{
    uint32_t cap = 64;
    while (cap < n)
        cap <<= 1;
    return cap;
}

/* ** Mix the (pattern, offset, goal) triple into a hash. A cheap multiplicative
** mix is plenty here: keys are well spread (distinct IR pointers, dense offsets)
** and the table is generously sized. */
static uint32_t tp_match_hash(const tp_ir *pattern, uint32_t offset,
                              uint32_t goal)
{
    uint32_t h = (uint32_t)((uintptr_t)pattern >> 4);
    h = (h ^ offset) * 2654435761u;
    h = (h ^ goal) * 2654435761u;
    h ^= h >> 15;
    return h;
}

/* ** Initialise an empty memo sized for `input_length' bytes (see internal.h).
** Capacity scales with the input so the table stays sparse -- many tokens are
** probed per offset -- while remaining O(input). */
void tp_match_memo_init(tp_match_memo *memo, tp_arena *arena,
                        uint32_t input_length)
{
    uint32_t cap = tp_round_pow2(input_length * 4u + 64u);
    memo->arena    = arena;
    memo->count    = 0;
    /* The slot table is allocated RAW: its bytes are read only after the `live'
    ** column reports a slot occupied, so clearing it would be a memset the parse
    ** immediately overwrites (W15-1 -- it was the largest cost in the profile).
    ** Only the one-byte-per-slot `live' column is zeroed, which is what marks
    ** every slot empty at the start of the parse. */
    memo->slots    = (tp_match_slot *)tp_alloc_raw(arena, cap * sizeof *memo->slots);
    memo->live     = (uint8_t *)tp_alloc(arena, cap * sizeof *memo->live);
    if (memo->slots == 0 || memo->live == 0) {
        memo->slots    = 0;
        memo->live     = 0;
        memo->capacity = 0;
        memo->mask     = 0;
        return; /* allocation failed: memo disabled, matcher still correct */
    }
    memo->capacity = cap;
    memo->mask     = cap - 1;
}

/* ** Initialise an extras memo sized for `input_length' bytes (see internal.h).
** One byte per offset plus the end sentinel; all start "unknown" (zero). On
** allocation failure the array is left NULL, which disables the memo. */
void tp_extras_memo_init(tp_extras_memo *memo, tp_arena *arena,
                         uint32_t input_length)
{
    /* Prepare only: record the arena and the size the marks want, leave marks NULL
    ** (the memo disabled) so a clean parse never allocates or zeroes the input-scaled
    ** array. tp_extras_memo_ensure builds it lazily when the parse backtracks. */
    memo->marks          = 0;
    memo->length         = 0;
    memo->arena          = arena;
    memo->pending_length = input_length;
}

void tp_extras_memo_ensure(tp_extras_memo *memo)
{
    uint32_t entries;
    if (memo->marks != 0)
        return; /* already allocated */
    entries = memo->pending_length + 1u;
    memo->marks = (uint8_t *)tp_alloc(memo->arena, entries);
    if (memo->marks == 0) {
        memo->length = 0;
        return; /* allocation failed: memo disabled, scanner still correct */
    }
    /* tp_alloc zeroes the block, which is exactly the "all offsets unknown"
    ** start state -- no init loop needed (W15-1 removed the redundant clear). */
    memo->length = entries;
}

/* ** Probe the memo for (pattern, offset, goal). Returns the slot index of the
** matching entry when the key is present, or of the empty slot where it would be
** inserted; returns the capacity (an out-of-range marker) only when the table is
** full (which the load policy below prevents). Occupancy is read from the `live'
** column, never from the raw slot -- an empty slot's fields are uninitialised. */
static uint32_t tp_match_memo_probe(tp_match_memo *memo,
                                    const tp_ir *pattern,
                                    uint32_t offset, uint32_t goal)
{
    uint32_t i = tp_match_hash(pattern, offset, goal) & memo->mask;
    uint32_t steps = 0;
    while (steps <= memo->mask) {
        if (memo->live[i] == 0)
            return i; /* empty: miss, insertion point */
        if (memo->slots[i].pattern == pattern && memo->slots[i].offset == offset &&
            memo->slots[i].goal == goal)
            return i; /* hit */
        i = (i + 1) & memo->mask;
        steps++;
    }
    return memo->capacity; /* full: no slot */
}

int tp_token_match_memo(const tinyparse_engine *engine, tp_ir *pattern,
                        const uint8_t *input, uint32_t length, uint32_t start,
                        uint32_t goal, tp_match_memo *memo, uint32_t *out_end)
{
    uint32_t       index = 0;
    int            have_slot = 0;
    int            matched;
    uint32_t       end = start;

    /* First-byte dispatch: a provably impossible lead byte is a guaranteed
    ** miss, with no need to touch the memo or the interpreter. */
    if (!tp_first_byte_admits(pattern, input, length, start))
        return 0;

    /* Memo lookup. A full or absent table simply falls through to a direct
    ** match, so correctness never depends on the cache. Occupancy is the `live'
    ** column: when the probed slot is live it is a hit; the raw slot fields are
    ** read only in that case. */
    if (memo != 0 && memo->capacity != 0) {
        index = tp_match_memo_probe(memo, pattern, start, goal);
        if (index < memo->capacity) {
            have_slot = 1;
            if (memo->live[index]) {
                tp_match_slot *slot = &memo->slots[index];
                if (slot->matched)
                    *out_end = slot->end;
                return slot->matched;
            }
        }
    }

    /* The match itself. On the hot path this runs the ITERATIVE token bytecode
    ** (tp_token_run over the lazily-lowered, cached program) -- the recursive
    ** tp_match_ir tree-walk is off the hot path (W15-2). The bytecode is
    ** byte-identical (it mirrors tp_match_ir node-for-node) and iterative (a jump-
    ** threaded register loop with a small fixed save-stack, no C recursion, no
    ** per-match allocation). It falls back to tp_match_ir only for the few token
    ** shapes the lowering declines (a `not' guard) or a save-stack overflow -- both
    ** byte-identical and rare. */
    {
        tp_token_program *tprog = tp_token_program_get(engine, pattern);
        if (tprog != 0) {
            int r = tp_token_run(engine, tprog, input, length, start, &end);
            matched = (r < 0) ? tp_match_ir(engine, pattern, input, length,
                                            start, &end)
                              : r;
        } else {
            matched = tp_match_ir(engine, pattern, input, length, start, &end);
        }
    }
    if (matched)
        *out_end = end;

    /* Record the outcome. Keep the table under ~3/4 full so probe chains stay
    ** short; once it is, stop inserting (lookups still answer existing keys).
    ** `have_slot' with a clear `live' byte is the empty insertion point. */
    if (have_slot && memo->live[index] == 0 &&
        memo->count < memo->capacity - (memo->capacity >> 2)) {
        tp_match_slot *slot = &memo->slots[index];
        slot->pattern = pattern;
        slot->offset  = start;
        slot->goal    = goal;
        slot->matched = (uint8_t)matched;
        slot->end     = matched ? end : start;
        memo->live[index] = 1;
        memo->count++;
    }
    return matched;
}
