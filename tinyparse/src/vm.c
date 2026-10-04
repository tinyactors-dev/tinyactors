/*
** vm.c -- the bytecode VM dispatch loop (W59), the tinyparse engine.
**
** A flat dispatch loop over the bytecode tp_compile_program lowers (see
** vmcompile.c), driving the tree builder (tp_tree_builder_*) and the token matcher
** (tp_token_match_memo, first-byte dispatch). Since W14-5 this is the SOLE engine:
** the recursive-descent IR interpreter that once lived in parse.c was retired once
** the VM was the only production path. The concrete syntax tree the
** VM builds is byte-identical to the C-compiled parser's (mise run gate:cc-diff),
** with the conformance suite (296 independent expected outputs) the standing
** correctness anchor. The VM replaces a per-node C call frame and pointer chase
** with one loop over a contiguous instruction stream and explicit, arena-grown
** control stacks.
**
** The control stack is the explicit form of the recursion a recursive descent
** would use: each frame is an in-progress combinator (a seq stepping through its
** children, a choice trying alternatives, a repeat iterating, a rule whose body is
** running) with the marks it would have held in C locals -- the saved builder
** count, input position, and child-sink count -- so backtracking restores them and
** FAIL unwinds without the C stack. Terminals, extras, recovery, lexical goals,
** and reserved words are handled by dedicated helpers below.
**
** W14-6: the dispatch is now FULLY iterative. The earlier cut still C-recursed per
** rule call (tp_vm_run -> tp_vm_run_call -> tp_vm_run), so a deeply nested input
** overflowed the C stack. The driver below (tp_vm_run) drives EVERY combinator --
** rule calls included -- on one explicit, arena-backed frame stack, so a ~200k-deep
** input grows the arena instead of crashing (mise run gate:vm-nocrash). It is
** the single source of truth: the generated compiled parser (emitparser.c) calls
** this same tp_vm_run rather than emitting its own loop. A CALL the lowerer marks
** tail-position is exploited via FIELD/ALIAS frame reuse (see the driver comment).
**
** Freestanding: every stack grows from the parse arena.
*/
#include "internal.h"

#ifdef TP_DEBUG_INTERN_GUARD
#include <assert.h>
#endif

/* ** The tp_vm_sink, tp_rule_slot, tp_rule_memo, tp_vm_state and tp_vm_signal
** types now live in internal.h so the compiled recursive-descent C the emitter
** generates (W14-3) shares the same runtime. vm.c keeps the helpers' bodies and
** the dispatch loop; the generated parser calls the same helpers but bakes the
** rule structure into C control flow instead of interpreting tp_vm_run. */

/* ** Append a node index to a sink, growing geometrically. Returns 0 on OOM.
** Geometric growth policy (start 8, double), matching the tree builder's so the
** arena layout and any downstream behaviour stay predictable. */
/* ** The slow path of tp_vm_sink_push (the geometric grow), factored out so the
** inline fast path in internal.h stays a tiny append. Returns 0 on OOM. */
int tp_vm_sink_grow(tp_arena *arena, tp_vm_sink *sink)
{
    uint32_t nc = sink->capacity == 0 ? 8 : sink->capacity * 2;
    /* RAW: the live prefix [0,count) is copied in below and every later slot is
    ** written before it is read (count grows one at a time), so zeroing the
    ** block would be wasted (W16-1). */
    uint32_t *grown = (uint32_t *)tp_alloc_raw(arena, nc * sizeof *grown);
    uint32_t i;
    if (grown == 0)
        return 0;
    for (i = 0; i < sink->count; i++)
        grown[i] = sink->items[i];
    sink->items    = grown;
    sink->capacity = nc;
    return 1;
}

/* ** The injection splice (W62) pushes its injection node into the active child
** sink through a generic callback (tp_inject_run), so inject.c needs no knowledge
** of tp_vm_sink. This bundles the arena and the sink the callback writes through. */
typedef struct tp_vm_inject_sink {
    tp_tree_builder *builder;
    tp_arena        *arena;
    tp_vm_sink      *sink;
} tp_vm_inject_sink;

/* ** Append callback for tp_inject_run: push one node index into the bundled VM
** sink. Returns 1 on success, 0 on allocation failure. */
static int tp_vm_inject_sink_append(void *sink_user, uint32_t node_index)
{
    tp_vm_inject_sink *bundle = (tp_vm_inject_sink *)sink_user;
    if (bundle->sink == 0)
        return 1;
    return tp_vm_sink_push(bundle->builder, bundle->arena, bundle->sink,
                           node_index);
}

/* ** The rule memo (W67) is keyed by (program index, offset, lexical goal,
** field, alias): a rule body produces the same verdict every time it is retried,
** so the verdict is cached and a later attempt short-circuits instead of
** re-descending the body. A FAILURE caches just the verdict; a SUCCESS caches a
** self-contained SNAPSHOT of the subtree it built (relocated to base 0), which
** replaying copies into the builder as FRESH nodes. Both verdicts are cached only
** when no recovery is armed, no_extras is clear, and the body emitted no
** diagnostic. The tp_rule_slot / tp_rule_memo / tp_vm_state types now live in
** internal.h (shared with the generated recursive-descent C). */

/* ** The slot verdict codes (TP_RULE_FAIL/SUCCESS/SEEN) and the snapshot
** threshold (TP_RULE_MEMO_MIN_SUBTREE) now live in internal.h so the generated
** compiled parser (emitparser.c), which inlines the CALL memo logic into its
** explicit-stack frame machine, reproduces the same verdicts byte-for-byte. */

/* ** Round up to a power of two (mirrors tp_round_pow2 in token.c). */
static uint32_t tp_vm_round_pow2(uint32_t n)
{
    uint32_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

/* ** Allocate the rule memo's tables, sized to the input. On allocation failure
** the table is left empty (capacity 0), which simply disables memoisation. The
** allocation is the input-scaled `live' column whose zeroing (one byte per slot,
** pow2(2*input+64) for a clean parse) was the single largest memset in the profile.
** It is split out of the init so the COMMON CASE -- a clean, linear parse that
** never goes super-linear -- can skip it entirely (W17-GOAL): the memo is dormant
** until tp_vm_call_enter flips memo_active on, and only then is the table built.
** Returns 1 if the memo is usable (capacity != 0), 0 otherwise. */
static int tp_rule_memo_ensure(tp_rule_memo *memo)
{
    uint32_t cap;
    if (memo->capacity != 0)
        return 1;            /* already allocated */
    cap = tp_vm_round_pow2(memo->pending_length * 2u + 64u);
    /* The slot table is RAW: read only after the `live' column reports a slot
    ** occupied, so clearing it would be a memset the parse discards. Only the
    ** one-byte-per-slot `live' column is zeroed, which marks every slot empty
    ** -- this dormant table's zeroing dominated the profile once the token memo
    ** was fixed (W15-1). */
    memo->slots = (tp_rule_slot *)tp_alloc_raw(memo->arena, cap * sizeof *memo->slots);
    memo->live  = (uint8_t *)tp_alloc(memo->arena, cap * sizeof *memo->live);
    if (memo->slots == 0 || memo->live == 0) {
        memo->slots    = 0;
        memo->live     = 0;
        memo->capacity = 0;
        memo->mask     = 0;
        return 0;
    }
    memo->capacity = cap;
    memo->mask     = cap - 1;
    return 1;
}

/* ** Prepare the rule memo for a parse WITHOUT allocating its tables: record the
** arena and the size the input wants, and leave the table empty (capacity 0). A
** clean parse, which never activates the memo, then never allocates or zeroes the
** input-scaled `live' column at all (W17-GOAL). tp_rule_memo_ensure builds the
** tables the first time the parse goes super-linear; the differential's force-on
** path calls ensure right after this so the replay path is exercised. */
static void tp_rule_memo_init(tp_rule_memo *memo, tp_arena *arena,
                              uint32_t input_length)
{
    memo->arena          = arena;
    memo->count          = 0;
    memo->slots          = 0;
    memo->live           = 0;
    memo->capacity       = 0;
    memo->mask           = 0;
    memo->pending_length = input_length;
}

/* ** Hash a memo key. */
static uint32_t tp_rule_memo_hash(uint32_t program, uint32_t offset,
                                  uint32_t goal, uint32_t field, uint32_t alias)
{
    uint32_t h = 2166136261u;
    h = (h ^ program) * 16777619u;
    h = (h ^ offset)  * 16777619u;
    h = (h ^ goal)    * 16777619u;
    h = (h ^ field)   * 16777619u;
    h = (h ^ alias)   * 16777619u;
    return h;
}

/* ** Whether a probed rule slot is occupied. Occupancy is the memo's `live'
** column, not a field in the (raw) slot, so callers must ask here rather than
** read the slot directly -- an empty slot's fields are uninitialised (W15-1).
** A NULL slot (a full table) is treated as not live. */
int tp_rule_slot_live(const tp_rule_memo *memo, const tp_rule_slot *slot)
{
    if (slot == 0)
        return 0;
    return memo->live[(uint32_t)(slot - memo->slots)] != 0;
}

/* ** Probe for a key: returns the matching occupied slot, the empty slot where
** it would be inserted, or NULL when the table is full. Occupancy is read from
** the `live' column (tp_rule_slot_live), never from the raw slot. */
tp_rule_slot *tp_rule_memo_probe(tp_rule_memo *memo, uint32_t program,
                                 uint32_t offset, uint32_t goal,
                                 uint32_t field, uint32_t alias)
{
    uint32_t i = tp_rule_memo_hash(program, offset, goal, field, alias)
                 & memo->mask;
    uint32_t steps = 0;
    while (steps <= memo->mask) {
        tp_rule_slot *slot = &memo->slots[i];
        if (!memo->live[i])
            return slot;
        if (slot->program == program && slot->offset == offset &&
            slot->goal == goal && slot->field == field && slot->alias == alias)
            return slot;
        i = (i + 1) & memo->mask;
        steps++;
    }
    return 0;
}

/* ** Double the rule memo's tables and re-insert every live key. The table is
** first sized to the input, but the number of keys a parse produces is not bounded
** by the input length: every rule program, under every field, alias and lexical
** goal, can be tried at the same offset. A grammar with a thousand programs fills a
** table sized for a short input almost at once, and a table that stops admitting
** keys stops bounding the re-descent -- an unclosed `(' in std.typescript then
** costs twice as much per byte. So the table grows like any hash table, out of
** the parser arena; the old arrays are simply abandoned there (the arena frees
** them with the parser), which keeps the total at most twice the final table.
** Returns 1 on success, 0 if the new tables could not be allocated (the old
** table is kept and simply stops admitting keys, as before). */
static int tp_rule_memo_grow(tp_rule_memo *memo)
{
    tp_rule_memo   grown = *memo;
    uint32_t       i;
    if (memo->capacity > 0x7FFFFFFFu / 2u ||
        (size_t)memo->capacity * 2u > ((size_t)-1) / sizeof *memo->slots)
        return 0;
    grown.capacity = memo->capacity * 2u;
    grown.mask     = grown.capacity - 1u;
    grown.count    = 0;
    grown.slots = (tp_rule_slot *)tp_alloc_raw(memo->arena,
                                               grown.capacity * sizeof *grown.slots);
    grown.live  = (uint8_t *)tp_alloc(memo->arena, grown.capacity * sizeof *grown.live);
    if (grown.slots == 0 || grown.live == 0)
        return 0;
    for (i = 0; i < memo->capacity; i++) {
        const tp_rule_slot *old = &memo->slots[i];
        tp_rule_slot       *slot;
        if (!memo->live[i])
            continue;
        slot = tp_rule_memo_probe(&grown, old->program, old->offset, old->goal,
                                  old->field, old->alias);
        *slot = *old;
        grown.live[(uint32_t)(slot - grown.slots)] = 1;
        grown.count++;
    }
    *memo = grown;
    return 1;
}

/* ** Record a verdict at a probed empty slot. Past ~3/4 load the table doubles
** (tp_rule_memo_grow) so probe chains stay short and every key is still admitted;
** only when the arena cannot supply the larger table does a key go unrecorded.
** For a success, snapshot/snap_count/end carry the subtree. */
void tp_rule_memo_store(tp_rule_memo *memo, tp_rule_slot *slot,
                        uint32_t program, uint32_t offset, uint32_t goal,
                        uint32_t field, uint32_t alias, uint8_t matched,
                        uint32_t end, tp_node *snapshot,
                        uint32_t snap_count)
{
    if (slot == 0)
        return;
    if (tp_rule_slot_live(memo, slot)) {
        /* In-place upgrade: a SEEN success becoming a snapshotted SUCCESS. The
        ** key already occupies this slot, so no new insertion and no load check. */
        if (slot->matched == TP_RULE_SEEN && matched == TP_RULE_SUCCESS) {
            slot->matched    = TP_RULE_SUCCESS;
            slot->end        = end;
            slot->snapshot   = snapshot;
            slot->snap_count = snap_count;
        }
        return;
    }
    if (memo->count >= memo->capacity - (memo->capacity >> 2)) {
        if (!tp_rule_memo_grow(memo))
            return;
        slot = tp_rule_memo_probe(memo, program, offset, goal, field, alias);
    }
    slot->program    = program;
    slot->offset     = offset;
    slot->goal       = goal;
    slot->field      = field;
    slot->alias      = alias;
    memo->live[(uint32_t)(slot - memo->slots)] = 1;
    slot->matched    = matched;
    slot->end        = end;
    slot->snapshot   = snapshot;
    slot->snap_count = snap_count;
    memo->count++;
}

/* ** Snapshot the subtree the builder holds in [root, end_count) into a fresh,
** self-contained array whose internal node references are relocated to base 0
** (so the root sits at index 0). Returns the array (and its size through
** *out_count) or NULL on allocation failure / an empty range. The builder's own
** nodes are left untouched. */
tp_node *tp_vm_snapshot_subtree(tp_tree_builder *builder, uint32_t root,
                                uint32_t end_count, uint32_t *out_count)
{
    uint32_t size = end_count - root;
    tp_node *snap;
    uint32_t i;
    if (size == 0)
        return 0;
    snap = (tp_node *)tp_alloc(builder->arena, size * sizeof *snap);
    if (snap == 0)
        return 0;
    for (i = 0; i < size; i++) {
        tp_node n = builder->nodes[root + i];
        if (n.parent != TP_NO_INDEX)
            n.parent -= root;
        if (n.first_child != TP_NO_INDEX)
            n.first_child -= root;
        if (n.next_sibling != TP_NO_INDEX)
            n.next_sibling -= root;
        snap[i] = n;
    }
    *out_count = size;
    return snap;
}

/* ** Replay a cached success: copy the snapshot subtree into the builder as fresh
** nodes (relocating its base-0 references to the new base), append the copied
** root to the sink, and advance the cursor to the cached end offset. The copy is
** independent of every other use, so the caller's later link step mutates only
** this copy. Returns 1 on success, 0 on allocation failure. */
int tp_vm_replay_success(tp_vm_state *state, tp_vm_sink *sink,
                         const tp_rule_slot *slot)
{
    tp_tree_builder *builder = &state->parser->builder;
    uint32_t base = builder->count;
    uint32_t i;
    for (i = 0; i < slot->snap_count; i++) {
        const tp_node *s = &slot->snapshot[i];
        uint32_t idx = tp_tree_builder_push(builder, s->kind, s->field,
                                            s->start_byte, s->end_byte,
                                            TP_NO_INDEX);
        tp_node *d;
        if (idx == TP_NO_INDEX)
            return 0;
        d = &builder->nodes[idx];
        d->parent       = (s->parent == TP_NO_INDEX) ? TP_NO_INDEX
                                                     : s->parent + base;
        d->first_child  = (s->first_child == TP_NO_INDEX) ? TP_NO_INDEX
                                                          : s->first_child + base;
        d->next_sibling = (s->next_sibling == TP_NO_INDEX) ? TP_NO_INDEX
                                                           : s->next_sibling + base;
        d->child_count  = s->child_count;
        d->flags        = s->flags;
        d->language_tag = s->language_tag;
    }
    if (sink != 0 && !tp_vm_sink_push(&state->parser->builder,
                                      state->parser->arena, sink, base))
        return 0;
    state->position = slot->end;
    return 1;
}

/* ====================================================================== */
/* The executor helpers, over VM state. (These were once mirrored by the   */
/* recursive IR interpreter in parse.c, retired in W14-5; the rationale     */
/* that lived alongside it now lives at each helper below.)                 */
/* ====================================================================== */

/* ** Build the grammar's token-by-symbol index (W66): an array indexed by symbol
** id holding each token def, so tp_vm_find_token is O(1) instead of a linear scan
** over the token table on every terminal match. Built once and cached on the
** grammar (a runtime cache like vm_program). On allocation failure the index is
** left NULL and tp_vm_find_token falls back to the scan, so it is purely an
** optimization. */
static void tp_vm_build_token_index(tinyparse_grammar *grammar)
{
    uint32_t i, max_symbol = 0, size;
    tp_token_def **index;
    if (grammar->token_count == 0)
        return;
    for (i = 0; i < grammar->token_count; i++)
        if (grammar->tokens[i].symbol != TP_NO_SYMBOL &&
            grammar->tokens[i].symbol > max_symbol)
            max_symbol = grammar->tokens[i].symbol;
    size = max_symbol + 1;
    index = (tp_token_def **)tp_alloc(grammar->arena, size * sizeof *index);
    if (index == 0)
        return; /* fall back to the linear scan */
    for (i = 0; i < grammar->token_count; i++) {
        uint32_t sym = grammar->tokens[i].symbol;
        if (sym != TP_NO_SYMBOL && sym < size)
            index[sym] = &grammar->tokens[i];
    }
    grammar->token_index      = index;
    grammar->token_index_size = size;
}

/* ** Build the grammar's hidden-rule-by-symbol index (W17-FLAT): a byte array
** indexed by symbol id holding 1 for a rule marked TP_RULE_HIDDEN and 0
** otherwise, so the shared node-close path resolves "is this rule hidden?" in
** O(1) instead of scanning the rule table per close. Built once and cached on the
** grammar (a runtime cache like token_index). On allocation failure the index is
** left NULL and tp_grammar_rule_hidden returns 0, so a hidden rule simply stays
** visible -- sound, merely un-collapsed. */
static void tp_vm_build_hidden_index(tinyparse_grammar *grammar)
{
    uint32_t i, max_symbol = 0, size, any = 0;
    uint8_t *index;
    grammar->hidden_index_built = 1;
    for (i = 0; i < grammar->rule_count; i++) {
        if ((grammar->rules[i].flags & TP_RULE_HIDDEN) == 0)
            continue;
        any = 1;
        if (grammar->rules[i].symbol != TP_NO_SYMBOL &&
            grammar->rules[i].symbol > max_symbol)
            max_symbol = grammar->rules[i].symbol;
    }
    if (!any)
        return; /* no hidden rule: leave the index NULL (lookup returns 0) */
    size  = max_symbol + 1;
    index = (uint8_t *)tp_alloc(grammar->arena, size * sizeof *index);
    if (index == 0)
        return; /* fall back to treating every rule as visible */
    for (i = 0; i < size; i++)
        index[i] = 0;
    for (i = 0; i < grammar->rule_count; i++) {
        uint32_t sym = grammar->rules[i].symbol;
        if ((grammar->rules[i].flags & TP_RULE_HIDDEN) == 0 ||
            sym == TP_NO_SYMBOL || sym >= size)
            continue;
        /* Bit 0 marks a hidden rule (the W17-FLAT exit-time collapse candidate);
        ** bit 1 additionally marks a TRANSPARENT_ALWAYS rule (W17-RECLAIM), whose
        ** wrapper node the entry path skips outright. The two share one byte array
        ** so the hot path is still a single read. */
        index[sym] = TP_RULE_HIDDEN;
        if (grammar->rules[i].flags & TP_RULE_TRANSPARENT_ALWAYS)
            index[sym] |= TP_RULE_TRANSPARENT_ALWAYS;
    }
    grammar->hidden_index      = index;
    grammar->hidden_index_size = size;
}

/* ** Build the hidden-rule index once (W17-FLAT). Idempotent: the
** `hidden_index_built' flag makes a second call a no-op, so it is cheap to call
** unconditionally before each parse loop. */
void tp_grammar_build_hidden_index(tinyparse_grammar *grammar)
{
    if (!grammar->hidden_index_built)
        tp_vm_build_hidden_index(grammar);
}

/* ** True when `symbol' names a hidden rule (W17-FLAT): one array read against the
** index tp_grammar_build_hidden_index already built. Returns 0 when no rule is
** hidden or the index was not built. */
int tp_grammar_rule_hidden(const tinyparse_grammar *grammar, uint32_t symbol)
{
    if (grammar->hidden_index != 0 && symbol < grammar->hidden_index_size)
        return (grammar->hidden_index[symbol] & TP_RULE_HIDDEN) != 0;
    return 0;
}

/* ** Find a token definition by symbol, or NULL. Uses the O(1) token index when
** built (the common case); falls back to a linear scan otherwise. */
static const tp_token_def *tp_vm_find_token(const tinyparse_grammar *grammar,
                                            uint32_t symbol)
{
    uint32_t i;
    if (grammar->token_index != 0) {
        if (symbol < grammar->token_index_size)
            return grammar->token_index[symbol];
        return 0;
    }
    for (i = 0; i < grammar->token_count; i++)
        if (grammar->tokens[i].symbol == symbol)
            return &grammar->tokens[i];
    return 0;
}

/* ** Resolve a terminal reference under the active lexical goal: when the active
** goal redirects this trigger to a different token, return that token (so the same
** lexeme scans differently in this context), else NULL. */
static const tp_token_def *tp_vm_goal_redirect(const tp_vm_state *state,
                                               uint32_t trigger_symbol)
{
    const tinyparse_grammar *grammar = state->grammar;
    uint32_t i;
    if (state->active_goal == TP_NO_SYMBOL || grammar->lex_goal_count == 0)
        return 0;
    for (i = 0; i < grammar->lex_goal_count; i++) {
        if (grammar->lex_goals[i].goal_symbol == state->active_goal &&
            grammar->lex_goals[i].trigger_symbol == trigger_symbol) {
            uint32_t j;
            uint32_t target = grammar->lex_goals[i].target_symbol;
            for (j = 0; j < grammar->token_count; j++)
                if (grammar->tokens[j].symbol == target)
                    return &grammar->tokens[j];
            return 0;
        }
    }
    return 0;
}

/* ** Resolve a lexical-goal redirect for a PARAMETRIZED entry (the Phase-8 heredoc
** hook). Returns 1 and sets *out_pat / *out_kind when `trigger' resolves, under the
** active goal, to a parametrized lex-goal entry WITH a pattern currently on its
** stack; 0 otherwise (not a trigger, not parametrized, or the stack is empty). On
** the 0 path the caller falls through to the static redirect / base pattern -- and
** the parametrized token's base pattern is an empty class that never matches, so an
** empty stack (depth 0) yields no match, exactly the spec's "depth zero -> ERROR"
** behaviour, with no special-casing here.
**
** The emitted leaf KIND is the trigger symbol itself (the parametrized token), not
** a redirect target: the instantiated literal carries TP_NO_SYMBOL, so the kind
** must come from here. The entry's compact index (count of parametrized entries
** before it in lex_goals[]) selects the param stack. */
static int tp_vm_goal_redirect_param(const tp_vm_state *state, uint32_t trigger,
                                     const tp_ir **out_pat, uint32_t *out_kind)
{
    const tinyparse_grammar *grammar = state->grammar;
    uint32_t i, compact = 0;
    if (state->active_goal == TP_NO_SYMBOL || grammar->lex_goal_count == 0)
        return 0;
    for (i = 0; i < grammar->lex_goal_count; i++) {
        if (!grammar->lex_goals[i].is_parametrized) {
            /* Static entries do not consume a compact index and are handled by the
            ** static tp_vm_goal_redirect path. */
            continue;
        }
        if (grammar->lex_goals[i].goal_symbol == state->active_goal &&
            grammar->lex_goals[i].trigger_symbol == trigger) {
            tp_param_stack *stack = state->parser->param_stacks
                ? &state->parser->param_stacks[compact] : 0;
            if (stack != 0 && stack->depth > 0) {
                *out_pat  = stack->patterns[stack->depth - 1];
                *out_kind = trigger;
                return 1;
            }
            return 0; /* depth 0 -> base empty-class -> no match */
        }
        compact++;
    }
    return 0;
}

/* ** Append one held `attach: leading' trivia span, growing the buffer
** geometrically. Returns 1, or 0 on allocation failure. The span is NOT yet a tree
** node -- it materializes only at the next flush (tp_vm_flush_held). */
static int tp_vm_hold_extra(tp_vm_state *state, uint32_t symbol,
                            uint32_t start, uint32_t end)
{
    if (state->held_count == state->held_capacity) {
        uint32_t new_cap = state->held_capacity == 0 ? 8
                                                     : state->held_capacity * 2;
        tp_held_extra *grown = (tp_held_extra *)tp_alloc(
            state->parser->arena, new_cap * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return 0;
        for (i = 0; i < state->held_count; i++)
            grown[i] = state->held[i];
        state->held          = grown;
        state->held_capacity = new_cap;
    }
    state->held[state->held_count].symbol = symbol;
    state->held[state->held_count].start  = start;
    state->held[state->held_count].end    = end;
    state->held_count++;
    return 1;
}

/* ** Flush every held `attach: leading' trivia into `sink' as leaf nodes, in held
** order, then clear the buffer. See the declaration in internal.h. */
int tp_vm_flush_held(tp_vm_state *state, tp_vm_sink *sink)
{
    uint32_t i;
    if (state->held_count == 0)
        return 1;
    for (i = 0; i < state->held_count; i++) {
        uint32_t node = tp_tree_builder_push(
            &state->parser->builder, state->held[i].symbol, TP_NO_SYMBOL,
            state->held[i].start, state->held[i].end, TP_NO_INDEX);
        if (node == TP_NO_INDEX)
            return 0;
        if (sink != 0 && !tp_vm_sink_push(&state->parser->builder,
                                          state->parser->arena, sink, node))
            return 0;
        /* A flushed comment IS materialized content of THIS sink (a real sibling), so
        ** it advances the content high-water mark past its bytes. */
        if (state->held[i].end > state->content_end)
            state->content_end = state->held[i].end;
    }
    state->held_count = 0;
    return 1;
}

/* ** Consume extras (trivia/skip/error tokens) at the current position: trivia and
** error become leaf nodes appended to `sink', skip is dropped, error also raises a
** diagnostic; the per-position "no extras" memo short-circuits revisits. A trivia
** kind marked `attach: leading' is HELD (tp_vm_hold_extra) instead of linked, and the
** held buffer is flushed into `sink' at entry so a comment a closing node tried to
** claim re-emerges ahead of the next sibling. Returns 0 only on allocation failure. */
int tp_vm_consume_extras(tp_vm_state *state, tp_vm_sink *sink)
{
    const tinyparse_grammar *grammar = state->grammar;
    int progress = 1;
    uint32_t start = state->position;
    int memoable;
    /* Flush any held `attach: leading' trivia FIRST: this consume sits at a sibling
    ** boundary (a rule entry or a terminal), so a comment held when the previous node
    ** closed lands here, ahead of the node about to fill this sink. Done before every
    ** other early return so the buffer drains at the boundary even when the cursor byte
    ** begins no further extra. */
    if (state->held_count != 0 && !tp_vm_flush_held(state, sink))
        return 0;
    if (state->no_extras)
        return 1;
    /* W16-1/W16-GOAL fast path FIRST: at the overwhelming majority of positions the
    ** cursor byte begins no extra (an identifier/operator/string byte, not
    ** whitespace or a comment opener). One lead-byte bitset probe proves that and
    ** returns, skipping the per-entry token-match loop AND the extras-memo machinery
    ** -- the bitset reject is itself O(1), so caching it (a write per terminal) was
    ** pure memory traffic. The lead set is sound (the union of every extra's
    ** first-byte set, disabled when an extra's lead byte cannot be pinned down), so a
    ** byte outside it provably begins no extra and the cursor stays put. Ordering
    ** this ahead of the memoable computation makes the dominant no-extra case the
    ** cheapest path through the function. */
    if (!grammar->extras_lead_open && grammar->extras_lead_state) {
        if (start >= state->length)
            return 1; /* end of input: no extra can match */
        {
            uint8_t b = state->input[start];
            if (((grammar->extras_lead[b >> 3] >> (b & 7)) & 1) == 0)
                return 1; /* this byte begins no extra: nothing to consume */
        }
    }
    /* A byte that COULD begin an extra: fall to the full scan. The extras memo
    ** short-circuits a position already proven to consume no extra (e.g. a comment
    ** opener `/` that turned out to be a divide, revisited on a backtrack). */
    memoable = (state->active_goal == TP_NO_SYMBOL &&
                state->extras_memo.marks != 0 &&
                start < state->extras_memo.length);
    if (memoable && state->extras_memo.marks[start] != 0)
        return 1;
    /* W17-2 baked extras fast path: the generated parser supplies a baked scanner
    ** that finds the first matching extra (in table order) and its end without the
    ** token VM. The matched extra's policy / node / diagnostic logic below is shared
    ** verbatim, so the tree and diagnostics are byte-identical.
    **
    ** W18-EXTRAS: the baked scanner runs REGARDLESS of the active lexical goal. A
    ** lexical goal is purely about a TERMINAL REFERENCE in a rule body: under a goal,
    ** tp_vm_goal_redirect rewrites a trigger terminal to a different token (the
    ** regex-vs-divide, template-body, JSX-text mechanism). An EXTRA is never a goal
    ** trigger nor target -- the extras loop matches each extra's fixed pattern
    ** directly and never redirects -- so which bytes are trivia (whitespace,
    ** comments) does not depend on the goal at all. The generic fallback below even
    ** passes memo=0, so the `goal' it forwards to tp_token_match_memo (which uses it
    ** only as a memo-probe key) is dead. The earlier `active_goal == TP_NO_SYMBOL'
    ** guard was therefore conservative, not load-bearing: the baked scanner and the
    ** generic loop produce byte-identical spans in both the goal and no-goal cases.
    ** Dropping the guard lets the straight-line scanner replace the per-extra
    ** token_match_memo->token_run loop in the goal-active path -- which, in TSX, is
    ** almost everywhere a lexical goal is live. The per-position extras memo stays
    ** gated to the no-goal case (its `memoable' flag is computed there), unchanged. */
    if (state->extras_match != 0) {
        while (progress) {
            uint32_t e = 0;
            uint32_t end = 0;
            progress = 0;
            if (!state->extras_match(state->parser->engine, state->input,
                                     state->length, state->position, &e, &end))
                break;
            if (end == state->position)
                break;
            if (grammar->extras[e].policy == TP_EXTRA_TRIVIA &&
                (grammar->extras[e].attach == TP_EXTRA_ATTACH_LEADING ||
                 state->held_count != 0)) {
                /* Hold this span when it is `attach: leading' OR when a leading comment
                ** is already held: everything after a held comment belongs to the NEXT
                ** sibling, so attaching it here would extend the closing node across the
                ** floated comment and overlap it. Both re-emerge in held order at the
                ** next sibling boundary. content_end does NOT advance -- these bytes are
                ** not content of the node now closing. */
                if (!tp_vm_hold_extra(state, grammar->extras[e].token_symbol,
                                      state->position, end))
                    return 0;
            } else {
                if (grammar->extras[e].policy != TP_EXTRA_SKIP) {
                    uint32_t node = tp_tree_builder_push(
                        &state->parser->builder, grammar->extras[e].token_symbol,
                        TP_NO_SYMBOL, state->position, end, TP_NO_INDEX);
                    if (node == TP_NO_INDEX)
                        return 0;
                    if (sink != 0 && !tp_vm_sink_push(&state->parser->builder, state->parser->arena, sink, node))
                        return 0;
                    if (grammar->extras[e].policy == TP_EXTRA_ERROR)
                        tp_diag_error(&state->parser->diagnostics, "extra.error",
                                      "error: unexpected input",
                                      state->position, end, "unexpected here");
                }
                /* An attached trivia / skip / error extra is consumed content of the
                ** enclosing node, so it advances the content high-water mark. */
                state->content_end = end;
            }
            state->position = end;
            progress = 1;
        }
        if (memoable && state->position == start)
            state->extras_memo.marks[start] = 1;
        return 1;
    }
    while (progress) {
        uint32_t e;
        progress = 0;
        for (e = 0; e < grammar->extra_count; e++) {
            const tp_token_def *token =
                tp_vm_find_token(grammar, grammar->extras[e].token_symbol);
            uint32_t end;
            if (token == 0)
                continue;
            /* W16-GOAL: match the extra token WITHOUT the per-position token memo
            ** (pass 0). The extras scan has its own per-position memo
            ** (extras_memo.marks) that short-circuits a fully-revisited offset, and
            ** the rule memo bounds re-descent once a parse goes super-linear, so the
            ** token memo's probe/store here was near-pure churn -- measured ~6%
            ** faster on the React corpus to skip it. The verdict is identical
            ** (the memo is a pure cache), so cc-diff stays byte-for-byte. The
            ** terminal path (tp_vm_terminal) KEEPS the token memo, where it hits ~43%
            ** of the time on the same corpus and genuinely saves re-matches. */
            if (!tp_token_match_memo(state->parser->engine, token->pattern,
                                     state->input, state->length, state->position,
                                     state->active_goal, 0, &end))
                continue;
            if (end == state->position)
                continue;
            if (grammar->extras[e].policy == TP_EXTRA_TRIVIA &&
                (grammar->extras[e].attach == TP_EXTRA_ATTACH_LEADING ||
                 state->held_count != 0)) {
                /* `attach: leading', or any trivia after a held comment: hold it (see
                ** the baked branch above); content_end does NOT advance across it. */
                if (!tp_vm_hold_extra(state, token->symbol, state->position, end))
                    return 0;
            } else {
                if (grammar->extras[e].policy != TP_EXTRA_SKIP) {
                    uint32_t node = tp_tree_builder_push(
                        &state->parser->builder, token->symbol, TP_NO_SYMBOL,
                        state->position, end, TP_NO_INDEX);
                    if (node == TP_NO_INDEX)
                        return 0;
                    if (sink != 0 && !tp_vm_sink_push(&state->parser->builder, state->parser->arena, sink, node))
                        return 0;
                    if (grammar->extras[e].policy == TP_EXTRA_ERROR)
                        tp_diag_error(&state->parser->diagnostics, "extra.error",
                                      "error: unexpected input",
                                      state->position, end, "unexpected here");
                }
                state->content_end = end; /* attached/skip/error: consumed content */
            }
            state->position = end;
            progress = 1;
            break;
        }
    }
    if (memoable && state->position == start)
        state->extras_memo.marks[start] = 1;
    return 1;
}

/* ** Synthesize the active recover node's insert: a zero-width MISSING node of the
** recover kind at the cursor, appended to the sink, plus the author-supplied
** diagnostic on the first fire. */
int tp_vm_recover_insert(tp_vm_state *state, tp_vm_sink *sink)
{
    const tp_ir *r = state->recover;
    uint32_t     kind = r->symbol;
    uint32_t     at = state->position;
    uint32_t     node = tp_tree_builder_push(&state->parser->builder, kind,
                                             TP_NO_SYMBOL, at, at, TP_NO_INDEX);
    if (node == TP_NO_INDEX)
        return 0;
    state->parser->builder.nodes[node].flags |= TP_NODE_MISSING;
    if (sink != 0 && !tp_vm_sink_push(&state->parser->builder, state->parser->arena, sink, node))
        return 0;
    if (at > state->content_end) /* a recovery insert is materialized content */
        state->content_end = at;
    if (!state->recover_fired) {
        state->recover_fired = 1;
        tp_diag_recover(&state->parser->diagnostics, r->recover_message,
                        r->recover_help, r->recover_label, at, at);
    }
    return 1;
}

/* ** Link a vector of child indices into a parent node, wiring each child's
** parent/sibling links and extending the parent's byte span to cover them. */
void tp_vm_link_children(tp_tree_builder *builder, uint32_t parent,
                         const tp_vm_sink *children)
{
    tp_node *p = &builder->nodes[parent];
    uint32_t i;
    p->child_count = children->count;
    p->first_child = children->count > 0 ? children->items[0] : TP_NO_INDEX;
    for (i = 0; i < children->count; i++) {
        tp_node *child = &builder->nodes[children->items[i]];
        child->parent = parent;
        child->next_sibling = (i + 1 < children->count)
                                  ? children->items[i + 1] : TP_NO_INDEX;
    }
    if (children->count > 0) {
        uint32_t first = children->items[0];
        uint32_t last  = children->items[children->count - 1];
        uint32_t s = builder->nodes[first].start_byte;
        uint32_t en = builder->nodes[last].end_byte;
        if (s < p->start_byte)
            p->start_byte = s;
        if (en > p->end_byte)
            p->end_byte = en;
    }
}

/* ** Emit a rule/token trace event by symbol name, gated on a plug being
** installed (with no plug the seam costs one pointer compare). */
void tp_vm_trace_symbol(tp_vm_state *state, const char *event,
                        uint32_t symbol)
{
    tinyparse_string name;
    if (state->parser->engine->plug_count == 0)
        return;
    name = tp_intern_name(&state->grammar->interner, symbol);
    tp_trace(state->parser->engine, event, name);
}

/* ** Match a terminal at the cursor after consuming leading extras and emit a
** leaf, exactly as tp_parse_terminal does. Returns 1 on a match (cursor advanced,
** leaf emitted into `sink'), 0 on no-match, -1 on allocation failure. */
static int tp_vm_terminal(tp_vm_state *state, const tp_ir *pattern, uint32_t kind,
                          tp_vm_sink *sink, uint32_t field, uint32_t alias)
{
    uint32_t end;
    uint32_t node;
    uint32_t emit_kind = (alias != TP_NO_SYMBOL) ? alias : kind;
    if (pattern == 0)
        return 0;
    if (!tp_vm_consume_extras(state, sink))
        return -1;
    /* W16-GOAL: match the terminal WITHOUT the per-position token memo (pass 0).
    ** On a clean predictive parse the first-byte dispatch + jump-threaded token
    ** bytecode (tp_token_run) is so cheap that the memo's hash, random-access slot
    ** probe, and store cost MORE than simply re-running the matcher -- and the memo
    ** churn thrashes the cache. Measured ~38% faster on the React corpus to skip it.
    ** The verdict is identical (a pure cache), so cc-diff stays byte-for-byte.
    ** Pathological backtracking is still bounded: once a parse goes super-linear the
    ** rule memo (tp_rule_memo) wakes and short-circuits re-descent, so a terminal is
    ** not re-matched unboundedly even without its own memo. */
    if (!tp_token_match_memo(state->parser->engine, (tp_ir *)pattern, state->input,
                             state->length, state->position, state->active_goal,
                             0, &end))
        return 0;
    node = tp_tree_builder_push(&state->parser->builder, emit_kind, field,
                                state->position, end, TP_NO_INDEX);
    if (node == TP_NO_INDEX)
        return -1;
    state->position = end;
    state->content_end = end; /* a real token leaf is materialized content */
    tp_vm_trace_symbol(state, "token", emit_kind);
    if (sink && !tp_vm_sink_push(&state->parser->builder, state->parser->arena, sink, node))
        return -1;
    return 1;
}

/* ** Match a terminal whose pattern is matched by a BAKED per-token matcher
** (W17-2): the generated parser's scan_<token> functions reproduce tp_match_ir for
** their token's pattern as straight-line C and pass that pure matcher here, so the
** generic token VM (tp_token_run, the ~30% profile slice) is replaced by the
** straight-line code a hand lexer is. Byte-identical to tp_vm_terminal by
** construction: the extras consume, the lexical-goal redirect, the leaf push and the
** cursor advance are this same shared logic; only the inner "where does the token end"
** call changes -- and the baked matcher returns the IDENTICAL end offset tp_token_run
** would (both mirror tp_match_ir). `kind' is the token's kind symbol and `pattern'
** its IR (needed to detect a goal redirect: a redirect keys on the token symbol, and
** when one is active the redirected token is matched through the generic path, exactly
** as tp_vm_do_match_pattern does -- the baked fast path covers only the common,
** no-redirect case, so soundness is total). Returns 1 match, 0 no-match, -1 OOM. */
int tp_vm_terminal_baked(tp_vm_state *state, tp_ir *pattern, uint32_t kind,
                         tp_vm_sink *sink, uint32_t field, uint32_t alias,
                         int (*matchfn)(const tinyparse_engine *,
                                        const uint8_t *, uint32_t, uint32_t,
                                        uint32_t *))
{
    uint32_t end;
    uint32_t node;
    uint32_t emit_kind = (alias != TP_NO_SYMBOL) ? alias : kind;
    /* tp_vm_extras is the inline fast-reject wrapper (W17-GOAL): the dominant
    ** "no extra at this byte" position is proven and skipped inline, byte-identically
    ** to tp_vm_consume_extras, without the cross-TU call. This path is generated-only
    ** (the VM's terminal keeps tp_vm_consume_extras, the cc-diff reference). */
    if (!tp_vm_extras(state, sink))
        return -1;
    /* A lexical-goal redirect (the regex-vs-divide / heredoc mechanism) keys on the
    ** token symbol. When one is active for this kind, the SAME byte scans as a
    ** different token, so the baked matcher (which is this token's pattern) does not
    ** apply -- fall back to the generic terminal on the redirect's pattern, exactly as
    ** tp_vm_do_match_pattern does. This is cold (only inside a (goal ...) scope). */
    if (kind != TP_NO_SYMBOL && state->active_goal != TP_NO_SYMBOL) {
        const tp_token_def *redirect;
        const tp_ir *param_pat;
        uint32_t param_kind;
        /* Phase 8: parametrized goal entry first (the live heredoc delimiter). */
        if (tp_vm_goal_redirect_param(state, kind, &param_pat, &param_kind))
            return tp_vm_terminal(state, param_pat, param_kind,
                                  sink, field, alias);
        redirect = tp_vm_goal_redirect(state, kind);
        if (redirect != 0)
            return tp_vm_terminal(state, redirect->pattern, redirect->symbol,
                                  sink, field, alias);
    }
    (void)pattern;
    if (!matchfn(state->parser->engine, state->input, state->length,
                 state->position, &end))
        return 0;
    node = tp_tree_builder_push(&state->parser->builder, emit_kind, field,
                                state->position, end, TP_NO_INDEX);
    if (node == TP_NO_INDEX)
        return -1;
    state->position = end;
    state->content_end = end; /* a real token leaf is materialized content */
    tp_vm_trace_symbol(state, "token", emit_kind);
    if (sink && !tp_vm_sink_push(&state->parser->builder, state->parser->arena, sink, node))
        return -1;
    return 1;
}

/* ** Run a MATCH given its pattern/kind directly (the instruction-free form the
** generated recursive descent calls; the VM's TP_VM_MATCH case forwards to it).
** A token reference, literal, or unresolved reference: a token reference applies
** the active lexical-goal redirect; a bare literal does not. Returns 1 match,
** 0 no-match, -1 OOM. */
int tp_vm_do_match_pattern(tp_vm_state *state, tp_ir *pattern, uint32_t kind,
                           tp_vm_sink *sink, uint32_t field, uint32_t alias)
{
    if (pattern == 0)
        return 0;
    /* Only a token reference (one whose symbol names a grammar token) is subject
    ** to goal redirection. A literal's pattern is its own bytes and its kind is the
    ** literal text; a bare literal is never goal-redirected. */
    if (kind != TP_NO_SYMBOL) {
        const tp_token_def *token = tp_vm_find_token(state->grammar, kind);
        if (token != 0 && token->pattern == pattern) {
            const tp_token_def *redirect;
            const tp_ir *param_pat;
            uint32_t param_kind;
            /* Phase 8: a parametrized goal entry takes precedence -- its live
            ** instantiated pattern (the captured heredoc delimiter) overrides the
            ** token's empty base pattern when one is on the stack. */
            if (tp_vm_goal_redirect_param(state, kind, &param_pat, &param_kind))
                return tp_vm_terminal(state, param_pat, param_kind,
                                      sink, field, alias);
            redirect = tp_vm_goal_redirect(state, kind);
            if (redirect != 0)
                return tp_vm_terminal(state, redirect->pattern, redirect->symbol,
                                      sink, field, alias);
        }
    }
    return tp_vm_terminal(state, pattern, kind, sink, field, alias);
}

/* ** Run the MATCH instruction: forwards to tp_vm_do_match_pattern. */
static int tp_vm_do_match(tp_vm_state *state, const tp_instruction *in,
                          tp_vm_sink *sink, uint32_t field, uint32_t alias)
{
    return tp_vm_do_match_pattern(state, in->pattern, in->symbol, sink,
                                  field, alias);
}

/* ** Run a byte-level op given its pattern directly (the instruction-free form the
** generated recursive descent calls): an anonymous one-byte operator used in a
** rule body (any/class/class-name/not/not-keyword). Consumes extras, matches, and
** advances WITHOUT emitting a node. Returns 1 match, 0 no-match, -1 OOM. */
int tp_vm_do_byte_pattern(tp_vm_state *state, tp_ir *pattern, tp_vm_sink *sink)
{
    uint32_t end;
    if (pattern == 0)
        return 0;
    if (!tp_vm_consume_extras(state, sink))
        return -1;
    /* W16-GOAL: like the terminal path, match WITHOUT the per-position token memo
    ** (pass 0). On the clean predictive parse the memo churned the cache faster than
    ** it saved re-matches; the verdict is identical (a pure cache), so cc-diff stays
    ** byte-for-byte. The rule memo still bounds pathological backtracking. */
    if (!tp_token_match_memo(state->parser->engine, pattern, state->input,
                             state->length, state->position, state->active_goal,
                             0, &end))
        return 0;
    state->position = end;
    state->content_end = end; /* matched bytes are content, though no node is emitted */
    return 1;
}

/* ** Run a TP_VM_BYTE instruction: forwards to tp_vm_do_byte_pattern. */
static int tp_vm_do_byte(tp_vm_state *state, const tp_instruction *in,
                         tp_vm_sink *sink)
{
    return tp_vm_do_byte_pattern(state, in->pattern, sink);
}

/* ** Evaluate a rule-level NOT's token-level guard PURELY: return 1 if the guard
** matches at the current position (so the NOT must FAIL), 0 otherwise (so the NOT's
** body runs). `guard' is child[0]'s base token pattern; `trigger' is its token
** symbol when the guard references a token (TP_NO_SYMBOL for a literal/class guard).
** A live parametrized goal (the heredoc delimiter) overrides the base pattern --
** the SAME tp_vm_goal_redirect_param / tp_vm_goal_redirect resolution the terminal
** match and the VM's TP_VM_NOT_RULE use, so a parametrized guard (HeredocDelimLine)
** resolves identically. The match does NOT advance state->position (pure lookahead).
** Both the VM dispatch and the generated parser call this so the guard is evaluated
** byte-for-byte the same (lex-goal-implementation, "Rule-level NOT"). */
int tp_vm_not_rule_guard(tp_vm_state *state, const tp_ir *guard, uint32_t trigger)
{
    const tp_ir *look = guard;
    uint32_t end;
    if (trigger != TP_NO_SYMBOL) {
        const tp_ir *param_pat;
        uint32_t param_kind;
        if (tp_vm_goal_redirect_param(state, trigger, &param_pat, &param_kind)) {
            look = param_pat;
        } else {
            const tp_token_def *redirect = tp_vm_goal_redirect(state, trigger);
            if (redirect != 0) look = redirect->pattern;
        }
    }
    return (look != 0) &&
        tp_token_match_memo(state->parser->engine, (tp_ir *)look,
                            state->input, state->length, state->position,
                            state->active_goal, 0, &end);
}

/* ** Run the EOF op: consume extras, then succeed iff at end of input. EOF is a
** finalizer that opens no node, so a trailing `attach: leading' comment scanned here
** would be held with nothing following to flush it: drain the buffer into the sink so
** an end-of-input doc comment lands as a trailing top-level sibling rather than being
** stranded. */
int tp_vm_do_eof(tp_vm_state *state, tp_vm_sink *sink)
{
    if (!tp_vm_consume_extras(state, sink))
        return TP_VM_OOM;
    if (!tp_vm_flush_held(state, sink))
        return TP_VM_OOM;
    return state->position >= state->length ? TP_VM_MATCH_OK : TP_VM_FAIL;
}

/* ** Run the INJECT op (W62), with the region body supplied as a callback. Consume
** leading extras, match the REGION (via `body') to delimit the span, discard the
** host nodes REGION produced (the embedded parse replaces them), then run the
** inline sub-VM splice under an injection node. `inject' is the inject IR node
** carrying the embedded grammar and language tag. Drives the VM's TP_VM_INJECT
** case. Returns the signal. */
int tp_vm_do_inject(tp_vm_state *state, tp_vm_sink *sink, tp_ir *inject,
                    int (*body)(tp_vm_state *, tp_vm_sink *,
                                uint32_t, uint32_t, void *),
                    void *body_user)
{
    uint32_t   saved_count;
    uint32_t   span_start;
    uint32_t   span_end;
    tp_vm_sink region_sink;
    int        sig;
    if (!tp_vm_consume_extras(state, sink))
        return TP_VM_OOM;
    saved_count = state->parser->builder.count;
    span_start  = state->position;
    region_sink.items = 0; region_sink.count = 0; region_sink.capacity = 0;
    region_sink.parent = TP_NO_INDEX;
    sig = body(state, &region_sink, TP_NO_SYMBOL, TP_NO_SYMBOL, body_user);
    if (sig == TP_VM_OOM)
        return TP_VM_OOM;
    if (sig != TP_VM_MATCH_OK)
        return TP_VM_FAIL;
    span_end = state->position;
    state->parser->builder.count = saved_count;
    {
        tp_vm_inject_sink bundle;
        bundle.builder = &state->parser->builder;
        bundle.arena   = state->parser->arena;
        bundle.sink    = sink;
        if (!tp_inject_run(state->parser, inject, span_start, span_end,
                           tp_vm_inject_sink_append, &bundle))
            return TP_VM_OOM;
    }
    return TP_VM_MATCH_OK;
}

/* ====================================================================== */
/* The dispatch loop.                                                      */
/* ====================================================================== */

/* ** The tp_vm_signal enum (matched/failed/OOM) and the tp_vm_frame / tp_vm_stack
** types now live in internal.h, shared with the generated compiled parser. */

/* ** Append one captured item onto a slot's FIFO, growing the arena-backed item
** array by doubling. arenas never realloc in place, so a full slot allocates a
** fresh block of twice the capacity and copies the existing items across; the old
** block is abandoned in the arena (it cannot be freed -- that is expected). The
** first push (items == NULL) allocates an initial block of 4 items. Returns 0 only
** on allocation failure (lex-goal-implementation, "Slot frame"). */
int tp_slot_push(tp_arena *arena, tp_slot_frame *slot, tp_deferred_item item)
{
    if (slot->items == 0) {
        slot->items = (tp_deferred_item *)tp_alloc(arena, 4 * sizeof *slot->items);
        if (slot->items == 0) return 0;
        slot->capacity = 4;
        slot->count    = 0;
    } else if (slot->count == slot->capacity) {
        uint32_t nc = slot->capacity * 2;
        tp_deferred_item *grown =
            (tp_deferred_item *)tp_alloc(arena, nc * sizeof *grown);
        uint32_t k;
        if (grown == 0) return 0;
        for (k = 0; k < slot->count; k++)
            grown[k] = slot->items[k];
        slot->items    = grown;
        slot->capacity = nc;
    }
    slot->items[slot->count++] = item;
    return 1;
}

/* ** Push an instantiated pattern onto a parametrized goal entry's stack, growing
** the arena-backed patterns array by doubling. arenas never realloc in place,
** so a full stack allocates a fresh block of twice the capacity and copies the
** existing pointers across; the old block is abandoned in the arena (it cannot be
** freed -- that is expected). The first push (patterns == NULL) allocates an
** initial block of 4 entries. Returns 0 only on allocation failure. Mirrors
** tp_slot_push's growth policy (lex-goal-implementation, "Goal-table stack"). */
int tp_param_stack_push(tp_arena *arena, tp_param_stack *stack, const tp_ir *pattern)
{
    if (stack->patterns == 0) {
        stack->patterns =
            (const tp_ir **)tp_alloc(arena, 4 * sizeof *stack->patterns);
        if (stack->patterns == 0) return 0;
        stack->capacity = 4;
        stack->depth    = 0;
    } else if (stack->depth == stack->capacity) {
        uint32_t nc = stack->capacity * 2;
        const tp_ir **grown =
            (const tp_ir **)tp_alloc(arena, nc * sizeof *grown);
        uint32_t k;
        if (grown == 0) return 0;
        for (k = 0; k < stack->depth; k++)
            grown[k] = stack->patterns[k];
        stack->patterns = grown;
        stack->capacity = nc;
    }
    stack->patterns[stack->depth++] = pattern;
    return 1;
}

/* ** Pop the top instantiated pattern off a parametrized goal entry's stack. The
** arena block stays (append-only); only the depth shrinks. A pop on an empty stack
** is a defensive no-op (lex-goal-implementation, "Goal-table stack"). */
void tp_param_stack_pop(tp_param_stack *stack)
{
    if (stack->depth > 0)
        stack->depth--;
}

/* ** Allocate `slot_count' fresh slot frames and push them as the innermost active
** slot scope (Option A's unified resolution stack). Each frame starts unclaimed
** (slot_name == TP_NO_SYMBOL -- not 0, a valid interned id) with an empty FIFO;
** TP_OP_DEFERRED_SLOT claims a frame, TP_OP_DEFER fills it, TP_OP_PARSE_DEFERRED
** drains it. The scope array (tp_slot_scope entries) grows by doubling off the
** parse arena; arenas never realloc, so a full array allocates a fresh, larger
** block and copies. Returns the slot array, or NULL on any allocation failure (the
** caller maps that to TP_VM_OOM). slot_count is always > 0 at the call sites
** (lex-goal-implementation, "Slot name resolution"). */
tp_slot_frame *tp_slot_scope_push(tinyparse_parser *parser, tp_arena *arena,
                                  uint32_t slot_count)
{
    tp_slot_frame *slots;
    uint32_t k;
    slots = (tp_slot_frame *)tp_alloc(arena, slot_count * sizeof *slots);
    if (slots == 0)
        return 0;
    for (k = 0; k < slot_count; k++) {
        slots[k].slot_name = TP_NO_SYMBOL;
        slots[k].items     = 0;
        slots[k].count     = 0;
        slots[k].capacity  = 0;
    }
    if (parser->slot_scopes == 0) {
        parser->slot_scopes = (tp_slot_scope *)tp_alloc(
            arena, 8 * sizeof *parser->slot_scopes);
        if (parser->slot_scopes == 0)
            return 0;
        parser->slot_scope_capacity = 8;
        parser->slot_scope_count    = 0;
    } else if (parser->slot_scope_count == parser->slot_scope_capacity) {
        uint32_t nc = parser->slot_scope_capacity * 2;
        tp_slot_scope *grown =
            (tp_slot_scope *)tp_alloc(arena, nc * sizeof *grown);
        if (grown == 0)
            return 0;
        for (k = 0; k < parser->slot_scope_count; k++)
            grown[k] = parser->slot_scopes[k];
        parser->slot_scopes         = grown;
        parser->slot_scope_capacity = nc;
    }
    parser->slot_scopes[parser->slot_scope_count].slots      = slots;
    parser->slot_scopes[parser->slot_scope_count].slot_count = slot_count;
    parser->slot_scope_count++;
    return slots;
}

/* ** Pop the innermost active slot scope (pairs with tp_slot_scope_push). The arena
** memory stays (append-only); only the scope count shrinks. A pop on an empty stack
** is a defensive no-op. */
void tp_slot_scope_pop(tinyparse_parser *parser)
{
    if (parser->slot_scope_count > 0)
        parser->slot_scope_count--;
}

/* ** Allocate a parser's per-parametrized-goal stack array: one zeroed tp_param_stack
** per parametrized goal entry (grammar->param_goal_count). The count is precomputed
** at grammar-compile time and on .tpg load, so no scan of lex_goals[] is needed
** here. tp_alloc zeroes the block, but the fields are set explicitly for clarity.
** Writes the array (or 0 when the grammar has no parametrized goals) to *out;
** returns 0 only on allocation failure (lex-goal-implementation,
** "Pattern instantiation"). Shared by tinyparse_parser_new and the inject.c
** sub-parser so the host and an embedded grammar are initialized identically. */
int tp_param_stacks_alloc(tp_arena *arena, const tinyparse_grammar *grammar,
                          tp_param_stack **out)
{
    uint32_t i;

    if (grammar->param_goal_count == 0) {
        *out = 0;
        return 1;
    }

    *out = (tp_param_stack *)tp_alloc(
        arena, grammar->param_goal_count * sizeof **out);
    if (*out == 0)
        return 0;

    for (i = 0; i < grammar->param_goal_count; i++) {
        (*out)[i].patterns = 0;
        (*out)[i].depth    = 0;
        (*out)[i].capacity = 0;
    }
    return 1;
}

/* ** Push a fresh frame for `program' onto the arena-backed work stack, growing it
** geometrically (arenas never realloc in place, so growth copies). The new
** frame's step starts at 0; its scratch marks are zeroed lazily by the op that
** uses them. Returns 0 only on allocation failure. Byte-for-byte the same growth
** policy the generated parser used. */
int tp_vm_stack_push(tp_arena *arena, tp_vm_stack *stack, uint32_t program,
                     tp_vm_sink *sink, uint32_t field, uint32_t alias)
{
    tp_vm_frame *fr;
    if (stack->count == stack->capacity) {
        uint32_t nc = stack->capacity == 0 ? 64 : stack->capacity * 2;
        /* RAW: the live prefix [0,count) is copied in below and each new frame is
        ** fully written by the push that claims it before any read (W16-1). */
        tp_vm_frame *grown =
            (tp_vm_frame *)tp_alloc_raw(arena, nc * sizeof *grown);
        uint32_t k;
        if (grown == 0)
            return 0;
        for (k = 0; k < stack->count; k++)
            grown[k] = stack->frames[k];
        stack->frames   = grown;
        stack->capacity = nc;
    }
    fr = &stack->frames[stack->count++];
    fr->program = program;
    fr->step    = 0;
    fr->sink    = sink;
    fr->field   = field;
    fr->alias   = alias;
    /* Frames are tp_alloc_raw (indeterminate); every field a later read can reach
    ** must be written before the read (W16-1). A non-CALL frame never owns slots,
    ** and the CALL descend path overwrites these from the annotated count. */
    fr->slot_count = 0;
    fr->slots      = 0;
    return 1;
}

/* ** CALL prologue, factored out of the old recursive tp_vm_run_call_body so the
** iterative driver can run the rule body on the SAME explicit stack. It does the
** memo-attempt count, the rule-memo probe (resolving a cached verdict
** immediately), the leading-extras consume (TP_OP_REF consumes them into the
** caller's sink before the node opens), and the node open -- everything up to
** running the body. Returns 1 to descend into the body (the frame holds the marks
** for the epilogue), or 0 to resolve the CALL now with *resolved set. Reproduces
** tp_vm_run_call_body's prologue byte-for-byte; the generated parser calls it too. */
int tp_vm_call_enter(tp_vm_state *state, tp_vm_frame *fr,
                     uint32_t target, uint32_t symbol, tp_vm_sink *sink,
                     uint32_t field, uint32_t alias, int *resolved)
{
    fr->kind        = (alias != TP_NO_SYMBOL) ? alias : symbol;
    fr->saved_count = state->parser->builder.count;
    fr->saved_pos   = state->position;
    fr->saved_sink  = sink ? sink->count : 0;
    /* Held-trivia high-water mark and content-end mark, captured before any
    ** leading-extras consume (which may flush / advance content_end); the failure path
    ** in tp_vm_call_exit restores both. See tp_vm_state.held / content_end. */
    fr->saved_held    = state->held_count;
    fr->saved_content = state->content_end;
    /* W17-FLAT / W17-RECLAIM: read both rule marks with one inline index probe.
    ** Bit 0 (TP_RULE_HIDDEN) makes a no-alias call a collapse candidate (see
    ** tp_vm_call_exit). Bit 1 (TP_RULE_TRANSPARENT_ALWAYS) additionally lets the
    ** prologue skip the wrapper node and run the body into the caller's sink, set
    ** below once the memo probe has had its chance -- it is disabled while the memo
    ** is eligible, since a memoed transparent success would replay nothing (no
    ** node), so the rule keeps the exit-time collapse on the rare memo-active path. */
    {
        uint32_t marks = (alias == TP_NO_SYMBOL)
                         ? tp_grammar_rule_marks(state->grammar, symbol) : 0;
        fr->hidden      = (marks & TP_RULE_HIDDEN) != 0;
        fr->transparent = 0; /* armed below, after the memo probe, if bit 1 is set */
        fr->seen_transparent_mark = (marks & TP_RULE_TRANSPARENT_ALWAYS) != 0;
    }
    fr->target      = target;
    fr->memo_pos    = state->position;
    fr->diag_before = state->parser->diagnostics.count;
    fr->seen_success = 0;
    /* Count this attempt and, once the parse proves super-linear, switch the memo
    ** on for the remainder -- allocating its input-scaled tables lazily on that
    ** first activation (W17-GOAL), so a clean parse never builds or zeroes them. */
    if (!state->memo_active && ++state->call_attempts > state->memo_threshold) {
        state->memo_active = 1;
        /* Traced so gate:vm-diff can see the adaptive memo woke mid-parse. */
        tp_vm_trace_symbol(state, "rule.memo-on", fr->kind);
        tp_rule_memo_ensure(&state->rule_memo);
        tp_extras_memo_ensure(&state->extras_memo);
    }
    /* The rule memo is consulted only when no recovery is armed, no no_extras
    ** scope is in force, no `attach: leading' comment is held across this call
    ** boundary, and (below) the body emits no diagnostic. The held-buffer guard is
    ** the soundness fix for the held buffer: a memoed SUCCESS replay
    ** copies a frozen subtree and advances the cursor WITHOUT re-running the body, so
    ** it can neither flush a comment that was held at entry (the replay would skip the
    ** flush) nor reproduce one the body left held at exit (the snapshot did not freeze
    ** it). Refusing to memo while the buffer is non-empty -- at entry here, and at exit
    ** in tp_vm_call_exit -- removes both hazards. The buffer is non-empty only across a
    ** doc-comment boundary, and the rule memo only wakes on a super-linear blowup
    ** (never on the generated parser at all), so this costs nothing on the hot path. */
    fr->memoable = (state->memo_active && state->recover == 0 &&
                    state->no_extras == 0 && state->held_count == 0 &&
                    state->rule_memo.capacity != 0);
    if (fr->memoable) {
        tp_rule_slot *hit = tp_rule_memo_probe(&state->rule_memo, target,
                                               fr->memo_pos, state->active_goal,
                                               field, alias);
        if (tp_rule_slot_live(&state->rule_memo, hit)) {
            if (hit->matched == TP_RULE_FAIL) { *resolved = TP_VM_FAIL; return 0; }
            if (hit->matched == TP_RULE_SUCCESS) {
                if (!tp_vm_consume_extras(state, sink)) { *resolved = TP_VM_OOM; return 0; }
                /* Traced so gate:vm-diff can see the replay path ran: success
                ** replays are rare (a third attempt at one key), and a corpus
                ** that never reaches one would hold nothing to the memo-off tree. */
                tp_vm_trace_symbol(state, "rule.replay", fr->kind);
                if (!tp_vm_replay_success(state, sink, hit)) { *resolved = TP_VM_OOM; return 0; }
                *resolved = TP_VM_MATCH_OK; return 0;
            }
            fr->seen_success = 1; /* TP_RULE_SEEN: re-parse and snapshot now */
        }
    }
    if (!tp_vm_consume_extras(state, sink)) { *resolved = TP_VM_OOM; return 0; }
    /* If THIS call's leading-extras consume held an `attach: leading' comment, the
    ** memoable decision above (taken before the consume) is now stale: a memoed
    ** success would freeze a subtree whose still-held comment a replay could not
    ** reproduce, and keeping the wrapper node (the `memoable' branch below) would trap
    ** the comment inside it instead of letting it float to the parent. Demote to
    ** non-memoable so the transparent fast-path runs and no success is stored -- the
    ** exit-time store is already gated on held_count, but demoting here also restores
    ** the byte-identical collapse the forced-memo path would otherwise lose. */
    if (state->held_count != 0)
        fr->memoable = 0;
    /* W17-RECLAIM: a TRANSPARENT_ALWAYS rule always collapses to its single child,
    ** so off the memo path skip the wrapper node entirely -- run the body straight
    ** into the caller's sink (no separate child sink, no tp_tree_builder_push), and
    ** let tp_vm_call_exit re-stamp the call's field onto that child. The leading
    ** extras were just consumed into the caller's sink, exactly as the node-open
    ** path would have. Memoable calls keep the node + exit-collapse so the memo's
    ** subtree snapshot/replay sees a real root (see tp_vm_call_exit). */
    if (!fr->memoable && fr->seen_transparent_mark) {
        fr->transparent = 1;
        fr->children     = sink;
        fr->start        = state->position;
        /* fr->node holds the sink index of the body's first contributed node,
        ** captured AFTER the leading extras above so it names the significant child
        ** the exit re-stamps, not a leading trivia node. saved_sink stays pre-extras
        ** for the failure rollback (see tp_vm_call_exit). */
        fr->node         = sink ? sink->count : 0;
        tp_vm_trace_symbol(state, "rule.enter", fr->kind);
        return 1;
    }
    /* The rule body's child sink. When the caller's frame is on a stable address
    ** (the generated native recursive-descent parser -- state->children_inline), use
    ** the frame's inline storage and skip the per-rule heap allocation entirely
    ** (W16-GOAL: one of the hottest per-rule allocations). The VM keeps its frames on
    ** a relocatable arena stack, so it allocates the sink separately (a pointer into
    ** a frame would dangle when the stack grows). Allocated RAW either way: the three
    ** fields are overwritten immediately, so zeroing would be a discarded memset. */
    if (state->children_inline) {
        fr->children = &fr->children_storage;
    } else {
        fr->children = (tp_vm_sink *)tp_alloc_raw(state->parser->arena,
                                                  sizeof *fr->children);
        if (fr->children == 0) { *resolved = TP_VM_OOM; return 0; }
    }
    fr->children->items = 0; fr->children->count = 0; fr->children->capacity = 0;
    /* Open at the last MATERIALIZED byte, not the raw cursor: if this rule's leading-
    ** extras consume HELD a comment (held_count != 0), the cursor sits past it but the
    ** comment belongs to the next sibling, so a node that then matches empty (a trailing
    ** optional `semi') must not open past the comment and drag its span across it.
    ** content_end == state->position whenever nothing is held, so the default is
    ** unchanged. See tp_vm_state.content_end. */
    fr->start = state->content_end;
    fr->node  = tp_tree_builder_push(&state->parser->builder, fr->kind, field,
                                     fr->start, fr->start, TP_NO_INDEX);
    if (fr->node == TP_NO_INDEX) { *resolved = TP_VM_OOM; return 0; }
    /* W17-CST: name the parent so the body's children link inline as they emit. */
    fr->children->parent = fr->node;
    tp_vm_trace_symbol(state, "rule.enter", fr->kind);
    return 1;
}

/* ** CALL epilogue, factored out of tp_vm_run_call_body: after the body returns
** `sig', on failure restore the marks and cache the failure; on success link the
** children, close the span, push the node, and cache/snapshot the success.
** Returns the signal the CALL frame resolves with. Byte-for-byte the same as the
** old function's tail; the generated parser calls it too. */
int tp_vm_call_exit(tp_vm_state *state, tp_vm_frame *fr, tp_vm_sink *sink,
                    uint32_t field, uint32_t alias, int sig)
{
    if (sig == TP_VM_OOM)
        return TP_VM_OOM;
    if (sig != TP_VM_MATCH_OK) {
        state->parser->builder.count = fr->saved_count;
        state->position = fr->saved_pos;
        if (sink)
            sink->count = fr->saved_sink;
        /* Restore the held-trivia high-water mark: a flush inside the failed body
        ** materialised spans into a sink that just rolled back, so re-expose them for
        ** the retry to re-flush (see tp_vm_state.held). The content-end mark rolls back
        ** with the cursor so the retry recomputes the node extent from scratch. */
        state->held_count   = fr->saved_held;
        state->content_end  = fr->saved_content;
        /* Cache the failure only if the body left no diagnostic behind (a memo hit
        ** short-circuits before re-running the body, so it must reproduce no side
        ** effect). */
        if (fr->memoable && state->parser->diagnostics.count == fr->diag_before) {
            tp_rule_slot *slot = tp_rule_memo_probe(&state->rule_memo, fr->target,
                                                    fr->memo_pos, state->active_goal,
                                                    field, alias);
            tp_rule_memo_store(&state->rule_memo, slot, fr->target, fr->memo_pos,
                               state->active_goal, field, alias, 0, 0, 0, 0);
        }
        return TP_VM_FAIL;
    }
    /* W17-RECLAIM: a transparent rule pushed no node and ran its body into the
    ** caller's sink, so its single child already sits at sink[saved_sink], wired to
    ** the real parent. Re-stamp the call's field onto it -- byte-identical to the
    ** collapse below, with the wrapper allocation reclaimed. No memo store: this
    ** path is taken only when !memoable. */
    if (fr->transparent) {
        if (sink && sink->count > fr->node)
            state->parser->builder.nodes[sink->items[fr->node]].field = field;
        tp_vm_trace_symbol(state, "rule.exit", fr->kind);
        return TP_VM_MATCH_OK;
    }
    /* W17-FLAT: a hidden rule that was a pure pass-through (one child) collapses --
    ** promote its single child in place. The wrapper node is orphaned, so a memo of
    ** this success would snapshot the wrong (un-collapsed) subtree; the memo is a
    ** pure cache, so we simply do not store a collapsed success (return early). Both
    ** engines run the SAME collapse here, so they stay byte-identical. */
    if (fr->hidden && fr->children->count == 1) {
        tp_vm_trace_symbol(state, "rule.exit", fr->kind);
        if (!tp_vm_collapse_hidden(state, fr, sink, field))
            return TP_VM_OOM;
        return TP_VM_MATCH_OK;
    }
    tp_vm_sink_close(&state->parser->builder, fr->node, fr->children);
    /* Close the node's extent at the last MATERIALIZED byte, not the raw cursor: a
    ** comment captured during this node's (or a descendant's) trailing-element consume
    ** advanced the cursor past it before it floated out as a sibling, so state->position
    ** over-extends. content_end excludes that held trivia and equals state->position
    ** whenever nothing is held, so the `trailing'-only default stays byte-identical.
    ** Never shrink below what sink_close set from the children (content_end >= every
    ** materialized child's end), so a held-only childless node keeps its start. */
    if (state->content_end > state->parser->builder.nodes[fr->node].end_byte)
        state->parser->builder.nodes[fr->node].end_byte = state->content_end;
    tp_vm_trace_symbol(state, "rule.exit", fr->kind);
    if (sink && !tp_vm_sink_push(&state->parser->builder, state->parser->arena,
                                 sink, fr->node))
        return TP_VM_OOM;
    /* Cache the success, but only for non-trivial subtrees and only if the body
    ** left no diagnostic. The first sizable success is merely MARKED seen; the
    ** second snapshots its subtree so later attempts replay it -- so a clean parse,
    ** which sees each key once, never pays the snapshot copy. A body that ends with a
    ** held `attach: leading' comment (held_count != 0) is NOT cached: the snapshot
    ** froze the subtree without that still-held comment, so a replay -- which advances
    ** the cursor without re-running the body -- could not reproduce it. (Soundness; see
    ** the memoable gate in tp_vm_call_enter.) */
    if (fr->memoable && state->held_count == 0 &&
        state->parser->diagnostics.count == fr->diag_before &&
        state->parser->builder.count - fr->node >= TP_RULE_MEMO_MIN_SUBTREE) {
        tp_rule_slot *slot = tp_rule_memo_probe(&state->rule_memo, fr->target,
                                                fr->memo_pos, state->active_goal,
                                                field, alias);
        if (fr->seen_success) {
            uint32_t snap_count = 0;
            tp_node *snap = tp_vm_snapshot_subtree(&state->parser->builder, fr->node,
                                                   state->parser->builder.count,
                                                   &snap_count);
            if (snap != 0)
                tp_rule_memo_store(&state->rule_memo, slot, fr->target, fr->memo_pos,
                                   state->active_goal, field, alias,
                                   TP_RULE_SUCCESS, state->position, snap, snap_count);
        } else {
            tp_rule_memo_store(&state->rule_memo, slot, fr->target, fr->memo_pos,
                               state->active_goal, field, alias, TP_RULE_SEEN, 0, 0, 0);
        }
    }
    return TP_VM_MATCH_OK;
}

/* ** The INJECT region body callback: tp_vm_do_inject calls this to match the
** region. The region is a single child program, so it runs through a nested
** tp_vm_run driver call -- bounded by injection nesting depth (host languages
** embedded in host languages), not by input nesting, so it cannot overflow the C
** stack on a deeply nested ordinary input (that runs on the one explicit stack). */
static int tp_vm_inject_body_run(tp_vm_state *state, tp_vm_sink *children,
                                 uint32_t field, uint32_t alias, void *body_user)
{
    uint32_t region = *(const uint32_t *)body_user;
    (void)field; (void)alias;
    return tp_vm_run(state, region, children, TP_NO_SYMBOL, TP_NO_SYMBOL);
}

/* ** Find the slot frame for a DEFERRED_SLOT declaration and claim it under
** `slot_name'. The enclosing rule invocation is the nearest frame below the
** declaration's own top frame whose instruction is a TP_VM_CALL with allocated
** slots. Within that frame, an entry already named `slot_name' is reused
** (idempotent re-init); otherwise the first unclaimed entry (slot_name ==
** TP_NO_SYMBOL -- not 0, which is a valid interned id) is claimed. Returns 0 only
** if no enclosing CALL frame has a free slot, which means the compile-time
** slot_count under-counted the declarations -- an internal invariant break that
** should never happen (lex-goal-implementation, "Slot name resolution"). */
int tp_slot_claim(tp_vm_state *state, uint32_t slot_name)
{
    tinyparse_parser *parser = state->parser;
    uint32_t si = parser->slot_scope_count;
    if (si == 0)
        return 0;
    {
        /* The innermost scope is THIS rule invocation's own scope (pushed at its
        ** entry), so the declaration claims a frame there -- not in an enclosing
        ** rule. Scanning only the top scope matches the VM's old "nearest enclosing
        ** CALL frame" behaviour now that every invocation pushes exactly one scope. */
        tp_slot_scope *sc = &parser->slot_scopes[si - 1];
        uint32_t k;
        for (k = 0; k < sc->slot_count; k++)
            if (sc->slots[k].slot_name == slot_name)
                return 1; /* already declared this invocation; idempotent */
        for (k = 0; k < sc->slot_count; k++)
            if (sc->slots[k].slot_name == TP_NO_SYMBOL) {
                sc->slots[k].slot_name = slot_name;
                return 1;
            }
        return 0; /* no free slot: slot_count under-counted (invariant break) */
    }
}

/* ** Resolve a slot by name for DEFER (and Phase 4 PARSE_DEFERRED): scan frames
** innermost to outermost for the first TP_VM_CALL frame holding a slot named
** `slot_name', returning a pointer to that tp_slot_frame. This is the lexical-
** scope resolution the spec requires -- a (defer NAME ...) writes into the
** innermost enclosing rule that declared NAME. Returns NULL if no enclosing slot
** carries the name (lex-goal-implementation, "Slot name resolution"). */
tp_slot_frame *tp_slot_resolve(tp_vm_state *state, uint32_t slot_name)
{
    tinyparse_parser *parser = state->parser;
    uint32_t si = parser->slot_scope_count;
    while (si > 0) {
        tp_slot_scope *sc = &parser->slot_scopes[si - 1];
        uint32_t k;
        for (k = 0; k < sc->slot_count; k++)
            if (sc->slots[k].slot_name == slot_name)
                return &sc->slots[k];
        si--;
    }
    return 0;
}

/* ** Append one uint32 onto the parser's flat slot-save stack, growing the arena-
** backed array by doubling (initial capacity 16). arenas never realloc in
** place, so a full array allocates a fresh block of twice the capacity and copies
** the live prefix across; the old block is abandoned. Returns 0 only on
** allocation failure (lex-goal-implementation, "Parallel slot-save stack"). */
static int tp_slot_save_emit(tinyparse_parser *parser, uint32_t value)
{
    if (parser->slot_save_stack == 0) {
        parser->slot_save_stack = (uint32_t *)tp_alloc(
            parser->arena, 16 * sizeof *parser->slot_save_stack);
        if (parser->slot_save_stack == 0) return 0;
        parser->slot_save_capacity = 16;
        parser->slot_save_count    = 0;
    } else if (parser->slot_save_count == parser->slot_save_capacity) {
        uint32_t nc = parser->slot_save_capacity * 2;
        uint32_t *grown =
            (uint32_t *)tp_alloc(parser->arena, nc * sizeof *grown);
        uint32_t k;
        if (grown == 0) return 0;
        for (k = 0; k < parser->slot_save_count; k++)
            grown[k] = parser->slot_save_stack[k];
        parser->slot_save_stack    = grown;
        parser->slot_save_capacity = nc;
    }
    parser->slot_save_stack[parser->slot_save_count++] = value;
    return 1;
}

/* ** Save the state of every active slot before entering a speculative branch.
** Pushes a sentinel (TP_NO_INDEX) batch marker, then the `count' of every active
** slot in the SAME innermost-to-outermost frame order tp_slot_resolve uses
** (TP_VM_CALL frames with allocated slots only). On any allocation failure mid-
** batch the partial batch is abandoned -- slot_save_count is reset to its value
** before the push, so no malformed batch is ever read -- and 0 is returned. The
** matching pop is tp_slot_save_restore_pop (on the branch's failure) or
** tp_slot_save_commit_pop (on its success); each push pairs with exactly one pop
** (lex-goal-implementation, "Saving slot state on choice entry"). */
int tp_slot_save_push(tp_vm_state *state)
{
    tinyparse_parser *parser = state->parser;
    uint32_t start = parser->slot_save_count;
    uint32_t si;
    if (!tp_slot_save_emit(parser, TP_NO_INDEX)) {
        parser->slot_save_count = start;
        return 0;
    }
    for (si = parser->slot_scope_count; si > 0; si--) {
        tp_slot_scope *sc = &parser->slot_scopes[si - 1];
        uint32_t k;
        for (k = 0; k < sc->slot_count; k++)
            if (!tp_slot_save_emit(parser, sc->slots[k].count)) {
                parser->slot_save_count = start;
                return 0;
            }
    }
    return 1;
}

/* ** Restore every active slot's `count' from the top batch and pop it. Finds the
** nearest TP_NO_INDEX sentinel at index S below slot_save_count; the saved counts
** sit at S+1.. in innermost-first order. Re-scans the active slots in the SAME
** order and assigns each its saved count, then drops the batch (sentinel
** included) by setting slot_save_count = S. The failed branch's frames have
** already been popped, so the active-slot set matches push time. Truncating
** `count' makes items appended during the failed branch invisible (the arena
** memory is unreclaimable but unreachable). If the active-slot count and batch
** size disagree -- an invariant break that should never happen -- only the common
** prefix is restored, never reading past the sentinel (lex-goal-implementation,
** "Restoring slot state on backtrack"). */
void tp_slot_save_restore_pop(tp_vm_state *state)
{
    tinyparse_parser *parser = state->parser;
    uint32_t s = parser->slot_save_count;
    uint32_t j, si;
    while (s > 0 && parser->slot_save_stack[s - 1] != TP_NO_INDEX)
        s--;
    if (s == 0) return;            /* no sentinel: nothing was pushed */
    s--;                           /* s now indexes the sentinel itself */
    j = 0;
    for (si = parser->slot_scope_count; si > 0; si--) {
        tp_slot_scope *sc = &parser->slot_scopes[si - 1];
        uint32_t k;
        for (k = 0; k < sc->slot_count; k++) {
            if (s + 1 + j >= parser->slot_save_count) break;
            sc->slots[k].count = parser->slot_save_stack[s + 1 + j];
            j++;
        }
    }
    parser->slot_save_count = s;
}

/* ** Pop the top batch WITHOUT restoring -- the speculative branch committed, so
** its enqueued items stay in their slots. Finds the nearest TP_NO_INDEX sentinel
** at index S below slot_save_count and drops everything from S upward. Pairs with
** tp_slot_save_push as the success path (lex-goal-implementation, "Saving slot
** state on choice entry"). */
void tp_slot_save_commit_pop(tp_vm_state *state)
{
    tinyparse_parser *parser = state->parser;
    uint32_t s = parser->slot_save_count;
    while (s > 0 && parser->slot_save_stack[s - 1] != TP_NO_INDEX)
        s--;
    if (s == 0) return;            /* no sentinel: nothing was pushed */
    parser->slot_save_count = s - 1;
}

/* ** Run one lowered program to completion on an explicit, arena-backed frame
** stack (W14-6). This is the iterative transformation of the old recursive
** tp_vm_run + tp_vm_run_call: a root frame for `program' is pushed and the stack
** is pumped until it resolves. Each iteration runs the top frame's next step,
** which either RESOLVES it (set `signal', pop) or DESCENDS (save the resume step,
** push a child frame). There is NO C recursion across grammar nesting --
** SEQ/CHOICE/REPEAT/OPTIONAL/CALL all iterate on the arena stack -- so a deeply
** nested input grows the arena, never overflowing the host C stack. Each step
** reproduces, op for op, what the old recursive tp_vm_run did in the same order,
** so the concrete syntax tree is byte-identical (vm-diff proves it).
**
** TAIL-CALL OPTIMIZATION (W14-6): a CALL the lowerer marked `tail' (its signal is
** the enclosing rule's signal with no epilogue) REPLACES the resolving frame's
** continuation instead of stacking a new body frame above a now-useless CALL
** epilogue. The rule node is still opened and its epilogue still runs (the tree is
** unchanged), but the *enclosing* frame -- which would otherwise sit on the stack
** waiting only to forward this CALL's signal -- is popped first, so a right-
** recursive ladder (X -> Y (op X)?) keeps stack depth at the true nesting rather
** than the call-chain length. ~43-50% of CALLs in the real grammars are tail
** (see docs/peg-vs-glr.md, W14-6), so this measurably bounds the hot stack. */
int tp_vm_run(tp_vm_state *state, uint32_t program, tp_vm_sink *sink,
              uint32_t field, uint32_t alias)
{
    tp_vm_stack stack;
    int         signal = TP_VM_FAIL;
    tp_arena   *arena  = state->parser->arena;
#ifdef TP_VM_PROBE_DEPTH
    extern unsigned long tp_vm_peak_depth;
#endif
    stack.frames = 0; stack.count = 0; stack.capacity = 0;
    if (program == TP_NO_INDEX)
        return TP_VM_FAIL;
    if (!tp_vm_stack_push(arena, &stack, program, sink, field, alias))
        return TP_VM_OOM;

    while (stack.count > 0) {
        tp_vm_frame          *fr = &stack.frames[stack.count - 1];
#ifdef TP_VM_PROBE_DEPTH
        if (stack.count > tp_vm_peak_depth) tp_vm_peak_depth = stack.count;
#endif
        const tp_instruction *in = &state->program->instructions[fr->program];
        switch (in->op) {

        case TP_VM_MATCH:
            signal = tp_vm_do_match(state, in, fr->sink, fr->field, fr->alias);
            stack.count--; continue;

        case TP_VM_BYTE:
            signal = tp_vm_do_byte(state, in, fr->sink);
            stack.count--; continue;

        case TP_VM_EOF:
            signal = tp_vm_do_eof(state, fr->sink);
            stack.count--; continue;

        case TP_VM_HALT:
            signal = TP_VM_MATCH_OK; stack.count--; continue;

        case TP_VM_CALL:
            if (fr->step == 0) {
                int resolved = TP_VM_FAIL;
                if (!tp_vm_call_enter(state, fr, in->target, in->symbol,
                                      fr->sink, fr->field, fr->alias, &resolved)) {
                    signal = resolved; stack.count--; continue;
                }
                if (fr->target == TP_NO_INDEX) {
                    fr->step = 1; signal = TP_VM_FAIL; continue;
                }
                /* Tail-call optimization: if THIS CALL is in tail position, the
                ** enclosing frame exists only to forward this CALL's signal. We
                ** still need this CALL's own epilogue (the rule node / memo), so we
                ** descend into the body normally; the depth saving comes when this
                ** CALL's body in turn ends in a tail CALL, because each such CALL
                ** frame resolves and pops the moment its body resolves rather than
                ** keeping a stack of forwarding frames above it. The frame is set
                ** to step 1 so it resumes at tp_vm_call_exit; the body is pushed as
                ** a child. (Pointer fr may dangle after the push grows the stack;
                ** index back through stack.frames.) */
                /* Push this CALL invocation's deferred-slot scope from the annotated
                ** count BEFORE the body push (fr dangles after the push grows the
                ** stack). The scope is the UNIFIED resolution stack on the parser
                ** (Option A) -- the SAME stack the generated parser pushes onto at
                ** rule entry, so (defer …)/(parse-deferred …) and the slot-save
                ** rollback resolve identically in both executors. Only the descend
                ** path needs slots; the memo-replay / immediate-resolve fast paths
                ** (tp_vm_call_enter returned 0) skip the body entirely. fr->slot_count
                ** records that a scope was pushed so the exit pops exactly once
                ** (lex-goal-implementation, Option A). */
                if (in->slot_count > 0) {
                    if (tp_slot_scope_push(state->parser, arena,
                                           in->slot_count) == 0)
                        return TP_VM_OOM;
                    fr->slot_count = in->slot_count;
                } else {
                    fr->slot_count = 0;
                }
                fr->slots = 0;
                stack.frames[stack.count - 1].step = 1;
                if (!tp_vm_stack_push(arena, &stack, fr->target, fr->children,
                                      TP_NO_SYMBOL, TP_NO_SYMBOL))
                    return TP_VM_OOM;
                continue;
            } else {
                /* Pop this invocation's slot scope (pushed on the descend path) so
                ** the unified scope stack stays balanced -- exactly once per CALL
                ** that descended with slots (Option A). */
                if (fr->slot_count > 0)
                    tp_slot_scope_pop(state->parser);
                signal = tp_vm_call_exit(state, fr, fr->sink,
                                         fr->field, fr->alias, signal);
                stack.count--; continue;
            }

        case TP_VM_SEQ:
            /* Sequential children; recovery-insert a missing required element at
            ** end of input, exactly like the old tp_vm_run's TP_VM_SEQ. */
            if (fr->step == 0) {
                fr->i = 0;
            } else {
                if (signal == TP_VM_OOM) return TP_VM_OOM;
                if (signal != TP_VM_MATCH_OK) {
                    if (state->recover != 0 && state->position >= state->length &&
                        state->position > state->recover_start) {
                        if (!tp_vm_recover_insert(state, fr->sink)) return TP_VM_OOM;
                    } else {
                        signal = TP_VM_FAIL; stack.count--; continue;
                    }
                }
                fr->i++;
            }
            if (fr->i >= in->count) {
                signal = TP_VM_MATCH_OK; stack.count--; continue;
            }
            stack.frames[stack.count - 1].step = 1;
            if (!tp_vm_stack_push(arena, &stack,
                                  state->program->children[in->start + fr->i],
                                  fr->sink, TP_NO_SYMBOL, TP_NO_SYMBOL))
                return TP_VM_OOM;
            continue;

        case TP_VM_CHOICE:
            /* Ordered choice with W46 first-set prediction: disarm recovery for the
            ** speculative alternatives, consume the shared leading extras once, then
            ** try each admitted alternative restoring the marks on failure. */
            if (fr->step == 0) {
                fr->saved_count   = state->parser->builder.count;
                fr->saved_pos     = state->position;
                fr->saved_sink    = fr->sink ? fr->sink->count : 0;
                fr->saved_held    = state->held_count;
                fr->saved_content = state->content_end;
                fr->outer_recover = state->recover;
                state->recover = 0;
                if (!tp_vm_consume_extras(state, fr->sink)) {
                    state->recover = fr->outer_recover; return TP_VM_OOM;
                }
                fr->look = state->position;
                fr->i = 0;
            } else {
                if (signal == TP_VM_OOM) { state->recover = fr->outer_recover; return TP_VM_OOM; }
                if (signal == TP_VM_MATCH_OK) {
                    tp_slot_save_commit_pop(state);
                    state->recover = fr->outer_recover;
                    signal = TP_VM_MATCH_OK; stack.count--; continue;
                }
                tp_slot_save_restore_pop(state);
                state->parser->builder.count = fr->saved_count;
                state->position = fr->saved_pos;
                if (fr->sink) fr->sink->count = fr->saved_sink;
                state->held_count = fr->saved_held;
                state->content_end = fr->saved_content;
                fr->i++;
            }
            /* Skip alternatives the first-set prediction rejects, on the SAME IR
            ** node (the child instruction's `source') the prediction caches on. */
            while (fr->i < in->count) {
                uint32_t child = state->program->children[in->start + fr->i];
                const tp_instruction *cin = &state->program->instructions[child];
                if (cin->source != 0 &&
                    !tp_choice_first_admits_fast(state->grammar, cin->source,
                                                 state->input, state->length, fr->look)) {
                    fr->i++;
                    continue;
                }
                break;
            }
            if (fr->i >= in->count) {
                state->recover = fr->outer_recover;
                signal = TP_VM_FAIL; stack.count--; continue;
            }
            if (!tp_slot_save_push(state)) {
                state->recover = fr->outer_recover; return TP_VM_OOM;
            }
            stack.frames[stack.count - 1].step = 1;
            if (!tp_vm_stack_push(arena, &stack,
                                  state->program->children[in->start + fr->i],
                                  fr->sink, fr->field, fr->alias))
                return TP_VM_OOM;
            continue;

        case TP_VM_REPEAT:
            /* Repeat the target until it fails or makes no progress, exactly like
            ** the old tp_vm_run's TP_VM_REPEAT (zero-width guard included). */
            if (fr->step == 0) {
                fr->reps = 0;
                fr->outer_recover = state->recover;
                state->recover = 0;
            } else {
                if (signal == TP_VM_OOM) { state->recover = fr->outer_recover; return TP_VM_OOM; }
                if (signal != TP_VM_MATCH_OK) {
                    tp_slot_save_restore_pop(state);
                    state->parser->builder.count = fr->saved_count;
                    state->position = fr->saved_pos;
                    if (fr->sink) fr->sink->count = fr->saved_sink;
                    state->held_count = fr->saved_held;
                    state->content_end = fr->saved_content;
                    state->recover = fr->outer_recover;
                    signal = (fr->reps >= in->min ? TP_VM_MATCH_OK : TP_VM_FAIL);
                    stack.count--; continue;
                }
                if (state->position == fr->saved_pos) { /* zero-width guard */
                    tp_slot_save_commit_pop(state);
                    state->recover = fr->outer_recover;
                    signal = (fr->reps >= in->min ? TP_VM_MATCH_OK : TP_VM_FAIL);
                    stack.count--; continue;
                }
                tp_slot_save_commit_pop(state);
                fr->reps++;
            }
            if (in->max != 0 && fr->reps >= in->max) {
                state->recover = fr->outer_recover;
                signal = (fr->reps >= in->min ? TP_VM_MATCH_OK : TP_VM_FAIL);
                stack.count--; continue;
            }
            fr->saved_count = state->parser->builder.count;
            fr->saved_pos   = state->position;
            fr->saved_sink  = fr->sink ? fr->sink->count : 0;
            fr->saved_held  = state->held_count;
            fr->saved_content = state->content_end;
            if (!tp_slot_save_push(state)) {
                state->recover = fr->outer_recover; return TP_VM_OOM;
            }
            if (in->target == TP_NO_INDEX) {
                fr->step = 1; signal = TP_VM_FAIL; continue;
            }
            stack.frames[stack.count - 1].step = 1;
            if (!tp_vm_stack_push(arena, &stack, in->target, fr->sink,
                                  TP_NO_SYMBOL, TP_NO_SYMBOL))
                return TP_VM_OOM;
            continue;

        case TP_VM_OPTIONAL:
            if (fr->step == 0) {
                fr->saved_count   = state->parser->builder.count;
                fr->saved_pos     = state->position;
                fr->saved_sink    = fr->sink ? fr->sink->count : 0;
                fr->saved_held    = state->held_count;
                fr->saved_content = state->content_end;
                fr->outer_recover = state->recover;
                state->recover = 0;
                if (!tp_slot_save_push(state)) {
                    state->recover = fr->outer_recover; return TP_VM_OOM;
                }
                if (in->target == TP_NO_INDEX) {
                    fr->step = 1; signal = TP_VM_FAIL; continue;
                }
                stack.frames[stack.count - 1].step = 1;
                if (!tp_vm_stack_push(arena, &stack, in->target, fr->sink,
                                      fr->field, fr->alias))
                    return TP_VM_OOM;
                continue;
            } else {
                if (signal == TP_VM_OOM) { state->recover = fr->outer_recover; return TP_VM_OOM; }
                if (signal != TP_VM_MATCH_OK) {
                    tp_slot_save_restore_pop(state);
                    state->parser->builder.count = fr->saved_count;
                    state->position = fr->saved_pos;
                    if (fr->sink) fr->sink->count = fr->saved_sink;
                    state->held_count = fr->saved_held;
                    state->content_end = fr->saved_content;
                } else {
                    tp_slot_save_commit_pop(state);
                }
                state->recover = fr->outer_recover;
                signal = TP_VM_MATCH_OK; stack.count--; continue;
            }

        case TP_VM_FIELD:
            /* FIELD only decorates its child's emitted node with a field symbol;
            ** it has NO epilogue (the old recursive form simply returned the
            ** child's signal). So instead of pushing the child and keeping a
            ** do-nothing continuation frame, REUSE this frame in place for the
            ** child, decorated with the field -- a tail-call frame reuse that
            ** bounds stack depth at the true nesting rather than the wrapper chain
            ** (W14-6). A field-decorated rule reference in tail position is exactly
            ** where this fires. Byte-identical: the child runs with the same sink
            ** and the field passed down, as before. */
            if (in->target == TP_NO_INDEX) {
                signal = TP_VM_FAIL; stack.count--; continue;
            }
            fr->program = in->target;
            fr->step    = 0;
            fr->field   = in->symbol;
            /* fr->sink and fr->alias are inherited unchanged. */
            continue;

        case TP_VM_ALIAS:
            /* ALIAS, like FIELD, only re-tags its child's node and has no epilogue,
            ** so reuse this frame in place for the child decorated with the alias
            ** kind (W14-6 tail-call frame reuse). */
            if (in->target == TP_NO_INDEX) {
                signal = TP_VM_FAIL; stack.count--; continue;
            }
            fr->program = in->target;
            fr->step    = 0;
            fr->alias   = in->symbol;
            /* fr->sink and fr->field are inherited unchanged. */
            continue;

        case TP_VM_GOAL:
            if (fr->step == 0) {
                fr->saved_goal = state->active_goal;
                state->active_goal = in->symbol;
                if (in->target == TP_NO_INDEX) {
                    fr->step = 1; signal = TP_VM_FAIL; continue;
                }
                stack.frames[stack.count - 1].step = 1;
                if (!tp_vm_stack_push(arena, &stack, in->target, fr->sink,
                                      fr->field, fr->alias))
                    return TP_VM_OOM;
                continue;
            } else {
                state->active_goal = fr->saved_goal;
                stack.count--; continue;
            }

        case TP_VM_NO_EXTRAS:
            if (fr->step == 0) {
                fr->saved_no_extras = state->no_extras;
                state->no_extras = (in->aux == 0) ? 1 : 0;
                if (in->target == TP_NO_INDEX) {
                    fr->step = 1; signal = TP_VM_FAIL; continue;
                }
                stack.frames[stack.count - 1].step = 1;
                if (!tp_vm_stack_push(arena, &stack, in->target, fr->sink,
                                      fr->field, fr->alias))
                    return TP_VM_OOM;
                continue;
            } else {
                state->no_extras = fr->saved_no_extras;
                stack.count--; continue;
            }

        case TP_VM_RECOVER:
            /* Try the body cleanly (step 1); on failure re-run it with recovery
            ** armed (step 2), exactly like the old tp_vm_run's TP_VM_RECOVER. */
            if (fr->step == 0) {
                fr->saved_count   = state->parser->builder.count;
                fr->saved_pos     = state->position;
                fr->saved_sink    = fr->sink ? fr->sink->count : 0;
                fr->saved_held    = state->held_count;
                fr->saved_content = state->content_end;
                fr->outer_recover = state->recover;
                fr->outer_fired   = state->recover_fired;
                fr->outer_start   = state->recover_start;
                if (in->target == TP_NO_INDEX) {
                    fr->step = 1; signal = TP_VM_FAIL; continue;
                }
                stack.frames[stack.count - 1].step = 1;
                if (!tp_vm_stack_push(arena, &stack, in->target, fr->sink,
                                      fr->field, fr->alias))
                    return TP_VM_OOM;
                continue;
            } else if (fr->step == 1) {
                if (signal == TP_VM_OOM) return TP_VM_OOM;
                if (signal == TP_VM_MATCH_OK) {
                    signal = TP_VM_MATCH_OK; stack.count--; continue;
                }
                state->parser->builder.count = fr->saved_count;
                state->position = fr->saved_pos;
                if (fr->sink) fr->sink->count = fr->saved_sink;
                state->held_count = fr->saved_held;
                state->content_end = fr->saved_content;
                state->recover       = in->pattern;
                state->recover_fired = 0;
                state->recover_start = fr->saved_pos;
                if (in->target == TP_NO_INDEX) {
                    fr->step = 2; signal = TP_VM_FAIL; continue;
                }
                stack.frames[stack.count - 1].step = 2;
                if (!tp_vm_stack_push(arena, &stack, in->target, fr->sink,
                                      fr->field, fr->alias))
                    return TP_VM_OOM;
                continue;
            } else {
                state->recover       = fr->outer_recover;
                state->recover_fired = fr->outer_fired;
                state->recover_start = fr->outer_start;
                stack.count--; continue;
            }

        case TP_VM_INJECT: {
            /* Language injection (W62): match the REGION, then splice. The region
            ** runs through tp_vm_do_inject's body callback, on a nested driver call
            ** (bounded by injection depth, not input nesting). */
            uint32_t region = in->target;
            signal = tp_vm_do_inject(state, fr->sink, in->pattern,
                                     tp_vm_inject_body_run, &region);
            stack.count--; continue;
        }

        case TP_VM_DEFERRED_SLOT:
            /* Claim a per-invocation slot in the enclosing rule frame and succeed
            ** zero-width (no input consumed, no node emitted). A failed claim means
            ** the compile-time slot_count under-counted the declarations -- an
            ** invariant break that should never happen; fail cleanly. */
            if (!tp_slot_claim(state, in->symbol)) {
                signal = TP_VM_FAIL; stack.count--; continue;
            }
            signal = TP_VM_MATCH_OK; stack.count--; continue;

        case TP_VM_DEFER:
            /* Match the child sub-pattern, then on success copy its byte span into
            ** the parse arena and enqueue it onto the named slot. Mirrors TP_VM_GOAL
            ** so the match is tree-transparent: the child runs in the same sink with
            ** the same field/alias, building the identical CST. */
            if (fr->step == 0) {
                fr->saved_pos = state->position;
                fr->look      = fr->sink ? fr->sink->count : 0; /* node-index probe */
                if (in->target == TP_NO_INDEX) {
                    fr->step = 1; signal = TP_VM_FAIL; continue;
                }
                stack.frames[stack.count - 1].step = 1;
                if (!tp_vm_stack_push(arena, &stack, in->target, fr->sink,
                                      fr->field, fr->alias))
                    return TP_VM_OOM;
                continue;
            } else {
                if (signal == TP_VM_MATCH_OK) {
                    uint32_t start = fr->saved_pos;
                    uint32_t len   = state->position - start;
                    tp_slot_frame *slot;
                    tp_deferred_item item;
                    uint8_t *copy = 0;
                    if (len > 0) {
                        uint32_t b;
                        copy = (uint8_t *)tp_alloc(arena, len);
                        if (copy == 0) return TP_VM_OOM;
                        /* -fno-builtin: byte-copy loop, not memcpy. */
                        for (b = 0; b < len; b++)
                            copy[b] = state->input[start + b];
                    }
                    item.text     = copy;
                    item.text_len = len;
                    /* node_index: best-effort post-parse cross-link -- the first
                    ** node the child added to the sink (the sink's items[] holds
                    ** node indices). TP_NO_INDEX when the child added none. */
                    item.node_index =
                        (fr->sink != 0 && fr->sink->count > fr->look)
                            ? fr->sink->items[fr->look] : TP_NO_INDEX;
                    item._pad     = 0;
                    slot = tp_slot_resolve(state, in->symbol);
                    if (slot != 0) {
                        if (!tp_slot_push(arena, slot, item))
                            return TP_VM_OOM;
                    }
                    /* No enclosing slot (dangling defer caught at compile time):
                    ** drop the item but pass the match through, keeping the tree. */
                    stack.count--; continue; /* signal stays TP_VM_MATCH_OK */
                }
                /* Child failed: nothing enqueued; pop and propagate the failure. */
                stack.count--; continue;
            }

        case TP_VM_PARSE_DEFERRED: {
            /* DRAIN the slot FIFO: for each captured item, in order, invoke the
            ** target rule once, appending its CST as a sibling in the enclosing
            ** rule's body (the rule pushes into fr->sink, exactly like a plain
            ** CALL). Modelled on TP_VM_REPEAT's multi-step loop but with DRAIN
            ** semantics: the drain is COMMITTED -- it never backtracks and never
            ** fails. A failed rule invocation does NOT abort the drain; the
            ** cursor stays wherever the rule left it (including past recovery)
            ** and the remaining items still drain. The loop is bounded by the
            ** item count (fr->reps), not by progress, so a zero-width drained
            ** rule still terminates after slot->count items. The slot is
            ** re-resolved by name each resume rather than stored across the
            ** push -- the uint32 scratch fields cannot hold a pointer, and the
            ** slot's count is fixed during the drain (a drained rule defers only
            ** into its OWN fresh slots, never this one), so re-resolution is
            ** consistent and avoids any stale-pointer worry across stack growth.
            ** Phase 4 is NON-PARAMETRIZED: no param-stack push (Phase 7-8). */
            tp_slot_frame *slot = tp_slot_resolve(state, in->symbol);
            if (slot == 0 || slot->count == 0 || in->target == TP_NO_INDEX) {
                /* Empty slot / no target: no-op success. */
                signal = TP_VM_MATCH_OK; stack.count--; continue;
            }
            if (fr->step == 0)
                fr->reps = 0;
            else {
                /* Resume after a drained rule returned. parse-deferred is
                ** non-failing and committed: ignore the child signal except for
                ** OOM, and do NOT restore state->position -- the cursor stays
                ** wherever the rule left it. Advance by item index.
                **
                ** Phase 8: for a parametrized drain (in->max != TP_NO_INDEX) the
                ** prologue pushed an instantiated pattern onto the goal-stack before
                ** this item's rule ran; pop it now, BEFORE advancing to the next
                ** item, so the push/pop balance per item even when the rule failed
                ** (the drain is non-failing -- pop unconditionally). The OOM check
                ** runs after the pop so the stack is left balanced even on OOM. */
                if (in->max != TP_NO_INDEX)
                    tp_param_stack_pop(&state->parser->param_stacks[in->max]);
                if (signal == TP_VM_OOM) return TP_VM_OOM;
                fr->reps++;
            }
            if (fr->reps < slot->count) {
                /* Phase 8: instantiate the captured delimiter into the param goal's
                ** template pattern and push it before the rule runs (the lexer's
                ** goal redirect reads the stack top). in->aux is the template rule
                ** index; on a bad index tp_param_instantiate returns 0 -> OOM. */
                if (in->max != TP_NO_INDEX) {
                    tp_deferred_item item = slot->items[fr->reps];
                    const tp_ir *pat = tp_param_instantiate(
                        state->parser->arena, state->grammar, in->aux,
                        item.text, item.text_len);
                    if (pat == 0)
                        return TP_VM_OOM;
                    if (!tp_param_stack_push(state->parser->arena,
                                             &state->parser->param_stacks[in->max],
                                             pat))
                        return TP_VM_OOM;
                }
                stack.frames[stack.count - 1].step = 1;
                if (!tp_vm_stack_push(arena, &stack, in->target, fr->sink,
                                      TP_NO_SYMBOL, TP_NO_SYMBOL))
                    return TP_VM_OOM;
                continue;
            }
            /* All items drained. */
            signal = TP_VM_MATCH_OK; stack.count--; continue;
        }

        case TP_VM_NOT_RULE:
            /* Rule-level negative lookahead `(not GUARD BODY)': GUARD (in->pattern,
            ** with optional goal trigger in in->symbol) is a token-level lookahead
            ** at the current position; BODY (in->target) is a rule-level program.
            ** Step 0 evaluates the guard PURELY (no node, no cursor advance): if the
            ** guard matches, the NOT fails; otherwise BODY runs into the same sink,
            ** transparently, and its node becomes the NOT's contribution. */
            if (fr->step == 0) {
                /* Evaluate the guard PURELY through the shared helper (the SAME the
                ** generated parser calls): it routes a goal-triggered guard through
                ** tp_vm_goal_redirect_param / tp_vm_goal_redirect and matches without
                ** advancing the cursor. Guard present -> the NOT fails; the guard does
                ** NOT consume extras (child[0] is a raw-byte lookahead at the current
                ** position); child[1]'s CALL consumes its own leading extras. */
                if (tp_vm_not_rule_guard(state, in->pattern, in->symbol)) {
                    signal = TP_VM_FAIL; stack.count--; continue;
                }
                if (in->target == TP_NO_INDEX) {
                    signal = TP_VM_FAIL; stack.count--; continue;
                }
                stack.frames[stack.count - 1].step = 1;
                if (!tp_vm_stack_push(arena, &stack, in->target, fr->sink,
                                      fr->field, fr->alias))
                    return TP_VM_OOM;
                continue;
            } else {
                /* Resume: BODY returned `signal' (MATCH_OK / FAIL / OOM); pass it
                ** straight through as the NOT's verdict. */
                stack.count--; continue;
            }

        default:
            signal = TP_VM_FAIL; stack.count--; continue;
        }
    }
    return signal;
}

/* ** Freeze the parser's node builder into an immutable tree. */
tinyparse_result tp_vm_freeze_tree(tinyparse_parser *parser,
                                     uint32_t root)
{
    tinyparse_tree *tree = (tinyparse_tree *)tp_alloc(
        parser->arena, sizeof *tree);
    if (tree == 0)
        return TINYPARSE_OUT_OF_MEMORY;
    tree->arena         = parser->arena;
    tree->interner      = &parser->grammar->interner;
    tree->nodes         = parser->builder.nodes;
    tree->node_count    = parser->builder.count;
    tree->node_capacity = parser->builder.capacity;
    tree->root          = root;
    parser->tree = tree;
    return TINYPARSE_OK;
}

/* ** Initialise a tp_vm_state for a fresh parse of `parser' over `program',
** building the token index and the (adaptive) rule memo. Factored out of
** tp_execute_vm so the generated recursive descent (W14-3) starts from
** byte-identical state. */
void tp_vm_state_init(tp_vm_state *state, tinyparse_parser *parser,
                      const tp_program *program)
{
    state->parser        = parser;
    state->grammar       = parser->grammar;
    state->program       = program;
    state->input         = parser->input;
    state->length        = parser->input_length;
    state->position      = 0;
    state->recover       = 0;
    state->recover_fired = 0;
    state->recover_start = 0;
    state->active_goal   = TP_NO_SYMBOL;
    state->no_extras     = 0;
    /* The held `attach: leading' trivia buffer starts empty; it grows lazily from
    ** parser->arena the first time a LEADING comment is scanned. */
    state->held          = 0;
    state->held_count    = 0;
    state->held_capacity = 0;
    state->content_end   = 0;
    /* The native-recursion depth guard is OFF for the VM (it iterates on an arena
    ** stack and never recurses on the C stack). The generated parser lowers
    ** depth_limit to its measured value after this init. */
    state->depth         = 0;
    state->depth_limit   = TP_NO_INDEX;
    /* Default: heap-allocate each rule's child sink (safe for the VM's relocatable
    ** frame stack). The generated native parser sets children_inline = 1 after init,
    ** since its frames are stable C locals (W16-GOAL). */
    state->children_inline = 0;
    /* The baked extras scanner is OFF by default (the VM keeps its generic, goal-aware
    ** extras loop). The generated parser sets it after init (W17-2). */
    state->extras_match = 0;
    /* The per-position TOKEN memo is no longer consulted on the parse path (W16-GOAL:
    ** every caller passes 0 -- it churned the cache faster than it saved re-matches on
    ** a clean predictive parse). Leave it DISABLED (capacity 0) rather than allocating
    ** an input-scaled slot table the parse never reads. tp_token_match_memo still takes
    ** the parameter (and honours a live table) so the matcher's contract is unchanged;
    ** the parse simply never hands it one. */
    state->memo.slots    = 0;
    state->memo.live     = 0;
    state->memo.capacity = 0;
    state->memo.mask     = 0;
    state->memo.count    = 0;
    state->memo.arena    = parser->arena;
    tp_extras_memo_init(&state->extras_memo, parser->arena, parser->input_length);

    /* W17-CST: pre-size the CST node array from the input length. The node array
    ** grew by doubling from 32, and arenas never realloc in place, so each
    ** doubling COPIED every node already built -- the copy loop in
    ** tp_tree_builder_push and the memcpy it drives were a top profile slice. A real
    ** parse settles near one node per few input bytes, so reserving input_length/4
    ** up front (when the builder is still empty -- it is, this is the first push of
    ** the parse) absorbs essentially all the growth into one allocation. It is a
    ** pure capacity hint: the tree contents are byte-identical, only the realloc
    ** schedule changes. The reserve is skipped for an empty input and never shrinks
    ** an already-grown builder (the buffer-friendly resume path keeps its array). */
    if (parser->builder.capacity == 0 && parser->input_length >= 256u) {
        uint32_t want = parser->input_length / 4u;
        tp_node *nodes = (tp_node *)tp_alloc_raw(parser->builder.arena,
                                                 want * sizeof *nodes);
        if (nodes != 0) {
            parser->builder.nodes    = nodes;
            parser->builder.capacity = want;
        }
    }

    /* Build the O(1) token-by-symbol index once per grammar (W66). */
    if (parser->grammar->token_index == 0)
        tp_vm_build_token_index(parser->grammar);

    /* Build the extras lead-byte fast-path set once per grammar (W16-1) so the
    ** per-terminal consume_extras can reject a non-extra cursor byte in O(1). */
    if (parser->grammar->extras_lead_state == 0)
        tp_extras_lead_build(parser->grammar);

    /* Build the O(1) hidden-rule-by-symbol index once per grammar (W17-FLAT) so
    ** the shared node-close can decide collapse with one array read. Shared by the
    ** VM and the generated parser (both run tp_vm_state_init), so they flatten
    ** identically. */
    tp_grammar_build_hidden_index(parser->grammar);

    /* The rule memo (W67) bounds PEG backtracking; scoped to this parse. It is
    ** dormant until the parse goes super-linear (see tp_vm_state), so a clean parse
    ** never pays for it. TINYPARSE_RULEMEMO=always forces it on from the first
    ** call (used by the differential to exercise the replay path); =off disables it. */
    tp_rule_memo_init(&state->rule_memo, parser->arena, parser->input_length);
    state->call_attempts = 0;
    state->memo_active   = 0;
    /* Threshold: a linear parse does O(input) attempts; this multiple of the input
    ** plus a fixed floor sits above a clean parse's count yet far below any
    ** pathological (super-linear) one, so the memo wakes early on a blowup without
    ** taxing the common case. */
    state->memo_threshold = parser->input_length * 32u + 65536u;
    if (parser->engine->vm_rule_memo_force == 1) {       /* always on */
        state->memo_active = 1;
        tp_rule_memo_ensure(&state->rule_memo);          /* build now: forced on */
        tp_extras_memo_ensure(&state->extras_memo);
    } else if (parser->engine->vm_rule_memo_force == 2) {/* off */
        state->memo_threshold = 0xFFFFFFFFu;
    }
}

/* ** Run the compiled grammar over the buffered input via the bytecode VM -- the
** engine since W14-5. It pushes the Source node, parses the start body into the
** root's children through the VM dispatch loop, consumes trailing extras, links,
** and reports trailing/leftover input. The tree and diagnostics are byte-identical
** to the C-compiled parser's (mise run gate:cc-diff), anchored by the conformance suite. */
tinyparse_result tp_execute_vm(tinyparse_parser *parser)
{
    tp_vm_state         state;
    tp_program         *program;
    const tp_rule_def  *start;
    tp_vm_sink          children;
    uint32_t            root;
    int                 sig;
#ifdef TP_DEBUG_INTERN_GUARD
    /* W44 tripwire: no name may be interned during the parse. Snapshot the global
    ** call counter and assert at every return that it has not moved. */
    unsigned long       interns_before = tp_intern_call_count;
#define TP_GUARD_NO_INTERN() \
    assert(tp_intern_call_count == interns_before && \
           "W44: tp_intern called on the parse hot path")
#else
#define TP_GUARD_NO_INTERN() ((void)0)
#endif

    /* Locate the start rule, then lower the grammar. */
    {
        uint32_t i;
        start = 0;
        for (i = 0; i < parser->grammar->rule_count; i++)
            if (parser->grammar->rules[i].symbol == parser->start_symbol) {
                start = &parser->grammar->rules[i];
                break;
            }
    }
    if (start == 0) {
        tp_diag_error(&parser->diagnostics, "parse.no-start",
                      "error: start rule not found", 0, 0, "missing start rule");
        return TINYPARSE_SYNTAX_ERROR;
    }

    /* Lower the grammar once per start symbol and reuse it across parses: the
    ** IR->bytecode lowering is a pure function of (grammar, start symbol), so the
    ** cache on the grammar amortises it to at most once per start symbol. */
    if (parser->grammar->vm_program != 0 &&
        parser->grammar->vm_program_start == parser->start_symbol) {
        program = parser->grammar->vm_program;
    } else {
        /* The lowered program is cached on the grammar and reused across every
        ** parse, so it MUST come from the grammar's (persistent) arena, not the
        ** per-parser arena -- a per-parser child arena is reset on parser_free, and
        ** a cache pointing into reset memory would dangle on the next parse. */
        program = tp_compile_program(parser->grammar->arena, parser->grammar,
                                     parser->start_symbol);
        if (program == 0)
            return TINYPARSE_OUT_OF_MEMORY;
        parser->grammar->vm_program       = program;
        parser->grammar->vm_program_start = parser->start_symbol;
    }
    tp_vm_state_init(&state, parser, program);

    children.items = 0; children.count = 0; children.capacity = 0;
    children.parent = TP_NO_INDEX;   /* root: legacy full tp_vm_link_children pass */
    root = tp_tree_builder_push(&parser->builder, start->symbol, TP_NO_SYMBOL,
                                0, parser->input_length, TP_NO_INDEX);
    if (root == TP_NO_INDEX)
        return TINYPARSE_OUT_OF_MEMORY;

    sig = tp_vm_run(&state, program->start_program, &children,
                    TP_NO_SYMBOL, TP_NO_SYMBOL);
    if (sig == TP_VM_OOM)
        return TINYPARSE_OUT_OF_MEMORY;

    if (sig != TP_VM_MATCH_OK) {
        tp_diag_error(&parser->diagnostics, "parse.syntax",
                      "error: unexpected input", state.position,
                      state.position < parser->input_length
                          ? state.position + 1 : state.position,
                      "parse failed here");
        tp_vm_link_children(&parser->builder, root, &children);
        tp_vm_freeze_tree(parser, root);
        TP_GUARD_NO_INTERN();
        return TINYPARSE_SYNTAX_ERROR;
    }

    /* Trailing extras after the start rule, then drain any held `attach: leading'
    ** comment (an end-of-input doc comment with nothing following): this is a
    ** finalizer that opens no node, so the buffer must be flushed explicitly or the
    ** comment is stranded. */
    tp_vm_consume_extras(&state, &children);
    tp_vm_flush_held(&state, &children);

    tp_vm_link_children(&parser->builder, root, &children);
    parser->builder.nodes[root].end_byte = parser->input_length;

    if (state.position < parser->input_length) {
        tp_diag_error(&parser->diagnostics, "parse.trailing",
                      "error: unexpected trailing input", state.position,
                      state.position + 1, "unexpected here");
        tp_vm_freeze_tree(parser, root);
        TP_GUARD_NO_INTERN();
        return TINYPARSE_SYNTAX_ERROR;
    }

    TP_GUARD_NO_INTERN();
    return tp_vm_freeze_tree(parser, root);
}
#undef TP_GUARD_NO_INTERN

/* ** The buffer-friendly top-level item loop (W17-4). See the contract in
** internal.h. The generated <prefix>_run_chunk has already built the lowered
** program and called tp_vm_state_init + set depth_limit / extras_match /
** children_inline, so `state' is ready; here we only own the resumable top-level
** loop and the finalisation. The native generated parser cannot suspend mid-stack,
** so the ONLY suspension point is this loop's item boundary, where the C stack is
** empty.
**
** Byte-identity with one whole parse: the committed items live in the parser's
** builder and the persistent child sink (parser->bf_children); each call restores
** state->position from parser->bf_position and continues the same loop. An item is
** committed only when it completes strictly before the buffer end OR the input is
** complete, so no item is ever committed on a truncated buffer and then re-parsed
** differently. With input_complete set on the final chunk the loop runs the exact
** same finalisation (trailing extras, link, EOF/trailing check, freeze) as the
** whole-input driver, over the same items in the same order. */
tinyparse_result tp_parser_chunk_loop(
    tp_vm_state *state,
    int (*item_fn)(tp_vm_state *, tp_vm_sink *, uint32_t, uint32_t),
    int has_eof, uint32_t min)
{
    tinyparse_parser *parser = state->parser;
    tp_vm_sink         *children = &parser->bf_children;
    uint32_t            reps;
    uint32_t            saved_recover_start;

    /* First activation on this parser: open the root node (spanning the whole
    ** input, extended on freeze) and reset the persistent child sink. The root is
    ** built once and kept across MORE returns. */
    if (!parser->bf_active) {
        uint32_t root = tp_tree_builder_push(&parser->builder, parser->start_symbol,
                                             TP_NO_SYMBOL, 0, parser->input_length,
                                             TP_NO_INDEX);
        if (root == TP_NO_INDEX)
            return TINYPARSE_OUT_OF_MEMORY;
        parser->bf_root           = root;
        parser->bf_position       = 0;
        parser->bf_children.items = 0;
        parser->bf_children.count = 0;
        parser->bf_children.capacity = 0;
        parser->bf_children.parent = TP_NO_INDEX; /* root: legacy link pass */
        parser->bf_active         = 1;
    }

    /* Resume at the saved boundary. The repeat count only gates the min bound; the
    ** committed items already in the sink are what matter for the tree. */
    state->position = parser->bf_position;
    reps = children->count;

    /* The top-level item loop. Each iteration mirrors one turn of the generated
    ** REPEAT: save the marks, parse one item, and on a terminating failure restore
    ** the marks. Recovery is disarmed inside, as in the repeat combinator. */
    saved_recover_start = state->recover_start;
    state->recover = 0;
    for (;;) {
        uint32_t mark_count = parser->builder.count;
        uint32_t mark_pos   = state->position;
        uint32_t mark_sink  = children->count;
        uint32_t mark_held  = state->held_count;
        uint32_t mark_content = state->content_end;
        int      sig;

        /* The buffer-friendly boundary: before committing to a new item, if the
        ** cursor is already at the buffer end and more input may still come, the
        ** next item cannot be known to fit -- suspend here (the stack is empty) and
        ** ask for more. bf_position already names this item start. */
        if (!parser->input_complete && mark_pos >= parser->input_length) {
            parser->bf_position = mark_pos;
            return TINYPARSE_MORE;
        }

        sig = item_fn(state, children, TP_NO_SYMBOL, TP_NO_SYMBOL);
        if (sig == TP_VM_OOM)
            return TINYPARSE_OUT_OF_MEMORY;

        if (sig != TP_VM_MATCH_OK || state->position == mark_pos) {
            /* The item did not match (or made no progress): the repeat ends here.
            ** Roll the marks back to before the attempt. */
            parser->builder.count = mark_count;
            state->position       = mark_pos;
            children->count       = mark_sink;
            state->held_count     = mark_held;
            state->content_end    = mark_content;
            break;
        }

        /* The item matched and advanced. But if it consumed right up to the buffer
        ** end while input is still incomplete, the next bytes might have extended
        ** it (an identifier continuing, a list still open) -- so it is NOT yet safe
        ** to commit. Roll back to this item's start and ask for more. */
        if (!parser->input_complete && state->position >= parser->input_length) {
            parser->builder.count = mark_count;
            state->position       = mark_pos;
            children->count       = mark_sink;
            state->held_count     = mark_held;
            state->content_end    = mark_content;
            parser->bf_position   = mark_pos;
            return TINYPARSE_MORE;
        }

        /* Commit this item and continue. */
        reps++;
    }
    state->recover       = 0;
    state->recover_start = saved_recover_start;
    parser->bf_position  = state->position;

    /* More input may still arrive: hold off finalising. The repeat has consumed
    ** every item that fits; the next call (with more bytes, or input_complete) does
    ** the rest. We return MORE rather than freeze a partial tree. */
    if (!parser->input_complete)
        return TINYPARSE_MORE;

    /* Input is complete: finalise exactly as the whole-input driver does. */
    if (reps < min) {
        tp_diag_error(&parser->diagnostics, "parse.syntax",
                      "error: unexpected input", state->position,
                      state->position < parser->input_length
                          ? state->position + 1 : state->position,
                      "parse failed here");
        tp_vm_link_children(&parser->builder, parser->bf_root, children);
        tp_vm_freeze_tree(parser, parser->bf_root);
        return TINYPARSE_SYNTAX_ERROR;
    }

    /* Trailing extras after the last item, then the optional EOF. When the start
    ** body ends in EOF, tp_vm_do_eof consumes the trailing extras itself (exactly
    ** as the whole-input driver runs the SEQ's EOF child), so do not consume them
    ** twice; otherwise consume them here as the driver's tail does. */
    if (has_eof) {
        int sig = tp_vm_do_eof(state, children);
        if (sig == TP_VM_OOM)
            return TINYPARSE_OUT_OF_MEMORY;
        if (sig != TP_VM_MATCH_OK) {
            tp_diag_error(&parser->diagnostics, "parse.syntax",
                          "error: unexpected input", state->position,
                          state->position < parser->input_length
                              ? state->position + 1 : state->position,
                          "parse failed here");
            tp_vm_link_children(&parser->builder, parser->bf_root, children);
            tp_vm_freeze_tree(parser, parser->bf_root);
            return TINYPARSE_SYNTAX_ERROR;
        }
    } else {
        tp_vm_consume_extras(state, children);
        tp_vm_flush_held(state, children); /* drain a stranded end-of-input doc comment */
    }

    tp_vm_link_children(&parser->builder, parser->bf_root, children);
    parser->builder.nodes[parser->bf_root].end_byte = parser->input_length;

    if (state->position < parser->input_length) {
        tp_diag_error(&parser->diagnostics, "parse.trailing",
                      "error: unexpected trailing input", state->position,
                      state->position + 1, "unexpected here");
        tp_vm_freeze_tree(parser, parser->bf_root);
        return TINYPARSE_SYNTAX_ERROR;
    }

    return tp_vm_freeze_tree(parser, parser->bf_root);
}
