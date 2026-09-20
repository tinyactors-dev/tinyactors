/*
 * The writer: events in, bytes out. See docs/internal/writer.md.
 *
 * Every emit call is compiled synchronously into a small operation: all state
 * changes (frames, bindings, needs) happen at call time, and only the byte
 * production is deferred. An operation is a sequence of phases (optional XML
 * declaration, namespace declarations, tag close, main body), each of which
 * fills a short queue of segments; the queue runner writes segments into the
 * caller's buffer and stops with TXML_NEED_SPACE when it is full. A drain
 * resumes at the exact segment and byte, so no string is ever copied and no
 * byte is ever produced twice.
 *
 * txml_write_subtree is a walk over a document that compiles the same
 * operations one at a time; its position lives in the writer.
 *
 * Functions take at most three parameters; what does not fit travels in a
 * request struct.
 */
#include "doc.h"
#include "text.h"

#define QUEUE_MAX 16u
#define ESCAPE_MAX 16u
#define ARENA_CHUNK ((size_t)8192u)

static const char WRITER_XMLNS_URI[] = "http://www.w3.org/2000/xmlns/";
static const char WRITER_XML_URI[] = "http://www.w3.org/XML/1998/namespace";

/* ------------------------------------------------------------------------
 * Types.
 */

typedef struct wbinding {
    struct wbinding *prev;
    txml_str prefix;
    txml_str uri;
    uint32_t depth; /* the frame depth whose start tag declared it */
} wbinding;

/* A binding a start tag still has to declare. */
typedef struct need {
    struct need *next;
    txml_str prefix;
    txml_str uri;
    bool emit;
} need;

typedef struct attribute_name {
    struct attribute_name *next;
    txml_str ns;
    txml_str local;
} attribute_name;

typedef struct {
    txml_str prefix;
    txml_str local;
    wbinding *bindings; /* the binding stack before this element's declarations */
    txml_arena_mark mark;
    uint32_t subtree_end; /* txml_write_subtree only */
    bool self_close;      /* txml_write_subtree: the source was <a/> */
    bool had_child;
    bool suppress; /* an ancestor or this element received text: no indentation */
} writer_frame;

typedef enum { STATE_PROLOG, STATE_TAG, STATE_CONTENT, STATE_EPILOG } writer_state;
typedef enum { CLOSE_NONE, CLOSE_GT, CLOSE_SELF } close_mode;
typedef enum {
    OP_NONE, OP_START, OP_ATTRIBUTE, OP_TEXT, OP_CDATA, OP_COMMENT, OP_PI, OP_END, OP_DECLARATION, OP_DOCTYPE
} op_kind;
typedef enum { SEG_RAW, SEG_TEXT, SEG_ATTRIBUTE, SEG_CDATA, SEG_INDENT } segment_kind;

typedef struct {
    const uint8_t *ptr;
    uint32_t length; /* SEG_INDENT: the spaces after the newline */
    segment_kind kind;
} segment;

typedef struct {
    segment items[QUEUE_MAX];
    uint32_t count;
    uint32_t index;
    uint32_t offset;
    uint8_t escape[ESCAPE_MAX];
    uint32_t escape_length;
    uint32_t escape_offset;
} queue;

/* The operation in progress. */
typedef struct {
    op_kind kind;
    uint32_t phase;
    uint32_t main_phase;
    bool xml_declaration;
    close_mode close;
    need *cursor; /* next declaration to emit */
    int32_t indent; /* spaces after a newline, or -1 for none */
    txml_str prefix; /* names, values: stable until the operation completes */
    txml_str local;
    txml_str value;
    txml_str extra;
    bool has_attribute_declaration; /* an invented prefix declared before the attribute */
    txml_str declared_prefix;
    txml_str declared_uri;
    bool has_release;
    txml_arena_mark release;
    /* OP_DECLARATION: a document declaration node's attributes */
    txml_cursor node;
    txml_attr_cursor attribute;
    uint32_t attributes_left;
} operation;

typedef struct {
    bool active;
    const txml_doc *doc;
    uint32_t index;
    uint32_t end;
    uint32_t base_depth;
    bool in_attributes;
    txml_attr_cursor attribute;
    uint32_t attributes_left;
} walk_state;

struct txml_writer {
    txml_allocator allocator;
    txml_limits limits;
    uint32_t flags;
    txml_buffer output;
    uint32_t output_length; /* produced and not yet consumed */

    txml_arena arena;
    writer_frame *frames;
    uint32_t depth;
    wbinding *bindings;
    writer_state state;
    bool declaration_done;
    bool pending;
    bool broken;

    /* The start tag being written. */
    need *needs;
    need *needs_tail;
    attribute_name *attribute_names;
    uint32_t attribute_count;

    operation op;
    queue queue;
    walk_state walk;
};

/* ------------------------------------------------------------------------
 * Byte-level helpers.
 */

static uint32_t literal_length(const char *literal) {
    uint32_t length = 0u;
    while (literal[length] != '\0') {
        length += 1u;
    }
    return length;
}

static txml_str str_of(const char *literal) {
    return (txml_str){ literal, literal_length(literal) };
}

/* A Name without a colon: the shape of a prefix, a local part, a PI target.
 * The bytes must be well-formed UTF-8, which is checked first. */
static bool is_ncname(txml_str name) {
    const uint8_t *bytes = (const uint8_t *)name.ptr;
    uint32_t i = 0u;

    if (name.length == 0u || txml_text_first_invalid(bytes, name.length) != name.length) {
        return false;
    }
    while (i < name.length) {
        uint32_t width = 0u;
        const uint32_t code = txml_text_decode(bytes + i, &width);
        const bool ok = i == 0u ? txml_text_is_name_start(code) : txml_text_is_name_char(code);
        if (!ok || code == ':') {
            return false;
        }
        i += width;
    }
    return true;
}

static bool valid_text(txml_str text) {
    return txml_text_first_invalid((const uint8_t *)text.ptr, text.length) == text.length;
}

static bool is_xml_target(txml_str target) {
    return target.length == 3u && (target.ptr[0] | 0x20) == 'x' && (target.ptr[1] | 0x20) == 'm' && (target.ptr[2] | 0x20) == 'l';
}

static bool put(txml_writer *w, uint8_t byte) {
    if (w->output_length == w->output.capacity) {
        return false;
    }
    w->output.ptr[w->output_length] = byte;
    w->output_length += 1u;
    return true;
}

/* ------------------------------------------------------------------------
 * The segment queue.
 */

static void queue_clear(queue *q) {
    q->count = q->index = q->offset = 0u;
    q->escape_length = q->escape_offset = 0u;
}

static bool queue_pending(const queue *q) {
    return q->index < q->count || q->escape_offset < q->escape_length;
}

static void queue_add(queue *q, segment_kind kind, txml_str text) {
    q->items[q->count++] = (segment){ (const uint8_t *)text.ptr, text.length, kind };
}

static void queue_raw(queue *q, const char *literal) {
    queue_add(q, SEG_RAW, str_of(literal));
}

static void queue_name(queue *q, txml_str prefix, txml_str local) {
    if (prefix.length != 0u) {
        queue_add(q, SEG_RAW, prefix);
        queue_raw(q, ":");
    }
    queue_add(q, SEG_RAW, local);
}

static void queue_indent(queue *q, int32_t spaces) {
    if (spaces >= 0) {
        q->items[q->count++] = (segment){ NULL, (uint32_t)spaces, SEG_INDENT };
    }
}

static void escape_set(queue *q, const char *literal) {
    q->escape_length = literal_length(literal);
    q->escape_offset = 0u;
    for (uint32_t i = 0u; i < q->escape_length; i++) {
        q->escape[i] = (uint8_t)literal[i];
    }
}

static uint32_t segment_length(const segment *s) {
    return s->kind == SEG_INDENT ? s->length + 1u : s->length;
}

/* Write queued segments. TXML_NEED_SPACE leaves the queue exactly where it
 * stopped, mid-escape included. */
static txml_result queue_run(txml_writer *w) {
    queue *q = &w->queue;

    for (;;) {
        while (q->escape_offset < q->escape_length) {
            if (!put(w, q->escape[q->escape_offset])) {
                return TXML_NEED_SPACE;
            }
            q->escape_offset += 1u;
        }
        if (q->index >= q->count) {
            return TXML_OK;
        }
        const segment *s = &q->items[q->index];
        if (q->offset >= segment_length(s)) {
            q->index += 1u;
            q->offset = 0u;
            continue;
        }

        const uint8_t byte = s->kind == SEG_INDENT ? (q->offset == 0u ? (uint8_t)'\n' : (uint8_t)' ') : s->ptr[q->offset];
        switch (s->kind) {
        case SEG_RAW:
        case SEG_INDENT:
            if (!put(w, byte)) {
                return TXML_NEED_SPACE;
            }
            q->offset += 1u;
            break;
        case SEG_TEXT:
        case SEG_ATTRIBUTE: {
            const bool attribute = s->kind == SEG_ATTRIBUTE;
            q->offset += 1u;
            if (byte == '&') {
                escape_set(q, "&amp;");
            } else if (byte == '<') {
                escape_set(q, "&lt;");
            } else if (byte == '>') {
                escape_set(q, "&gt;");
            } else if (byte == '\r') {
                escape_set(q, "&#13;");
            } else if (attribute && byte == '"') {
                escape_set(q, "&quot;");
            } else if (attribute && byte == '\t') {
                escape_set(q, "&#9;");
            } else if (attribute && byte == '\n') {
                escape_set(q, "&#10;");
            } else if (!put(w, byte)) {
                q->offset -= 1u;
                return TXML_NEED_SPACE;
            }
            break;
        }
        case SEG_CDATA:
            if (byte == ']' && q->offset + 2u < s->length && s->ptr[q->offset + 1u] == ']' && s->ptr[q->offset + 2u] == '>') {
                /* "]]>" is split across two sections; they re-parse to the same characters. */
                escape_set(q, "]]]]><![CDATA[>");
                q->offset += 3u;
            } else {
                if (!put(w, byte)) {
                    return TXML_NEED_SPACE;
                }
                q->offset += 1u;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------------
 * Bindings and needs.
 */

typedef struct {
    bool bound;
    txml_str uri;
    uint32_t depth;
    const wbinding *binding;
} binding_lookup;

static binding_lookup lookup_prefix(const txml_writer *w, txml_str prefix) {
    for (const wbinding *b = w->bindings; b != NULL; b = b->prev) {
        if (txml_str_equal(b->prefix, prefix)) {
            return (binding_lookup){ true, b->uri, b->depth, b };
        }
    }
    if (txml_str_equal(prefix, TXML_STR("xml"))) {
        return (binding_lookup){ true, TXML_STR(WRITER_XML_URI), 0u, NULL };
    }
    return (binding_lookup){ false, { "", 0u }, 0u, NULL };
}

static bool copy_str(txml_writer *w, txml_str text, txml_str *copy) {
    char *bytes = text.length == 0u ? NULL : (char *)txml_arena_alloc(&w->arena, text.length, 1u);
    if (text.length != 0u && bytes == NULL) {
        return false;
    }
    for (uint32_t i = 0u; i < text.length; i++) {
        bytes[i] = text.ptr[i];
    }
    *copy = (txml_str){ bytes == NULL ? "" : bytes, text.length };
    return true;
}

typedef struct {
    txml_str prefix;
    txml_str uri;
} declaration_request;

static txml_result push_writer_binding(txml_writer *w, const declaration_request *request) {
    wbinding *b = (wbinding *)txml_arena_alloc(&w->arena, sizeof(wbinding), _Alignof(wbinding));
    if (b == NULL || !copy_str(w, request->prefix, &b->prefix) || !copy_str(w, request->uri, &b->uri)) {
        w->broken = true;
        return TXML_ERR_NOMEM;
    }
    b->prev = w->bindings;
    b->depth = w->depth;
    w->bindings = b;
    return TXML_OK;
}

/* Record that the tag being written must declare prefix -> uri unless a
 * declaration written explicitly says the same by the time it closes. */
static txml_result add_need(txml_writer *w, const declaration_request *request) {
    for (const need *n = w->needs; n != NULL; n = n->next) {
        if (txml_str_equal(n->prefix, request->prefix)) {
            return txml_str_equal(n->uri, request->uri) ? TXML_OK : TXML_ERR_INVALID;
        }
    }
    need *n = (need *)txml_arena_alloc(&w->arena, sizeof(need), _Alignof(need));
    if (n == NULL || !copy_str(w, request->prefix, &n->prefix) || !copy_str(w, request->uri, &n->uri)) {
        w->broken = true;
        return TXML_ERR_NOMEM;
    }
    n->next = NULL;
    n->emit = false;
    if (w->needs_tail == NULL) {
        w->needs = n;
    } else {
        w->needs_tail->next = n;
    }
    w->needs_tail = n;
    return TXML_OK;
}

/* A need whose binding is already in scope is a no-op. */
static txml_result require_binding(txml_writer *w, const declaration_request *request) {
    const binding_lookup found = lookup_prefix(w, request->prefix);
    if (found.bound && txml_str_equal(found.uri, request->uri)) {
        return TXML_OK;
    }
    if (found.bound && found.depth == w->depth && w->depth != 0u && found.binding != NULL) {
        return TXML_ERR_INVALID; /* the same tag already binds it differently */
    }
    return add_need(w, request);
}

/* The innermost, unshadowed binding of `uri` to a non-default prefix (or, if
 * allowed, the default one). */
typedef struct {
    txml_str uri;
    bool allow_default;
} prefix_query;

static bool find_prefix_for(const txml_writer *w, const prefix_query *query, txml_str *prefix) {
    const txml_str uri = query->uri;
    const bool allow_default = query->allow_default;
    for (const wbinding *b = w->bindings; b != NULL; b = b->prev) {
        if (!txml_str_equal(b->uri, uri) || (b->prefix.length == 0u && !allow_default)) {
            continue;
        }
        if (lookup_prefix(w, b->prefix).binding == b) {
            *prefix = b->prefix;
            return true;
        }
    }
    for (const need *n = w->needs; n != NULL; n = n->next) {
        if (txml_str_equal(n->uri, uri) && (n->prefix.length != 0u || allow_default)) {
            *prefix = n->prefix;
            return true;
        }
    }
    return false;
}

static bool prefix_in_use(const txml_writer *w, txml_str prefix) {
    if (lookup_prefix(w, prefix).bound) {
        return true;
    }
    for (const need *n = w->needs; n != NULL; n = n->next) {
        if (txml_str_equal(n->prefix, prefix)) {
            return true;
        }
    }
    return false;
}

/* nsN, the first one nothing in scope or pending uses. */
static bool invent_prefix(txml_writer *w, txml_str *prefix) {
    char digits[12];
    for (uint32_t n = 1u;; n++) {
        uint32_t length = 0u;
        for (uint32_t v = n; v != 0u; v /= 10u) {
            digits[length++] = (char)('0' + v % 10u);
        }
        char *name = (char *)txml_arena_alloc(&w->arena, 2u + length, 1u);
        if (name == NULL) {
            return false;
        }
        name[0] = 'n';
        name[1] = 's';
        for (uint32_t i = 0u; i < length; i++) {
            name[2u + i] = digits[length - 1u - i];
        }
        const txml_str candidate = { name, 2u + length };
        if (!prefix_in_use(w, candidate)) {
            *prefix = candidate;
            return true;
        }
    }
}

/* ------------------------------------------------------------------------
 * Operations.
 */

static void op_begin(txml_writer *w, op_kind kind) {
    txml_text_zero(&w->op, sizeof(w->op));
    w->op.kind = kind;
    w->op.indent = -1;
    queue_clear(&w->queue);
}

static void op_declaration_if_needed(txml_writer *w) {
    if ((w->flags & TXML_WRITE_DECLARATION) != 0u && !w->declaration_done) {
        w->declaration_done = true;
        w->op.xml_declaration = true;
    }
}

/* Decide which needs the tag being closed must emit, and bind them. */
static txml_result settle_tag(txml_writer *w, close_mode mode) {
    if (w->state != STATE_TAG) {
        return TXML_OK;
    }
    for (need *n = w->needs; n != NULL; n = n->next) {
        const binding_lookup found = lookup_prefix(w, n->prefix);
        if (found.bound && txml_str_equal(found.uri, n->uri)) {
            continue;
        }
        if (found.bound && found.binding != NULL && found.depth == w->depth) {
            return TXML_ERR_INVALID;
        }
        if (n->prefix.length == 0u && n->uri.length == 0u && !found.bound) {
            continue; /* undeclaring a default that is not there */
        }
        n->emit = true;
        const txml_result pushed = push_writer_binding(w, &(declaration_request){ n->prefix, n->uri });
        if (pushed != TXML_OK) {
            return pushed;
        }
    }
    w->op.cursor = w->needs;
    w->op.close = mode;
    w->needs = w->needs_tail = NULL;
    w->state = STATE_CONTENT;
    return TXML_OK;
}

static void tag_reset(txml_writer *w) {
    w->needs = w->needs_tail = NULL;
    w->attribute_names = NULL;
    w->attribute_count = 0u;
}

typedef struct {
    txml_str ns;
    txml_str local;
    txml_str prefix;
    bool exact; /* the prefix is authoritative: an empty one means unprefixed */
} name_spec;

static bool ns_is_xmlns(txml_str ns) {
    return txml_str_equal(ns, TXML_STR(WRITER_XMLNS_URI));
}

/* Checks that need no state. */
static txml_result validate_name(const name_spec *name, bool trusted) {
    if (trusted) {
        return TXML_OK;
    }
    if (!is_ncname(name->local) || !valid_text(name->ns)) {
        return TXML_ERR_INVALID;
    }
    if (name->prefix.length != 0u) {
        if (!is_ncname(name->prefix) || name->ns.length == 0u || txml_str_equal(name->prefix, TXML_STR("xmlns"))) {
            return TXML_ERR_INVALID;
        }
        if (txml_str_equal(name->prefix, TXML_STR("xml")) != txml_str_equal(name->ns, TXML_STR(WRITER_XML_URI))) {
            return TXML_ERR_INVALID;
        }
    }
    return TXML_OK;
}

/* Choose the prefix an element is written with, recording what it needs. */
static txml_result plan_element(txml_writer *w, const name_spec *name, txml_str *prefix) {
    *prefix = name->prefix;

    if (name->prefix.length != 0u) {
        return txml_str_equal(name->prefix, TXML_STR("xml")) ? TXML_OK : require_binding(w, &(declaration_request){ name->prefix, name->ns });
    }
    if (name->ns.length == 0u) {
        /* Unqualified: undeclare a default that would otherwise capture it. */
        const binding_lookup found = lookup_prefix(w, (txml_str){ "", 0u });
        if (found.bound && found.uri.length != 0u) {
            return add_need(w, &(declaration_request){ { "", 0u }, { "", 0u } });
        }
        return TXML_OK;
    }
    if (name->exact) {
        return require_binding(w, &(declaration_request){ { "", 0u }, name->ns });
    }
    if (txml_str_equal(name->ns, TXML_STR(WRITER_XML_URI))) {
        *prefix = TXML_STR("xml");
        return TXML_OK;
    }
    if (find_prefix_for(w, &(prefix_query){ name->ns, true }, prefix)) {
        return TXML_OK;
    }
    const binding_lookup found = lookup_prefix(w, (txml_str){ "", 0u });
    if (!found.bound || found.uri.length == 0u) {
        *prefix = (txml_str){ "", 0u };
        return add_need(w, &(declaration_request){ { "", 0u }, name->ns });
    }
    if (!invent_prefix(w, prefix)) {
        w->broken = true;
        return TXML_ERR_NOMEM;
    }
    return add_need(w, &(declaration_request){ *prefix, name->ns });
}

/* ------------------------------------------------------------------------
 * Emit-call compilation.
 */

typedef struct {
    name_spec name;
    uint32_t subtree_end;
    bool self_close;
    bool trusted;
} start_request;

static txml_result check_output_ready(const txml_writer *w) {
    return w == NULL || w->pending || w->broken || w->op.kind != OP_NONE ? TXML_ERR_INVALID : TXML_OK;
}

static int32_t indent_for(const txml_writer *w) {
    if ((w->flags & TXML_WRITE_INDENT) == 0u || w->depth == 0u || w->frames[w->depth - 1u].suppress) {
        return -1;
    }
    return (int32_t)(2u * w->depth);
}

static void mark_child(txml_writer *w) {
    if (w->depth != 0u) {
        w->frames[w->depth - 1u].had_child = true;
    }
}

static txml_result compile_start(txml_writer *w, const start_request *request) {
    txml_result result = validate_name(&request->name, request->trusted);
    if (result != TXML_OK) {
        return result;
    }
    if (w->state == STATE_EPILOG) {
        return TXML_ERR_INVALID;
    }
    if (w->depth >= w->limits.max_depth ||
        (uint64_t)request->name.prefix.length + request->name.local.length + 1u > w->limits.max_name_length) {
        return TXML_ERR_LIMIT;
    }

    op_begin(w, OP_START);
    op_declaration_if_needed(w);
    result = settle_tag(w, CLOSE_GT);
    if (result != TXML_OK) {
        return result;
    }
    w->op.indent = indent_for(w);
    const bool suppress = w->depth != 0u && w->frames[w->depth - 1u].suppress;
    mark_child(w);

    /* The new tag's declarations belong to the new depth. */
    const txml_arena_mark mark = txml_arena_position(&w->arena);
    writer_frame *f = &w->frames[w->depth];
    f->bindings = w->bindings;
    f->mark = mark;
    tag_reset(w);
    w->depth += 1u;

    txml_str prefix = { "", 0u };
    result = plan_element(w, &request->name, &prefix);
    if (result == TXML_OK && (!copy_str(w, prefix, &f->prefix) || !copy_str(w, request->name.local, &f->local))) {
        result = TXML_ERR_NOMEM;
    }
    if (result != TXML_OK) {
        w->broken = true; /* the previous tag is already settled: not recoverable */
        return result;
    }
    f->subtree_end = request->subtree_end;
    f->self_close = request->self_close;
    f->had_child = false;
    f->suppress = suppress;
    w->state = STATE_TAG;
    w->op.prefix = f->prefix;
    w->op.local = f->local;
    return TXML_OK;
}

typedef struct {
    name_spec name;
    txml_str value;
    bool trusted;
} attribute_request;

static txml_result compile_attribute(txml_writer *w, const attribute_request *request) {
    if (w->state != STATE_TAG) {
        return TXML_ERR_INVALID;
    }
    const name_spec *name = &request->name;
    const bool declaration = ns_is_xmlns(name->ns);
    txml_result result = TXML_OK;

    if (!request->trusted && (!valid_text(request->value) || !is_ncname(name->local) || !valid_text(name->ns))) {
        return TXML_ERR_INVALID;
    }
    if (!declaration) {
        name_spec checked = *name;
        result = validate_name(&checked, request->trusted);
        if (result != TXML_OK) {
            return result;
        }
    }
    if (w->attribute_count >= w->limits.max_attributes) {
        return TXML_ERR_LIMIT;
    }
    const bool default_declaration = declaration && txml_str_equal(name->local, TXML_STR("xmlns"));
    if (declaration && !default_declaration && request->value.length == 0u) {
        return TXML_ERR_INVALID;
    }
    for (const attribute_name *n = w->attribute_names; n != NULL; n = n->next) {
        if (txml_str_equal(n->ns, name->ns) && txml_str_equal(n->local, name->local)) {
            return TXML_ERR_INVALID; /* the same expanded name twice on one tag */
        }
    }

    op_begin(w, OP_ATTRIBUTE);
    txml_str prefix = name->prefix;
    txml_str invented = { "", 0u };
    bool invent = false;

    if (declaration) {
        prefix = default_declaration ? (txml_str){ "", 0u } : TXML_STR("xmlns");
    } else if (name->ns.length == 0u) {
        prefix = (txml_str){ "", 0u };
    } else if (name->prefix.length != 0u) {
        if (!txml_str_equal(name->prefix, TXML_STR("xml"))) {
            result = require_binding(w, &(declaration_request){ name->prefix, name->ns });
        }
    } else if (txml_str_equal(name->ns, TXML_STR(WRITER_XML_URI))) {
        prefix = TXML_STR("xml");
    } else if (!find_prefix_for(w, &(prefix_query){ name->ns, false }, &prefix)) {
        /* An attribute cannot use the default namespace: it needs a prefix now. */
        invent = invent_prefix(w, &invented);
        if (!invent) {
            w->broken = true;
            return TXML_ERR_NOMEM;
        }
        prefix = invented;
    }
    if (result != TXML_OK) {
        return result;
    }

    attribute_name *entry = (attribute_name *)txml_arena_alloc(&w->arena, sizeof(attribute_name), _Alignof(attribute_name));
    if (entry == NULL || !copy_str(w, name->ns, &entry->ns) || !copy_str(w, name->local, &entry->local)) {
        w->broken = true;
        return TXML_ERR_NOMEM;
    }
    entry->next = w->attribute_names;
    w->attribute_names = entry;
    w->attribute_count += 1u;

    if (declaration) {
        result = push_writer_binding(w, &(declaration_request){ default_declaration ? (txml_str){ "", 0u } : name->local, request->value });
        w->op.prefix = prefix;
    } else {
        if (invent) {
            result = push_writer_binding(w, &(declaration_request){ invented, name->ns });
            w->op.has_attribute_declaration = true;
            w->op.declared_prefix = invented;
            w->op.declared_uri = name->ns;
            /* The invented prefix is also an expanded-name entry on this tag. */
            attribute_name *declared = (attribute_name *)txml_arena_alloc(&w->arena, sizeof(attribute_name), _Alignof(attribute_name));
            if (declared == NULL || !copy_str(w, TXML_STR(WRITER_XMLNS_URI), &declared->ns) || !copy_str(w, invented, &declared->local)) {
                w->broken = true;
                return TXML_ERR_NOMEM;
            }
            declared->next = w->attribute_names;
            w->attribute_names = declared;
        }
        w->op.prefix = prefix;
    }
    if (result == TXML_OK && !copy_str(w, w->op.prefix, &w->op.prefix)) {
        w->broken = true;
        return TXML_ERR_NOMEM;
    }
    w->op.local = entry->local;
    w->op.value = request->value;
    return result;
}

typedef struct {
    txml_str text;
    op_kind kind; /* OP_TEXT or OP_CDATA */
    bool trusted;
} text_request;

static txml_result compile_text(txml_writer *w, const text_request *request) {
    if (!request->trusted) {
        if (!valid_text(request->text)) {
            return TXML_ERR_INVALID;
        }
        if (w->state == STATE_PROLOG || w->state == STATE_EPILOG) {
            return TXML_ERR_INVALID;
        }
    }
    if (request->kind == OP_TEXT && request->text.length == 0u) {
        return TXML_OK; /* nothing to write, and it must not close the tag */
    }
    if (request->kind == OP_CDATA && (w->state == STATE_PROLOG || w->state == STATE_EPILOG)) {
        return TXML_ERR_INVALID;
    }

    op_begin(w, request->kind);
    op_declaration_if_needed(w);
    const txml_result result = settle_tag(w, CLOSE_GT);
    if (result != TXML_OK) {
        return result;
    }
    if (w->depth != 0u) {
        w->frames[w->depth - 1u].suppress = true;
    }
    w->op.value = request->text;
    return TXML_OK;
}

typedef struct {
    txml_str first;  /* the comment text, or the PI target */
    txml_str second; /* the PI data */
    op_kind kind;    /* OP_COMMENT or OP_PI */
    bool trusted;
} leaf_request;

static txml_result compile_leaf(txml_writer *w, const leaf_request *request) {
    if (!request->trusted) {
        if (!valid_text(request->first) || !valid_text(request->second)) {
            return TXML_ERR_INVALID;
        }
        if (request->kind == OP_COMMENT) {
            const txml_str text = request->first;
            if (text.length != 0u && text.ptr[text.length - 1u] == '-') {
                return TXML_ERR_INVALID;
            }
            for (uint32_t i = 0u; i + 1u < text.length; i++) {
                if (text.ptr[i] == '-' && text.ptr[i + 1u] == '-') {
                    return TXML_ERR_INVALID;
                }
            }
        } else {
            if (!is_ncname(request->first) || is_xml_target(request->first)) {
                return TXML_ERR_INVALID;
            }
            for (uint32_t i = 0u; i + 1u < request->second.length; i++) {
                if (request->second.ptr[i] == '?' && request->second.ptr[i + 1u] == '>') {
                    return TXML_ERR_INVALID;
                }
            }
        }
    }

    op_begin(w, request->kind);
    op_declaration_if_needed(w);
    const txml_result result = settle_tag(w, CLOSE_GT);
    if (result != TXML_OK) {
        return result;
    }
    w->op.indent = indent_for(w);
    mark_child(w);
    w->op.value = request->first;
    w->op.extra = request->second;
    return TXML_OK;
}

typedef struct {
    bool walking;
    bool self_close;
} end_request;

static txml_result compile_end(txml_writer *w, const end_request *request) {
    if (w->depth == 0u) {
        return TXML_ERR_INVALID;
    }
    op_begin(w, OP_END);
    const writer_frame *top = &w->frames[w->depth - 1u];
    bool self = false;

    if (w->state == STATE_TAG) {
        self = request->walking ? request->self_close : (w->flags & TXML_WRITE_EMPTY_TAGS) != 0u;
    }
    const txml_result result = settle_tag(w, self ? CLOSE_SELF : CLOSE_GT);
    if (result != TXML_OK) {
        return result;
    }
    w->op.prefix = top->prefix;
    w->op.local = top->local;
    w->op.has_release = true;
    w->op.release = top->mark;
    w->bindings = top->bindings;
    w->depth -= 1u;
    if (!self) {
        w->op.main_phase = 0u;
        w->op.indent = top->had_child && !top->suppress && (w->flags & TXML_WRITE_INDENT) != 0u ? (int32_t)(2u * w->depth) : -1;
    } else {
        w->op.main_phase = 1u; /* nothing to write after "/>" */
    }
    w->state = w->depth == 0u ? STATE_EPILOG : STATE_CONTENT;
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * The operation stepper.
 */

static void queue_declaration(txml_writer *w, const need *n) {
    queue *q = &w->queue;
    if (n->prefix.length == 0u) {
        queue_raw(q, " xmlns=\"");
    } else {
        queue_raw(q, " xmlns:");
        queue_add(q, SEG_RAW, n->prefix);
        queue_raw(q, "=\"");
    }
    queue_add(q, SEG_ATTRIBUTE, n->uri);
    queue_raw(q, "\"");
}

/* Fill the queue with the operation's main body, phase by phase. Returns
 * true when the body is finished. */
static bool step_main(txml_writer *w) {
    queue *q = &w->queue;
    operation *op = &w->op;
    const uint32_t phase = op->main_phase++;

    switch (op->kind) {
    case OP_START:
        queue_indent(q, op->indent);
        queue_raw(q, "<");
        queue_name(q, op->prefix, op->local);
        (void)phase;
        return true;
    case OP_ATTRIBUTE:
        if (phase == 0u && op->has_attribute_declaration) {
            queue_raw(q, " xmlns:");
            queue_add(q, SEG_RAW, op->declared_prefix);
            queue_raw(q, "=\"");
            queue_add(q, SEG_ATTRIBUTE, op->declared_uri);
            queue_raw(q, "\"");
            return false;
        }
        queue_raw(q, " ");
        queue_name(q, op->prefix, op->local);
        queue_raw(q, "=\"");
        queue_add(q, SEG_ATTRIBUTE, op->value);
        queue_raw(q, "\"");
        return true;
    case OP_TEXT:
        queue_add(q, SEG_TEXT, op->value);
        return true;
    case OP_CDATA:
        queue_raw(q, "<![CDATA[");
        queue_add(q, SEG_CDATA, op->value);
        queue_raw(q, "]]>");
        return true;
    case OP_COMMENT:
        queue_indent(q, op->indent);
        queue_raw(q, "<!--");
        queue_add(q, SEG_RAW, op->value);
        queue_raw(q, "-->");
        return true;
    case OP_PI:
        queue_indent(q, op->indent);
        queue_raw(q, "<?");
        queue_add(q, SEG_RAW, op->value);
        if (op->extra.length != 0u) {
            queue_raw(q, " ");
            queue_add(q, SEG_RAW, op->extra);
        }
        queue_raw(q, "?>");
        return true;
    case OP_END:
        if (phase == 0u) {
            queue_indent(q, op->indent);
            queue_raw(q, "</");
            queue_name(q, op->prefix, op->local);
            queue_raw(q, ">");
        }
        return true;
    case OP_DOCTYPE:
        queue_raw(q, "<!DOCTYPE");
        queue_add(q, SEG_RAW, op->value);
        queue_raw(q, ">");
        return true;
    case OP_DECLARATION:
        if (phase == 0u) {
            queue_raw(q, "<?xml");
            return false;
        }
        if (op->attributes_left == 0u) {
            queue_raw(q, "?>");
            return true;
        }
        {
            txml_attribute_view view;
            txml_qname name = { { "", 0u }, { "", 0u } };
            if (txml_attribute_read(op->attribute, &view) == TXML_OK) {
                txml_doc_name_at(op->attribute.doc, view.name, &name);
            }
            queue_raw(q, " ");
            queue_add(q, SEG_RAW, name.local);
            queue_raw(q, "=\"");
            queue_add(q, SEG_ATTRIBUTE, view.value);
            queue_raw(q, "\"");
            op->attributes_left -= 1u;
            if (op->attributes_left != 0u) {
                txml_next_attribute(&op->attribute);
            }
        }
        return false;
    case OP_NONE:
        break;
    }
    return true;
}

/* Called with an empty queue: fill it with the next piece of the operation, or
 * finish the operation. */
static void step_op(txml_writer *w) {
    operation *op = &w->op;

    switch (op->phase) {
    case 0:
        op->phase = 1u;
        if (op->xml_declaration) {
            queue_raw(&w->queue, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
            if ((w->flags & TXML_WRITE_INDENT) != 0u) {
                queue_raw(&w->queue, "\n");
            }
        }
        return;
    case 1:
        while (op->cursor != NULL) {
            const need *n = op->cursor;
            op->cursor = n->next;
            if (n->emit) {
                queue_declaration(w, n);
                return;
            }
        }
        op->phase = 2u;
        return;
    case 2:
        op->phase = 3u;
        if (op->close == CLOSE_GT) {
            queue_raw(&w->queue, ">");
        } else if (op->close == CLOSE_SELF) {
            queue_raw(&w->queue, "/>");
        }
        return;
    case 3:
        if (step_main(w)) {
            op->phase = 4u;
        }
        return;
    default:
        if (op->has_release) {
            txml_arena_release(&w->arena, op->release);
        }
        op->kind = OP_NONE;
        return;
    }
}

/* ------------------------------------------------------------------------
 * The subtree walk.
 */

static txml_result walk_element(txml_writer *w, txml_cursor node) {
    txml_qname name;
    const uint32_t id = txml_node_name_id(node);
    if (txml_doc_name_at(node.doc, id, &name) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    const txml_result result = compile_start(w, &(start_request){
        { name.ns, name.local, txml_node_prefix(node), true }, txml_subtree_end(node),
        (txml_flags(node) & TXML_NODE_EMPTY) != 0u, true });
    if (result != TXML_OK) {
        return result;
    }
    w->walk.attributes_left = txml_attribute_count(node);
    w->walk.in_attributes = true;
    if (w->walk.attributes_left != 0u) {
        txml_first_attribute(node, &w->walk.attribute);
    }
    return TXML_OK;
}

static txml_result walk_attribute(txml_writer *w) {
    walk_state *walk = &w->walk;
    txml_attribute_view view;
    txml_qname name;

    if (txml_attribute_read(walk->attribute, &view) != TXML_OK || txml_doc_name_at(walk->doc, view.name, &name) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    walk->attributes_left -= 1u;
    if (walk->attributes_left != 0u) {
        txml_next_attribute(&walk->attribute);
    }
    if ((view.flags & TXML_ATTR_DEFAULTED) != 0u) {
        return TXML_OK; /* supplied by the DTD, not written */
    }
    return compile_attribute(w, &(attribute_request){ { name.ns, name.local, view.prefix, true }, view.value, true });
}

static txml_result walk_node(txml_writer *w) {
    walk_state *walk = &w->walk;
    const txml_cursor node = txml_cursor_at(walk->doc, walk->index);
    const txml_node_kind kind = txml_kind(node);
    const txml_str text = txml_node_text(node);

    walk->index += 1u;
    switch (kind) {
    case TXML_NODE_DOCUMENT:
        return TXML_OK;
    case TXML_NODE_ELEMENT:
        return walk_element(w, node);
    case TXML_NODE_TEXT:
        return compile_text(w, &(text_request){ text, (txml_flags(node) & TXML_NODE_CDATA) != 0u ? OP_CDATA : OP_TEXT, true });
    case TXML_NODE_COMMENT:
        return compile_leaf(w, &(leaf_request){ text, { "", 0u }, OP_COMMENT, true });
    case TXML_NODE_PI: {
        txml_qname target;
        if (txml_doc_name_at(walk->doc, txml_node_name_id(node), &target) != TXML_OK) {
            return TXML_ERR_INVALID;
        }
        return compile_leaf(w, &(leaf_request){ target.local, text, OP_PI, true });
    }
    case TXML_NODE_DECLARATION:
        if (w->state != STATE_PROLOG || w->depth != 0u) {
            return TXML_ERR_INVALID;
        }
        op_begin(w, OP_DECLARATION);
        w->declaration_done = true;
        w->op.attributes_left = txml_attribute_count(node);
        if (w->op.attributes_left != 0u) {
            txml_first_attribute(node, &w->op.attribute);
        }
        return TXML_OK;
    case TXML_NODE_DOCTYPE:
        if (w->state != STATE_PROLOG || w->depth != 0u) {
            return TXML_ERR_INVALID;
        }
        op_begin(w, OP_DOCTYPE);
        op_declaration_if_needed(w);
        w->op.value = text;
        return TXML_OK;
    }
    return TXML_ERR_INVALID;
}

static txml_result walk_step(txml_writer *w) {
    walk_state *walk = &w->walk;

    if (walk->in_attributes) {
        if (walk->attributes_left != 0u) {
            return walk_attribute(w);
        }
        walk->in_attributes = false;
    }
    if (w->depth > walk->base_depth && w->frames[w->depth - 1u].subtree_end == walk->index) {
        return compile_end(w, &(end_request){ true, w->frames[w->depth - 1u].self_close });
    }
    if (walk->index >= walk->end) {
        walk->active = false;
        return TXML_OK;
    }
    return walk_node(w);
}

/* ------------------------------------------------------------------------
 * Driving.
 */

static txml_result advance(txml_writer *w) {
    for (;;) {
        if (queue_pending(&w->queue)) {
            if (queue_run(w) == TXML_NEED_SPACE) {
                w->pending = true;
                return TXML_NEED_SPACE;
            }
        }
        if (w->op.kind != OP_NONE) {
            queue_clear(&w->queue);
            step_op(w);
            continue;
        }
        if (w->walk.active) {
            const txml_result result = walk_step(w);
            if (result != TXML_OK) {
                w->walk.active = false;
                w->op.kind = OP_NONE;
                queue_clear(&w->queue);
                w->pending = false;
                return result;
            }
            continue;
        }
        w->pending = false;
        return TXML_OK;
    }
}

/* Run a freshly compiled operation, turning a compile failure into the result. */
static txml_result run(txml_writer *w, txml_result compiled) {
    if (compiled != TXML_OK) {
        w->op.kind = OP_NONE; /* a refused call leaves nothing pending */
        queue_clear(&w->queue);
        return compiled;
    }
    return advance(w);
}

/* ------------------------------------------------------------------------
 * Public API.
 */

txml_result txml_writer_init(txml_writer **out, const txml_writer_config *config) {
    if (out == NULL || config == NULL || config->allocator.v == NULL || config->allocator.v->alloc == NULL ||
        config->allocator.v->free == NULL || config->output.ptr == NULL || config->output.capacity == 0u ||
        config->limits.max_depth == 0u || config->limits.max_depth > 65535u || config->limits.max_name_length == 0u) {
        return TXML_ERR_INVALID;
    }
    txml_writer *w = (txml_writer *)config->allocator.v->alloc(config->allocator.self, sizeof(txml_writer), _Alignof(txml_writer));
    if (w == NULL) {
        return TXML_ERR_NOMEM;
    }
    w->allocator = config->allocator;
    w->limits = config->limits;
    w->flags = config->flags;
    w->output = config->output;
    w->output_length = 0u;
    w->frames = NULL;
    w->depth = 0u;
    w->bindings = NULL;
    w->state = STATE_PROLOG;
    w->declaration_done = false;
    w->pending = false;
    w->broken = false;
    tag_reset(w);
    txml_text_zero(&w->op, sizeof(w->op));
    queue_clear(&w->queue);
    txml_text_zero(&w->walk, sizeof(w->walk));
    w->arena.head = NULL;

    txml_result result = txml_arena_init(&w->arena, config->allocator, ARENA_CHUNK);
    if (result == TXML_OK) {
        w->frames = (writer_frame *)txml_arena_alloc(&w->arena, (size_t)config->limits.max_depth * sizeof(writer_frame), _Alignof(writer_frame));
        if (w->frames == NULL) {
            result = TXML_ERR_NOMEM;
        }
    }
    if (result != TXML_OK) {
        txml_writer_destroy(w);
        return result;
    }
    *out = w;
    return TXML_OK;
}

txml_result txml_write_start(txml_writer *w, const txml_write_name *name) {
    if (check_output_ready(w) != TXML_OK || name == NULL) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_start(w, &(start_request){ { name->ns, name->local, name->prefix, false }, TXML_INDEX_NONE, false, false }));
}

txml_result txml_write_attribute(txml_writer *w, const txml_write_attr *attribute) {
    if (check_output_ready(w) != TXML_OK || attribute == NULL) {
        return TXML_ERR_INVALID;
    }
    const txml_write_name *name = &attribute->name;
    return run(w, compile_attribute(w, &(attribute_request){ { name->ns, name->local, name->prefix, false }, attribute->value, false }));
}

txml_result txml_write_text(txml_writer *w, txml_str text) {
    if (check_output_ready(w) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_text(w, &(text_request){ text, OP_TEXT, false }));
}

txml_result txml_write_cdata(txml_writer *w, txml_str text) {
    if (check_output_ready(w) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_text(w, &(text_request){ text, OP_CDATA, false }));
}

txml_result txml_write_comment(txml_writer *w, txml_str text) {
    if (check_output_ready(w) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_leaf(w, &(leaf_request){ text, { "", 0u }, OP_COMMENT, false }));
}

txml_result txml_write_pi(txml_writer *w, txml_str target, txml_str data) {
    if (check_output_ready(w) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_leaf(w, &(leaf_request){ target, data, OP_PI, false }));
}

txml_result txml_write_end(txml_writer *w) {
    if (check_output_ready(w) != TXML_OK) {
        return TXML_ERR_INVALID;
    }
    return run(w, compile_end(w, &(end_request){ false, false }));
}

txml_result txml_write_subtree(txml_writer *w, txml_cursor cursor) {
    if (check_output_ready(w) != TXML_OK || w->walk.active) {
        return TXML_ERR_INVALID;
    }
    const uint32_t end = txml_subtree_end(cursor);
    if (cursor.doc == NULL || cursor.node >= txml_doc_node_count(cursor.doc) || end == TXML_INDEX_NONE) {
        return TXML_ERR_INVALID; /* absent, or still open: no bound to walk to */
    }
    if (w->state == STATE_EPILOG) {
        return TXML_ERR_INVALID;
    }
    /* A document that carries its own declaration never gets a second one. */
    if (txml_kind(cursor) == TXML_NODE_DOCUMENT && cursor.node + 1u < end &&
        txml_kind(txml_cursor_at(cursor.doc, cursor.node + 1u)) == TXML_NODE_DECLARATION) {
        w->declaration_done = true;
    }
    w->walk = (walk_state){ .active = true, .doc = cursor.doc, .index = cursor.node, .end = end, .base_depth = w->depth };
    return advance(w);
}

txml_bytes txml_writer_output(const txml_writer *w) {
    if (w == NULL) {
        return (txml_bytes){ NULL, 0u };
    }
    return (txml_bytes){ w->output.ptr, w->output_length };
}

txml_result txml_writer_drain(txml_writer *w) {
    if (w == NULL) {
        return TXML_ERR_INVALID;
    }
    w->output_length = 0u;
    if (!w->pending) {
        return TXML_OK;
    }
    return advance(w);
}

txml_result txml_writer_finish(txml_writer *w) {
    if (w == NULL || w->pending || w->broken || w->op.kind != OP_NONE || w->walk.active || w->depth != 0u) {
        return TXML_ERR_INVALID;
    }
    return TXML_OK;
}

void txml_writer_destroy(txml_writer *w) {
    if (w == NULL) {
        return;
    }
    const txml_allocator allocator = w->allocator;
    txml_arena_destroy(&w->arena);
    txml_allocator_free_params params = { .ptr = w, .size = sizeof(txml_writer), .alignment = _Alignof(txml_writer) };
    allocator.v->free(allocator.self, &params);
}
