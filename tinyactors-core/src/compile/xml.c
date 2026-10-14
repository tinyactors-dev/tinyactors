#include "compile/xml.h"

#include <string.h>

typedef struct xml_parser {
    ta_growable_arena *arena;
    int *out_of_memory;
    const char *source;
    uint32_t length, position;
    ta_diagnostics *diagnostics;
    const char *file;
    uint32_t location_offset, location_line, location_column;
    ta_vector text;  /* scratch for text accumulation */
    ta_vector stack; /* frames */
    ta_xml_node *root;
    int failed;
    const volatile int *abort_flag;
    int *aborted;
    /* namespace scope: the in-scope default namespace, and per prefix the
     * innermost declaration (an index into `namespace_bindings`, whose entries remember the
     * declaration they shadow so closing an element restores it) */
    const char *default_namespace;
    ta_vector namespace_bindings; /* nsbind */
    ta_string_map namespace_map;  /* prefix -> namespace_bindings index */
} xml_parser;

typedef struct namespace_binding {
    const char *prefix; /* key in namespace_map (the attribute name after "xmlns:") */
    const char *uri;
    uint32_t length;
    uint32_t shadowed; /* shadowed namespace_bindings index or TA_NIL */
} namespace_binding;

typedef struct xml_frame {
    ta_xml_node *node;
    uint32_t text_start;
    uint32_t namespace_mark;       /* namespace_bindings.length when the element opened */
    const char *default_namespace; /* default namespace outside the element */
} xml_frame;

static void loc(xml_parser *parser, uint32_t offset, uint32_t *line, uint32_t *col) {
    if (offset < parser->location_offset) {
        parser->location_offset = 0;
        parser->location_line = 1;
        parser->location_column = 1;
    }
    while (parser->location_offset < offset) {
        if (parser->source[parser->location_offset] == '\n') {
            parser->location_line++;
            parser->location_column = 1;
        } else {
            parser->location_column++;
        }
        parser->location_offset++;
    }
    *line = parser->location_line;
    *col = parser->location_column;
}

static void error_at(xml_parser *parser, uint32_t offset, const char *message) {
    uint32_t l, c;
    loc(parser, offset, &l, &c);
    if (!parser->failed)
        ta_diagnostics_add(parser->diagnostics, &(ta_diagnostic_input){TA_DIAGNOSTIC_ERROR, parser->file, l, c, "XML: %s"}, message);
    parser->failed = 1;
}

/* The host's abort flag, polled per markup token and every 64 KiB of a long
 * one: once set, parsing fails (the caller reports the abort). */
static int stop(xml_parser *parser, uint32_t i) {
    if ((i & 0xFFFFu) != 0 || !parser->abort_flag || !*parser->abort_flag) return 0;
    parser->failed = 1;
    *parser->aborted = 1;
    return 1;
}

static int starts(xml_parser *parser, const char *lit) {
    size_t n = strlen(lit);
    return parser->length - parser->position >= n &&
           memcmp(parser->source + parser->position, lit, n) == 0;
}

static int find(xml_parser *parser, const char *lit) {
    size_t n = strlen(lit);
    for (uint32_t i = parser->position; i + n <= parser->length && !stop(parser, i); i++)
        if (memcmp(parser->source + i, lit, n) == 0) {
            parser->position = i + (uint32_t)n;
            return 1;
        }
    return 0;
}

static int is_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == '.' || c == ':' || (unsigned char)c >= 0x80;
}
static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_whitespace(xml_parser *parser) {
    while (parser->position < parser->length && is_space(parser->source[parser->position]))
        parser->position++;
}

static void put_utf8(ta_vector *vector, uint32_t cp) {
    char b[4];
    int n;
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    char *dst = (char *)ta_vector_push_many(vector, (uint32_t)n);
    if (dst) memcpy(dst, b, (size_t)n);
}

/* Append unescaped s[a,b) to v. */
static void unescape(xml_parser *parser, ta_vector *vector, uint32_t a, uint32_t b) {
    for (uint32_t i = a; i < b; i++) {
        if (stop(parser, i)) return;
        char c = parser->source[i];
        if (c != '&') {
            char *dst = (char *)ta_vector_push(vector);
            if (dst) *dst = c;
            continue;
        }
        uint32_t semi = i + 1;
        while (semi < b && parser->source[semi] != ';' && semi - i < 12)
            semi++;
        if (semi >= b || parser->source[semi] != ';') {
            error_at(parser, i, "unterminated entity reference");
            return;
        }
        const char *e = parser->source + i + 1;
        uint32_t el = semi - i - 1;
        uint32_t cp = 0;
        if (el == 2 && !memcmp(e, "lt", 2))
            cp = '<';
        else if (el == 2 && !memcmp(e, "gt", 2))
            cp = '>';
        else if (el == 3 && !memcmp(e, "amp", 3))
            cp = '&';
        else if (el == 4 && !memcmp(e, "quot", 4))
            cp = '"';
        else if (el == 4 && !memcmp(e, "apos", 4))
            cp = '\'';
        else if (el > 1 && e[0] == '#') {
            int hex = e[1] == 'x' || e[1] == 'X';
            for (uint32_t k = hex ? 2u : 1u; k < el; k++) {
                char d = e[k];
                uint32_t dv;
                if (d >= '0' && d <= '9')
                    dv = (uint32_t)(d - '0');
                else if (hex && d >= 'a' && d <= 'f')
                    dv = (uint32_t)(d - 'a' + 10);
                else if (hex && d >= 'A' && d <= 'F')
                    dv = (uint32_t)(d - 'A' + 10);
                else {
                    error_at(parser, i, "bad character reference");
                    return;
                }
                cp = cp * (hex ? 16u : 10u) + dv;
                if (cp > 0x10FFFF) {
                    error_at(parser, i, "character reference out of range");
                    return;
                }
            }
        } else {
            error_at(parser, i, "unknown entity");
            return;
        }
        put_utf8(vector, cp);
        i = semi;
    }
}

typedef struct vector_string_request {
    ta_vector *vector;
    uint32_t start;
    uint32_t *out_length;
} vector_string_request;

static char *finish_vector_string(xml_parser *parser, const vector_string_request *request) {
    ta_vector *vector = request->vector;
    uint32_t start = request->start;
    uint32_t n = vector->length - start;
    const char *text = vector->data ? (const char *)vector->data + start : NULL;
    char *s = ta_compile_copy(parser->arena, (ta_string_view){text, n}, parser->out_of_memory);
    *request->out_length = n;
    vector->length = start;
    return s;
}

/* Bring the element's xmlns declarations into scope and resolve its
 * namespace in O(attributes), whatever the depth. The first of duplicate
 * declarations wins, as an innermost-first ancestor walk would find it. */
static void scope_ns(xml_parser *parser, ta_xml_node *node, uint32_t mark) {
    int default_namespace_seen = 0;
    for (const ta_xml_attribute *attribute = node->attributes; attribute;
         attribute = attribute->next) {
        if (strncmp(attribute->name, "xmlns", 5) != 0) continue;
        if (attribute->name[5] == 0) {
            if (!default_namespace_seen) parser->default_namespace = attribute->value;
            default_namespace_seen = 1;
            continue;
        }
        if (attribute->name[5] != ':') continue;
        const char *pre = attribute->name + 6;
        uint32_t length = (uint32_t)strlen(pre);
        uint32_t prev = ta_string_map_get(&parser->namespace_map, pre, length);
        if (prev != TA_NIL && prev >= mark) continue; /* duplicate on this element */
        namespace_binding *bd = (namespace_binding *)ta_vector_push(&parser->namespace_bindings);
        if (!bd) return;
        bd->prefix = pre;
        bd->length = length;
        bd->uri = attribute->value;
        bd->shadowed = prev;
        ta_string_map_put(&parser->namespace_map, pre, length,
                          parser->namespace_bindings.length - 1);
    }
    if (!node->prefix) {
        node->namespace_uri = parser->default_namespace;
        return;
    }
    uint32_t i =
        ta_string_map_get(&parser->namespace_map, node->prefix, (uint32_t)strlen(node->prefix));
    if (i != TA_NIL)
        node->namespace_uri = TA_VECTOR_AT(&parser->namespace_bindings, namespace_binding, i).uri;
    else
        node->namespace_uri =
            strcmp(node->prefix, "xml") == 0 ? "http://www.w3.org/XML/1998/namespace" : "";
}

/* Leave the scope of an element: drop its declarations (innermost first). */
static void unscope_ns(xml_parser *parser, uint32_t mark, const char *default_namespace) {
    while (parser->namespace_bindings.length > mark) {
        const namespace_binding *bd = &TA_VECTOR_AT(&parser->namespace_bindings, namespace_binding,
                                                    parser->namespace_bindings.length - 1);
        ta_string_map_put(&parser->namespace_map, bd->prefix, bd->length, bd->shadowed);
        parser->namespace_bindings.length--;
    }
    parser->default_namespace = default_namespace;
}

static uint32_t parse_name(xml_parser *parser) {
    uint32_t start = parser->position;
    while (parser->position < parser->length && is_name_char(parser->source[parser->position]))
        parser->position++;
    return parser->position - start;
}

static void open_tag(xml_parser *parser) {
    uint32_t tag_offset = parser->position;
    parser->position++; /* '<' */
    uint32_t ns = parser->position, nl = parse_name(parser);
    if (!nl) {
        error_at(parser, tag_offset, "expected element name");
        return;
    }
    ta_xml_node *node = (ta_xml_node *)ta_compile_alloc(parser->arena, sizeof *node, parser->out_of_memory);
    if (!node) {
        parser->failed = 1;
        return;
    }
    loc(parser, tag_offset, &node->line, &node->column);
    char *qname = ta_compile_copy(parser->arena, (ta_string_view){parser->source + ns, (uint32_t)(nl)}, parser->out_of_memory);
    if (!qname) {
        parser->failed = 1;
        return;
    }
    char *colon = strchr(qname, ':');
    if (colon) {
        *colon = 0;
        node->prefix = qname;
        node->name = colon + 1;
    } else {
        node->name = qname;
    }
    ta_xml_attribute **tail = &node->attributes;
    for (;;) {
        skip_whitespace(parser);
        if (parser->position >= parser->length) {
            error_at(parser, tag_offset, "unterminated start tag");
            return;
        }
        char c = parser->source[parser->position];
        if (c == '/' || c == '>') break;
        uint32_t an = parser->position, al = parse_name(parser);
        if (!al) {
            error_at(parser, parser->position, "expected attribute name");
            return;
        }
        skip_whitespace(parser);
        if (parser->position >= parser->length || parser->source[parser->position] != '=') {
            error_at(parser, parser->position, "expected '=' after attribute name");
            return;
        }
        parser->position++;
        skip_whitespace(parser);
        if (parser->position >= parser->length ||
            (parser->source[parser->position] != '"' && parser->source[parser->position] != '\'')) {
            error_at(parser, parser->position, "expected quoted attribute value");
            return;
        }
        char q = parser->source[parser->position++];
        uint32_t vs = parser->position;
        while (parser->position < parser->length && parser->source[parser->position] != q)
            parser->position++;
        if (parser->position >= parser->length) {
            error_at(parser, vs, "unterminated attribute value");
            return;
        }
        uint32_t ve = parser->position++;
        ta_xml_attribute *attribute =
            (ta_xml_attribute *)ta_compile_alloc(parser->arena, sizeof *attribute, parser->out_of_memory);
        if (!attribute) {
            parser->failed = 1;
            return;
        }
        attribute->name = ta_compile_copy(parser->arena, (ta_string_view){parser->source + an, (uint32_t)(al)}, parser->out_of_memory);
        uint32_t start = parser->text.length;
        unescape(parser, &parser->text, vs, ve);
        uint32_t vl;
        attribute->value = finish_vector_string(parser, &(vector_string_request){&parser->text, start, &vl});
        attribute->value_length = vl;
        if (!attribute->name || !attribute->value) {
            parser->failed = 1;
            return;
        }
        *tail = attribute;
        tail = &attribute->next;
    }
    xml_frame *top = parser->stack.length
                         ? &TA_VECTOR_AT(&parser->stack, xml_frame, parser->stack.length - 1)
                         : NULL;
    if (top) {
        node->parent = top->node;
        if (top->node->last)
            top->node->last->next = node;
        else
            top->node->first = node;
        top->node->last = node;
    } else {
        if (parser->root) {
            error_at(parser, tag_offset, "multiple root elements");
            return;
        }
        parser->root = node;
    }
    uint32_t mark = parser->namespace_bindings.length;
    const char *outer_default_namespace = parser->default_namespace;
    scope_ns(parser, node, mark);
    if (parser->source[parser->position] == '/') {
        unscope_ns(parser, mark, outer_default_namespace);
        parser->position++;
        if (parser->position >= parser->length || parser->source[parser->position] != '>') {
            error_at(parser, parser->position, "expected '>'");
            return;
        }
        parser->position++;
        node->inner_start = node->inner_end = parser->position;
        node->text = ta_compile_copy(parser->arena, (ta_string_view){"", (uint32_t)(0)}, parser->out_of_memory);
        return;
    }
    parser->position++; /* '>' */
    node->inner_start = parser->position;
    xml_frame *f = (xml_frame *)ta_vector_push(&parser->stack);
    if (!f) {
        parser->failed = 1;
        return;
    }
    f->node = node;
    f->text_start = parser->text.length;
    f->namespace_mark = mark;
    f->default_namespace = outer_default_namespace;
}

static ta_xml_node *close_tag(xml_parser *parser) {
    uint32_t offset = parser->position;
    parser->position += 2;
    uint32_t ns = parser->position, nl = parse_name(parser);
    skip_whitespace(parser);
    if (parser->position >= parser->length || parser->source[parser->position] != '>') {
        error_at(parser, offset, "malformed end tag");
        return NULL;
    }
    parser->position++;
    if (!parser->stack.length) {
        error_at(parser, offset, "unexpected end tag");
        return NULL;
    }
    xml_frame *f = &TA_VECTOR_AT(&parser->stack, xml_frame, parser->stack.length - 1);
    ta_xml_node *node = f->node;
    size_t ql = (node->prefix ? strlen(node->prefix) + 1 : 0) + strlen(node->name);
    int match = ql == nl;
    if (match && node->prefix) {
        size_t pl = strlen(node->prefix);
        match = !memcmp(parser->source + ns, node->prefix, pl) && parser->source[ns + pl] == ':' &&
                !memcmp(parser->source + ns + pl + 1, node->name, nl - pl - 1);
    } else if (match) {
        match = !memcmp(parser->source + ns, node->name, nl);
    }
    if (!match) {
        error_at(parser, offset, "mismatched end tag");
        return NULL;
    }
    node->inner_end = offset;
    node->text = finish_vector_string(parser, &(vector_string_request){&parser->text, f->text_start, &node->text_length});
    unscope_ns(parser, f->namespace_mark, f->default_namespace);
    parser->stack.length--;
    return node;
}

ta_xml_node *ta_xml_parse(ta_growable_arena *arena, const ta_xml_parse_request *request) {
    const char *source = request->source.pointer;
    uint32_t length = request->source.length;
    xml_parser parser;
    memset(&parser, 0, sizeof parser);
    parser.arena = arena;
    parser.out_of_memory = request->out_of_memory;
    parser.source = source;
    parser.length = length;
    parser.diagnostics = request->diagnostics;
    parser.file = request->file;
    parser.location_line = parser.location_column = 1;
    parser.abort_flag = request->abort_flag;
    parser.aborted = request->aborted;
    parser.default_namespace = "";
    ta_vector_init(&parser.text, request->allocator, 1);
    ta_vector_init(&parser.stack, request->allocator, sizeof(xml_frame));
    ta_vector_init(&parser.namespace_bindings, request->allocator, sizeof(namespace_binding));
    ta_string_map_init(&parser.namespace_map, request->allocator);
    if (length >= 3 && (unsigned char)source[0] == 0xEF && (unsigned char)source[1] == 0xBB &&
        (unsigned char)source[2] == 0xBF)
        parser.position = 3;
    while (parser.position < parser.length && !parser.failed) {
        if (stop(&parser, 0)) break;
        if (parser.source[parser.position] == '<') {
            uint32_t at = parser.position;
            if (starts(&parser, "<!--")) {
                if (!find(&parser, "-->")) error_at(&parser, at, "unterminated comment");
            } else if (starts(&parser, "<![CDATA[")) {
                parser.position += 9;
                uint32_t cs = parser.position;
                if (!find(&parser, "]]>")) {
                    error_at(&parser, at, "unterminated CDATA section");
                    break;
                }
                if (!parser.stack.length) {
                    error_at(&parser, at, "CDATA outside root element");
                    break;
                }
                char *dst = (char *)ta_vector_push_many(&parser.text, parser.position - 3 - cs);
                if (dst) memcpy(dst, parser.source + cs, parser.position - 3 - cs);
            } else if (starts(&parser, "<?")) {
                if (!find(&parser, "?>"))
                    error_at(&parser, at, "unterminated processing instruction");
            } else if (starts(&parser, "<!")) {
                int depth = 0;
                while (parser.position < parser.length) {
                    char c = parser.source[parser.position++];
                    if (c == '[') depth++;
                    if (c == ']') depth--;
                    if (c == '>' && depth <= 0) break;
                }
            } else if (starts(&parser, "</")) {
                close_tag(&parser);
            } else {
                open_tag(&parser);
            }
        } else {
            uint32_t ts = parser.position;
            while (parser.position < parser.length && parser.source[parser.position] != '<')
                parser.position++;
            if (parser.stack.length) {
                unescape(&parser, &parser.text, ts, parser.position);
            } else {
                for (uint32_t i = ts; i < parser.position; i++)
                    if (!is_space(parser.source[i])) {
                        error_at(&parser, i, "text outside root element");
                        break;
                    }
            }
        }
    }
    if (!parser.failed && parser.stack.length)
        error_at(&parser, parser.length, "unexpected end of document (unclosed element)");
    if (!parser.failed && !parser.root) error_at(&parser, 0, "no root element");
    if (parser.text.out_of_memory || parser.stack.out_of_memory ||
        parser.namespace_bindings.out_of_memory || parser.namespace_map.out_of_memory ||
        *parser.out_of_memory) {
        if (!parser.failed) error_at(&parser, 0, "out of memory");
    }
    ta_vector_free(&parser.text);
    ta_vector_free(&parser.stack);
    ta_vector_free(&parser.namespace_bindings);
    ta_string_map_free(&parser.namespace_map);
    return parser.failed ? NULL : parser.root;
}

const ta_xml_attribute *ta_xml_find_attribute(const ta_xml_node *node, const char *name) {
    for (const ta_xml_attribute *attribute = node->attributes; attribute;
         attribute = attribute->next)
        if (strcmp(attribute->name, name) == 0) return attribute;
    return NULL;
}

const char *ta_xml_attribute_value(const ta_xml_node *node, const char *name) {
    const ta_xml_attribute *attribute = ta_xml_find_attribute(node, name);
    return attribute ? attribute->value : NULL;
}
