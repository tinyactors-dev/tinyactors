/*
** pack.c -- the binary grammar format: serialize a compiled grammar to a
** self-describing `.tpg` image and load one back into a usable grammar.
**
** The two functions here are mirror images. tinyparse_grammar_serialize walks
** a compiled grammar and writes a little-endian blob; tinyparse_load_binary_
** grammar reads that blob and rebuilds an equivalent tinyparse_grammar. A
** round trip is lossless: a grammar loaded from the bytes parses byte-for-byte
** identically to the .scm it was packed from.
**
** Freestanding, like the rest of src/ except cli.c: no libc, no IO. The host
** supplies the bytes (load) and is handed the bytes (serialize); every
** allocation comes from the engine arena via tp_alloc. The serializer runs in
** two passes -- measure, then emit -- so it needs a single arena block of the
** exact size rather than a growable buffer (arenas never realloc).
**
** Image layout (all integers little-endian, 32-bit unless noted):
**
**   "TPG1"                     magic (4 bytes)
**   version                    TP_PACK_VERSION
**   abi_tag                    TP_PACK_ABI, guards against struct/enum drift
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
** The four recover_* slices are present only when op == TP_OP_RECOVER, so an
** ordinary IR node serializes exactly as before and old-shaped images still
** describe the same grammar. A TP_OP_INJECT node adds one field after its
** children:
**
**   [ language_symbol ]?            (op == TP_OP_INJECT only)
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
** Each IMAGE is a full nested .tpg (magic, version, abi, the lot), so an embedded
** grammar that itself injects a third language round-trips recursively. On load,
** the section is decoded into a map language_symbol -> grammar and every inject
** node's embedded_grammar pointer is wired from it. This is what makes the
** generated (compiled-to-C) parser preserve language embedding: it loads the
** packed grammar with no resolver, yet the embedded grammar is right there.
**
** A TP_OP_CLASS_NAME node (a (class NAME) token primitive) carries its class
** name -- the host CLASS binding it resolves against -- in the IR's text slice,
** the very field every op already writes. So a named class round-trips with no
** new on-disk field, gated on the op only in the sense that the name simply lives
** where the op puts it; the format is unchanged and backward compatible.
*/
#include "internal.h"

/* ** Format constants. The magic identifies a .tpg image at a glance; the
** version tracks layout revisions; the ABI tag mixes the sizes of the structures
** the image mirrors so a load against a mismatched build is refused rather than
** silently misread. */
#define TP_PACK_MAGIC0 'T'
#define TP_PACK_MAGIC1 'P'
#define TP_PACK_MAGIC2 'G'
#define TP_PACK_MAGIC3 '1'
#define TP_PACK_VERSION 4u
#define TP_PACK_ABI \
    ((uint32_t)(sizeof(tp_ir) + sizeof(tp_class) + sizeof(tp_token_def) + \
                sizeof(tp_rule_def) + sizeof(tp_extra) + sizeof(tp_lex_goal) + \
                (uint32_t)TP_OP_EOF * 131u + (uint32_t)TP_EXTRA_ERROR * 17u + \
                (uint32_t)TP_EXTRA_ATTACH_LEADING * 19u))

/* ** A cursor over a byte image. On write, `bytes` is either NULL (measuring
** pass -- only `length` advances) or a real buffer being filled. On read,
** `bytes`/`limit` bound the input and `error` flips to nonzero the moment a
** read would run past the end, so callers can test it once at the end. */
typedef struct tp_pack_cursor {
    uint8_t *bytes;
    uint32_t length;   /* bytes written/read so far */
    uint32_t limit;    /* total readable bytes (read side) */
    int      error;    /* set on overrun (read side) */
} tp_pack_cursor;

/* ** Write one byte, or just count it during the measuring pass. */
static void tp_put_byte(tp_pack_cursor *cursor, uint8_t value)
{
    if (cursor->bytes != 0)
        cursor->bytes[cursor->length] = value;
    cursor->length++;
}

/* ** Write a 32-bit value little-endian. */
static void tp_put_u32(tp_pack_cursor *cursor, uint32_t value)
{
    tp_put_byte(cursor, (uint8_t)(value & 0xFFu));
    tp_put_byte(cursor, (uint8_t)((value >> 8) & 0xFFu));
    tp_put_byte(cursor, (uint8_t)((value >> 16) & 0xFFu));
    tp_put_byte(cursor, (uint8_t)((value >> 24) & 0xFFu));
}

/* ** Write a length-prefixed byte slice. */
static void tp_put_bytes(tp_pack_cursor *cursor, tinyparse_string slice)
{
    uint32_t i;
    tp_put_u32(cursor, slice.length);
    for (i = 0; i < slice.length; i++)
        tp_put_byte(cursor, slice.pointer[i]);
}

/* ** Read one byte, flagging overrun. Returns 0 past the end. */
static uint8_t tp_get_byte(tp_pack_cursor *cursor)
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
static uint32_t tp_get_u32(tp_pack_cursor *cursor)
{
    uint32_t b0 = tp_get_byte(cursor);
    uint32_t b1 = tp_get_byte(cursor);
    uint32_t b2 = tp_get_byte(cursor);
    uint32_t b3 = tp_get_byte(cursor);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

/* ** Read a length-prefixed name into the arena; defined below, declared here so
** tp_read_ir can read a recover node's payload before the definition appears. */
static int tp_read_name(tp_arena *arena, tp_pack_cursor *cursor,
                        tinyparse_string *out_name);

/* ** Serialize one IR node depth-first into the cursor. */
static void tp_write_ir(tp_pack_cursor *cursor, const tp_ir *ir)
{
    uint32_t i;
    tp_put_u32(cursor, (uint32_t)ir->op);
    tp_put_bytes(cursor, ir->text);
    for (i = 0; i < 32; i++)
        tp_put_byte(cursor, ir->cls.bits[i]);
    tp_put_u32(cursor, (uint32_t)ir->cls.negated);
    tp_put_u32(cursor, ir->symbol);
    tp_put_u32(cursor, ir->min);
    tp_put_u32(cursor, ir->max);
    tp_put_u32(cursor, ir->child_count);
    for (i = 0; i < ir->child_count; i++)
        tp_write_ir(cursor, ir->children[i]);
    /* The recovery payload travels only with a TP_OP_RECOVER node; every other
    ** op leaves these fields unset (token.c mints IR without them), so reading
    ** them would be undefined -- write them solely where they are meaningful. */
    if (ir->op == TP_OP_RECOVER) {
        tp_put_bytes(cursor, ir->recover_message);
        tp_put_bytes(cursor, ir->recover_help);
        tp_put_bytes(cursor, ir->recover_label);
        tp_put_bytes(cursor, ir->recover_insert);
    }
    /* The injection payload travels only with a TP_OP_INJECT node: language_symbol,
    ** the embedded grammar's name interned into this grammar's interner. The
    ** embedded grammar itself rides in the trailing embedded-grammars section (see
    ** tp_write_grammar); language_symbol is the key that section is read back by.
    ** Without this an inject node loads with embedded_grammar == 0 and the executor
    ** emits a bare injection node -- the language embedding silently dropped. */
    if (ir->op == TP_OP_INJECT)
        tp_put_u32(cursor, ir->language_symbol);
}

/* ** Visit every IR node reachable from `ir' depth-first, calling `visit(user,
** node)' on each. Used to find the TP_OP_INJECT nodes a grammar carries so their
** distinct embedded grammars can be serialized. */
static void tp_ir_walk(const tp_ir *ir,
                       void (*visit)(void *user, const tp_ir *ir),
                       void *user)
{
    uint32_t i;
    if (ir == 0)
        return;
    visit(user, ir);
    for (i = 0; i < ir->child_count; i++)
        tp_ir_walk(ir->children[i], visit, user);
}

/* ** A small accumulator of the distinct embedded grammars an inject-using grammar
** references. Two inject nodes that name the same language share one grammar
** pointer (resolved once at compile time), so dedup is a pointer-identity check;
** at most TP_PACK_MAX_EMBEDDED distinct languages are collected, which is ample
** for real grammars and bounds the work. */
enum { TP_PACK_MAX_EMBEDDED = 64 };
typedef struct tp_embedded_set {
    tinyparse_grammar *grammars[TP_PACK_MAX_EMBEDDED];
    uint32_t             symbols[TP_PACK_MAX_EMBEDDED];
    uint32_t             count;
    int                  overflow;
} tp_embedded_set;

/* ** Record one inject node's embedded grammar in the set, deduplicating by
** grammar pointer. A node with no resolved embedded grammar (embedded_grammar ==
** 0) is skipped -- it serializes its language_symbol but contributes no image, so
** the loader leaves it unresolved exactly as it was. */
static void tp_embedded_collect(void *user, const tp_ir *ir)
{
    tp_embedded_set *set = (tp_embedded_set *)user;
    uint32_t i;
    if (ir->op != TP_OP_INJECT || ir->embedded_grammar == 0)
        return;
    for (i = 0; i < set->count; i++)
        if (set->grammars[i] == ir->embedded_grammar)
            return;
    if (set->count >= TP_PACK_MAX_EMBEDDED) {
        set->overflow = 1;
        return;
    }
    set->grammars[set->count] = ir->embedded_grammar;
    set->symbols[set->count]  = ir->language_symbol;
    set->count++;
}

/* ** Gather every distinct embedded grammar reachable from `grammar's token and
** rule IR into `set'. */
static void tp_collect_embedded(const tinyparse_grammar *grammar,
                                tp_embedded_set *set)
{
    uint32_t i;
    set->count    = 0;
    set->overflow = 0;
    for (i = 0; i < grammar->token_count; i++)
        tp_ir_walk(grammar->tokens[i].pattern, tp_embedded_collect, set);
    for (i = 0; i < grammar->rule_count; i++)
        tp_ir_walk(grammar->rules[i].body, tp_embedded_collect, set);
}

/* ** Serialize the whole grammar into the cursor. Run once with cursor->bytes
** NULL to size the image, then again over a real buffer to fill it; the two
** passes walk identical paths so the lengths match exactly. */
static void tp_write_grammar(tp_pack_cursor *cursor,
                             const tinyparse_grammar *grammar)
{
    uint32_t i;

    tp_put_byte(cursor, TP_PACK_MAGIC0);
    tp_put_byte(cursor, TP_PACK_MAGIC1);
    tp_put_byte(cursor, TP_PACK_MAGIC2);
    tp_put_byte(cursor, TP_PACK_MAGIC3);
    tp_put_u32(cursor, TP_PACK_VERSION);
    tp_put_u32(cursor, TP_PACK_ABI);

    tp_put_u32(cursor, grammar->interner.count);
    for (i = 0; i < grammar->interner.count; i++)
        tp_put_bytes(cursor, grammar->interner.names[i]);

    tp_put_u32(cursor, grammar->token_count);
    for (i = 0; i < grammar->token_count; i++) {
        tp_put_u32(cursor, grammar->tokens[i].symbol);
        tp_write_ir(cursor, grammar->tokens[i].pattern);
    }

    tp_put_u32(cursor, grammar->rule_count);
    for (i = 0; i < grammar->rule_count; i++) {
        tp_put_u32(cursor, grammar->rules[i].symbol);
        /* W17-FLAT: the per-rule flags (TP_RULE_HIDDEN) ride alongside the body so
        ** a packed grammar collapses identically to the .scm it was built from. */
        tp_put_u32(cursor, grammar->rules[i].flags);
        /* Per-rule deferred-slot count (lex-goal-implementation), written between
        ** flags and body so the read loop mirrors this order. */
        tp_put_u32(cursor, grammar->rules[i].slot_count);
        tp_write_ir(cursor, grammar->rules[i].body);
    }

    tp_put_u32(cursor, grammar->extra_count);
    for (i = 0; i < grammar->extra_count; i++) {
        tp_put_u32(cursor, grammar->extras[i].token_symbol);
        tp_put_u32(cursor, (uint32_t)grammar->extras[i].policy);
        tp_put_u32(cursor, (uint32_t)grammar->extras[i].attach);
    }

    tp_put_u32(cursor, grammar->start_symbol);

    /* Lexical-goal table. As of TP_PACK_VERSION 2 the count is ALWAYS written
    ** (0 when the grammar has no (lex-goal ...) form), so the trailing embedded-
    ** grammars section that follows has an unambiguous start -- a v1 image gated
    ** the whole table out, leaving the reader unable to tell lex-goal triples from
    ** a following section. The version bump (and the ABI tag) refuses a v1 image,
    ** so there is no ambiguity to inherit. */
    tp_put_u32(cursor, grammar->lex_goal_count);
    for (i = 0; i < grammar->lex_goal_count; i++) {
        tp_put_u32(cursor, grammar->lex_goals[i].goal_symbol);
        tp_put_u32(cursor, grammar->lex_goals[i].trigger_symbol);
        /* The union word: target_symbol and template_rule_index alias the same
        ** bytes (lex-goal-implementation). is_parametrized selects the meaning. */
        tp_put_u32(cursor, grammar->lex_goals[i].target_symbol);
        tp_put_u32(cursor, grammar->lex_goals[i].is_parametrized);
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
        tp_embedded_set set;
        uint32_t        e;
        tp_collect_embedded(grammar, &set);
        tp_put_u32(cursor, set.count);
        for (e = 0; e < set.count; e++) {
            tp_pack_cursor measure;
            tp_put_u32(cursor, set.symbols[e]);
            measure.bytes = 0;
            measure.length = 0;
            measure.limit = 0;
            measure.error = 0;
            tp_write_grammar(&measure, set.grammars[e]);
            tp_put_u32(cursor, measure.length);
            tp_write_grammar(cursor, set.grammars[e]);
        }
    }
}

/* ** Serialize a compiled grammar into a fresh arena buffer. */
tinyparse_result tinyparse_grammar_serialize(
    const tinyparse_grammar *grammar,
    tinyparse_string        *out_bytes)
{
    tp_pack_cursor measure;
    tp_pack_cursor emit;
    uint8_t       *buffer;

    if (out_bytes != 0) {
        out_bytes->pointer = 0;
        out_bytes->length  = 0;
    }
    if (grammar == 0 || out_bytes == 0)
        return TINYPARSE_INVALID;

    /* Pass one: measure. */
    measure.bytes = 0;
    measure.length = 0;
    measure.limit = 0;
    measure.error = 0;
    tp_write_grammar(&measure, grammar);

    buffer = (uint8_t *)tp_alloc(grammar->arena, measure.length);
    if (buffer == 0)
        return TINYPARSE_OUT_OF_MEMORY;

    /* Pass two: emit into the sized buffer. */
    emit.bytes = buffer;
    emit.length = 0;
    emit.limit = measure.length;
    emit.error = 0;
    tp_write_grammar(&emit, grammar);

    out_bytes->pointer = buffer;
    out_bytes->length  = emit.length;
    return TINYPARSE_OK;
}

/* ** Record a fault on the engine and return INVALID, so load failures are one
** line. The message is a C literal with static lifetime. */
static tinyparse_result tp_pack_fail(tinyparse_engine *engine,
                                       const char *message)
{
    if (engine != 0)
        engine->fault = tp_text(message);
    return TINYPARSE_INVALID;
}

/* ** Rebuild one IR node depth-first from the cursor, allocating from the arena.
** Returns NULL on allocation failure or a malformed image (cursor->error set,
** or a child count so large the read overruns -- caught on the next read). */
static tp_ir *tp_read_ir(tp_arena *arena, tp_pack_cursor *cursor)
{
    tp_ir   *ir;
    uint32_t i;
    uint32_t text_length;

    if (cursor->error)
        return 0;

    ir = (tp_ir *)tp_alloc(arena, sizeof *ir);
    if (ir == 0)
        return 0;

    ir->op = (tp_op)tp_get_u32(cursor);

    text_length = tp_get_u32(cursor);
    if (text_length > cursor->limit - cursor->length) {
        cursor->error = 1;
        return 0;
    }
    if (text_length == 0) {
        ir->text.pointer = (const uint8_t *)"";
        ir->text.length  = 0;
    } else {
        uint8_t *text = (uint8_t *)tp_alloc(arena, text_length);
        if (text == 0)
            return 0;
        for (i = 0; i < text_length; i++)
            text[i] = tp_get_byte(cursor);
        ir->text.pointer = text;
        ir->text.length  = text_length;
    }

    for (i = 0; i < 32; i++)
        ir->cls.bits[i] = tp_get_byte(cursor);
    ir->cls.negated = (int)tp_get_u32(cursor);

    ir->symbol = tp_get_u32(cursor);
    ir->min    = tp_get_u32(cursor);
    ir->max    = tp_get_u32(cursor);

    ir->recover_message = ir->text; /* placeholder; overwritten below */
    ir->recover_message.length = 0;
    ir->recover_help   = ir->recover_message;
    ir->recover_label  = ir->recover_message;
    ir->recover_insert = ir->recover_message;

    ir->child_count = tp_get_u32(cursor);
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
        ir->children = (tp_ir **)tp_alloc(
            arena, ir->child_count * sizeof *ir->children);
        if (ir->children == 0)
            return 0;
        for (i = 0; i < ir->child_count; i++) {
            ir->children[i] = tp_read_ir(arena, cursor);
            if (ir->children[i] == 0)
                return 0;
        }
    }
    /* Recovery payload, present only for TP_OP_RECOVER (see tp_write_ir). */
    if (ir->op == TP_OP_RECOVER) {
        if (!tp_read_name(arena, cursor, &ir->recover_message) ||
            !tp_read_name(arena, cursor, &ir->recover_help) ||
            !tp_read_name(arena, cursor, &ir->recover_label) ||
            !tp_read_name(arena, cursor, &ir->recover_insert))
            return 0;
    }
    /* Injection payload, present only for TP_OP_INJECT (see tp_write_ir).
    ** language_symbol names the embedded grammar; embedded_grammar stays 0 here and
    ** is resolved once the whole grammar (and its interner) is loaded, by
    ** tp_load_resolve_injects. */
    if (ir->op == TP_OP_INJECT) {
        ir->language_symbol = tp_get_u32(cursor);
        if (cursor->error)
            return 0;
    }
    return ir;
}

/* ** Read a length-prefixed name straight out of the image into the arena.
** Returns 0 on overrun or allocation failure (caller checks cursor->error). */
static int tp_read_name(tp_arena *arena, tp_pack_cursor *cursor,
                        tinyparse_string *out_name)
{
    uint32_t length = tp_get_u32(cursor);
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
    name = (uint8_t *)tp_alloc(arena, length);
    if (name == 0)
        return 0;
    for (i = 0; i < length; i++)
        name[i] = tp_get_byte(cursor);
    out_name->pointer = name;
    out_name->length  = length;
    return 1;
}

/* ** Wire each TP_OP_INJECT node's embedded_grammar pointer from the loaded
** embedded-grammars map (symbols[i] -> grammars[i]), matching by language_symbol.
** A node whose language_symbol is absent from the map (an image packed before this
** section existed, or an inject whose grammar never resolved) is left at 0, so the
** executor falls back to the bare injection node exactly as it would have. */
static void tp_load_wire_injects(void *user, const tp_ir *ir)
{
    tp_embedded_set *set = (tp_embedded_set *)user;
    uint32_t i;
    if (ir->op != TP_OP_INJECT)
        return;
    for (i = 0; i < set->count; i++) {
        if (set->symbols[i] == ir->language_symbol) {
            /* The IR node is arena-owned and mutable; the const here is only the
            ** walk's read-only view, so cast it away to install the pointer. */
            ((tp_ir *)ir)->embedded_grammar = set->grammars[i];
            return;
        }
    }
}

/* ** Load a packed grammar image from `cursor' (positioned at the magic) into a
** fully usable grammar in the engine arena. Recursive: the embedded-grammars
** section loads each nested image through this same function, so a grammar that
** injects a language that itself injects a third loads the whole chain. Returns
** the grammar through *out_grammar, or a fault code with engine->fault set. */
static tinyparse_result tp_load_image(
    tinyparse_engine  *engine,
    tp_pack_cursor      *cursorp,
    tinyparse_grammar **out_grammar)
{
    tp_pack_cursor       cursor = *cursorp;
    tinyparse_grammar *grammar;
    uint32_t             count, i;

    if (out_grammar != 0)
        *out_grammar = 0;

    /* Magic + version + ABI tag. */
    if (cursor.limit - cursor.length < 16)
        return tp_pack_fail(engine, "binary grammar too short");
    if (tp_get_byte(&cursor) != TP_PACK_MAGIC0 ||
        tp_get_byte(&cursor) != TP_PACK_MAGIC1 ||
        tp_get_byte(&cursor) != TP_PACK_MAGIC2 ||
        tp_get_byte(&cursor) != TP_PACK_MAGIC3)
        return tp_pack_fail(engine, "not a packed grammar (bad magic)");
    if (tp_get_u32(&cursor) != TP_PACK_VERSION)
        return tp_pack_fail(engine, "packed grammar version mismatch");
    if (tp_get_u32(&cursor) != TP_PACK_ABI)
        return tp_pack_fail(engine, "packed grammar ABI mismatch");

    grammar = (tinyparse_grammar *)tp_alloc(engine->arena, sizeof *grammar);
    if (grammar == 0)
        return tp_pack_fail(engine, "out of memory loading grammar");
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
    grammar->start_symbol      = TP_NO_SYMBOL;
    grammar->vm_program        = 0;
    grammar->vm_program_start  = TP_NO_SYMBOL;
    grammar->token_index       = 0;
    grammar->token_index_size  = 0;
    grammar->hidden_index       = 0;
    grammar->hidden_index_size  = 0;
    grammar->hidden_index_built = 0;

    /* Interned string table. Names are copied verbatim so ids stay dense and
    ** identical to the packing side; this preserves every symbol id intact. */
    count = tp_get_u32(&cursor);
    if (cursor.error)
        return tp_pack_fail(engine, "truncated packed grammar (names)");
    if (count > cursor.limit)
        return tp_pack_fail(engine, "corrupt packed grammar (name count)");
    if (count > 0) {
        grammar->interner.names = (tinyparse_string *)tp_alloc(
            engine->arena, count * sizeof *grammar->interner.names);
        if (grammar->interner.names == 0)
            return tp_pack_fail(engine, "out of memory loading grammar");
        for (i = 0; i < count; i++) {
            if (!tp_read_name(engine->arena, &cursor,
                              &grammar->interner.names[i]))
                return tp_pack_fail(engine,
                                    "truncated packed grammar (name)");
        }
    }
    grammar->interner.count    = count;
    grammar->interner.capacity = count;

    /* Token table. */
    count = tp_get_u32(&cursor);
    if (cursor.error || count > cursor.limit)
        return tp_pack_fail(engine, "corrupt packed grammar (tokens)");
    grammar->token_count = count;
    grammar->tokens = (tp_token_def *)tp_alloc(
        engine->arena, (count == 0 ? 1 : count) * sizeof *grammar->tokens);
    if (grammar->tokens == 0)
        return tp_pack_fail(engine, "out of memory loading grammar");
    for (i = 0; i < count; i++) {
        grammar->tokens[i].symbol = tp_get_u32(&cursor);
        grammar->tokens[i].pattern = tp_read_ir(engine->arena, &cursor);
        if (grammar->tokens[i].pattern == 0 || cursor.error)
            return tp_pack_fail(engine, "corrupt packed grammar (token IR)");
    }

    /* Rule table. */
    count = tp_get_u32(&cursor);
    if (cursor.error || count > cursor.limit)
        return tp_pack_fail(engine, "corrupt packed grammar (rules)");
    grammar->rule_count = count;
    grammar->rules = (tp_rule_def *)tp_alloc(
        engine->arena, (count == 0 ? 1 : count) * sizeof *grammar->rules);
    if (grammar->rules == 0)
        return tp_pack_fail(engine, "out of memory loading grammar");
    for (i = 0; i < count; i++) {
        grammar->rules[i].symbol = tp_get_u32(&cursor);
        grammar->rules[i].flags  = tp_get_u32(&cursor); /* W17-FLAT */
        /* Per-rule deferred-slot count (lex-goal-implementation), mirrors the
        ** write order: read between flags and body. */
        grammar->rules[i].slot_count = tp_get_u32(&cursor);
        grammar->rules[i].body = tp_read_ir(engine->arena, &cursor);
        if (grammar->rules[i].body == 0 || cursor.error)
            return tp_pack_fail(engine, "corrupt packed grammar (rule IR)");
    }

    /* Extras table. */
    count = tp_get_u32(&cursor);
    if (cursor.error || count > cursor.limit)
        return tp_pack_fail(engine, "corrupt packed grammar (extras)");
    grammar->extra_count = count;
    grammar->extras = (tp_extra *)tp_alloc(
        engine->arena, (count == 0 ? 1 : count) * sizeof *grammar->extras);
    if (grammar->extras == 0)
        return tp_pack_fail(engine, "out of memory loading grammar");
    for (i = 0; i < count; i++) {
        grammar->extras[i].token_symbol = tp_get_u32(&cursor);
        grammar->extras[i].policy = (tp_extra_policy)tp_get_u32(&cursor);
        grammar->extras[i].attach = (tp_extra_attach)tp_get_u32(&cursor);
    }

    grammar->start_symbol = tp_get_u32(&cursor);
    if (cursor.error)
        return tp_pack_fail(engine, "truncated packed grammar (start)");

    /* Lexical-goal table. TP_PACK_VERSION 2 always writes the count (the version
    ** check above refused a v1 image, which gated the table out), so the embedded
    ** section that follows has an unambiguous start. */
    count = tp_get_u32(&cursor);
    if (cursor.error || count > cursor.limit)
        return tp_pack_fail(engine, "corrupt packed grammar (lex-goals)");
    grammar->lex_goal_count = count;
    grammar->lex_goals = (tp_lex_goal *)tp_alloc(
        engine->arena, (count == 0 ? 1 : count) * sizeof *grammar->lex_goals);
    if (grammar->lex_goals == 0)
        return tp_pack_fail(engine, "out of memory loading grammar");
    for (i = 0; i < count; i++) {
        grammar->lex_goals[i].goal_symbol    = tp_get_u32(&cursor);
        grammar->lex_goals[i].trigger_symbol = tp_get_u32(&cursor);
        /* The union word aliases target_symbol/template_rule_index; is_parametrized
        ** follows and selects the meaning (lex-goal-implementation). */
        grammar->lex_goals[i].target_symbol  = tp_get_u32(&cursor);
        grammar->lex_goals[i].is_parametrized = tp_get_u32(&cursor);
    }
    if (cursor.error)
        return tp_pack_fail(engine, "truncated packed grammar (lex-goals)");

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
    count = tp_get_u32(&cursor);
    if (cursor.error || count > cursor.limit)
        return tp_pack_fail(engine, "corrupt packed grammar (embedded)");
    if (count > TP_PACK_MAX_EMBEDDED)
        return tp_pack_fail(engine, "too many embedded grammars");
    {
        tp_embedded_set set;
        set.count    = 0;
        set.overflow = 0;
        for (i = 0; i < count; i++) {
            uint32_t            symbol = tp_get_u32(&cursor);
            uint32_t            image_length = tp_get_u32(&cursor);
            tp_pack_cursor      sub;
            tinyparse_grammar *embedded = 0;
            tinyparse_result   loaded;
            if (cursor.error ||
                image_length > cursor.limit - cursor.length)
                return tp_pack_fail(engine,
                                    "corrupt packed grammar (embedded image)");
            sub.bytes  = cursor.bytes;
            sub.length = cursor.length;
            sub.limit  = cursor.length + image_length;
            sub.error  = 0;
            loaded = tp_load_image(engine, &sub, &embedded);
            if (loaded != TINYPARSE_OK)
                return loaded; /* engine->fault already set by the nested load */
            cursor.length += image_length;
            set.grammars[set.count] = embedded;
            set.symbols[set.count]  = symbol;
            set.count++;
        }
        for (i = 0; i < grammar->token_count; i++)
            tp_ir_walk(grammar->tokens[i].pattern, tp_load_wire_injects, &set);
        for (i = 0; i < grammar->rule_count; i++)
            tp_ir_walk(grammar->rules[i].body, tp_load_wire_injects, &set);
    }

    engine->fault = tp_text("");
    *cursorp = cursor;
    if (out_grammar != 0)
        *out_grammar = grammar;
    return TINYPARSE_OK;
}

/* ** Load a packed binary grammar image into a fully usable grammar. The image
** is copied into the engine arena as it is read, so the reconstructed grammar
** owns all its bytes and does not point back into the host's source buffer. The
** real work is tp_load_image, which is recursive over the embedded-grammars
** section; this entry just frames the cursor over the whole image. */
tinyparse_result tinyparse_load_binary_grammar(
    tinyparse_engine  *engine,
    tinyparse_source   source,
    tinyparse_grammar **out_grammar)
{
    tp_pack_cursor cursor;

    if (out_grammar != 0)
        *out_grammar = 0;
    if (engine == 0)
        return TINYPARSE_INVALID;

    cursor.bytes  = (uint8_t *)source.bytes.pointer;
    cursor.length = 0;
    cursor.limit  = source.bytes.length;
    cursor.error  = 0;
    return tp_load_image(engine, &cursor, out_grammar);
}
