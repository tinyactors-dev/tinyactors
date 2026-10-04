/*
** grammar.c -- grammar validation, interning, and the rule/token IR tables.
**
** pocketparse_compile_scheme is the front door: it reads the .scm source into
** forms, validates the top-level shape, interns every name to a dense symbol,
** compiles each (token ...) and (rule ...) body into IR, records the extras
** policy, and resolves the start rule. A malformed grammar -- an unterminated
** list, a start: that names no rule, an unknown combinator -- is rejected with
** a fault message (which the CLI renders as an "error"). Freestanding.
**
** A grammar may name its dependencies with top-level (import NAME) forms. The
** core has no filesystem, so it never reads a module itself: it asks the host's
** module resolver (pocketparse_set_module_resolver) for the module's bytes, then
** compiles those bytes into the SAME grammar, merging the module's tokens, rules,
** and extras so the importing grammar can reference them by name. Imports resolve
** entirely at compile time -- nothing about the rule IR or the packed .ppg format
** changes -- and a small visited set guards against import cycles.
*/
#include "internal.h"

/* ** Carve a fresh, empty grammar from the engine arena. The interner shares
** that arena so symbol names outlive compilation. */
static pocketparse_grammar *pp_grammar_new(pocketparse_engine *engine)
{
    pocketparse_grammar *grammar = (pocketparse_grammar *)pp_alloc(
        engine->arena, sizeof *grammar);
    if (grammar == 0)
        return 0;
    grammar->arena            = engine->arena;
    grammar->name_symbol      = PP_NO_SYMBOL;
    grammar->interner.arena   = engine->arena;
    grammar->interner.names   = 0;
    grammar->interner.count   = 0;
    grammar->interner.capacity = 0;
    grammar->tokens           = 0;
    grammar->token_count      = 0;
    grammar->rules            = 0;
    grammar->rule_count       = 0;
    grammar->extras           = 0;
    grammar->extra_count      = 0;
    grammar->lex_goals        = 0;
    grammar->lex_goal_count   = 0;
    grammar->param_goal_count = 0;
    grammar->start_symbol     = PP_NO_SYMBOL;
    grammar->vm_program       = 0;
    grammar->vm_program_start = PP_NO_SYMBOL;
    grammar->token_index      = 0;
    grammar->token_index_size = 0;
    grammar->hidden_index      = 0;
    grammar->hidden_index_size = 0;
    grammar->hidden_index_built = 0;
    return grammar;
}

/* ** Record a fault on the engine and return INVALID, so compile failures are
** one line. The message is a C literal with static lifetime. */
static pocketparse_result pp_grammar_fail(pocketparse_engine *engine,
                                          const char *message)
{
    engine->fault = pp_text(message);
    return POCKETPARSE_INVALID;
}

/* ** A grammar under construction. It wraps the grammar handle with the table
** capacities the public struct does not carry (the loaded-from-.ppg path in
** pack.c never appends, so the public grammar tracks only counts). Imports make
** the token/rule/extras tables grow as the main source and every imported module
** contribute definitions, so the builder grows them geometrically rather than
** sizing them in one up-front pass. The visited list records the module names
** already being compiled, so an import cycle is caught instead of recursing
** forever. */
typedef struct pp_grammar_builder {
    pocketparse_engine  *engine;
    pocketparse_grammar *grammar;
    uint32_t             token_capacity;
    uint32_t             rule_capacity;
    uint32_t             extra_capacity;
    uint32_t             lex_goal_capacity;
    pocketparse_string  *visited;
    uint32_t             visited_count;
    uint32_t             visited_capacity;
    /* The token/rule symbols a base grammar contributed through (extends NAME),
    ** so the root source's own redefinition of one is caught as a collision
    ** unless it is wrapped in (override ...). Empty for a grammar that extends
    ** nothing. */
    uint32_t            *base_symbols;
    uint32_t             base_symbol_count;
    uint32_t             base_symbol_capacity;
    /* Per-compile counter for synthesized (param ...) template-rule names; on the
    ** builder (not a process static) so grammar compilation stays reentrant. */
    uint32_t             param_template_counter;
} pp_grammar_builder;

/* ** Record one symbol as base-defined (introduced by an (extends ...) base), so
** a later bare redefinition collides. Returns 1, or 0 on allocation failure. */
static int pp_builder_mark_base(pp_grammar_builder *builder, uint32_t symbol)
{
    if (builder->base_symbol_count == builder->base_symbol_capacity) {
        uint32_t  new_cap = builder->base_symbol_capacity == 0
                                ? 16 : builder->base_symbol_capacity * 2;
        uint32_t *grown = (uint32_t *)pp_alloc(
            builder->grammar->arena, new_cap * sizeof *grown);
        uint32_t  i;
        if (grown == 0)
            return 0;
        for (i = 0; i < builder->base_symbol_count; i++)
            grown[i] = builder->base_symbols[i];
        builder->base_symbols         = grown;
        builder->base_symbol_capacity = new_cap;
    }
    builder->base_symbols[builder->base_symbol_count++] = symbol;
    return 1;
}

/* ** Is this symbol one a base grammar already defined? Used to reject a bare
** redefinition (one not wrapped in (override ...)) with an override diagnostic. */
static int pp_builder_is_base_symbol(const pp_grammar_builder *builder,
                                     uint32_t symbol)
{
    uint32_t i;
    for (i = 0; i < builder->base_symbol_count; i++)
        if (builder->base_symbols[i] == symbol)
            return 1;
    return 0;
}

/* ** Replace the IR pattern of an existing token with this symbol, so an
** (override (token NAME ...)) supersedes the base definition rather than adding a
** duplicate. Returns 1 if a token was replaced, 0 if none carried that symbol. */
static int pp_builder_replace_token(pp_grammar_builder *builder, uint32_t symbol,
                                    pp_ir *pattern)
{
    uint32_t i;
    for (i = 0; i < builder->grammar->token_count; i++)
        if (builder->grammar->tokens[i].symbol == symbol) {
            builder->grammar->tokens[i].pattern = pattern;
            return 1;
        }
    return 0;
}

/* ** Replace the body of an existing rule with this symbol, so an (override (rule
** NAME ...)) supersedes the base definition rather than adding a duplicate.
** Returns 1 if a rule was replaced, 0 if none carried that symbol. */
static int pp_builder_replace_rule(pp_grammar_builder *builder, uint32_t symbol,
                                   pp_ir *body, uint32_t flags)
{
    uint32_t i;
    for (i = 0; i < builder->grammar->rule_count; i++)
        if (builder->grammar->rules[i].symbol == symbol) {
            builder->grammar->rules[i].body  = body;
            builder->grammar->rules[i].flags = flags;
            return 1;
        }
    return 0;
}

/* ** Append a compiled token to the grammar's token table, growing it
** geometrically. Returns 1, or 0 on allocation failure. */
static int pp_builder_add_token(pp_grammar_builder *builder, uint32_t symbol,
                                pp_ir *pattern)
{
    pocketparse_grammar *grammar = builder->grammar;
    if (grammar->token_count == builder->token_capacity) {
        uint32_t new_cap = builder->token_capacity == 0
                               ? 8 : builder->token_capacity * 2;
        pp_token_def *grown = (pp_token_def *)pp_alloc(
            grammar->arena, new_cap * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return 0;
        for (i = 0; i < grammar->token_count; i++)
            grown[i] = grammar->tokens[i];
        grammar->tokens         = grown;
        builder->token_capacity = new_cap;
    }
    grammar->tokens[grammar->token_count].symbol  = symbol;
    grammar->tokens[grammar->token_count].pattern = pattern;
    grammar->token_count++;
    return 1;
}

/* ** Append a compiled rule to the grammar's rule table, growing it
** geometrically. Returns 1, or 0 on allocation failure. */
static int pp_builder_add_rule(pp_grammar_builder *builder, uint32_t symbol,
                               pp_ir *body, uint32_t flags)
{
    pocketparse_grammar *grammar = builder->grammar;
    if (grammar->rule_count == builder->rule_capacity) {
        uint32_t new_cap = builder->rule_capacity == 0
                               ? 8 : builder->rule_capacity * 2;
        pp_rule_def *grown = (pp_rule_def *)pp_alloc(
            grammar->arena, new_cap * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return 0;
        for (i = 0; i < grammar->rule_count; i++)
            grown[i] = grammar->rules[i];
        grammar->rules         = grown;
        builder->rule_capacity = new_cap;
    }
    grammar->rules[grammar->rule_count].symbol = symbol;
    grammar->rules[grammar->rule_count].flags  = flags;
    grammar->rules[grammar->rule_count].slot_count = 0; /* lex-goal-implementation */
    grammar->rules[grammar->rule_count].body   = body;
    grammar->rule_count++;
    return 1;
}

/* ** Append one extras entry to the grammar's extras table, growing it
** geometrically. Returns 1, or 0 on allocation failure. */
static int pp_builder_add_extra(pp_grammar_builder *builder,
                                uint32_t token_symbol, pp_extra_policy policy,
                                pp_extra_attach attach)
{
    pocketparse_grammar *grammar = builder->grammar;
    if (grammar->extra_count == builder->extra_capacity) {
        uint32_t new_cap = builder->extra_capacity == 0
                               ? 8 : builder->extra_capacity * 2;
        pp_extra *grown = (pp_extra *)pp_alloc(
            grammar->arena, new_cap * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return 0;
        for (i = 0; i < grammar->extra_count; i++)
            grown[i] = grammar->extras[i];
        grammar->extras         = grown;
        builder->extra_capacity = new_cap;
    }
    grammar->extras[grammar->extra_count].token_symbol = token_symbol;
    grammar->extras[grammar->extra_count].policy       = policy;
    grammar->extras[grammar->extra_count].attach       = attach;
    grammar->extra_count++;
    return 1;
}

/* ** Compile the (extras ...) form into the grammar's extras table. Each entry is
** (trivia X) / (skip X) / (error X) naming a token, optionally followed by an
** `attach: leading' or `attach: trailing' modifier (default trailing) that sets how a
** TRIVIA kind binds to the tree (leading = held and re-emitted ahead of the next
** sibling; see docs/grammar.md). Entries append
** through the builder, so a module's extras merge after the importing grammar's own. */
static int pp_compile_extras(pp_grammar_builder *builder,
                             const pp_form *extras_form, const char **error)
{
    uint32_t i;
    for (i = 1; i < extras_form->count; i++) {
        const pp_form *entry = extras_form->items[i];
        pp_extra_policy policy;
        pp_extra_attach attach = PP_EXTRA_ATTACH_TRAILING;
        uint32_t        token_symbol;
        /* The entry is (POLICY SYMBOL) with an optional trailing `attach: VALUE' pair,
        ** so it is a 2- or 4-element list. */
        if (entry->kind != PP_FORM_LIST ||
            (entry->count != 2 && entry->count != 4) ||
            entry->items[0]->kind != PP_FORM_SYMBOL ||
            entry->items[1]->kind != PP_FORM_SYMBOL) {
            *error = "malformed extras entry";
            return 0;
        }
        if (pp_string_equal_c(entry->items[0]->text, "trivia"))
            policy = PP_EXTRA_TRIVIA;
        else if (pp_string_equal_c(entry->items[0]->text, "skip"))
            policy = PP_EXTRA_SKIP;
        else if (pp_string_equal_c(entry->items[0]->text, "error"))
            policy = PP_EXTRA_ERROR;
        else {
            *error = "unknown extras policy";
            return 0;
        }
        if (entry->count == 4) {
            const pp_form *key = entry->items[2];
            const pp_form *val = entry->items[3];
            if (key->kind != PP_FORM_KEYWORD || val->kind != PP_FORM_SYMBOL ||
                !pp_string_equal_c(key->text, "attach")) {
                *error = "extras entry expects an attach: modifier";
                return 0;
            }
            if (pp_string_equal_c(val->text, "leading"))
                attach = PP_EXTRA_ATTACH_LEADING;
            else if (pp_string_equal_c(val->text, "trailing"))
                attach = PP_EXTRA_ATTACH_TRAILING;
            else {
                *error = "unknown attach value (want leading or trailing)";
                return 0;
            }
        }
        token_symbol = pp_intern(&builder->grammar->interner,
                                 entry->items[1]->text);
        if (!pp_builder_add_extra(builder, token_symbol, policy, attach)) {
            *error = "out of memory";
            return 0;
        }
    }
    return 1;
}

/* ** Append one lexical-goal entry to the grammar's table, growing it
** geometrically. The third union word is `target_symbol` for a static redirect
** (is_parametrized == 0) or `template_rule_index` for a parametrized entry
** (is_parametrized != 0). Returns 1, or 0 on allocation failure. Like extras, a
** module's lex-goals merge after the importing grammar's own. */
static int pp_builder_add_lex_goal_entry(pp_grammar_builder *builder,
                                         uint32_t goal_symbol,
                                         uint32_t trigger_symbol,
                                         uint32_t union_word,
                                         uint32_t is_parametrized)
{
    pocketparse_grammar *grammar = builder->grammar;
    if (grammar->lex_goal_count == builder->lex_goal_capacity) {
        uint32_t new_cap = builder->lex_goal_capacity == 0
                               ? 8 : builder->lex_goal_capacity * 2;
        pp_lex_goal *grown = (pp_lex_goal *)pp_alloc(
            grammar->arena, new_cap * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return 0;
        for (i = 0; i < grammar->lex_goal_count; i++)
            grown[i] = grammar->lex_goals[i];
        grammar->lex_goals         = grown;
        builder->lex_goal_capacity = new_cap;
    }
    grammar->lex_goals[grammar->lex_goal_count].goal_symbol    = goal_symbol;
    grammar->lex_goals[grammar->lex_goal_count].trigger_symbol = trigger_symbol;
    /* The union members share storage, so writing target_symbol also sets
    ** template_rule_index; the flag records which member is live. */
    grammar->lex_goals[grammar->lex_goal_count].target_symbol  = union_word;
    grammar->lex_goals[grammar->lex_goal_count].is_parametrized = is_parametrized;
    grammar->lex_goal_count++;
    return 1;
}

/* ** Append a static lexical-goal redirect (trigger token -> target token). */
static int pp_builder_add_lex_goal(pp_grammar_builder *builder,
                                   uint32_t goal_symbol,
                                   uint32_t trigger_symbol,
                                   uint32_t target_symbol)
{
    return pp_builder_add_lex_goal_entry(builder, goal_symbol, trigger_symbol,
                                         target_symbol, 0);
}

/* ** Rewrite every PP_OP_REF leaf whose symbol matches `name_symbol` into a
** PP_OP_PARAM_REF placeholder, in place, throughout the compiled BODY of a
** (param NAME BODY) entry. A reference matching the parameter NAME stands for the
** captured literal: at drain time pp_param_instantiate replaces the placeholder
** with a PP_OP_LITERAL of the captured bytes. A PP_OP_REF has no children, so the
** rewrite clears nothing but the operand fields (lex-goal-implementation). Any
** PP_OP_REF that does NOT match NAME is left intact, so the body restriction scan
** that runs afterwards can reject it as a non-token-level op. */
static void pp_param_rewrite_refs(pp_ir *ir, uint32_t name_symbol)
{
    uint32_t i;
    if (ir == 0)
        return;
    if (ir->op == PP_OP_REF && ir->symbol == name_symbol) {
        ir->op       = PP_OP_PARAM_REF;
        ir->min      = 0;          /* param_index (always 0) */
        ir->max      = 0;
        ir->symbol   = 0;
        ir->children = 0;          /* a REF carries no children */
        ir->child_count = 0;
        return;
    }
    for (i = 0; i < ir->child_count; i++)
        pp_param_rewrite_refs(ir->children[i], name_symbol);
}

/* ** Validate that a converted (param NAME BODY) subtree contains only
** token-level combinators. The instantiated body is run by the token matcher,
** which handles only these ops; any other op (a rule reference, a goal/field/
** alias/inject form, or the deferred-slot ops) would misbehave or crash, so it is
** a compile error. The scan is recursive and descends into ALL children,
** including both children of PP_OP_NOT. Per the spec's explicit allow-list,
** PP_OP_CLASS_NAME is NOT allowed. Returns 1 if the subtree is clean, 0 with
** *error set on the first forbidden op (lex-goal-implementation). */
static int pp_param_body_token_level(const pp_ir *ir, const char **error)
{
    uint32_t i;
    if (ir == 0)
        return 1;
    switch (ir->op) {
    case PP_OP_LITERAL:
    case PP_OP_CLASS:
    case PP_OP_ANY:
    case PP_OP_SEQ:
    case PP_OP_CHOICE:
    case PP_OP_REPEAT:
    case PP_OP_OPTIONAL:
    case PP_OP_NOT:
    case PP_OP_PARAM_REF:
        break;
    default:
        *error = "param body must contain only token-level patterns";
        return 0;
    }
    for (i = 0; i < ir->child_count; i++)
        if (!pp_param_body_token_level(ir->children[i], error))
            return 0;
    return 1;
}

/* ** Monotonic counter so each synthesized (param ...) template rule gets a
** distinct internal symbol. A leading space cannot appear in a user .scm symbol
** (the reader treats whitespace as a token delimiter, so no bare symbol begins
** with one), so " param-template" names never collide with a grammar identifier;
** the counter keeps two param entries in one grammar from colliding with each
** other. The counter lives on the per-compile pp_grammar_builder (not a process
** static), so grammar compilation stays reentrant: the name only has to be
** unique within a single grammar's interner, which a per-builder counter gives. */

/* ** Compile a (param NAME BODY) value inside a (lex-goal ...) entry: compile
** BODY, rewrite NAME references to PP_OP_PARAM_REF, enforce the token-level
** restriction, append BODY as an internal template rule, and report the appended
** rule's index. Returns 1 (and writes *out_rule_index), or 0 with *error set. */
static int pp_compile_param_template(pp_grammar_builder *builder,
                                     const pp_form *param_form,
                                     uint32_t *out_rule_index,
                                     const char **error)
{
    pocketparse_grammar *grammar  = builder->grammar;
    pp_interner         *interner = &grammar->interner;
    const pp_form       *name_form;
    const pp_form       *body_form;
    pp_ir               *body;
    uint32_t             name_symbol;
    uint32_t             template_symbol;
    uint32_t             err_byte;
    /* The synthesized template-rule name: " param-template" plus a counter. The
    ** leading space guarantees it cannot collide with any user grammar symbol. */
    char                 namebuf[32];
    uint32_t             n = builder->param_template_counter++;
    uint32_t             p = 0, d;
    pocketparse_string   namestr;

    if (param_form->count != 3 ||
        param_form->items[1]->kind != PP_FORM_SYMBOL) {
        *error = "param expects NAME and a body";
        return 0;
    }
    name_form = param_form->items[1];
    body_form = param_form->items[2];
    name_symbol = pp_intern(interner, name_form->text);

    body = pp_compile_pattern(builder->engine, grammar->arena, interner,
                              body_form, error, &err_byte);
    if (body == 0) {
        if ((*error)[0] == 0) *error = "bad param body";
        return 0;
    }
    pp_param_rewrite_refs(body, name_symbol);
    if (!pp_param_body_token_level(body, error))
        return 0;

    /* Build " param-template<N>" by hand (the freestanding core has no libc). */
    { const char *prefix = " param-template";
      const char *c = prefix;
      while (*c) namebuf[p++] = *c++; }
    if (n == 0) {
        namebuf[p++] = '0';
    } else {
        char digits[10];
        d = 0;
        while (n > 0) { digits[d++] = (char)('0' + (n % 10)); n /= 10; }
        while (d > 0) namebuf[p++] = digits[--d];
    }
    namestr.pointer = (const uint8_t *)namebuf;
    namestr.length  = p;
    template_symbol = pp_intern(interner, namestr);

    if (!pp_builder_add_rule(builder, template_symbol, body, 0)) {
        *error = "out of memory";
        return 0;
    }
    *out_rule_index = grammar->rule_count - 1;
    return 1;
}

/* ** Compile a (lex-goal GOAL trigger: VALUE ...) form into the grammar's
** lex-goal table. The head names the goal; the body is a flat run of
** keyword/value pairs. A keyword `trigger:` binds the trigger token (the keyword,
** colon stripped). VALUE is either a SYMBOL -- a static redirect to that target
** token, the JS `/`-is-regex-or-division case -- or a (param NAME BODY) list,
** which marks the entry PARAMETRIZED: BODY is compiled as an internal template
** rule whose index the entry stores, and the captured literal is substituted for
** NAME at drain time (lex-goal-implementation). */
static int pp_compile_lex_goal(pp_grammar_builder *builder,
                               const pp_form *form, const char **error)
{
    pp_interner *interner = &builder->grammar->interner;
    uint32_t     goal_symbol;
    uint32_t     i;

    if (form->count < 2 || form->items[1]->kind != PP_FORM_SYMBOL) {
        *error = "lex-goal needs a goal name";
        return 0;
    }
    goal_symbol = pp_intern(interner, form->items[1]->text);

    if (form->count < 4 || (form->count & 1) != 0) {
        *error = "lex-goal expects trigger: Target pairs";
        return 0;
    }
    for (i = 2; i + 1 < form->count; i += 2) {
        const pp_form *key = form->items[i];
        const pp_form *val = form->items[i + 1];
        uint32_t       trigger_symbol;
        if (key->kind != PP_FORM_KEYWORD) {
            *error = "lex-goal expects trigger: Target pairs";
            return 0;
        }
        trigger_symbol = pp_intern(interner, key->text);

        if (val->kind == PP_FORM_SYMBOL) {
            /* Static redirect: trigger token scans as the named target token. */
            uint32_t target_symbol = pp_intern(interner, val->text);
            if (!pp_builder_add_lex_goal(builder, goal_symbol, trigger_symbol,
                                         target_symbol)) {
                *error = "out of memory";
                return 0;
            }
        } else if (val->kind == PP_FORM_LIST && val->count >= 1 &&
                   val->items[0]->kind == PP_FORM_SYMBOL &&
                   pp_string_equal_c(val->items[0]->text, "param")) {
            /* Parametrized entry: (param NAME BODY). There is no static target;
            ** the entry stores the template rule index and is flagged
            ** parametrized. */
            uint32_t template_rule_index;
            if (!pp_compile_param_template(builder, val, &template_rule_index,
                                           error))
                return 0;
            if (!pp_builder_add_lex_goal_entry(builder, goal_symbol,
                                               trigger_symbol,
                                               template_rule_index, 1)) {
                *error = "out of memory";
                return 0;
            }
        } else {
            *error = "lex-goal expects trigger: Target pairs";
            return 0;
        }
    }
    return 1;
}

/* ** Allocate a zeroed IR node of the given op with every field initialised. The
** token-engine's own pp_ir_new is static to token.c, so grammar.c mints the few
** IR nodes it needs (the recover node, the empty-class placeholder) through this
** shared zeroing path. Returns NULL on allocation failure. */
static pp_ir *pp_grammar_ir_new(pp_arena *arena, pp_op op)
{
    pp_ir   *ir = (pp_ir *)pp_alloc(arena, sizeof *ir);
    uint32_t i;
    if (ir == 0)
        return 0;
    ir->op              = op;
    ir->text            = pp_text("");
    ir->symbol          = PP_NO_SYMBOL;
    ir->min             = 0;
    ir->max             = 0;
    ir->children        = 0;
    ir->child_count     = 0;
    ir->cls.negated     = 0;
    for (i = 0; i < 32; i++)
        ir->cls.bits[i] = 0;
    ir->recover_message = pp_text("");
    ir->recover_help    = pp_text("");
    ir->recover_label   = pp_text("");
    ir->recover_insert  = pp_text("");
    return ir;
}

/* ** Allocate a PP_OP_RECOVER IR node with every field initialised. The token-
** engine's own pp_ir_new is static to token.c, so the recover node -- the only
** IR node grammar.c mints directly -- is built and zeroed here. The recover_*
** slices start empty and are filled from the (on eof ...) clause. */
static pp_ir *pp_recover_new(pp_arena *arena)
{
    pp_ir *ir = (pp_ir *)pp_alloc(arena, sizeof *ir);
    uint32_t i;
    if (ir == 0)
        return 0;
    ir->op              = PP_OP_RECOVER;
    ir->text            = pp_text("");
    ir->symbol          = PP_NO_SYMBOL;
    ir->min             = 0;
    ir->max             = 0;
    ir->children        = 0;
    ir->child_count     = 0;
    ir->cls.negated     = 0;
    for (i = 0; i < 32; i++)
        ir->cls.bits[i] = 0;
    ir->recover_message = pp_text("");
    ir->recover_help    = pp_text("");
    ir->recover_label   = pp_text("");
    ir->recover_insert  = pp_text("");
    return ir;
}

/* ** Compile a (recover BODY (on eof message: "..." [help: "..."]
** [insert: "..."] [label here: "..."])) form into a PP_OP_RECOVER node wrapping
** BODY. The (on eof ...) clause is a flat run of keyword/value pairs, save for
** (label NAME: "text") which is a nested list. Unknown keys are rejected. */
static pp_ir *pp_compile_recover(const pocketparse_engine *engine,
                                 pp_arena *arena, pp_interner *interner,
                                 const pp_form *form, const char **error,
                                 uint32_t *error_byte)
{
    pp_ir         *ir;
    pp_ir         *body;
    const pp_form *clause;
    uint32_t       i;

    *error = "";
    *error_byte = form->start_byte;
    if (form->count != 3) {
        *error = "recover expects a body and an (on eof ...) clause";
        return 0;
    }

    body = pp_compile_pattern(engine, arena, interner, form->items[1],
                              error, error_byte);
    if (body == 0)
        return 0;

    clause = form->items[2];
    if (clause->kind != PP_FORM_LIST || clause->count < 2 ||
        clause->items[0]->kind != PP_FORM_SYMBOL ||
        !pp_string_equal_c(clause->items[0]->text, "on") ||
        clause->items[1]->kind != PP_FORM_SYMBOL ||
        !pp_string_equal_c(clause->items[1]->text, "eof")) {
        *error = "recover clause must be (on eof ...)";
        *error_byte = clause->start_byte;
        return 0;
    }

    ir = pp_recover_new(arena);
    if (ir == 0) { *error = "out of memory"; return 0; }
    ir->children = (pp_ir **)pp_alloc(arena, sizeof(pp_ir *));
    if (ir->children == 0) { *error = "out of memory"; return 0; }
    ir->children[0]  = body;
    ir->child_count  = 1;

    /* Walk the (on eof ...) payload: keyword/value pairs plus a (label ...). */
    for (i = 2; i < clause->count; i++) {
        const pp_form *key = clause->items[i];
        if (key->kind == PP_FORM_LIST) {
            /* (label here: "expected ) before end of file") */
            if (key->count == 3 &&
                key->items[0]->kind == PP_FORM_SYMBOL &&
                pp_string_equal_c(key->items[0]->text, "label") &&
                key->items[1]->kind == PP_FORM_KEYWORD &&
                key->items[2]->kind == PP_FORM_STRING) {
                ir->recover_label = key->items[2]->text;
                continue;
            }
            *error = "malformed label in recover clause";
            *error_byte = key->start_byte;
            return 0;
        }
        if (key->kind != PP_FORM_KEYWORD || i + 1 >= clause->count ||
            clause->items[i + 1]->kind != PP_FORM_STRING) {
            *error = "recover clause expects keyword: \"string\" pairs";
            *error_byte = key->start_byte;
            return 0;
        }
        if (pp_string_equal_c(key->text, "message"))
            ir->recover_message = clause->items[i + 1]->text;
        else if (pp_string_equal_c(key->text, "help"))
            ir->recover_help = clause->items[i + 1]->text;
        else if (pp_string_equal_c(key->text, "insert"))
            ir->recover_insert = clause->items[i + 1]->text;
        else {
            *error = "unknown keyword in recover clause";
            *error_byte = key->start_byte;
            return 0;
        }
        i++;
    }

    if (ir->recover_message.length == 0) {
        *error = "recover clause needs a message:";
        *error_byte = clause->start_byte;
        return 0;
    }
    /* The synthesized missing node's kind name is the recover_insert text.
    ** Resolve its intern id once here and cache it in ir->symbol (unused for
    ** PP_OP_RECOVER otherwise) so pp_recover_insert never interns at parse time. */
    ir->symbol = pp_intern(interner, ir->recover_insert);
    return ir;
}

/* ** Compile a rule body, intercepting a top-level (recover ...) head (which the
** token-engine combinator compiler does not know) and delegating every other
** shape -- including (goal ...), which the shared pattern compiler handles so it
** may also nest inside seq/choice/repeat -- to pp_compile_pattern. */
static pp_ir *pp_compile_rule_body(const pocketparse_engine *engine,
                                   pp_arena *arena, pp_interner *interner,
                                   const pp_form *form, const char **error,
                                   uint32_t *error_byte)
{
    if (form->kind == PP_FORM_LIST && form->count >= 1 &&
        form->items[0]->kind == PP_FORM_SYMBOL &&
        pp_string_equal_c(form->items[0]->text, "recover"))
        return pp_compile_recover(engine, arena, interner, form,
                                  error, error_byte);
    return pp_compile_pattern(engine, arena, interner, form, error, error_byte);
}

/* ** Validate that every rule/token reference resolves to a defined rule or
** token. Walks the IR, returning 0 with a message on the first dangling ref. */
static int pp_resolve_refs(const pocketparse_grammar *grammar, const pp_ir *ir,
                           const char **error)
{
    uint32_t i;
    if (ir == 0)
        return 1;
    if (ir->op == PP_OP_REF) {
        uint32_t j;
        int found = 0;
        for (j = 0; j < grammar->rule_count; j++)
            if (grammar->rules[j].symbol == ir->symbol) { found = 1; break; }
        if (!found)
            for (j = 0; j < grammar->token_count; j++)
                if (grammar->tokens[j].symbol == ir->symbol) { found = 1; break; }
        if (!found) {
            *error = "rule references an undefined name";
            return 0;
        }
    }
    for (i = 0; i < ir->child_count; i++)
        if (!pp_resolve_refs(grammar, ir->children[i], error))
            return 0;
    return 1;
}

/* ** W17-RECLAIM: does this IR body ALWAYS contribute exactly one child to its
** parent sink, by every path, with no own operator/literal/second-child? This is
** the structural test behind PP_RULE_TRANSPARENT_ALWAYS: a body that can only ever
** yield one child is a pure pass-through whose wrapper node the W17-FLAT collapse
** would always orphan, so the entry path can skip the node entirely.
**
** The yields-exactly-one shapes, and only these:
**   - PP_OP_REF       -- a reference contributes the referenced node: one child.
**   - PP_OP_FIELD     -- decorates its child; one child if its child yields one.
**                        (the field it stamps is overwritten by the call's field
**                        when the wrapper collapses, so it does not block it.)
**   - PP_OP_GOAL      -- sets the lexical goal around its child; transparent to the
**                        child count, so one child if its child yields one.
**   - PP_OP_CHOICE    -- yields one child iff EVERY alternative yields exactly one.
**
** Every other op can yield zero, two, or a variable number of children (SEQ,
** REPEAT, OPTIONAL), or contributes its own leaf/decoration in a way the simple
** field re-stamp does not reproduce (LITERAL, CLASS, ANY, ALIAS, NOT, NO_EXTRAS,
** RECOVER, INJECT), so it is conservatively NOT one-child. A false negative merely
** keeps the exit-time collapse (still correct, just unreclaimed); a false positive
** would change the tree, so the test errs strictly toward "no". */
static int pp_ir_yields_one_child(const pp_ir *ir)
{
    uint32_t i;
    if (ir == 0)
        return 0;
    switch (ir->op) {
    case PP_OP_REF:
        return 1;
    case PP_OP_FIELD:
    case PP_OP_GOAL:
        return ir->child_count == 1 && pp_ir_yields_one_child(ir->children[0]);
    case PP_OP_CHOICE:
        if (ir->child_count == 0)
            return 0;
        for (i = 0; i < ir->child_count; i++)
            if (!pp_ir_yields_one_child(ir->children[i]))
                return 0;
        return 1;
    default:
        return 0;
    }
}

/* ** W17-RECLAIM: mark every HIDDEN rule whose body always yields exactly one
** child as PP_RULE_TRANSPARENT_ALWAYS, so the entry path can skip its wrapper node
** outright instead of allocating it only to orphan it at exit. Called once, after
** every reference resolves (so pp_ir_yields_one_child reads a complete IR), from
** pocketparse_compile_scheme. The flag implies PP_RULE_HIDDEN and rides the packed
** flags field, so the .ppg load path and the generated parser inherit it. */
static void pp_grammar_mark_transparent_rules(pocketparse_grammar *grammar)
{
    uint32_t i;
    for (i = 0; i < grammar->rule_count; i++) {
        if ((grammar->rules[i].flags & PP_RULE_HIDDEN) == 0)
            continue;
        if (pp_ir_yields_one_child(grammar->rules[i].body))
            grammar->rules[i].flags |= PP_RULE_TRANSPARENT_ALWAYS;
    }
}

/* ** Compile one (token NAME ...) form and append it through the builder. A
** single-form body compiles directly; trailing forms wrap in an implicit seq.
** Returns 1, or 0 with *error set on a malformed body or allocation failure. */
static int pp_compile_token_form(pp_grammar_builder *builder,
                                 const pp_form *item, int is_override,
                                 const char **error)
{
    pp_arena    *arena    = builder->grammar->arena;
    pp_interner *interner = &builder->grammar->interner;
    pp_ir       *pattern;
    uint32_t     symbol;
    uint32_t     err_byte;

    if (item->count < 2 || item->items[1]->kind != PP_FORM_SYMBOL) {
        *error = "malformed token form";
        return 0;
    }
    symbol = pp_intern(interner, item->items[1]->text);
    if (!is_override && pp_builder_is_base_symbol(builder, symbol)) {
        *error = "redefining a base name requires (override ...)";
        return 0;
    }
    /* A body-less token -- (token NAME) with no pattern -- is a PARAMETRIZED
    ** placeholder (lex-goal-implementation). It has no static spelling: the real
    ** pattern is instantiated from a (param ...) lex-goal body and pushed onto the
    ** goal stack at drain time, so the static pattern must match NOTHING. An empty
    ** POSITIVE character class (negated == 0, every bit clear) accepts no byte,
    ** which is exactly that: the token is reached only through the parametrized
    ** goal redirect, never by its own static pattern. */
    if (item->count == 2) {
        pattern = pp_grammar_ir_new(arena, PP_OP_CLASS);
        if (pattern == 0) { *error = "out of memory"; return 0; }
        pattern->symbol = PP_NO_SYMBOL;
        /* cls.negated == 0 and all cls.bits == 0 already from pp_grammar_ir_new. */
        if (is_override && pp_builder_replace_token(builder, symbol, pattern))
            return 1;
        if (!pp_builder_add_token(builder, symbol, pattern)) {
            *error = "out of memory";
            return 0;
        }
        return 1;
    }
    /* A token body is a single pattern; allow (token N a b ...) as a seq for
    ** convenience, but the common case is one form. */
    if (item->count == 3) {
        pattern = pp_compile_pattern(builder->engine, arena, interner,
                                     item->items[2], error, &err_byte);
    } else {
        pp_ir   *seq;
        uint32_t k, child_count = item->count - 2;
        pp_ir **kids = (pp_ir **)pp_alloc(arena, child_count * sizeof *kids);
        if (kids == 0) { *error = "out of memory"; return 0; }
        for (k = 0; k < child_count; k++) {
            kids[k] = pp_compile_pattern(builder->engine, arena, interner,
                                         item->items[2 + k], error, &err_byte);
            if (kids[k] == 0)
                return 0;
        }
        seq = (pp_ir *)pp_alloc(arena, sizeof *seq);
        if (seq == 0) { *error = "out of memory"; return 0; }
        seq->op = PP_OP_SEQ;
        seq->text = pp_text("");
        seq->symbol = PP_NO_SYMBOL;
        seq->min = seq->max = 0;
        seq->cls.negated = 0;
        { uint32_t z; for (z = 0; z < 32; z++) seq->cls.bits[z] = 0; }
        seq->children = kids;
        seq->child_count = child_count;
        pattern = seq;
    }
    if (pattern == 0) {
        if ((*error)[0] == 0) *error = "bad token";
        return 0;
    }
    /* An override of a base token replaces that base definition in place; a fresh
    ** token (or an override naming a token not yet present) is appended. */
    if (is_override && pp_builder_replace_token(builder, symbol, pattern))
        return 1;
    if (!pp_builder_add_token(builder, symbol, pattern)) {
        *error = "out of memory";
        return 0;
    }
    return 1;
}

/* ** Compile one (rule NAME BODY) form and append it through the builder, or --
** when is_override -- replace the base rule of that name in place. A bare
** redefinition of a base name is rejected with an override diagnostic; a fresh
** rule appends. Returns 1, or 0 with *error set. */
static int pp_compile_rule_form(pp_grammar_builder *builder,
                                const pp_form *item, int is_override,
                                uint32_t rule_flags, const char **error)
{
    pocketparse_grammar *grammar = builder->grammar;
    pp_ir   *body;
    uint32_t symbol;
    uint32_t err_byte;
    uint32_t body_index = 2;
    int      no_extras = 0;

    if (item->count < 3 || item->items[1]->kind != PP_FORM_SYMBOL) {
        *error = "malformed rule form";
        return 0;
    }
    symbol = pp_intern(&grammar->interner, item->items[1]->text);
    if (!is_override && pp_builder_is_base_symbol(builder, symbol)) {
        *error = "redefining a base name requires (override ...)";
        return 0;
    }
    /* An optional `extras: none` modifier between the name and the body disables
    ** implicit trivia/skip/error consumption for the whole rule subtree, so a
    ** token-like rule (a template string, say) is not split open by comments or
    ** whitespace it should hold literally. `extras: none` is the only value the
    ** keyword accepts in V1. */
    if (item->count >= 4 && item->items[2]->kind == PP_FORM_KEYWORD &&
        pp_string_equal_c(item->items[2]->text, "extras")) {
        if (item->items[3]->kind != PP_FORM_SYMBOL ||
            (!pp_string_equal_c(item->items[3]->text, "none") &&
             !pp_string_equal_c(item->items[3]->text, "all"))) {
            *error = "extras: on a rule accepts only `none` or `all`";
            return 0;
        }
        /* `none` suppresses extras for the subtree; `all` re-enables them, so a
        ** rule reached inside an `extras: none` scope (the expression in a
        ** template substitution) gets ordinary whitespace handling back. */
        no_extras = pp_string_equal_c(item->items[3]->text, "none") ? 1 : 2;
        body_index = 4;
    }
    if (body_index >= item->count) {
        *error = "rule has no body";
        return 0;
    }
    body = pp_compile_rule_body(builder->engine, grammar->arena,
                                &grammar->interner, item->items[body_index],
                                error, &err_byte);
    if (body == 0) {
        if ((*error)[0] == 0) *error = "bad rule";
        return 0;
    }
    if (no_extras) {
        pp_ir *wrap = (pp_ir *)pp_alloc(grammar->arena, sizeof *wrap);
        uint32_t z;
        if (wrap == 0) { *error = "out of memory"; return 0; }
        wrap->op = PP_OP_NO_EXTRAS;
        wrap->text = pp_text("");
        wrap->symbol = PP_NO_SYMBOL;
        /* min carries the mode: 0 = disable extras, 1 = re-enable them. */
        wrap->min = (no_extras == 2) ? 1u : 0u;
        wrap->max = 0;
        wrap->cls.negated = 0;
        for (z = 0; z < 32; z++) wrap->cls.bits[z] = 0;
        wrap->children = (pp_ir **)pp_alloc(grammar->arena, sizeof(pp_ir *));
        if (wrap->children == 0) { *error = "out of memory"; return 0; }
        wrap->children[0] = body;
        wrap->child_count = 1;
        wrap->recover_message = pp_text("");
        wrap->recover_help = pp_text("");
        wrap->recover_label = pp_text("");
        wrap->recover_insert = pp_text("");
        body = wrap;
    }
    /* An override of a base rule replaces that base definition in place; a fresh
    ** rule (or an override naming a rule not yet present) is appended. */
    if (is_override && pp_builder_replace_rule(builder, symbol, body, rule_flags))
        return 1;
    if (!pp_builder_add_rule(builder, symbol, body, rule_flags)) {
        *error = "out of memory";
        return 0;
    }
    return 1;
}

/* ** Compile one (override (rule NAME ...)) / (override (token NAME ...)) form: a
** sanctioned redefinition of a base name. The single wrapped form must be a rule
** or token; it is compiled with override semantics, so it replaces the base
** definition rather than colliding with it. */
static int pp_compile_override_form(pp_grammar_builder *builder,
                                    const pp_form *item, const char **error)
{
    const pp_form *inner;

    if (item->count != 2 || item->items[1]->kind != PP_FORM_LIST ||
        item->items[1]->count < 1 ||
        item->items[1]->items[0]->kind != PP_FORM_SYMBOL) {
        *error = "override expects one (rule ...) or (token ...) form";
        return 0;
    }
    inner = item->items[1];
    if (pp_string_equal_c(inner->items[0]->text, "rule"))
        return pp_compile_rule_form(builder, inner, 1, 0, error);
    if (pp_string_equal_c(inner->items[0]->text, "token"))
        return pp_compile_token_form(builder, inner, 1, error);
    *error = "override expects a (rule ...) or (token ...) form";
    return 0;
}

/* ** Compile a (hidden (rule NAME ...)) / (hidden (override (rule NAME ...)))
** form (W17-FLAT). The HIDDEN marker is pocketparse's equivalent of tree-sitter's
** leading-underscore rule-name convention: the rule's node is omitted from the
** CST when it is a pure pass-through (it contributed exactly one child and no
** alias), with that child promoted to the parent. The marker wraps a rule form so
** the rule keeps its descriptive name (the name still shows in the non-pass-through
** case, e.g. a precedence-ladder rule that DID match an operator), unlike the
** leading-underscore convention which would bury the name. The collapse decision
** itself lives in the shared node-close (pp_vm_exit / pp_vm_call_exit). */
static int pp_compile_hidden_form(pp_grammar_builder *builder,
                                  const pp_form *item, const char **error)
{
    const pp_form *inner;
    if (item->count != 2 || item->items[1]->kind != PP_FORM_LIST ||
        item->items[1]->count < 1 ||
        item->items[1]->items[0]->kind != PP_FORM_SYMBOL) {
        *error = "hidden expects one (rule ...) or (override (rule ...)) form";
        return 0;
    }
    inner = item->items[1];
    /* (hidden (rule NAME ...)): mark the rule PP_RULE_HIDDEN as it is compiled. */
    if (pp_string_equal_c(inner->items[0]->text, "rule"))
        return pp_compile_rule_form(builder, inner, 0, PP_RULE_HIDDEN, error);
    /* (hidden (override (rule NAME ...))): a hidden redefinition of a base rule;
    ** the wrapped form must be an override of a rule (not a token). */
    if (pp_string_equal_c(inner->items[0]->text, "override")) {
        const pp_form *ov;
        if (inner->count != 2 || inner->items[1]->kind != PP_FORM_LIST ||
            inner->items[1]->count < 1 ||
            inner->items[1]->items[0]->kind != PP_FORM_SYMBOL ||
            !pp_string_equal_c(inner->items[1]->items[0]->text, "rule")) {
            *error = "hidden override expects an (override (rule ...)) form";
            return 0;
        }
        ov = inner->items[1];
        return pp_compile_rule_form(builder, ov, 1, PP_RULE_HIDDEN, error);
    }
    *error = "hidden expects a (rule ...) or (override (rule ...)) form";
    return 0;
}

/* ** Forward declaration: compiling an (import NAME) form re-enters the
** per-source compiler on the resolved module's bytes. */
static int pp_compile_source(pp_grammar_builder *builder,
                             pocketparse_engine *engine,
                             pocketparse_source source, int is_root,
                             const char **error);

/* ** Resolve and compile an (import NAME) / (import a.b as: alias) form, merging
** the module's tokens, rules, and extras into the grammar under construction. The
** core has no filesystem: it asks the engine's module resolver for the module's
** bytes, then compiles those bytes as a (non-root) source. A missing resolver, a
** resolver failure, and an import cycle each fail with a clear message. The `as:`
** alias is accepted and ignored for now -- merged names already live in one flat
** namespace, so an alias changes nothing; the (import a.b as: js) shape parses so
** grammars can be written forward-compatibly. */
static int pp_compile_import_form(pp_grammar_builder *builder,
                                  pocketparse_engine *engine,
                                  const pp_form *item, const char **error)
{
    pocketparse_string  name;
    pocketparse_source  module_source;
    pocketparse_result  resolved;
    uint32_t            v;

    if (item->count < 2 || item->items[1]->kind != PP_FORM_SYMBOL) {
        *error = "import needs a module name";
        return 0;
    }
    name = item->items[1]->text;

    /* Guard against import cycles: a name already on the stack of sources being
    ** compiled must not be re-entered. */
    for (v = 0; v < builder->visited_count; v++)
        if (pp_string_equal(builder->visited[v], name)) {
            *error = "import cycle detected";
            return 0;
        }

    if (engine->module_resolve == 0) {
        *error = "import requires a module resolver";
        return 0;
    }

    module_source.name  = name;
    module_source.bytes = pp_text("");
    resolved = engine->module_resolve(engine->module_user, name, &module_source);
    if (resolved != POCKETPARSE_OK) {
        *error = "module resolver could not load an imported module";
        return 0;
    }

    /* Push the name onto the visited stack, compile the module, then pop it. */
    if (builder->visited_count == builder->visited_capacity) {
        uint32_t new_cap = builder->visited_capacity == 0
                               ? 8 : builder->visited_capacity * 2;
        pocketparse_string *grown = (pocketparse_string *)pp_alloc(
            engine->arena, new_cap * sizeof *grown);
        uint32_t i;
        if (grown == 0) { *error = "out of memory"; return 0; }
        for (i = 0; i < builder->visited_count; i++)
            grown[i] = builder->visited[i];
        builder->visited          = grown;
        builder->visited_capacity = new_cap;
    }
    builder->visited[builder->visited_count++] = name;

    if (!pp_compile_source(builder, engine, module_source, 0, error))
        return 0;

    builder->visited_count--;
    return 1;
}

/* ** Resolve and compile an (extends NAME) form: pull a base grammar into the
** grammar under construction, the same compile-time merge (import) uses, then
** record every token and rule the base contributed as a base symbol. Those marks
** make the root source's own redefinition of a base name a collision unless it is
** wrapped in (override ...). The base is resolved through the same host module
** resolver imports use; a missing resolver or a resolver failure fails with a
** clear message. The base's start: is ignored -- the extending grammar's own
** start: wins (it is compiled is_root). */
static int pp_compile_extends_form(pp_grammar_builder *builder,
                                   pocketparse_engine *engine,
                                   const pp_form *item, const char **error)
{
    pocketparse_string  name;
    pocketparse_source  base_source;
    pocketparse_result  resolved;
    uint32_t            v;
    uint32_t            t;
    uint32_t            r;

    if (item->count < 2 || item->items[1]->kind != PP_FORM_SYMBOL) {
        *error = "extends needs a base grammar name";
        return 0;
    }
    name = item->items[1]->text;

    /* The same cycle guard imports use: a base already on the compile stack must
    ** not be re-entered. */
    for (v = 0; v < builder->visited_count; v++)
        if (pp_string_equal(builder->visited[v], name)) {
            *error = "extends cycle detected";
            return 0;
        }

    if (engine->module_resolve == 0) {
        *error = "extends requires a module resolver";
        return 0;
    }

    base_source.name  = name;
    base_source.bytes = pp_text("");
    resolved = engine->module_resolve(engine->module_user, name, &base_source);
    if (resolved != POCKETPARSE_OK) {
        *error = "module resolver could not load the base grammar";
        return 0;
    }

    if (builder->visited_count == builder->visited_capacity) {
        uint32_t new_cap = builder->visited_capacity == 0
                               ? 8 : builder->visited_capacity * 2;
        pocketparse_string *grown = (pocketparse_string *)pp_alloc(
            engine->arena, new_cap * sizeof *grown);
        uint32_t i;
        if (grown == 0) { *error = "out of memory"; return 0; }
        for (i = 0; i < builder->visited_count; i++)
            grown[i] = builder->visited[i];
        builder->visited          = grown;
        builder->visited_capacity = new_cap;
    }
    builder->visited[builder->visited_count++] = name;

    if (!pp_compile_source(builder, engine, base_source, 0, error))
        return 0;

    builder->visited_count--;

    /* Every token and rule now present came from the base (extends is resolved
    ** before the extending grammar's own body is walked), so mark them all as
    ** base symbols for the collision check. */
    for (t = 0; t < builder->grammar->token_count; t++)
        if (!pp_builder_mark_base(builder, builder->grammar->tokens[t].symbol)) {
            *error = "out of memory";
            return 0;
        }
    for (r = 0; r < builder->grammar->rule_count; r++)
        if (!pp_builder_mark_base(builder, builder->grammar->rules[r].symbol)) {
            *error = "out of memory";
            return 0;
        }
    return 1;
}

/* ** Compile one .scm source into the grammar under construction. The root
** source supplies the start: rule and may import modules; an imported module
** contributes its tokens, rules, and extras (its own start:, module, and
** pocketparse forms are ignored). Top-level (import NAME) forms are resolved
** before the (grammar ...) body is walked, so a module's definitions are present
** by the time the body references them. */
static int pp_compile_source(pp_grammar_builder *builder,
                             pocketparse_engine *engine,
                             pocketparse_source source, int is_root,
                             const char **error)
{
    pp_read_result       read;
    pocketparse_grammar *grammar = builder->grammar;
    const pp_form       *grammar_form = 0;
    uint32_t             i;

    read = pp_read(engine->arena, source.bytes);
    if (!read.ok) {
        *error = read.error;
        return 0;
    }

    /* Resolve every top-level (import NAME) and (extends NAME) first, so the
    ** imported/base names are defined before the body that references them is
    ** compiled. extends additionally records base names so a later bare
    ** redefinition collides; the base's start: is ignored. */
    for (i = 0; i < read.count; i++) {
        const pp_form *form = read.forms[i];
        if (form->kind == PP_FORM_LIST && form->count >= 1 &&
            form->items[0]->kind == PP_FORM_SYMBOL) {
            if (pp_string_equal_c(form->items[0]->text, "import")) {
                if (!pp_compile_import_form(builder, engine, form, error))
                    return 0;
            } else if (pp_string_equal_c(form->items[0]->text, "extends")) {
                if (!pp_compile_extends_form(builder, engine, form, error))
                    return 0;
            }
        }
    }

    /* Locate the (grammar NAME ...) form among the top-level forms. */
    for (i = 0; i < read.count; i++) {
        const pp_form *form = read.forms[i];
        if (form->kind == PP_FORM_LIST && form->count >= 1 &&
            form->items[0]->kind == PP_FORM_SYMBOL &&
            pp_string_equal_c(form->items[0]->text, "grammar")) {
            grammar_form = form;
            break;
        }
    }
    if (grammar_form == 0) {
        *error = "grammar form not found";
        return 0;
    }
    if (grammar_form->count < 2 ||
        grammar_form->items[1]->kind != PP_FORM_SYMBOL) {
        *error = "grammar needs a name";
        return 0;
    }

    /* W17-3 record the root grammar's name (e.g. "json", "javascript") so the
    ** emitter can default the symbol prefix from it. Only the root governs the
    ** prefix; a merged module's (grammar NAME) does not rename the composite. */
    if (is_root)
        grammar->name_symbol =
            pp_intern(&grammar->interner, grammar_form->items[1]->text);

    /* Walk the grammar body: start: (root only), token, extras, rule. */
    for (i = 1; i < grammar_form->count; i++) {
        const pp_form *item = grammar_form->items[i];

        if (item->kind == PP_FORM_KEYWORD) {
            if (pp_string_equal_c(item->text, "start")) {
                if (i + 1 >= grammar_form->count ||
                    grammar_form->items[i + 1]->kind != PP_FORM_SYMBOL) {
                    *error = "start: needs a rule name";
                    return 0;
                }
                /* Only the root grammar's start: rule governs the parse; a
                ** module's start: is ignored when it is merged. */
                if (is_root)
                    grammar->start_symbol = pp_intern(
                        &grammar->interner, grammar_form->items[i + 1]->text);
                i++;
            }
            continue;
        }

        if (item->kind != PP_FORM_LIST || item->count < 1 ||
            item->items[0]->kind != PP_FORM_SYMBOL)
            continue;

        if (pp_string_equal_c(item->items[0]->text, "token")) {
            if (!pp_compile_token_form(builder, item, 0, error))
                return 0;
        } else if (pp_string_equal_c(item->items[0]->text, "rule")) {
            if (!pp_compile_rule_form(builder, item, 0, 0, error))
                return 0;
        } else if (pp_string_equal_c(item->items[0]->text, "hidden")) {
            if (!pp_compile_hidden_form(builder, item, error))
                return 0;
        } else if (pp_string_equal_c(item->items[0]->text, "override")) {
            if (!pp_compile_override_form(builder, item, error))
                return 0;
        } else if (pp_string_equal_c(item->items[0]->text, "extras")) {
            if (!pp_compile_extras(builder, item, error))
                return 0;
        } else if (pp_string_equal_c(item->items[0]->text, "lex-goal")) {
            if (!pp_compile_lex_goal(builder, item, error))
                return 0;
        }
    }

    return 1;
}

/* ** Maximum distinct deferred-slot declarations counted per rule body. A rule
** declares at most a handful of slots; this bound is generous. */
#define PP_SLOT_SEEN_MAX 64

/* ** Recursively collect the DISTINCT interned slot names declared by
** PP_OP_DEFERRED_SLOT nodes in `ir` into `seen` (a fixed buffer of *seen_count
** entries). Distinctness is by interned symbol id. Returns 1, or 0 if the buffer
** overflowed (treated as a compile error by the caller; practically unreachable).
** (lex-goal-implementation, "Per-rule slot count".) */
static int pp_collect_slot_decls(const pp_ir *ir, uint32_t *seen,
                                 uint32_t *seen_count)
{
    uint32_t i;
    if (ir == 0)
        return 1;
    if (ir->op == PP_OP_DEFERRED_SLOT) {
        int present = 0;
        for (i = 0; i < *seen_count; i++)
            if (seen[i] == ir->symbol) { present = 1; break; }
        if (!present) {
            if (*seen_count >= PP_SLOT_SEEN_MAX)
                return 0;
            seen[(*seen_count)++] = ir->symbol;
        }
    }
    for (i = 0; i < ir->child_count; i++)
        if (!pp_collect_slot_decls(ir->children[i], seen, seen_count))
            return 0;
    return 1;
}

/* ** Is `slot_symbol` declared by a PP_OP_DEFERRED_SLOT anywhere in `ir`? */
static int pp_slot_declared_in(const pp_ir *ir, uint32_t slot_symbol)
{
    uint32_t i;
    if (ir == 0)
        return 0;
    if (ir->op == PP_OP_DEFERRED_SLOT && ir->symbol == slot_symbol)
        return 1;
    for (i = 0; i < ir->child_count; i++)
        if (pp_slot_declared_in(ir->children[i], slot_symbol))
            return 1;
    return 0;
}

/* ** Is `slot_symbol` declared by a PP_OP_DEFERRED_SLOT anywhere in the grammar
** (any rule body)? This is a global-existence check, not a true lexical-scope
** check: full enclosing-scope verification is a call-graph-level analysis, and
** the runtime frame-stack walk in a later phase is the real enforcement. This
** check catches typos, which is the MUST intent (lex-goal-implementation,
** "Compile-time scope validation"). */
static int pp_slot_declared_anywhere(const pocketparse_grammar *grammar,
                                     uint32_t slot_symbol)
{
    uint32_t r;
    for (r = 0; r < grammar->rule_count; r++)
        if (pp_slot_declared_in(grammar->rules[r].body, slot_symbol))
            return 1;
    return 0;
}

/* ** The compact parametrized-goal index of the lex_goal entry at raw position
** `raw' (the count of parametrized entries before it), or PP_NO_INDEX if that
** entry is not parametrized. */
static uint32_t pp_param_goal_compact_index(const pocketparse_grammar *grammar,
                                            uint32_t raw)
{
    uint32_t i, compact = 0;
    if (raw >= grammar->lex_goal_count || !grammar->lex_goals[raw].is_parametrized)
        return PP_NO_INDEX;
    for (i = 0; i < raw; i++)
        if (grammar->lex_goals[i].is_parametrized)
            compact++;
    return compact;
}

/* ** Recursively scan `ir' for PP_OP_GOAL nodes; for each that names a
** parametrized lex_goal entry, fold its compact index into *found. *found starts
** at PP_NO_INDEX (none seen). If two DISTINCT parametrized compact indices are
** reachable, sets *conflict and returns. Descends into ALL children. */
static void pp_collect_reachable_param_goals(const pocketparse_grammar *grammar,
                                             const pp_ir *ir,
                                             uint32_t *found, int *conflict)
{
    uint32_t i;
    if (ir == 0 || *conflict)
        return;
    if (ir->op == PP_OP_GOAL) {
        uint32_t g;
        for (g = 0; g < grammar->lex_goal_count; g++) {
            if (grammar->lex_goals[g].is_parametrized &&
                grammar->lex_goals[g].goal_symbol == ir->symbol) {
                uint32_t compact = pp_param_goal_compact_index(grammar, g);
                if (*found == PP_NO_INDEX)
                    *found = compact;
                else if (*found != compact) {
                    *conflict = 1;
                    return;
                }
            }
        }
    }
    for (i = 0; i < ir->child_count; i++) {
        pp_collect_reachable_param_goals(grammar, ir->children[i], found,
                                         conflict);
        if (*conflict)
            return;
    }
}

/* ** Resolve every PP_OP_PARSE_DEFERRED node in `ir': the stashed rule name (in
** ir->text) becomes ir->min (the target rule index), and the single reachable
** parametrized goal index becomes ir->max (or PP_NO_INDEX if none). Returns 1, or
** 0 with *error set (undefined rule, or conflicting parametrized goals).
** (lex-goal-implementation, "Resolve PP_OP_PARSE_DEFERRED".) */
static int pp_resolve_parse_deferred(pocketparse_grammar *grammar, pp_ir *ir,
                                     const char **error)
{
    uint32_t i;
    if (ir == 0)
        return 1;
    if (ir->op == PP_OP_PARSE_DEFERRED) {
        uint32_t rule_symbol = pp_intern(&grammar->interner, ir->text);
        uint32_t rule_index  = PP_NO_INDEX;
        uint32_t found       = PP_NO_INDEX;
        int      conflict    = 0;
        for (i = 0; i < grammar->rule_count; i++)
            if (grammar->rules[i].symbol == rule_symbol) {
                rule_index = i;
                break;
            }
        if (rule_index == PP_NO_INDEX) {
            *error = "parse-deferred names an undefined rule";
            return 0;
        }
        ir->min = rule_index;
        pp_collect_reachable_param_goals(grammar, grammar->rules[rule_index].body,
                                         &found, &conflict);
        if (conflict) {
            *error = "parse-deferred reaches conflicting parametrized goals";
            return 0;
        }
        /* found == PP_NO_INDEX means no parametrized goal is reachable: the drain
        ** will simply never push a pattern. This is advisory, not an error. */
        ir->max = found;
    }
    for (i = 0; i < ir->child_count; i++)
        if (!pp_resolve_parse_deferred(grammar, ir->children[i], error))
            return 0;
    return 1;
}

/* ** Validate every PP_OP_DEFER and PP_OP_PARSE_DEFERRED in `ir': its slot name
** must be declared by a PP_OP_DEFERRED_SLOT somewhere in the grammar. Returns 1,
** or 0 with *error set on the first undeclared slot. */
static int pp_validate_defer_slots(const pocketparse_grammar *grammar,
                                   const pp_ir *ir, const char **error)
{
    uint32_t i;
    if (ir == 0)
        return 1;
    if ((ir->op == PP_OP_DEFER || ir->op == PP_OP_PARSE_DEFERRED) &&
        !pp_slot_declared_anywhere(grammar, ir->symbol)) {
        *error = "defer/parse-deferred references an undeclared slot";
        return 0;
    }
    for (i = 0; i < ir->child_count; i++)
        if (!pp_validate_defer_slots(grammar, ir->children[i], error))
            return 0;
    return 1;
}

/* ** The deferred-slot post-compile pass: compact parametrized goal indices,
** count per-rule slots, resolve every PP_OP_PARSE_DEFERRED, then validate that
** every defer/parse-deferred slot is declared. Run after pp_resolve_refs and
** before the lex-goal trigger/target validation. Returns POCKETPARSE_OK, or sets
** the engine fault and returns POCKETPARSE_INVALID on the first error.
**
** TODO(lex-goal): SHOULD-warnings (a declared-but-never-drained slot; a (param
** ...) entry reachable outside parse-deferred; parse-deferred in a speculative or
** non-final repeat context) need a compile diagnostic channel. The freestanding
** library has only the fatal *error path, so they are deferred; only the
** MUST-level errors are enforced here. */
static pocketparse_result pp_resolve_deferred_slots(pocketparse_engine *engine,
                                                    pocketparse_grammar *grammar)
{
    const char *error = "";
    uint32_t    i;

    /* 1. Compact param indices + count. */
    grammar->param_goal_count = 0;
    for (i = 0; i < grammar->lex_goal_count; i++)
        if (grammar->lex_goals[i].is_parametrized)
            grammar->param_goal_count++;

    /* 2. Per-rule slot_count: distinct PP_OP_DEFERRED_SLOT names in each body. */
    for (i = 0; i < grammar->rule_count; i++) {
        uint32_t seen[PP_SLOT_SEEN_MAX];
        uint32_t seen_count = 0;
        if (!pp_collect_slot_decls(grammar->rules[i].body, seen, &seen_count))
            return pp_grammar_fail(engine,
                "too many distinct deferred-slot declarations in one rule");
        grammar->rules[i].slot_count = seen_count;
    }

    /* 3. Resolve PP_OP_PARSE_DEFERRED rule_index + goal_entry_index. */
    for (i = 0; i < grammar->rule_count; i++)
        if (!pp_resolve_parse_deferred(grammar, grammar->rules[i].body, &error))
            return pp_grammar_fail(engine, error);

    /* 4. Dangling-slot validation (MUST). */
    for (i = 0; i < grammar->rule_count; i++)
        if (!pp_validate_defer_slots(grammar, grammar->rules[i].body, &error))
            return pp_grammar_fail(engine, error);

    return POCKETPARSE_OK;
}

/* ** Compile a .scm grammar source -- and every module it imports -- into a
** single grammar handle. The root source is compiled first; its (import NAME)
** forms are resolved through the engine's module resolver and merged in. */
pocketparse_result pocketparse_compile_scheme(
    pocketparse_engine  *engine,
    pocketparse_source   source,
    pocketparse_grammar **out_grammar)
{
    pocketparse_grammar *grammar;
    pp_grammar_builder   builder;
    const char          *error = "";
    uint32_t             i;

    if (out_grammar != 0)
        *out_grammar = 0;
    if (engine == 0)
        return POCKETPARSE_INVALID;

    grammar = pp_grammar_new(engine);
    if (grammar == 0)
        return pp_grammar_fail(engine, "out of memory compiling grammar");

    builder.engine           = engine;
    builder.grammar          = grammar;
    builder.token_capacity   = 0;
    builder.rule_capacity    = 0;
    builder.extra_capacity   = 0;
    builder.lex_goal_capacity = 0;
    builder.visited          = 0;
    builder.visited_count    = 0;
    builder.visited_capacity = 0;
    builder.base_symbols         = 0;
    builder.base_symbol_count    = 0;
    builder.base_symbol_capacity = 0;
    builder.param_template_counter = 0;

    if (!pp_compile_source(&builder, engine, source, 1, &error))
        return pp_grammar_fail(engine, error[0] ? error : "bad grammar");

    /* Validate the start rule exists. */
    if (grammar->start_symbol == PP_NO_SYMBOL)
        return pp_grammar_fail(engine, "grammar has no start: rule");
    {
        int found = 0;
        for (i = 0; i < grammar->rule_count; i++)
            if (grammar->rules[i].symbol == grammar->start_symbol) {
                found = 1;
                break;
            }
        if (!found)
            return pp_grammar_fail(engine, "start: names a missing rule");
    }

    /* Validate every reference resolves (across the merged tables). */
    for (i = 0; i < grammar->rule_count; i++)
        if (!pp_resolve_refs(grammar, grammar->rules[i].body, &error))
            return pp_grammar_fail(engine, error);

    /* Deferred-slot post-pass: compact parametrized goal indices, count per-rule
    ** slots, resolve PP_OP_PARSE_DEFERRED, validate slot declarations. Run after
    ** refs resolve (so parse-deferred can find its rule) and before the lex-goal
    ** trigger/target validation (which now skips the target for a parametrized
    ** entry). */
    {
        pocketparse_result deferred = pp_resolve_deferred_slots(engine, grammar);
        if (deferred != POCKETPARSE_OK)
            return deferred;
    }

    /* Validate every lex-goal trigger names a defined token: a static goal
    ** redirects one token's scan to another, so both trigger and target must be
    ** real tokens. A PARAMETRIZED entry has no static target (the union word holds
    ** a template rule index, not a token symbol), so only its trigger is checked. */
    for (i = 0; i < grammar->lex_goal_count; i++) {
        uint32_t t;
        int trigger_ok = 0, target_ok = 0;
        int parametrized = grammar->lex_goals[i].is_parametrized != 0;
        for (t = 0; t < grammar->token_count; t++) {
            if (grammar->tokens[t].symbol == grammar->lex_goals[i].trigger_symbol)
                trigger_ok = 1;
            if (!parametrized &&
                grammar->tokens[t].symbol == grammar->lex_goals[i].target_symbol)
                target_ok = 1;
        }
        if (parametrized)
            target_ok = 1; /* no static target to validate */
        if (!trigger_ok || !target_ok)
            return pp_grammar_fail(engine,
                "lex-goal trigger and target must name defined tokens");
    }

    /* W17-RECLAIM: with every reference resolved, mark the dispatch-spine HIDDEN
    ** rules whose body always yields one child as TRANSPARENT_ALWAYS so the entry
    ** path skips their wrapper node. The flag rides the packed flags field. */
    pp_grammar_mark_transparent_rules(grammar);

    engine->fault = pp_text("");
    if (out_grammar != 0)
        *out_grammar = grammar;
    return POCKETPARSE_OK;
}

/* ** Resolve a grammar NAME named by an (inject grammar: NAME ...) form and
** compile it into its own grammar handle (W62 language injection). NAME is
** resolved through the engine's module resolver -- the same seam (import NAME)
** uses -- so the core never reads the filesystem; the CLI's resolver falls back to
** the embedded registry, which is what lets `inject grammar: std.javascript` find
** the built-in standard grammar with no files at all. The resulting grammar is a
** wholly separate handle (its own interner, tokens, rules, start rule), compiled
** once at host-grammar compile time and reused for every span the inject node
** parses. Returns 1 and writes *out_grammar on success; 0 with *error set if no
** resolver is installed, the resolver fails, or the embedded source is malformed. */
int pp_resolve_embedded_grammar(pocketparse_engine *engine,
                                pocketparse_string name,
                                pocketparse_grammar **out_grammar,
                                const char **error)
{
    pocketparse_source  embedded_source;
    pocketparse_result  resolved;
    pocketparse_result  compiled;

    *out_grammar = 0;
    if (engine->module_resolve == 0) {
        *error = "inject requires a module resolver to find the embedded grammar";
        return 0;
    }
    embedded_source.name  = name;
    embedded_source.bytes = pp_text("");
    resolved = engine->module_resolve(engine->module_user, name,
                                      &embedded_source);
    if (resolved != POCKETPARSE_OK) {
        *error = "inject could not resolve the embedded grammar";
        return 0;
    }
    /* Compile the embedded source into its own grammar. pocketparse_compile_scheme
    ** sets engine->fault on failure; map that to the inject error so the host
    ** compile reports a clear message. The embedded compile uses the same module
    ** resolver, so an embedded grammar may itself (import ...) or (inject ...). */
    compiled = pocketparse_compile_scheme(engine, embedded_source, out_grammar);
    if (compiled != POCKETPARSE_OK || *out_grammar == 0) {
        *error = "inject could not compile the embedded grammar";
        return 0;
    }
    return 1;
}

/* ** Release a grammar. Its storage is in the engine arena, freed with the
** engine; nothing to do per-grammar. */
void pocketparse_grammar_free(pocketparse_grammar *grammar)
{
    (void)grammar;
}

/* ** pocketparse_load_binary_grammar lives in pack.c, alongside its mirror
** pocketparse_grammar_serialize and the rest of the binary grammar format. */
