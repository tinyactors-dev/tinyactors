/*
** parse.c -- the streaming parser front end.
**
** tinyparse_push buffers input bytes in arbitrary chunks; tinyparse_finish
** runs the compiled grammar over the whole buffer by driving the iterative
** bytecode VM. The VM, its token matcher, and the contiguous tree builder live
** in vm.c / token.c / tree.c. This file owns only the parser lifecycle (the
** state machine tp_parser_state documents in internal.h) and the input buffer;
** the recursive-descent IR interpreter that once lived here was retired in W14-5
** once the bytecode VM became the sole engine, with the conformance suite as the
** standing correctness anchor and cc-diff as the consistency check against the
** generated parsers. Freestanding: all memory is arena memory.
*/
#include "internal.h"

/* ** Append input bytes to the parser's growable buffer. */
tinyparse_result tp_parser_buffer(tinyparse_parser *parser,
                                    tinyparse_string bytes)
{
    uint32_t i;
    if (parser->input_length + bytes.length > parser->input_capacity) {
        uint32_t nc = parser->input_capacity == 0 ? 256 : parser->input_capacity;
        uint8_t *grown;
        while (parser->input_length + bytes.length > nc)
            nc *= 2;
        grown = (uint8_t *)tp_alloc_raw(parser->arena, nc);
        if (grown == 0)
            return TINYPARSE_OUT_OF_MEMORY;
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
    return TINYPARSE_OK;
}

/* ** Create a streaming parser for a grammar, starting at a named rule. The
** parser owns its own arena carved from the engine grant, so freeing it is the
** host's business (the arena lives in the engine block). */
tinyparse_result tinyparse_parser_new(
    tinyparse_engine  *engine,
    tinyparse_grammar *grammar,
    tinyparse_string   start_rule,
    tinyparse_parser **out_parser)
{
    tinyparse_parser *parser;
    uint32_t            start_symbol;

    if (out_parser != 0)
        *out_parser = 0;
    if (engine == 0 || grammar == 0)
        return TINYPARSE_INVALID;

    if (start_rule.length == 0) {
        start_symbol = grammar->start_symbol;
    } else if (!tp_intern_find(&grammar->interner, start_rule, &start_symbol)) {
        engine->fault = tp_text("start rule not found in grammar");
        return TINYPARSE_INVALID;
    }

    /* The parser STRUCT lives in the engine arena (stable, ~one struct per parse,
    ** negligible) so a pointer to it stays valid after the parse is freed. The parse
    ** DATA -- builder, sinks, memos, input buffer, and the tree -- goes in a child
    ** arena sharing the engine's pool, so tinyparse_parser_free can hand all of it
    ** back to the pool for the next parse to reuse: a whole codebase parses in one
    ** engine without accreting every tree. CONTRACT: the tree is valid until
    ** tinyparse_parser_free; use it (or copy out what you need) before freeing. */
    parser = (tinyparse_parser *)tp_alloc(engine->arena, sizeof *parser);
    if (parser == 0)
        return TINYPARSE_OUT_OF_MEMORY;
    {
        tp_arena *child = tp_arena_child(engine->arena);
        if (child == 0)
            return TINYPARSE_OUT_OF_MEMORY;
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
    parser->state          = TP_PARSER_ACCEPTING;
    /* Whole input known up front by default (the historical finish/run path,
    ** byte-identical). A buffer-friendly host clears this before each non-final
    ** run_chunk and sets it on the final one (see tinyparse_parser, W17-4). */
    parser->input_complete = 1;
    parser->bf_active      = 0;
    parser->bf_root        = TP_NO_INDEX;
    parser->bf_position    = 0;
    parser->bf_children.items    = 0;
    parser->bf_children.count    = 0;
    parser->bf_children.capacity = 0;
    parser->bf_children.parent   = TP_NO_INDEX;
    parser->builder.nodes = 0;
    parser->builder.count = 0;
    parser->builder.capacity = 0;
    parser->diagnostics.items = 0;
    parser->diagnostics.count = 0;
    parser->diagnostics.capacity = 0;
    parser->tree = 0;
    parser->push_failed  = TINYPARSE_OK;
    parser->slot_save_stack    = 0;
    parser->slot_save_count    = 0;
    parser->slot_save_capacity = 0;
    parser->slot_scopes        = 0;
    parser->slot_scope_count    = 0;
    parser->slot_scope_capacity = 0;
    /* Per-parametrized-goal stacks, one per parametrized goal entry, allocated from
    ** the parse CHILD arena -- so they are released by tinyparse_parser_free's
    ** arena reset and freshly re-derived on the next parser_new (the spec's
    ** "fully re-initialized when changing grammars" contract). 0 when the grammar
    ** has no parametrized goals. Not read by the lexer until Phase 8. */
    if (!tp_param_stacks_alloc(parser->arena, grammar, &parser->param_stacks))
        return TINYPARSE_OUT_OF_MEMORY;

    *out_parser = parser;
    return TINYPARSE_OK;
}

/* ** Release a parser by resetting its child arena: every byte this parse pulled
** -- the parser struct, the builder, the sinks, the memos, AND the tree -- returns
** to the engine's pool for the next parse to reuse, so a codebase parses in one
** engine without accreting. The parser and its tree are invalid after this call
** (the tree's lifetime ends here; copy out anything you still need first). The
** arena handle itself lives in the engine arena and outlives the reset, so reading
** parser->arena before resetting is safe. */
void tinyparse_parser_free(tinyparse_parser *parser)
{
    tp_arena *arena;
    if (parser == 0)
        return;
    arena = parser->arena;
    tp_arena_reset(arena);
}

/* ** Feed the next chunk of input: the ACCEPTING and DROPPED states buffer it.
** Buffering tolerates arbitrary chunk sizes, so callers may push one byte at a
** time. The first push that cannot buffer moves the parser to DROPPED. */
tinyparse_result tinyparse_push(tinyparse_parser *parser,
                                    tinyparse_string bytes)
{
    tinyparse_result result;
    if (parser == 0)
        return TINYPARSE_INVALID;
    if (parser->state == TP_PARSER_FINISHED || parser->state == TP_PARSER_FAILED)
        return TINYPARSE_INVALID;
    if (bytes.length == 0)
        return TINYPARSE_OK;
    result = tp_parser_buffer(parser, bytes);
    if (result != TINYPARSE_OK && parser->state == TP_PARSER_ACCEPTING) {
        parser->push_failed = result;
        parser->state       = TP_PARSER_DROPPED;
    }
    return result;
}

/* ** The finishing transition (see internal.h). From DROPPED (and again from
** FAILED, when a generated run entry is called a second time) the executor never
** runs: the buffered input is incomplete, so any tree it built would be a lie --
** this is the guard that keeps a dropped chunk from passing as a successful
** one-node parse. Otherwise the executor runs to completion, in this call, and
** the parser is FINISHED. */
tinyparse_result tp_parser_run(tinyparse_parser *parser,
                                 tp_parser_executor execute)
{
    tinyparse_result result;
    if (parser->state == TP_PARSER_DROPPED || parser->state == TP_PARSER_FAILED) {
        parser->state = TP_PARSER_FAILED;
        return parser->push_failed;
    }
    result        = execute(parser);
    parser->state = TP_PARSER_FINISHED;
    return result;
}

/* ** Signal end of input and run the parse with the bytecode VM. A parser that
** has already finished (or failed) is not run again: the result reports whether
** a tree exists. */
tinyparse_result tinyparse_finish(tinyparse_parser *parser)
{
    if (parser == 0)
        return TINYPARSE_INVALID;
    if (parser->state == TP_PARSER_FINISHED || parser->state == TP_PARSER_FAILED)
        return parser->tree != 0 ? TINYPARSE_OK : TINYPARSE_SYNTAX_ERROR;
    if (parser->state == TP_PARSER_DROPPED && parser->engine != 0)
        parser->engine->fault = tp_text("input chunk was dropped before finish");
    return tp_parser_run(parser, tp_execute_vm);
}

/* ** The lowercase name of the parser's lifecycle state. */
const char *tp_parser_state_name(const tinyparse_parser *parser)
{
    if (parser == 0)
        return "none";
    switch (parser->state) {
    case TP_PARSER_ACCEPTING: return "accepting";
    case TP_PARSER_DROPPED:   return "dropped";
    case TP_PARSER_FINISHED:  return "finished";
    case TP_PARSER_FAILED:    return "failed";
    }
    return "unknown";
}
