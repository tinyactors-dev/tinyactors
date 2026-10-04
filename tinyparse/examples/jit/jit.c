/*
 * jit.c -- a tiny native-codegen JIT for a sane subset of C.
 *
 * Pipeline:
 *   1. parse the C source (argv[1]) with pocketparse into a concrete syntax
 *      tree, walked entirely through the cursor API;
 *   2. emit native machine code for the host (x86-64 on Linux) by recursing
 *      directly over that tree -- there is no intermediate AST -- into a page
 *      that is mapped W then flipped to X (W^X);
 *   3. call the function with the integer argument (argv[3]) and print the
 *      integer result.
 *
 * The codegen is a straightforward stack machine: every expression leaves its
 * value on the native stack, binary operators pop two and push one. The single
 * incoming integer parameter lives in a stack slot. Recursion works because a
 * call to the function's own name emits a real CALL to the entry point with a
 * proper prologue/epilogue and frame.
 *
 * The emitter is the only tree walk: emit_expr(tree, cursor) dispatches on the
 * node's kind name (Number / Ident / Call) or, for the layered precedence
 * rules, on the field names its children carry (lhs / op / rhs, cond / then /
 * else, neg, num / var / group, expr, arg). Left-associative chains are folded
 * in place as the cursor walks the sibling list; ternaries are emitted with
 * back-patched conditional jumps. The CST already encodes the left-leaning
 * operator sequence, so no parallel host tree is built.
 *
 * This is hosted code (libc is fine); only the freestanding pocketparse core is
 * freestanding.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

#include "pocketparse.h"

#if !defined(__x86_64__)
#  error "this JIT example emits x86-64; build on an x86-64 host (the repo CI host)"
#endif

/* -------------------------------------------------------------------------- */
/* Source bytes, shared so the emitter can read literal/operator spans by      */
/* offset.                                                                     */
/* -------------------------------------------------------------------------- */

static const char *g_src;

/* -------------------------------------------------------------------------- */
/* Cursor helpers.                                                            */
/* -------------------------------------------------------------------------- */

/* Does a field/symbol name equal the given literal? */
static int name_is(pocketparse_string s, const char *want)
{
    size_t n = strlen(want);
    return s.length == n && memcmp(s.pointer, want, n) == 0;
}

/* The source text spanned by the cursor's current node. */
static pocketparse_string span_of(const pocketparse_tree_cursor *cursor)
{
    pocketparse_string out;
    uint32_t start = pocketparse_tree_cursor_start_byte(cursor);
    uint32_t end   = pocketparse_tree_cursor_end_byte(cursor);
    out.pointer = (const uint8_t *)(g_src + start);
    out.length  = end - start;
    return out;
}

static int field_is(const pocketparse_tree *tree,
                    const pocketparse_tree_cursor *cursor, const char *want)
{
    pocketparse_symbol field = pocketparse_tree_cursor_field(cursor);
    if (field.id == POCKETPARSE_NO_FIELD)
        return 0;
    return name_is(pocketparse_tree_symbol_name(tree, field), want);
}

static int kind_is(const pocketparse_tree *tree,
                   const pocketparse_tree_cursor *cursor, const char *want)
{
    pocketparse_symbol kind = pocketparse_tree_cursor_kind(cursor);
    return name_is(pocketparse_tree_symbol_name(tree, kind), want);
}

/* Parse the integer literal the cursor's node spans. */
static long literal_value(const pocketparse_tree_cursor *cursor)
{
    pocketparse_string t = span_of(cursor);
    char buf[64];
    size_t k = t.length < sizeof buf - 1 ? t.length : sizeof buf - 1;
    memcpy(buf, t.pointer, k);
    buf[k] = '\0';
    return strtol(buf, NULL, 10);
}

/* -------------------------------------------------------------------------- */
/* x86-64 codegen.  Stack machine, System V calling convention.               */
/*                                                                            */
/* Frame on entry to a body: the parameter arrives in EDI. We store it at      */
/* -8(%rbp). Every expression leaves its int result on the native stack (push/ */
/* pop rax). The final value is popped into eax and returned.                  */
/*                                                                            */
/* The grammar layers precedence into rules, and the emitter recurses straight */
/* over those CST nodes:                                                       */
/*                                                                            */
/*   Number / Ident      -> leaf (literal or parameter)                        */
/*   Call                -> self-recursive call, one argument                  */
/*   Expr / Ternary / Relational / Additive / Multiplicative / Unary /         */
/*   Primary             -> rule nodes whose CHILDREN carry the fields          */
/*                          lhs/op/rhs, cond/then/else, neg, num/var/group,     */
/*                          expr                                                */
/*                                                                            */
/* Because the chain rules attach lhs/op/rhs as their own children, a chain is  */
/* folded left-associatively as the cursor walks the sibling list -- no extra   */
/* descent and no parallel tree.                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} code;

static void emit(code *c, const void *bytes, size_t n)
{
    if (c->len + n > c->cap) {
        fprintf(stderr, "jit: code buffer overflow\n");
        exit(2);
    }
    memcpy(c->buf + c->len, bytes, n);
    c->len += n;
}
static void emit1(code *c, uint8_t b) { emit(c, &b, 1); }
static void emit4(code *c, int32_t v) { emit(c, &v, 4); }

/* push rax */
static void push_rax(code *c) { emit1(c, 0x50); }
/* pop rax / pop rcx */
static void pop_rax(code *c)  { emit1(c, 0x58); }
static void pop_rcx(code *c)  { emit1(c, 0x59); }

/* mov eax, imm32; push rax. */
static void gen_num(code *c, long value)
{
    emit1(c, 0xB8); emit4(c, (int32_t)value);   /* mov eax, imm32 */
    push_rax(c);
}

/* mov eax, [rbp-8]; push rax  (load the single parameter). */
static void gen_param(code *c)
{
    emit1(c, 0x8B); emit1(c, 0x45); emit1(c, 0xF8);
    push_rax(c);
}

/* Fold one (op, rhs) tail into the value already on the stack: the rhs result
 * is pushed on top, then both are popped and combined back onto the stack. */
static void gen_binop(code *c, pocketparse_string op)
{
    pop_rcx(c);               /* rhs -> ecx */
    pop_rax(c);               /* lhs -> eax */
    if (op.length == 1 && op.pointer[0] == '+') {
        emit1(c, 0x01); emit1(c, 0xC8);                 /* add eax, ecx */
        push_rax(c);
    } else if (op.length == 1 && op.pointer[0] == '-') {
        emit1(c, 0x29); emit1(c, 0xC8);                 /* sub eax, ecx */
        push_rax(c);
    } else if (op.length == 1 && op.pointer[0] == '*') {
        emit1(c, 0x0F); emit1(c, 0xAF); emit1(c, 0xC1); /* imul eax, ecx */
        push_rax(c);
    } else {                                            /* '<' */
        emit1(c, 0x39); emit1(c, 0xC8);                 /* cmp eax, ecx */
        emit1(c, 0x0F); emit1(c, 0x9C); emit1(c, 0xC0); /* setl al */
        emit1(c, 0x0F); emit1(c, 0xB6); emit1(c, 0xC0); /* movzx eax, al */
        push_rax(c);
    }
}

/* Forward decl: the single CST-walking expression emitter. */
static void emit_expr(const pocketparse_tree *tree,
                      pocketparse_tree_cursor *cursor, code *c);

/* Emit a self-recursive call. The cursor sits on a Call node; its `arg` field
 * (one level down through the CallArgs child) holds the single argument. The
 * callee resolves to this function's own entry at offset 0. */
static void emit_call(const pocketparse_tree *tree,
                      pocketparse_tree_cursor *cursor, code *c)
{
    int emitted_arg = 0;
    size_t call_at;
    int32_t rel;

    if (pocketparse_tree_cursor_goto_first_child(cursor)) {
        do {
            if (field_is(tree, cursor, "arg")) {
                emit_expr(tree, cursor, c);
                emitted_arg = 1;
            } else if (kind_is(tree, cursor, "CallArgs")) {
                if (pocketparse_tree_cursor_goto_first_child(cursor)) {
                    do {
                        if (field_is(tree, cursor, "arg")) {
                            emit_expr(tree, cursor, c);
                            emitted_arg = 1;
                        }
                    } while (pocketparse_tree_cursor_goto_next_sibling(cursor));
                    pocketparse_tree_cursor_goto_parent(cursor);
                }
            }
        } while (pocketparse_tree_cursor_goto_next_sibling(cursor));
        pocketparse_tree_cursor_goto_parent(cursor);
    }

    if (!emitted_arg)
        gen_num(c, 0);            /* no argument: pass 0 */

    pop_rax(c);
    emit1(c, 0x89); emit1(c, 0xC7);   /* mov edi, eax */
    /* call entry (rel32 to offset 0, the function prologue) */
    emit1(c, 0xE8);
    call_at = c->len; emit4(c, 0);
    rel = (int32_t)(0 - (int32_t)(call_at + 4));
    memcpy(c->buf + call_at, &rel, 4);
    /* result in eax */
    push_rax(c);
}

/* Emit a rule node: walk its children once, dispatching on field names. The
 * cursor is restored to the rule node on return. This handles the layered
 * precedence rules uniformly:
 *
 *   - lhs / op / rhs : a left-associative operator chain. Emit lhs, then for
 *     each (op, rhs) tail fold the pair into the running value on the stack.
 *   - cond / then / else : a ternary. Emit cond, jz to else, then-branch, jmp
 *     past else, else-branch; back-patch both rel32 displacements.
 *   - neg : prefix unary minus over the operand emitted alongside it.
 *   - num / var / group / expr : a Primary or Expr forwarding to a literal,
 *     parameter, parenthesized group, or nested expression.
 *
 * A node may carry only some of these; the walk handles whatever it finds and
 * assembles the shape at the end. */
static void emit_rule(const pocketparse_tree *tree,
                      pocketparse_tree_cursor *cursor, code *c)
{
    int  have_chain = 0;        /* an lhs has been emitted onto the stack */
    int  is_neg = 0;
    int  is_ternary = 0;
    pocketparse_string pending_op = { (const uint8_t *)"", 0 };

    /* Ternary back-patch sites, set when the `then` field is reached. */
    size_t jz_at = 0, jmp_at = 0;
    int32_t rel;

    if (!pocketparse_tree_cursor_goto_first_child(cursor)) {
        /* A fielded leaf token with no recognized kind: classify by its own
         * source text. */
        pocketparse_string t = span_of(cursor);
        if (t.length && t.pointer[0] >= '0' && t.pointer[0] <= '9')
            gen_num(c, literal_value(cursor));
        else
            gen_param(c);
        return;
    }

    do {
        if (field_is(tree, cursor, "num")) {
            gen_num(c, literal_value(cursor));
            have_chain = 1;
        } else if (field_is(tree, cursor, "var")) {
            gen_param(c);
            have_chain = 1;
        } else if (field_is(tree, cursor, "group")) {
            emit_expr(tree, cursor, c);
            have_chain = 1;
        } else if (field_is(tree, cursor, "expr")) {
            emit_expr(tree, cursor, c);
            have_chain = 1;
        } else if (field_is(tree, cursor, "neg")) {
            is_neg = 1;
        } else if (field_is(tree, cursor, "cond")) {
            is_ternary = 1;
            emit_expr(tree, cursor, c);     /* cond on stack */
            pop_rax(c);
            emit1(c, 0x85); emit1(c, 0xC0); /* test eax, eax */
            /* jz else  (rel32 placeholder) */
            emit1(c, 0x0F); emit1(c, 0x84);
            jz_at = c->len; emit4(c, 0);
        } else if (field_is(tree, cursor, "then")) {
            emit_expr(tree, cursor, c);     /* then -> pushes */
            /* jmp end (rel32 placeholder) */
            emit1(c, 0xE9);
            jmp_at = c->len; emit4(c, 0);
            /* else target: back-patch the jz now that the else begins next. */
            rel = (int32_t)(c->len - (jz_at + 4));
            memcpy(c->buf + jz_at, &rel, 4);
        } else if (field_is(tree, cursor, "else")) {
            emit_expr(tree, cursor, c);     /* else -> pushes */
            /* end target: back-patch the jmp over the else branch. */
            rel = (int32_t)(c->len - (jmp_at + 4));
            memcpy(c->buf + jmp_at, &rel, 4);
        } else if (field_is(tree, cursor, "lhs")) {
            emit_expr(tree, cursor, c);     /* lhs on stack */
            have_chain = 1;
        } else if (field_is(tree, cursor, "op")) {
            pending_op = span_of(cursor);
        } else if (field_is(tree, cursor, "rhs")) {
            emit_expr(tree, cursor, c);     /* rhs pushed on top */
            gen_binop(c, pending_op);       /* fold (op, rhs) left-assoc */
        } else {
            /* An unfielded child: a pass-through rule node (Relational with no
             * chain, Primary, Unary, ...) or a punctuation token. Descend only
             * into nodes that have structure, and only if no value is pending. */
            if (!have_chain && !is_ternary &&
                pocketparse_tree_cursor_child_count(cursor) > 0) {
                emit_expr(tree, cursor, c);
                have_chain = 1;
            }
        }
    } while (pocketparse_tree_cursor_goto_next_sibling(cursor));

    pocketparse_tree_cursor_goto_parent(cursor);

    if (!is_ternary && is_neg) {
        /* Prefix unary minus over the value left on the stack. */
        pop_rax(c);
        emit1(c, 0xF7); emit1(c, 0xD8); /* neg eax */
        push_rax(c);
    }
}

/* The single tree-driven emitter. Leaves the expression's result pushed on the
 * native stack. Dispatches on the leaf kinds, then falls through to the rule
 * walker for the layered precedence nodes. */
static void emit_expr(const pocketparse_tree *tree,
                      pocketparse_tree_cursor *cursor, code *c)
{
    if (kind_is(tree, cursor, "Number")) {
        gen_num(c, literal_value(cursor));
        return;
    }
    if (kind_is(tree, cursor, "Ident")) {
        gen_param(c);
        return;
    }
    if (kind_is(tree, cursor, "Call")) {
        emit_call(tree, cursor, c);
        return;
    }
    emit_rule(tree, cursor, c);
}

/* -------------------------------------------------------------------------- */
/* Locate the named function and emit its body's return expression.           */
/* -------------------------------------------------------------------------- */

/* With the cursor on a return Statement, emit its `value` field expression.
 * Returns nonzero if a value field was found and emitted. */
static int emit_statement(const pocketparse_tree *tree,
                          pocketparse_tree_cursor *cursor, code *c)
{
    int emitted = 0;
    if (pocketparse_tree_cursor_goto_first_child(cursor)) {
        do {
            if (field_is(tree, cursor, "value")) {
                emit_expr(tree, cursor, c);
                emitted = 1;
            }
        } while (pocketparse_tree_cursor_goto_next_sibling(cursor));
        pocketparse_tree_cursor_goto_parent(cursor);
    }
    return emitted;
}

/* With the cursor on the matched Function node, emit prologue, body, epilogue.
 * The entry is offset 0, so a self-call (rel32 to 0) reaches it. Returns
 * nonzero if a return statement was emitted. */
static int emit_function_body(const pocketparse_tree *tree,
                              pocketparse_tree_cursor *cursor, code *c)
{
    int emitted = 0;

    /* Prologue. */
    emit1(c, 0x55);                                     /* push rbp */
    emit1(c, 0x48); emit1(c, 0x89); emit1(c, 0xE5);     /* mov rbp, rsp */
    emit1(c, 0x48); emit1(c, 0x83); emit1(c, 0xEC); emit1(c, 0x10); /* sub rsp,16 */
    emit1(c, 0x89); emit1(c, 0x7D); emit1(c, 0xF8);     /* mov [rbp-8], edi */

    /* Body: the function's Statement children are inlined here (the grammar's
     * (repeat Statement) introduces no wrapper node). */
    if (pocketparse_tree_cursor_goto_first_child(cursor)) {
        do {
            if (kind_is(tree, cursor, "Statement"))
                emitted |= emit_statement(tree, cursor, c);
        } while (pocketparse_tree_cursor_goto_next_sibling(cursor));
        pocketparse_tree_cursor_goto_parent(cursor);
    }

    /* Epilogue. */
    pop_rax(c);                 /* result -> eax */
    emit1(c, 0xC9);             /* leave */
    emit1(c, 0xC3);             /* ret */
    return emitted;
}

/* Walk the Unit's Function children, find the one whose `name` field matches,
 * and emit it. Returns nonzero on success. The cursor starts at the root. */
static int emit_unit(const pocketparse_tree *tree,
                     pocketparse_tree_cursor *cursor, code *c,
                     const char *want_name)
{
    int done = 0;

    if (!pocketparse_tree_cursor_goto_first_child(cursor))
        return 0;
    do {
        if (!kind_is(tree, cursor, "Function"))
            continue;

        /* Decide whether this Function's `name` field matches before emitting:
         * scan the children for the name, restore, then emit only on a match. */
        {
            int matches = 0;
            if (pocketparse_tree_cursor_goto_first_child(cursor)) {
                do {
                    if (field_is(tree, cursor, "name"))
                        matches = name_is(span_of(cursor), want_name);
                } while (pocketparse_tree_cursor_goto_next_sibling(cursor));
                pocketparse_tree_cursor_goto_parent(cursor);
            }
            if (matches) {
                done = emit_function_body(tree, cursor, c);
                if (done)
                    break;
            }
        }
    } while (pocketparse_tree_cursor_goto_next_sibling(cursor));
    pocketparse_tree_cursor_goto_parent(cursor);
    return done;
}

/* -------------------------------------------------------------------------- */
/* W^X execution: map a page RW, copy code, flip to RX, call.                  */
/* -------------------------------------------------------------------------- */

typedef int (*jit_fn)(int);

static int run_jit(const uint8_t *code_bytes, size_t code_len, int arg)
{
    size_t page = 4096;
    void  *mem;
    jit_fn fn;
    int    result;

    mem = mmap(NULL, page, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); exit(2); }

    memcpy(mem, code_bytes, code_len);

    if (mprotect(mem, page, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect"); exit(2);
    }

    fn = (jit_fn)mem;
    result = fn(arg);

    munmap(mem, page);
    return result;
}

/* -------------------------------------------------------------------------- */
/* pocketparse driver glue.                                                    */
/* -------------------------------------------------------------------------- */

static pocketparse_string str_of(const char *s)
{
    pocketparse_string out;
    out.pointer = (const uint8_t *)s;
    out.length  = (uint32_t)strlen(s);
    return out;
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long  n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = '\0';
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

#define JIT_GRANT (8u * 1024u * 1024u)

static void *jit_grow(void *user, size_t need, size_t *got) { (void)user; if (got != 0) *got = need; return malloc(need); }

int main(int argc, char **argv)
{
    pocketparse_configuration cfg;
    pocketparse_engine  *engine;
    pocketparse_grammar *grammar = NULL;
    pocketparse_parser  *parser  = NULL;
    pocketparse_source   gsrc;
    pocketparse_string    empty = { (const uint8_t *)"", 0 };
    const pocketparse_tree *tree;
    pocketparse_tree_cursor *cursor = NULL;
    const char *c_source, *func_name;
    int arg_value;
    char *grammar_bytes;
    size_t grammar_len;
    code c;
    uint8_t code_storage[4096];
    int result;

    if (argc < 4) {
        fprintf(stderr, "usage: %s '<C source>' <function> <int-arg>\n", argv[0]);
        return 2;
    }
    c_source  = argv[1];
    func_name = argv[2];
    arg_value = atoi(argv[3]);
    g_src     = c_source;

    /* Locate the grammar next to this binary's source dir via env, falling back
     * to a path relative to the build's known layout. The build.sh passes the
     * grammar path through the JIT_GRAMMAR environment variable. */
    {
        const char *gp = getenv("JIT_GRAMMAR");
        if (!gp) gp = "examples/jit/jit.scm";
        grammar_bytes = read_file(gp, &grammar_len);
        if (!grammar_bytes) {
            fprintf(stderr, "jit: cannot read grammar '%s'\n", gp);
            return 2;
        }
    }

    /* Open the engine. */
    {
        void *block = malloc(JIT_GRANT);
        if (!block) { fprintf(stderr, "jit: out of memory\n"); return 2; }
        memset(&cfg, 0, sizeof cfg);
        cfg.memory.base = block;
        cfg.memory.size = JIT_GRANT;
        cfg.memory.grow = jit_grow;
        engine = pocketparse_open(cfg);
    }
    if (!engine) { fprintf(stderr, "jit: engine open failed\n"); return 2; }

    gsrc.name = str_of("jit.scm");
    gsrc.bytes.pointer = (const uint8_t *)grammar_bytes;
    gsrc.bytes.length  = (uint32_t)grammar_len;
    if (pocketparse_compile_scheme(engine, gsrc, &grammar) != POCKETPARSE_OK) {
        pocketparse_string f = pocketparse_fault(engine);
        fprintf(stderr, "jit: grammar error: %.*s\n", (int)f.length, f.pointer);
        return 2;
    }

    if (pocketparse_parser_new(engine, grammar, empty, &parser) != POCKETPARSE_OK
        || !parser) {
        fprintf(stderr, "jit: parser_new failed\n");
        return 2;
    }

    pocketparse_push(parser, str_of(c_source));
    {
        pocketparse_result r = pocketparse_finish(parser);
        if (r == POCKETPARSE_SYNTAX_ERROR || r == POCKETPARSE_ERROR) {
            uint32_t n = pocketparse_parser_diagnostic_count(parser), i;
            for (i = 0; i < n; i++) {
                const pocketparse_diagnostic *d =
                    pocketparse_parser_diagnostic(parser, i);
                if (d) fprintf(stderr, "jit: parse: %.*s\n",
                               (int)d->message.length, d->message.pointer);
            }
            fprintf(stderr, "jit: could not parse the C source\n");
            return 2;
        }
    }

    tree = pocketparse_parser_tree(parser);
    if (!tree || pocketparse_tree_cursor_new(tree, &cursor) != POCKETPARSE_OK) {
        fprintf(stderr, "jit: no tree\n");
        return 2;
    }

    c.buf = code_storage;
    c.len = 0;
    c.cap = sizeof code_storage;
    if (!emit_unit(tree, cursor, &c, func_name)) {
        pocketparse_tree_cursor_free(cursor);
        fprintf(stderr, "jit: function '%s' not found or not emittable\n",
                func_name);
        return 2;
    }
    pocketparse_tree_cursor_free(cursor);

    result = run_jit(c.buf, c.len, arg_value);
    printf("%d\n", result);
    return 0;
}
