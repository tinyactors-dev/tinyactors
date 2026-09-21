/*
 * txml: parse XML and print the parse tree the way `tree-sitter parse` does.
 *
 *   txml [-s] [-S] [--color=WHEN] [FILE...]
 *
 * Default output is `tree-sitter parse --cst`: one line per node, its range
 * `row:col - row:col`, the kind, and leaf text in backticks. `-S` prints the
 * plain `tree-sitter parse` S-expression form instead.
 *
 * With no FILE, or a FILE of "-", the input is standard input. The tree goes
 * to standard output, diagnostics to standard error. Exit status: 0 when every
 * input parsed, 1 when a document was malformed, 2 for a usage or I/O error.
 *
 * Every range comes from the document (txml_node_span, txml_attribute_span_read,
 * ADR 0023); nothing here reads the source again, so the input streams through
 * the parser in chunks and is never held whole.
 *
 * This is a program, not the library: it may use the C library, and does. The
 * library's own constraints (AGENTS.md) do not apply here, but the walk is
 * still iterative, because a document's nesting must not decide our stack.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/txml.h"

enum { EXIT_OK = 0, EXIT_MALFORMED = 1, EXIT_FAILURE_USAGE = 2 };

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

typedef struct {
    bool skip_whitespace;
    bool sexp;
    bool color;
} options;

/* ------------------------------------------------------------------------
 * Rows: the tree flattened to what is printed, in order.
 */

typedef struct {
    uint32_t depth;
    const char *kind;
    bool has_range;      /* false for an attribute with no source text */
    txml_position start;
    txml_position end;
    bool has_text;
    txml_str text;
    char *owned;         /* text built here (a qualified name) */
} row;

typedef struct {
    row *rows;
    size_t count;
    size_t capacity;
} row_list;

/* A new row, or NULL when memory ran out. */
static row *row_add(row_list *list, uint32_t depth, const char *kind) {
    if (list->count == list->capacity) {
        size_t grown = list->capacity == 0 ? 256u : list->capacity * 2u;
        row *bigger = realloc(list->rows, grown * sizeof *bigger);
        if (bigger == NULL) return NULL;
        list->rows = bigger;
        list->capacity = grown;
    }
    row *r = &list->rows[list->count++];
    memset(r, 0, sizeof *r);
    r->depth = depth;
    r->kind = kind;
    return r;
}

static void row_list_free(row_list *list) {
    for (size_t i = 0; i < list->count; i++) free(list->rows[i].owned);
    free(list->rows);
}

/* `count` bytes on from `at`, on the same line: a name never spans lines. */
static txml_position along(txml_position at, uint32_t count) {
    at.offset += count;
    at.column += count;
    return at;
}

static void row_range(row *r, txml_position start, txml_position end) {
    r->has_range = true;
    r->start = start;
    r->end = end;
}

static void row_text(row *r, txml_str text) {
    r->has_text = true;
    r->text = text;
}

/* "prefix:local" as written; the row owns the result. */
static bool row_qualified(row *r, txml_cursor node, txml_str prefix, uint32_t id) {
    txml_qname name = { { "", 0 }, { "", 0 } };
    (void)txml_doc_name_at(node.doc, id, &name);
    size_t n = prefix.length + (prefix.length ? 1u : 0u) + name.local.length;
    char *text = malloc(n + 1u);
    if (text == NULL) return false;
    size_t at = 0;
    if (prefix.length) {
        memcpy(text, prefix.ptr, prefix.length);
        at = prefix.length;
        text[at++] = ':';
    }
    memcpy(text + at, name.local.ptr, name.local.length);
    text[n] = '\0';
    r->owned = text;
    row_text(r, (txml_str){ text, (uint32_t)n });
    return true;
}

static const char *kind_name(txml_cursor node) {
    switch (txml_kind(node)) {
    case TXML_NODE_DOCUMENT: return "document";
    case TXML_NODE_ELEMENT: return "element";
    case TXML_NODE_TEXT: return (txml_flags(node) & TXML_NODE_CDATA) ? "cdata" : "text";
    case TXML_NODE_COMMENT: return "comment";
    case TXML_NODE_PI: return "pi";
    case TXML_NODE_DECLARATION: return "declaration";
    case TXML_NODE_DOCTYPE: return "doctype";
    }
    return "unknown";
}

static bool is_blank(txml_str text) {
    for (uint32_t i = 0; i < text.length; i++) {
        char c = text.ptr[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;
}

/* The attributes of an element or declaration, each an `attribute` row over a
 * `name` row and a `value` row. Returns false when memory ran out. */
static bool add_attributes(row_list *list, txml_cursor node, uint32_t depth) {
    txml_attr_cursor cursor;
    if (txml_first_attribute(node, &cursor) != TXML_MOVED) return true;
    do {
        txml_attribute_view view;
        if (txml_attribute_read(cursor, &view) != TXML_OK) continue;
        txml_attribute_span span;
        bool sourced = txml_attribute_span_read(cursor, &span) == TXML_OK;

        row *attribute = row_add(list, depth, "attribute");
        if (attribute == NULL) return false;
        if (sourced) row_range(attribute, span.start, span.end);

        row *name = row_add(list, depth + 1u, "name");
        if (name == NULL || !row_qualified(name, node, view.prefix, view.name)) return false;
        if (sourced) row_range(name, span.start, span.name_end);

        row *value = row_add(list, depth + 1u, "value");
        if (value == NULL) return false;
        row_text(value, view.value);
        if (sourced) row_range(value, span.value_start, span.value_end);
    } while (txml_next_attribute(&cursor) == TXML_MOVED);
    return true;
}

static bool build_rows(row_list *list, const txml_doc *doc, const options *opts) {
    uint32_t count = txml_doc_node_count(doc);
    for (uint32_t index = 0; index < count; index++) {
        txml_cursor node = txml_cursor_at(doc, index);
        txml_node_kind kind = txml_kind(node);
        if (opts->skip_whitespace && kind == TXML_NODE_TEXT && !(txml_flags(node) & TXML_NODE_CDATA) &&
            is_blank(txml_node_text(node))) {
            continue;
        }

        txml_span span;
        if (txml_node_span(node, &span) != TXML_OK) return false; /* cannot happen after DONE */
        uint32_t depth = txml_depth(node);
        row *r = row_add(list, depth, kind_name(node));
        if (r == NULL) return false;
        row_range(r, span.start, span.end);

        if (kind == TXML_NODE_TEXT || kind == TXML_NODE_COMMENT || kind == TXML_NODE_DOCTYPE ||
            kind == TXML_NODE_PI) {
            row_text(r, txml_node_text(node));
        }
        if (kind == TXML_NODE_ELEMENT) {
            /* The name follows the '<'. */
            row *name = row_add(list, depth + 1u, "name");
            if (name == NULL || !row_qualified(name, node, txml_node_prefix(node), txml_node_name_id(node))) return false;
            row_range(name, along(span.start, 1u), along(span.start, 1u + name->text.length));
        } else if (kind == TXML_NODE_PI) {
            /* The target follows the "<?". */
            row *target = row_add(list, depth + 1u, "target");
            if (target == NULL || !row_qualified(target, node, (txml_str){ "", 0 }, txml_node_name_id(node))) return false;
            row_range(target, along(span.start, 2u), along(span.start, 2u + target->text.length));
        }
        if (kind == TXML_NODE_ELEMENT || kind == TXML_NODE_DECLARATION) {
            if (!add_attributes(list, node, depth + 1u)) return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Output. Rows and columns are 0-based, as tree-sitter prints them.
 */

typedef struct { const char *reset, *range, *kind, *text, *tick; } palette;
static const palette PLAIN = { "", "", "", "", "" };
static const palette COLORED = { "\x1b[0m", "\x1b[37m", "\x1b[96m", "\x1b[38;2;118;118;118m", "\x1b[38;2;101;192;67m" };

/* Text is escaped so that a node stays on one line. */
static void put_escaped(FILE *out, txml_str text, char quote) {
    for (uint32_t i = 0; i < text.length; i++) {
        unsigned char c = (unsigned char)text.ptr[i];
        if (c == (unsigned char)quote || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\r') fputs("\\r", out);
        else if (c == '\t') fputs("\\t", out);
        else if (c < 0x20u || c == 0x7fu) fprintf(out, "\\x%02x", c);
        else fputc(c, out);
    }
}

static int digits(uint32_t value) {
    int n = 1;
    while (value >= 10u) { value /= 10u; n++; }
    return n;
}

static int point_width(txml_position at) {
    return digits(at.line - 1u) + 1 + digits(at.column - 1u);
}

/* `tree-sitter parse --cst`: aligned ranges, then indentation and the node. */
static void print_cst(FILE *out, const row_list *list, const palette *p) {
    int start_width = 0, end_width = 0;
    for (size_t i = 0; i < list->count; i++) {
        const row *r = &list->rows[i];
        if (!r->has_range) continue;
        if (point_width(r->start) > start_width) start_width = point_width(r->start);
        if (point_width(r->end) > end_width) end_width = point_width(r->end);
    }
    for (size_t i = 0; i < list->count; i++) {
        const row *r = &list->rows[i];
        if (r->has_range) {
            char left[32], right[32];
            snprintf(left, sizeof left, "%u:%u", (unsigned)(r->start.line - 1u), (unsigned)(r->start.column - 1u));
            snprintf(right, sizeof right, "%u:%u", (unsigned)(r->end.line - 1u), (unsigned)(r->end.column - 1u));
            fprintf(out, "%s%-*s - %-*s%s", p->range, start_width, left, end_width, right, p->reset);
        } else {
            fprintf(out, "%*s", start_width + end_width + 3, "");
        }
        fprintf(out, " %*s", (int)(r->depth * 2u + 1u), "");
        fprintf(out, "%s%s%s", p->kind, r->kind, p->reset);
        if (r->has_text) {
            fprintf(out, " %s`%s%s", p->tick, p->reset, p->text);
            put_escaped(out, r->text, '`');
            fprintf(out, "%s%s`%s", p->reset, p->tick, p->reset);
        }
        fputc('\n', out);
    }
}

/* Plain `tree-sitter parse`: (kind [row, col] - [row, col] children...). */
static void print_sexp(FILE *out, const row_list *list) {
    uint32_t open = 0;
    for (size_t i = 0; i < list->count; i++) {
        const row *r = &list->rows[i];
        while (open > r->depth) { fputc(')', out); open--; }
        if (i > 0) fputc('\n', out);
        for (uint32_t d = 0; d < r->depth; d++) fputs("  ", out);
        fprintf(out, "(%s", r->kind);
        if (r->has_range) {
            fprintf(out, " [%u, %u] - [%u, %u]", (unsigned)(r->start.line - 1u), (unsigned)(r->start.column - 1u),
                    (unsigned)(r->end.line - 1u), (unsigned)(r->end.column - 1u));
        }
        open = r->depth + 1u;
    }
    while (open > 0) { fputc(')', out); open--; }
    fputc('\n', out);
}

/* ------------------------------------------------------------------------
 * Driver
 */

static int report(const char *name, const char *what) {
    fprintf(stderr, "txml: %s: %s\n", name, what);
    return EXIT_FAILURE_USAGE;
}

static int parse_stream(FILE *in, const char *name, const options *opts, bool heading) {
    txml_parser_config config = {
        .allocator = { .self = NULL, .v = &heap_vtable },
        .limits = TXML_LIMITS_DEFAULT,
    };
    txml_parser *parser = NULL;
    if (txml_parser_init(&parser, &config) != TXML_OK) return report(name, "cannot start parser");

    uint8_t chunk[65536];
    size_t got = 0, offset = 0;
    bool final = false;
    txml_step step = TXML_STEP_NEED_INPUT;
    while (step != TXML_STEP_DONE && step != TXML_STEP_FAILED) {
        if (step == TXML_STEP_NEED_MEMORY) {
            txml_parser_destroy(parser);
            return report(name, "out of memory");
        }
        if (step == TXML_STEP_NEED_INPUT) {
            if (offset == got) { /* what we read has all been taken: read more */
                if (final) break; /* the parser has the end of input and still wants more */
                got = fread(chunk, 1, sizeof chunk, in);
                offset = 0;
                if (got < sizeof chunk) {
                    if (ferror(in)) {
                        txml_parser_destroy(parser);
                        return report(name, "read error");
                    }
                    final = true;
                }
            }
            /* The window may take only part of it; the rest is offered again. */
            offset += txml_parse_feed(parser, (txml_bytes){ chunk + offset, (uint32_t)(got - offset) }, final);
        }
        step = txml_parse_step(parser);
    }

    if (step != TXML_STEP_DONE) {
        txml_document_error error;
        if (txml_parser_error(parser, &error) == TXML_OK) {
            fprintf(stderr, "%s:%u:%u: %.*s\n", name, (unsigned)error.at.line, (unsigned)error.at.column,
                    (int)error.message.length, error.message.ptr);
        } else {
            fprintf(stderr, "%s: parse did not complete\n", name);
        }
        txml_parser_destroy(parser);
        return EXIT_MALFORMED;
    }

    txml_doc *doc = NULL;
    if (txml_parser_finish(parser, &doc) != TXML_OK) {
        txml_parser_destroy(parser);
        return report(name, "cannot detach document");
    }

    row_list list = { NULL, 0, 0 };
    int status = EXIT_OK;
    if (!build_rows(&list, doc, opts)) {
        status = report(name, "out of memory");
    } else {
        if (heading) printf("==> %s <==\n", name);
        if (opts->sexp) print_sexp(stdout, &list);
        else print_cst(stdout, &list, opts->color ? &COLORED : &PLAIN);
    }
    row_list_free(&list);
    txml_doc_destroy(doc);
    return status;
}

static void usage(FILE *out) {
    fputs("usage: txml [-s] [-S] [--color=auto|always|never] [FILE...]\n"
          "\n"
          "Parse XML and print the parse tree like `tree-sitter parse --cst`: one line\n"
          "per node with its range (row:col - row:col, 0-based, byte columns), the\n"
          "node kind, and leaf text in backticks. With no FILE, or FILE is -, read\n"
          "standard input.\n"
          "\n"
          "  -S, --sexp             print the S-expression form of `tree-sitter parse`\n"
          "  -s, --skip-whitespace  omit whitespace-only text nodes\n"
          "      --color=WHEN       auto (default: a terminal, unless NO_COLOR), always, never\n"
          "  -h, --help             show this help\n"
          "\n"
          "Exit status: 0 success, 1 a document was malformed, 2 usage or I/O error.\n",
          out);
}

int main(int argc, char **argv) {
    options opts = { false, false, false };
    const char *color = "auto";
    int first = 1;
    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (strcmp(arg, "--") == 0) { first++; break; }
        if (strcmp(arg, "-") == 0 || arg[0] != '-') break;
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) { usage(stdout); return EXIT_OK; }
        if (strcmp(arg, "-s") == 0 || strcmp(arg, "--skip-whitespace") == 0) { opts.skip_whitespace = true; continue; }
        if (strcmp(arg, "-S") == 0 || strcmp(arg, "--sexp") == 0) { opts.sexp = true; continue; }
        if (strncmp(arg, "--color=", 8) == 0) { color = arg + 8; continue; }
        fprintf(stderr, "txml: unknown option %s\n", arg);
        usage(stderr);
        return EXIT_FAILURE_USAGE;
    }
    if (strcmp(color, "always") == 0) opts.color = true;
    else if (strcmp(color, "never") == 0) opts.color = false;
    else if (strcmp(color, "auto") == 0) opts.color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
    else {
        fprintf(stderr, "txml: --color takes auto, always or never, not %s\n", color);
        return EXIT_FAILURE_USAGE;
    }

    int status = EXIT_OK;
    int files = argc - first;
    if (files <= 0) return parse_stream(stdin, "<stdin>", &opts, false);

    for (int i = first; i < argc; i++) {
        const char *name = argv[i];
        FILE *in = stdin;
        if (strcmp(name, "-") == 0) {
            name = "<stdin>";
        } else if ((in = fopen(name, "rb")) == NULL) {
            perror(name);
            status = EXIT_FAILURE_USAGE;
            continue;
        }
        if (files > 1 && i > first) fputc('\n', stdout);
        int result = parse_stream(in, name, &opts, files > 1);
        if (in != stdin) fclose(in);
        if (result > status) status = result;
    }
    return status;
}
