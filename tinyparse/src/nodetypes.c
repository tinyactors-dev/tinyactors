/*
** nodetypes.c -- the node-types schema emitter: a compiled grammar's typed
** surface, as JSON.
**
** A compiled grammar names a set of nodes -- one per rule and per token -- and,
** through (field NAME X) combinators, names the typed slots a rule's children
** fill. Tools that read a parse tree (highlighters, structural editors, query
** engines) want that schema without re-deriving it from the .scm source, so
** pocketparse_emit_node_types renders it as a JSON document shaped like
** tree-sitter's node-types.json: an array of node entries, each with a "type",
** a "named" flag, and a "fields" object mapping each field to the named types
** it may hold. A rule with unnamed positional children also carries "children".
**
** Like the rest of src/ except cli.c this module is freestanding: it walks the
** rule/token IR through internal.h alone, never libc, and every byte it emits is
** carved from the engine arena. The text is assembled in two passes -- a
** measuring pass with a NULL buffer counts the bytes, then a single arena block
** of that size is filled -- exactly as emit.c and pack.c do, because ax arenas
** never realloc and so a growable buffer is not an option. The two passes walk
** identical code paths, which keeps the measured and emitted lengths in step.
*/
#include "internal.h"

/* ** A text cursor mirroring emit.c's: when `bytes` is NULL the writer only
** advances `length` (the measuring pass); when it points at a real buffer the
** writer fills it. Emission is write-only -- there is no read side. */
typedef struct pp_json_cursor {
    char    *bytes;
    uint32_t length;
} pp_json_cursor;

/* ** Append one byte, or just count it during the measuring pass. */
static void pp_json_byte(pp_json_cursor *cursor, char value)
{
    if (cursor->bytes != 0)
        cursor->bytes[cursor->length] = value;
    cursor->length++;
}

/* ** Append a NUL-terminated C literal (the NUL itself is not written). */
static void pp_json_text(pp_json_cursor *cursor, const char *text)
{
    while (*text != '\0')
        pp_json_byte(cursor, *text++);
}

/* ** Append a slice as a quoted, escaped JSON string (the surrounding quotes
** included). The control characters and the two structural characters a JSON
** string may not carry verbatim -- the double quote and the backslash -- are
** escaped; everything else is written through. Node and field names are
** identifiers, so this rarely does more than wrap the bytes in quotes, but a
** grammar may alias a node to an arbitrary name, so the escaping is honest. */
static void pp_json_string(pp_json_cursor *cursor, pocketparse_string slice)
{
    uint32_t i;
    pp_json_byte(cursor, '"');
    for (i = 0; i < slice.length; i++) {
        char c = (char)slice.pointer[i];
        switch (c) {
        case '"':  pp_json_text(cursor, "\\\""); break;
        case '\\': pp_json_text(cursor, "\\\\"); break;
        case '\b': pp_json_text(cursor, "\\b");  break;
        case '\f': pp_json_text(cursor, "\\f");  break;
        case '\n': pp_json_text(cursor, "\\n");  break;
        case '\r': pp_json_text(cursor, "\\r");  break;
        case '\t': pp_json_text(cursor, "\\t");  break;
        default:
            if ((unsigned char)c < 0x20) {
                /* A bare control byte: emit the \u00XX escape JSON requires. */
                static const char hex[] = "0123456789abcdef";
                pp_json_text(cursor, "\\u00");
                pp_json_byte(cursor, hex[((unsigned char)c >> 4) & 0xf]);
                pp_json_byte(cursor, hex[(unsigned char)c & 0xf]);
            } else {
                pp_json_byte(cursor, c);
            }
            break;
        }
    }
    pp_json_byte(cursor, '"');
}

/* ** True when a symbol names a defined token (a named leaf node) rather than a
** rule. A field or child reference resolves to one or the other; the schema
** marks both "named": true, so the distinction matters only for deciding which
** table to look the kind name up in -- the name itself is shared through the
** interner either way. */
static int pp_is_token_symbol(const pocketparse_grammar *grammar, uint32_t symbol)
{
    uint32_t i;
    for (i = 0; i < grammar->token_count; i++)
        if (grammar->tokens[i].symbol == symbol)
            return 1;
    return 0;
}

/* ** True when a symbol names a defined rule. */
static int pp_is_rule_symbol(const pocketparse_grammar *grammar, uint32_t symbol)
{
    uint32_t i;
    for (i = 0; i < grammar->rule_count; i++)
        if (grammar->rules[i].symbol == symbol)
            return 1;
    return 0;
}

/* ** Emit one type reference: { "type": "<Name>", "named": <bool> }. The named
** flag is true for any symbol that names a rule or token (every node pocketparse
** reports under the interner is named); it stays false only for the synthetic
** "_" placeholder the children pass uses when it found no concrete type. */
static void pp_json_type_ref(pp_json_cursor *cursor,
                             const pocketparse_grammar *grammar,
                             uint32_t symbol)
{
    pocketparse_string name = pp_intern_name(&grammar->interner, symbol);
    pp_json_text(cursor, "{ \"type\": ");
    pp_json_string(cursor, name);
    pp_json_text(cursor, ", \"named\": true }");
}

/* ** A growable array of interned symbols, carved from the engine arena. The
** node-types collectors gather an unbounded number of kinds -- a field may admit
** any number of alternatives, a rule any number of loose children, and a grammar
** any number of distinct node kinds and fields -- so the collection grows
** geometrically rather than stopping at a fixed cap that would silently truncate
** a large grammar. ax arenas never realloc, so "grow" means allocate a block of
** twice the capacity and copy the live entries across; the abandoned block is
** reclaimed when the arena is. The growth is exercised in the measuring and
** emitting passes alike, so both walk an identical document and their byte counts
** agree. */
typedef struct pp_symbol_list {
    ax_arena *arena;
    uint32_t *items;
    uint32_t  count;
    uint32_t  capacity;
} pp_symbol_list;

/* ** Initialise an empty symbol list bound to an arena. The first append
** allocates; an empty list owns no block. */
static void pp_symbol_list_init(pp_symbol_list *list, ax_arena *arena)
{
    list->arena    = arena;
    list->items    = 0;
    list->count    = 0;
    list->capacity = 0;
}

/* ** Append a symbol to the list, growing the backing block geometrically when it
** is full. Returns 1 on success, 0 if the arena is exhausted (the caller then
** stops collecting; an out-of-memory grammar is reported up the stack). */
static int pp_symbol_list_push(pp_symbol_list *list, uint32_t symbol)
{
    if (list->count == list->capacity) {
        uint32_t  new_capacity = (list->capacity == 0) ? 8u : list->capacity * 2u;
        uint32_t *grown =
            (uint32_t *)ax_alloc(list->arena, new_capacity * sizeof *grown);
        uint32_t  i;
        if (grown == 0)
            return 0;
        for (i = 0; i < list->count; i++)
            grown[i] = list->items[i];
        list->items    = grown;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = symbol;
    return 1;
}

/* ** Collect the kind symbols a field may hold by walking the IR below a
** PP_OP_FIELD node. A field tags exactly one subpattern (child[0]); that
** subpattern is usually a single PP_OP_REF, but it may be a choice or a repeat
** of references, so the walk gathers every distinct rule/token reference under
** it. Symbols are appended to `out` only if not already present, so a choice of
** the same type twice lists it once. References that resolve to neither a rule
** nor a token (there should be none in a valid grammar) are skipped. */
static void pp_collect_field_types(const pocketparse_grammar *grammar,
                                   const pp_ir *ir, pp_symbol_list *out)
{
    uint32_t i;
    if (ir == 0)
        return;
    if (ir->op == PP_OP_REF) {
        if (pp_is_rule_symbol(grammar, ir->symbol) ||
            pp_is_token_symbol(grammar, ir->symbol)) {
            uint32_t k;
            for (k = 0; k < out->count; k++)
                if (out->items[k] == ir->symbol)
                    return; /* already recorded */
            pp_symbol_list_push(out, ir->symbol);
        }
        return;
    }
    for (i = 0; i < ir->child_count; i++)
        pp_collect_field_types(grammar, ir->children[i], out);
}

/* ** A field discovered on a rule: its name symbol and the (de-duplicated) kind
** symbols it may hold, gathered into a growable list so a field with many
** alternatives is never truncated. */
typedef struct pp_field_entry {
    uint32_t       name_symbol;
    pp_symbol_list types;
} pp_field_entry;

/* ** A growable array of field entries, carved from the engine arena, so a rule
** with many distinct fields -- or a grammar with many node kinds -- is never
** truncated. Grows geometrically exactly like pp_symbol_list. */
typedef struct pp_field_list {
    ax_arena       *arena;
    pp_field_entry *items;
    uint32_t        count;
    uint32_t        capacity;
} pp_field_list;

/* ** Initialise an empty field list bound to an arena. */
static void pp_field_list_init(pp_field_list *list, ax_arena *arena)
{
    list->arena    = arena;
    list->items    = 0;
    list->count    = 0;
    list->capacity = 0;
}

/* ** Append a fresh field entry (name set, empty type list) and return a pointer
** to it, growing the backing block geometrically when full. Returns NULL if the
** arena is exhausted. */
static pp_field_entry *pp_field_list_push(pp_field_list *list, uint32_t name_symbol)
{
    pp_field_entry *entry;
    if (list->count == list->capacity) {
        uint32_t        new_capacity = (list->capacity == 0) ? 8u : list->capacity * 2u;
        pp_field_entry *grown =
            (pp_field_entry *)ax_alloc(list->arena, new_capacity * sizeof *grown);
        uint32_t        i;
        if (grown == 0)
            return NULL;
        for (i = 0; i < list->count; i++)
            grown[i] = list->items[i];
        list->items    = grown;
        list->capacity = new_capacity;
    }
    entry = &list->items[list->count++];
    entry->name_symbol = name_symbol;
    pp_symbol_list_init(&entry->types, list->arena);
    return entry;
}

/* ** Find the field record for a field name in the in-progress list, or NULL.
** Fields are keyed by their interned name symbol, so two (field word: ...) sites
** in one rule body merge their types into a single entry. */
static pp_field_entry *pp_field_find(pp_field_list *fields, uint32_t name_symbol)
{
    uint32_t i;
    for (i = 0; i < fields->count; i++)
        if (fields->items[i].name_symbol == name_symbol)
            return &fields->items[i];
    return NULL;
}

/* ** Walk a rule body, gathering its fields (PP_OP_FIELD nodes) into `fields`
** and its unnamed positional children -- the named rule/token references that
** sit OUTSIDE any field -- into `children`. The walk descends through the
** structural combinators (seq/choice/repeat/optional/not/alias/recover/goal),
** so a field nested inside a repeat or choice is still found. A PP_OP_FIELD is
** recorded and its subtree is then scanned for the field's types but NOT for
** more loose children, since everything under a field belongs to that field.
** `in_field` is zero at the rule's top level and nonzero once the walk has
** entered a field, which keeps a field's own references out of the children
** list. */
static void pp_collect_rule_shape(const pocketparse_grammar *grammar,
                                  const pp_ir *ir, int in_field,
                                  pp_field_list *fields,
                                  pp_symbol_list *children)
{
    uint32_t i;
    if (ir == 0)
        return;

    if (ir->op == PP_OP_FIELD) {
        /* A field tags child[0] with the field name in ir->symbol. Record the
        ** field (merging into an existing entry of the same name) and gather the
        ** types its subtree can hold; do not treat those as loose children. */
        pp_field_entry *entry = pp_field_find(fields, ir->symbol);
        if (entry == NULL)
            entry = pp_field_list_push(fields, ir->symbol);
        if (entry != NULL)
            pp_collect_field_types(grammar, ir, &entry->types);
        return;
    }

    if (ir->op == PP_OP_REF) {
        /* A bare reference outside any field is a positional child. Record the
        ** named rule/token kinds it may yield, de-duplicated. */
        if (!in_field &&
            (pp_is_rule_symbol(grammar, ir->symbol) ||
             pp_is_token_symbol(grammar, ir->symbol))) {
            uint32_t k;
            for (k = 0; k < children->count; k++)
                if (children->items[k] == ir->symbol)
                    return;
            pp_symbol_list_push(children, ir->symbol);
        }
        return;
    }

    for (i = 0; i < ir->child_count; i++)
        pp_collect_rule_shape(grammar, ir->children[i], in_field,
                              fields, children);
}

/* ** Emit the "fields" object for a rule: each field name mapped to the types it
** may hold, as { "<name>": { "types": [ ... ] }, ... }. A rule with no fields
** still emits "fields": {} so the key is always present (the node-types.test
** asserts the bytes "fields" appear). */
static void pp_emit_fields(pp_json_cursor *cursor,
                           const pocketparse_grammar *grammar,
                           const pp_field_list *fields)
{
    uint32_t f;
    pp_json_text(cursor, "\"fields\": {");
    for (f = 0; f < fields->count; f++) {
        const pp_field_entry *field = &fields->items[f];
        uint32_t              t;
        pocketparse_string    field_name =
            pp_intern_name(&grammar->interner, field->name_symbol);
        if (f > 0)
            pp_json_byte(cursor, ',');
        pp_json_text(cursor, "\n      ");
        pp_json_string(cursor, field_name);
        pp_json_text(cursor, ": { \"types\": [");
        for (t = 0; t < field->types.count; t++) {
            if (t > 0)
                pp_json_byte(cursor, ',');
            pp_json_text(cursor, " ");
            pp_json_type_ref(cursor, grammar, field->types.items[t]);
        }
        pp_json_text(cursor, (field->types.count > 0) ? " ] }" : "] }");
    }
    if (fields->count > 0)
        pp_json_text(cursor, "\n    ");
    pp_json_byte(cursor, '}');
}

/* ** Emit the "children" object for a rule: the named positional kinds that sit
** outside any field, as { "multiple": <bool>, "required": false, "types":
** [ ... ] }. Mirrors tree-sitter's shape; "multiple" is true when more than one
** kind may appear, and "required" is reported false (the schema does not track
** arity precisely). Emitted only when the rule has loose children. */
static void pp_emit_children(pp_json_cursor *cursor,
                             const pocketparse_grammar *grammar,
                             const pp_symbol_list *children)
{
    uint32_t c;
    pp_json_text(cursor, ",\n    \"children\": { \"multiple\": ");
    pp_json_text(cursor, (children->count > 1) ? "true" : "false");
    pp_json_text(cursor, ", \"required\": false, \"types\": [");
    for (c = 0; c < children->count; c++) {
        if (c > 0)
            pp_json_byte(cursor, ',');
        pp_json_text(cursor, " ");
        pp_json_type_ref(cursor, grammar, children->items[c]);
    }
    pp_json_text(cursor, " ] }");
}

/* ** Emit one rule entry: its type name, the named flag, the fields object, and
** -- when the rule has unnamed positional children -- the children object. The
** rule body is walked once to collect both the fields and the loose children. */
static void pp_emit_rule_entry(pp_json_cursor *cursor,
                               const pocketparse_grammar *grammar,
                               const pp_rule_def *rule, int first)
{
    pp_field_list      fields;
    pp_symbol_list     children;
    pocketparse_string name = pp_intern_name(&grammar->interner, rule->symbol);

    pp_field_list_init(&fields, grammar->arena);
    pp_symbol_list_init(&children, grammar->arena);
    pp_collect_rule_shape(grammar, rule->body, 0, &fields, &children);

    if (!first)
        pp_json_byte(cursor, ',');
    pp_json_text(cursor, "\n  {\n    \"type\": ");
    pp_json_string(cursor, name);
    pp_json_text(cursor, ",\n    \"named\": true,\n    ");
    pp_emit_fields(cursor, grammar, &fields);
    if (children.count > 0)
        pp_emit_children(cursor, grammar, &children);
    pp_json_text(cursor, "\n  }");
}

/* ** Emit one token entry: a named leaf node with no fields and no children, so
** "fields": {} is its whole body beyond the type and named flag. */
static void pp_emit_token_entry(pp_json_cursor *cursor,
                                const pocketparse_grammar *grammar,
                                const pp_token_def *token, int first)
{
    pocketparse_string name = pp_intern_name(&grammar->interner, token->symbol);
    if (!first)
        pp_json_byte(cursor, ',');
    pp_json_text(cursor, "\n  {\n    \"type\": ");
    pp_json_string(cursor, name);
    pp_json_text(cursor, ",\n    \"named\": true,\n    \"fields\": {}\n  }");
}

/* ** Build the whole node-types JSON document into the cursor. The schema is a
** JSON array: every rule entry first, in definition order, then every token
** entry, in definition order. A `first` flag threads through so the comma
** separators land between entries and not before the first one. */
static void pp_emit_node_types_document(pp_json_cursor *cursor,
                                        const pocketparse_grammar *grammar)
{
    uint32_t i;
    int      first = 1;

    pp_json_byte(cursor, '[');
    for (i = 0; i < grammar->rule_count; i++) {
        pp_emit_rule_entry(cursor, grammar, &grammar->rules[i], first);
        first = 0;
    }
    for (i = 0; i < grammar->token_count; i++) {
        pp_emit_token_entry(cursor, grammar, &grammar->tokens[i], first);
        first = 0;
    }
    pp_json_text(cursor, "\n]\n");
}

/* ** Emit the node-types schema for a compiled grammar as JSON, into the engine
** arena, returned through out_json. Two passes -- measure with a NULL buffer,
** then fill a single arena block of that exact size -- because ax arenas never
** realloc; both passes walk the identical document builder so the lengths agree.
** The host owns nothing it must free and does the file IO. */
pocketparse_result pocketparse_emit_node_types(
    pocketparse_engine        *engine,
    const pocketparse_grammar *grammar,
    pocketparse_string        *out_json)
{
    pp_json_cursor measure;
    pp_json_cursor emit;
    char          *buffer;

    if (out_json != 0) {
        out_json->pointer = 0;
        out_json->length  = 0;
    }
    if (engine == 0 || grammar == 0 || out_json == 0)
        return POCKETPARSE_INVALID;

    /* Pass one: measure. */
    measure.bytes  = 0;
    measure.length = 0;
    pp_emit_node_types_document(&measure, grammar);

    buffer = (char *)ax_alloc(grammar->arena, measure.length);
    if (buffer == 0)
        return POCKETPARSE_OUT_OF_MEMORY;

    /* Pass two: emit into the sized buffer along an identical path. */
    emit.bytes  = buffer;
    emit.length = 0;
    pp_emit_node_types_document(&emit, grammar);

    out_json->pointer = (const uint8_t *)buffer;
    out_json->length  = emit.length;
    return POCKETPARSE_OK;
}
