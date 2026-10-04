/*
** util.c -- shared freestanding helpers for the pocketparse core.
**
** String comparison, the arena-backed string interner, the CST node builder,
** and the diagnostic collector live here because the reader, grammar compiler,
** lexer, parser, tree, and diagnostics modules all lean on them. No libc: every
** byte of storage comes from an ax arena.
*/
#include "internal.h"

#ifdef PP_DEBUG_INTERN_GUARD
/* ** A debug-only tripwire: every pp_intern call bumps this counter. The VM
** snapshots it on entry to pp_execute_vm and asserts it has not moved on exit,
** proving that W44 took string interning off the parse hot path. Compiled out of
** release builds entirely; defined here so all interning flows through it. */
unsigned long pp_intern_call_count = 0;
#endif

/* ** Build a pocketparse_string from a C literal, computing its length without
** libc so the file stays freestanding. */
pocketparse_string pp_text(const char *literal)
{
    pocketparse_string string;
    uint32_t           length = 0;
    while (literal[length] != '\0')
        length++;
    string.pointer = (const uint8_t *)literal;
    string.length  = length;
    return string;
}

/* ** Compare two byte slices for exact equality. */
int pp_string_equal(pocketparse_string a, pocketparse_string b)
{
    uint32_t i;
    if (a.length != b.length)
        return 0;
    for (i = 0; i < a.length; i++)
        if (a.pointer[i] != b.pointer[i])
            return 0;
    return 1;
}

/* ** Compare a slice against a C literal. */
int pp_string_equal_c(pocketparse_string a, const char *literal)
{
    return pp_string_equal(a, pp_text(literal));
}

/* ** Look up an interned name without creating it. Returns 1 on a hit and
** writes the id; returns 0 otherwise. */
int pp_intern_find(const pp_interner *interner, pocketparse_string name,
                   uint32_t *out_id)
{
    uint32_t i;
    for (i = 0; i < interner->count; i++) {
        if (pp_string_equal(interner->names[i], name)) {
            if (out_id != 0)
                *out_id = i;
            return 1;
        }
    }
    return 0;
}

/* ** Intern a name, returning its dense id. The name bytes are copied into the
** arena so they outlive the grammar source. The id table grows geometrically.
** On allocation failure the existing id for an empty match is returned, but
** since names are copied this only fails when the arena is exhausted. */
uint32_t pp_intern(pp_interner *interner, pocketparse_string name)
{
    uint32_t id;
    uint8_t *copy;
    uint32_t i;

#ifdef PP_DEBUG_INTERN_GUARD
    pp_intern_call_count++;
#endif

    if (pp_intern_find(interner, name, &id))
        return id;

    if (interner->count == interner->capacity) {
        uint32_t new_capacity = interner->capacity == 0
                                    ? 16 : interner->capacity * 2;
        pocketparse_string *grown = (pocketparse_string *)ax_alloc(
            interner->arena, new_capacity * sizeof *grown);
        if (grown == 0)
            return interner->count; /* arena exhausted */
        for (i = 0; i < interner->count; i++)
            grown[i] = interner->names[i];
        interner->names    = grown;
        interner->capacity = new_capacity;
    }

    copy = (uint8_t *)ax_alloc(interner->arena, name.length == 0 ? 1 : name.length);
    if (copy == 0)
        return interner->count;
    for (i = 0; i < name.length; i++)
        copy[i] = name.pointer[i];

    id = interner->count;
    interner->names[id].pointer = copy;
    interner->names[id].length  = name.length;
    interner->count++;
    return id;
}

/* ** Intern a C literal name. */
uint32_t pp_intern_c(pp_interner *interner, const char *literal)
{
    return pp_intern(interner, pp_text(literal));
}

/* ** The printable name for an interned id, or the empty string if out range. */
pocketparse_string pp_intern_name(const pp_interner *interner, uint32_t id)
{
    if (id >= interner->count)
        return pp_text("");
    return interner->names[id];
}

/* ** Append a node record to the builder, returning its index. The node begins
** with no children and no sibling link; the parser wires those up as it builds
** the tree in pre-order. Returns PP_NO_INDEX when the arena is exhausted. */
uint32_t pp_tree_builder_push(pp_tree_builder *builder, uint32_t kind,
                              uint32_t field, uint32_t start_byte,
                              uint32_t end_byte, uint32_t parent)
{
    pp_node *node;
    uint32_t index;

    if (builder->count == builder->capacity) {
        uint32_t new_capacity = builder->capacity == 0
                                    ? 32 : builder->capacity * 2;
        /* RAW: the live prefix [0,count) is copied in below; every later node is
        ** fully written by the pp_tree_builder_push that claims it before any read,
        ** so zeroing the (geometrically large) tail block would be wasted (W16-1). */
        pp_node *grown = (pp_node *)ax_alloc_raw(builder->arena,
                                                 new_capacity * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return PP_NO_INDEX;
        for (i = 0; i < builder->count; i++)
            grown[i] = builder->nodes[i];
        builder->nodes    = grown;
        builder->capacity = new_capacity;
    }

    index = builder->count++;
    node  = &builder->nodes[index];
    node->kind         = kind;
    node->field        = field;
    node->start_byte   = start_byte;
    node->end_byte     = end_byte;
    node->parent       = parent;
    node->first_child  = PP_NO_INDEX;
    node->child_count  = 0;
    node->next_sibling = PP_NO_INDEX;
    node->flags        = 0;
    node->language_tag = PP_NO_SYMBOL;
    return index;
}

/* ** Append an error diagnostic with one label over [start_byte, end_byte).
** The strings are C literals owned by the caller's static storage, so they are
** referenced directly; the label array is carved from the arena. Returns 1 on
** success, 0 on allocation failure. */
int pp_diag_error(pp_diag_list *list, const char *code, const char *message,
                  uint32_t start_byte, uint32_t end_byte,
                  const char *label_message)
{
    pocketparse_diagnostic *diag;
    pocketparse_label      *label;

    if (list->count == list->capacity) {
        uint32_t new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        pocketparse_diagnostic *grown = (pocketparse_diagnostic *)ax_alloc(
            list->arena, new_capacity * sizeof *grown);
        uint32_t i;
        if (grown == 0)
            return 0;
        for (i = 0; i < list->count; i++)
            grown[i] = list->items[i];
        list->items    = grown;
        list->capacity = new_capacity;
    }

    label = (pocketparse_label *)ax_alloc(list->arena, sizeof *label);
    if (label == 0)
        return 0;
    label->start_byte = start_byte;
    label->end_byte   = end_byte;
    label->message    = pp_text(label_message);
    label->flags      = 0;

    diag = &list->items[list->count++];
    diag->severity    = POCKETPARSE_SEVERITY_ERROR;
    diag->code        = pp_text(code);
    diag->message     = pp_text(message);
    diag->labels      = label;
    diag->label_count = 1;
    diag->help        = pp_text("");
    diag->note        = pp_text("");
    return 1;
}
