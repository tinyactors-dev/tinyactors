/*
** parse.c -- the streaming parser front end.
**
** pocketparse_push buffers input bytes in arbitrary chunks; pocketparse_finish
** runs the compiled grammar over the whole buffer by driving the iterative
** bytecode VM -- inside an ax actor when one is available (the library default),
** in process otherwise. The VM, its token matcher, and the contiguous tree
** builder live in vm.c / token.c / tree.c. This file owns only the parser
** lifecycle and the input buffer; the recursive-descent IR interpreter that once
** lived here was retired in W14-5 once the bytecode VM became the
** sole engine, with the conformance suite as the standing correctness anchor and
** vm-diff (vm vs vm-ax) and cc-diff as consistency checks. Freestanding: all
** memory is arena memory.
*/
#include "internal.h"

/* ** Append input bytes to the parser's growable buffer. */
pocketparse_result pp_parser_buffer(pocketparse_parser *parser,
                                    pocketparse_string bytes)
{
    uint32_t i;
    if (parser->input_length + bytes.length > parser->input_capacity) {
        uint32_t nc = parser->input_capacity == 0 ? 256 : parser->input_capacity;
        uint8_t *grown;
        while (parser->input_length + bytes.length > nc)
            nc *= 2;
        grown = (uint8_t *)ax_alloc_raw(parser->arena, nc);
        if (grown == 0)
            return POCKETPARSE_OUT_OF_MEMORY;
        if (parser->input_length != 0)
            __builtin_memcpy(grown, parser->input, parser->input_length);
        parser->input = grown;
        parser->input_capacity = nc;
    }
    if (bytes.length != 0)
        __builtin_memcpy(parser->input + parser->input_length,
                         bytes.pointer, bytes.length);
    parser->input_length += bytes.length;
    (void)i;
    return POCKETPARSE_OK;
}

/* ** Create a streaming parser for a grammar, starting at a named rule. The
** parser owns its own arena carved from the engine grant, so freeing it is the
** host's business (the arena lives in the engine block). */
pocketparse_result pocketparse_parser_new(
    pocketparse_engine  *engine,
    pocketparse_grammar *grammar,
    pocketparse_string   start_rule,
    pocketparse_parser **out_parser)
{
    pocketparse_parser *parser;
    uint32_t            start_symbol;

    if (out_parser != 0)
        *out_parser = 0;
    if (engine == 0 || grammar == 0)
        return POCKETPARSE_INVALID;

    if (start_rule.length == 0) {
        start_symbol = grammar->start_symbol;
    } else if (!pp_intern_find(&grammar->interner, start_rule, &start_symbol)) {
        engine->fault = pp_text("start rule not found in grammar");
        return POCKETPARSE_INVALID;
    }

    /* The parser STRUCT lives in the engine arena (stable, ~one struct per parse,
    ** negligible) so the parser actor's back-pointer to it never dangles. The parse
    ** DATA -- builder, sinks, memos, input buffer, and the tree -- goes in a child
    ** arena sharing the engine's pool, so pocketparse_parser_free can hand all of it
    ** back to the pool for the next parse to reuse: a whole codebase parses in one
    ** engine without accreting every tree. CONTRACT: the tree is valid until
    ** pocketparse_parser_free; use it (or copy out what you need) before freeing. */
    parser = (pocketparse_parser *)ax_alloc(engine->arena, sizeof *parser);
    if (parser == 0)
        return POCKETPARSE_OUT_OF_MEMORY;
    {
        ax_arena *child = ax_arena_child(engine->arena);
        if (child == 0)
            return POCKETPARSE_OUT_OF_MEMORY;
        parser->arena         = child;
        parser->builder.arena = child;
        parser->diagnostics.arena = child;
    }

    parser->engine        = engine;
    parser->grammar       = grammar;
    parser->start_symbol  = start_symbol;
    parser->input         = 0;
    parser->input_length  = 0;
    parser->input_capacity = 0;
    parser->finished      = 0;
    /* Whole input known up front by default (the historical finish/run path,
    ** byte-identical). A buffer-friendly host clears this before each non-final
    ** run_chunk and sets it on the final one (see pocketparse_parser, W17-4). */
    parser->input_complete = 1;
    parser->bf_active      = 0;
    parser->bf_root        = PP_NO_INDEX;
    parser->bf_position    = 0;
    parser->bf_children.items    = 0;
    parser->bf_children.count    = 0;
    parser->bf_children.capacity = 0;
    parser->bf_children.parent   = PP_NO_INDEX;
    parser->builder.nodes = 0;
    parser->builder.count = 0;
    parser->builder.capacity = 0;
    parser->diagnostics.items = 0;
    parser->diagnostics.count = 0;
    parser->diagnostics.capacity = 0;
    parser->tree = 0;
    parser->actor_pid    = (ax_pid){ 0, 0 };
    parser->actor_result = POCKETPARSE_OK;
    parser->actor_ran    = 0;
    parser->push_failed  = POCKETPARSE_OK;
    parser->slot_save_stack    = 0;
    parser->slot_save_count    = 0;
    parser->slot_save_capacity = 0;
    parser->slot_scopes        = 0;
    parser->slot_scope_count    = 0;
    parser->slot_scope_capacity = 0;
    /* Per-parametrized-goal stacks, one per parametrized goal entry, allocated from
    ** the parse CHILD arena -- so they are released by pocketparse_parser_free's
    ** arena reset and freshly re-derived on the next parser_new (the spec's
    ** "fully re-initialized when changing grammars" contract). 0 when the grammar
    ** has no parametrized goals. Not read by the lexer until Phase 8. */
    if (!pp_param_stacks_alloc(parser->arena, grammar, &parser->param_stacks))
        return POCKETPARSE_OUT_OF_MEMORY;

    /* W60: on the actor-hosted VM path, spawn the parser actor now so push can
    ** post input.chunk to it. A spawn failure (or no ax system on a small grant)
    ** simply leaves actor_pid null; pocketparse_push and pocketparse_finish then
    ** fall back to the in-process VM, so the flag never breaks a parse. */
    if (engine->engine_use_vm_actor && engine->system != 0)
        (void)pp_parser_actor_spawn(parser);

    *out_parser = parser;
    return POCKETPARSE_OK;
}

/* ** Release a parser by resetting its child arena: every byte this parse pulled
** -- the parser struct, the builder, the sinks, the memos, AND the tree -- returns
** to the engine's pool for the next parse to reuse, so a codebase parses in one
** engine without accreting. The parser and its tree are invalid after this call
** (the tree's lifetime ends here; copy out anything you still need first). The
** arena handle itself lives in the engine arena and outlives the reset, so reading
** parser->arena before resetting is safe. On the actor path the parser's actor has
** already reached its final state during finish (pumped to quiescence), so ax has
** reaped it; nothing in the ax system is left dangling. */
void pocketparse_parser_free(pocketparse_parser *parser)
{
    ax_arena *arena;
    if (parser == 0)
        return;
    arena = parser->arena;
    ax_arena_reset(arena);
}

/* ** Feed the next chunk of input. Buffering tolerates arbitrary chunk sizes,
** so callers may push one byte at a time. */
pocketparse_result pocketparse_push(pocketparse_parser *parser,
                                    pocketparse_string bytes)
{
    pocketparse_result result;
    if (parser == 0)
        return POCKETPARSE_INVALID;
    if (parser->finished)
        return POCKETPARSE_INVALID;
    if (bytes.length == 0)
        return POCKETPARSE_OK;
    /* W60: on the actor path with a live actor, buffer the chunk through
    ** pp_parser_actor_push (which writes it into the parser arena, never the ax
    ** system slice). Otherwise buffer directly, exactly as before. */
    if (parser->engine != 0 && parser->engine->engine_use_vm_actor &&
        ax_pid_ok(parser->actor_pid))
        result = pp_parser_actor_push(parser, bytes);
    else
        result = pp_parser_buffer(parser, bytes);
    /* Remember the FIRST failure so finish cannot later report a successful-
    ** looking parse over input that was never fully buffered. */
    if (result != POCKETPARSE_OK && parser->push_failed == POCKETPARSE_OK)
        parser->push_failed = result;
    return result;
}

/* ** Signal end of input and run the parse. */
pocketparse_result pocketparse_finish(pocketparse_parser *parser)
{
    if (parser == 0)
        return POCKETPARSE_INVALID;
    if (parser->finished)
        return parser->tree != 0 ? POCKETPARSE_OK : POCKETPARSE_SYNTAX_ERROR;
    parser->finished = 1;
    /* A push dropped a chunk: the buffered input is incomplete, so any tree we
    ** could build would be a lie. Surface the failure instead of running the VM
    ** over a truncated buffer -- this is the guard that keeps a dropped chunk
    ** from passing as a successful one-node parse (the W60 silent corruption). */
    if (parser->push_failed != POCKETPARSE_OK) {
        if (parser->engine != 0)
            parser->engine->fault = pp_text("input chunk was dropped before finish");
        return parser->push_failed;
    }
    /* The bytecode VM is the only engine (W14-5 retired the recursive interpreter).
    ** Prefer the actor-hosted VM when its private selector is set -- the library
    ** default, also reached via POCKETPARSE_ENGINE=vm-ax: the dispatch loop runs
    ** inside the parser actor's input.end action and the tree is byte-identical to
    ** the in-process VM. With no ax system (a grant too small to carve one, or the
    ** CLI's POCKETPARSE_ENGINE=vm), fall back to the SAME VM in process. Either way
    ** pp_execute_vm always works, so there is no further fallback to reach. */
    if (parser->engine != 0 && parser->engine->engine_use_vm_actor)
        return pp_execute_vm_actor(parser);
    return pp_execute_vm(parser);
}
