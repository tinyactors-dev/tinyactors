/*
** pack.c -- the binary grammar format: serialize a compiled grammar to a
** self-describing `.ppg` image and load one back into a usable grammar.
**
** The two functions here are mirror images. pocketparse_grammar_serialize walks
** a compiled grammar and writes a little-endian blob; pocketparse_load_binary_
** grammar reads that blob and rebuilds an equivalent pocketparse_grammar. A
** round trip is lossless: a grammar loaded from the bytes parses byte-for-byte
** identically to the .scm it was packed from.
**
** Freestanding, like the rest of src/ except cli.c: no libc, no IO. The host
** supplies the bytes (load) and is handed the bytes (serialize); every
** allocation comes from the engine arena via ax_alloc. The serializer runs in
** two passes -- measure, then emit -- so it needs a single arena block of the
** exact size rather than a growable buffer (ax arenas never realloc).
**
** Image layout (all integers little-endian, 32-bit unless noted):
**
**   "PPG1"                     magic (4 bytes)
**   version                    PP_PACK_VERSION
**   abi_tag                    PP_PACK_ABI, guards against struct/enum drift
**   name_count                 interned string table
**     [ length, bytes[length] ]*
**   token_count
**     [ symbol, IR(pattern) ]*
**   rule_count
**     [ symbol, IR(body) ]*
**   extra_count
**     [ token_symbol, policy, attach ]*
**   start_symbol
**
** An IR node is written depth-first:
**
**   op, text_length, text_bytes,
**   class_bits[32], class_negated,
**   symbol, min, max,
**   child_count, [ IR(child) ]*
**   [ recover_message, recover_help, recover_label, recover_insert ]?
**
** The four recover_* slices are present only when op == PP_OP_RECOVER, so an
** ordinary IR node serializes exactly as before and old-shaped images still
** describe the same grammar. A PP_OP_INJECT node adds one field after its
** children:
**
**   [ language_symbol ]?            (op == PP_OP_INJECT only)
**
** language_symbol names the embedded grammar (an id into this grammar's interner).
** The embedded grammar itself -- a compile-time pointer that cannot be a raw
** address in an image -- is serialized in a self-contained trailing section so a
** loaded grammar carries its nested languages with it and needs no module
** resolver to re-resolve them:
**
**   embedded_count                  (gated trailing section; 0 if no injects)
**     [ language_symbol, image_length, IMAGE(embedded grammar) ]*
**
** Each IMAGE is a full nested .ppg (magic, version, abi, the lot), so an embedded
** grammar that itself injects a third language round-trips recursively. On load,
** the section is decoded into a map language_symbol -> grammar and every inject
** node's embedded_grammar pointer is wired from it. This is what makes the
** generated (compiled-to-C) parser preserve language embedding: it loads the
** packed grammar with no resolver, yet the embedded grammar is right there.
**
** A PP_OP_CLASS_NAME node (a (class NAME) token primitive) carries its class
** name -- the host CLASS binding it resolves against -- in the IR's text slice,
** the very field every op already writes. So a named class round-trips with no
** new on-disk field, gated on the op only in the sense that the name simply lives
** where the op puts it; the format is unchanged and backward compatible.
*/
#include "internal.h"

/* ** Format constants. The magic identifies a .ppg image at a glance; the
** version tracks layout revisions; the ABI tag mixes the sizes of the structures
** the image mirrors so a load against a mismatched build is refused rather than
** silently misread. */
#define PP_PACK_MAGIC0 'P'
#define PP_PACK_MAGIC1 'P'
#define PP_PACK_MAGIC2 'G'
#define PP_PACK_MAGIC3 '1'
#define PP_PACK_VERSION 4u
#define PP_PACK_ABI \
    ((uint32_t)(sizeof(pp_ir) + sizeof(pp_class) + sizeof(pp_token_def) + \
                sizeof(pp_rule_def) + sizeof(pp_extra) + sizeof(pp_lex_goal) + \
                (uint32_t)PP_OP_EOF * 131u + (uint32_t)PP_EXTRA_ERROR * 17u + \
                (uint32_t)PP_EXTRA_ATTACH_LEADING * 19u))

/* ** A cursor over a byte image. On write, `bytes` is either NULL (measuring
** pass -- only `length` advances) or a real buffer being filled. On read,
** `bytes`/`limit` bound the input and `error` flips to nonzero the moment a
** read would run past the end, so callers can test it once at the end. */
typedef struct pp_pack_cursor {
    uint8_t *bytes;
    uint32_t length;   /* bytes written/read so far */
    uint32_t limit;    /* total readable bytes (read side) */
    int      error;    /* set on overrun (read side) */
} pp_pack_cursor;

/* ** Write one byte, or just count it during the measuring pass. */
static void pp_put_byte(pp_pack_cursor *cursor, uint8_t value)
{
    if (cursor->bytes != 0)
        cursor->bytes[cursor->length] = value;
    cursor->length++;
}

/* ** Write a 32-bit value little-endian. */
static void pp_put_u32(pp_pack_cursor *cursor, uint32_t value)
{
    pp_put_byte(cursor, (uint8_t)(value & 0xFFu));
    pp_put_byte(cursor, (uint8_t)((value >> 8) & 0xFFu));
    pp_put_byte(cursor, (uint8_t)((value >> 16) & 0xFFu));
    pp_put_byte(cursor, (uint8_t)((value >> 24) & 0xFFu));
}

/* ** Write a length-prefixed byte slice. */
static void pp_put_bytes(pp_pack_cursor *cursor, pocketparse_string slice)
{
    uint32_t i;
    pp_put_u32(cursor, slice.length);
    for (i = 0; i < slice.length; i++)
        pp_put_byte(cursor, slice.pointer[i]);
}

/* ** Read one byte, flagging overrun. Returns 0 past the end. */
static uint8_t pp_get_byte(pp_pack_cursor *cursor)
{
    uint8_t value;
    if (cursor->length >= cursor->limit) {
        cursor->error = 1;
        return 0;
    }
    value = cursor->bytes[cursor->length];
    cursor->length++;
    return value;
}

/* ** Read a 32-bit little-endian value, flagging overrun. */
static uint32_t pp_get_u32(pp_pack_cursor *cursor)
{
    uint32_t b0 = pp_get_byte(cursor);
    uint32_t b1 = pp_get_byte(cursor);
    uint32_t b2 = pp_get_byte(cursor);
    uint32_t b3 = pp_get_byte(cursor);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

/* ** Read a length-prefixed name into the arena; defined below, declared here so
** pp_read_ir can read a recover node's payload before the definition appears. */
static int pp_read_name(ax_arena *arena, pp_pack_cursor *cursor,
                        pocketparse_string *out_name);

/* ** Serialize one IR node depth-first into the cursor. */
static void pp_write_ir(pp_pack_cursor *cursor, const pp_ir *ir)
{
    uint32_t i;
    pp_put_u32(cursor, (uint32_t)ir->op);
    pp_put_bytes(cursor, ir->text);
    for (i = 0; i < 32; i++)
        pp_put_byte(cursor, ir->cls.bits[i]);
    pp_put_u32(cursor, (uint32_t)ir->cls.negated);
    pp_put_u32(cursor, ir->symbol);
    pp_put_u32(cursor, ir->min);
    pp_put_u32(cursor, ir->max);
    pp_put_u32(cursor, ir->child_count);
    for (i = 0; i < ir->child_count; i++)
        pp_write_ir(cursor, ir->children[i]);
    /* The recovery payload travels only with a PP_OP_RECOVER node; every other
    ** op leaves these fields unset (token.c mints IR without them), so reading
    ** them would be undefined -- write them solely where they are meaningful. */
    if (ir->op == PP_OP_RECOVER) {
        pp_put_bytes(cursor, ir->recover_message);
        pp_put_bytes(cursor, ir->recover_help);
        pp_put_bytes(cursor, ir->recover_label);
        pp_put_bytes(cursor, ir->recover_insert);
    }
    /* The injection payload travels only with a PP_OP_INJECT node: language_symbol,
    ** the embedded grammar's name interned into this grammar's interner. The
    ** embedded grammar itself rides in the trailing embedded-grammars section (see
    ** pp_write_grammar); language_symbol is the key that section is read back by.
    ** Without this an inject node loads with embedded_grammar == 0 and the executor
    ** emits a bare injection node -- the language embedding silently dropped. */
    if (ir->op == PP_OP_INJECT)
        pp_put_u32(cursor, ir->language_symbol);
}

/* ** Visit every IR node reachable from `ir' depth-first, calling `visit(user,
** node)' on each. Used to find the PP_OP_INJECT nodes a grammar carries so their
** distinct embedded grammars can be serialized. */
static void pp_ir_walk(const pp_ir *ir,
                       void (*visit)(void *user, const pp_ir *ir),
                       void *user)
{
    uint32_t i;
    if (ir == 0)
        return;
    visit(user, ir);
    for (i = 0; i < ir->child_count; i++)
        pp_ir_walk(ir->children[i], visit, user);
}

/* ** A small accumulator of the distinct embedded grammars an inject-using grammar
** references. Two inject nodes that name the same language share one grammar
** pointer (resolved once at compile time), so dedup is a pointer-identity check;
** at most PP_PACK_MAX_EMBEDDED distinct languages are collected, which is ample
** for real grammars and bounds the work. */
enum { PP_PACK_MAX_EMBEDDED = 64 };
typedef struct pp_embedded_set {
    pocketparse_grammar *grammars[PP_PACK_MAX_EMBEDDED];
    uint32_t             symbols[PP_PACK_MAX_EMBEDDED];
    uint32_t             count;
    int                  overflow;
} pp_embedded_set;

/* ** Record one inject node's embedded grammar in the set, deduplicating by
** grammar pointer. A node with no resolved embedded grammar (embedded_grammar ==
** 0) is skipped -- it serializes its language_symbol but contributes no image, so
** the loader leaves it unresolved exactly as it was. */
static void pp_embedded_collect(void *user, const pp_ir *ir)
{
    pp_embedded_set *set = (pp_embedded_set *)user;
    uint32_t i;
    if (ir->op != PP_OP_INJECT || ir->embedded_grammar == 0)
        return;
    for (i = 0; i < set->count; i++)
        if (set->grammars[i] == ir->embedded_grammar)
            return;
    if (set->count >= PP_PACK_MAX_EMBEDDED) {
        set->overflow = 1;
        return;
    }
    set->grammars[set->count] = ir->embedded_grammar;
    set->symbols[set->count]  = ir->language_symbol;
    set->count++;
}

/* ** Gather every distinct embedded grammar reachable from `grammar's token and
** rule IR into `set'. */
static void pp_collect_embedded(const pocketparse_grammar *grammar,
                                pp_embedded_set *set)
{
    uint32_t i;
    set->count    = 0;
    set->overflow = 0;
    for (i = 0; i < grammar->token_count; i++)
        pp_ir_walk(grammar->tokens[i].pattern, pp_embedded_collect, set);
    for (i = 0; i < grammar->rule_count; i++)
        pp_ir_walk(grammar->rules[i].body, pp_embedded_collect, set);
}

/* ** Serialize the whole grammar into the cursor. Run once with cursor->bytes
** NULL to size the image, then again over a real buffer to fill it; the two
** passes walk identical paths so the lengths match exactly. */
static void pp_write_grammar(pp_pack_cursor *cursor,
                             const pocketparse_grammar *grammar)
{
    uint32_t i;

    pp_put_byte(cursor, PP_PACK_MAGIC0);
    pp_put_byte(cursor, PP_PACK_MAGIC1);
    pp_put_byte(cursor, PP_PACK_MAGIC2);
    pp_put_byte(cursor, PP_PACK_MAGIC3);
    pp_put_u32(cursor, PP_PACK_VERSION);
    pp_put_u32(cursor, PP_PACK_ABI);

    pp_put_u32(cursor, grammar->interner.count);
    for (i = 0; i < grammar->interner.count; i++)
        pp_put_bytes(cursor, grammar->interner.names[i]);

    pp_put_u32(cursor, grammar->token_count);
    for (i = 0; i < grammar->token_count; i++) {
        pp_put_u32(cursor, grammar->tokens[i].symbol);
        pp_write_ir(cursor, grammar->tokens[i].pattern);
    }

    pp_put_u32(cursor, grammar->rule_count);
    for (i = 0; i < grammar->rule_count; i++) {
        pp_put_u32(cursor, grammar->rules[i].symbol);
        /* W17-FLAT: the per-rule flags (PP_RULE_HIDDEN) ride alongside the body so
        ** a packed grammar collapses identically to the .scm it was built from. */
        pp_put_u32(cursor, grammar->rules[i].flags);
        /* Per-rule deferred-slot count (lex-goal-implementation), written between
        ** flags and body so the read loop mirrors this order. */
        pp_put_u32(cursor, grammar->rules[i].slot_count);
        pp_write_ir(cursor, grammar->rules[i].body);
    }

    pp_put_u32(cursor, grammar->extra_count);
    for (i = 0; i < grammar->extra_count; i++) {
        pp_put_u32(cursor, grammar->extras[i].token_symbol);
        pp_put_u32(cursor, (uint32_t)grammar->extras[i].policy);
        pp_put_u32(cursor, (uint32_t)grammar->extras[i].attach);
    }

    pp_put_u32(cursor, grammar->start_symbol);

    /* Lexical-goal table. As of PP_PACK_VERSION 2 the count is ALWAYS written
    ** (0 when the grammar has no (lex-goal ...) form), so the trailing embedded-
    ** grammars section that follows has an unambiguous start -- a v1 image gated
    ** the whole table out, leaving the reader unable to tell lex-goal triples from
    ** a following section. The version bump (and the ABI tag) refuses a v1 image,
    ** so there is no ambiguity to inherit. */
    pp_put_u32(cursor, grammar->lex_goal_count);
    for (i = 0; i < grammar->lex_goal_count; i++) {
        pp_put_u32(cursor, grammar->lex_goals[i].goal_symbol);
        pp_put_u32(cursor, grammar->lex_goals[i].trigger_symbol);
        /* The union word: target_symbol and template_rule_index alias the same
        ** bytes (lex-goal-implementation). is_parametrized selects the meaning. */
        pp_put_u32(cursor, grammar->lex_goals[i].target_symbol);
        pp_put_u32(cursor, grammar->lex_goals[i].is_parametrized);
    }

    /* Embedded-grammars section (W17-1): the distinct grammars named by the
    ** grammar's (inject grammar: NAME ...) forms, each serialized as a full nested
    ** image so the loaded grammar is self-contained -- the compiled-to-C parser
    ** reconstructs its embedded languages with no module resolver. Each entry is
    ** the language_symbol key, the nested image's length, then the image itself;
    ** measuring the nested image with a throwaway cursor gives the length up front
    ** so the reader can skip or bound it. Recursive: an embedded grammar that
    ** itself injects writes its own embedded section in turn. */
    {
        pp_embedded_set set;
        uint32_t        e;
        pp_collect_embedded(grammar, &set);
        pp_put_u32(cursor, set.count);
        for (e = 0; e < set.count; e++) {
            pp_pack_cursor measure;
            pp_put_u32(cursor, set.symbols[e]);
            measure.bytes = 0;
            measure.length = 0;
            measure.limit = 0;
            measure.error = 0;
            pp_write_grammar(&measure, set.grammars[e]);
            pp_put_u32(cursor, measure.length);
            pp_write_grammar(cursor, set.grammars[e]);
        }
    }
}

/* ** Serialize a compiled grammar into a fresh arena buffer. */
pocketparse_result pocketparse_grammar_serialize(
    const pocketparse_grammar *grammar,
    pocketparse_string        *out_bytes)
{
    pp_pack_cursor measure;
    pp_pack_cursor emit;
    uint8_t       *buffer;

    if (out_bytes != 0) {
        out_bytes->pointer = 0;
        out_bytes->length  = 0;
    }
    if (grammar == 0 || out_bytes == 0)
        return POCKETPARSE_INVALID;

    /* Pass one: measure. */
    measure.bytes = 0;
    measure.length = 0;
    measure.limit = 0;
    measure.error = 0;
    pp_write_grammar(&measure, grammar);

    buffer = (uint8_t *)ax_alloc(grammar->arena, measure.length);
    if (buffer == 0)
        return POCKETPARSE_OUT_OF_MEMORY;

    /* Pass two: emit into the sized buffer. */
    emit.bytes = buffer;
    emit.length = 0;
    emit.limit = measure.length;
    emit.error = 0;
    pp_write_grammar(&emit, grammar);

    out_bytes->pointer = buffer;
    out_bytes->length  = emit.length;
    return POCKETPARSE_OK;
}

/* ** Record a fault on the engine and return INVALID, so load failures are one
** line. The message is a C literal with static lifetime. */
static pocketparse_result pp_pack_fail(pocketparse_engine *engine,
                                       const char *message)
{
    if (engine != 0)
        engine->fault = pp_text(message);
    return POCKETPARSE_INVALID;
}

/* ** Rebuild one IR node depth-first from the cursor, allocating from the arena.
** Returns NULL on allocation failure or a malformed image (cursor->error set,
** or a child count so large the read overruns -- caught on the next read). */
static pp_ir *pp_read_ir(ax_arena *arena, pp_pack_cursor *cursor)
{
    pp_ir   *ir;
    uint32_t i;
    uint32_t text_length;

    if (cursor->error)
        return 0;

    ir = (pp_ir *)ax_alloc(arena, sizeof *ir);
    if (ir == 0)
        return 0;

    ir->op = (pp_op)pp_get_u32(cursor);

    text_length = pp_get_u32(cursor);
    if (text_length > cursor->limit - cursor->length) {
        cursor->error = 1;
        return 0;
    }
    if (text_length == 0) {
        ir->text.pointer = (const uint8_t *)"";
        ir->text.length  = 0;
    } else {
        uint8_t *text = (uint8_t *)ax_alloc(arena, text_length);
        if (text == 0)
            return 0;
        for (i = 0; i < text_length; i++)
            text[i] = pp_get_byte(cursor);
        ir->text.pointer = text;
        ir->text.length  = text_length;
    }

    for (i = 0; i < 32; i++)
        ir->cls.bits[i] = pp_get_byte(cursor);
    ir->cls.negated = (int)pp_get_u32(cursor);

    ir->symbol = pp_get_u32(cursor);
    ir->min    = pp_get_u32(cursor);
    ir->max    = pp_get_u32(cursor);

    ir->recover_message = ir->text; /* placeholder; overwritten below */
    ir->recover_message.length = 0;
    ir->recover_help   = ir->recover_message;
    ir->recover_label  = ir->recover_message;
    ir->recover_insert = ir->recover_message;

    ir->child_count = pp_get_u32(cursor);
    if (cursor->error)
        return 0;
    if (ir->child_count == 0) {
        ir->children = 0;
    } else {
        /* A child count larger than the remaining bytes is impossible for a
        ** well-formed image (each child costs many bytes); reject early so a
        ** corrupt count cannot drive a huge allocation. */
        if (ir->child_count > cursor->limit - cursor->length) {
            cursor->error = 1;
            return 0;
        }
        ir->children = (pp_ir **)ax_alloc(
            arena, ir->child_count * sizeof *ir->children);
        if (ir->children == 0)
            return 0;
        for (i = 0; i < ir->child_count; i++) {
            ir->children[i] = pp_read_ir(arena, cursor);
            if (ir->children[i] == 0)
                return 0;
        }
    }
    /* Recovery payload, present only for PP_OP_RECOVER (see pp_write_ir). */
    if (ir->op == PP_OP_RECOVER) {
        if (!pp_read_name(arena, cursor, &ir->recover_message) ||
            !pp_read_name(arena, cursor, &ir->recover_help) ||
            !pp_read_name(arena, cursor, &ir->recover_label) ||
            !pp_read_name(arena, cursor, &ir->recover_insert))
            return 0;
    }
    /* Injection payload, present only for PP_OP_INJECT (see pp_write_ir).
    ** language_symbol names the embedded grammar; embedded_grammar stays 0 here and
    ** is resolved once the whole grammar (and its interner) is loaded, by
    ** pp_load_resolve_injects. */
    if (ir->op == PP_OP_INJECT) {
        ir->language_symbol = pp_get_u32(cursor);
        if (cursor->error)
            return 0;
    }
    return ir;
}

/* ** Read a length-prefixed name straight out of the image into the arena.
** Returns 0 on overrun or allocation failure (caller checks cursor->error). */
static int pp_read_name(ax_arena *arena, pp_pack_cursor *cursor,
                        pocketparse_string *out_name)
{
    uint32_t length = pp_get_u32(cursor);
    uint32_t i;
    uint8_t *name;

    if (cursor->error || length > cursor->limit - cursor->length) {
        cursor->error = 1;
        return 0;
    }
    if (length == 0) {
        out_name->pointer = (const uint8_t *)"";
        out_name->length  = 0;
        return 1;
    }
    name = (uint8_t *)ax_alloc(arena, length);
    if (name == 0)
        return 0;
    for (i = 0; i < length; i++)
        name[i] = pp_get_byte(cursor);
    out_name->pointer = name;
    out_name->length  = length;
    return 1;
}

/* ** Wire each PP_OP_INJECT node's embedded_grammar pointer from the loaded
** embedded-grammars map (symbols[i] -> grammars[i]), matching by language_symbol.
** A node whose language_symbol is absent from the map (an image packed before this
** section existed, or an inject whose grammar never resolved) is left at 0, so the
** executor falls back to the bare injection node exactly as it would have. */
static void pp_load_wire_injects(void *user, const pp_ir *ir)
{
    pp_embedded_set *set = (pp_embedded_set *)user;
    uint32_t i;
    if (ir->op != PP_OP_INJECT)
        return;
    for (i = 0; i < set->count; i++) {
        if (set->symbols[i] == ir->language_symbol) {
            /* The IR node is arena-owned and mutable; the const here is only the
            ** walk's read-only view, so cast it away to install the pointer. */
            ((pp_ir *)ir)->embedded_grammar = set->grammars[i];
            return;
        }
    }
}

/* ** Load a packed grammar image from `cursor' (positioned at the magic) into a
** fully usable grammar in the engine arena. Recursive: the embedded-grammars
** section loads each nested image through this same function, so a grammar that
** injects a language that itself injects a third loads the whole chain. Returns
** the grammar through *out_grammar, or a fault code with engine->fault set. */
static pocketparse_result pp_load_image(
    pocketparse_engine  *engine,
    pp_pack_cursor      *cursorp,
    pocketparse_grammar **out_grammar)
{
    pp_pack_cursor       cursor = *cursorp;
    pocketparse_grammar *grammar;
    uint32_t             count, i;

    if (out_grammar != 0)
        *out_grammar = 0;

    /* Magic + version + ABI tag. */
    if (cursor.limit - cursor.length < 16)
        return pp_pack_fail(engine, "binary grammar too short");
    if (pp_get_byte(&cursor) != PP_PACK_MAGIC0 ||
        pp_get_byte(&cursor) != PP_PACK_MAGIC1 ||
        pp_get_byte(&cursor) != PP_PACK_MAGIC2 ||
        pp_get_byte(&cursor) != PP_PACK_MAGIC3)
        return pp_pack_fail(engine, "not a packed grammar (bad magic)");
    if (pp_get_u32(&cursor) != PP_PACK_VERSION)
        return pp_pack_fail(engine, "packed grammar version mismatch");
    if (pp_get_u32(&cursor) != PP_PACK_ABI)
        return pp_pack_fail(engine, "packed grammar ABI mismatch");

    grammar = (pocketparse_grammar *)ax_alloc(engine->arena, sizeof *grammar);
    if (grammar == 0)
        return pp_pack_fail(engine, "out of memory loading grammar");
    grammar->arena             = engine->arena;
    grammar->interner.arena    = engine->arena;
    grammar->interner.names    = 0;
    grammar->interner.count    = 0;
    grammar->interner.capacity = 0;
    grammar->tokens            = 0;
    grammar->token_count       = 0;
    grammar->rules             = 0;
    grammar->rule_count        = 0;
    grammar->extras            = 0;
    grammar->extra_count       = 0;
    grammar->lex_goals         = 0;
    grammar->lex_goal_count    = 0;
    grammar->param_goal_count  = 0;
    grammar->start_symbol      = PP_NO_SYMBOL;
    grammar->vm_program        = 0;
    grammar->vm_program_start  = PP_NO_SYMBOL;
    grammar->token_index       = 0;
    grammar->token_index_size  = 0;
    grammar->hidden_index       = 0;
    grammar->hidden_index_size  = 0;
    grammar->hidden_index_built = 0;

    /* Interned string table. Names are copied verbatim so ids stay dense and
    ** identical to the packing side; this preserves every symbol id intact. */
    count = pp_get_u32(&cursor);
    if (cursor.error)
        return pp_pack_fail(engine, "truncated packed grammar (names)");
    if (count > cursor.limit)
        return pp_pack_fail(engine, "corrupt packed grammar (name count)");
    if (count > 0) {
        grammar->interner.names = (pocketparse_string *)ax_alloc(
            engine->arena, count * sizeof *grammar->interner.names);
        if (grammar->interner.names == 0)
            return pp_pack_fail(engine, "out of memory loading grammar");
        for (i = 0; i < count; i++) {
            if (!pp_read_name(engine->arena, &cursor,
                              &grammar->interner.names[i]))
                return pp_pack_fail(engine,
                                    "truncated packed grammar (name)");
        }
    }
    grammar->interner.count    = count;
    grammar->interner.capacity = count;

    /* Token table. */
    count = pp_get_u32(&cursor);
    if (cursor.error || count > cursor.limit)
        return pp_pack_fail(engine, "corrupt packed grammar (tokens)");
    grammar->token_count = count;
    grammar->tokens = (pp_token_def *)ax_alloc(
        engine->arena, (count == 0 ? 1 : count) * sizeof *grammar->tokens);
    if (grammar->tokens == 0)
        return pp_pack_fail(engine, "out of memory loading grammar");
    for (i = 0; i < count; i++) {
        grammar->tokens[i].symbol = pp_get_u32(&cursor);
        grammar->tokens[i].pattern = pp_read_ir(engine->arena, &cursor);
        if (grammar->tokens[i].pattern == 0 || cursor.error)
            return pp_pack_fail(engine, "corrupt packed grammar (token IR)");
    }

    /* Rule table. */
    count = pp_get_u32(&cursor);
    if (cursor.error || count > cursor.limit)
        return pp_pack_fail(engine, "corrupt packed grammar (rules)");
    grammar->rule_count = count;
    grammar->rules = (pp_rule_def *)ax_alloc(
        engine->arena, (count == 0 ? 1 : count) * sizeof *grammar->rules);
    if (grammar->rules == 0)
        return pp_pack_fail(engine, "out of memory loading grammar");
    for (i = 0; i < count; i++) {
        grammar->rules[i].symbol = pp_get_u32(&cursor);
        grammar->rules[i].flags  = pp_get_u32(&cursor); /* W17-FLAT */
        /* Per-rule deferred-slot count (lex-goal-implementation), mirrors the
        ** write order: read between flags and body. */
        grammar->rules[i].slot_count = pp_get_u32(&cursor);
        grammar->rules[i].body = pp_read_ir(engine->arena, &cursor);
        if (grammar->rules[i].body == 0 || cursor.error)
            return pp_pack_fail(engine, "corrupt packed grammar (rule IR)");
    }

    /* Extras table. */
    count = pp_get_u32(&cursor);
    if (cursor.error || count > cursor.limit)
        return pp_pack_fail(engine, "corrupt packed grammar (extras)");
    grammar->extra_count = count;
    grammar->extras = (pp_extra *)ax_alloc(
        engine->arena, (count == 0 ? 1 : count) * sizeof *grammar->extras);
    if (grammar->extras == 0)
        return pp_pack_fail(engine, "out of memory loading grammar");
    for (i = 0; i < count; i++) {
        grammar->extras[i].token_symbol = pp_get_u32(&cursor);
        grammar->extras[i].policy = (pp_extra_policy)pp_get_u32(&cursor);
        grammar->extras[i].attach = (pp_extra_attach)pp_get_u32(&cursor);
    }

    grammar->start_symbol = pp_get_u32(&cursor);
    if (cursor.error)
        return pp_pack_fail(engine, "truncated packed grammar (start)");

    /* Lexical-goal table. PP_PACK_VERSION 2 always writes the count (the version
    ** check above refused a v1 image, which gated the table out), so the embedded
    ** section that follows has an unambiguous start. */
    count = pp_get_u32(&cursor);
    if (cursor.error || count > cursor.limit)
        return pp_pack_fail(engine, "corrupt packed grammar (lex-goals)");
    grammar->lex_goal_count = count;
    grammar->lex_goals = (pp_lex_goal *)ax_alloc(
        engine->arena, (count == 0 ? 1 : count) * sizeof *grammar->lex_goals);
    if (grammar->lex_goals == 0)
        return pp_pack_fail(engine, "out of memory loading grammar");
    for (i = 0; i < count; i++) {
        grammar->lex_goals[i].goal_symbol    = pp_get_u32(&cursor);
        grammar->lex_goals[i].trigger_symbol = pp_get_u32(&cursor);
        /* The union word aliases target_symbol/template_rule_index; is_parametrized
        ** follows and selects the meaning (lex-goal-implementation). */
        grammar->lex_goals[i].target_symbol  = pp_get_u32(&cursor);
        grammar->lex_goals[i].is_parametrized = pp_get_u32(&cursor);
    }
    if (cursor.error)
        return pp_pack_fail(engine, "truncated packed grammar (lex-goals)");

    /* Derive param_goal_count from the loaded table rather than serialising it: a
    ** parametrized entry is one with is_parametrized != 0 (lex-goal-implementation). */
    grammar->param_goal_count = 0;
    for (i = 0; i < grammar->lex_goal_count; i++)
        if (grammar->lex_goals[i].is_parametrized != 0)
            grammar->param_goal_count++;

    /* Embedded-grammars section (W17-1). Read each nested image through this same
    ** loader (recursion bounded by injection nesting), collecting language_symbol
    ** -> grammar, then wire every inject node's embedded_grammar pointer from that
    ** map. After this the grammar is self-contained: its embedded languages are
    ** loaded and pointed-to, so the compiled-to-C parser nests with no resolver. */
    count = pp_get_u32(&cursor);
    if (cursor.error || count > cursor.limit)
        return pp_pack_fail(engine, "corrupt packed grammar (embedded)");
    if (count > PP_PACK_MAX_EMBEDDED)
        return pp_pack_fail(engine, "too many embedded grammars");
    {
        pp_embedded_set set;
        set.count    = 0;
        set.overflow = 0;
        for (i = 0; i < count; i++) {
            uint32_t            symbol = pp_get_u32(&cursor);
            uint32_t            image_length = pp_get_u32(&cursor);
            pp_pack_cursor      sub;
            pocketparse_grammar *embedded = 0;
            pocketparse_result   loaded;
            if (cursor.error ||
                image_length > cursor.limit - cursor.length)
                return pp_pack_fail(engine,
                                    "corrupt packed grammar (embedded image)");
            sub.bytes  = cursor.bytes;
            sub.length = cursor.length;
            sub.limit  = cursor.length + image_length;
            sub.error  = 0;
            loaded = pp_load_image(engine, &sub, &embedded);
            if (loaded != POCKETPARSE_OK)
                return loaded; /* engine->fault already set by the nested load */
            cursor.length += image_length;
            set.grammars[set.count] = embedded;
            set.symbols[set.count]  = symbol;
            set.count++;
        }
        for (i = 0; i < grammar->token_count; i++)
            pp_ir_walk(grammar->tokens[i].pattern, pp_load_wire_injects, &set);
        for (i = 0; i < grammar->rule_count; i++)
            pp_ir_walk(grammar->rules[i].body, pp_load_wire_injects, &set);
    }

    engine->fault = pp_text("");
    *cursorp = cursor;
    if (out_grammar != 0)
        *out_grammar = grammar;
    return POCKETPARSE_OK;
}

/* ** Load a packed binary grammar image into a fully usable grammar. The image
** is copied into the engine arena as it is read, so the reconstructed grammar
** owns all its bytes and does not point back into the host's source buffer. The
** real work is pp_load_image, which is recursive over the embedded-grammars
** section; this entry just frames the cursor over the whole image. */
pocketparse_result pocketparse_load_binary_grammar(
    pocketparse_engine  *engine,
    pocketparse_source   source,
    pocketparse_grammar **out_grammar)
{
    pp_pack_cursor cursor;

    if (out_grammar != 0)
        *out_grammar = 0;
    if (engine == 0)
        return POCKETPARSE_INVALID;

    cursor.bytes  = (uint8_t *)source.bytes.pointer;
    cursor.length = 0;
    cursor.limit  = source.bytes.length;
    cursor.error  = 0;
    return pp_load_image(engine, &cursor, out_grammar);
}
