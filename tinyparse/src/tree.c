/*
** tree.c -- the contiguous concrete syntax tree and its cursor API.
**
** Nodes live in one arena-backed array, appended in pre-order so a parent
** always precedes its children. Each record links to its parent, first child,
** and next sibling, which is all the cursor needs to walk the tree without any
** allocation per node. Both the heap cursor (an arena-carved storage struct)
** and the stack-allocatable storage form share one implementation. Freestanding.
*/
#include "internal.h"

/* ** Resolve a node index to its record, or NULL when out of range. */
static const pp_node *pp_tree_node(const pocketparse_tree *tree, uint32_t index)
{
    if (tree == 0 || index >= tree->node_count)
        return 0;
    return &tree->nodes[index];
}

/* ** The cursor is exactly the public storage struct; the opaque handle is a
** thin wrapper carved from the tree arena. */
struct pocketparse_tree_cursor {
    pocketparse_tree_cursor_storage storage;
};

/* ** Point the storage at a node, recomputing its child range and parent. */
static void pp_cursor_focus(pocketparse_tree_cursor_storage *storage,
                            uint32_t index)
{
    const pocketparse_tree *tree = storage->tree;
    const pp_node *node = pp_tree_node(tree, index);
    storage->current_node_index = index;
    if (node == 0) {
        storage->first_child  = PP_NO_INDEX;
        storage->child_count  = 0;
        storage->parent_node_index = PP_NO_INDEX;
        return;
    }
    storage->first_child       = node->first_child;
    storage->child_count       = node->child_count;
    storage->parent_node_index = node->parent;
}

/* ** Initialise stack-allocated cursor storage at a tree's root. */
void pocketparse_tree_cursor_init(pocketparse_tree_cursor_storage *storage,
                                  const pocketparse_tree *tree)
{
    uint32_t i;
    storage->tree  = tree;
    storage->depth = 0;
    storage->child_index = 0;
    storage->flags = 0;
    for (i = 0; i < 8; i++)
        storage->reserved[i] = 0;
    if (tree == 0) {
        storage->current_node_index = PP_NO_INDEX;
        storage->parent_node_index  = PP_NO_INDEX;
        storage->first_child = PP_NO_INDEX;
        storage->child_count = 0;
        return;
    }
    pp_cursor_focus(storage, tree->root);
    storage->parent_node_index = PP_NO_INDEX;
}

/* ** Create a heap cursor over a tree. */
pocketparse_result pocketparse_tree_cursor_new(
    const pocketparse_tree *tree,
    pocketparse_tree_cursor **out_cursor)
{
    pocketparse_tree_cursor *cursor;
    if (out_cursor != 0)
        *out_cursor = 0;
    if (tree == 0 || out_cursor == 0)
        return POCKETPARSE_INVALID;
    cursor = (pocketparse_tree_cursor *)ax_alloc(tree->arena, sizeof *cursor);
    if (cursor == 0)
        return POCKETPARSE_OUT_OF_MEMORY;
    pocketparse_tree_cursor_init(&cursor->storage, tree);
    *out_cursor = cursor;
    return POCKETPARSE_OK;
}

/* ** Release a cursor. Its storage is in the tree arena; nothing to free. */
void pocketparse_tree_cursor_free(pocketparse_tree_cursor *cursor)
{
    (void)cursor;
}

/* ** Rebind a cursor to a (possibly different) tree at its root. */
void pocketparse_tree_cursor_reset(pocketparse_tree_cursor *cursor,
                                   const pocketparse_tree *tree)
{
    if (cursor == 0)
        return;
    pocketparse_tree_cursor_init(&cursor->storage, tree);
}

/* ** Reposition a cursor at the root of its current tree. */
void pocketparse_tree_cursor_reset_to_root(pocketparse_tree_cursor *cursor)
{
    if (cursor == 0)
        return;
    pocketparse_tree_cursor_init(&cursor->storage, cursor->storage.tree);
}

/* ** The kind symbol of the node under the cursor. */
pocketparse_symbol pocketparse_tree_cursor_kind(
    const pocketparse_tree_cursor *cursor)
{
    pocketparse_symbol symbol;
    const pp_node *node;
    symbol.id = 0;
    if (cursor == 0)
        return symbol;
    node = pp_tree_node(cursor->storage.tree, cursor->storage.current_node_index);
    if (node != 0)
        symbol.id = node->kind;
    return symbol;
}

/* ** The field symbol of the node under the cursor, or POCKETPARSE_NO_FIELD. */
pocketparse_symbol pocketparse_tree_cursor_field(
    const pocketparse_tree_cursor *cursor)
{
    pocketparse_symbol symbol;
    const pp_node *node;
    symbol.id = PP_NO_SYMBOL;
    if (cursor == 0)
        return symbol;
    node = pp_tree_node(cursor->storage.tree, cursor->storage.current_node_index);
    if (node != 0)
        symbol.id = node->field;
    return symbol;
}

/* ** The printable name of a symbol within a tree. */
pocketparse_string pocketparse_tree_symbol_name(const pocketparse_tree *tree,
                                                pocketparse_symbol symbol)
{
    if (tree == 0 || tree->interner == 0)
        return pp_text("");
    return pp_intern_name(tree->interner, symbol.id);
}

/* ** The start byte offset of the node under the cursor. */
uint32_t pocketparse_tree_cursor_start_byte(
    const pocketparse_tree_cursor *cursor)
{
    const pp_node *node;
    if (cursor == 0)
        return 0;
    node = pp_tree_node(cursor->storage.tree, cursor->storage.current_node_index);
    return node ? node->start_byte : 0;
}

/* ** The end byte offset of the node under the cursor. */
uint32_t pocketparse_tree_cursor_end_byte(
    const pocketparse_tree_cursor *cursor)
{
    const pp_node *node;
    if (cursor == 0)
        return 0;
    node = pp_tree_node(cursor->storage.tree, cursor->storage.current_node_index);
    return node ? node->end_byte : 0;
}

/* ** The child count of the node under the cursor. */
uint32_t pocketparse_tree_cursor_child_count(
    const pocketparse_tree_cursor *cursor)
{
    if (cursor == 0)
        return 0;
    return cursor->storage.child_count;
}

/* ** The depth of the node under the cursor below the root. */
uint32_t pocketparse_tree_cursor_depth(const pocketparse_tree_cursor *cursor)
{
    if (cursor == 0)
        return 0;
    return cursor->storage.depth;
}

/* ** The flag word of the node under the cursor (POCKETPARSE_NODE_* bits). Note
** this reads the node record's flags, not the cursor storage's own scratch
** flags field, so a host learns whether the node is a recovery insertion. */
uint32_t pocketparse_tree_cursor_flags(const pocketparse_tree_cursor *cursor)
{
    const pp_node *node;
    if (cursor == 0)
        return 0;
    node = pp_tree_node(cursor->storage.tree, cursor->storage.current_node_index);
    return node ? node->flags : 0;
}

/* ** The language tag (W62 injection) of the node under the cursor: the interned
** id of the language the node was parsed by, or PP_NO_SYMBOL for a host-grammar
** node. INTERNAL only; --dump-tree renders it as a `[lang]' annotation. */
uint32_t pp_tree_cursor_language_tag(const pocketparse_tree_cursor *cursor)
{
    const pp_node *node;
    if (cursor == 0)
        return PP_NO_SYMBOL;
    node = pp_tree_node(cursor->storage.tree, cursor->storage.current_node_index);
    return node ? node->language_tag : PP_NO_SYMBOL;
}

/* ** The index of the node under the cursor within its parent. Computed by
** walking the parent's child chain. The root reports 0. */
uint32_t pocketparse_tree_cursor_index_in_parent(
    const pocketparse_tree_cursor *cursor)
{
    const pocketparse_tree *tree;
    const pp_node *parent;
    uint32_t index, walk, position = 0;
    if (cursor == 0)
        return 0;
    tree   = cursor->storage.tree;
    index  = cursor->storage.current_node_index;
    parent = pp_tree_node(tree, cursor->storage.parent_node_index);
    if (parent == 0)
        return 0;
    walk = parent->first_child;
    while (walk != PP_NO_INDEX && walk != index) {
        const pp_node *node = pp_tree_node(tree, walk);
        if (node == 0)
            break;
        walk = node->next_sibling;
        position++;
    }
    return position;
}

/* ** Move the cursor to the parent; nonzero on success. */
int pocketparse_tree_cursor_goto_parent(pocketparse_tree_cursor *cursor)
{
    uint32_t parent;
    if (cursor == 0)
        return 0;
    parent = cursor->storage.parent_node_index;
    if (parent == PP_NO_INDEX)
        return 0;
    pp_cursor_focus(&cursor->storage, parent);
    if (cursor->storage.depth > 0)
        cursor->storage.depth--;
    return 1;
}

/* ** Move the cursor to the first child; nonzero on success. */
int pocketparse_tree_cursor_goto_first_child(pocketparse_tree_cursor *cursor)
{
    uint32_t first;
    if (cursor == 0)
        return 0;
    first = cursor->storage.first_child;
    if (first == PP_NO_INDEX)
        return 0;
    pp_cursor_focus(&cursor->storage, first);
    cursor->storage.depth++;
    return 1;
}

/* ** Move the cursor to the last child; nonzero on success. */
int pocketparse_tree_cursor_goto_last_child(pocketparse_tree_cursor *cursor)
{
    uint32_t walk, last = PP_NO_INDEX;
    if (cursor == 0)
        return 0;
    walk = cursor->storage.first_child;
    while (walk != PP_NO_INDEX) {
        const pp_node *node = pp_tree_node(cursor->storage.tree, walk);
        last = walk;
        if (node == 0)
            break;
        walk = node->next_sibling;
    }
    if (last == PP_NO_INDEX)
        return 0;
    pp_cursor_focus(&cursor->storage, last);
    cursor->storage.depth++;
    return 1;
}

/* ** Move the cursor to the next sibling; nonzero on success. */
int pocketparse_tree_cursor_goto_next_sibling(pocketparse_tree_cursor *cursor)
{
    const pp_node *node;
    if (cursor == 0)
        return 0;
    node = pp_tree_node(cursor->storage.tree, cursor->storage.current_node_index);
    if (node == 0 || node->next_sibling == PP_NO_INDEX)
        return 0;
    pp_cursor_focus(&cursor->storage, node->next_sibling);
    return 1;
}

/* ** Move the cursor to the previous sibling; nonzero on success. */
int pocketparse_tree_cursor_goto_previous_sibling(
    pocketparse_tree_cursor *cursor)
{
    const pocketparse_tree *tree;
    const pp_node *parent;
    uint32_t index, walk, previous = PP_NO_INDEX;
    if (cursor == 0)
        return 0;
    tree   = cursor->storage.tree;
    index  = cursor->storage.current_node_index;
    parent = pp_tree_node(tree, cursor->storage.parent_node_index);
    if (parent == 0)
        return 0;
    walk = parent->first_child;
    while (walk != PP_NO_INDEX && walk != index) {
        const pp_node *node = pp_tree_node(tree, walk);
        previous = walk;
        if (node == 0)
            break;
        walk = node->next_sibling;
    }
    if (previous == PP_NO_INDEX)
        return 0;
    pp_cursor_focus(&cursor->storage, previous);
    return 1;
}

/* ** Move the cursor to a child by index; nonzero on success. */
int pocketparse_tree_cursor_goto_child(pocketparse_tree_cursor *cursor,
                                       uint32_t child_index)
{
    uint32_t walk, position = 0;
    if (cursor == 0)
        return 0;
    walk = cursor->storage.first_child;
    while (walk != PP_NO_INDEX) {
        const pp_node *node;
        if (position == child_index) {
            pp_cursor_focus(&cursor->storage, walk);
            cursor->storage.depth++;
            return 1;
        }
        node = pp_tree_node(cursor->storage.tree, walk);
        if (node == 0)
            break;
        walk = node->next_sibling;
        position++;
    }
    return 0;
}

/* ** Move to the first child carrying a named field; nonzero on success. */
int pocketparse_tree_cursor_goto_first_child_by_field_id(
    pocketparse_tree_cursor *cursor, pocketparse_symbol field)
{
    uint32_t walk;
    if (cursor == 0)
        return 0;
    walk = cursor->storage.first_child;
    while (walk != PP_NO_INDEX) {
        const pp_node *node = pp_tree_node(cursor->storage.tree, walk);
        if (node == 0)
            break;
        if (node->field == field.id) {
            pp_cursor_focus(&cursor->storage, walk);
            cursor->storage.depth++;
            return 1;
        }
        walk = node->next_sibling;
    }
    return 0;
}

/* ** Move to the first child carrying a named field, by field name. */
int pocketparse_tree_cursor_goto_first_child_by_field(
    pocketparse_tree_cursor *cursor, pocketparse_string field)
{
    pocketparse_symbol symbol;
    uint32_t id;
    if (cursor == 0 || cursor->storage.tree == 0 ||
        cursor->storage.tree->interner == 0)
        return 0;
    if (!pp_intern_find(cursor->storage.tree->interner, field, &id))
        return 0;
    symbol.id = id;
    return pocketparse_tree_cursor_goto_first_child_by_field_id(cursor, symbol);
}

/* ** Clone a cursor's full state into a freshly allocated cursor. */
pocketparse_result pocketparse_tree_cursor_clone(
    const pocketparse_tree_cursor *source,
    pocketparse_tree_cursor **out_cursor)
{
    pocketparse_tree_cursor *cursor;
    if (out_cursor != 0)
        *out_cursor = 0;
    if (source == 0 || out_cursor == 0 || source->storage.tree == 0)
        return POCKETPARSE_INVALID;
    cursor = (pocketparse_tree_cursor *)ax_alloc(
        source->storage.tree->arena, sizeof *cursor);
    if (cursor == 0)
        return POCKETPARSE_OUT_OF_MEMORY;
    cursor->storage = source->storage;
    *out_cursor = cursor;
    return POCKETPARSE_OK;
}

/* ** Copy one cursor's state over another's. */
void pocketparse_tree_cursor_assign(pocketparse_tree_cursor *target,
                                    const pocketparse_tree_cursor *source)
{
    if (target == 0 || source == 0)
        return;
    target->storage = source->storage;
}
