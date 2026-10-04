/*
** vmcompile.c -- lower the rule/token IR to a flat bytecode program (W59).
**
** The recursive-descent executor in parse.c walks the tp_ir tree directly. This
** pass lowers that same tree, once per parse, to the flat tp_instruction array
** the bytecode VM (vm.c) dispatches over. The lowering is STRUCTURAL: every IR
** node becomes exactly one instruction whose children -- where it has any -- are
** themselves lowered programs named by index, gathered in a side table. There is
** no inlining and no reordering, so the instruction stream is a faithful image of
** the IR tree and the VM can reproduce tp_parse's control flow (and therefore its
** concrete syntax tree) instruction for instruction.
**
** A "program" is the index of the single instruction at which an IR node's
** execution begins. tp_compile_program lowers the start rule's body and, because
** rule references lower to TP_VM_CALL of the referenced rule's lowered body,
** every reachable rule is lowered on demand and memoised so each rule lowers
** once. Token references and literals lower to TP_VM_MATCH, which calls the same
** token matcher the recursive descent uses; the byte-level operators used
** directly in a rule body lower to TP_VM_BYTE, the anonymous-terminal case.
**
** Freestanding throughout: every allocation comes from the parse arena.
*/
#include "internal.h"

/* ** A memo entry pairing a rule symbol with the program its body lowered to, so
** a rule referenced many times (and a recursive rule) lowers exactly once. */
typedef struct tp_rule_program {
    uint32_t symbol;
    uint32_t program;     /* TP_NO_INDEX while the rule is mid-lowering (cycle) */
    uint32_t call_target; /* the CALL-able entry: opens the node, runs the body */
} tp_rule_program;

/* ** The lowering's working state: the program under construction plus the
** rule-program memo. Threaded through the recursive lowering so a rule reference
** can find or create the referenced rule's entry without re-lowering it. */
typedef struct tp_lower {
    tp_program      *program;
    const tinyparse_grammar *grammar;
    tp_rule_program *rules;
    uint32_t         rule_count;
    uint32_t         rule_capacity;
    int              ok;
} tp_lower;

/* ** Append one instruction to the program, returning its index (the program that
** begins at it), or TP_NO_INDEX on allocation failure. Grows geometrically;
** arenas never realloc, so growth copies into a fresh, larger block. */
static uint32_t tp_emit(tp_lower *lower, tp_vm_op op)
{
    tp_program *program = lower->program;
    tp_instruction *slot;
    uint32_t index;
    if (program->instruction_count == program->instruction_capacity) {
        uint32_t nc = program->instruction_capacity == 0
                          ? 64 : program->instruction_capacity * 2;
        tp_instruction *grown = (tp_instruction *)tp_alloc(
            program->arena, nc * sizeof *grown);
        uint32_t i;
        if (grown == 0) { lower->ok = 0; return TP_NO_INDEX; }
        for (i = 0; i < program->instruction_count; i++)
            grown[i] = program->instructions[i];
        program->instructions         = grown;
        program->instruction_capacity = nc;
    }
    index = program->instruction_count++;
    slot = &program->instructions[index];
    slot->op      = op;
    slot->pattern = 0;
    slot->source  = 0;
    slot->symbol  = TP_NO_SYMBOL;
    slot->target  = TP_NO_INDEX;
    slot->start   = 0;
    slot->count   = 0;
    slot->min     = 0;
    slot->max     = 0;
    slot->aux     = 0;
    slot->tail    = 0;
    slot->slot_count = 0;
    return index;
}

/* ** Reserve `count' contiguous slots in the child-program table and return the
** index of the first, or TP_NO_INDEX on failure. The caller fills them in. */
static uint32_t tp_reserve_children(tp_lower *lower, uint32_t count)
{
    tp_program *program = lower->program;
    uint32_t first;
    if (count == 0)
        return program->child_count; /* empty slice: a valid, unused start */
    if (program->child_count + count > program->child_capacity) {
        uint32_t nc = program->child_capacity == 0 ? 64 : program->child_capacity;
        uint32_t *grown;
        uint32_t i;
        while (program->child_count + count > nc)
            nc *= 2;
        grown = (uint32_t *)tp_alloc(program->arena, nc * sizeof *grown);
        if (grown == 0) { lower->ok = 0; return TP_NO_INDEX; }
        for (i = 0; i < program->child_count; i++)
            grown[i] = program->children[i];
        program->children      = grown;
        program->child_capacity = nc;
    }
    first = program->child_count;
    program->child_count += count;
    return first;
}

static uint32_t tp_lower_ir(tp_lower *lower, const tp_ir *ir);
static uint32_t tp_lower_ir_inner(tp_lower *lower, const tp_ir *ir);

/* ** Lower one IR node and tag the resulting instruction with the IR node it came
** from, so choice prediction (W46) can resolve the alternative's FIRST set through
** the SAME node tp_choice_first_admits caches on -- the cache is then shared with
** the recursive descent. A thin wrapper over tp_lower_ir_inner keeps the per-op
** lowering uncluttered. */
static uint32_t tp_lower_ir(tp_lower *lower, const tp_ir *ir)
{
    uint32_t program = tp_lower_ir_inner(lower, ir);
    if (program != TP_NO_INDEX && lower->ok)
        lower->program->instructions[program].source = (tp_ir *)ir;
    return program;
}

/* ** Return 1 if any node in this subtree is a rule reference (TP_OP_REF),
** descending into every child. Used to decide a `(not EXPR0 EXPR1)' lowering:
** when EXPR1 (child[1]) holds a rule reference anywhere -- directly or nested in
** a composite -- the NOT body cannot be matched as a token-level byte pattern and
** must lower to TP_VM_NOT_RULE, which runs child[1] through the rule-call
** machinery. A subtree with no TP_OP_REF is purely token-level and keeps the
** existing TP_VM_BYTE path. */
static int tp_ir_subtree_has_ref(const tp_ir *ir)
{
    uint32_t i;
    if (ir == 0)
        return 0;
    if (ir->op == TP_OP_REF)
        return 1;
    for (i = 0; i < ir->child_count; i++)
        if (tp_ir_subtree_has_ref(ir->children[i]))
            return 1;
    return 0;
}

/* ** Find an existing rule-program memo entry by symbol, or NULL. */
static tp_rule_program *tp_lower_find_rule(tp_lower *lower, uint32_t symbol)
{
    uint32_t i;
    for (i = 0; i < lower->rule_count; i++)
        if (lower->rules[i].symbol == symbol)
            return &lower->rules[i];
    return 0;
}

/* ** Find a token definition by symbol, or NULL. */
static const tp_token_def *tp_lower_find_token(const tinyparse_grammar *grammar,
                                               uint32_t symbol)
{
    uint32_t i;
    for (i = 0; i < grammar->token_count; i++)
        if (grammar->tokens[i].symbol == symbol)
            return &grammar->tokens[i];
    return 0;
}

/* ** Find a rule definition by symbol, or NULL. */
static const tp_rule_def *tp_lower_find_rule_def(const tinyparse_grammar *grammar,
                                                 uint32_t symbol)
{
    uint32_t i;
    for (i = 0; i < grammar->rule_count; i++)
        if (grammar->rules[i].symbol == symbol)
            return &grammar->rules[i];
    return 0;
}

/* ** Lower a rule's CALL entry, lowering its body the first time and memoising the
** result so a recursive or repeated reference reuses it. Returns the program that,
** when run, opens the rule's node, parses its body, links the children, and
** closes -- i.e. the TP_VM_CALL instruction's target. The memo entry is created
** BEFORE the body is lowered so a self-reference inside the body resolves to the
** same in-progress CALL target (the bytecode handles recursion through the return
** stack, exactly as the C recursion did). */
static uint32_t tp_lower_rule(tp_lower *lower, uint32_t symbol)
{
    tp_rule_program *entry = tp_lower_find_rule(lower, symbol);
    const tp_rule_def *def;
    uint32_t call_index;
    uint32_t body_program;

    if (entry != 0)
        return entry->call_target;

    def = tp_lower_find_rule_def(lower->grammar, symbol);
    if (def == 0)
        return TP_NO_INDEX;

    /* Reserve the CALL instruction up front and memoise it, so a self-reference
    ** encountered while lowering the body resolves to this same entry. */
    call_index = tp_emit(lower, TP_VM_CALL);
    if (call_index == TP_NO_INDEX)
        return TP_NO_INDEX;
    lower->program->instructions[call_index].symbol = symbol;
    /* Annotate this rule's canonical CALL with the rule's deferred-slot count so
    ** the dispatch loop allocates that many slot frames on descent
    ** (lex-goal-implementation, "Per-rule slot count"). */
    lower->program->instructions[call_index].slot_count = def->slot_count;

    if (lower->rule_count == lower->rule_capacity) {
        uint32_t nc = lower->rule_capacity == 0 ? 16 : lower->rule_capacity * 2;
        tp_rule_program *grown = (tp_rule_program *)tp_alloc(
            lower->program->arena, nc * sizeof *grown);
        uint32_t i;
        if (grown == 0) { lower->ok = 0; return TP_NO_INDEX; }
        for (i = 0; i < lower->rule_count; i++)
            grown[i] = lower->rules[i];
        lower->rules         = grown;
        lower->rule_capacity = nc;
    }
    entry = &lower->rules[lower->rule_count++];
    entry->symbol      = symbol;
    entry->program     = TP_NO_INDEX;
    entry->call_target = call_index;

    body_program = tp_lower_ir(lower, def->body);
    if (!lower->ok)
        return TP_NO_INDEX;
    /* The memo array may have moved while lowering the body; re-find the entry. */
    entry = tp_lower_find_rule(lower, symbol);
    entry->program = body_program;
    lower->program->instructions[call_index].target = body_program;
    return call_index;
}

/* ** Lower one IR node to a single instruction and return its program index. A
** combinator's children are lowered first and referenced by program index, so the
** instruction stream mirrors the IR tree without inlining. The switch parallels
** tp_parse's switch case for case, which is what makes the VM byte-identical. */
static uint32_t tp_lower_ir_inner(tp_lower *lower, const tp_ir *ir)
{
    switch (ir->op) {
    case TP_OP_REF: {
        /* A rule reference lowers to CALL of the rule's body; a token reference
        ** lowers to MATCH of the token's pattern. Goal redirects are resolved at
        ** dispatch time (the active goal is dynamic), exactly as tp_parse does, so
        ** the token reference carries its own pattern and the VM consults the
        ** goal table when it runs. Resolution order (rule first, then token)
        ** matches tp_parse's TP_OP_REF case. */
        const tp_rule_def  *rule  = tp_lower_find_rule_def(lower->grammar, ir->symbol);
        const tp_token_def *token;
        if (rule != 0) {
            /* Every reference to a rule resolves to the rule's single canonical
            ** CALL instruction (tp_lower_rule lowers the body once and memoises
            ** it). Sharing one CALL is sound -- it is read-only during dispatch --
            ** and it sidesteps the recursion hazard of copying a body target that
            ** is not yet known: a self-reference inside the body returns this same
            ** CALL, whose target tp_lower_rule fills in once the body is lowered. */
            return tp_lower_rule(lower, ir->symbol);
        }
        token = tp_lower_find_token(lower->grammar, ir->symbol);
        if (token != 0) {
            uint32_t m = tp_emit(lower, TP_VM_MATCH);
            if (m == TP_NO_INDEX) return TP_NO_INDEX;
            lower->program->instructions[m].pattern = token->pattern;
            lower->program->instructions[m].symbol  = token->symbol;
            return m;
        }
        /* Unresolved reference: an always-failing MATCH with a NULL pattern, which
        ** the VM treats as no-match -- the same as tp_parse returning 0. */
        {
            uint32_t m = tp_emit(lower, TP_VM_MATCH);
            if (m == TP_NO_INDEX) return TP_NO_INDEX;
            lower->program->instructions[m].pattern = 0;
            return m;
        }
    }

    case TP_OP_LITERAL: {
        uint32_t m = tp_emit(lower, TP_VM_MATCH);
        if (m == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[m].pattern = (tp_ir *)ir;
        lower->program->instructions[m].symbol  = ir->symbol;
        return m;
    }

    case TP_OP_EOF:
        return tp_emit(lower, TP_VM_EOF);

    case TP_OP_SEQ: {
        uint32_t i;
        uint32_t first = tp_reserve_children(lower, ir->child_count);
        uint32_t seq;
        if (first == TP_NO_INDEX && ir->child_count != 0) return TP_NO_INDEX;
        /* Lower each child to its own program and record it in the slice. The
        ** child lowerings append after this point, but the slice indices were
        ** reserved, so they stay valid as the table grows. */
        for (i = 0; i < ir->child_count; i++) {
            uint32_t child = tp_lower_ir(lower, ir->children[i]);
            if (!lower->ok) return TP_NO_INDEX;
            lower->program->children[first + i] = child;
        }
        seq = tp_emit(lower, TP_VM_SEQ);
        if (seq == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[seq].start = first;
        lower->program->instructions[seq].count = ir->child_count;
        return seq;
    }

    case TP_OP_CHOICE: {
        uint32_t i;
        uint32_t first = tp_reserve_children(lower, ir->child_count);
        uint32_t ch;
        if (first == TP_NO_INDEX && ir->child_count != 0) return TP_NO_INDEX;
        for (i = 0; i < ir->child_count; i++) {
            uint32_t child = tp_lower_ir(lower, ir->children[i]);
            if (!lower->ok) return TP_NO_INDEX;
            lower->program->children[first + i] = child;
        }
        ch = tp_emit(lower, TP_VM_CHOICE);
        if (ch == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[ch].start = first;
        lower->program->instructions[ch].count = ir->child_count;
        return ch;
    }

    case TP_OP_REPEAT: {
        uint32_t child = ir->child_count >= 1
            ? tp_lower_ir(lower, ir->children[0]) : TP_NO_INDEX;
        uint32_t rep;
        if (!lower->ok) return TP_NO_INDEX;
        rep = tp_emit(lower, TP_VM_REPEAT);
        if (rep == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[rep].target = child;
        lower->program->instructions[rep].min    = ir->min;
        lower->program->instructions[rep].max    = ir->max;
        return rep;
    }

    case TP_OP_OPTIONAL: {
        uint32_t child = ir->child_count >= 1
            ? tp_lower_ir(lower, ir->children[0]) : TP_NO_INDEX;
        uint32_t opt;
        if (!lower->ok) return TP_NO_INDEX;
        opt = tp_emit(lower, TP_VM_OPTIONAL);
        if (opt == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[opt].target = child;
        return opt;
    }

    case TP_OP_FIELD: {
        uint32_t child = ir->child_count >= 1
            ? tp_lower_ir(lower, ir->children[0]) : TP_NO_INDEX;
        uint32_t fld;
        if (!lower->ok) return TP_NO_INDEX;
        fld = tp_emit(lower, TP_VM_FIELD);
        if (fld == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[fld].target = child;
        lower->program->instructions[fld].symbol = ir->symbol;
        return fld;
    }

    case TP_OP_ALIAS: {
        uint32_t child = ir->child_count >= 1
            ? tp_lower_ir(lower, ir->children[0]) : TP_NO_INDEX;
        uint32_t als;
        if (!lower->ok) return TP_NO_INDEX;
        als = tp_emit(lower, TP_VM_ALIAS);
        if (als == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[als].target = child;
        lower->program->instructions[als].symbol = ir->symbol;
        return als;
    }

    case TP_OP_GOAL: {
        uint32_t child = ir->child_count >= 1
            ? tp_lower_ir(lower, ir->children[0]) : TP_NO_INDEX;
        uint32_t gl;
        if (!lower->ok) return TP_NO_INDEX;
        gl = tp_emit(lower, TP_VM_GOAL);
        if (gl == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[gl].target = child;
        lower->program->instructions[gl].symbol = ir->symbol;
        return gl;
    }

    case TP_OP_NO_EXTRAS: {
        uint32_t child = ir->child_count >= 1
            ? tp_lower_ir(lower, ir->children[0]) : TP_NO_INDEX;
        uint32_t ne;
        if (!lower->ok) return TP_NO_INDEX;
        ne = tp_emit(lower, TP_VM_NO_EXTRAS);
        if (ne == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[ne].target = child;
        lower->program->instructions[ne].aux    = ir->min; /* 0 disables extras */
        return ne;
    }

    case TP_OP_RECOVER: {
        uint32_t child = ir->child_count >= 1
            ? tp_lower_ir(lower, ir->children[0]) : TP_NO_INDEX;
        uint32_t rc;
        if (!lower->ok) return TP_NO_INDEX;
        rc = tp_emit(lower, TP_VM_RECOVER);
        if (rc == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[rc].target  = child;
        /* The recover IR node carries the diagnostic/insert payload; the VM reads
        ** it back through `pattern' (the IR node), so recovery synthesizes the
        ** exact MISSING node and diagnostic tp_vm_recover_insert produces. */
        lower->program->instructions[rc].pattern = (tp_ir *)ir;
        return rc;
    }

    case TP_OP_INJECT: {
        /* Language injection (W62). Lower the REGION sub-pattern to its own
        ** program (the span delimiter the VM matches) and carry the inject IR node
        ** itself through `pattern', so the dispatch loop reads the embedded grammar
        ** and language tag back from it and runs the inline sub-VM splice. */
        uint32_t child = ir->child_count >= 1
            ? tp_lower_ir(lower, ir->children[0]) : TP_NO_INDEX;
        uint32_t inj;
        if (!lower->ok) return TP_NO_INDEX;
        inj = tp_emit(lower, TP_VM_INJECT);
        if (inj == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[inj].target  = child;
        lower->program->instructions[inj].pattern = (tp_ir *)ir;
        return inj;
    }

    case TP_OP_DEFERRED_SLOT: {
        /* Declare a per-invocation slot in the enclosing rule frame. Zero-width:
        ** the dispatch claims the slot and succeeds without consuming input or
        ** emitting a node. The slot name rides on `symbol'. No child. */
        uint32_t ds = tp_emit(lower, TP_VM_DEFERRED_SLOT);
        if (ds == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[ds].symbol = ir->symbol;
        return ds;
    }

    case TP_OP_DEFER: {
        /* Match the sub-pattern, then capture its byte span and enqueue it onto
        ** the named slot. Structurally mirrors TP_OP_GOAL: the child program is
        ** lowered and carried in `target', so the match is tree-transparent. */
        uint32_t child = ir->child_count >= 1
            ? tp_lower_ir(lower, ir->children[0]) : TP_NO_INDEX;
        uint32_t df;
        if (!lower->ok) return TP_NO_INDEX;
        df = tp_emit(lower, TP_VM_DEFER);
        if (df == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[df].target = child;
        lower->program->instructions[df].symbol = ir->symbol;
        return df;
    }

    case TP_OP_PARSE_DEFERRED: {
        /* Drain a slot, invoking a rule per item. `ir->min' holds the resolved
        ** target rule index in grammar->rules[] (established by Phase-2
        ** resolution); lower a CALL to that rule and stash its program index in
        ** `target' so the drain can push the rule per item. `symbol' carries the
        ** slot name (resolved at dispatch by the same frame scan as DEFER) and
        ** `max' the goal_entry_index (unused this phase, kept for Phase 8). */
        uint32_t target = TP_NO_INDEX;
        uint32_t pd;
        if (ir->min != TP_NO_INDEX && ir->min < lower->grammar->rule_count) {
            uint32_t rule_sym = lower->grammar->rules[ir->min].symbol;
            target = tp_lower_rule(lower, rule_sym);
            if (!lower->ok) return TP_NO_INDEX;
        }
        pd = tp_emit(lower, TP_VM_PARSE_DEFERRED);
        if (pd == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[pd].symbol = ir->symbol;
        lower->program->instructions[pd].min    = ir->min;
        lower->program->instructions[pd].max    = ir->max;
        lower->program->instructions[pd].target = target;
        /* Phase 8: stash the parametrized template rule index in `aux' so the drain
        ** can instantiate the captured pattern per item. `max' is the COMPACT index
        ** over parametrized lex-goal entries; walk lex_goals[] counting parametrized
        ** entries until the count matches, then take that entry's template_rule_index.
        ** TP_NO_INDEX in `max' (no reachable param goal) leaves aux = TP_NO_INDEX, so
        ** the drain runs the Phase-4 non-instantiating path. */
        lower->program->instructions[pd].aux = TP_NO_INDEX;
        if (ir->max != TP_NO_INDEX) {
            uint32_t k, compact = 0;
            for (k = 0; k < lower->grammar->lex_goal_count; k++) {
                if (!lower->grammar->lex_goals[k].is_parametrized)
                    continue;
                if (compact == ir->max) {
                    lower->program->instructions[pd].aux =
                        lower->grammar->lex_goals[k].template_rule_index;
                    break;
                }
                compact++;
            }
        }
        return pd;
    }

    case TP_OP_PARAM_REF: {
        /* A TP_OP_PARAM_REF leaf lives only inside a (param NAME BODY) template
        ** rule, which is instantiated by tp_param_instantiate (its leaves replaced
        ** by TP_OP_LITERAL) before execution -- it is never lowered as a normal
        ** rule body. Reaching the bytecode lowerer is therefore an internal bug;
        ** lower to an always-failing MATCH with a NULL pattern (the unresolved-ref
        ** convention) so the VM mirrors tp_parse's 0 return rather than crashing. */
        uint32_t m = tp_emit(lower, TP_VM_MATCH);
        if (m == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[m].pattern = 0;
        return m;
    }

    case TP_OP_NOT: {
        /* `(not EXPR0 EXPR1)': EXPR0 (child[0]) is always a token-level negative
        ** lookahead into raw bytes; EXPR1 (child[1]) may be token-level OR contain
        ** a rule reference. When child[1] is purely token-level (no TP_OP_REF
        ** anywhere -- a literal/class/any/composite, e.g. the classic `(not X any)'),
        ** the whole NOT lowers to TP_VM_BYTE exactly as before: the byte matcher
        ** evaluates the lookahead and body together. When child[1] reaches a rule,
        ** the body cannot be a byte pattern, so we lower to TP_VM_NOT_RULE: the VM
        ** runs the token-level guard child[0] as a pure lookahead and, only if it
        ** does NOT match, dispatches child[1] through the rule-call machinery.
        ** A defensive child_count != 2 keeps the byte path. */
        const tp_ir *child0;
        tp_ir       *base;
        uint32_t     trigger;
        uint32_t     child1_prog;
        uint32_t     nr;
        if (ir->child_count != 2 || !tp_ir_subtree_has_ref(ir->children[1])) {
            uint32_t b = tp_emit(lower, TP_VM_BYTE);
            if (b == TP_NO_INDEX) return TP_NO_INDEX;
            lower->program->instructions[b].pattern = (tp_ir *)ir;
            return b;
        }
        child0 = ir->children[0];
        /* Capture child[0]'s base pattern + trigger symbol for goal-aware matching
        ** at dispatch. A guard that REFERENCES a token resolves to that token's
        ** pattern and carries the trigger symbol, so the VM routes it through
        ** tp_vm_goal_redirect (the same path the terminal match uses -- the Phase-8
        ** parametrized-goal hook). A guard that is a literal/class/any/composite has
        ** no trigger and matches its IR directly. */
        if (child0->op == TP_OP_REF) {
            const tp_token_def *tok =
                tp_lower_find_token(lower->grammar, child0->symbol);
            trigger = child0->symbol;
            /* A guard that references a RULE is invalid (guards are token-level);
            ** leave base = 0 (matches nothing -> NOT proceeds), defensively. */
            base = tok ? tok->pattern : 0;
        } else {
            trigger = TP_NO_SYMBOL;
            base    = (tp_ir *)child0;
        }
        child1_prog = tp_lower_ir(lower, ir->children[1]);
        if (!lower->ok) return TP_NO_INDEX;
        nr = tp_emit(lower, TP_VM_NOT_RULE);
        if (nr == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[nr].pattern = base;
        lower->program->instructions[nr].symbol  = trigger;
        lower->program->instructions[nr].target  = child1_prog;
        return nr;
    }

    case TP_OP_ANY:
    case TP_OP_CLASS:
    case TP_OP_CLASS_NAME:
    case TP_OP_NOT_KEYWORD: {
        /* Byte-level / reserved operators used directly in a rule body: the
        ** recursive descent treats these as an anonymous one-byte terminal that
        ** consumes extras, matches, and advances WITHOUT emitting a node. TP_VM_BYTE
        ** reproduces exactly that. (NOT_KEYWORD as a bare rule element is also an
        ** anonymous terminal in tp_parse's default leaf handling.) */
        uint32_t b = tp_emit(lower, TP_VM_BYTE);
        if (b == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[b].pattern = (tp_ir *)ir;
        return b;
    }

    default: {
        /* An op tp_parse's default also rejects: lower to an always-failing MATCH
        ** with a NULL pattern so the VM mirrors the reference's 0 return. */
        uint32_t m = tp_emit(lower, TP_VM_MATCH);
        if (m == TP_NO_INDEX) return TP_NO_INDEX;
        lower->program->instructions[m].pattern = 0;
        return m;
    }
    }
}

/* ** Mark every CALL in tail position within a rule body, so the iterative VM
** driver can reuse the current frame for a tail call instead of pushing a new one
** (W14-6, tail-call optimization). A CALL is in TAIL POSITION when it is the last
** action a rule body performs before returning its verdict -- the CALL's signal
** becomes the body's signal with no epilogue in between. The two structural cases
** that produce this in the real grammars are the right-recursive operator ladders
** (X -> Y (op X)?  lowers so the recursive reference is the last element) and rule
** aliases (A -> B  lowers the body to a single CALL).
**
** Tail-ness propagates structurally from a "tail slot" through the combinators
** that pass their child's signal straight up with no post-processing:
**   - SEQ: only the LAST child inherits the slot (earlier children must succeed
**     and then control continues to the next, so they are not tail);
**   - CHOICE: EVERY alternative inherits the slot (the chosen one's signal is the
**     choice's signal -- a CHOICE simply returns the winning alternative);
**   - a bare CALL in a tail slot IS a tail call.
** OPTIONAL/REPEAT discard or post-process the child signal (OPTIONAL always
** succeeds; REPEAT loops and compares against min), and FIELD/ALIAS/GOAL/
** NO_EXTRAS/RECOVER restore saved state (goal, no_extras, recover) AFTER the
** child returns -- an epilogue -- so a CALL beneath any of them is NOT tail and
** the slot does not propagate through them. This is deliberately conservative:
** it never marks a CALL that has work after it, so frame reuse is always sound.
**
** `tail' on the marked CALL instruction is read by tp_vm_run's iterative driver
** (vm.c) and by the generated compiled parser; a non-tail CALL keeps tail==0. */
static void tp_mark_tail(tp_program *program, uint32_t entry, int tail)
{
    const tp_instruction *in;
    if (entry == TP_NO_INDEX)
        return;
    in = &program->instructions[entry];
    switch (in->op) {
    case TP_VM_CALL:
        if (tail)
            program->instructions[entry].tail = 1;
        break;
    case TP_VM_SEQ:
        if (in->count > 0)
            tp_mark_tail(program, program->children[in->start + in->count - 1],
                         tail);
        break;
    case TP_VM_CHOICE: {
        uint32_t i;
        for (i = 0; i < in->count; i++)
            tp_mark_tail(program, program->children[in->start + i], tail);
        break;
    }
    default:
        /* Every other op either post-processes its child's signal or has no
        ** child program: the tail slot does not pass through, so stop here. */
        break;
    }
}

/* ** Walk the lowered program and mark the tail-position CALLs. Every CALL's
** target is a rule body that is itself a tail slot, so each rule body is the root
** of a fresh propagation. The body entry of a CALL is in->target; the start
** program is the start rule's body. Idempotent and order-independent: marking only
** ever sets a flag the lowering cleared to 0. */
static void tp_mark_tail_calls(tp_program *program)
{
    uint32_t i;
    if (program->start_program != TP_NO_INDEX)
        tp_mark_tail(program, program->start_program, 1);
    for (i = 0; i < program->instruction_count; i++)
        if (program->instructions[i].op == TP_VM_CALL &&
            program->instructions[i].target != TP_NO_INDEX)
            tp_mark_tail(program, program->instructions[i].target, 1);
}

/* ** Count the lowered program's CALL instructions and how many sit in tail
** position (the W14-6 tail-call analysis). Walks the flat instruction array once;
** the tail flags were set by tp_mark_tail_calls during lowering. Writes the totals
** through the out-params (either may be NULL). A library function -- declared in
** internal.h -- so the count is reproducible from the CLI (analyze-tailcalls) and
** the design evidence can be regenerated, not a throwaway. */
void tp_count_tail_calls(const tp_program *program,
                         uint32_t *out_total, uint32_t *out_tail)
{
    uint32_t i, total = 0, tail = 0;
    if (program != 0) {
        for (i = 0; i < program->instruction_count; i++) {
            if (program->instructions[i].op != TP_VM_CALL)
                continue;
            total++;
            if (program->instructions[i].tail)
                tail++;
        }
    }
    if (out_total != 0) *out_total = total;
    if (out_tail  != 0) *out_tail  = tail;
}

/* ** Lower a grammar and report its CALL / tail-CALL counts (the W14-6 tail-call
** analysis), taking only public types so the hosted CLI can call it without
** pulling in this private header (which matters for the amalgamation, where the
** CLI is a separate translation unit). Lowers the start rule onto the grammar's
** arena -- the same lowering the VM uses, so the counts reflect the shipped
** bytecode -- and writes the totals through the out-params. On a lowering failure
** the counts are left at zero. */
void tp_grammar_tail_call_counts(const tinyparse_grammar *grammar,
                                 uint32_t *out_total, uint32_t *out_tail)
{
    tp_program *program;
    if (out_total != 0) *out_total = 0;
    if (out_tail  != 0) *out_tail  = 0;
    if (grammar == 0)
        return;
    program = tp_compile_program(grammar->arena, grammar, grammar->start_symbol);
    if (program == 0)
        return;
    tp_count_tail_calls(program, out_total, out_tail);
}

/* ** Lower a grammar's start rule (and everything reachable) to a flat bytecode
** program. The start program runs the start rule's BODY directly (tp_execute_vm
** pushes the Source node itself and parses start->body into the root's children),
** so the entry point is the lowered body, followed by a HALT sentinel the
** dispatch loop stops on. Returns NULL on allocation failure. */
tp_program *tp_compile_program(tp_arena *arena,
                               const tinyparse_grammar *grammar,
                               uint32_t start_symbol)
{
    tp_program *program;
    tp_lower    lower;
    const tp_rule_def *start;
    uint32_t    body_program;
    uint32_t    halt;

    program = (tp_program *)tp_alloc(arena, sizeof *program);
    if (program == 0)
        return 0;
    program->arena                = arena;
    program->instructions         = 0;
    program->instruction_count    = 0;
    program->instruction_capacity = 0;
    program->children             = 0;
    program->child_count          = 0;
    program->child_capacity       = 0;
    program->start_program        = TP_NO_INDEX;
    program->grammar              = grammar;

    start = tp_lower_find_rule_def(grammar, start_symbol);
    if (start == 0)
        return program; /* no start rule: tp_execute_vm reports it */

    lower.program       = program;
    lower.grammar       = grammar;
    lower.rules         = 0;
    lower.rule_count    = 0;
    lower.rule_capacity = 0;
    lower.ok            = 1;

    body_program = tp_lower_ir(&lower, start->body);
    if (!lower.ok)
        return 0;
    halt = tp_emit(&lower, TP_VM_HALT);
    if (halt == TP_NO_INDEX || !lower.ok)
        return 0;
    (void)halt;
    program->start_program = body_program;
    /* Mark the tail-position CALLs (W14-6) so the iterative driver can reuse the
    ** current frame on a tail call rather than push a new one, bounding stack
    ** depth at the true nesting instead of the call-chain length. */
    tp_mark_tail_calls(program);
    return program;
}
