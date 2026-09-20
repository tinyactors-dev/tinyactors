/*
 * Cursors: navigation, ranges, and the read side of a node. Every function is
 * total (an absent cursor reads as absent) and is integer work on the node
 * column. See docs/internal/cursor.md.
 */
#include "doc.h"

static const txml_node *node_of(txml_cursor cursor) {
    return txml_doc_node(cursor.doc, cursor.node);
}

static bool is_open(const txml_node *node) {
    return (node->flags & TXML_NODE_OPEN) != 0u;
}

txml_cursor txml_doc_root(const txml_doc *doc) {
    txml_cursor cursor = { doc, 0u };
    return cursor;
}

txml_cursor txml_cursor_at(const txml_doc *doc, uint32_t index) {
    txml_cursor cursor = { doc, index };
    return cursor;
}

uint32_t txml_index(txml_cursor cursor) {
    return cursor.node;
}

/* ------------------------------------------------------------------------
 * Movement.
 */

txml_move txml_first_child(txml_cursor *cursor) {
    const txml_node *node = node_of(*cursor);
    if (node == NULL) {
        return TXML_NONE;
    }
    const uint32_t next = cursor->node + 1u;
    /* An open node's children arrive as the parse proceeds; `next` exists as
     * soon as one has been appended. */
    const uint32_t limit = is_open(node) ? txml_doc_node_count(cursor->doc) : node->end;
    if (next < limit) {
        cursor->node = next;
        return TXML_MOVED;
    }
    return is_open(node) && !cursor->doc->complete ? TXML_PENDING : TXML_NONE;
}

txml_move txml_next_sibling(txml_cursor *cursor) {
    const txml_node *node = node_of(*cursor);
    if (node == NULL || node->parent == TXML_INDEX_NONE) {
        return TXML_NONE;
    }
    if (is_open(node)) {
        return TXML_PENDING;
    }

    const txml_node *parent = txml_doc_node(cursor->doc, node->parent);
    const uint32_t count = txml_doc_node_count(cursor->doc);
    if (is_open(parent)) {
        /* Everything after an open node is inside it, so anything appended
         * past this node is a sibling; nothing yet means "ask again". */
        if (node->end < count) {
            cursor->node = node->end;
            return TXML_MOVED;
        }
        return TXML_PENDING;
    }
    if (node->end < parent->end) {
        cursor->node = node->end;
        return TXML_MOVED;
    }
    return TXML_NONE;
}

txml_move txml_parent(txml_cursor *cursor) {
    const txml_node *node = node_of(*cursor);
    if (node == NULL || node->parent == TXML_INDEX_NONE) {
        return TXML_NONE;
    }
    cursor->node = node->parent;
    return TXML_MOVED;
}

txml_move txml_next_element(txml_cursor *cursor) {
    txml_cursor probe = *cursor;
    for (;;) {
        const txml_move move = txml_next_sibling(&probe);
        if (move != TXML_MOVED) {
            return move;
        }
        if (node_of(probe)->kind == TXML_NODE_ELEMENT) {
            *cursor = probe;
            return TXML_MOVED;
        }
    }
}

/* ------------------------------------------------------------------------
 * Ranges.
 */

uint32_t txml_subtree_end(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    return node == NULL || is_open(node) ? TXML_INDEX_NONE : node->end;
}

bool txml_contains(txml_cursor ancestor, txml_cursor node) {
    const uint32_t end = txml_subtree_end(ancestor);
    return ancestor.doc == node.doc && end != TXML_INDEX_NONE && node.node >= ancestor.node && node.node < end;
}

txml_move txml_find_next(txml_cursor *cursor, const txml_find *query) {
    if (node_of(*cursor) == NULL || query->name == TXML_NAME_NONE) {
        return TXML_NONE;
    }
    const uint32_t count = txml_doc_node_count(cursor->doc);
    const uint32_t bound = query->bound < count ? query->bound : count;

    for (uint32_t i = cursor->node + 1u; i < bound; i++) {
        const txml_node *node = txml_doc_node(cursor->doc, i);
        if (node->name == query->name && node->kind == TXML_NODE_ELEMENT) {
            cursor->node = i;
            return TXML_MOVED;
        }
    }
    /* Reached the end of what exists; more may still arrive inside the bound. */
    if (query->bound > count && !cursor->doc->complete) {
        return TXML_PENDING;
    }
    return TXML_NONE;
}

/* ------------------------------------------------------------------------
 * Reading.
 */

txml_node_kind txml_kind(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    return node == NULL ? TXML_NODE_DOCUMENT : (txml_node_kind)node->kind;
}

uint32_t txml_flags(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    return node == NULL ? 0u : node->flags;
}

uint32_t txml_depth(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    return node == NULL ? 0u : node->depth;
}

uint32_t txml_node_name_id(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    if (node == NULL || (node->kind != TXML_NODE_ELEMENT && node->kind != TXML_NODE_PI)) {
        return TXML_NAME_NONE;
    }
    return node->name;
}

txml_str txml_node_prefix(txml_cursor cursor) {
    if (node_of(cursor) == NULL || txml_node_name_id(cursor) == TXML_NAME_NONE) {
        return txml_doc_pool_str(cursor.doc, 0u, 0u);
    }
    return txml_doc_prefix_of(cursor.doc, false, cursor.node);
}

txml_str txml_node_text(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    if (node == NULL || (node->kind != TXML_NODE_TEXT && node->kind != TXML_NODE_COMMENT &&
                         node->kind != TXML_NODE_PI && node->kind != TXML_NODE_DOCTYPE)) {
        return txml_doc_pool_str(cursor.doc, 0u, 0u);
    }
    return txml_doc_pool_str(cursor.doc, node->as.text.offset, node->as.text.length);
}

txml_result txml_node_position(txml_cursor cursor, txml_position *out) {
    if (node_of(cursor) == NULL) {
        return TXML_ERR_INVALID;
    }
    *out = *txml_doc_position(cursor.doc, cursor.node);
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * Attributes.
 */

uint32_t txml_attribute_count(txml_cursor cursor) {
    const txml_node *node = node_of(cursor);
    if (node == NULL || (node->kind != TXML_NODE_ELEMENT && node->kind != TXML_NODE_DECLARATION)) {
        return 0u;
    }
    return node->as.element.attribute_count;
}

txml_move txml_first_attribute(txml_cursor cursor, txml_attr_cursor *out) {
    if (txml_attribute_count(cursor) == 0u) {
        return TXML_NONE;
    }
    out->doc = cursor.doc;
    out->index = node_of(cursor)->as.element.first_attribute;
    return TXML_MOVED;
}

txml_move txml_next_attribute(txml_attr_cursor *attribute) {
    const txml_attribute_entry *entry = txml_doc_attribute(attribute->doc, attribute->index);
    if (entry == NULL || (entry->name & TXML_ATTRIBUTE_LAST) != 0u) {
        return TXML_NONE;
    }
    attribute->index += 1u;
    return TXML_MOVED;
}

txml_result txml_attribute_read(txml_attr_cursor attribute, txml_attribute_view *out) {
    const txml_attribute_entry *entry = txml_doc_attribute(attribute.doc, attribute.index);
    if (entry == NULL) {
        return TXML_ERR_INVALID;
    }
    const uint32_t name = entry->name & TXML_ATTRIBUTE_NAME_MASK;
    out->name = name;
    out->value = txml_doc_pool_str(attribute.doc, entry->value_offset, entry->value_length);
    out->flags = txml_doc_attribute_flags(attribute.doc, entry);
    return TXML_OK;
}

bool txml_find_attribute(txml_cursor cursor, uint32_t name, txml_str *value) {
    txml_attr_cursor attribute;
    txml_move move = txml_first_attribute(cursor, &attribute);

    while (move == TXML_MOVED) {
        const txml_attribute_entry *entry = txml_doc_attribute(attribute.doc, attribute.index);
        if ((entry->name & TXML_ATTRIBUTE_NAME_MASK) == name) {
            *value = txml_doc_pool_str(attribute.doc, entry->value_offset, entry->value_length);
            return true;
        }
        move = txml_next_attribute(&attribute);
    }
    return false;
}
