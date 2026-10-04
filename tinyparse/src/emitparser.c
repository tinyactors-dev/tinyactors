/*
** emitparser.c -- the compiled-parser C emitter (W17-2: the IR-walking emitter).
**
** This is pocketparse's embedding backend: it turns a compiled grammar into a
** self-contained C parser. Its history is in docs/decisions.md; the short version
** is that through W16-2 the emitter walked the LOWERED bytecode program
** (pp_program / pp_instruction) and emitted one function per program index --
** pp_parser_p<N> -- whose body read structural operands from
** state->program->instructions[N] at runtime. It was true predictive recursive
** descent, but it still read "bytecode rendered as C": rule_0/rule_N, an opcode
** dispatched per IR node, the program array consulted on the hot path.
**
** W17-2 RE-GROUNDS the emitter ON THE IR, BY NAME (docs/codegen-transform.md, the
** owner-signed blueprint). The generator now walks the IR -- the compiled grammar
** as NAMED rules and tokens (grammar->rules[i].body, grammar->tokens[i].pattern,
** each a pp_ir tree) -- and emits the parser a person would write from that
** grammar:
**
**   - one function PER NAMED RULE  -- <prefix>_parse_<rule_name>  (parse_Value,
**     parse_Object), NOT rule_0/rule_1;
**   - one function PER NAMED TOKEN -- <prefix>_scan_<token_name>  (scan_String,
**     scan_Number);
**   - PP_OP_REF -> a DIRECT NAMED CALL: parse_<name> when the symbol is a rule,
**     scan_<name> when it is a token;
**   - SEQ/CHOICE/REPEAT/OPTIONAL/NOT/FIELD/ALIAS/GOAL/NO_EXTRAS/RECOVER/INJECT/EOF
**     translated to idiomatic C control flow inline in the rule body -- a sequence
**     of calls, a first-set-guarded ordered choice with cheap mark/rewind, a loop
**     with the zero-width guard, a try/restore, a save/set/restore;
**   - the FIRST sets the choice predictor needs are the SAME resolving sets the VM
**     consults (pp_choice_first_admits_fast, keyed on the child IR node), so a
**     choice alternative is skipped by a baked byte test exactly where the VM skips
**     it.
**
** There is NO program array, NO opcode dispatch, NO program->instructions[N] read
** in the generated parser, and NO rule_0/rule_N. The function names, the field
** names, the literal tokens and the FIRST-set branches are all in the source,
** because they were all in the IR.
**
** BYTE-IDENTITY is preserved because the CST construction and every special
** feature run through the SAME shared runtime helpers the VM (pp_vm_run) calls --
** exactly as a hand-written parser still calls its own make_node/intern:
**
**   - the arena CST builder and the rule CALL prologue/epilogue (pp_vm_enter /
**     pp_vm_exit: the leading-extras consume, node open, child link, node close --
**     a hand-written RD parser's per-rule work, with NO packrat memo: the generated
**     parser carries none (Phase 17 de-packrat), so there is no probe / store /
**     snapshot / replay and no call_attempts / memo_threshold bookkeeping on this
**     path. The memo lives only on the VM, for dynamic runtime grammars);
**   - the terminal matcher (pp_vm_do_match_pattern, which applies the lexical-goal
**     redirect), the byte op (pp_vm_do_byte_pattern), EOF (pp_vm_do_eof);
**   - the extras policy engine (pp_vm_consume_extras), the recovery insert
**     (pp_vm_recover_insert), the inject splice (pp_vm_do_inject -> pp_inject_run).
**
** So the generated parser builds the SAME tightly-packed, arena-allocated CST the
** VM builds (ci/pocketparse-cc-diff.sh proves it, byte-identical), keeps the
** detailed recovery diagnostics (ci/pocketparse-diag-diff.sh), nests embedded
** languages (the std.fence inject grammar), and is bounded against a pathological
** deep input by the measured recursion-depth guard.
**
** HOW A LEAF FINDS ITS RUNTIME IR NODE. The shared terminal/recover/inject helpers
** take a pp_ir* -- a runtime address that cannot be serialized. The generated
** parser recovers it by NAVIGATING the loaded grammar's IR tree: the rule/token
** tables and every IR node's children round-trip in order through pack.c, so the
** IR node the emitter saw at rule i, child path [a,b,...] is, at runtime, exactly
** state->grammar->rules[i].body->children[a]->children[b]->.... Each emitted
** parse_/scan_ function binds the body/pattern root once, and the recursion threads
** the C-expression for the current node down to each leaf. No instruction indices,
** no program array -- the structure is in the C, the few runtime pp_ir operands are
** read in place off the named grammar tables.
**
** CRASH-SAFETY via a MEASURED depth guard (W16-2, unchanged). Native recursion can
** overflow the host C stack on adversarial deep input. Each rule's body recursion
** is wrapped by a depth counter (depth / depth_limit in pp_vm_state): when entering
** a rule body would exceed the limit, the rule returns PP_VM_FAIL instead of
** recursing, so a pathologically nested input fails GRACEFULLY (a clean syntax
** error, no SIGSEGV). The limit (PP_PARSER_DEPTH_LIMIT) is measured -- above the
** deepest real corpus nesting, below the ulimit -s 512 overflow point. See the
** constant's comment and docs/peg-vs-glr.md.
**
** Like emit.c (and all of src/ except cli.c) this file is freestanding: it builds
** the C text into the engine arena in two passes (measure with a NULL buffer, then
** fill a block of that exact size, because arenas never realloc) and does no IO.
** The hosted cli.c writes the bytes. It holds no static mutable state. The emitted
** C is macro-free: it is generated, so the per-rule control flow is expanded once
** at generation time. The W16-3 symbol prefix (--name) renames every emitted
** file-scope symbol so two generated parsers link into one program collision-free.
*/
#include "internal.h"

/* ** A text cursor: when `bytes' is NULL the writer only counts (the measuring
** pass); when it points at a buffer the writer fills it. Mirrors emit.c. */
typedef struct pp_parser_cursor {
    char    *bytes;
    uint32_t length;
} pp_parser_cursor;

/* ** Append one byte, or just count it during the measuring pass. */
static void pp_parser_byte(pp_parser_cursor *cursor, char value)
{
    if (cursor->bytes != 0)
        cursor->bytes[cursor->length] = value;
    cursor->length++;
}

/* ** Append a NUL-terminated C literal (the NUL itself is not written). */
static void pp_parser_text(pp_parser_cursor *cursor, const char *text)
{
    while (*text != '\0')
        pp_parser_byte(cursor, *text++);
}

/* ** Append a non-negative integer in decimal. */
static void pp_parser_u32(pp_parser_cursor *cursor, uint32_t value)
{
    char     digits[10];
    uint32_t count = 0;
    if (value == 0) {
        pp_parser_byte(cursor, '0');
        return;
    }
    while (value > 0 && count < sizeof digits) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (count > 0)
        pp_parser_byte(cursor, digits[--count]);
}

/* ** Append a slice verbatim, folding newlines to spaces so it stays inside a C
** line comment (used only for the banner's grammar name). */
static void pp_parser_slice(pp_parser_cursor *cursor, pocketparse_string slice)
{
    uint32_t i;
    for (i = 0; i < slice.length; i++) {
        char c = (char)slice.pointer[i];
        pp_parser_byte(cursor, (c == '\n' || c == '\r') ? ' ' : c);
    }
}

/* ** Append a grammar symbol's name as a C-identifier-safe slug, for the readable
** half of an emitted function name (parse_<slug>, scan_<slug>). Every byte that is
** not [A-Za-z0-9_] becomes `_', and a name that does not begin with a letter or
** underscore is prefixed with `s' -- so a name like "+" or "1" still yields a legal
** identifier. The slug alone is NOT guaranteed unique (two names could slug to the
** same text); the caller appends the symbol id, which IS unique, to disambiguate.
** An empty/unknown name yields nothing here, so the id suffix carries the whole
** name -- still legal and unique. */
static void pp_parser_slug(pp_parser_cursor *cursor, pocketparse_string name)
{
    uint32_t i;
    for (i = 0; i < name.length; i++) {
        char c = (char)name.pointer[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '_';
        if (i == 0 && c >= '0' && c <= '9')
            pp_parser_byte(cursor, 's'); /* keep the leading digit legal */
        pp_parser_byte(cursor, ok ? c : '_');
    }
}

/* ** Append the packed grammar bytes as a C array body (comma-separated decimal
** octets, wrapped). The declaration around it is emitted by the caller. */
static void pp_parser_byte_array(pp_parser_cursor *cursor, pocketparse_string bytes)
{
    uint32_t i;
    for (i = 0; i < bytes.length; i++) {
        if (i % 16u == 0u)
            pp_parser_text(cursor, "\n    ");
        pp_parser_u32(cursor, bytes.pointer[i]);
        pp_parser_byte(cursor, ',');
    }
    pp_parser_byte(cursor, '\n');
}

/* ** The native recursive-descent depth guard limit (W16-2). Measured, not
** guessed: see the banner above and docs/peg-vs-glr.md (W16-2). It sits well above
** the deepest real corpus nesting (so no valid input ever bails) and far below
** what limit x per-rule C-frame would overflow the `ulimit -s 512' no-crash
** budget (so a pathological input bails gracefully before the C stack overflows).
** The generated driver writes it into pp_vm_state.depth_limit. */
#define PP_PARSER_DEPTH_LIMIT 1000u

/* ** Emit an emitted file-scope symbol's NAME, honouring the W16-3 prefix. When
** `prefix' is NULL the historical `default_name' is emitted verbatim, so the
** default-name path (the cc-diff oracle, every existing caller) is byte-for-byte
** unchanged. When `prefix' is set the symbol becomes `<prefix>_<suffix>', so two
** parsers generated with distinct prefixes carry disjoint symbol names and link
** into one program with no collision. */
static void pp_parser_name(pp_parser_cursor *cursor, const char *prefix,
                           const char *default_name, const char *suffix)
{
    if (prefix == 0) {
        pp_parser_text(cursor, default_name);
        return;
    }
    pp_parser_text(cursor, prefix);
    pp_parser_byte(cursor, '_');
    pp_parser_text(cursor, suffix);
}

/* ** Emit `<symbol>u' or `PP_NO_SYMBOL' for an interned id operand. */
static void pp_parser_emit_symbol(pp_parser_cursor *cursor, uint32_t symbol)
{
    if (symbol == PP_NO_SYMBOL)
        pp_parser_text(cursor, "PP_NO_SYMBOL");
    else { pp_parser_u32(cursor, symbol); pp_parser_byte(cursor, 'u'); }
}

/* ** Emit the NAME of the function that parses the rule with this symbol:
** `<prefix>_parse_<slug>_<symbol>'. The slug is the rule's source name (readable);
** the trailing symbol id makes the name unique even if two names slug alike. With
** no prefix the stem is the plain `pp_parse_<slug>_<symbol>' -- still no rule_0. */
static void pp_parser_rule_fn(pp_parser_cursor *cursor, const char *prefix,
                              const pocketparse_grammar *grammar, uint32_t symbol)
{
    pocketparse_string name = pp_intern_name(&grammar->interner, symbol);
    if (prefix != 0) {
        pp_parser_text(cursor, prefix);
        pp_parser_text(cursor, "_parse_");
    } else {
        pp_parser_text(cursor, "pp_parse_");
    }
    pp_parser_slug(cursor, name);
    pp_parser_byte(cursor, '_');
    pp_parser_u32(cursor, symbol);
}

/* ** Emit the NAME of the function that scans the token with this symbol:
** `<prefix>_scan_<slug>_<symbol>'. Parallel to pp_parser_rule_fn; the distinct
** `scan_'/`parse_' stems keep a token and a rule of the same name apart. */
static void pp_parser_token_fn(pp_parser_cursor *cursor, const char *prefix,
                               const pocketparse_grammar *grammar, uint32_t symbol)
{
    pocketparse_string name = pp_intern_name(&grammar->interner, symbol);
    if (prefix != 0) {
        pp_parser_text(cursor, prefix);
        pp_parser_text(cursor, "_scan_");
    } else {
        pp_parser_text(cursor, "pp_scan_");
    }
    pp_parser_slug(cursor, name);
    pp_parser_byte(cursor, '_');
    pp_parser_u32(cursor, symbol);
}

/* ** Whether a symbol names a rule in this grammar (resolution is rule-first, then
** token, exactly as pp_lower_ir_inner / the recursive descent resolve PP_OP_REF). */
static int pp_parser_is_rule(const pocketparse_grammar *grammar, uint32_t symbol)
{
    uint32_t i;
    for (i = 0; i < grammar->rule_count; i++)
        if (grammar->rules[i].symbol == symbol)
            return 1;
    return 0;
}

/* ** Whether a symbol names a token in this grammar. */
static int pp_parser_is_token(const pocketparse_grammar *grammar, uint32_t symbol)
{
    uint32_t i;
    for (i = 0; i < grammar->token_count; i++)
        if (grammar->tokens[i].symbol == symbol)
            return 1;
    return 0;
}

/* ====================================================================== */
/* The IR walk: translate one rule body / sub-pattern IR node to idiomatic C. */
/* ====================================================================== */

/* ** The emit context threaded through the IR walk: the text cursor, the symbol
** prefix, the grammar (so a REF resolves rule-vs-token and a child looks up its
** sub-rule's function name), and a monotonically increasing label counter that
** keeps the per-combinator C local names unique across the whole rule body (each
** combinator opens its own `{ }` scope, but a shared counter is simpler to read
** than nesting-depth reasoning and immune to it). */
typedef struct pp_parser_emit {
    pp_parser_cursor          *cursor;
    const char                *prefix;
    const pocketparse_grammar *grammar;
    uint32_t                   label;
    /* Inject region functions (W62 language embedding). Each distinct PP_OP_INJECT
    ** node gets one region-body function (pp_parser_inject_region_<n>) emitted before
    ** the rule functions; at the inject site, pp_parser_inject is handed that function
    ** and the inject node, so pp_vm_do_inject matches the region and splices. The
    ** inject node's region structure is walked at emit time off the captured region
    ** IR; at run time the region function navigates ((pp_ir *)inject)->children[0]. */
    const pp_ir              **inject_nodes;   /* the inject IR nodes, in emit order */
    uint32_t                   inject_count;
    uint32_t                   inject_capacity;
    /* A file-global counter for the baked-lexer class tables (pp_parser_class_<n>),
    ** so each distinct byte class across all tokens gets one uniquely-named [256]
    ** table. Each token's matcher reserves a contiguous block starting here. */
    uint32_t                   class_base;
    /* W17-GOAL: baked inline-literal matchers. A bare PP_OP_LITERAL in a rule body
    ** (the punctuation a grammar matches directly -- `(', `;', `=>') was matched
    ** through the generic token VM (pp_vm_do_match_pattern -> pp_token_run), the
    ** largest remaining lexer slice once the per-token scanners were baked. Each
    ** DISTINCT literal byte string gets one pure baked matcher (<prefix>_litmatch_<n>,
    ** straight-line byte compares) emitted before the rule functions; the literal site
    ** routes through pp_vm_terminal_baked with that matcher, byte-identically to the
    ** generic path (a bare literal is never goal-redirected). Deduped by bytes so the
    ** common punctuation shares one matcher. */
    const pp_ir              **literal_nodes;  /* one representative IR per distinct text */
    uint32_t                   literal_count;
    uint32_t                   literal_capacity;
    /* Set when the baked extras scanner (<prefix>_extras_match) was emitted (every
    ** extras token bakeable), so the driver wires state->extras_match to it. */
    int                        extras_baked;
    /* W17-4: set when the start rule is a shallow top-level-item loop, so the
    ** buffer-friendly per-item function + chunk driver were emitted and the lib tail
    ** should also expose <prefix>_run_chunk. */
    int                        chunk_loop;
    int                        chunk_has_eof;
    uint32_t                   chunk_min;
    /* Set when ANY rule declares a deferred slot (slot_count > 0). The speculative
    ** combinators (choice/optional/repeat) then emit the slot-save push/restore/commit
    ** calls so (defer …) is rolled back on backtrack -- byte-identical to the VM. A
    ** grammar with no slots emits none, keeping the common parser lean. */
    int                        uses_slots;
} pp_parser_emit;

/* ** The region-function index already assigned to this inject IR node (by pointer
** identity), or PP_NO_INDEX if it was never registered. */
static uint32_t pp_parser_inject_lookup(const pp_parser_emit *ctx, const pp_ir *ir)
{
    uint32_t i;
    for (i = 0; i < ctx->inject_count; i++)
        if (ctx->inject_nodes[i] == ir)
            return i;
    return PP_NO_INDEX;
}

/* ** Record one inject IR node, returning the region-function index assigned to it
** (its position in the inject_nodes array). A node already recorded keeps its index.
** Returns PP_NO_INDEX on allocation failure. */
static uint32_t pp_parser_register_inject(pp_parser_emit *ctx, const pp_ir *ir)
{
    uint32_t existing = pp_parser_inject_lookup(ctx, ir);
    if (existing != PP_NO_INDEX)
        return existing;
    if (ctx->inject_count == ctx->inject_capacity) {
        uint32_t nc = ctx->inject_capacity == 0 ? 8 : ctx->inject_capacity * 2;
        const pp_ir **grown = (const pp_ir **)pp_alloc(
            ctx->grammar->arena, nc * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return PP_NO_INDEX;
        for (i = 0; i < ctx->inject_count; i++)
            grown[i] = ctx->inject_nodes[i];
        ctx->inject_nodes    = grown;
        ctx->inject_capacity = nc;
    }
    ctx->inject_nodes[ctx->inject_count] = ir;
    return ctx->inject_count++;
}

/* ** Whether two literal IR nodes carry the same byte string. */
static int pp_parser_literal_eq(const pp_ir *a, const pp_ir *b)
{
    uint32_t i;
    if (a->text.length != b->text.length)
        return 0;
    for (i = 0; i < a->text.length; i++)
        if (a->text.pointer[i] != b->text.pointer[i])
            return 0;
    return 1;
}

/* ** The litmatch-function index for a literal with this byte string (dedup by
** bytes), or PP_NO_INDEX if none registered yet. */
static uint32_t pp_parser_literal_lookup(const pp_parser_emit *ctx, const pp_ir *ir)
{
    uint32_t i;
    for (i = 0; i < ctx->literal_count; i++)
        if (pp_parser_literal_eq(ctx->literal_nodes[i], ir))
            return i;
    return PP_NO_INDEX;
}

/* ** Register a bare-literal IR node (deduped by bytes), returning the index of the
** baked matcher that will match it. An empty literal is never registered (it has no
** bytes to bake; the caller keeps the generic path). Returns PP_NO_INDEX on OOM. */
static uint32_t pp_parser_register_literal(pp_parser_emit *ctx, const pp_ir *ir)
{
    uint32_t existing;
    if (ir->text.length == 0)
        return PP_NO_INDEX;
    existing = pp_parser_literal_lookup(ctx, ir);
    if (existing != PP_NO_INDEX)
        return existing;
    if (ctx->literal_count == ctx->literal_capacity) {
        uint32_t nc = ctx->literal_capacity == 0 ? 16 : ctx->literal_capacity * 2;
        const pp_ir **grown = (const pp_ir **)pp_alloc(
            ctx->grammar->arena, nc * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return PP_NO_INDEX;
        for (i = 0; i < ctx->literal_count; i++)
            grown[i] = ctx->literal_nodes[i];
        ctx->literal_nodes    = grown;
        ctx->literal_capacity = nc;
    }
    ctx->literal_nodes[ctx->literal_count] = ir;
    return ctx->literal_count++;
}

static void pp_parser_emit_ir(pp_parser_emit *ctx, const char *node,
                              const pp_ir *ir, const char *sink,
                              const char *field, const char *alias,
                              const char *indent);

static void pp_parser_emit_not_rule(pp_parser_emit *ctx, const char *node,
                                    const pp_ir *ir, const char *sink,
                                    const char *field, const char *alias,
                                    const char *indent);
static void pp_parser_token_pattern_expr(char *buffer, uint32_t token_index);
static uint32_t pp_parser_token_index(const pocketparse_grammar *grammar,
                                      uint32_t symbol);

/* ** Return 1 if any node in this subtree is a rule reference (PP_OP_REF). Mirrors
** vmcompile.c's pp_ir_subtree_has_ref EXACTLY so the generated parser splits a
** `(not GUARD BODY)' into the byte-level / rule-level path on the SAME condition the
** VM lowerer uses -- byte-identity by construction. */
static int pp_parser_subtree_has_ref(const pp_ir *ir)
{
    uint32_t i;
    if (ir == 0)
        return 0;
    if (ir->op == PP_OP_REF)
        return 1;
    for (i = 0; i < ir->child_count; i++)
        if (pp_parser_subtree_has_ref(ir->children[i]))
            return 1;
    return 0;
}

/* ** The template rule index for a parametrized goal entry, addressed by its COMPACT
** index (the index over only the parametrized lex_goals[] entries -- the value
** PP_OP_PARSE_DEFERRED carries in ir->max). Mirrors vmcompile.c's `aux'
** computation, so the generated drain instantiates the SAME template the VM does.
** PP_NO_INDEX when the compact index has no entry (the non-parametrized drain). */
static uint32_t pp_parser_param_template_rule(const pocketparse_grammar *grammar,
                                              uint32_t compact_goal)
{
    uint32_t k, compact = 0;
    if (compact_goal == PP_NO_INDEX)
        return PP_NO_INDEX;
    for (k = 0; k < grammar->lex_goal_count; k++) {
        if (!grammar->lex_goals[k].is_parametrized)
            continue;
        if (compact == compact_goal)
            return grammar->lex_goals[k].template_rule_index;
        compact++;
    }
    return PP_NO_INDEX;
}

/* ** Emit an indent prefix. */
static void pp_parser_indent(pp_parser_emit *ctx, const char *indent)
{
    pp_parser_text(ctx->cursor, indent);
}

/* ** Emit the C expression that, at runtime, is the IR node `node's child[k]:
** simply `(<node>)->children[<k>]'. The structural path the emitter walked at emit
** time reaches the same node at run time because pack.c round-trips children in
** order. Allocates the small expression into a caller-supplied buffer so it can be
** passed down as the child's `node' argument. */
static void pp_parser_child_expr(char *buffer, const char *node, uint32_t k)
{
    /* buffer is sized generously by every caller (>= strlen(node)+24). */
    uint32_t i = 0, j = 0;
    char     digits[10];
    uint32_t dc = 0, v = k;
    buffer[i++] = '(';
    while (node[j] != '\0') buffer[i++] = node[j++];
    buffer[i++] = ')';
    buffer[i++] = '-'; buffer[i++] = '>';
    buffer[i++] = 'c'; buffer[i++] = 'h'; buffer[i++] = 'i';
    buffer[i++] = 'l'; buffer[i++] = 'd'; buffer[i++] = 'r';
    buffer[i++] = 'e'; buffer[i++] = 'n'; buffer[i++] = '[';
    if (v == 0) { digits[dc++] = '0'; }
    else { while (v > 0) { digits[dc++] = (char)('0' + v % 10u); v /= 10u; } }
    while (dc > 0) buffer[i++] = digits[--dc];
    buffer[i++] = ']';
    buffer[i] = '\0';
}

/* ** Emit a REF node: a direct named call. A rule reference becomes
** `sig = parse_<callee>(state, sink, field, alias);' (the callee runs the whole
** CALL -- enter/body/exit -- so the leading-extras consume and node open/close
** all happen inside it, byte-identically to the VM's PP_VM_CALL). A token
** reference becomes `sig = scan_<callee>(state, sink, field, alias);'. An
** unresolved reference matches nothing -- `sig = pp_vm_do_match_pattern(state, 0,
** PP_NO_SYMBOL, ...)' returns 0 -- mirroring the VM's NULL-pattern MATCH. */
static void pp_parser_emit_ref(pp_parser_emit *ctx, const pp_ir *ir,
                               const char *sink, const char *field,
                               const char *alias, const char *indent)
{
    pp_parser_indent(ctx, indent);
    pp_parser_text(ctx->cursor, "sig = ");
    if (pp_parser_is_rule(ctx->grammar, ir->symbol)) {
        pp_parser_rule_fn(ctx->cursor, ctx->prefix, ctx->grammar, ir->symbol);
        pp_parser_text(ctx->cursor, "(state, ");
        pp_parser_text(ctx->cursor, sink);
        pp_parser_text(ctx->cursor, ", ");
        pp_parser_text(ctx->cursor, field);
        pp_parser_text(ctx->cursor, ", ");
        pp_parser_text(ctx->cursor, alias);
        pp_parser_text(ctx->cursor, ");\n");
    } else if (pp_parser_is_token(ctx->grammar, ir->symbol)) {
        pp_parser_token_fn(ctx->cursor, ctx->prefix, ctx->grammar, ir->symbol);
        pp_parser_text(ctx->cursor, "(state, ");
        pp_parser_text(ctx->cursor, sink);
        pp_parser_text(ctx->cursor, ", ");
        pp_parser_text(ctx->cursor, field);
        pp_parser_text(ctx->cursor, ", ");
        pp_parser_text(ctx->cursor, alias);
        pp_parser_text(ctx->cursor, ");\n");
    } else {
        /* Unresolved reference: an always-failing match, exactly as the VM lowers a
        ** reference that names neither a rule nor a token to a NULL-pattern MATCH. */
        pp_parser_text(ctx->cursor, "pp_vm_do_match_pattern(state, 0, PP_NO_SYMBOL, ");
        pp_parser_text(ctx->cursor, sink);
        pp_parser_text(ctx->cursor, ", ");
        pp_parser_text(ctx->cursor, field);
        pp_parser_text(ctx->cursor, ", ");
        pp_parser_text(ctx->cursor, alias);
        pp_parser_text(ctx->cursor, ");\n");
    }
}

/* ** Translate one IR node to C that sets `sig' to its match signal. `node' is the
** C-expression that, at runtime, is this IR node (used to read in-place operands a
** leaf helper needs: a literal's bytes, a recover node's payload, an inject node's
** language). `sink'/`field'/`alias' are C-expressions for the child sink and the
** decoration in force. Every op reproduces the corresponding pp_vm_run arm's effect
** step for step, in the same order, so the tree is byte-identical. */
static void pp_parser_emit_ir(pp_parser_emit *ctx, const char *node,
                              const pp_ir *ir, const char *sink,
                              const char *field, const char *alias,
                              const char *indent)
{
    pp_parser_cursor *c = ctx->cursor;

    switch (ir->op) {

    case PP_OP_REF:
        pp_parser_emit_ref(ctx, ir, sink, field, alias, indent);
        break;

    case PP_OP_LITERAL: {
        /* A bare literal: match its own bytes at the cursor and emit a leaf under the
        ** literal's kind (its text). W17-GOAL: route a non-empty literal through a
        ** baked pure matcher (<prefix>_litmatch_<n>: straight-line byte compares) via
        ** pp_vm_terminal_baked instead of the generic token VM (pp_vm_do_match_pattern
        ** -> pp_token_run, the largest remaining lexer slice). A bare literal is never
        ** goal-redirected (its kind is the literal text, not a token symbol), so
        ** pp_vm_terminal_baked is byte-identical to pp_vm_do_match_pattern here -- same
        ** extras consume, same leaf push, same `token' trace, same end offset. An empty
        ** literal (no bytes to bake) keeps the generic path. */
        uint32_t lit = pp_parser_register_literal(ctx, ir);
        pp_parser_indent(ctx, indent);
        if (lit == PP_NO_INDEX) {
            pp_parser_text(c, "sig = pp_vm_do_match_pattern(state, (pp_ir *)(");
            pp_parser_text(c, node);
            pp_parser_text(c, "), ");
            pp_parser_emit_symbol(c, ir->symbol);
            pp_parser_text(c, ", ");
            pp_parser_text(c, sink);
            pp_parser_text(c, ", ");
            pp_parser_text(c, field);
            pp_parser_text(c, ", ");
            pp_parser_text(c, alias);
            pp_parser_text(c, ");\n");
        } else {
            pp_parser_text(c, "sig = pp_vm_terminal_baked(state, (pp_ir *)(");
            pp_parser_text(c, node);
            pp_parser_text(c, "), ");
            pp_parser_emit_symbol(c, ir->symbol);
            pp_parser_text(c, ", ");
            pp_parser_text(c, sink);
            pp_parser_text(c, ", ");
            pp_parser_text(c, field);
            pp_parser_text(c, ", ");
            pp_parser_text(c, alias);
            pp_parser_text(c, ",\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "                          ");
            pp_parser_name(c, ctx->prefix, "pp_parser_litmatch", "litmatch");
            pp_parser_byte(c, '_');
            pp_parser_u32(c, lit);
            pp_parser_text(c, ");\n");
        }
        break;
    }

    case PP_OP_NOT:
        /* `(not GUARD BODY)': the lowering splits on child[1]. When child[1] is purely
        ** token-level (no PP_OP_REF anywhere -- the classic `(not X any)'), the whole
        ** NOT is one byte-level terminal, matched by pp_vm_do_byte_pattern below
        ** exactly like the VM's PP_VM_BYTE. When child[1] reaches a rule, the VM lowers
        ** it to PP_VM_NOT_RULE; the generated parser reproduces that here (the
        ** rule-level NOT path), so the SAME (not HeredocDelimLine BodyCommand) guard /
        ** body runs identically. The split test is vmcompile's pp_ir_subtree_has_ref. */
        if (ir->child_count == 2 && pp_parser_subtree_has_ref(ir->children[1])) {
            pp_parser_emit_not_rule(ctx, node, ir, sink, field, alias, indent);
            break;
        }
        /* fall through to the byte-level operator path */
        /* FALLTHROUGH */
    case PP_OP_ANY:
    case PP_OP_CLASS:
    case PP_OP_CLASS_NAME:
    case PP_OP_NOT_KEYWORD:
        /* A byte-level / reserved operator used directly in a rule body: an
        ** anonymous one-byte terminal that consumes extras, matches and advances
        ** WITHOUT emitting a node (pp_vm_do_byte_pattern). The pattern IR is `node'.
        ** pp_vm_do_byte_pattern returns 1/0/-1; map to the MATCH/FAIL/OOM signals
        ** exactly as the VM's PP_VM_BYTE does (1->MATCH_OK, 0->FAIL, -1->OOM, which
        ** are the values PP_VM_MATCH_OK/FAIL/OOM hold). */
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "sig = pp_vm_do_byte_pattern(state, (pp_ir *)(");
        pp_parser_text(c, node);
        pp_parser_text(c, "), ");
        pp_parser_text(c, sink);
        pp_parser_text(c, ");\n");
        break;

    case PP_OP_EOF:
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "sig = pp_vm_do_eof(state, ");
        pp_parser_text(c, sink);
        pp_parser_text(c, ");\n");
        break;

    case PP_OP_SEQ: {
        /* Sequential children as straight-line C. Each child runs into the same
        ** sink, undecorated; on a child failure, end-of-input recovery synthesizes a
        ** missing required element when armed, exactly like pp_vm_run's PP_VM_SEQ. The
        ** done-label id is captured up front so nested children (which bump the shared
        ** counter) cannot perturb the goto/label pairing. */
        uint32_t my = ctx->label++;
        uint32_t k;
        for (k = 0; k < ir->child_count; k++) {
            char childbuf[256];
            pp_parser_child_expr(childbuf, node, k);
            pp_parser_emit_ir(ctx, childbuf, ir->children[k], sink,
                              "PP_NO_SYMBOL", "PP_NO_SYMBOL", indent);
            pp_parser_indent(ctx, indent);
            pp_parser_text(c,
                "if (sig == PP_VM_OOM) goto rule_oom;\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "if (sig != PP_VM_MATCH_OK) {\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c,
                "    if (state->recover != 0 && state->position >= state->length &&\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c,
                "        state->position > state->recover_start) {\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        if (!pp_vm_recover_insert(state, ");
            pp_parser_text(c, sink);
            pp_parser_text(c, ")) goto rule_oom;\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "    } else {\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        sig = PP_VM_FAIL; goto seq_done_");
            pp_parser_u32(c, my);
            pp_parser_text(c, ";\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "    }\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "}\n");
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "sig = PP_VM_MATCH_OK;\n");
        pp_parser_text(c, "seq_done_");
        pp_parser_u32(c, my);
        pp_parser_text(c, ":;\n");
        break;
    }

    case PP_OP_CHOICE: {
        /* Ordered choice with W46 first-set prediction. Save the cheap marks, disarm
        ** recovery for the speculative alternatives, consume the shared leading
        ** extras once, then try each ADMITTED alternative -- a baked
        ** pp_choice_first_admits_fast byte test on the child's own IR node
        ** (`childbuf', the same node the VM keys the cached FIRST set on) guards the
        ** attempt -- restoring the marks between attempts; first match wins. Byte-
        ** identical to pp_vm_run's PP_VM_CHOICE. */
        uint32_t my = ctx->label++;
        uint32_t k;
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "{\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t ch_count_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->parser->builder.count;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t ch_pos_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->position;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t ch_sink_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = ("); pp_parser_text(c, sink);
        pp_parser_text(c, ") ? ("); pp_parser_text(c, sink);
        pp_parser_text(c, ")->count : 0;\n");
        /* Held `attach: leading' trivia high-water mark, captured before the shared
        ** leading-extras consume below; restored on each alternative's failure so a
        ** flush an abandoned alternative drained is re-exposed for the next (mirrors
        ** the VM's PP_VM_CHOICE save/restore). */
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t ch_held_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->held_count;\n");
        /* Content-end high-water mark: like held_count it is speculative state the
        ** abandoned alternative's restore must roll back, or a node closing after the
        ** retry would over-extend (the cc-diff divergence; see content_end). */
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t ch_content_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->content_end;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    const pp_ir *ch_rec_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->recover;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t ch_look_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->recover = 0;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    if (!pp_vm_extras(state, ");
        pp_parser_text(c, sink);
        pp_parser_text(c, ")) { state->recover = ch_rec_"); pp_parser_u32(c, my);
        pp_parser_text(c, "; goto rule_oom; }\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    ch_look_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->position;\n");
        for (k = 0; k < ir->child_count; k++) {
            char childbuf[256];
            pp_parser_child_expr(childbuf, node, k);
            /* The baked FIRST-set guard: skip an alternative the predictor rejects,
            ** keyed on the child's IR node (the SAME node the VM consults). */
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "    if (pp_choice_first_admits_fast(state->grammar, (pp_ir *)(");
            pp_parser_text(c, childbuf);
            pp_parser_text(c, "), state->input, state->length, ch_look_");
            pp_parser_u32(c, my);
            pp_parser_text(c, ")) {\n");
            /* Slot-save: snapshot active slot counts before the speculative alternative
            ** (mirrors the VM's PP_VM_CHOICE pp_slot_save_push before pushing the alt). */
            if (ctx->uses_slots) {
                pp_parser_indent(ctx, indent);
                pp_parser_text(c, "        if (!pp_slot_save_push(state)) { state->recover = ch_rec_");
                pp_parser_u32(c, my);
                pp_parser_text(c, "; goto rule_oom; }\n");
            }
            /* The alternative inherits the choice's field/alias decoration. */
            {
                char inner[64];
                /* deeper indent for the alternative body */
                uint32_t li = 0;
                while (indent[li] != '\0' && li < 60) { inner[li] = indent[li]; li++; }
                inner[li++] = ' '; inner[li++] = ' ';
                inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' ';
                inner[li++] = ' '; inner[li] = '\0';
                pp_parser_emit_ir(ctx, childbuf, ir->children[k], sink,
                                  field, alias, inner);
            }
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        if (sig == PP_VM_OOM) { state->recover = ch_rec_");
            pp_parser_u32(c, my);
            pp_parser_text(c, "; goto rule_oom; }\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        if (sig == PP_VM_MATCH_OK) { ");
            if (ctx->uses_slots)
                pp_parser_text(c, "pp_slot_save_commit_pop(state); ");
            pp_parser_text(c, "state->recover = ch_rec_");
            pp_parser_u32(c, my);
            pp_parser_text(c, "; goto ch_done_"); pp_parser_u32(c, my);
            pp_parser_text(c, "; }\n");
            if (ctx->uses_slots) {
                pp_parser_indent(ctx, indent);
                pp_parser_text(c, "        pp_slot_save_restore_pop(state);\n");
            }
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        state->parser->builder.count = ch_count_");
            pp_parser_u32(c, my); pp_parser_text(c, ";\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        state->position = ch_pos_");
            pp_parser_u32(c, my); pp_parser_text(c, ";\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        if ("); pp_parser_text(c, sink);
            pp_parser_text(c, ") ("); pp_parser_text(c, sink);
            pp_parser_text(c, ")->count = ch_sink_"); pp_parser_u32(c, my);
            pp_parser_text(c, ";\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        state->held_count = ch_held_");
            pp_parser_u32(c, my); pp_parser_text(c, ";\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        state->content_end = ch_content_");
            pp_parser_u32(c, my); pp_parser_text(c, ";\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "    }\n");
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->recover = ch_rec_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    sig = PP_VM_FAIL;\n");
        pp_parser_text(c, "    ch_done_"); pp_parser_u32(c, my);
        pp_parser_text(c, ":;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "}\n");
        break;
    }

    case PP_OP_REPEAT: {
        /* Repeat the child until it fails or makes no progress (zero-width guard),
        ** with the min/max bounds, exactly like pp_vm_run's PP_VM_REPEAT. Recovery
        ** is disarmed inside the loop; the marks are re-saved before each attempt and
        ** restored on the terminating failure. */
        uint32_t my = ctx->label++;
        const pp_ir *child = ir->child_count >= 1 ? ir->children[0] : 0;
        char childbuf[256];
        char inner[64];
        uint32_t li = 0;
        while (indent[li] != '\0' && li < 60) { inner[li] = indent[li]; li++; }
        inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' ';
        inner[li] = '\0';
        pp_parser_child_expr(childbuf, node, 0);
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "{\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t rp_reps_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = 0;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t rp_count_"); pp_parser_u32(c, my);
        pp_parser_text(c, ", rp_pos_"); pp_parser_u32(c, my);
        pp_parser_text(c, ", rp_sink_"); pp_parser_u32(c, my);
        pp_parser_text(c, ", rp_held_"); pp_parser_u32(c, my);
        pp_parser_text(c, ", rp_content_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    const pp_ir *rp_rec_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->recover;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->recover = 0;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    for (;;) {\n");
        if (ir->max != 0) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        if (rp_reps_"); pp_parser_u32(c, my);
            pp_parser_text(c, " >= "); pp_parser_u32(c, ir->max);
            pp_parser_text(c, "u) { state->recover = rp_rec_"); pp_parser_u32(c, my);
            pp_parser_text(c, "; sig = (rp_reps_"); pp_parser_u32(c, my);
            pp_parser_text(c, " >= "); pp_parser_u32(c, ir->min);
            pp_parser_text(c, "u) ? PP_VM_MATCH_OK : PP_VM_FAIL; goto rp_done_");
            pp_parser_u32(c, my); pp_parser_text(c, "; }\n");
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        rp_count_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->parser->builder.count;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        rp_pos_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->position;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        rp_sink_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = ("); pp_parser_text(c, sink);
        pp_parser_text(c, ") ? ("); pp_parser_text(c, sink);
        pp_parser_text(c, ")->count : 0;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        rp_held_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->held_count;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        rp_content_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->content_end;\n");
        /* Slot-save before the iteration's speculative match (the VM's PP_VM_REPEAT
        ** pp_slot_save_push). Restored on a failed iteration, committed when the
        ** iteration matched (with or without progress). */
        if (ctx->uses_slots) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        if (!pp_slot_save_push(state)) { state->recover = rp_rec_");
            pp_parser_u32(c, my); pp_parser_text(c, "; goto rule_oom; }\n");
        }
        if (child == 0) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        sig = PP_VM_FAIL;\n");
        } else {
            pp_parser_emit_ir(ctx, childbuf, child, sink,
                              "PP_NO_SYMBOL", "PP_NO_SYMBOL", inner);
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        if (sig == PP_VM_OOM) { state->recover = rp_rec_");
        pp_parser_u32(c, my); pp_parser_text(c, "; goto rule_oom; }\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        if (sig != PP_VM_MATCH_OK || state->position == rp_pos_");
        pp_parser_u32(c, my); pp_parser_text(c, ") {\n");
        /* Terminating iteration: restore the slot batch on a real failure, commit it
        ** on a zero-width success (matches the VM's restore-vs-commit split). */
        if (ctx->uses_slots) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "            if (sig != PP_VM_MATCH_OK) "
                              "pp_slot_save_restore_pop(state);\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "            else pp_slot_save_commit_pop(state);\n");
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            state->parser->builder.count = rp_count_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            state->position = rp_pos_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            if ("); pp_parser_text(c, sink);
        pp_parser_text(c, ") ("); pp_parser_text(c, sink);
        pp_parser_text(c, ")->count = rp_sink_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            state->held_count = rp_held_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            state->content_end = rp_content_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            state->recover = rp_rec_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            sig = (rp_reps_"); pp_parser_u32(c, my);
        pp_parser_text(c, " >= "); pp_parser_u32(c, ir->min);
        pp_parser_text(c, "u) ? PP_VM_MATCH_OK : PP_VM_FAIL; goto rp_done_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        }\n");
        /* Iteration matched with progress: commit the slot batch (the VM's
        ** pp_slot_save_commit_pop before reps++). */
        if (ctx->uses_slots) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        pp_slot_save_commit_pop(state);\n");
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        rp_reps_"); pp_parser_u32(c, my);
        pp_parser_text(c, "++;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    }\n");
        pp_parser_text(c, "    rp_done_"); pp_parser_u32(c, my);
        pp_parser_text(c, ":;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "}\n");
        break;
    }

    case PP_OP_OPTIONAL: {
        /* Run the child once; on failure restore the marks; always succeed. The
        ** child inherits the optional's field/alias (pp_vm_run's PP_VM_OPTIONAL
        ** pushes the child with fr->field/fr->alias). */
        uint32_t my = ctx->label++;
        const pp_ir *child = ir->child_count >= 1 ? ir->children[0] : 0;
        char childbuf[256];
        pp_parser_child_expr(childbuf, node, 0);
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "{\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t op_count_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->parser->builder.count;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t op_pos_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->position;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t op_sink_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = ("); pp_parser_text(c, sink);
        pp_parser_text(c, ") ? ("); pp_parser_text(c, sink);
        pp_parser_text(c, ")->count : 0;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t op_held_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->held_count;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t op_content_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->content_end;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    const pp_ir *op_rec_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->recover;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->recover = 0;\n");
        /* Slot-save before the optional's speculative body (the VM's PP_VM_OPTIONAL
        ** pp_slot_save_push); restored on failure, committed on success. */
        if (ctx->uses_slots) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "    if (!pp_slot_save_push(state)) { state->recover = op_rec_");
            pp_parser_u32(c, my); pp_parser_text(c, "; goto rule_oom; }\n");
        }
        if (child == 0) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "    sig = PP_VM_FAIL;\n");
        } else {
            char inner[64];
            uint32_t li = 0;
            while (indent[li] != '\0' && li < 60) { inner[li] = indent[li]; li++; }
            inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' ';
            inner[li] = '\0';
            pp_parser_emit_ir(ctx, childbuf, child, sink, field, alias, inner);
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    if (sig == PP_VM_OOM) { state->recover = op_rec_");
        pp_parser_u32(c, my); pp_parser_text(c, "; goto rule_oom; }\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    if (sig != PP_VM_MATCH_OK) {\n");
        if (ctx->uses_slots) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "        pp_slot_save_restore_pop(state);\n");
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        state->parser->builder.count = op_count_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        state->position = op_pos_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        if ("); pp_parser_text(c, sink);
        pp_parser_text(c, ") ("); pp_parser_text(c, sink);
        pp_parser_text(c, ")->count = op_sink_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        state->held_count = op_held_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        state->content_end = op_content_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    }");
        if (ctx->uses_slots)
            pp_parser_text(c, " else { pp_slot_save_commit_pop(state); }");
        pp_parser_text(c, "\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->recover = op_rec_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    sig = PP_VM_MATCH_OK;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "}\n");
        break;
    }

    case PP_OP_FIELD: {
        /* FIELD decorates its child's node with a field symbol and has no epilogue
        ** (pp_vm_run reuses the frame in place with the field set). So translate the
        ** child with the field literal threaded in, the alias inherited. */
        const pp_ir *child = ir->child_count >= 1 ? ir->children[0] : 0;
        char childbuf[256];
        char fieldlit[32];
        pp_parser_cursor tmp;
        uint32_t n;
        pp_parser_child_expr(childbuf, node, 0);
        tmp.bytes = fieldlit; tmp.length = 0;
        pp_parser_emit_symbol(&tmp, ir->symbol);
        n = tmp.length; fieldlit[n] = '\0';
        if (child == 0) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "sig = PP_VM_FAIL;\n");
        } else {
            pp_parser_emit_ir(ctx, childbuf, child, sink, fieldlit, alias, indent);
        }
        break;
    }

    case PP_OP_ALIAS: {
        /* ALIAS re-tags its child's node and has no epilogue: translate the child
        ** with the alias literal threaded in, the field inherited. */
        const pp_ir *child = ir->child_count >= 1 ? ir->children[0] : 0;
        char childbuf[256];
        char aliaslit[32];
        pp_parser_cursor tmp;
        uint32_t n;
        pp_parser_child_expr(childbuf, node, 0);
        tmp.bytes = aliaslit; tmp.length = 0;
        pp_parser_emit_symbol(&tmp, ir->symbol);
        n = tmp.length; aliaslit[n] = '\0';
        if (child == 0) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "sig = PP_VM_FAIL;\n");
        } else {
            pp_parser_emit_ir(ctx, childbuf, child, sink, field, aliaslit, indent);
        }
        break;
    }

    case PP_OP_GOAL: {
        /* Set the active lexical goal around the child, restore it after. The baked
        ** token matchers consult state->active_goal (and the grammar's lex_goals
        ** redirect table), so the regex-vs-divide / heredoc-body mechanism is intact. */
        uint32_t my = ctx->label++;
        const pp_ir *child = ir->child_count >= 1 ? ir->children[0] : 0;
        char childbuf[256];
        pp_parser_child_expr(childbuf, node, 0);
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "{\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t gl_saved_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->active_goal;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->active_goal = ");
        pp_parser_emit_symbol(c, ir->symbol);
        pp_parser_text(c, ";\n");
        if (child == 0) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "    sig = PP_VM_FAIL;\n");
        } else {
            char inner[64];
            uint32_t li = 0;
            while (indent[li] != '\0' && li < 60) { inner[li] = indent[li]; li++; }
            inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' ';
            inner[li] = '\0';
            pp_parser_emit_ir(ctx, childbuf, child, sink, field, alias, inner);
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->active_goal = gl_saved_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "}\n");
        break;
    }

    case PP_OP_NO_EXTRAS: {
        /* Toggle extras consumption around the child, restore after. The aux the VM
        ** carries is ir->min: 0 disables extras (no_extras=1), nonzero enables. */
        uint32_t my = ctx->label++;
        const pp_ir *child = ir->child_count >= 1 ? ir->children[0] : 0;
        char childbuf[256];
        pp_parser_child_expr(childbuf, node, 0);
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "{\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    int ne_saved_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->no_extras;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->no_extras = ");
        pp_parser_text(c, (ir->min == 0) ? "1" : "0");
        pp_parser_text(c, ";\n");
        if (child == 0) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "    sig = PP_VM_FAIL;\n");
        } else {
            char inner[64];
            uint32_t li = 0;
            while (indent[li] != '\0' && li < 60) { inner[li] = indent[li]; li++; }
            inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' ';
            inner[li] = '\0';
            pp_parser_emit_ir(ctx, childbuf, child, sink, field, alias, inner);
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->no_extras = ne_saved_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "}\n");
        break;
    }

    case PP_OP_RECOVER: {
        /* Try the body cleanly; on failure restore the marks, arm recovery with this
        ** recover node's baked diagnostic/insert payload (read in place off `node'),
        ** and re-run the body. Byte-identical to pp_vm_run's three-step PP_VM_RECOVER.
        ** The recover node's payload lives on `node' (the IR), reached by navigation. */
        uint32_t my = ctx->label++;
        const pp_ir *child = ir->child_count >= 1 ? ir->children[0] : 0;
        char childbuf[256];
        char inner[64];
        uint32_t li = 0;
        while (indent[li] != '\0' && li < 60) { inner[li] = indent[li]; li++; }
        inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' ';
        inner[li] = '\0';
        pp_parser_child_expr(childbuf, node, 0);
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "{\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t rc_count_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->parser->builder.count;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t rc_pos_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->position;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t rc_sink_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = ("); pp_parser_text(c, sink);
        pp_parser_text(c, ") ? ("); pp_parser_text(c, sink);
        pp_parser_text(c, ")->count : 0;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t rc_held_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->held_count;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t rc_content_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->content_end;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    const pp_ir *rc_rec_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->recover;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    int rc_fired_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->recover_fired;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t rc_start_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->recover_start;\n");
        /* First attempt: clean. */
        if (child == 0) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "    sig = PP_VM_FAIL;\n");
        } else {
            pp_parser_emit_ir(ctx, childbuf, child, sink, field, alias, inner);
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    if (sig == PP_VM_OOM) goto rule_oom;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    if (sig == PP_VM_MATCH_OK) goto rc_done_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->parser->builder.count = rc_count_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->position = rc_pos_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    if ("); pp_parser_text(c, sink);
        pp_parser_text(c, ") ("); pp_parser_text(c, sink);
        pp_parser_text(c, ")->count = rc_sink_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->held_count = rc_held_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->content_end = rc_content_");
        pp_parser_u32(c, my); pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->recover = (pp_ir *)(");
        pp_parser_text(c, node);
        pp_parser_text(c, ");\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->recover_fired = 0;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->recover_start = rc_pos_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        /* Second attempt: recovery armed. */
        if (child == 0) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "    sig = PP_VM_FAIL;\n");
        } else {
            pp_parser_emit_ir(ctx, childbuf, child, sink, field, alias, inner);
        }
        pp_parser_text(c, "    rc_done_"); pp_parser_u32(c, my);
        pp_parser_text(c, ":;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->recover = rc_rec_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->recover_fired = rc_fired_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    state->recover_start = rc_start_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "}\n");
        break;
    }

    case PP_OP_INJECT: {
        /* Match the region (the child sub-pattern), then splice an embedded parse of
        ** that span under an injection node. The inject runtime (pp_vm_do_inject ->
        ** pp_inject_run) is the shared primitive; the region is matched by this inject
        ** node's region function (registered in the pre-pass). The inject IR node
        ** (`node') carries the embedded grammar + language tag, read in place. The
        ** shared primitive resolves/runs/splices, exactly as the VM's PP_VM_INJECT does. */
        uint32_t which = pp_parser_inject_lookup(ctx, ir);
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "sig = pp_vm_do_inject(state, ");
        pp_parser_text(c, sink);
        pp_parser_text(c, ", (pp_ir *)(");
        pp_parser_text(c, node);
        pp_parser_text(c, "),\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "                      ");
        pp_parser_name(c, ctx->prefix, "pp_parser_inject_region", "inject_region");
        pp_parser_byte(c, '_');
        pp_parser_u32(c, which == PP_NO_INDEX ? 0u : which);
        pp_parser_text(c, ", (void *)(");
        pp_parser_text(c, node);
        pp_parser_text(c, "));\n");
        break;
    }

    case PP_OP_DEFERRED_SLOT:
        /* Declare a per-invocation slot. The rule's scope was pushed at entry (see
        ** pp_parser_emit_rule_fn); claim the matching frame in it and succeed zero-
        ** width -- no input consumed, no node emitted. A failed claim means the baked
        ** slot count under-counted (invariant break); fail cleanly, as the VM does. */
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "sig = pp_slot_claim(state, ");
        pp_parser_emit_symbol(c, ir->symbol);
        pp_parser_text(c, ") ? PP_VM_MATCH_OK : PP_VM_FAIL;\n");
        break;

    case PP_OP_DEFER: {
        /* Match the child sub-pattern transparently (same sink/field/alias, exactly
        ** like PP_OP_GOAL), then on success copy the matched byte span into the parse
        ** arena and enqueue it onto the resolved slot. Mirrors the VM's PP_VM_DEFER
        ** dispatch step for step. The captured node_index is the first node the child
        ** added to the sink (post-parse cross-link metadata). */
        uint32_t my = ctx->label++;
        const pp_ir *child = ir->child_count >= 1 ? ir->children[0] : 0;
        char childbuf[256];
        char inner[64];
        uint32_t li = 0;
        while (indent[li] != '\0' && li < 60) { inner[li] = indent[li]; li++; }
        inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' ';
        inner[li] = '\0';
        pp_parser_child_expr(childbuf, node, 0);
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "{\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t df_pos_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->position;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    uint32_t df_look_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = ("); pp_parser_text(c, sink);
        pp_parser_text(c, ") ? ("); pp_parser_text(c, sink);
        pp_parser_text(c, ")->count : 0;\n");
        if (child == 0) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "    sig = PP_VM_FAIL;\n");
        } else {
            pp_parser_emit_ir(ctx, childbuf, child, sink, field, alias, inner);
        }
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    if (sig == PP_VM_MATCH_OK) {\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        uint32_t df_start_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = df_pos_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        uint32_t df_len_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = state->position - df_start_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        pp_slot_frame *df_slot_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        pp_deferred_item df_item_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        uint8_t *df_copy_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = 0;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        if (df_len_"); pp_parser_u32(c, my);
        pp_parser_text(c, " > 0) {\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            uint32_t df_b_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            df_copy_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = (uint8_t *)pp_alloc(state->parser->arena, df_len_");
        pp_parser_u32(c, my); pp_parser_text(c, ");\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            if (df_copy_"); pp_parser_u32(c, my);
        pp_parser_text(c, " == 0) goto rule_oom;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            for (df_b_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = 0; df_b_"); pp_parser_u32(c, my);
        pp_parser_text(c, " < df_len_"); pp_parser_u32(c, my);
        pp_parser_text(c, "; df_b_"); pp_parser_u32(c, my);
        pp_parser_text(c, "++)\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "                df_copy_"); pp_parser_u32(c, my);
        pp_parser_text(c, "[df_b_"); pp_parser_u32(c, my);
        pp_parser_text(c, "] = state->input[df_start_"); pp_parser_u32(c, my);
        pp_parser_text(c, " + df_b_"); pp_parser_u32(c, my);
        pp_parser_text(c, "];\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        }\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        df_item_"); pp_parser_u32(c, my);
        pp_parser_text(c, ".text = df_copy_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        df_item_"); pp_parser_u32(c, my);
        pp_parser_text(c, ".text_len = df_len_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        df_item_"); pp_parser_u32(c, my);
        pp_parser_text(c, ".node_index = (("); pp_parser_text(c, sink);
        pp_parser_text(c, ") != 0 && ("); pp_parser_text(c, sink);
        pp_parser_text(c, ")->count > df_look_"); pp_parser_u32(c, my);
        pp_parser_text(c, ")\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            ? ("); pp_parser_text(c, sink);
        pp_parser_text(c, ")->items[df_look_"); pp_parser_u32(c, my);
        pp_parser_text(c, "] : PP_NO_INDEX;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        df_item_"); pp_parser_u32(c, my);
        pp_parser_text(c, "._pad = 0;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        df_slot_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = pp_slot_resolve(state, ");
        pp_parser_emit_symbol(c, ir->symbol);
        pp_parser_text(c, ");\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        if (df_slot_"); pp_parser_u32(c, my);
        pp_parser_text(c, " != 0) {\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            if (!pp_slot_push(state->parser->arena, df_slot_");
        pp_parser_u32(c, my); pp_parser_text(c, ", df_item_"); pp_parser_u32(c, my);
        pp_parser_text(c, ")) goto rule_oom;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        }\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    }\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "}\n");
        break;
    }

    case PP_OP_PARSE_DEFERRED: {
        /* DRAIN the resolved slot FIFO: for each captured item, in order, instantiate
        ** the parametrized delimiter (when this drain targets a parametrized goal),
        ** push it on the goal stack, invoke the target rule once into the current
        ** sink, then pop. COMMITTED and non-failing -- a failed rule invocation does
        ** NOT abort the drain and the cursor is left where the rule stopped. Mirrors
        ** the VM's PP_VM_PARSE_DEFERRED. The slot is re-resolved once up front; its
        ** count is fixed during the drain. ir->min is the target rule index; ir->max
        ** is the compact parametrized-goal index (PP_NO_INDEX for a non-parametrized
        ** drain); the template rule index is baked from the grammar at codegen time. */
        uint32_t my = ctx->label++;
        uint32_t goal_idx = ir->max;
        uint32_t tmpl = pp_parser_param_template_rule(ctx->grammar, ir->max);
        uint32_t target_sym = (ir->min != PP_NO_INDEX &&
                               ir->min < ctx->grammar->rule_count)
            ? ctx->grammar->rules[ir->min].symbol : PP_NO_SYMBOL;
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "{\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    pp_slot_frame *pd_slot_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = pp_slot_resolve(state, ");
        pp_parser_emit_symbol(c, ir->symbol);
        pp_parser_text(c, ");\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    if (pd_slot_"); pp_parser_u32(c, my);
        pp_parser_text(c, " == 0 || pd_slot_"); pp_parser_u32(c, my);
        pp_parser_text(c, "->count == 0) {\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        sig = PP_VM_MATCH_OK;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    } else {\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        uint32_t pd_i_"); pp_parser_u32(c, my);
        pp_parser_text(c, ";\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        for (pd_i_"); pp_parser_u32(c, my);
        pp_parser_text(c, " = 0; pd_i_"); pp_parser_u32(c, my);
        pp_parser_text(c, " < pd_slot_"); pp_parser_u32(c, my);
        pp_parser_text(c, "->count; pd_i_"); pp_parser_u32(c, my);
        pp_parser_text(c, "++) {\n");
        if (goal_idx != PP_NO_INDEX) {
            /* Parametrized drain: instantiate the captured delimiter into the param
            ** goal template, push it before the rule runs (the lexer reads the stack
            ** top), pop after -- BEFORE advancing, like the VM, so push/pop balance. */
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "            pp_deferred_item pd_item_"); pp_parser_u32(c, my);
            pp_parser_text(c, " = pd_slot_"); pp_parser_u32(c, my);
            pp_parser_text(c, "->items[pd_i_"); pp_parser_u32(c, my);
            pp_parser_text(c, "];\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "            const pp_ir *pd_pat_"); pp_parser_u32(c, my);
            pp_parser_text(c, " = pp_param_instantiate(state->parser->arena, "
                              "state->grammar, ");
            pp_parser_u32(c, tmpl); pp_parser_text(c, "u,\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "                pd_item_"); pp_parser_u32(c, my);
            pp_parser_text(c, ".text, pd_item_"); pp_parser_u32(c, my);
            pp_parser_text(c, ".text_len);\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "            if (pd_pat_"); pp_parser_u32(c, my);
            pp_parser_text(c, " == 0) goto rule_oom;\n");
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "            if (!pp_param_stack_push(state->parser->arena, "
                              "&state->parser->param_stacks[");
            pp_parser_u32(c, goal_idx); pp_parser_text(c, "u], pd_pat_");
            pp_parser_u32(c, my); pp_parser_text(c, ")) goto rule_oom;\n");
        }
        /* Invoke the target rule into the current sink. */
        if (target_sym != PP_NO_SYMBOL) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "            sig = ");
            pp_parser_rule_fn(c, ctx->prefix, ctx->grammar, target_sym);
            pp_parser_text(c, "(state, "); pp_parser_text(c, sink);
            pp_parser_text(c, ", PP_NO_SYMBOL, PP_NO_SYMBOL);\n");
        } else {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "            sig = PP_VM_FAIL;\n");
        }
        if (goal_idx != PP_NO_INDEX) {
            pp_parser_indent(ctx, indent);
            pp_parser_text(c, "            pp_param_stack_pop(&state->parser->param_stacks[");
            pp_parser_u32(c, goal_idx); pp_parser_text(c, "u]);\n");
        }
        /* Committed/non-failing: ignore the child signal except OOM. */
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "            if (sig == PP_VM_OOM) goto rule_oom;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        }\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "        sig = PP_VM_MATCH_OK;\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    }\n");
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "}\n");
        break;
    }

    case PP_OP_PARAM_REF:
        /* PP_OP_PARAM_REF is a leaf in a (param …) template, replaced by a
        ** PP_OP_LITERAL during pp_param_instantiate and matched by the token matcher;
        ** it is never emitted as rule-body code. Fail defensively if ever reached. */
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "sig = PP_VM_FAIL;\n");
        break;

    default:
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "sig = PP_VM_FAIL;\n");
        break;
    }
}

/* ** Emit a rule-level `(not GUARD BODY)' (the VM's PP_VM_NOT_RULE). The guard
** (child[0]) is a token-level lookahead resolved exactly as the lowerer would: when
** it references a token, the base pattern is that token's `pattern' and the trigger
** is the token symbol (so a live parametrized goal redirects -- HeredocDelimLine);
** otherwise the base is child[0]'s own IR and the trigger is PP_NO_SYMBOL. The
** shared pp_vm_not_rule_guard evaluates it purely (no cursor advance); if the guard
** matches, the NOT fails; otherwise BODY (child[1]) runs transparently into the same
** sink, its node becoming the NOT's contribution -- byte-identical to PP_VM_NOT_RULE. */
static void pp_parser_emit_not_rule(pp_parser_emit *ctx, const char *node,
                                    const pp_ir *ir, const char *sink,
                                    const char *field, const char *alias,
                                    const char *indent)
{
    pp_parser_cursor *c = ctx->cursor;
    uint32_t my = ctx->label++;
    const pp_ir *guard = ir->children[0];
    const pp_ir *body  = ir->children[1];
    char child1buf[256];
    char inner[64];
    uint32_t li = 0;
    int guard_is_token_ref =
        guard != 0 && guard->op == PP_OP_REF &&
        pp_parser_is_token(ctx->grammar, guard->symbol);
    while (indent[li] != '\0' && li < 60) { inner[li] = indent[li]; li++; }
    inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' '; inner[li++] = ' ';
    inner[li] = '\0';
    pp_parser_child_expr(child1buf, node, 1);
    pp_parser_indent(ctx, indent);
    pp_parser_text(c, "if (pp_vm_not_rule_guard(state, ");
    if (guard_is_token_ref) {
        /* Base is the referenced token's pattern; the trigger drives goal redirect. */
        uint32_t ti = pp_parser_token_index(ctx->grammar, guard->symbol);
        char patexpr[64];
        pp_parser_token_pattern_expr(patexpr, ti == PP_NO_INDEX ? 0u : ti);
        pp_parser_text(c, patexpr);
        pp_parser_text(c, ", ");
        pp_parser_emit_symbol(c, guard->symbol);
    } else {
        /* Base is child[0]'s own IR; no trigger (a literal/class/composite guard). */
        char guardbuf[256];
        pp_parser_child_expr(guardbuf, node, 0);
        pp_parser_text(c, "(pp_ir *)(");
        pp_parser_text(c, guardbuf);
        pp_parser_text(c, "), PP_NO_SYMBOL");
    }
    pp_parser_text(c, ")) {\n");
    pp_parser_indent(ctx, indent);
    pp_parser_text(c, "    sig = PP_VM_FAIL;\n");
    pp_parser_indent(ctx, indent);
    pp_parser_text(c, "} else {\n");
    (void)my;
    if (body == 0) {
        pp_parser_indent(ctx, indent);
        pp_parser_text(c, "    sig = PP_VM_FAIL;\n");
    } else {
        /* BODY runs transparently with the NOT's field/alias, like PP_VM_NOT_RULE. */
        pp_parser_emit_ir(ctx, child1buf, body, sink, field, alias, inner);
    }
    pp_parser_indent(ctx, indent);
    pp_parser_text(c, "}\n");
}

/* ====================================================================== */
/* Walk the IR collecting which rules and tokens are reachable, so only the   */
/* parse_/scan_ functions actually used are emitted (and forward-declared).   */
/* ====================================================================== */

/* ** A small set of reachable rule / token symbols. Sized once from the grammar's
** rule+token count (an upper bound on distinct reachable symbols). */
typedef struct pp_parser_reach {
    uint32_t *rule_symbols;   uint32_t rule_count;
    uint32_t *token_symbols;  uint32_t token_count;
    const pocketparse_grammar *grammar;
} pp_parser_reach;

/* ** Whether `symbol' is already in the `set' array of `count' ids. */
static int pp_parser_reach_has(const uint32_t *set, uint32_t count, uint32_t symbol)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        if (set[i] == symbol)
            return 1;
    return 0;
}

static void pp_parser_reach_ir(pp_parser_reach *reach, const pp_ir *ir);

/* ** Mark a rule reachable (recording its symbol and walking its body once) so a
** parse_<rule> function is emitted for it. Recursion is bounded by the rule set
** (each rule is walked at most once). */
static void pp_parser_reach_rule(pp_parser_reach *reach, uint32_t symbol)
{
    uint32_t i;
    if (pp_parser_reach_has(reach->rule_symbols, reach->rule_count, symbol))
        return;
    reach->rule_symbols[reach->rule_count++] = symbol;
    for (i = 0; i < reach->grammar->rule_count; i++)
        if (reach->grammar->rules[i].symbol == symbol) {
            pp_parser_reach_ir(reach, reach->grammar->rules[i].body);
            return;
        }
}

/* ** Mark a token reachable (recording its symbol) so a scan_<token> function is
** emitted for it. A token pattern is matched as a whole by the shared terminal
** helper, so its inner IR is not walked for reachability. */
static void pp_parser_reach_token(pp_parser_reach *reach, uint32_t symbol)
{
    if (pp_parser_reach_has(reach->token_symbols, reach->token_count, symbol))
        return;
    reach->token_symbols[reach->token_count++] = symbol;
}

/* ** Walk an IR node, marking every rule/token a REF reaches and descending into
** combinator children, so the reachable-symbol sets are complete. */
static void pp_parser_reach_ir(pp_parser_reach *reach, const pp_ir *ir)
{
    uint32_t i;
    if (ir == 0)
        return;
    if (ir->op == PP_OP_REF) {
        if (pp_parser_is_rule(reach->grammar, ir->symbol))
            pp_parser_reach_rule(reach, ir->symbol);
        else if (pp_parser_is_token(reach->grammar, ir->symbol))
            pp_parser_reach_token(reach, ir->symbol);
    } else if (ir->op == PP_OP_PARSE_DEFERRED) {
        /* The drained rule is referenced by index (ir->min), not as a PP_OP_REF, so
        ** mark it (and its body's references) reachable explicitly -- otherwise its
        ** parse_<rule> function is never emitted and the generated drain fails to link
        ** (lex-goal-implementation, generated PARSE_DEFERRED). */
        if (ir->min != PP_NO_INDEX && ir->min < reach->grammar->rule_count)
            pp_parser_reach_rule(reach, reach->grammar->rules[ir->min].symbol);
    }
    for (i = 0; i < ir->child_count; i++)
        pp_parser_reach_ir(reach, ir->children[i]);
}

/* ** The C-expression for a rule's body root IR node, at runtime:
** `state->grammar->rules[<i>].body', where <i> is the rule's index in the grammar's
** rule table (which round-trips in order through pack.c). */
static void pp_parser_rule_body_expr(char *buffer, uint32_t rule_index)
{
    pp_parser_cursor tmp;
    tmp.bytes = buffer; tmp.length = 0;
    pp_parser_text(&tmp, "state->grammar->rules[");
    pp_parser_u32(&tmp, rule_index);
    pp_parser_text(&tmp, "].body");
    buffer[tmp.length] = '\0';
}

/* ** The C-expression for a token's pattern root IR node, at runtime:
** `state->grammar->tokens[<i>].pattern'. */
static void pp_parser_token_pattern_expr(char *buffer, uint32_t token_index)
{
    pp_parser_cursor tmp;
    tmp.bytes = buffer; tmp.length = 0;
    pp_parser_text(&tmp, "state->grammar->tokens[");
    pp_parser_u32(&tmp, token_index);
    pp_parser_text(&tmp, "].pattern");
    buffer[tmp.length] = '\0';
}

/* ** Find a rule's index in the grammar table (PP_NO_INDEX if absent). */
static uint32_t pp_parser_rule_index(const pocketparse_grammar *grammar,
                                     uint32_t symbol)
{
    uint32_t i;
    for (i = 0; i < grammar->rule_count; i++)
        if (grammar->rules[i].symbol == symbol)
            return i;
    return PP_NO_INDEX;
}

/* ** Find a token's index in the grammar table (PP_NO_INDEX if absent). */
static uint32_t pp_parser_token_index(const pocketparse_grammar *grammar,
                                      uint32_t symbol)
{
    uint32_t i;
    for (i = 0; i < grammar->token_count; i++)
        if (grammar->tokens[i].symbol == symbol)
            return i;
    return PP_NO_INDEX;
}

/* ** Emit one rule's parse function. It runs the WHOLE rule CALL: the hand-written
** prologue (pp_vm_enter -- leading-extras consume, node open; NO packrat memo), the
** rule body translated to inline C (running into the frame's child sink, guarded by
** the depth limit so a pathologically deep recursion fails gracefully), and the
** epilogue (pp_vm_exit -- child link, node close). Byte-identical to pp_vm_run's
** PP_VM_CALL of this rule -- the memo the VM would run is a pure cache the generated
** parser does not carry (Phase 17 de-packrat). */
static void pp_parser_emit_rule_fn(pp_parser_emit *ctx, uint32_t symbol)
{
    pp_parser_cursor *c = ctx->cursor;
    const pocketparse_grammar *grammar = ctx->grammar;
    uint32_t rule_index = pp_parser_rule_index(grammar, symbol);
    const pp_rule_def *def;
    char bodyexpr[64];

    if (rule_index == PP_NO_INDEX)
        return;
    def = &grammar->rules[rule_index];

    pp_parser_text(c, "/* parse the rule `");
    {
        pocketparse_string name = pp_intern_name(&grammar->interner, symbol);
        pp_parser_slice(c, name);
    }
    pp_parser_text(c, "': open its node, parse its body, link and close. */\n");
    pp_parser_text(c, "static int ");
    pp_parser_rule_fn(c, ctx->prefix, grammar, symbol);
    pp_parser_text(c,
        "(pp_vm_state *state, pp_vm_sink *sink, uint32_t field, uint32_t alias)\n"
        "{\n"
        "    pp_vm_frame fr;\n"
        "    int resolved = PP_VM_FAIL;\n"
        "    int sig;\n");
    /* The CALL prologue. pp_vm_enter is the inline hand-written-RD wrapper
    ** (Phase 17 de-packrat): on the dominant steady state it does the marks / extras
    ** / node-open as straight-line C with this rule's kind in registers, carrying NO
    ** packrat memo, and DELEGATES to the shared pp_vm_call_enter only when the
    ** lifecycle is genuinely non-trivial (recovery armed, a no_extras scope, a trace
    ** plug), where the memo is inert because the generated driver pins it off. */
    pp_parser_text(c, "    if (!pp_vm_enter(state, &fr, ");
    pp_parser_emit_symbol(c, symbol);
    pp_parser_text(c, ", sink, field, alias, &resolved))\n"
        "        return resolved;\n");
    /* The depth guard wraps the body recursion: when entering this body would
    ** exceed the measured limit, FAIL gracefully rather than overflow the C stack. */
    pp_parser_text(c,
        "    if (state->depth >= state->depth_limit) {\n"
        "        sig = PP_VM_FAIL;\n"
        "    } else {\n"
        "        state->depth++;\n"
        "        {\n");
    /* Deferred-slot scope (lex-goal-implementation, Option A): when this rule
    ** declares (deferred-slot …)s, push a fresh slot scope onto the parser's
    ** UNIFIED slot stack at rule entry -- the SAME stack the VM pushes onto in its
    ** PP_VM_CALL dispatch -- so (defer …)/(parse-deferred …) and the slot-save
    ** rollback resolve identically. The count is baked from the rule's IR. The pop
    ** below balances it on every non-OOM path (an OOM goto aborts the whole parse,
    ** so the abandoned scope never matters). */
    if (def->slot_count > 0) {
        pp_parser_text(c,
            "            if (pp_slot_scope_push(state->parser, "
            "state->parser->arena, ");
        pp_parser_u32(c, def->slot_count);
        pp_parser_text(c, "u) == 0) goto rule_oom;\n");
    }
    pp_parser_rule_body_expr(bodyexpr, rule_index);
    pp_parser_emit_ir(ctx, bodyexpr, def->body, "fr.children",
                      "PP_NO_SYMBOL", "PP_NO_SYMBOL", "            ");
    if (def->slot_count > 0)
        pp_parser_text(c,
            "            pp_slot_scope_pop(state->parser);\n");
    pp_parser_text(c,
        "        }\n"
        "        state->depth--;\n"
        "    }\n");
    pp_parser_text(c,
        "    return pp_vm_exit(state, &fr, sink, field, alias, sig);\n"
        "rule_oom:\n"
        "    return PP_VM_OOM;\n"
        "}\n\n");
}

/* ====================================================================== */
/* The per-token baked lexer (W17-2): reproduce pp_match_ir for a token's      */
/* pattern as straight-line / looped C, so the generic token VM (pp_token_run, */
/* the ~30% profile slice) is replaced by the code a hand lexer is. A baked     */
/* matcher is a PURE function (input,length,start -> matched + end); the        */
/* extras/goal/leaf logic stays the shared pp_vm_terminal_baked. The baked      */
/* subset is the common token shapes (literal keywords/punctuation, byte        */
/* classes, class repeats, seq/choice/optional/repeat over those); a pattern    */
/* using a host class binding, a `not' lookahead, or a not-keyword falls back   */
/* to the generic matcher, so byte-identity is total.                          */
/* ====================================================================== */

/* ** Whether a token pattern is bakeable to straight-line C: the shapes whose
** pp_match_ir semantics are reproduced exactly below. CLASS_NAME (host binding),
** NOT/NOT_KEYWORD (lookahead / reserved-word), REF and the rule-only ops are NOT
** baked -- the token keeps the generic matcher, which is always correct. */
static int pp_parser_can_bake(const pp_ir *ir)
{
    uint32_t i;
    if (ir == 0)
        return 0;
    switch (ir->op) {
    case PP_OP_LITERAL:
    case PP_OP_CLASS:
    case PP_OP_ANY:
        return 1;
    case PP_OP_SEQ:
    case PP_OP_CHOICE:
        for (i = 0; i < ir->child_count; i++)
            if (!pp_parser_can_bake(ir->children[i]))
                return 0;
        return ir->child_count > 0;
    case PP_OP_REPEAT:
    case PP_OP_OPTIONAL:
        return ir->child_count >= 1 && pp_parser_can_bake(ir->children[0]);
    case PP_OP_NOT_KEYWORD:
        /* Match the inner pattern (child[0]); reject if the matched slice equals a
        ** reserved word (children[1..], all literals). The inner pattern must be
        ** bakeable; the reserved words are compared by bytes -- fully deterministic. */
        if (ir->child_count < 1 || !pp_parser_can_bake(ir->children[0]))
            return 0;
        for (i = 1; i < ir->child_count; i++)
            if (ir->children[i]->op != PP_OP_LITERAL)
                return 0;
        return 1;
    case PP_OP_NOT:
        /* (not GUARD BODY...): the GUARD (child[0]) and each BODY element
        ** (children[1..]) must be bakeable. The common (not GUARD any) -- a comment /
        ** string body byte -- bakes; this unlocks comments, strings and slash. */
        for (i = 0; i < ir->child_count; i++)
            if (!pp_parser_can_bake(ir->children[i]))
                return 0;
        return 1;
    default:
        return 0;
    }
}

/* ** Emit a `static const uint8_t <name>[256]' membership table for a byte class,
** honouring negation, so a class test is one array index (the form a hand lexer
** takes). The table is `static const' (the no-statics gate forbids only MUTABLE
** statics). */
static void pp_parser_emit_class_table(pp_parser_cursor *c, const char *prefix,
                                       uint32_t id, const pp_class *cls)
{
    uint32_t b;
    pp_parser_text(c, "static const uint8_t ");
    pp_parser_name(c, prefix, "pp_parser_class", "class");
    pp_parser_byte(c, '_');
    pp_parser_u32(c, id);
    pp_parser_text(c, "[256] = {");
    for (b = 0; b < 256; b++) {
        int in = ((cls->bits[b >> 3] >> (b & 7)) & 1) ? 1 : 0;
        if (cls->negated)
            in = !in;
        if (b % 32u == 0u)
            pp_parser_text(c, "\n    ");
        pp_parser_byte(c, in ? '1' : '0');
        pp_parser_byte(c, ',');
    }
    pp_parser_text(c, "\n};\n");
}

/* ** The class-table / label context threaded through the matcher emitter: a running
** class-table id (so each PP_OP_CLASS gets one [256] table) and a match-local label
** id, plus the cursor and prefix. */
typedef struct pp_parser_lex {
    pp_parser_cursor *cursor;
    const char       *prefix;
    uint32_t          class_id;
    uint32_t          label;
} pp_parser_lex;

/* ** Emit a class table for every PP_OP_CLASS in a token pattern, in pre-order, so
** the ids line up with the matcher body's re-derivation of the same pre-order. */
static void pp_parser_emit_class_tables(pp_parser_lex *lex, const pp_ir *ir)
{
    uint32_t i;
    if (ir == 0)
        return;
    if (ir->op == PP_OP_CLASS) {
        pp_parser_emit_class_table(lex->cursor, lex->prefix, lex->class_id, &ir->cls);
        lex->class_id++;
        return;
    }
    for (i = 0; i < ir->child_count; i++)
        pp_parser_emit_class_tables(lex, ir->children[i]);
}

/* ** Emit a short C-identifier label `<stem><my>' (optionally `<stem><my>_<k>') into
** `buffer'. */
static void pp_parser_label(char *buffer, const char *stem, uint32_t my,
                            int two, uint32_t k)
{
    pp_parser_cursor t;
    t.bytes = buffer; t.length = 0;
    pp_parser_text(&t, stem);
    pp_parser_u32(&t, my);
    if (two) { pp_parser_byte(&t, '_'); pp_parser_u32(&t, k); }
    buffer[t.length] = '\0';
}

/* ** Emit the matcher body for one token-pattern node: C that, starting from the
** cursor variable `cur', advances it on a match or `goto <fail>' on a mismatch,
** reproducing pp_match_ir op-for-op (possessive, greedy, left-to-right). The class-
** table id counter is advanced in the SAME pre-order pp_parser_emit_class_tables used,
** so the [256] table names line up. */
static void pp_parser_emit_match(pp_parser_lex *lex, const pp_ir *ir,
                                 const char *cur, const char *fail)
{
    pp_parser_cursor *c = lex->cursor;
    switch (ir->op) {

    case PP_OP_LITERAL: {
        uint32_t i;
        if (ir->text.length == 0)
            break;
        pp_parser_text(c, "    if ("); pp_parser_text(c, cur);
        pp_parser_text(c, " + "); pp_parser_u32(c, ir->text.length);
        pp_parser_text(c, "u > length) goto "); pp_parser_text(c, fail);
        pp_parser_text(c, ";\n");
        for (i = 0; i < ir->text.length; i++) {
            pp_parser_text(c, "    if (input["); pp_parser_text(c, cur);
            if (i > 0) { pp_parser_text(c, " + "); pp_parser_u32(c, i); pp_parser_text(c, "u"); }
            pp_parser_text(c, "] != "); pp_parser_u32(c, (uint32_t)ir->text.pointer[i]);
            pp_parser_text(c, "u) goto "); pp_parser_text(c, fail); pp_parser_text(c, ";\n");
        }
        pp_parser_text(c, "    "); pp_parser_text(c, cur);
        pp_parser_text(c, " += "); pp_parser_u32(c, ir->text.length);
        pp_parser_text(c, "u;\n");
        break;
    }

    case PP_OP_ANY:
        pp_parser_text(c, "    if ("); pp_parser_text(c, cur);
        pp_parser_text(c, " >= length) goto "); pp_parser_text(c, fail);
        pp_parser_text(c, ";\n    "); pp_parser_text(c, cur); pp_parser_text(c, "++;\n");
        break;

    case PP_OP_CLASS: {
        uint32_t id = lex->class_id++;
        pp_parser_text(c, "    if ("); pp_parser_text(c, cur);
        pp_parser_text(c, " >= length || !");
        pp_parser_name(c, lex->prefix, "pp_parser_class", "class");
        pp_parser_byte(c, '_'); pp_parser_u32(c, id);
        pp_parser_text(c, "[input["); pp_parser_text(c, cur);
        pp_parser_text(c, "]]) goto "); pp_parser_text(c, fail);
        pp_parser_text(c, ";\n    "); pp_parser_text(c, cur); pp_parser_text(c, "++;\n");
        break;
    }

    case PP_OP_SEQ: {
        uint32_t i;
        for (i = 0; i < ir->child_count; i++)
            pp_parser_emit_match(lex, ir->children[i], cur, fail);
        break;
    }

    case PP_OP_OPTIONAL: {
        uint32_t my = lex->label++;
        char save[40], ok[40], flbl[40];
        pp_parser_label(save, "opt_save_", my, 0, 0);
        pp_parser_label(ok,   "opt_ok_",   my, 0, 0);
        pp_parser_label(flbl, "opt_fail_", my, 0, 0);
        pp_parser_text(c, "    { uint32_t "); pp_parser_text(c, save);
        pp_parser_text(c, " = "); pp_parser_text(c, cur); pp_parser_text(c, ";\n");
        pp_parser_emit_match(lex, ir->children[0], cur, flbl);
        pp_parser_text(c, "    goto "); pp_parser_text(c, ok); pp_parser_text(c, ";\n");
        pp_parser_text(c, "    "); pp_parser_text(c, flbl); pp_parser_text(c, ": ");
        pp_parser_text(c, cur); pp_parser_text(c, " = "); pp_parser_text(c, save);
        pp_parser_text(c, ";\n    "); pp_parser_text(c, ok); pp_parser_text(c, ": ; }\n");
        break;
    }

    case PP_OP_REPEAT: {
        uint32_t my = lex->label++;
        char reps[40], save[40], flbl[40], done[40];
        pp_parser_label(reps, "rep_n_",    my, 0, 0);
        pp_parser_label(save, "rep_save_", my, 0, 0);
        pp_parser_label(flbl, "rep_fail_", my, 0, 0);
        pp_parser_label(done, "rep_done_", my, 0, 0);
        pp_parser_text(c, "    { uint32_t "); pp_parser_text(c, reps);
        pp_parser_text(c, " = 0, "); pp_parser_text(c, save); pp_parser_text(c, ";\n");
        pp_parser_text(c, "    for (;;) {\n");
        if (ir->max != 0) {
            pp_parser_text(c, "        if ("); pp_parser_text(c, reps);
            pp_parser_text(c, " >= "); pp_parser_u32(c, ir->max);
            pp_parser_text(c, "u) goto "); pp_parser_text(c, done); pp_parser_text(c, ";\n");
        }
        pp_parser_text(c, "        "); pp_parser_text(c, save);
        pp_parser_text(c, " = "); pp_parser_text(c, cur); pp_parser_text(c, ";\n");
        pp_parser_emit_match(lex, ir->children[0], cur, flbl);
        pp_parser_text(c, "        if ("); pp_parser_text(c, cur);
        pp_parser_text(c, " == "); pp_parser_text(c, save);
        pp_parser_text(c, ") goto "); pp_parser_text(c, done); pp_parser_text(c, ";\n");
        pp_parser_text(c, "        "); pp_parser_text(c, reps); pp_parser_text(c, "++;\n");
        pp_parser_text(c, "        continue;\n");
        pp_parser_text(c, "    "); pp_parser_text(c, flbl); pp_parser_text(c, ": ");
        pp_parser_text(c, cur); pp_parser_text(c, " = "); pp_parser_text(c, save);
        pp_parser_text(c, "; goto "); pp_parser_text(c, done); pp_parser_text(c, ";\n");
        pp_parser_text(c, "    }\n");
        pp_parser_text(c, "    "); pp_parser_text(c, done); pp_parser_text(c, ": ;\n");
        if (ir->min != 0) {
            pp_parser_text(c, "    if ("); pp_parser_text(c, reps);
            pp_parser_text(c, " < "); pp_parser_u32(c, ir->min);
            pp_parser_text(c, "u) goto "); pp_parser_text(c, fail); pp_parser_text(c, ";\n");
        }
        pp_parser_text(c, "    }\n");
        break;
    }

    case PP_OP_CHOICE: {
        uint32_t my = lex->label++;
        uint32_t k;
        char save[40], ok[40];
        pp_parser_label(save, "ch_save_", my, 0, 0);
        pp_parser_label(ok,   "ch_ok_",   my, 0, 0);
        pp_parser_text(c, "    { uint32_t "); pp_parser_text(c, save);
        pp_parser_text(c, " = "); pp_parser_text(c, cur); pp_parser_text(c, ";\n");
        for (k = 0; k < ir->child_count; k++) {
            char flbl[48];
            pp_parser_label(flbl, "ch_alt_", my, 1, k);
            pp_parser_emit_match(lex, ir->children[k], cur, flbl);
            pp_parser_text(c, "    goto "); pp_parser_text(c, ok); pp_parser_text(c, ";\n");
            pp_parser_text(c, "    "); pp_parser_text(c, flbl); pp_parser_text(c, ": ");
            pp_parser_text(c, cur); pp_parser_text(c, " = "); pp_parser_text(c, save);
            pp_parser_text(c, ";\n");
        }
        pp_parser_text(c, "    goto "); pp_parser_text(c, fail); pp_parser_text(c, ";\n");
        pp_parser_text(c, "    "); pp_parser_text(c, ok); pp_parser_text(c, ": ; }\n");
        break;
    }

    case PP_OP_NOT_KEYWORD: {
        /* Match the inner pattern (child[0]) advancing the cursor, then fail if the
        ** matched slice [kw_start,pos) equals one of the reserved-word literals
        ** (children[1..]) exactly -- whole-slice equality, like pp_match_ir. */
        uint32_t my = lex->label++;
        uint32_t k;
        char start[40];
        pp_parser_label(start, "nk_start_", my, 0, 0);
        pp_parser_text(c, "    { uint32_t "); pp_parser_text(c, start);
        pp_parser_text(c, " = "); pp_parser_text(c, cur); pp_parser_text(c, ";\n");
        pp_parser_emit_match(lex, ir->children[0], cur, fail);
        for (k = 1; k < ir->child_count; k++) {
            const pp_ir *w = ir->children[k];
            uint32_t j;
            pp_parser_text(c, "    if ("); pp_parser_text(c, cur);
            pp_parser_text(c, " - "); pp_parser_text(c, start);
            pp_parser_text(c, " == "); pp_parser_u32(c, w->text.length);
            pp_parser_text(c, "u");
            for (j = 0; j < w->text.length; j++) {
                pp_parser_text(c, " && input["); pp_parser_text(c, start);
                if (j > 0) { pp_parser_text(c, " + "); pp_parser_u32(c, j); pp_parser_text(c, "u"); }
                pp_parser_text(c, "] == "); pp_parser_u32(c, (uint32_t)w->text.pointer[j]);
                pp_parser_text(c, "u");
            }
            pp_parser_text(c, ") goto "); pp_parser_text(c, fail); pp_parser_text(c, ";\n");
        }
        pp_parser_text(c, "    }\n");
        break;
    }

    case PP_OP_NOT: {
        /* (not GUARD BODY...): fail if GUARD (child[0]) matches at the cursor; else
        ** match BODY (children[1..]) in sequence, or consume one byte when there is no
        ** body -- exactly like pp_match_ir's PP_OP_NOT. The GUARD is run on a scratch
        ** cursor with a local "guard failed" label: reaching its end means it matched,
        ** so the not fails; jumping to gfail means it did not, so we proceed. The class
        ** tables are still walked GUARD-first then BODY, matching emit_class_tables. */
        uint32_t my = lex->label++;
        uint32_t k;
        char guard[40], gfail[40];
        pp_parser_label(guard, "nt_g_",     my, 0, 0);
        pp_parser_label(gfail, "nt_gfail_", my, 0, 0);
        if (ir->child_count >= 1) {
            pp_parser_text(c, "    { uint32_t "); pp_parser_text(c, guard);
            pp_parser_text(c, " = "); pp_parser_text(c, cur); pp_parser_text(c, ";\n");
            pp_parser_emit_match(lex, ir->children[0], guard, gfail);
            /* GUARD matched -> the not fails. */
            pp_parser_text(c, "    goto "); pp_parser_text(c, fail); pp_parser_text(c, ";\n");
            pp_parser_text(c, "    "); pp_parser_text(c, gfail); pp_parser_text(c, ": ; }\n");
        }
        if (ir->child_count >= 2) {
            for (k = 1; k < ir->child_count; k++)
                pp_parser_emit_match(lex, ir->children[k], cur, fail);
        } else {
            /* (not X) with no body: consume a single byte not starting X. */
            pp_parser_text(c, "    if ("); pp_parser_text(c, cur);
            pp_parser_text(c, " >= length) goto "); pp_parser_text(c, fail);
            pp_parser_text(c, ";\n    "); pp_parser_text(c, cur); pp_parser_text(c, "++;\n");
        }
        break;
    }

    default:
        pp_parser_text(c, "    goto "); pp_parser_text(c, fail); pp_parser_text(c, ";\n");
        break;
    }
}

/* ** Emit one token's baked pure matcher (engine,input,length,start -> matched + end)
** reproducing pp_match_ir for the token's pattern, used by scan_<token> through
** pp_vm_terminal_baked. Only emitted for a bakeable pattern. */
static void pp_parser_emit_token_matcher(pp_parser_emit *ctx, uint32_t symbol)
{
    pp_parser_cursor *c = ctx->cursor;
    const pocketparse_grammar *grammar = ctx->grammar;
    uint32_t token_index = pp_parser_token_index(grammar, symbol);
    const pp_ir *pat;
    pp_parser_lex lex;
    if (token_index == PP_NO_INDEX)
        return;
    pat = grammar->tokens[token_index].pattern;
    if (!pp_parser_can_bake(pat))
        return;

    /* Reserve a contiguous, file-unique class-table id block for this token, so its
    ** tables do not collide with another token's (each matcher restarts its local
    ** count from this base). */
    lex.cursor = c; lex.prefix = ctx->prefix;
    lex.class_id = ctx->class_base; lex.label = 0;
    pp_parser_emit_class_tables(&lex, pat);

    pp_parser_text(c, "/* baked matcher for token `");
    {
        pocketparse_string name = pp_intern_name(&grammar->interner, symbol);
        pp_parser_slice(c, name);
    }
    pp_parser_text(c, "' (pure: input,start -> end). */\n");
    pp_parser_text(c, "static int ");
    pp_parser_token_fn(c, ctx->prefix, grammar, symbol);
    pp_parser_text(c,
        "_match(const pocketparse_engine *engine, const uint8_t *input,\n"
        "       uint32_t length, uint32_t start, uint32_t *out_end)\n"
        "{\n"
        "    uint32_t pos = start;\n"
        "    (void)engine;\n");
    lex.class_id = ctx->class_base; lex.label = 0;
    pp_parser_emit_match(&lex, pat, "pos", "no_match");
    ctx->class_base = lex.class_id; /* advance the file-global counter */
    pp_parser_text(c,
        "    *out_end = pos;\n"
        "    return 1;\n"
        "no_match:\n"
        "    return 0;\n"
        "}\n\n");
}

/* ** Emit one token's scan function: consume leading extras, apply the lexical-goal
** redirect, match the token at the cursor and emit a leaf. A bakeable token routes
** through pp_vm_terminal_baked + its baked pure matcher (the per-token straight-line
** lexer, no token VM); any other token keeps the generic pp_vm_do_match_pattern. Both
** are byte-identical to the VM's terminal match. */
static void pp_parser_emit_token_fn(pp_parser_emit *ctx, uint32_t symbol)
{
    pp_parser_cursor *c = ctx->cursor;
    const pocketparse_grammar *grammar = ctx->grammar;
    uint32_t token_index = pp_parser_token_index(grammar, symbol);
    char patexpr[64];
    int baked;

    if (token_index == PP_NO_INDEX)
        return;
    baked = pp_parser_can_bake(grammar->tokens[token_index].pattern);

    /* The baked pure matcher comes first (scan_ references it). */
    if (baked)
        pp_parser_emit_token_matcher(ctx, symbol);

    pp_parser_text(c, "/* scan the token `");
    {
        pocketparse_string name = pp_intern_name(&grammar->interner, symbol);
        pp_parser_slice(c, name);
    }
    pp_parser_text(c, "' at the cursor and emit its leaf. */\n");
    pp_parser_text(c, "static int ");
    pp_parser_token_fn(c, ctx->prefix, grammar, symbol);
    pp_parser_token_pattern_expr(patexpr, token_index);
    if (baked) {
        pp_parser_text(c,
            "(pp_vm_state *state, pp_vm_sink *sink, uint32_t field, uint32_t alias)\n"
            "{\n"
            "    return pp_vm_terminal_baked(state, ");
        pp_parser_text(c, patexpr);
        pp_parser_text(c, ", ");
        pp_parser_emit_symbol(c, symbol);
        pp_parser_text(c, ", sink, field, alias,\n"
            "                                ");
        pp_parser_token_fn(c, ctx->prefix, grammar, symbol);
        pp_parser_text(c, "_match);\n"
            "}\n\n");
    } else {
        pp_parser_text(c,
            "(pp_vm_state *state, pp_vm_sink *sink, uint32_t field, uint32_t alias)\n"
            "{\n"
            "    return pp_vm_do_match_pattern(state, ");
        pp_parser_text(c, patexpr);
        pp_parser_text(c, ", ");
        pp_parser_emit_symbol(c, symbol);
        pp_parser_text(c, ", sink, field, alias);\n"
            "}\n\n");
    }
}

/* ** Emit one inject node's region-body function. pp_vm_do_inject calls it (through
** the body callback) to match the region that delimits the embedded span; on a match
** the runtime drops the raw region nodes and splices the embedded parse. `body_user'
** is the inject IR node, so the region is ((pp_ir *)body_user)->children[0]; the
** region's structure is translated inline here (walked at emit time off the captured
** region IR). It runs on a nested call, bounded by injection nesting, not input
** depth. The depth guard wraps the region match so a pathological region cannot
** overflow the C stack either. */
static void pp_parser_emit_inject_region_fn(pp_parser_emit *ctx, uint32_t which)
{
    pp_parser_cursor *c = ctx->cursor;
    const pp_ir *inject = ctx->inject_nodes[which];
    const pp_ir *region = inject->child_count >= 1 ? inject->children[0] : 0;
    pp_parser_text(c,
        "/* Match the region delimiting an embedded span (inject region function ");
    pp_parser_u32(c, which);
    pp_parser_text(c, "). */\n"
        "static int ");
    pp_parser_name(c, ctx->prefix, "pp_parser_inject_region", "inject_region");
    pp_parser_byte(c, '_');
    pp_parser_u32(c, which);
    pp_parser_text(c,
        "(pp_vm_state *state, pp_vm_sink *children,\n"
        "                                   uint32_t field, uint32_t alias, void *body_user)\n"
        "{\n"
        "    const pp_ir *region = ((pp_ir *)body_user)->child_count >= 1\n"
        "        ? ((pp_ir *)body_user)->children[0] : 0;\n"
        "    int sig = PP_VM_FAIL;\n"
        "    (void)field; (void)alias; (void)region;\n");
    if (region != 0) {
        pp_parser_text(c,
        "    if (state->depth >= state->depth_limit) {\n"
        "        sig = PP_VM_FAIL;\n"
        "    } else {\n"
        "        state->depth++;\n"
        "        {\n");
        pp_parser_emit_ir(ctx, "region", region, "children",
                          "PP_NO_SYMBOL", "PP_NO_SYMBOL", "            ");
        pp_parser_text(c,
        "        }\n"
        "        state->depth--;\n"
        "    }\n");
    }
    pp_parser_text(c,
        "    return sig;\n"
        "rule_oom:\n"
        "    return PP_VM_OOM;\n"
        "}\n\n");
}

/* ** Walk a rule body and register every inject node it contains, in deterministic
** (pre-order) order, so each gets a stable region-function index BEFORE any inject
** site looks it up. Recurses into children. */
static void pp_parser_register_injects_ir(pp_parser_emit *ctx, const pp_ir *ir)
{
    uint32_t i;
    if (ir == 0)
        return;
    if (ir->op == PP_OP_INJECT)
        pp_parser_register_inject(ctx, ir);
    for (i = 0; i < ir->child_count; i++)
        pp_parser_register_injects_ir(ctx, ir->children[i]);
}

/* ** Walk a rule body and register every bare PP_OP_LITERAL it contains (deduped by
** bytes), so each distinct literal text gets a stable litmatch-function index BEFORE
** any literal site looks it up. The walk order mirrors the emit order (pre-order over
** the same bodies in the same sequence), so the lazy registration during emission
** returns the index the pre-pass assigned. Tokens are NOT walked: a token's pattern is
** matched by its own scan_<token>, not as an inline literal. */
static void pp_parser_register_literals_ir(pp_parser_emit *ctx, const pp_ir *ir)
{
    uint32_t i;
    if (ir == 0)
        return;
    if (ir->op == PP_OP_LITERAL) {
        pp_parser_register_literal(ctx, ir);
        return;
    }
    for (i = 0; i < ir->child_count; i++)
        pp_parser_register_literals_ir(ctx, ir->children[i]);
}

/* ** Emit one baked inline-literal matcher (<prefix>_litmatch_<n>): a PURE function
** (input,start -> matched + end) of straight-line byte compares reproducing
** pp_match_ir for a literal, used by pp_vm_terminal_baked at a bare-literal site. */
static void pp_parser_emit_litmatch_fn(pp_parser_emit *ctx, uint32_t which)
{
    pp_parser_cursor *c = ctx->cursor;
    const pp_ir *lit = ctx->literal_nodes[which];
    pp_parser_lex lex;
    lex.cursor = c; lex.prefix = ctx->prefix; lex.class_id = 0; lex.label = 0;
    pp_parser_text(c, "/* baked matcher for inline literal \"");
    {
        uint32_t i;
        for (i = 0; i < lit->text.length; i++) {
            char ch = (char)lit->text.pointer[i];
            pp_parser_byte(c, (ch == '\n' || ch == '\r' || ch == '*') ? ' ' : ch);
        }
    }
    pp_parser_text(c, "\" (pure: input,start -> end). */\n"
        "static int ");
    pp_parser_name(c, ctx->prefix, "pp_parser_litmatch", "litmatch");
    pp_parser_byte(c, '_');
    pp_parser_u32(c, which);
    pp_parser_text(c,
        "(const pocketparse_engine *engine, const uint8_t *input,\n"
        "       uint32_t length, uint32_t start, uint32_t *out_end)\n"
        "{\n"
        "    uint32_t pos = start;\n"
        "    (void)engine;\n");
    pp_parser_emit_match(&lex, lit, "pos", "no_match");
    pp_parser_text(c,
        "    *out_end = pos;\n"
        "    return 1;\n"
        "no_match:\n"
        "    return 0;\n"
        "}\n\n");
}

/* ** Emit the baked extras scanner (<prefix>_extras_match): a PURE function that, for
** the cursor position, returns the FIRST extras-table entry (in order) whose baked
** matcher matches with a non-empty span, and its end. pp_vm_consume_extras calls it
** in place of the generic token-VM loop, so whitespace/comment scanning is the
** straight-line C a hand lexer is -- the last large token-VM slice the profile named.
** Emitted only when EVERY extras token is bakeable; then ctx->extras_baked is set so
** the driver wires state->extras_match. The matchers for extras tokens not already
** emitted as reachable scan_ tokens are emitted here first. */
static void pp_parser_emit_extras_scanner(pp_parser_emit *ctx,
                                          const pp_parser_reach *reach)
{
    pp_parser_cursor *c = ctx->cursor;
    const pocketparse_grammar *grammar = ctx->grammar;
    uint32_t e;

    ctx->extras_baked = 0;
    if (grammar->extra_count == 0)
        return;
    /* Every extras token must be bakeable, else keep the generic loop. */
    for (e = 0; e < grammar->extra_count; e++) {
        uint32_t ti = pp_parser_token_index(grammar, grammar->extras[e].token_symbol);
        if (ti == PP_NO_INDEX || !pp_parser_can_bake(grammar->tokens[ti].pattern))
            return;
    }
    /* Emit a baked matcher for each extras token not already emitted as a reachable
    ** token (a reachable token already has its <fn>_match). */
    for (e = 0; e < grammar->extra_count; e++) {
        uint32_t sym = grammar->extras[e].token_symbol;
        uint32_t e2;
        int dup = 0;
        if (pp_parser_reach_has(reach->token_symbols, reach->token_count, sym))
            continue; /* already emitted as a reachable scan_ token's _match */
        for (e2 = 0; e2 < e; e2++)
            if (grammar->extras[e2].token_symbol == sym) { dup = 1; break; }
        if (dup)
            continue;
        pp_parser_emit_token_matcher(ctx, sym);
    }
    /* The scanner: try each extras token's baked matcher in table order. */
    pp_parser_text(c,
        "/* Baked extras scanner: the first extras-table entry whose baked matcher\n"
        "** matches a non-empty span at `start', and its end. Replaces the token VM in\n"
        "** pp_vm_consume_extras for whitespace/comment scanning. */\n"
        "static int ");
    pp_parser_name(c, ctx->prefix, "pp_parser_extras_match", "extras_match");
    pp_parser_text(c,
        "(const pocketparse_engine *engine, const uint8_t *input,\n"
        "                                  uint32_t length, uint32_t start,\n"
        "                                  uint32_t *out_extra, uint32_t *out_end)\n"
        "{\n"
        "    uint32_t end;\n");
    for (e = 0; e < grammar->extra_count; e++) {
        uint32_t sym = grammar->extras[e].token_symbol;
        pp_parser_text(c, "    if (");
        pp_parser_token_fn(c, ctx->prefix, grammar, sym);
        pp_parser_text(c, "_match(engine, input, length, start, &end) && end > start) {\n");
        pp_parser_text(c, "        *out_extra = "); pp_parser_u32(c, e);
        pp_parser_text(c, "u; *out_end = end; return 1;\n");
        pp_parser_text(c, "    }\n");
    }
    pp_parser_text(c,
        "    return 0;\n"
        "}\n\n");
    ctx->extras_baked = 1;
}

/* ** Emit the whole IR-walking body: the inject region functions, the scan_
** functions (one per reachable token), then the parse_ functions (one per reachable
** rule), with forward declarations first so mutual recursion compiles in one pass.
** Returns the reachable-symbol sets through `out_reach' so the driver, which walks
** the start body inline, registers the same inject indices the pre-pass assigned. */
static void pp_parser_emit_body(pp_parser_emit *ctx, uint32_t start_symbol,
                                pp_parser_reach *out_reach)
{
    pp_parser_cursor *c = ctx->cursor;
    const pocketparse_grammar *grammar = ctx->grammar;
    pp_parser_reach reach;
    uint32_t cap = grammar->rule_count + grammar->token_count + 1;
    uint32_t i;
    pp_arena *arena = grammar->arena;

    reach.rule_symbols  = (uint32_t *)pp_alloc(arena, (cap == 0 ? 1 : cap) * sizeof(uint32_t));
    reach.token_symbols = (uint32_t *)pp_alloc(arena, (cap == 0 ? 1 : cap) * sizeof(uint32_t));
    reach.rule_count    = 0;
    reach.token_count   = 0;
    reach.grammar       = grammar;
    if (out_reach != 0) { out_reach->rule_count = 0; out_reach->token_count = 0; }
    if (reach.rule_symbols == 0 || reach.token_symbols == 0)
        return; /* OOM: pp_emit_parser checks the buffer below and bails */

    /* The start rule is the root of reachability. */
    if (pp_parser_is_rule(grammar, start_symbol))
        pp_parser_reach_rule(&reach, start_symbol);
    if (out_reach != 0)
        *out_reach = reach;

    /* Pre-pass: register every inject node, in the deterministic order the rule
    ** functions (and then the driver's start body) will emit them, so each inject
    ** site finds the region-function index already assigned. The start body is walked
    ** first (the driver emits it inline) followed by each reachable rule's body. */
    if (pp_parser_is_rule(grammar, start_symbol)) {
        uint32_t si = pp_parser_rule_index(grammar, start_symbol);
        if (si != PP_NO_INDEX)
            pp_parser_register_injects_ir(ctx, grammar->rules[si].body);
    }
    for (i = 0; i < reach.rule_count; i++) {
        uint32_t ri = pp_parser_rule_index(grammar, reach.rule_symbols[i]);
        if (ri != PP_NO_INDEX && reach.rule_symbols[i] != start_symbol)
            pp_parser_register_injects_ir(ctx, grammar->rules[ri].body);
    }

    /* Pre-pass: register every distinct inline literal (deduped by bytes) across all
    ** reachable rule bodies, so each literal site can look up its baked matcher index
    ** during emission (dedup makes the order immaterial -- every literal is registered
    ** here before any emission, so the lazy register at a site only ever finds an
    ** existing index). The start body's literals are the start rule's, so walking every
    ** reachable rule covers the driver's inline start body too. */
    for (i = 0; i < reach.rule_count; i++) {
        uint32_t ri = pp_parser_rule_index(grammar, reach.rule_symbols[i]);
        if (ri != PP_NO_INDEX)
            pp_parser_register_literals_ir(ctx, grammar->rules[ri].body);
    }
    /* Inject region bodies are emitted too (pp_parser_emit_inject_region_fn), so their
    ** inline literals need a registered matcher as well. */
    for (i = 0; i < ctx->inject_count; i++) {
        const pp_ir *inj = ctx->inject_nodes[i];
        if (inj != 0 && inj->child_count >= 1)
            pp_parser_register_literals_ir(ctx, inj->children[0]);
    }

    /* Forward declarations: every reachable parse_/scan_ function, so a REF can call
    ** a callee defined later and mutual recursion compiles in one pass. */
    pp_parser_text(c,
        "/* Forward declarations: one parse_ function per reachable rule, one scan_\n"
        "** function per reachable token. A REF is a direct call to one of these. */\n");
    for (i = 0; i < ctx->literal_count; i++) {
        pp_parser_text(c, "static int ");
        pp_parser_name(c, ctx->prefix, "pp_parser_litmatch", "litmatch");
        pp_parser_byte(c, '_');
        pp_parser_u32(c, i);
        pp_parser_text(c,
            "(const pocketparse_engine *engine, const uint8_t *input,\n"
            "       uint32_t length, uint32_t start, uint32_t *out_end);\n");
    }
    for (i = 0; i < ctx->inject_count; i++) {
        pp_parser_text(c, "static int ");
        pp_parser_name(c, ctx->prefix, "pp_parser_inject_region", "inject_region");
        pp_parser_byte(c, '_');
        pp_parser_u32(c, i);
        pp_parser_text(c,
            "(pp_vm_state *state, pp_vm_sink *children,\n"
            "                                   uint32_t field, uint32_t alias, void *body_user);\n");
    }
    for (i = 0; i < reach.token_count; i++) {
        pp_parser_text(c, "static int ");
        pp_parser_token_fn(c, ctx->prefix, grammar, reach.token_symbols[i]);
        pp_parser_text(c,
            "(pp_vm_state *state, pp_vm_sink *sink, uint32_t field, uint32_t alias);\n");
    }
    for (i = 0; i < reach.rule_count; i++) {
        pp_parser_text(c, "static int ");
        pp_parser_rule_fn(c, ctx->prefix, grammar, reach.rule_symbols[i]);
        pp_parser_text(c,
            "(pp_vm_state *state, pp_vm_sink *sink, uint32_t field, uint32_t alias);\n");
    }
    pp_parser_text(c, "\n");

    /* One baked matcher per distinct inline literal (leaf: no dependencies). */
    for (i = 0; i < ctx->literal_count; i++)
        pp_parser_emit_litmatch_fn(ctx, i);

    /* One region function per registered inject node. */
    for (i = 0; i < ctx->inject_count; i++)
        pp_parser_emit_inject_region_fn(ctx, i);

    /* One scan_ function per reachable token. */
    for (i = 0; i < reach.token_count; i++)
        pp_parser_emit_token_fn(ctx, reach.token_symbols[i]);

    /* One parse_ function per reachable rule. */
    for (i = 0; i < reach.rule_count; i++)
        pp_parser_emit_rule_fn(ctx, reach.rule_symbols[i]);

    /* The baked extras scanner, if every extras token is bakeable. */
    pp_parser_emit_extras_scanner(ctx, &reach);
}

/* ** Emit the outer driver: locate the start rule, init the shared state, set the
** measured depth limit, push the Source node, run the start rule's body through the
** generated parse_<start> (its CALL prologue opens a SECOND node of the start kind,
** matching the VM, whose start program is the start rule's BODY -- so here we open
** the root and then call parse_<start>, which opens the start rule's own node as the
** root's single child... no: the VM runs the start BODY directly into the root.
**
** To stay byte-identical we therefore DO NOT call parse_<start> (which would open an
** extra node); instead we translate the start rule's BODY inline here, into the
** root's child sink -- exactly as pp_vm_run is handed program->start_program (the
** body) and runs it into the root pushed by pp_execute_vm. */
static void pp_parser_emit_driver(pp_parser_emit *ctx, uint32_t start_symbol)
{
    pp_parser_cursor *c = ctx->cursor;
    const pocketparse_grammar *grammar = ctx->grammar;
    uint32_t rule_index = pp_parser_rule_index(grammar, start_symbol);
    char bodyexpr[64];

    pp_parser_text(c, "static pocketparse_result ");
    pp_parser_name(c, ctx->prefix, "pp_execute_parser", "execute");
    pp_parser_text(c,
        "(pocketparse_parser *parser)\n"
        "{\n"
        "    pp_vm_state state_storage;\n"
        "    pp_vm_state *state = &state_storage;\n"
        "    pp_program *program;\n"
        "    const pp_rule_def *start = 0;\n"
        "    pp_vm_sink children;\n"
        "    uint32_t root;\n"
        "    uint32_t i;\n"
        "    int sig = PP_VM_FAIL;\n"
        "    for (i = 0; i < parser->grammar->rule_count; i++)\n"
        "        if (parser->grammar->rules[i].symbol == parser->start_symbol) {\n"
        "            start = &parser->grammar->rules[i];\n"
        "            break;\n"
        "        }\n"
        "    if (start == 0) {\n"
        "        pp_diag_error(&parser->diagnostics, \"parse.no-start\",\n"
        "                      \"error: start rule not found\", 0, 0, \"missing start rule\");\n"
        "        return POCKETPARSE_SYNTAX_ERROR;\n"
        "    }\n"
        "    /* The shared runtime still needs a lowered program for pp_vm_state_init\n"
        "    ** (the token index / extras lead set are built from the grammar, and the\n"
        "    ** state carries a program pointer). The generated functions never\n"
        "    ** read program->instructions[N]; the program is just the init vehicle, so\n"
        "    ** the cached lowering is reused if present and built once otherwise. */\n"
        "    if (parser->grammar->vm_program != 0 &&\n"
        "        parser->grammar->vm_program_start == parser->start_symbol) {\n"
        "        program = parser->grammar->vm_program;\n"
        "    } else {\n"
        "        program = pp_compile_program(parser->grammar->arena, parser->grammar,\n"
        "                                     parser->start_symbol);\n"
        "        if (program == 0) return POCKETPARSE_OUT_OF_MEMORY;\n"
        "        parser->grammar->vm_program = program;\n"
        "        parser->grammar->vm_program_start = parser->start_symbol;\n"
        "    }\n"
        "    pp_vm_state_init(state, parser, program);\n"
        "    state->depth       = 0;\n"
        "    state->depth_limit = ");
    pp_parser_u32(c, PP_PARSER_DEPTH_LIMIT);
    pp_parser_text(c, "u;\n");
    pp_parser_text(c,
        "    /* This is a hand-written recursive-descent parser: it carries NO packrat\n"
        "    ** memo. Pin the memo OFF so even a delegated CALL lifecycle (recovery /\n"
        "    ** no_extras) never wakes it -- the std grammars are left-factored to linear,\n"
        "    ** so no backtracking backstop is needed; the VM keeps the memo for dynamic\n"
        "    ** grammars. (memo is a pure cache, so the tree is byte-identical.) */\n"
        "    state->memo_threshold = 0xFFFFFFFFu;\n"
        "    state->memo_active    = 0;\n");
    if (ctx->extras_baked) {
        pp_parser_text(c,
        "    /* Bake the extras (whitespace/comment) scan: pp_vm_consume_extras uses\n"
        "    ** this straight-line scanner instead of the generic token VM. */\n"
        "    state->extras_match = ");
        pp_parser_name(c, ctx->prefix, "pp_parser_extras_match", "extras_match");
        pp_parser_text(c, ";\n");
    }
    pp_parser_text(c,
        "    state->children_inline = 1;\n"
        "    children.items = 0; children.count = 0; children.capacity = 0;\n"
        "    children.parent = PP_NO_INDEX;\n"
        "    root = pp_tree_builder_push(&parser->builder, start->symbol, PP_NO_SYMBOL,\n"
        "                                0, parser->input_length, PP_NO_INDEX);\n"
        "    if (root == PP_NO_INDEX) return POCKETPARSE_OUT_OF_MEMORY;\n");
    /* Translate the start rule's body directly into the root's child sink, exactly as
    ** pp_execute_vm hands pp_vm_run the start BODY (program->start_program). */
    if (rule_index != PP_NO_INDEX) {
        pp_parser_text(c, "    {\n");
        pp_parser_rule_body_expr(bodyexpr, rule_index);
        pp_parser_emit_ir(ctx, bodyexpr, grammar->rules[rule_index].body,
                          "&children", "PP_NO_SYMBOL", "PP_NO_SYMBOL", "        ");
        pp_parser_text(c, "    }\n");
    } else {
        pp_parser_text(c, "    sig = PP_VM_FAIL;\n");
    }
    pp_parser_text(c,
        "    if (sig == PP_VM_OOM) return POCKETPARSE_OUT_OF_MEMORY;\n"
        "    if (sig != PP_VM_MATCH_OK) {\n"
        "        pp_diag_error(&parser->diagnostics, \"parse.syntax\",\n"
        "                      \"error: unexpected input\", state->position,\n"
        "                      state->position < parser->input_length\n"
        "                          ? state->position + 1 : state->position,\n"
        "                      \"parse failed here\");\n"
        "        pp_vm_link_children(&parser->builder, root, &children);\n"
        "        pp_vm_freeze_tree(parser, root);\n"
        "        return POCKETPARSE_SYNTAX_ERROR;\n"
        "    }\n"
        "    pp_vm_consume_extras(state, &children);\n"
        "    pp_vm_link_children(&parser->builder, root, &children);\n"
        "    parser->builder.nodes[root].end_byte = parser->input_length;\n"
        "    if (state->position < parser->input_length) {\n"
        "        pp_diag_error(&parser->diagnostics, \"parse.trailing\",\n"
        "                      \"error: unexpected trailing input\", state->position,\n"
        "                      state->position + 1, \"unexpected here\");\n"
        "        pp_vm_freeze_tree(parser, root);\n"
        "        return POCKETPARSE_SYNTAX_ERROR;\n"
        "    }\n"
        "    return pp_vm_freeze_tree(parser, root);\n"
        "rule_oom:\n"
        "    return POCKETPARSE_OUT_OF_MEMORY;\n"
        "}\n\n");
    (void)c;
}

/* ** Whether the start rule has a SHALLOW top-level-item loop the buffer-friendly
** path can suspend at (W17-4). The native generated parser cannot suspend
** mid-stack, so it can only suspend where the C stack is empty: the start rule's
** repeat of top-level items. The recognised shapes are a bare `(repeat ITEM)' as
** the whole start body, or `(seq (repeat ITEM))' / `(seq (repeat ITEM) EOF)' -- the
** common "a document is a sequence of statements/values" form (sexpr's
** `(seq (repeat Form) EOF)', std.json's value sequence, the import/words example).
** On a match it writes the repeat node, its item child, the repeat's min bound, and
** whether a trailing EOF follows, and returns 1; otherwise returns 0 and the
** grammar gets no run_chunk entry (it is parsed whole, as before). */
static int pp_parser_top_level_loop(const pp_ir *body, const pp_ir **out_repeat,
                                    const pp_ir **out_item, uint32_t *out_min,
                                    int *out_has_eof)
{
    const pp_ir *repeat = 0;
    int          has_eof = 0;

    if (body == 0)
        return 0;
    if (body->op == PP_OP_REPEAT) {
        repeat = body;
    } else if (body->op == PP_OP_SEQ && body->child_count >= 1 &&
               body->children[0] != 0 &&
               body->children[0]->op == PP_OP_REPEAT) {
        uint32_t k;
        repeat = body->children[0];
        /* Every child after the leading repeat must be EOF -- anything else means the
        ** start body is not a plain item sequence and the boundary is not shallow. */
        for (k = 1; k < body->child_count; k++) {
            if (body->children[k] == 0 || body->children[k]->op != PP_OP_EOF)
                return 0;
            has_eof = 1;
        }
    } else {
        return 0;
    }
    if (repeat->child_count < 1 || repeat->children[0] == 0)
        return 0;
    /* A bounded repeat (max != 0) is a fixed-count head, not an open item stream;
    ** keep it on the whole-input path. */
    if (repeat->max != 0)
        return 0;
    *out_repeat   = repeat;
    *out_item     = repeat->children[0];
    *out_min      = repeat->min;
    *out_has_eof  = has_eof;
    return 1;
}

/* ** Emit the per-item parse function for the buffer-friendly loop: it runs the
** start repeat's child IR ONCE into the caller's sink, returning the match signal.
** It is the exact body of one iteration of the whole-input driver's inline REPEAT,
** so an item parsed here is byte-identical to an item parsed whole. The runtime
** loop (pp_parser_chunk_loop) owns the mark save/restore, the zero-width guard, and
** the boundary check; this function is purely "parse one item". */
static void pp_parser_emit_top_item_fn(pp_parser_emit *ctx, uint32_t start_symbol,
                                       const pp_ir *repeat, const pp_ir *item)
{
    pp_parser_cursor *c = ctx->cursor;
    uint32_t rule_index = pp_parser_rule_index(ctx->grammar, start_symbol);
    char bodyexpr[64];
    char repeatexpr[96];
    char itemexpr[128];

    pp_parser_text(c,
        "/* Parse ONE top-level item (the start repeat's child) into `sink'. The\n"
        "** buffer-friendly loop pp_parser_chunk_loop calls this once per item. */\n"
        "static int ");
    pp_parser_name(c, ctx->prefix, "pp_parser_top_item", "top_item");
    pp_parser_text(c,
        "(pp_vm_state *state, pp_vm_sink *sink, uint32_t field, uint32_t alias)\n"
        "{\n"
        "    int sig = PP_VM_FAIL;\n"
        "    (void)field; (void)alias;\n"
        "    {\n");
    /* Build the run-time IR expression for the item: start body -> [seq child 0 ->]
    ** repeat -> child 0. The whole-input driver navigates the same nodes. */
    pp_parser_rule_body_expr(bodyexpr, rule_index);
    if (ctx->grammar->rules[rule_index].body == repeat) {
        /* Bare repeat: the body IS the repeat. */
        pp_parser_child_expr(itemexpr, bodyexpr, 0);
    } else {
        /* (seq (repeat ITEM) ...): body->children[0] is the repeat. */
        pp_parser_child_expr(repeatexpr, bodyexpr, 0);
        pp_parser_child_expr(itemexpr, repeatexpr, 0);
    }
    pp_parser_emit_ir(ctx, itemexpr, item, "sink",
                      "PP_NO_SYMBOL", "PP_NO_SYMBOL", "        ");
    pp_parser_text(c,
        "    }\n"
        "    return sig;\n"
        "rule_oom:\n"
        "    return PP_VM_OOM;\n"
        "}\n\n");
}

/* ** Emit the buffer-friendly driver: it sets up the shared state exactly like the
** whole-input driver (lower the program for pp_vm_state_init, set the measured depth
** limit, wire the baked extras scanner, enable inline child storage) and then hands
** the resumable top-level loop to the shared runtime pp_parser_chunk_loop with the
** generated per-item function. A host calls it repeatedly: while more input may
** arrive it returns POCKETPARSE_MORE at each item boundary; on the final chunk
** (parser->input_complete set) it finalises and freezes the byte-identical tree. */
static void pp_parser_emit_chunk_driver(pp_parser_emit *ctx, int has_eof,
                                        uint32_t min)
{
    pp_parser_cursor *c = ctx->cursor;

    pp_parser_text(c, "static pocketparse_result ");
    pp_parser_name(c, ctx->prefix, "pp_execute_parser_chunk", "execute_chunk");
    pp_parser_text(c,
        "(pocketparse_parser *parser)\n"
        "{\n"
        "    pp_vm_state state_storage;\n"
        "    pp_vm_state *state = &state_storage;\n"
        "    pp_program *program;\n"
        "    if (parser->grammar->vm_program != 0 &&\n"
        "        parser->grammar->vm_program_start == parser->start_symbol) {\n"
        "        program = parser->grammar->vm_program;\n"
        "    } else {\n"
        "        program = pp_compile_program(parser->grammar->arena, parser->grammar,\n"
        "                                     parser->start_symbol);\n"
        "        if (program == 0) return POCKETPARSE_OUT_OF_MEMORY;\n"
        "        parser->grammar->vm_program = program;\n"
        "        parser->grammar->vm_program_start = parser->start_symbol;\n"
        "    }\n"
        "    pp_vm_state_init(state, parser, program);\n"
        "    state->depth       = 0;\n"
        "    state->depth_limit = ");
    pp_parser_u32(c, PP_PARSER_DEPTH_LIMIT);
    pp_parser_text(c, "u;\n");
    pp_parser_text(c,
        "    /* Hand-written RD parser: no packrat memo (see <prefix>_run). */\n"
        "    state->memo_threshold = 0xFFFFFFFFu;\n"
        "    state->memo_active    = 0;\n");
    if (ctx->extras_baked) {
        pp_parser_text(c, "    state->extras_match = ");
        pp_parser_name(c, ctx->prefix, "pp_parser_extras_match", "extras_match");
        pp_parser_text(c, ";\n");
    }
    pp_parser_text(c,
        "    state->children_inline = 1;\n"
        "    return pp_parser_chunk_loop(state, ");
    pp_parser_name(c, ctx->prefix, "pp_parser_top_item", "top_item");
    pp_parser_text(c, ", ");
    pp_parser_u32(c, (uint32_t)(has_eof ? 1 : 0));
    pp_parser_text(c, ", ");
    pp_parser_u32(c, min);
    pp_parser_text(c, "u);\n"
        "}\n\n");
}

/* ** Emit the library tail: the callable entries a host links and drives
** in-process, in place of the standalone main(). Always the <prefix>_load and
** <prefix>_run (whole-input) entries; when `has_chunk' the grammar has a shallow
** top-level-item loop, so the buffer-friendly <prefix>_run_chunk is emitted too. */
static void pp_parser_emit_lib_entries(pp_parser_cursor *cursor, const char *prefix,
                                       int has_chunk)
{
    pp_parser_text(cursor,
        "/* Load the embedded grammar into `engine'. The generated functions navigate\n"
        "** the IR of THIS grammar by index, so a host must parse with the grammar this\n"
        "** returns (not a separately loaded copy). */\n"
        "pocketparse_result ");
    pp_parser_name(cursor, prefix, "pp_compiled_parser_load", "load");
    pp_parser_text(cursor,
        "(pocketparse_engine *engine,\n"
        "                                           pocketparse_grammar **out_grammar)\n"
        "{\n"
        "    pocketparse_source source;\n"
        "    if (engine == 0 || out_grammar == 0)\n"
        "        return POCKETPARSE_INVALID;\n"
        "    source.name.pointer = ");
    pp_parser_name(cursor, prefix, "pp_parser_grammar_bytes", "grammar_bytes");
    pp_parser_text(cursor, "; source.name.length = 0;\n"
        "    source.bytes.pointer = ");
    pp_parser_name(cursor, prefix, "pp_parser_grammar_bytes", "grammar_bytes");
    pp_parser_text(cursor, ";\n"
        "    source.bytes.length = (uint32_t)sizeof ");
    pp_parser_name(cursor, prefix, "pp_parser_grammar_bytes", "grammar_bytes");
    pp_parser_text(cursor, ";\n"
        "    return pocketparse_load_binary_grammar(engine, source, out_grammar);\n"
        "}\n\n");

    pp_parser_text(cursor,
        "/* Run the compiled parser over a parser whose input has already been pushed.\n"
        "** Takes the parser lifecycle's finishing transition (pp_parser_run) with the\n"
        "** generated recursive descent as the executor. A push that failed (a dropped\n"
        "** chunk) is surfaced here rather than letting the run produce a successful-\n"
        "** looking tree over a truncated buffer. */\n"
        "pocketparse_result ");
    pp_parser_name(cursor, prefix, "pp_compiled_parser_run", "run");
    pp_parser_text(cursor,
        "(pocketparse_parser *parser)\n"
        "{\n"
        "    if (parser == 0)\n"
        "        return POCKETPARSE_INVALID;\n"
        "    return pp_parser_run(parser, ");
    pp_parser_name(cursor, prefix, "pp_execute_parser", "execute");
    pp_parser_text(cursor, ");\n"
        "}\n");

    if (has_chunk) {
        pp_parser_text(cursor,
            "\n/* Buffer-friendly run (W17-4): drive the parser over a GROWING buffer.\n"
            "** Call it after pushing the bytes available so far, with\n"
            "** parser->input_complete set to 0 while more may still arrive and 1 on the\n"
            "** final chunk. It parses every top-level item that completes within the\n"
            "** buffer; when the cursor reaches the buffer end with input still\n"
            "** incomplete it returns POCKETPARSE_MORE without freezing -- append more\n"
            "** bytes and call again, and the parse RESUMES at the next item. On the\n"
            "** final chunk it finalises and the tree is byte-identical to one whole run.\n"
            "** Native recursion cannot suspend mid-stack, so the boundary is the shallow\n"
            "** top-level-item loop only; arbitrary mid-item async push stays the VM's job. */\n"
            "pocketparse_result ");
        pp_parser_name(cursor, prefix, "pp_compiled_parser_run_chunk", "run_chunk");
        pp_parser_text(cursor,
            "(pocketparse_parser *parser)\n"
            "{\n"
            "    if (parser == 0)\n"
            "        return POCKETPARSE_INVALID;\n"
            "    return ");
        pp_parser_name(cursor, prefix, "pp_execute_parser_chunk", "execute_chunk");
        pp_parser_text(cursor, "(parser);\n"
            "}\n");
    }
}

/* ** Emit the whole compiled-parser file: banner, includes, embedded packed
** grammar + accessor, the IR-walking body (forward declarations, inject helper,
** scan_/parse_ functions), the outer driver, and -- depending on `lib' -- either a
** standalone main() that loads the grammar, parses stdin and dumps the CST (byte-
** identical to the VM's --dump-tree), or the two callable library entries. */
static void pp_parser_emit_all(pp_parser_cursor *cursor, const char *prefix,
                           const pocketparse_grammar *grammar,
                           pocketparse_string source_name,
                           pocketparse_string start_name,
                           pocketparse_string packed,
                           uint32_t start_symbol,
                           int lib)
{
    pp_parser_emit ctx;
    pp_parser_reach reach;
    ctx.cursor          = cursor;
    ctx.prefix          = prefix;
    ctx.grammar         = grammar;
    ctx.label           = 0;
    ctx.inject_nodes    = 0;
    ctx.inject_count    = 0;
    ctx.inject_capacity = 0;
    ctx.literal_nodes    = 0;
    ctx.literal_count    = 0;
    ctx.literal_capacity = 0;
    ctx.class_base      = 0;
    ctx.extras_baked    = 0;
    ctx.chunk_loop      = 0;
    ctx.chunk_has_eof   = 0;
    ctx.chunk_min       = 0;
    {
        uint32_t ri;
        ctx.uses_slots = 0;
        for (ri = 0; ri < grammar->rule_count; ri++)
            if (grammar->rules[ri].slot_count > 0) { ctx.uses_slots = 1; break; }
    }

    pp_parser_text(cursor,
        "/*\n"
        "** Generated by pocketparse c --parser -- do not edit.\n"
        "**\n"
        "** A compiled parser generated FROM THE IR BY NAME (W17-2): one C function per\n"
        "** grammar rule (parse_<rule>) and per token (scan_<token>); a rule reference\n"
        "** is a direct named call; SEQ/CHOICE/REPEAT/OPTIONAL/FIELD/ALIAS/GOAL/RECOVER/\n"
        "** INJECT are idiomatic C control flow with the FIRST sets baked as byte tests.\n"
        "** It builds the same arena CST the pocketparse bytecode VM builds (byte-\n"
        "** identical) by calling the same shared runtime primitives, and is crash-safe\n"
        "** on adversarial deep input via a measured recursion-depth guard. Build it\n"
        "** with the pocketparse library:\n"
        "**\n");
    if (lib) {
        pp_parser_text(cursor,
        "**   cc -std=c11 -O2 -c this.c   (then link with libpocketparse)\n"
        "**\n"
        "** This is the LIBRARY shape: no main(), no stdin/dump. It exposes two\n"
        "** entries -- ");
        pp_parser_name(cursor, prefix, "pp_compiled_parser_load", "load");
        pp_parser_text(cursor,
        "(engine, &grammar) loads the embedded\n"
        "** grammar; ");
        pp_parser_name(cursor, prefix, "pp_compiled_parser_run", "run");
        pp_parser_text(cursor,
        "(parser) runs the generated recursive descent.\n"
        "**\n");
    }
    else
        pp_parser_text(cursor,
        "**   cc -std=c11 -O2 this.c -lpocketparse -lax -o parser\n"
        "**\n"
        "** then feed input on stdin; it prints a pre-order dump of the CST.\n"
        "**\n");
    pp_parser_text(cursor,
        "** Source grammar: ");
    pp_parser_slice(cursor, source_name);
    pp_parser_text(cursor, "\n** Start rule:     ");
    pp_parser_slice(cursor, start_name);
    pp_parser_text(cursor, "\n*/\n");

    pp_parser_text(cursor,
        "#include \"pocketparse.h\"\n"
        "#include \"internal.h\"\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n\n");

    /* Embedded packed grammar + accessor (same as the VM module mode). */
    pp_parser_text(cursor,
        "/* The packed grammar image, loaded at startup. The generated functions\n"
        "** navigate its rule/token IR tables by index (which round-trip in order). */\n"
        "static const unsigned char ");
    pp_parser_name(cursor, prefix, "pp_parser_grammar_bytes", "grammar_bytes");
    pp_parser_text(cursor, "[] = {");
    pp_parser_byte_array(cursor, packed);
    pp_parser_text(cursor, "};\n\n");

    /* The IR-walking body, then the outer driver. The body's pre-pass registers the
    ** inject region indices; the driver walks the start body inline and looks them up. */
    pp_parser_emit_body(&ctx, start_symbol, &reach);
    pp_parser_emit_driver(&ctx, start_symbol);
    (void)reach;

    /* W17-4: when the start rule is a shallow top-level-item loop, emit the
    ** buffer-friendly per-item function + chunk driver so the parser can be driven
    ** over a growing buffer. The whole-input driver above is untouched (byte-
    ** identical), so this is purely additive. */
    {
        uint32_t start_index = pp_parser_rule_index(grammar, start_symbol);
        const pp_ir *repeat = 0, *item = 0;
        uint32_t loop_min = 0;
        int loop_has_eof = 0;
        if (start_index != PP_NO_INDEX &&
            pp_parser_top_level_loop(grammar->rules[start_index].body,
                                     &repeat, &item, &loop_min, &loop_has_eof)) {
            pp_parser_emit_top_item_fn(&ctx, start_symbol, repeat, item);
            pp_parser_emit_chunk_driver(&ctx, loop_has_eof, loop_min);
            ctx.chunk_loop = 1;
            ctx.chunk_has_eof = loop_has_eof;
            ctx.chunk_min = loop_min;
        }
    }

    if (lib) {
        pp_parser_emit_lib_entries(cursor, prefix, ctx.chunk_loop);
        return;
    }

    /* main(): open engine, load grammar, parse stdin via pp_execute_parser, dump. */
    pp_parser_text(cursor,
        "/* A generous parse arena. It is a BSS array, so the pages it does not touch\n"
        "** cost nothing (lazy-allocated by the OS); a deeply nested input grows the\n"
        "** tree into it, while the C stack is bounded by the depth guard. */\n"
        "static unsigned char pp_parser_grant[((size_t)1024u * 1024u * 1024u)];\n\n"
        "/* A source line index, identical to the CLI's dump_line_index: the byte\n"
        "** offset each line begins at, so a node byte offset converts to a 0-based\n"
        "** (line, column) without rescanning the source per node. */\n"
        "typedef struct { uint32_t *starts; uint32_t count;\n"
        "                 const unsigned char *src; uint32_t src_len; } pp_parser_lines;\n"
        "static void pp_parser_lines_build(pp_parser_lines *ix,\n"
        "                                  const unsigned char *src, uint32_t len)\n"
        "{\n"
        "    uint32_t i, lines = 1;\n"
        "    ix->src = src; ix->src_len = len; ix->starts = NULL; ix->count = 0;\n"
        "    for (i = 0; i < len; i++) if (src[i] == '\\n') lines++;\n"
        "    ix->starts = (uint32_t *)malloc(lines * sizeof *ix->starts);\n"
        "    if (ix->starts == NULL) return;\n"
        "    ix->starts[0] = 0; ix->count = 1;\n"
        "    for (i = 0; i < len; i++) if (src[i] == '\\n') ix->starts[ix->count++] = i + 1;\n"
        "}\n"
        "static void pp_parser_line_col(const pp_parser_lines *ix, uint32_t byte,\n"
        "                               uint32_t *ol, uint32_t *oc)\n"
        "{\n"
        "    if (byte > ix->src_len) byte = ix->src_len;\n"
        "    if (ix->starts == NULL) {\n"
        "        uint32_t i, line = 0, ls = 0;\n"
        "        for (i = 0; i < byte; i++) if (ix->src[i] == '\\n') { line++; ls = i + 1; }\n"
        "        *ol = line; *oc = byte - ls; return;\n"
        "    }\n"
        "    {\n"
        "        uint32_t lo = 0, hi = ix->count - 1, line = 0;\n"
        "        while (lo <= hi) {\n"
        "            uint32_t mid = lo + (hi - lo) / 2;\n"
        "            if (ix->starts[mid] <= byte) { line = mid; lo = mid + 1; }\n"
        "            else { if (mid == 0) break; hi = mid - 1; }\n"
        "        }\n"
        "        *ol = line; *oc = byte - ix->starts[line];\n"
        "    }\n"
        "}\n\n"
        "/* Print one node (indented by its depth), byte-identical to the CLI dump,\n"
        "** including the `[s-e] [l,c]-[l,c]' span. */\n"
        "static void pp_parser_dump_node(const pocketparse_tree *tree,\n"
        "                                pocketparse_tree_cursor *cursor,\n"
        "                                const pp_parser_lines *ix)\n"
        "{\n"
        "    uint32_t depth = pocketparse_tree_cursor_depth(cursor);\n"
        "    uint32_t i;\n"
        "    pocketparse_symbol kind  = pocketparse_tree_cursor_kind(cursor);\n"
        "    pocketparse_symbol field = pocketparse_tree_cursor_field(cursor);\n"
        "    uint32_t flags = pocketparse_tree_cursor_flags(cursor);\n"
        "    pocketparse_string kn = pocketparse_tree_symbol_name(tree, kind);\n"
        "    for (i = 0; i < depth; i++) fputs(\"  \", stdout);\n"
        "    if (flags & POCKETPARSE_NODE_MISSING) fputs(\"MISSING \", stdout);\n"
        "    if (field.id != POCKETPARSE_NO_FIELD) {\n"
        "        pocketparse_string fn = pocketparse_tree_symbol_name(tree, field);\n"
        "        printf(\"%.*s: \", (int)fn.length, (const char *)fn.pointer);\n"
        "    }\n"
        "    printf(\"%.*s\", (int)kn.length, (const char *)kn.pointer);\n"
        "    {\n"
        "        uint32_t lang = pp_tree_cursor_language_tag(cursor);\n"
        "        if (lang != PP_NO_SYMBOL) {\n"
        "            pocketparse_symbol ls; pocketparse_string ln;\n"
        "            ls.id = lang; ln = pocketparse_tree_symbol_name(tree, ls);\n"
        "            if (ln.length > 0)\n"
        "                printf(\" [%.*s]\", (int)ln.length, (const char *)ln.pointer);\n"
        "        }\n"
        "    }\n"
        "    {\n"
        "        uint32_t sb = pocketparse_tree_cursor_start_byte(cursor);\n"
        "        uint32_t eb = pocketparse_tree_cursor_end_byte(cursor);\n"
        "        uint32_t sl, sc, el, ec;\n"
        "        pp_parser_line_col(ix, sb, &sl, &sc);\n"
        "        pp_parser_line_col(ix, eb, &el, &ec);\n"
        "        printf(\" [%u-%u] [%u,%u]-[%u,%u]\", sb, eb, sl, sc, el, ec);\n"
        "    }\n"
        "    printf(\"\\n\");\n"
        "}\n\n"
        "/* Pre-order dump of the subtree under `cursor', ITERATIVELY (cursor-driven,\n"
        "** no C recursion) so a pathologically deep tree cannot overflow the C stack\n"
        "** here either. Output byte-identical to the CLI's recursive dump. */\n"
        "static void pp_parser_dump(const pocketparse_tree *tree,\n"
        "                       pocketparse_tree_cursor *cursor,\n"
        "                       const pp_parser_lines *ix)\n"
        "{\n"
        "    for (;;) {\n"
        "        pp_parser_dump_node(tree, cursor, ix);\n"
        "        if (pocketparse_tree_cursor_goto_first_child(cursor))\n"
        "            continue;\n"
        "        for (;;) {\n"
        "            if (pocketparse_tree_cursor_goto_next_sibling(cursor))\n"
        "                break;\n"
        "            if (!pocketparse_tree_cursor_goto_parent(cursor))\n"
        "                return;\n"
        "        }\n"
        "    }\n"
        "}\n\n");

    pp_parser_text(cursor,
        "static void pp_parser_diag(const pocketparse_diagnostic *diag)\n"
        "{\n"
        "    fprintf(stderr, \"%.*s\", (int)diag->message.length,\n"
        "            (const char *)diag->message.pointer);\n"
        "    if (diag->code.length > 0)\n"
        "        fprintf(stderr, \" [%.*s]\", (int)diag->code.length,\n"
        "                (const char *)diag->code.pointer);\n"
        "    fputc('\\n', stderr);\n"
        "    if (diag->help.length > 0)\n"
        "        fprintf(stderr, \"  help: %.*s\\n\", (int)diag->help.length,\n"
        "                (const char *)diag->help.pointer);\n"
        "}\n\n");

    pp_parser_text(cursor,
        "int main(void)\n"
        "{\n"
        "    pocketparse_configuration cfg;\n"
        "    pocketparse_engine  *engine;\n"
        "    pocketparse_grammar *grammar = NULL;\n"
        "    pocketparse_parser  *parser  = NULL;\n"
        "    pocketparse_source   source;\n"
        "    pocketparse_string   empty;\n"
        "    pocketparse_result   result;\n"
        "    const pocketparse_tree  *tree;\n"
        "    pocketparse_tree_cursor *cursor = NULL;\n"
        "    unsigned char *input = NULL;\n"
        "    size_t length = 0, capacity = 0;\n"
        "    uint32_t count, d; int had_error = 0;\n"
        "    cfg.memory.base = pp_parser_grant;\n"
        "    cfg.memory.size = ((size_t)1024u * 1024u * 1024u);\n"
        "    cfg.memory.grow = NULL; cfg.memory.user = NULL;\n"
        "    cfg.memory.chunk_minimum = 0; cfg.flags = 0;\n"
        "    engine = pocketparse_open(cfg);\n"
        "    if (engine == NULL) { fputs(\"out of memory\\n\", stderr); return 1; }\n"
        "    source.name.pointer = ");
    pp_parser_name(cursor, prefix, "pp_parser_grammar_bytes", "grammar_bytes");
    pp_parser_text(cursor, "; source.name.length = 0;\n"
        "    source.bytes.pointer = ");
    pp_parser_name(cursor, prefix, "pp_parser_grammar_bytes", "grammar_bytes");
    pp_parser_text(cursor, ";\n"
        "    source.bytes.length = (uint32_t)sizeof ");
    pp_parser_name(cursor, prefix, "pp_parser_grammar_bytes", "grammar_bytes");
    pp_parser_text(cursor, ";\n"
        "    if (pocketparse_load_binary_grammar(engine, source, &grammar)\n"
        "            != POCKETPARSE_OK) {\n"
        "        pocketparse_string f = pocketparse_fault(engine);\n"
        "        fprintf(stderr, \"error: %.*s\\n\", (int)f.length, (const char *)f.pointer);\n"
        "        return 1;\n"
        "    }\n"
        "    empty.pointer = NULL; empty.length = 0;\n"
        "    if (pocketparse_parser_new(engine, grammar, empty, &parser)\n"
        "            != POCKETPARSE_OK || parser == NULL) {\n"
        "        pocketparse_string f = pocketparse_fault(engine);\n"
        "        fprintf(stderr, \"error: %.*s\\n\", (int)f.length, (const char *)f.pointer);\n"
        "        return 1;\n"
        "    }\n"
        "    for (;;) {\n"
        "        size_t got;\n"
        "        if (length + 65536 > capacity) {\n"
        "            size_t nc = capacity == 0 ? 65536 : capacity * 2;\n"
        "            unsigned char *grown = realloc(input, nc);\n"
        "            if (grown == NULL) { free(input); return 1; }\n"
        "            input = grown; capacity = nc;\n"
        "        }\n"
        "        got = fread(input + length, 1, 65536, stdin);\n"
        "        length += got;\n"
        "        if (got < 65536) break;\n"
        "    }\n"
        "    {\n"
        "        pocketparse_string bytes;\n"
        "        bytes.pointer = input; bytes.length = (uint32_t)length;\n"
        "        pocketparse_push(parser, bytes);\n"
        "    }\n"
        "    result = pp_parser_run(parser, ");
    pp_parser_name(cursor, prefix, "pp_execute_parser", "execute");
    pp_parser_text(cursor, ");\n"
        "    if (result == POCKETPARSE_OUT_OF_MEMORY) {\n"
        "        fprintf(stderr, \"error: out of memory\\n\");\n"
        "        free(input);\n"
        "        return 2;\n"
        "    }\n"
        "    if (result == POCKETPARSE_SYNTAX_ERROR || result == POCKETPARSE_ERROR) {\n"
        "        count = pocketparse_parser_diagnostic_count(parser);\n"
        "        if (count == 0) fprintf(stderr, \"error: parse failed\\n\");\n"
        "        for (d = 0; d < count; d++) {\n"
        "            const pocketparse_diagnostic *diag =\n"
        "                pocketparse_parser_diagnostic(parser, d);\n"
        "            if (diag != NULL) pp_parser_diag(diag);\n"
        "        }\n"
        "        free(input);\n"
        "        return 1;\n"
        "    }\n"
        "    tree = pocketparse_parser_tree(parser);\n"
        "    if (tree != NULL &&\n"
        "        pocketparse_tree_cursor_new(tree, &cursor) == POCKETPARSE_OK) {\n"
        "        pp_parser_lines ix;\n"
        "        pp_parser_lines_build(&ix, input, (uint32_t)length);\n"
        "        pp_parser_dump(tree, cursor, &ix);\n"
        "        free(ix.starts);\n"
        "        pocketparse_tree_cursor_free(cursor);\n"
        "    }\n"
        "    count = pocketparse_parser_diagnostic_count(parser);\n"
        "    for (d = 0; d < count; d++) {\n"
        "        const pocketparse_diagnostic *diag =\n"
        "            pocketparse_parser_diagnostic(parser, d);\n"
        "        if (diag == NULL) continue;\n"
        "        pp_parser_diag(diag);\n"
        "        if (diag->severity == POCKETPARSE_SEVERITY_ERROR) had_error = 1;\n"
        "    }\n"
        "    free(input);\n"
        "    return had_error ? 1 : 0;\n"
        "}\n");
}

/* ** Emit compiled-parser C source for a compiled grammar into the engine arena,
** in two passes (measure, then fill). Returns the bytes through out_source. The
** caller (pocketparse_emit_c, behind POCKETPARSE_EMIT_PARSER) does the file IO. */
/* ** Derive the DEFAULT symbol prefix from a grammar's captured name (W17-3),
** returning a NUL-terminated C identifier allocated from the grammar arena, or
** NULL when the grammar has no usable name (so the caller keeps the historical
** default symbol names). The grammar name is sanitized exactly like pp_parser_slug:
** every byte outside [A-Za-z0-9_] becomes `_', and a leading digit is prefixed with
** `s', so any grammar name yields a legal identifier. An empty name yields NULL. */
static const char *pp_parser_default_prefix(const pocketparse_grammar *grammar)
{
    pocketparse_string name;
    char              *buffer;
    uint32_t           i;
    uint32_t           length;
    uint32_t           extra;

    if (grammar->name_symbol == PP_NO_SYMBOL)
        return 0;
    name = pp_intern_name(&grammar->interner, grammar->name_symbol);
    if (name.length == 0)
        return 0;

    /* A leading digit needs one extra `s' byte; size the buffer for that plus the
    ** terminating NUL. */
    extra  = (name.pointer[0] >= '0' && name.pointer[0] <= '9') ? 1u : 0u;
    buffer = (char *)pp_alloc(grammar->arena, name.length + extra + 1u);
    if (buffer == 0)
        return 0;

    length = 0;
    for (i = 0; i < name.length; i++) {
        char c  = (char)name.pointer[i];
        int  ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_';
        if (i == 0 && c >= '0' && c <= '9')
            buffer[length++] = 's';
        buffer[length++] = ok ? c : '_';
    }
    buffer[length] = '\0';
    return buffer;
}

pocketparse_result pp_emit_parser(pocketparse_engine *engine,
                              const pocketparse_grammar *grammar,
                              int lib,
                              pocketparse_string *out_source)
{
    pp_parser_cursor   measure;
    pp_parser_cursor   emit;
    pocketparse_string packed;
    pocketparse_string source_name;
    pocketparse_string start_name;
    pocketparse_result result;
    char              *buffer;
    const char        *prefix;

    if (out_source != 0) { out_source->pointer = 0; out_source->length = 0; }
    if (engine == 0 || grammar == 0 || out_source == 0)
        return POCKETPARSE_INVALID;

    /* Resolve the symbol prefix. An explicit --name (engine->emit_parser_prefix)
    ** always wins. With no --name, default the prefix from the grammar's own name
    ** (W17-3): "json" -> json_..., "javascript" -> javascript_..., so two parsers
    ** compiled from differently named grammars carry disjoint symbols and nest/coexist
    ** in one binary with no flag. A grammar with no captured name (e.g. an old binary
    ** .ppg) falls back to the historical default symbol names (prefix NULL), keeping
    ** every legacy caller byte-for-byte unchanged. */
    prefix = engine->emit_parser_prefix;
    if (prefix != 0 && prefix[0] == '\0')
        prefix = 0;
    if (prefix == 0)
        prefix = pp_parser_default_prefix(grammar);

    result = pocketparse_grammar_serialize(grammar, &packed);
    if (result != POCKETPARSE_OK)
        return result;

    source_name = pp_text("grammar");
    if (grammar->start_symbol == PP_NO_SYMBOL)
        start_name = pp_text("");
    else
        start_name = pp_intern_name(&grammar->interner, grammar->start_symbol);

    measure.bytes = 0; measure.length = 0;
    pp_parser_emit_all(&measure, prefix, grammar, source_name, start_name, packed,
                       grammar->start_symbol, lib);

    buffer = (char *)pp_alloc(grammar->arena, measure.length);
    if (buffer == 0)
        return POCKETPARSE_OUT_OF_MEMORY;

    emit.bytes = buffer; emit.length = 0;
    pp_parser_emit_all(&emit, prefix, grammar, source_name, start_name, packed,
                       grammar->start_symbol, lib);

    out_source->pointer = (const uint8_t *)buffer;
    out_source->length  = emit.length;
    return POCKETPARSE_OK;
}
