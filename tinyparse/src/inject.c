/*
** inject.c -- language injection: parse a span with an embedded grammar (W62).
**
** Language injection is the marquee Phase 11 capability: a region of one
** grammar's source is parsed by ANOTHER grammar and spliced into one composite
** tree, with each region carrying the language it was parsed by. A grammar marks
** the region declaratively:
**
**     (inject grammar: std.javascript content: REGION)
**
** REGION is an ordinary sub-pattern that delimits the embedded span. When the
** executor reaches the inject node it first matches REGION (so the host parser
** advances over the span exactly as it would over any sub-pattern), then it
** re-parses those same bytes with the embedded grammar -- named by `grammar:',
** resolved and compiled once at COMPILE time through the engine's module
** resolver (grammar.c) so the parse path never reads the filesystem -- and
** splices the embedded tree under an `injection' node.
**
** The child runs as an INLINE sub-VM: a synchronous recursive-descent parse over
** the span via a throwaway sub-parser, not a spawned ax child actor. The
** ax-topologies design names the child-VM-actor shape as the ideal, but also
** sanctions the inline sub-parse for this pass -- "if suspending/resuming the
** parent VM across ax steps proves too large for this pass, run the child as an
** inline sub-VM ... that still produces the composite tree." Suspending the
** host VM mid-dispatch to await a done.child outcome would mean checkpointing the
** whole control stack across ax_step boundaries; the inline sub-VM gives the same
** composite, offset-mapped, language-tagged tree without that surgery, and the
** child still gets full isolation (its own arena, lexical goals, and backtrack
** stack) because it is a wholly separate sub-parser. Promoting it to a spawned
** actor later changes only this file.
**
** Three pieces of plumbing make the splice sound:
**   - OFFSET MAPPING: the child parses the span as if it began at byte 0, so every
**     child node's [start,end) is shifted by the span's absolute start when copied.
**   - RE-INTERNING: the child's kind/field names live in the child grammar's
**     interner; they are interned afresh into the HOST grammar's interner so the
**     composite tree names every node through one table.
**   - LANGUAGE TAG: every spliced node (and the injection node) is stamped with
**     the embedded grammar's name, surfaced by --dump-tree as a `[lang]'
**     annotation -- internal CST metadata, never a public API accessor.
**
** Freestanding: every allocation comes from the host parser's arena.
*/
#include "internal.h"

/* ** Copy the embedded subtree rooted at child_index (in `child') into the host
** builder as a descendant of host_parent, shifting byte spans by span_start,
** re-interning kind/field names into host_interner, and stamping language_tag on
** every node. Returns the new host node index, or PP_NO_INDEX on allocation
** failure. Recursive over the child tree; the child tree is bounded by the span,
** so the recursion depth is bounded by that subtree's depth. */
static uint32_t pp_inject_copy_node(pp_tree_builder *builder,
                                    pp_interner *host_interner,
                                    const pocketparse_tree *child,
                                    uint32_t child_index,
                                    uint32_t span_start,
                                    uint32_t language_tag,
                                    uint32_t host_parent)
{
    const pp_node *src = &child->nodes[child_index];
    uint32_t       kind  = PP_NO_SYMBOL;
    uint32_t       field = PP_NO_SYMBOL;
    uint32_t       node;
    uint32_t       walk;
    uint32_t       previous = PP_NO_INDEX;
    uint32_t       child_count = 0;
    uint32_t       first = PP_NO_INDEX;

    /* Re-intern the child's names into the host interner. The child interner names
    ** them; the host tree reads only its own interner, so each name is interned
    ** afresh and the new id used. This runs once per spliced node -- off the
    ** per-token hot path -- so interning here is sound. */
    if (src->kind != PP_NO_SYMBOL)
        kind = pp_intern(host_interner, pp_intern_name(child->interner, src->kind));
    if (src->field != PP_NO_SYMBOL)
        field = pp_intern(host_interner, pp_intern_name(child->interner, src->field));

    node = pp_tree_builder_push(builder, kind, field,
                                src->start_byte + span_start,
                                src->end_byte + span_start, host_parent);
    if (node == PP_NO_INDEX)
        return PP_NO_INDEX;
    builder->nodes[node].flags        = src->flags;
    builder->nodes[node].language_tag = language_tag;

    /* Copy the children in source order, threading sibling links. The builder may
    ** grow (and move) during a child copy, so re-fetch the parent's record by
    ** index every time rather than holding a pointer across the recursion. */
    walk = src->first_child;
    while (walk != PP_NO_INDEX) {
        uint32_t copied = pp_inject_copy_node(builder, host_interner, child, walk,
                                              span_start, language_tag, node);
        if (copied == PP_NO_INDEX)
            return PP_NO_INDEX;
        if (first == PP_NO_INDEX)
            first = copied;
        if (previous != PP_NO_INDEX)
            builder->nodes[previous].next_sibling = copied;
        previous = copied;
        child_count++;
        walk = child->nodes[walk].next_sibling;
    }
    builder->nodes[node].first_child = first;
    builder->nodes[node].child_count = child_count;
    return node;
}

/* ** Parse the span [span_start, span_end) of the host input with the embedded
** grammar, then splice the embedded tree (offset-mapped, re-interned, language-
** tagged) as the children of an injection node, which is itself appended to the
** caller's sink-of-indices. Returns 1 on success (the injection node was emitted),
** 0 on allocation failure. A failed embedded parse is NOT a host-parse failure:
** the embedded tree (with its own ERROR/diagnostics) is spliced regardless, so a
** malformed embedded region is contained under the injection node rather than
** derailing the host parse -- the isolation the actor topology promises.
**
** `append' is the host's "push a node index into the active child sink" callback,
** so this function serves both executors (parse.c's pp_sink and vm.c's pp_vm_sink)
** without depending on either's sink type. */
int pp_inject_run(pocketparse_parser *host, const pp_ir *inject,
                  uint32_t span_start, uint32_t span_end,
                  int (*append)(void *sink_user, uint32_t node_index),
                  void *sink_user)
{
    pocketparse_grammar *embedded = inject->embedded_grammar;
    uint32_t             language_tag = inject->language_symbol;
    uint32_t             injection_node;
    uint32_t             interned_lang;
    pocketparse_string   lang_name;
    pocketparse_result   sub_result;
    /* W17-3 native nesting: the host's registered language entry for this region's
    ** language, if any. When set, it supplies the embedded grammar AND (optionally)
    ** a separately-compiled generated parser's run function, so a generated host
    ** parser can drive a generated embedded parser without linking it directly.
    ** registered_parse stays NULL for the dynamic / unregistered case, in which the
    ** region is parsed by the in-process VM over `embedded' exactly as before. */
    const pocketparse_language_entry *registered;
    pocketparse_result (*registered_parse)(pocketparse_parser *) = 0;
    pocketparse_parser   sub;

    /* The injection node spans the whole embedded region and carries the language
    ** tag, so the dump shows a clear `injection [lang]' boundary. Its kind name is
    ** the fixed marker "injection", interned into the host grammar's interner. */
    injection_node = pp_tree_builder_push(&host->builder,
                                          pp_intern_c(&host->grammar->interner,
                                                      "injection"),
                                          PP_NO_SYMBOL, span_start, span_end,
                                          PP_NO_INDEX);
    if (injection_node == PP_NO_INDEX)
        return 0;
    host->builder.nodes[injection_node].language_tag = language_tag;

    /* The language tag's printable name lives in the inject IR's own grammar (it
    ** was interned there at compile time); re-intern it into the host interner so
    ** the host tree names it. Recompute it here rather than trust the compile-time
    ** id, which belongs to the host interner already (the inject form interned the
    ** name into the host grammar's interner at compile time -- see grammar.c), so
    ** language_tag is already a host id. interned_lang keeps that explicit. */
    lang_name     = pp_intern_name(&host->grammar->interner, language_tag);
    interned_lang = pp_intern(&host->grammar->interner, lang_name);
    host->builder.nodes[injection_node].language_tag = interned_lang;

    /* Resolve the embedded language through the engine's registry by name (W17-3).
    ** A registration takes precedence over the compile-time embedded_grammar: it
    ** supplies the grammar whose interner names the spliced nodes AND, when the host
    ** registered a generated parser, the run function that re-parses the region
    ** natively. When nothing is registered, fall back to the inject IR's own
    ** compile-time embedded_grammar (the in-grammar injection case), parsed by the
    ** VM. This is how a host binary names a language without linking it: the
    ** registration wires the two separately-compiled parsers at runtime. */
    registered = pp_engine_find_language(host->engine, lang_name);
    if (registered != 0) {
        if (registered->grammar != 0)
            embedded = registered->grammar;
        registered_parse = registered->parse;
    }

    if (embedded == 0) {
        /* No embedded grammar resolved (it should have failed at compile time):
        ** emit the bare injection node so the structure is visible, then stop. */
        return append(sink_user, injection_node);
    }

    /* Build a throwaway sub-parser over the span bytes and run the reference
    ** recursive-descent executor on it. The sub-parser shares the host arena (so
    ** its nodes outlive this call for the splice) and the host engine (so embedded
    ** (class NAME) bindings and module wiring resolve), but it has its OWN builder,
    ** diagnostics, input buffer, and grammar -- full isolation, exactly the child's
    ** independent context the topology calls for. */
    sub.engine         = host->engine;
    sub.grammar        = embedded;
    sub.start_symbol   = embedded->start_symbol;
    sub.arena          = host->arena;
    sub.input          = host->input + span_start;
    sub.input_length   = span_end - span_start;
    sub.input_capacity = span_end - span_start;
    sub.finished       = 1;
    sub.builder.arena  = host->arena;
    sub.builder.nodes  = 0;
    sub.builder.count  = 0;
    sub.builder.capacity = 0;
    sub.diagnostics.arena = host->arena;
    sub.diagnostics.items = 0;
    sub.diagnostics.count = 0;
    sub.diagnostics.capacity = 0;
    sub.tree         = 0;
    sub.actor_pid    = (ax_pid){ 0, 0 };
    sub.actor_result = POCKETPARSE_OK;
    sub.actor_ran    = 0;
    sub.push_failed  = POCKETPARSE_OK;
    sub.slot_save_stack    = 0;
    sub.slot_save_count    = 0;
    sub.slot_save_capacity = 0;
    sub.slot_scopes        = 0;
    sub.slot_scope_count    = 0;
    sub.slot_scope_capacity = 0;
    /* The embedded grammar may itself declare parametrized goals; give the
    ** sub-parser its own param-stack array (from the shared host arena, like the
    ** sub-parser's other allocations) rather than leaving it unallocated, so the
    ** host and an injected grammar are initialized identically. 0 when the embedded
    ** grammar has no parametrized goals. On allocation failure fall back to 0; the
    ** failure surfaces as the embedded parse's own OOM rather than aborting the host. */
    if (!pp_param_stacks_alloc(host->arena, embedded, &sub.param_stacks))
        sub.param_stacks = 0;

    /* Run the embedded parse over the span. Two modes, ax's choice (W17-3):
    **   - NATIVE NESTING: when the host registered a generated parser for this
    **     language, drive its run function (registered_parse) over the sub-parser.
    **     The generated parser builds the SAME arena CST the VM would (vm-diff /
    **     cc-diff), so the spliced subtree is byte-identical -- a generated host
    **     parser nesting a generated embedded parser yields the same tree as the VM.
    **   - DYNAMIC: otherwise the inline child runs the in-process bytecode VM
    **     (pp_execute_vm) over the span -- today's behavior, unchanged.
    ** Either way the parse is a SUPERVISED, isolated child: it has its own builder,
    ** diagnostics, grammar, and input window, sharing only the host arena so its
    ** nodes outlive this call for the splice. A failed embedded parse is contained
    ** -- its tree (with its own ERROR/diagnostics) is spliced under the language-
    ** tagged injection node regardless, so the host tree and process are untouched.
    ** Injection depth is bounded by injection nesting (a generated host parser runs
    ** registered_parse synchronously over the fully-known span, with no suspension),
    ** not by input depth. */
    if (registered_parse != 0) {
        sub.finished = 1;
        sub_result   = registered_parse(&sub);
    } else {
        sub_result = pp_execute_vm(&sub);
    }
    (void)sub_result;

    /* Splice the embedded tree's root subtree under the injection node, mapping
    ** the child's relative offsets back to absolute host positions and re-interning
    ** every name into the host interner. The child tree's root is the embedded
    ** start rule's node; we splice the root itself so the embedded document's own
    ** top node (e.g. JSON `Document') appears under the injection node. */
    if (sub.tree != 0 && sub.tree->node_count > 0) {
        uint32_t copied = pp_inject_copy_node(&host->builder,
                                              &host->grammar->interner,
                                              sub.tree, sub.tree->root,
                                              span_start, interned_lang,
                                              injection_node);
        if (copied == PP_NO_INDEX)
            return 0;
        host->builder.nodes[injection_node].first_child = copied;
        host->builder.nodes[injection_node].child_count = 1;
        host->builder.nodes[copied].next_sibling = PP_NO_INDEX;
    }

    return append(sink_user, injection_node);
}
