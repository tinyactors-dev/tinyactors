/*
 * The data-driven parser suite: the .spec corpus in tests/spec.
 *
 * A spec case is an XML input plus the exact rendering of the document it must
 * produce. This program is the only thing that knows how a document renders,
 * so the renderer below and docs/internal/testing.md are the two halves of one
 * format: change one, change the other.
 *
 * Every case is also run through a chunk sweep — the same input fed 1, 2, 3, 7
 * and all bytes at a time — and every rendering must be byte-identical. That
 * property is what catches boundary bugs in the staging window, in name
 * assembly, and in text normalization.
 *
 * Test programs may use the C library. The library itself may not.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/txml.h"

static int failures;
static int cases_run;
static bool verbose;

/* ------------------------------------------------------------------------
 * A growable byte buffer.
 */

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} buffer;

static void buffer_reserve(buffer *b, size_t extra) {
    if (b->length + extra + 1u <= b->capacity) {
        return;
    }
    size_t capacity = b->capacity == 0u ? 256u : b->capacity;
    while (capacity < b->length + extra + 1u) {
        capacity *= 2u;
    }
    char *data = (char *)realloc(b->data, capacity);
    if (data == NULL) {
        fprintf(stderr, "spec_test: out of memory\n");
        exit(2);
    }
    b->data = data;
    b->capacity = capacity;
}

static void buffer_put(buffer *b, const char *bytes, size_t length) {
    buffer_reserve(b, length);
    memcpy(b->data + b->length, bytes, length);
    b->length += length;
    b->data[b->length] = '\0';
}

static void buffer_putc(buffer *b, char c) { buffer_put(b, &c, 1u); }
static void buffer_puts(buffer *b, const char *text) { buffer_put(b, text, strlen(text)); }
static void buffer_put_str(buffer *b, txml_str text) { buffer_put(b, text.ptr, text.length); }

static void buffer_printf(buffer *b, const char *format, ...) {
    char scratch[512];
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(scratch, sizeof(scratch), format, arguments);
    va_end(arguments);
    if (written < 0) {
        return;
    }
    buffer_put(b, scratch, (size_t)written);
}

static void buffer_clear(buffer *b) {
    b->length = 0u;
    if (b->data != NULL) {
        b->data[0] = '\0';
    }
}

static void buffer_free(buffer *b) {
    free(b->data);
    b->data = NULL;
    b->length = 0u;
    b->capacity = 0u;
}

static const char *buffer_text(const buffer *b) { return b->data == NULL ? "" : b->data; }

static bool buffer_equal(const buffer *b, const char *text) {
    size_t length = strlen(text);
    return b->length == length && memcmp(buffer_text(b), text, length) == 0;
}

/* ------------------------------------------------------------------------
 * Escapes shared by the fixture format and the renderer.
 */

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode \n \r \t \" \\ and \xNN; everything else is literal. */
static void unescape_into(buffer *out, const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (text[i] != '\\' || i + 1u >= length) {
            buffer_putc(out, text[i]);
            continue;
        }
        char next = text[i + 1u];
        i += 1u;
        switch (next) {
        case 'n': buffer_putc(out, '\n'); break;
        case 'r': buffer_putc(out, '\r'); break;
        case 't': buffer_putc(out, '\t'); break;
        case '"': buffer_putc(out, '"'); break;
        case '\\': buffer_putc(out, '\\'); break;
        case 'x': {
            if (i + 2u < length) {
                int high = hex_value(text[i + 1u]);
                int low = hex_value(text[i + 2u]);
                if (high >= 0 && low >= 0) {
                    buffer_putc(out, (char)((high << 4) | low));
                    i += 2u;
                    break;
                }
            }
            buffer_putc(out, 'x');
            break;
        }
        default:
            buffer_putc(out, next);
            break;
        }
    }
}

/* The inverse, for rendering text nodes and attribute values. */
static void render_quoted(buffer *out, txml_str text) {
    buffer_putc(out, '"');
    for (uint32_t i = 0; i < text.length; i++) {
        unsigned char c = (unsigned char)text.ptr[i];
        switch (c) {
        case '"': buffer_puts(out, "\\\""); break;
        case '\\': buffer_puts(out, "\\\\"); break;
        case '\n': buffer_puts(out, "\\n"); break;
        case '\r': buffer_puts(out, "\\r"); break;
        case '\t': buffer_puts(out, "\\t"); break;
        default:
            if (c < 0x20u || c == 0x7fu) {
                buffer_printf(out, "\\x%02x", c);
            } else {
                buffer_putc(out, (char)c);
            }
            break;
        }
    }
    buffer_putc(out, '"');
}

/* ------------------------------------------------------------------------
 * Fixtures.
 */

typedef struct {
    char name[160];
    txml_limits limits;
    uint32_t chunk; /* 0 feeds the whole input at once */

    buffer xml;

    bool has_nodes, has_attributes, has_names, has_positions, has_error, has_roundtrip;
    buffer nodes, attributes, names, positions, error, roundtrip;
} spec_case;

static void spec_case_init(spec_case *item) {
    memset(item, 0, sizeof(*item));
    item->limits = TXML_LIMITS_DEFAULT;
}

static void spec_case_free(spec_case *item) {
    buffer_free(&item->xml);
    buffer_free(&item->nodes);
    buffer_free(&item->attributes);
    buffer_free(&item->names);
    buffer_free(&item->positions);
    buffer_free(&item->error);
    buffer_free(&item->roundtrip);
}

static bool parse_limits(const char *text, txml_limits *limits, const char *path, int line) {
    const char *at = text;
    while (*at != '\0') {
        while (*at == ' ' || *at == '\t') at++;
        if (*at == '\0') break;

        const char *key = at;
        while (*at != '\0' && *at != '=') at++;
        if (*at != '=') {
            fprintf(stderr, "%s:%d: limits need key=value\n", path, line);
            return false;
        }
        size_t key_length = (size_t)(at - key);
        at++;
        char *stop = NULL;
        unsigned long long value = strtoull(at, &stop, 10);
        if (stop == at) {
            fprintf(stderr, "%s:%d: limit value is not a number\n", path, line);
            return false;
        }
        at = stop;

        if (key_length == strlen("max_depth") && memcmp(key, "max_depth", key_length) == 0) {
            limits->max_depth = (uint32_t)value;
        } else if (key_length == strlen("max_attributes") && memcmp(key, "max_attributes", key_length) == 0) {
            limits->max_attributes = (uint32_t)value;
        } else if (key_length == strlen("max_name_length") && memcmp(key, "max_name_length", key_length) == 0) {
            limits->max_name_length = (uint32_t)value;
        } else if (key_length == strlen("max_names") && memcmp(key, "max_names", key_length) == 0) {
            limits->max_names = (uint32_t)value;
        } else if (key_length == strlen("max_document_bytes") && memcmp(key, "max_document_bytes", key_length) == 0) {
            limits->max_document_bytes = value;
        } else if (key_length == strlen("entity_expansion_budget") &&
                   memcmp(key, "entity_expansion_budget", key_length) == 0) {
            limits->entity_expansion_budget = value;
        } else {
            fprintf(stderr, "%s:%d: unknown limit %.*s\n", path, line, (int)key_length, key);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Reading a spec file.
 */

typedef enum {
    SECTION_NONE,
    SECTION_XML,
    SECTION_NODES,
    SECTION_ATTRIBUTES,
    SECTION_NAMES,
    SECTION_POSITIONS,
    SECTION_ERROR,
    SECTION_ROUNDTRIP
} section;

static char *duplicate(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1u);
    if (copy == NULL) {
        fprintf(stderr, "spec_test: out of memory\n");
        exit(2);
    }
    memcpy(copy, text, length + 1u);
    return copy;
}

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "spec_test: cannot open %s\n", path);
        return NULL;
    }
    buffer contents = { 0 };
    char chunk[4096];
    size_t read;
    while ((read = fread(chunk, 1u, sizeof(chunk), file)) > 0u) {
        buffer_put(&contents, chunk, read);
    }
    fclose(file);
    *length = contents.length;
    return contents.data == NULL ? duplicate("") : contents.data;
}

typedef void (*case_handler)(const spec_case *item, const char *path);

static bool read_spec_file(const char *path, case_handler handle) {
    size_t length = 0u;
    char *text = read_file(path, &length);
    if (text == NULL) {
        return false;
    }

    spec_case item;
    spec_case_init(&item);
    section current = SECTION_NONE;
    bool open_case = false;
    bool ok = true;
    int line_number = 0;

    size_t at = 0u;
    while (at <= length) {
        size_t start = at;
        while (at < length && text[at] != '\n') at++;
        size_t line_length = at - start;
        const char *line = text + start;
        bool last = at >= length;
        at++;
        line_number++;

        if (last && line_length == 0u) {
            break;
        }

        /* Comments and blank lines outside a section. */
        if (current == SECTION_NONE && (line_length == 0u || line[0] == '#')) {
            continue;
        }

        if (line_length > 0u && line[0] == '@') {
            char directive[32];
            size_t directive_length = 0u;
            size_t i = 1u;
            while (i < line_length && line[i] != ' ' && line[i] != '\t' &&
                   directive_length + 1u < sizeof(directive)) {
                directive[directive_length++] = line[i++];
            }
            directive[directive_length] = '\0';
            while (i < line_length && (line[i] == ' ' || line[i] == '\t')) i++;

            char argument[512];
            size_t argument_length = line_length > i ? line_length - i : 0u;
            if (argument_length + 1u > sizeof(argument)) {
                argument_length = sizeof(argument) - 1u;
            }
            memcpy(argument, line + i, argument_length);
            argument[argument_length] = '\0';

            if (strcmp(directive, "case") == 0) {
                if (open_case) {
                    fprintf(stderr, "%s:%d: @case inside an unfinished case\n", path, line_number);
                    ok = false;
                    break;
                }
                spec_case_init(&item);
                /* Truncates like `argument` itself; the precision states that to the compiler. */
                snprintf(item.name, sizeof(item.name), "%.*s", (int)(sizeof(item.name) - 1u), argument);
                open_case = true;
                current = SECTION_NONE;
                continue;
            }

            if (!open_case) {
                fprintf(stderr, "%s:%d: @%s outside a case\n", path, line_number, directive);
                ok = false;
                break;
            }

            if (strcmp(directive, "limits") == 0) {
                if (!parse_limits(argument, &item.limits, path, line_number)) { ok = false; break; }
                current = SECTION_NONE;
            } else if (strcmp(directive, "chunk") == 0) {
                item.chunk = (uint32_t)strtoul(argument, NULL, 10);
                current = SECTION_NONE;
            } else if (strcmp(directive, "xml") == 0) {
                current = SECTION_XML;
            } else if (strcmp(directive, "nodes") == 0) {
                current = SECTION_NODES;
                item.has_nodes = true;
            } else if (strcmp(directive, "attributes") == 0) {
                current = SECTION_ATTRIBUTES;
                item.has_attributes = true;
            } else if (strcmp(directive, "names") == 0) {
                current = SECTION_NAMES;
                item.has_names = true;
            } else if (strcmp(directive, "positions") == 0) {
                current = SECTION_POSITIONS;
                item.has_positions = true;
            } else if (strcmp(directive, "error") == 0) {
                current = SECTION_ERROR;
                item.has_error = true;
            } else if (strcmp(directive, "roundtrip") == 0) {
                current = SECTION_ROUNDTRIP;
                item.has_roundtrip = true;
            } else if (strcmp(directive, "end") == 0) {
                cases_run++;
                handle(&item, path);
                spec_case_free(&item);
                spec_case_init(&item);
                open_case = false;
                current = SECTION_NONE;
            } else {
                fprintf(stderr, "%s:%d: unknown directive @%s\n", path, line_number, directive);
                ok = false;
                break;
            }
            continue;
        }

        switch (current) {
        case SECTION_XML:
            if (item.xml.length > 0u) {
                buffer_putc(&item.xml, '\n');
            }
            unescape_into(&item.xml, line, line_length);
            break;
        case SECTION_NODES:
            buffer_put(&item.nodes, line, line_length);
            buffer_putc(&item.nodes, '\n');
            break;
        case SECTION_ATTRIBUTES:
            buffer_put(&item.attributes, line, line_length);
            buffer_putc(&item.attributes, '\n');
            break;
        case SECTION_NAMES:
            buffer_put(&item.names, line, line_length);
            buffer_putc(&item.names, '\n');
            break;
        case SECTION_POSITIONS:
            buffer_put(&item.positions, line, line_length);
            buffer_putc(&item.positions, '\n');
            break;
        case SECTION_ERROR:
            buffer_put(&item.error, line, line_length);
            buffer_putc(&item.error, '\n');
            break;
        case SECTION_ROUNDTRIP:
            if (item.roundtrip.length > 0u) {
                buffer_putc(&item.roundtrip, '\n');
            }
            unescape_into(&item.roundtrip, line, line_length);
            break;
        case SECTION_NONE:
            fprintf(stderr, "%s:%d: content outside a section\n", path, line_number);
            ok = false;
            break;
        }
        if (!ok) {
            break;
        }
    }

    if (ok && open_case) {
        fprintf(stderr, "%s: last case is missing @end\n", path);
        ok = false;
    }

    spec_case_free(&item);
    free(text);
    return ok;
}

/* ------------------------------------------------------------------------
 * Host fixtures for the parser and writer.
 */

static void *heap_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    size_t actual = alignment < sizeof(void *) ? sizeof(void *) : alignment;
    size_t rounded = (size + actual - 1u) / actual * actual;
    return aligned_alloc(actual, rounded == 0u ? actual : rounded);
}

static void heap_free(void *self, const txml_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const txml_allocator_vtable heap_vtable = { .alloc = heap_alloc, .free = heap_free };
static const txml_allocator heap = { .self = NULL, .v = &heap_vtable };

/* ------------------------------------------------------------------------
 * The renderer: the single source of truth for what a document looks like.
 */

typedef struct {
    buffer nodes;
    buffer attributes;
    buffer names;
    buffer positions;
    buffer error;
    buffer roundtrip;
} rendering;

static void rendering_free(rendering *r) {
    buffer_free(&r->nodes);
    buffer_free(&r->attributes);
    buffer_free(&r->names);
    buffer_free(&r->positions);
    buffer_free(&r->error);
    buffer_free(&r->roundtrip);
}

static const char *kind_name(txml_node_kind kind) {
    switch (kind) {
    case TXML_NODE_DOCUMENT: return "document";
    case TXML_NODE_ELEMENT: return "element";
    case TXML_NODE_TEXT: return "text";
    case TXML_NODE_COMMENT: return "comment";
    case TXML_NODE_PI: return "pi";
    case TXML_NODE_DECLARATION: return "declaration";
    case TXML_NODE_DOCTYPE: return "doctype";
    }
    return "?";
}

static const char *error_code_name(txml_document_error_code code) {
    switch (code) {
    case TXML_DOC_OK: return "OK";
    case TXML_DOC_ENCODING: return "ENCODING";
    case TXML_DOC_SYNTAX: return "SYNTAX";
    case TXML_DOC_NAME: return "NAME";
    case TXML_DOC_TAG_MISMATCH: return "TAG_MISMATCH";
    case TXML_DOC_DUPLICATE_ATTRIBUTE: return "DUPLICATE_ATTRIBUTE";
    case TXML_DOC_UNDECLARED_PREFIX: return "UNDECLARED_PREFIX";
    case TXML_DOC_UNDECLARED_ENTITY: return "UNDECLARED_ENTITY";
    case TXML_DOC_UNEXPECTED_EOF: return "UNEXPECTED_EOF";
    case TXML_DOC_NO_ROOT: return "NO_ROOT";
    case TXML_DOC_TRAILING_CONTENT: return "TRAILING_CONTENT";
    case TXML_DOC_LIMIT: return "LIMIT";
    }
    return "?";
}

static const char *limit_kind_name(txml_limit_kind limit) {
    switch (limit) {
    case TXML_LIMIT_NONE: return "NONE";
    case TXML_LIMIT_DEPTH: return "DEPTH";
    case TXML_LIMIT_ATTRIBUTES: return "ATTRIBUTES";
    case TXML_LIMIT_NAME_LENGTH: return "NAME_LENGTH";
    case TXML_LIMIT_NAMES: return "NAMES";
    case TXML_LIMIT_DOCUMENT_BYTES: return "DOCUMENT_BYTES";
    case TXML_LIMIT_ENTITY_EXPANSION: return "ENTITY_EXPANSION";
    }
    return "?";
}

static void render_name(buffer *out, const txml_doc *doc, uint32_t name) {
    if (name == TXML_NAME_NONE) {
        buffer_putc(out, '-');
        return;
    }
    txml_qname qname;
    if (txml_doc_name_at(doc, name, &qname) != TXML_OK) {
        buffer_printf(out, "<unknown name %u>", name);
        return;
    }
    buffer_putc(out, '{');
    buffer_put_str(out, qname.ns);
    buffer_putc(out, '}');
    buffer_put_str(out, qname.local);
}

static void render_node_flags(buffer *out, uint32_t flags) {
    if (flags == 0u) {
        return;
    }
    buffer_puts(out, " flags=");
    const char *separator = "";
    if ((flags & TXML_NODE_EMPTY) != 0u) { buffer_printf(out, "%sEMPTY", separator); separator = ","; }
    if ((flags & TXML_NODE_CDATA) != 0u) { buffer_printf(out, "%sCDATA", separator); separator = ","; }
    if ((flags & TXML_NODE_OPEN) != 0u) { buffer_printf(out, "%sOPEN", separator); separator = ","; }
}

static void render_attribute_flags(buffer *out, uint32_t flags) {
    if (flags == 0u) {
        return;
    }
    buffer_puts(out, " flags=");
    const char *separator = "";
    if ((flags & TXML_ATTR_NS_DECL) != 0u) { buffer_printf(out, "%sNS_DECL", separator); separator = ","; }
    if ((flags & TXML_ATTR_DEFAULTED) != 0u) { buffer_printf(out, "%sDEFAULTED", separator); separator = ","; }
}

/* The two kinds that carry attributes. A declaration's version/encoding/
 * standalone are ordinary attributes (ADR 0017), so it renders them the same
 * way an element does. */
static bool has_attributes(txml_node_kind kind) {
    return kind == TXML_NODE_ELEMENT || kind == TXML_NODE_DECLARATION;
}

static void render_nodes(rendering *out, const txml_doc *doc) {
    const uint32_t count = txml_doc_node_count(doc);

    for (uint32_t index = 0u; index < count; index++) {
        const txml_cursor cursor = txml_cursor_at(doc, index);
        const txml_node_kind kind = txml_kind(cursor);

        buffer_printf(&out->nodes, "%u %s ", index, kind_name(kind));

        switch (kind) {
        case TXML_NODE_DOCUMENT:
        case TXML_NODE_DECLARATION:
            /* Neither has a name or text; a declaration's parts are
             * attributes, rendered below. */
            buffer_putc(&out->nodes, '-');
            break;
        case TXML_NODE_ELEMENT:
            render_name(&out->nodes, doc, txml_node_name_id(cursor));
            break;
        case TXML_NODE_PI:
            render_name(&out->nodes, doc, txml_node_name_id(cursor));
            buffer_putc(&out->nodes, ' ');
            render_quoted(&out->nodes, txml_node_text(cursor));
            break;
        case TXML_NODE_TEXT:
        case TXML_NODE_COMMENT:
        case TXML_NODE_DOCTYPE:
            render_quoted(&out->nodes, txml_node_text(cursor));
            break;
        }

        if (kind == TXML_NODE_ELEMENT) {
            const txml_str prefix = txml_node_prefix(cursor);
            if (prefix.length > 0u) {
                buffer_puts(&out->nodes, " prefix=");
                buffer_put_str(&out->nodes, prefix);
            }
        }

        const uint32_t end = txml_subtree_end(cursor);
        if (end == TXML_INDEX_NONE) {
            buffer_puts(&out->nodes, " end=-");
        } else {
            buffer_printf(&out->nodes, " end=%u", end);
        }

        txml_cursor parent = cursor;
        if (txml_parent(&parent) == TXML_MOVED) {
            buffer_printf(&out->nodes, " parent=%u", txml_index(parent));
        } else {
            buffer_puts(&out->nodes, " parent=-");
        }

        buffer_printf(&out->nodes, " depth=%u", txml_depth(cursor));

        if (has_attributes(kind)) {
            buffer_printf(&out->nodes, " attributes=%u", txml_attribute_count(cursor));
        }

        render_node_flags(&out->nodes, txml_flags(cursor));
        buffer_putc(&out->nodes, '\n');

        /* Attributes, in document order, numbered within their node. */
        if (has_attributes(kind)) {
            txml_attr_cursor attribute;
            uint32_t position = 0u;
            if (txml_first_attribute(cursor, &attribute) == TXML_MOVED) {
                do {
                    txml_attribute_view view;
                    if (txml_attribute_read(attribute, &view) != TXML_OK) {
                        buffer_printf(&out->attributes, "%u.%u <unreadable>\n", index, position);
                        position++;
                        continue;
                    }
                    buffer_printf(&out->attributes, "%u.%u ", index, position);
                    render_name(&out->attributes, doc, view.name);
                    buffer_putc(&out->attributes, ' ');
                    render_quoted(&out->attributes, view.value);
                    render_attribute_flags(&out->attributes, view.flags);
                    buffer_putc(&out->attributes, '\n');
                    position++;
                } while (txml_next_attribute(&attribute) == TXML_MOVED);
            }
        }
    }
}

static void render_names(rendering *out, const txml_doc *doc) {
    const uint32_t count = txml_doc_name_count(doc);
    for (uint32_t name = 0u; name < count; name++) {
        buffer_printf(&out->names, "%u ", name);
        render_name(&out->names, doc, name);
        buffer_putc(&out->names, '\n');
    }
}

static void render_positions(rendering *out, const txml_doc *doc) {
    const uint32_t count = txml_doc_node_count(doc);
    for (uint32_t index = 0u; index < count; index++) {
        txml_position position;
        if (txml_node_position(txml_cursor_at(doc, index), &position) != TXML_OK) {
            /* Every node has a position now (ADR 0017), so a refusal is a bug
             * rather than a configuration. Render it and keep going: an early
             * return would silently shorten the section instead. */
            buffer_printf(&out->positions, "%u <no position>\n", index);
            continue;
        }
        buffer_printf(&out->positions, "%u %u:%u offset=%llu\n", index, position.line, position.column,
                      (unsigned long long)position.offset);
    }
}

static void render_roundtrip(rendering *out, const txml_doc *doc) {
    unsigned char bytes[256];
    txml_writer_config config = {
        .allocator = heap,
        .output = { .ptr = bytes, .capacity = (uint32_t)sizeof(bytes) },
        .limits = TXML_LIMITS_DEFAULT,
        .flags = TXML_WRITE_EMPTY_TAGS,
    };

    txml_writer *writer = NULL;
    if (txml_writer_init(&writer, &config) != TXML_OK) {
        buffer_puts(&out->roundtrip, "<writer init failed>");
        return;
    }

    txml_result result = txml_write_subtree(writer, txml_doc_root(doc));
    while (result == TXML_NEED_SPACE) {
        const txml_bytes produced = txml_writer_output(writer);
        buffer_put(&out->roundtrip, (const char *)produced.ptr, produced.length);
        result = txml_writer_drain(writer);
    }
    if (result == TXML_OK) {
        result = txml_writer_finish(writer);
        while (result == TXML_NEED_SPACE) {
            const txml_bytes produced = txml_writer_output(writer);
            buffer_put(&out->roundtrip, (const char *)produced.ptr, produced.length);
            result = txml_writer_drain(writer);
        }
    }
    if (result != TXML_OK) {
        buffer_printf(&out->roundtrip, "<writer failed: %d>", (int)result);
    } else {
        const txml_bytes rest = txml_writer_output(writer);
        buffer_put(&out->roundtrip, (const char *)rest.ptr, rest.length);
        txml_writer_drain(writer);
    }

    txml_writer_destroy(writer);
}

/* ------------------------------------------------------------------------
 * Parsing one case at one chunk size.
 */

static bool parse_and_render(const spec_case *item, uint32_t chunk, rendering *out, const char *path) {
    txml_parser_config config = {
        .allocator = heap,
        .limits = item->limits,
        .staging_length = 0u,
        .chunk_size = 0u,
        .step_budget = 0u,
    };

    txml_parser *parser = NULL;
    if (txml_parser_init(&parser, &config) != TXML_OK) {
        fprintf(stderr, "%s: %s: parser init failed\n", path, item->name);
        failures += 1;
        return false;
    }

    const unsigned char *bytes = (const unsigned char *)buffer_text(&item->xml);
    const uint32_t total = (uint32_t)item->xml.length;
    uint32_t fed = 0u;
    bool final_sent = false;
    bool failed = false;
    txml_document_error error = { 0 };

    for (unsigned long iterations = 0u;; iterations++) {
        if (iterations > 10000000u) {
            fprintf(stderr, "%s: %s: parser made no progress\n", path, item->name);
            failures += 1;
            txml_parser_destroy(parser);
            return false;
        }

        const txml_step step = txml_parse_step(parser);
        if (step == TXML_STEP_PROGRESS) {
            continue;
        }
        if (step == TXML_STEP_NEED_INPUT) {
            if (final_sent) {
                fprintf(stderr, "%s: %s: input was requested after the final chunk\n", path, item->name);
                failures += 1;
                txml_parser_destroy(parser);
                return false;
            }
            const uint32_t remaining = total - fed;
            uint32_t want = chunk == 0u || chunk > remaining ? remaining : chunk;
            const bool final = fed + want == total;
            const txml_bytes piece = { bytes + fed, want };
            const uint32_t taken = txml_parse_feed(parser, piece, final);
            fed += taken;
            if (final && taken == want) {
                final_sent = true;
            }
            continue;
        }
        if (step == TXML_STEP_NEED_MEMORY) {
            fprintf(stderr, "%s: %s: the allocator refused\n", path, item->name);
            failures += 1;
            txml_parser_destroy(parser);
            return false;
        }
        if (step == TXML_STEP_FAILED) {
            failed = true;
            if (txml_parser_error(parser, &error) != TXML_OK) {
                fprintf(stderr, "%s: %s: the parse failed without an error\n", path, item->name);
                failures += 1;
            }
            break;
        }
        break; /* TXML_STEP_DONE */
    }

    if (txml_parser_offset(parser) != (uint64_t)fed) {
        fprintf(stderr, "%s: %s: parser offset %llu does not match %u bytes fed\n", path, item->name,
                (unsigned long long)txml_parser_offset(parser), fed);
        failures += 1;
    }

    if (failed) {
        const txml_doc *partial = txml_parser_doc(parser);
        if (partial != NULL) {
            render_nodes(out, partial);
            render_names(out, partial);
            render_positions(out, partial);
        }
        buffer_printf(&out->error, "%s %u:%u offset=%llu", error_code_name(error.code), error.at.line,
                      error.at.column, (unsigned long long)error.at.offset);
        if (error.code == TXML_DOC_LIMIT) {
            buffer_printf(&out->error, " limit=%s", limit_kind_name(error.limit));
        }
        buffer_putc(&out->error, '\n');
        txml_parser_destroy(parser);
        return true;
    }

    txml_doc *doc = NULL;
    if (txml_parser_finish(parser, &doc) != TXML_OK || doc == NULL) {
        fprintf(stderr, "%s: %s: finishing the parse failed\n", path, item->name);
        failures += 1;
        txml_parser_destroy(parser);
        return false;
    }

    render_nodes(out, doc);
    render_names(out, doc);
    render_positions(out, doc);
    render_roundtrip(out, doc);
    txml_doc_destroy(doc);
    return true;
}

/* ------------------------------------------------------------------------
 * Comparing.
 */

static void report_section(const char *path, const char *name, const char *section_name,
                           const char *expected, const buffer *actual) {
    fprintf(stderr, "%s: %s: @%s does not match\n", path, name, section_name);
    fprintf(stderr, "--- expected ---\n%s", expected);
    fprintf(stderr, "--- actual -----\n%s", buffer_text(actual));
    fprintf(stderr, "----------------\n");
    failures += 1;
}

static void check_section(const char *path, const char *name, const char *section_name, bool present,
                          const buffer *expected, const buffer *actual) {
    if (!present) {
        return;
    }
    if (buffer_equal(actual, buffer_text(expected))) {
        return;
    }
    report_section(path, name, section_name, buffer_text(expected), actual);
}

static void concatenate(buffer *out, const rendering *r) {
    buffer_clear(out);
    buffer_puts(out, "@nodes\n");
    buffer_put(out, buffer_text(&r->nodes), r->nodes.length);
    buffer_puts(out, "@attributes\n");
    buffer_put(out, buffer_text(&r->attributes), r->attributes.length);
    buffer_puts(out, "@names\n");
    buffer_put(out, buffer_text(&r->names), r->names.length);
    buffer_puts(out, "@positions\n");
    buffer_put(out, buffer_text(&r->positions), r->positions.length);
    buffer_puts(out, "@error\n");
    buffer_put(out, buffer_text(&r->error), r->error.length);
    buffer_puts(out, "@roundtrip\n");
    buffer_put(out, buffer_text(&r->roundtrip), r->roundtrip.length);
}

static void run_case(const spec_case *item, const char *path) {
    if (verbose) {
        printf("  %s: %s\n", path, item->name);
    }

    rendering first = { 0 };
    if (!parse_and_render(item, item->chunk, &first, path)) {
        rendering_free(&first);
        return;
    }

    check_section(path, item->name, "nodes", item->has_nodes, &item->nodes, &first.nodes);
    check_section(path, item->name, "attributes", item->has_attributes, &item->attributes, &first.attributes);
    check_section(path, item->name, "names", item->has_names, &item->names, &first.names);
    check_section(path, item->name, "positions", item->has_positions, &item->positions, &first.positions);

    /* An absent or empty @error section means the parse must succeed. */
    if (item->has_error && item->error.length > 0u) {
        check_section(path, item->name, "error", true, &item->error, &first.error);
    } else if (first.error.length != 0u) {
        fprintf(stderr, "%s: %s: expected the parse to succeed, got %s", path, item->name,
                buffer_text(&first.error));
        failures += 1;
    }

    if (item->has_roundtrip) {
        if (!buffer_equal(&first.roundtrip, buffer_text(&item->roundtrip))) {
            report_section(path, item->name, "roundtrip", buffer_text(&item->roundtrip), &first.roundtrip);
        }
    }

    /* The chunk sweep: the same input, fed differently, renders identically. */
    buffer reference = { 0 };
    buffer candidate = { 0 };
    concatenate(&reference, &first);

    const uint32_t sweep[] = { 1u, 2u, 3u, 7u, 0u };
    for (size_t i = 0; i < sizeof(sweep) / sizeof(sweep[0]); i++) {
        if (sweep[i] == item->chunk) {
            continue;
        }
        rendering other = { 0 };
        if (!parse_and_render(item, sweep[i], &other, path)) {
            rendering_free(&other);
            continue;
        }
        concatenate(&candidate, &other);
        if (!buffer_equal(&candidate, buffer_text(&reference))) {
            fprintf(stderr, "%s: %s: chunk size %u renders differently\n", path, item->name, sweep[i]);
            fprintf(stderr, "--- chunk %u ---\n%s", item->chunk, buffer_text(&reference));
            fprintf(stderr, "--- chunk %u ---\n%s", sweep[i], buffer_text(&candidate));
            fprintf(stderr, "----------------\n");
            failures += 1;
        }
        rendering_free(&other);
    }

    buffer_free(&reference);
    buffer_free(&candidate);
    rendering_free(&first);
}

/* ------------------------------------------------------------------------
 * Walking the corpus.
 */

static int compare_paths(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static bool has_spec_suffix(const char *name) {
    size_t length = strlen(name);
    return length > 5u && strcmp(name + length - 5u, ".spec") == 0;
}

static bool run_directory(const char *path) {
    DIR *directory = opendir(path);
    if (directory == NULL) {
        fprintf(stderr, "spec_test: cannot open directory %s\n", path);
        return false;
    }

    char *paths[256];
    size_t count = 0u;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL && count < sizeof(paths) / sizeof(paths[0])) {
        if (!has_spec_suffix(entry->d_name)) {
            continue;
        }
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        paths[count++] = duplicate(full);
    }
    closedir(directory);

    if (count == 0u) {
        fprintf(stderr, "spec_test: no .spec files in %s\n", path);
        return false;
    }

    qsort(paths, count, sizeof(paths[0]), compare_paths);

    bool ok = true;
    for (size_t i = 0; i < count; i++) {
        if (verbose) {
            printf("%s\n", paths[i]);
        }
        ok = read_spec_file(paths[i], run_case) && ok;
        free(paths[i]);
    }
    return ok;
}

int main(int argc, char **argv) {
    const char *targets[32];
    int target_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
            continue;
        }
        if (target_count < (int)(sizeof(targets) / sizeof(targets[0]))) {
            targets[target_count++] = argv[i];
        }
    }

    if (target_count == 0) {
        fprintf(stderr, "usage: spec_test [-v] <spec file or directory>...\n");
        return 2;
    }

    bool ok = true;
    for (int i = 0; i < target_count; i++) {
        if (has_spec_suffix(targets[i])) {
            if (verbose) {
                printf("%s\n", targets[i]);
            }
            ok = read_spec_file(targets[i], run_case) && ok;
        } else {
            ok = run_directory(targets[i]) && ok;
        }
    }

    if (!ok || failures != 0) {
        fprintf(stderr, "spec_test: %d check(s) failed in %d case(s)\n", failures, cases_run);
        return 1;
    }

    printf("spec_test: ok (%d cases)\n", cases_run);
    return 0;
}
