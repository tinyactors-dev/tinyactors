/*
** query.c -- queries as parsers over the linearized tree.
**
** A query is a parser. The tree to search is flattened to an ENTER/LEAF/EXIT
** token stream (pp_linearize), and each query pattern compiles to a small
** matcher that *parses that stream*: it consumes ENTER/LEAF/EXIT tokens the way
** the engine's lexer consumes bytes. A node pattern matches an ENTER..EXIT pair
** (or a single LEAF); its children are matched against the token run between
** them; a `@capture` binds to the matched node's byte span; `_` is the wildcard
** node. Matching at any depth is the "skip one balanced subtree" rule -- before
** an unanchored child the matcher may repeatedly swallow one whole balanced
** ENTER..EXIT (or LEAF) subtree -- and the `.` anchor turns that skip off
** between two adjacent children. `#eq?` and `#match?` predicates filter a whole
** match by byte comparison or the engine's regex. None of this mutates the tree;
** the stream is read-only and the matcher only records capture positions.
**
** Freestanding: the form reader, interner, regex, and arena are the only deps;
** captures are reported through a callback so the module never does IO.
*/
#include "internal.h"

/* ** A leaf node is one with no children; it linearizes to a single LEAF token.
** An internal node linearizes to ENTER ... EXIT around its children. */
static int pp_node_is_leaf(const pp_node *node)
{
    return node->child_count == 0;
}

/* ** Recursively append a node and its subtree to the linearized stream in
** pre-order. Returns 1 on success, 0 if the array could not grow. The token
** array is grown geometrically from the arena (no realloc: ax arenas only
** append, so a grow copies). */
static int pp_lin_walk(ax_arena *arena, const pocketparse_tree *tree,
                       uint32_t index, pp_lin_token **tokens, uint32_t *count,
                       uint32_t *capacity)
{
    const pp_node *node = &tree->nodes[index];
    pp_lin_token  *slot;
    uint32_t       child;

    /* Grow the token array if the next push (ENTER+EXIT, or one LEAF) would not
    ** fit. Reserving two keeps the leaf and internal cases from re-checking. */
    if (*count + 2 > *capacity) {
        uint32_t new_capacity = *capacity == 0 ? 32 : *capacity * 2;
        pp_lin_token *grown = (pp_lin_token *)ax_alloc(
            arena, new_capacity * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return 0;
        for (i = 0; i < *count; i++)
            grown[i] = (*tokens)[i];
        *tokens   = grown;
        *capacity = new_capacity;
    }

    if (pp_node_is_leaf(node)) {
        slot = &(*tokens)[(*count)++];
        slot->kind       = PP_LIN_LEAF;
        slot->node_kind  = node->kind;
        slot->field      = node->field;
        slot->node_index = index;
        slot->start_byte = node->start_byte;
        slot->end_byte   = node->end_byte;
        return 1;
    }

    slot = &(*tokens)[(*count)++];
    slot->kind       = PP_LIN_ENTER;
    slot->node_kind  = node->kind;
    slot->field      = node->field;
    slot->node_index = index;
    slot->start_byte = node->start_byte;
    slot->end_byte   = node->end_byte;

    for (child = node->first_child; child != PP_NO_INDEX;
         child = tree->nodes[child].next_sibling) {
        if (!pp_lin_walk(arena, tree, child, tokens, count, capacity))
            return 0;
    }

    /* The EXIT may not fit the reservation made for this node's own ENTER (the
    ** children consumed it); ensure room before writing it. */
    if (*count + 1 > *capacity) {
        uint32_t new_capacity = *capacity * 2;
        pp_lin_token *grown = (pp_lin_token *)ax_alloc(
            arena, new_capacity * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return 0;
        for (i = 0; i < *count; i++)
            grown[i] = (*tokens)[i];
        *tokens   = grown;
        *capacity = new_capacity;
    }
    slot = &(*tokens)[(*count)++];
    slot->kind       = PP_LIN_EXIT;
    slot->node_kind  = node->kind;
    slot->field      = PP_NO_SYMBOL;
    slot->node_index = index;
    slot->start_byte = node->start_byte;
    slot->end_byte   = node->end_byte;
    return 1;
}

/* ** Flatten a tree to its ENTER/LEAF/EXIT stream. */
int pp_linearize(pocketparse_engine *engine, const pocketparse_tree *tree,
                 pp_linearization *out)
{
    ax_arena     *arena = engine->arena;
    pp_lin_token *tokens = 0;
    uint32_t      count = 0, capacity = 0;

    out->tokens    = 0;
    out->count     = 0;
    out->interner  = tree ? tree->interner : 0;
    if (tree == 0 || tree->node_count == 0)
        return 1;
    if (!pp_lin_walk(arena, tree, tree->root, &tokens, &count, &capacity))
        return 0;
    out->tokens = tokens;
    out->count  = count;
    return 1;
}

/* ** One compiled predicate: an #eq? or #match? test against a named capture.
** `eq' compares the captured bytes to `text' exactly; `match' (eq==0) treats
** `text' as a regex and searches the captured bytes for it. The capture is named
** rather than resolved to an index because predicates are written by name. */
typedef struct pp_query_pred {
    int                eq;        /* 1: #eq?  0: #match?         */
    pocketparse_string capture;   /* @name to test (without @)  */
    pocketparse_string text;      /* literal text or regex      */
} pp_query_pred;

/* ** One compiled query pattern node. is_wildcard marks `_'; otherwise kind is
** the required node-kind id. field, when not PP_NO_SYMBOL, requires the matched
** node to carry that field. capture, when non-empty, binds the matched node.
** children are the sub-patterns; anchored[i] (for i>0) is set by a `.' before
** child i, which forbids skipping a subtree between child i-1 and child i. */
typedef struct pp_query_pat {
    int                   is_wildcard;
    uint32_t              kind;
    uint32_t              field;
    pocketparse_string    capture;
    struct pp_query_pat **children;
    uint8_t              *anchored;
    uint32_t              child_count;
} pp_query_pat;

/* ** One whole compiled query: its top pattern plus any wrapping predicates. */
typedef struct pp_query {
    pp_query_pat   *pattern;
    pp_query_pred  *predicates;
    uint32_t        predicate_count;
} pp_query;

/* ** A capture recorded during a trial match: name plus the matched node span.
** Captures are buffered so the predicates can be checked before any is reported;
** a failing predicate drops the whole buffer. */
typedef struct pp_query_binding {
    pocketparse_string name;
    uint32_t           start_byte;
    uint32_t           end_byte;
} pp_query_binding;

/* ** The mutable state threaded through a trial match: the compiler's arena, the
** linearized stream being parsed, and the growing capture buffer. */
typedef struct pp_query_state {
    ax_arena             *arena;
    const pp_linearization *lin;
    pp_query_binding     *bindings;
    uint32_t              binding_count;
    uint32_t              binding_capacity;
} pp_query_state;

/* ** Record a capture binding, growing the buffer as needed. Returns 1 on
** success, 0 on allocation failure. */
static int pp_query_bind(pp_query_state *state, pocketparse_string name,
                         uint32_t start_byte, uint32_t end_byte)
{
    pp_query_binding *slot;
    if (state->binding_count == state->binding_capacity) {
        uint32_t new_capacity =
            state->binding_capacity == 0 ? 8 : state->binding_capacity * 2;
        pp_query_binding *grown = (pp_query_binding *)ax_alloc(
            state->arena, new_capacity * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return 0;
        for (i = 0; i < state->binding_count; i++)
            grown[i] = state->bindings[i];
        state->bindings         = grown;
        state->binding_capacity = new_capacity;
    }
    slot = &state->bindings[state->binding_count++];
    slot->name       = name;
    slot->start_byte = start_byte;
    slot->end_byte   = end_byte;
    return 1;
}

/* ** Skip one balanced subtree in the stream starting at `pos': a LEAF is one
** token; an ENTER..EXIT pair (with its nested subtrees) is skipped by tracking
** depth. Returns the position just past the subtree, or `pos' unchanged when the
** token there is an EXIT (no subtree to skip). This is the "skip one balanced
** subtree" primitive the descendant rule repeats. */
static uint32_t pp_query_skip_subtree(const pp_linearization *lin, uint32_t pos)
{
    uint32_t depth;
    if (pos >= lin->count)
        return pos;
    if (lin->tokens[pos].kind == PP_LIN_EXIT)
        return pos;
    if (lin->tokens[pos].kind == PP_LIN_LEAF)
        return pos + 1;
    /* ENTER: advance to the matching EXIT. */
    depth = 0;
    while (pos < lin->count) {
        pp_lin_kind k = lin->tokens[pos].kind;
        pos++;
        if (k == PP_LIN_ENTER)
            depth++;
        else if (k == PP_LIN_EXIT) {
            depth--;
            if (depth == 0)
                return pos;
        }
    }
    return pos;
}

/* ** Forward declaration: match one pattern against the subtree at `pos'. */
static int pp_query_match_node(pp_query_state *state, const pp_query_pat *pat,
                               uint32_t pos, uint32_t *out_next);

/* ** Match a pattern's children against the token run inside a parent node,
** starting just past its ENTER at `pos' and ending at the parent's EXIT. Between
** children the descendant rule skips whole balanced subtrees, unless a child is
** anchored to its predecessor (a `.'), in which case it must match the very next
** subtree. After the last child, any remaining subtrees up to EXIT are skipped
** (a pattern need not name every child). Returns 1 on a full match of all
** children, 0 otherwise. */
static int pp_query_match_children(pp_query_state *state,
                                   const pp_query_pat *pat, uint32_t pos,
                                   uint32_t exit_pos)
{
    uint32_t c;
    uint32_t cursor = pos;
    for (c = 0; c < pat->child_count; c++) {
        const pp_query_pat *child = pat->children[c];
        int anchored = pat->anchored[c];
        uint32_t next;
        if (anchored) {
            /* The child must match the subtree immediately at the cursor. */
            if (cursor >= exit_pos ||
                !pp_query_match_node(state, child, cursor, &next))
                return 0;
            cursor = next;
        } else {
            /* Skip whole balanced subtrees until the child matches one. */
            int matched = 0;
            while (cursor < exit_pos) {
                uint32_t saved = state->binding_count;
                uint32_t skipped;
                if (pp_query_match_node(state, child, cursor, &next)) {
                    cursor  = next;
                    matched = 1;
                    break;
                }
                state->binding_count = saved; /* roll back trial captures */
                skipped = pp_query_skip_subtree(state->lin, cursor);
                if (skipped <= cursor)
                    break; /* nothing left to skip (EXIT or stall) */
                cursor = skipped;
            }
            if (!matched)
                return 0;
        }
    }
    return 1;
}

/* ** Match one pattern against the subtree whose first token is at `pos'. On
** success writes the position just past the subtree to *out_next and records the
** pattern's capture (if any) bound to the matched node's span. A wildcard `_'
** matches any node; a kind pattern requires the node-kind to agree; a field
** requirement, when set, requires the token to carry that field. A pattern with
** children requires the matched node to be internal (an ENTER..EXIT pair) and
** its children to match inside. */
static int pp_query_match_node(pp_query_state *state, const pp_query_pat *pat,
                               uint32_t pos, uint32_t *out_next)
{
    const pp_lin_token *tok;
    uint32_t            next;

    if (pos >= state->lin->count)
        return 0;
    tok = &state->lin->tokens[pos];
    if (tok->kind == PP_LIN_EXIT)
        return 0;

    /* The node's kind must agree, unless this is the wildcard. */
    if (!pat->is_wildcard && tok->node_kind != pat->kind)
        return 0;
    /* A field requirement must be carried by the matched node. */
    if (pat->field != PP_NO_SYMBOL && tok->field != pat->field)
        return 0;

    if (tok->kind == PP_LIN_LEAF) {
        /* A leaf can satisfy only a childless pattern. */
        if (pat->child_count != 0)
            return 0;
        next = pos + 1;
    } else {
        /* ENTER: find the matching EXIT and match children between. */
        uint32_t exit_pos = pp_query_skip_subtree(state->lin, pos);
        if (exit_pos == 0 || exit_pos > state->lin->count)
            return 0;
        if (!pp_query_match_children(state, pat, pos + 1, exit_pos - 1))
            return 0;
        next = exit_pos;
    }

    if (pat->capture.length != 0 || pat->capture.pointer != 0) {
        if (!pp_query_bind(state, pat->capture, tok->start_byte, tok->end_byte))
            return 0;
    }
    *out_next = next;
    return 1;
}

/* ** True when a form is a `@name' capture symbol. The reader keeps the leading
** `@' as part of the symbol text. */
static int pp_query_is_capture(const pp_form *form)
{
    return form->kind == PP_FORM_SYMBOL && form->text.length >= 1 &&
           form->text.pointer[0] == '@';
}

/* ** The capture name without its leading `@'. */
static pocketparse_string pp_query_capture_name(const pp_form *form)
{
    pocketparse_string name;
    name.pointer = form->text.pointer + 1;
    name.length  = form->text.length - 1;
    return name;
}

/* ** True when a form is the `.' anchor symbol. */
static int pp_query_is_anchor(const pp_form *form)
{
    return form->kind == PP_FORM_SYMBOL && pp_string_equal_c(form->text, ".");
}

/* ** True when a form is the `_' wildcard symbol. */
static int pp_query_is_wildcard(const pp_form *form)
{
    return form->kind == PP_FORM_SYMBOL && pp_string_equal_c(form->text, "_");
}

/* ** True when a form is a predicate clause: a list whose head is a symbol
** beginning with `#' (e.g. (#eq? @c "x")). */
static int pp_query_is_predicate(const pp_form *form)
{
    return form->kind == PP_FORM_LIST && form->count >= 1 &&
           form->items[0]->kind == PP_FORM_SYMBOL &&
           form->items[0]->text.length >= 1 &&
           form->items[0]->text.pointer[0] == '#';
}

/* ** Resolve a kind name to its interned id in the tree's interner. Returns 1
** on a hit. An unknown kind name can never match any node, so the caller fails
** the query early with a clear message rather than silently matching nothing. */
static int pp_query_resolve_kind(const pp_interner *interner,
                                 pocketparse_string name, uint32_t *out_id)
{
    return pp_intern_find(interner, name, out_id);
}

/* ** Compile a node pattern form (a list whose head names the node kind, or `_')
** into a pp_query_pat, resolving kinds and fields through the tree's interner.
** A list reads as: optional head kind/`_', then a stream of children, each of
** which may be preceded by a `field:' keyword, followed by an optional
** `@capture', and separated by an optional `.' anchor. On failure returns NULL
** and writes *error. */
static pp_query_pat *pp_query_compile_pattern(ax_arena *arena,
                                              const pp_interner *interner,
                                              const pp_form *form,
                                              const char **error)
{
    pp_query_pat *pat;
    uint32_t      i;
    uint32_t      cap = 0;
    int           anchor_pending = 0;

    if (form->kind != PP_FORM_LIST || form->count == 0) {
        *error = "query: a pattern must be a non-empty list";
        return 0;
    }

    pat = (pp_query_pat *)ax_alloc(arena, sizeof *pat);
    if (pat == 0) {
        *error = "query: out of memory";
        return 0;
    }
    pat->is_wildcard = 0;
    pat->kind        = PP_NO_SYMBOL;
    pat->field       = PP_NO_SYMBOL;
    pat->capture.pointer = 0;
    pat->capture.length  = 0;
    pat->children    = 0;
    pat->anchored    = 0;
    pat->child_count = 0;

    /* The head names the node kind, or `_' for the wildcard. */
    i = 0;
    if (form->items[0]->kind == PP_FORM_SYMBOL &&
        !pp_query_is_capture(form->items[0]) &&
        !pp_query_is_anchor(form->items[0])) {
        if (pp_query_is_wildcard(form->items[0])) {
            pat->is_wildcard = 1;
        } else {
            uint32_t id;
            if (!pp_query_resolve_kind(interner, form->items[0]->text, &id)) {
                *error = "query: unknown node kind in pattern";
                return 0;
            }
            pat->kind = id;
        }
        i = 1;
    } else {
        *error = "query: a pattern must begin with a node kind or _";
        return 0;
    }

    /* Reserve room for the children (at most the remaining items). */
    if (form->count > i) {
        pat->children = (pp_query_pat **)ax_alloc(
            arena, (form->count - i) * sizeof *pat->children);
        pat->anchored = (uint8_t *)ax_alloc(arena, (form->count - i));
        if (pat->children == 0 || pat->anchored == 0) {
            *error = "query: out of memory";
            return 0;
        }
    }

    for (; i < form->count; i++) {
        const pp_form *item = form->items[i];
        uint32_t       field = PP_NO_SYMBOL;
        pp_query_pat  *child;

        if (pp_query_is_anchor(item)) {
            anchor_pending = 1;
            continue;
        }
        if (pp_query_is_capture(item)) {
            /* A capture applies to the most recently added child. */
            if (cap == 0) {
                *error = "query: @capture has no preceding node";
                return 0;
            }
            pat->children[cap - 1]->capture = pp_query_capture_name(item);
            continue;
        }
        if (item->kind == PP_FORM_KEYWORD) {
            /* A field: keyword binds the next child through a field. */
            uint32_t id;
            if (i + 1 >= form->count) {
                *error = "query: field keyword has no node";
                return 0;
            }
            if (!pp_query_resolve_kind(interner, item->text, &id)) {
                *error = "query: unknown field in pattern";
                return 0;
            }
            field = id;
            i++;
            item = form->items[i];
        }
        if (item->kind != PP_FORM_LIST && !pp_query_is_wildcard(item)) {
            *error = "query: expected a child node pattern";
            return 0;
        }
        if (pp_query_is_wildcard(item)) {
            /* A bare `_' child: a wildcard node with no children. */
            child = (pp_query_pat *)ax_alloc(arena, sizeof *child);
            if (child == 0) {
                *error = "query: out of memory";
                return 0;
            }
            child->is_wildcard = 1;
            child->kind        = PP_NO_SYMBOL;
            child->field       = field;
            child->capture.pointer = 0;
            child->capture.length  = 0;
            child->children    = 0;
            child->anchored    = 0;
            child->child_count = 0;
        } else {
            child = pp_query_compile_pattern(arena, interner, item, error);
            if (child == 0)
                return 0;
            child->field = field;
        }
        pat->children[cap]  = child;
        pat->anchored[cap]  = (uint8_t)anchor_pending;
        anchor_pending      = 0;
        cap++;
    }
    pat->child_count = cap;
    return pat;
}

/* ** Compile a predicate clause (#eq? @c "x") or (#match? @c "re") into a
** pp_query_pred. Returns 1 on success; writes *error and returns 0 otherwise. */
static int pp_query_compile_predicate(const pp_form *form, pp_query_pred *out,
                                      const char **error)
{
    pocketparse_string head;
    if (form->count != 3 || !pp_query_is_capture(form->items[1]) ||
        form->items[2]->kind != PP_FORM_STRING) {
        *error = "query: predicate needs (#op? @capture \"text\")";
        return 0;
    }
    head = form->items[0]->text;
    if (pp_string_equal_c(head, "#eq?"))
        out->eq = 1;
    else if (pp_string_equal_c(head, "#match?"))
        out->eq = 0;
    else {
        *error = "query: unknown predicate (only #eq? and #match?)";
        return 0;
    }
    out->capture = pp_query_capture_name(form->items[1]);
    out->text    = form->items[2]->text;
    return 1;
}

/* ** Compile one top-level query form: either a bare pattern, or a wrapping list
** that pairs a pattern with one or more predicate clauses. Returns 1 on success.
** A wrapping list is recognised when its first item is itself a pattern list and
** later items are predicates. */
static int pp_query_compile(ax_arena *arena, const pp_interner *interner,
                            const pp_form *form, pp_query *out,
                            const char **error)
{
    out->pattern         = 0;
    out->predicates      = 0;
    out->predicate_count = 0;

    if (form->kind == PP_FORM_LIST && form->count >= 1 &&
        form->items[0]->kind == PP_FORM_LIST &&
        (form->count == 1 || pp_query_is_predicate(form->items[1]))) {
        /* A wrapping list: ((pattern) (#pred ...) ...). */
        uint32_t i;
        out->pattern = pp_query_compile_pattern(arena, interner,
                                                form->items[0], error);
        if (out->pattern == 0)
            return 0;
        if (form->count > 1) {
            out->predicates = (pp_query_pred *)ax_alloc(
                arena, (form->count - 1) * sizeof *out->predicates);
            if (out->predicates == 0) {
                *error = "query: out of memory";
                return 0;
            }
        }
        for (i = 1; i < form->count; i++) {
            if (!pp_query_is_predicate(form->items[i])) {
                *error = "query: expected a predicate beside the pattern";
                return 0;
            }
            if (!pp_query_compile_predicate(form->items[i],
                                            &out->predicates[out->predicate_count],
                                            error))
                return 0;
            out->predicate_count++;
        }
        return 1;
    }

    /* A bare pattern. */
    out->pattern = pp_query_compile_pattern(arena, interner, form, error);
    return out->pattern != 0;
}

/* ** Slice the input bytes for a capture's byte span. */
static pocketparse_string pp_query_slice(pocketparse_string input,
                                         uint32_t start, uint32_t end)
{
    pocketparse_string slice;
    if (end > input.length)
        end = input.length;
    if (start > end)
        start = end;
    slice.pointer = input.pointer + start;
    slice.length  = end - start;
    return slice;
}

/* ** Evaluate one predicate against the buffered captures. The named capture's
** text is sliced from the input; #eq? compares it to the literal, #match?
** searches it for the regex. A predicate over an unbound capture fails the
** match. Returns 1 when the predicate holds. */
static int pp_query_eval_predicate(pocketparse_engine *engine, ax_arena *arena,
                                   const pp_query_pred *pred,
                                   const pp_query_state *state,
                                   pocketparse_string input)
{
    uint32_t i;
    for (i = 0; i < state->binding_count; i++) {
        const pp_query_binding *b = &state->bindings[i];
        pocketparse_string text;
        if (!pp_string_equal(b->name, pred->capture))
            continue;
        text = pp_query_slice(input, b->start_byte, b->end_byte);
        if (pred->eq)
            return pp_string_equal(text, pred->text);
        /* #match?: compile the regex once and search the captured text for a
        ** match anchored at any offset. */
        {
            const char *regex_error = 0;
            pp_ir *ir = pp_compile_regex(arena, pred->text, &regex_error);
            uint32_t at;
            if (ir == 0)
                return 0;
            for (at = 0; at <= text.length; at++) {
                uint32_t end;
                if (pp_token_match(engine, ir, text.pointer, text.length, at,
                                   &end))
                    return 1;
            }
            return 0;
        }
    }
    return 0; /* capture not bound */
}

/* ** Report every buffered capture through the sink, in match order. */
static void pp_query_report(const pp_query_state *state,
                            pocketparse_string input,
                            const pp_query_sink *sink)
{
    uint32_t i;
    for (i = 0; i < state->binding_count; i++) {
        const pp_query_binding *b = &state->bindings[i];
        pp_query_capture capture;
        capture.name       = b->name;
        capture.start_byte = b->start_byte;
        capture.end_byte   = b->end_byte;
        capture.text       = pp_query_slice(input, b->start_byte, b->end_byte);
        sink->emit(sink->user, &capture);
    }
}

/* ** Try one query at every position in the stream. A match anchored at position
** p starts a trial; on success the predicates are checked, and if they all hold
** the captures are reported. Each trial uses a fresh capture buffer, so a failed
** match (or a failed predicate) reports nothing. */
static int pp_query_run_one(pocketparse_engine *engine, ax_arena *arena,
                            const pp_linearization *lin, const pp_query *query,
                            pocketparse_string input, const pp_query_sink *sink)
{
    uint32_t p;
    for (p = 0; p < lin->count; p++) {
        pp_query_state state;
        uint32_t       next;
        uint32_t       k;
        int            ok;
        if (lin->tokens[p].kind == PP_LIN_EXIT)
            continue;
        state.arena            = arena;
        state.lin              = lin;
        state.bindings         = 0;
        state.binding_count    = 0;
        state.binding_capacity = 0;
        if (!pp_query_match_node(&state, query->pattern, p, &next))
            continue;
        ok = 1;
        for (k = 0; k < query->predicate_count; k++) {
            if (!pp_query_eval_predicate(engine, arena, &query->predicates[k],
                                         &state, input)) {
                ok = 0;
                break;
            }
        }
        if (ok)
            pp_query_report(&state, input, sink);
    }
    return 1;
}

/* ** Read query .scm source, compile each top-level form, and run it. */
pocketparse_result pp_query_run(pocketparse_engine *engine,
                                const pocketparse_tree *tree,
                                pocketparse_string query_source,
                                pocketparse_string input,
                                const pp_query_sink *sink,
                                const char **error)
{
    ax_arena        *arena = engine->arena;
    pp_read_result   read;
    pp_linearization lin;
    uint32_t         i;
    const char      *local_error = 0;

    if (error == 0)
        error = &local_error;
    *error = 0;

    if (tree == 0 || tree->interner == 0) {
        *error = "query: no tree to query";
        return POCKETPARSE_INVALID;
    }
    if (!pp_linearize(engine, tree, &lin)) {
        *error = "query: out of memory linearizing tree";
        return POCKETPARSE_OUT_OF_MEMORY;
    }

    read = pp_read(arena, query_source);
    if (!read.ok) {
        *error = read.error ? read.error : "query: malformed query source";
        return POCKETPARSE_INVALID;
    }

    for (i = 0; i < read.count; i++) {
        pp_query query;
        if (!pp_query_compile(arena, tree->interner, read.forms[i], &query,
                              error))
            return POCKETPARSE_INVALID;
        pp_query_run_one(engine, arena, &lin, &query, input, sink);
    }
    return POCKETPARSE_OK;
}

/* ** Public: flatten a tree to its ENTER/LEAF/EXIT stream, reporting each token
** through the host callback. Internal token kinds and interned ids are resolved
** to the public token shape (kind/field names, leaf text) here, so the host sees
** only public types. */
pocketparse_result pocketparse_linearize(
    pocketparse_engine     *engine,
    const pocketparse_tree *tree,
    pocketparse_string      input,
    void (*emit)(void *user, const pocketparse_lin_token *token),
    void                   *user)
{
    pp_linearization lin;
    uint32_t         i;

    if (engine == 0 || tree == 0 || emit == 0)
        return POCKETPARSE_INVALID;
    if (!pp_linearize(engine, tree, &lin))
        return POCKETPARSE_OUT_OF_MEMORY;

    for (i = 0; i < lin.count; i++) {
        const pp_lin_token *t = &lin.tokens[i];
        pocketparse_lin_token out;
        switch (t->kind) {
        case PP_LIN_ENTER: out.kind = POCKETPARSE_LIN_ENTER; break;
        case PP_LIN_LEAF:  out.kind = POCKETPARSE_LIN_LEAF;  break;
        default:           out.kind = POCKETPARSE_LIN_EXIT;  break;
        }
        out.node_kind = pp_intern_name(lin.interner, t->node_kind);
        if (t->field != PP_NO_SYMBOL)
            out.field = pp_intern_name(lin.interner, t->field);
        else
            out.field = pp_text("");
        if (t->kind == PP_LIN_LEAF)
            out.text = pp_query_slice(input, t->start_byte, t->end_byte);
        else
            out.text = pp_text("");
        out.start_byte = t->start_byte;
        out.end_byte   = t->end_byte;
        emit(user, &out);
    }
    return POCKETPARSE_OK;
}

/* ** The public query sink trampoline: re-shape an internal pp_query_capture as
** the public pocketparse_query_capture and forward it to the host callback. The
** user pointer carries the host's emit/user pair. */
typedef struct pp_query_public_sink {
    void (*emit)(void *user, const pocketparse_query_capture *capture);
    void  *user;
} pp_query_public_sink;

/* ** Forward an internal capture to the public callback. */
static void pp_query_public_emit(void *user, const pp_query_capture *capture)
{
    pp_query_public_sink   *bridge = (pp_query_public_sink *)user;
    pocketparse_query_capture out;
    out.name       = capture->name;
    out.start_byte = capture->start_byte;
    out.end_byte   = capture->end_byte;
    out.text       = capture->text;
    bridge->emit(bridge->user, &out);
}

/* ** Public: run a .scm query over a tree, reporting captures through the host
** callback. Bridges the internal callback sink to the public one. */
pocketparse_result pocketparse_query(
    pocketparse_engine     *engine,
    const pocketparse_tree *tree,
    pocketparse_string      query_source,
    pocketparse_string      input,
    void (*emit)(void *user, const pocketparse_query_capture *capture),
    void                   *user)
{
    pp_query_public_sink bridge;
    pp_query_sink        sink;
    const char          *error = 0;

    if (engine == 0 || tree == 0 || emit == 0)
        return POCKETPARSE_INVALID;
    bridge.emit = emit;
    bridge.user = user;
    sink.emit   = pp_query_public_emit;
    sink.user   = &bridge;
    return pp_query_run(engine, tree, query_source, input, &sink, &error);
}
