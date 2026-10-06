/* link.c — string interning, chart queue, event-name trie, image emission. */
#include "compile/compile_internal.h"

#include <stdlib.h>
#include <string.h>

uint32_t ta_compiler_string(ta_compiler *compiler, const char *s, uint32_t length) {
    uint32_t hit = ta_string_map_get(&compiler->string_map, s, length);
    if (hit != TA_NIL) return hit;
    uint32_t sid = compiler->strings.length;
    uint32_t total = (4 + length + 1 + 3) & ~3u;
    unsigned char *dst = (unsigned char *)ta_vector_push_many(&compiler->strings, total);
    if (!dst) return 0;
    memcpy(dst, &length, 4);
    if (length) memcpy(dst + 4, s, length);
    char *key = ta_chunk_arena_strdup(&compiler->arena, s, length);
    if (key) ta_string_map_put(&compiler->string_map, key, length, sid);
    return sid;
}

uint32_t ta_compiler_event_name(ta_compiler *compiler, const char *s, uint32_t length) {
    uint32_t hit = ta_string_map_get(&compiler->event_name_map, s, length);
    if (hit != TA_NIL) return hit;
    char *key = ta_chunk_arena_strdup(&compiler->arena, s, length);
    ta_string_view *e = (ta_string_view *)ta_vector_push(&compiler->event_names);
    if (!key || !e) return 0;
    e->pointer = key;
    e->length = length;
    ta_string_map_put(&compiler->event_name_map, key, length, compiler->event_names.length - 1);
    return compiler->event_names.length - 1;
}

uint32_t ta_compiler_queue(ta_compiler *compiler, const char *name, const char *text,
                           uint32_t length, ta_xml_node *root) {
    uint32_t hit = ta_string_map_get(&compiler->source_map, name, (uint32_t)strlen(name));
    if (hit != TA_NIL) return hit;
    ta_pending_document *pending = (ta_pending_document *)ta_vector_push(&compiler->pending);
    if (!pending) return TA_NIL;
    pending->name = ta_chunk_arena_strdup(&compiler->arena, name, strlen(name));
    pending->text = root ? text : ta_chunk_arena_strdup(&compiler->arena, text, length);
    pending->length = length;
    pending->root = root;
    uint32_t index = compiler->pending.length - 1;
    if (pending->name)
        ta_string_map_put(&compiler->source_map, pending->name, (uint32_t)strlen(pending->name),
                          index);
    return index;
}

/* ---- event trie ------------------------------------------------------------------- */

typedef struct trie_node {
    const char *token;
    uint32_t token_length;
    const char *full;
    uint32_t full_length;
    struct trie_node **children;
    uint32_t child_count, child_capacity;
    uint32_t id;
    uint32_t index; /* 0 for the root, else 1 + its index in trie_index.nodes */
} trie_node;

/* (parent, token) -> child, so a step costs O(token) however many siblings
 * there are (and however long the dotted name is). */
typedef struct trie_index {
    ta_string_map map; /* key: parent ix (4 bytes) + token -> index in nodes */
    ta_vector nodes;   /* tnode* */
    ta_vector key;     /* scratch */
} trie_index;

static trie_node *tnode_new(ta_compiler *compiler, const char *tok, uint32_t tlen, const char *full,
                            uint32_t flen) {
    trie_node *n = (trie_node *)ta_chunk_arena_alloc(&compiler->arena, sizeof *n);
    if (!n) return NULL;
    n->token = tok;
    n->token_length = tlen;
    n->full = full;
    n->full_length = flen;
    return n;
}

static trie_node *tnode_child(ta_compiler *compiler, trie_index *ix, trie_node *n, const char *tok,
                              uint32_t tlen, const char *full, uint32_t flen) {
    ix->key.length = 0;
    char *key = (char *)ta_vector_push_many(&ix->key, 4 + tlen);
    if (!key) return NULL;
    memcpy(key, &n->index, 4);
    if (tlen) memcpy(key + 4, tok, tlen);
    uint32_t hit = ta_string_map_get(&ix->map, key, 4 + tlen);
    if (hit != TA_NIL) return TA_VECTOR_AT(&ix->nodes, trie_node *, hit);
    if (n->child_count == n->child_capacity) {
        uint32_t nc = n->child_capacity ? n->child_capacity * 2 : 4;
        trie_node **nk =
            (trie_node **)ta_chunk_arena_alloc(&compiler->arena, sizeof(trie_node *) * nc);
        if (!nk) return NULL;
        if (n->child_count) memcpy(nk, n->children, sizeof(trie_node *) * n->child_count);
        n->children = nk;
        n->child_capacity = nc;
    }
    trie_node *k = tnode_new(compiler, tok, tlen, full, flen);
    trie_node **slot = (trie_node **)ta_vector_push(&ix->nodes);
    char *kept = ta_chunk_arena_strdup(&compiler->arena, key, 4 + tlen);
    if (!k || !slot || !kept) return NULL;
    *slot = k;
    k->index = ix->nodes.length;
    ta_string_map_put(&ix->map, kept, 4 + tlen, ix->nodes.length - 1);
    n->children[n->child_count++] = k;
    return k;
}

static int cmp_tok(const void *a, const void *b) {
    const trie_node *x = *(trie_node *const *)a, *y = *(trie_node *const *)b;
    uint32_t m = x->token_length < y->token_length ? x->token_length : y->token_length;
    int c = memcmp(x->token, y->token, m);
    return c ? c : (int)x->token_length - (int)y->token_length;
}

/* Iterative preorder numbering. */
static void number(ta_compiler *compiler, trie_node *root) {
    ta_vector stack;
    ta_vector_init(&stack, compiler->allocator, sizeof(trie_node *));
    *(trie_node **)ta_vector_push(&stack) = root;
    while (stack.length) {
        trie_node *n = TA_VECTOR_AT(&stack, trie_node *, --stack.length);
        if (n->child_count > 1) qsort(n->children, n->child_count, sizeof(trie_node *), cmp_tok);
        n->id = compiler->event_nodes.length;
        ta_event_node_record *node = (ta_event_node_record *)ta_vector_push(&compiler->event_nodes);
        if (!node) break;
        node->token = ta_compiler_string(compiler, n->token, n->token_length);
        node->name = ta_compiler_string(compiler, n->full, n->full_length);
        node->parent = TA_NIL;
        for (uint32_t i = n->child_count; i-- > 0;) {
            trie_node **slot = (trie_node **)ta_vector_push(&stack);
            if (slot) *slot = n->children[i];
        }
    }
    ta_vector_free(&stack);
}

/* parent + end via a second pass using explicit stack of (node, parent id). */
static void fill_links(ta_compiler *compiler, trie_node *root) {
    typedef struct item {
        trie_node *count;
        uint32_t parent;
    } item;
    ta_vector stack;
    ta_vector_init(&stack, compiler->allocator, sizeof(item));
    item *it = (item *)ta_vector_push(&stack);
    it->count = root;
    it->parent = TA_NIL;
    while (stack.length) {
        item cur = TA_VECTOR_AT(&stack, item, --stack.length);
        TA_VECTOR_AT(&compiler->event_nodes, ta_event_node_record, cur.count->id).parent =
            cur.parent;
        for (uint32_t i = 0; i < cur.count->child_count; i++) {
            item *x = (item *)ta_vector_push(&stack);
            if (!x) break;
            x->count = cur.count->children[i];
            x->parent = cur.count->id;
        }
    }
    ta_vector_free(&stack);
    /* end: children have higher ids; process in reverse */
    uint32_t n = compiler->event_nodes.length;
    for (uint32_t i = 0; i < n; i++)
        TA_VECTOR_AT(&compiler->event_nodes, ta_event_node_record, i).end = i + 1;
    for (uint32_t i = n; i-- > 1;) {
        ta_event_node_record *node = &TA_VECTOR_AT(&compiler->event_nodes, ta_event_node_record, i);
        ta_event_node_record *node_record =
            &TA_VECTOR_AT(&compiler->event_nodes, ta_event_node_record, node->parent);
        if (node->end > node_record->end) node_record->end = node->end;
    }
}

void ta_compiler_link_names(ta_compiler *compiler) {
    /* platform names always present */
    static const char *const platform[] = {"done.state", "done.invoke", "error.execution",
                                           "error.communication", "error.platform"};
    for (size_t i = 0; i < sizeof platform / sizeof platform[0]; i++)
        ta_compiler_event_name(compiler, platform[i], (uint32_t)strlen(platform[i]));

    trie_node *root = tnode_new(compiler, "", 0, "", 0);
    if (!root) return;
    ta_vector leaf; /* event name -> trie_node* */
    ta_vector_init(&leaf, compiler->allocator, sizeof(trie_node *));
    trie_index ix;
    ta_string_map_init(&ix.map, compiler->allocator);
    ta_vector_init(&ix.nodes, compiler->allocator, sizeof(trie_node *));
    ta_vector_init(&ix.key, compiler->allocator, 1);
    for (uint32_t i = 0; i < compiler->event_names.length; i++) {
        if ((i & 1023) == 0 && ta_compiler_stop(compiler)) break;
        ta_string_view nm = TA_VECTOR_AT(&compiler->event_names, ta_string_view, i);
        trie_node *n = root;
        uint32_t position = 0;
        while (n && position < nm.length) {
            uint32_t e = position;
            while (e < nm.length && nm.pointer[e] != '.')
                e++;
            n = tnode_child(compiler, &ix, n, nm.pointer + position, e - position, nm.pointer, e);
            position = e + 1;
        }
        trie_node **slot = (trie_node **)ta_vector_push(&leaf);
        if (slot) *slot = n ? n : root;
    }
    int oom = ix.map.out_of_memory || ix.nodes.out_of_memory || ix.key.out_of_memory;
    ta_string_map_free(&ix.map);
    ta_vector_free(&ix.nodes);
    ta_vector_free(&ix.key);
    if (oom || ta_compiler_stop(compiler)) {
        compiler->arena.out_of_memory |= oom; /* reported as out of memory by the caller */
        ta_vector_free(&leaf);
        return;
    }
    number(compiler, root);
    fill_links(compiler, root);

    ta_vector_init(&compiler->event_name_node, compiler->allocator, sizeof(uint32_t));
    for (uint32_t i = 0; i < leaf.length; i++)
        ta_vector_push_u32(&compiler->event_name_node, TA_VECTOR_AT(&leaf, trie_node *, i)->id);
    ta_vector_free(&leaf);
    const uint32_t *map = (const uint32_t *)(void *)compiler->event_name_node.data;
    const ta_event_node_record *event =
        (const ta_event_node_record *)(void *)compiler->event_nodes.data;

    /* patch: transitions' descriptor pairs */
    for (uint32_t i = 0; i < compiler->transitions.length; i++) {
        ta_transition_record *transition =
            &TA_VECTOR_AT(&compiler->transitions, ta_transition_record, i);
        for (uint32_t k = 0; k < transition->event_count; k++) {
            uint32_t *pair =
                &TA_VECTOR_AT(&compiler->u32, uint32_t, transition->events_offset + 2 * k);
            uint32_t node = map[pair[0]];
            pair[0] = node;
            pair[1] = event[node].end;
        }
    }
    for (uint32_t i = 0; i < compiler->sends.length; i++) {
        ta_send_record *s = &TA_VECTOR_AT(&compiler->sends, ta_send_record, i);
        if (s->event != TA_NIL) s->event = map[s->event];
    }
    for (uint32_t i = 0; i < compiler->states.length; i++) {
        ta_state_record *s = &TA_VECTOR_AT(&compiler->states, ta_state_record, i);
        if (s->done_event != TA_NIL) s->done_event = map[s->done_event];
    }
    uint32_t *code = (uint32_t *)(void *)compiler->code.data;
    for (uint32_t p = 0; p < compiler->code.length;) {
        uint32_t opcode = code[p];
        if (opcode >= TA_OPCODE_MAX_) break;
        if (opcode == TA_OPCODE_RAISE) code[p + 1] = map[code[p + 1]];
        p += ta_opcode_length[opcode];
    }
}

/* ---- emission -------------------------------------------------------------------- */

typedef struct secsrc {
    uint32_t kind;
    const ta_vector *value;
} secsrc;

int ta_compiler_emit(ta_compiler *compiler, void **out, size_t *out_length) {
    const secsrc sections[] = {
        {TA_SECTION_STRINGS, &compiler->strings},
        {TA_SECTION_CHARTS, &compiler->charts},
        {TA_SECTION_STATES, &compiler->states},
        {TA_SECTION_TRANSITIONS, &compiler->transitions},
        {TA_SECTION_U32, &compiler->u32},
        {TA_SECTION_CODE, &compiler->code},
        {TA_SECTION_EXPRESSIONS, &compiler->expressions},
        {TA_SECTION_CAPTURES, &compiler->captures},
        {TA_SECTION_SENDS, &compiler->sends},
        {TA_SECTION_INVOKES, &compiler->invokes},
        {TA_SECTION_DATA, &compiler->data},
        {TA_SECTION_EVENT_NODES, &compiler->event_nodes},
        {TA_SECTION_ACTORS, &compiler->actors},
    };
    const uint32_t nsec = (uint32_t)(sizeof sections / sizeof sections[0]);
    size_t offset = sizeof(ta_header_record) + nsec * sizeof(ta_section_record);
    offset = (offset + 7) & ~(size_t)7;
    size_t total = offset;
    for (uint32_t i = 0; i < nsec; i++)
        total =
            ((total + (size_t)sections[i].value->length * sections[i].value->element_size) + 7) &
            ~(size_t)7;
    if (total > 0x7FFFFFFFu) return 0;
    unsigned char *buffer =
        (unsigned char *)compiler->allocator->alloc(compiler->allocator->context, total, 8);
    if (!buffer) return 0;
    memset(buffer, 0, total);
    ta_header_record *header = (ta_header_record *)(void *)buffer;
    header->magic = TA_IMAGE_MAGIC;
    header->version = TA_IMAGE_VERSION;
    header->total_size = (uint32_t)total;
    header->section_count = nsec;
    header->chart_count = compiler->charts.length;
    ta_section_record *section_table =
        (ta_section_record *)(void *)(buffer + sizeof(ta_header_record));
    for (uint32_t i = 0; i < nsec; i++) {
        size_t bytes = (size_t)sections[i].value->length * sections[i].value->element_size;
        section_table[i].kind = sections[i].kind;
        section_table[i].offset = (uint32_t)offset;
        section_table[i].size = (uint32_t)bytes;
        section_table[i].count =
            sections[i].kind == TA_SECTION_STRINGS ? (uint32_t)bytes : sections[i].value->length;
        if (bytes) memcpy(buffer + offset, sections[i].value->data, bytes);
        offset = (offset + bytes + 7) & ~(size_t)7;
    }
    header->hash = ta_image_hash(buffer, (uint32_t)total);
    *out = buffer;
    *out_length = total;
    return 1;
}

/* Key for a literal document: "inline:" + text with whitespace runs collapsed
 * to one space and trimmed. Must match the runtime normalization in
 * route.c. Returns the key length, 0 if it does not fit. */
uint32_t ta_compiler_document_key(const char *text, uint32_t length, char *out, uint32_t capacity) {
    const char *pre = "inline:";
    uint32_t n = 0;
    while (pre[n] && n < capacity) {
        out[n] = pre[n];
        n++;
    }
    int in_whitespace = 1;
    for (uint32_t i = 0; i < length; i++) {
        char character = text[i];
        if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
            in_whitespace = 1;
            continue;
        }
        if (in_whitespace && n > 7) {
            if (n + 1 >= capacity) return 0;
            out[n++] = ' ';
        }
        in_whitespace = 0;
        if (n + 1 >= capacity) return 0;
        out[n++] = character;
    }
    out[n] = 0;
    return n;
}
