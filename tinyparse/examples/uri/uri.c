/*
** uri.c -- parse a URI with the generated tinyparse grammar and print its
** components.
**
** The second stage of the pipeline example, and the "generated parser used by
** another program": it loads uri.scm (produced by the EBNF converter) at
** runtime, parses the URL given on the command line, walks the concrete syntax
** tree through the public cursor API, and prints each present component as
**
**     scheme=https
**     host=example.com
**     port=8080
**     path=/a/b
**     query=x=1
**     fragment=frag
**
** one per line, in a fixed order, omitting any component the URL lacks. A
** malformed URI prints a diagnostic to stderr and exits 1.
**
** This is a HOSTED program (libc is fine). It links the tinyparse dist
** archives; the library itself never touches IO.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyparse.h"

/* ** The host memory grant. */
#define GRANT (16u * 1024u * 1024u)

/* ** grow callback: spill into fresh malloc'd blocks if the arena fills. */
static void *grow(void *user, size_t need, size_t *got)
{
    (void)user;
    if (got != 0) *got = need;
    return malloc(need);
}

/* ** Wrap a NUL-terminated C string as a tinyparse_string. */
static tinyparse_string text(const char *s)
{
    tinyparse_string out;
    out.pointer = (const uint8_t *)s;
    out.length  = (uint32_t)strlen(s);
    return out;
}

/* ** Read an entire file into a malloc'd buffer; NULL on error. */
static char *read_all(const char *path, size_t *out_length)
{
    FILE  *file = fopen(path, "rb");
    char  *buffer = NULL;
    size_t length = 0, capacity = 0;
    if (file == NULL)
        return NULL;
    for (;;) {
        size_t got;
        if (length + 65536 > capacity) {
            size_t nc = capacity == 0 ? 65536 : capacity * 2;
            char *grown = realloc(buffer, nc);
            if (grown == NULL) { free(buffer); fclose(file); return NULL; }
            buffer = grown; capacity = nc;
        }
        got = fread(buffer + length, 1, 65536, file);
        length += got;
        if (got < 65536) break;
    }
    fclose(file);
    *out_length = length;
    return buffer;
}

/* ** The URL bytes the parser ran over, for slicing field text. */
static const char *g_input;

/* ** The current tree, for symbol-name lookups. */
static const tinyparse_tree *g_tree;

/* ** Return nonzero if the field bound to the node under the cursor is named
** exactly `name`. */
static int field_is(tinyparse_tree_cursor *cursor, const char *name)
{
    tinyparse_symbol field = tinyparse_tree_cursor_field(cursor);
    tinyparse_string n;
    size_t len = strlen(name);
    if (field.id == TINYPARSE_NO_FIELD)
        return 0;
    n = tinyparse_tree_symbol_name(g_tree, field);
    return n.length == len && memcmp(n.pointer, name, len) == 0;
}

/* ** If a field named `name` has been found, write its source text into out and
** return 1; otherwise return 0. The search is a recursive pre-order walk so it
** reaches fields nested inside the Authority sub-rule. */
static int find_field(tinyparse_tree_cursor *cursor, const char *name,
                      char *out, size_t cap)
{
    if (field_is(cursor, name)) {
        uint32_t a = tinyparse_tree_cursor_start_byte(cursor);
        uint32_t b = tinyparse_tree_cursor_end_byte(cursor);
        size_t n = (b > a) ? (size_t)(b - a) : 0;
        if (n > cap - 1) n = cap - 1;
        memcpy(out, g_input + a, n);
        out[n] = '\0';
        return 1;
    }
    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        do {
            if (find_field(cursor, name, out, cap)) {
                tinyparse_tree_cursor_goto_parent(cursor);
                return 1;
            }
        } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }
    return 0;
}

/* ** Print "name=value" for a field if present; the cursor is reset to the root
** before each lookup so order of calls is independent of tree position. */
static void print_field(tinyparse_tree_cursor *cursor, const char *name)
{
    char value[2048];
    tinyparse_tree_cursor_reset_to_root(cursor);
    if (find_field(cursor, name, value, sizeof value))
        printf("%s=%s\n", name, value);
}

/* ** Entry point: uri <url>. */
int main(int argc, char **argv)
{
    tinyparse_engine  *engine;
    tinyparse_grammar *grammar = NULL;
    tinyparse_parser  *parser  = NULL;
    tinyparse_tree_cursor *cursor = NULL;
    const tinyparse_tree  *tree;
    tinyparse_source   source;
    tinyparse_configuration cfg;
    tinyparse_result   r;
    char  *grammar_bytes;
    size_t grammar_length;
    void  *block;
    const char *self;
    char grammar_path[4096];

    if (argc < 2) {
        fprintf(stderr, "usage: uri <url>\n");
        return 2;
    }

    /* Locate uri.scm beside this executable. argv[0] holds the path run.sh used. */
    self = argv[0];
    {
        const char *slash = strrchr(self, '/');
        if (slash != NULL) {
            size_t dirlen = (size_t)(slash - self) + 1;
            if (dirlen > sizeof grammar_path - 16) dirlen = sizeof grammar_path - 16;
            memcpy(grammar_path, self, dirlen);
            strcpy(grammar_path + dirlen, "uri.scm");
        } else {
            strcpy(grammar_path, "uri.scm");
        }
    }

    grammar_bytes = read_all(grammar_path, &grammar_length);
    if (grammar_bytes == NULL) {
        fprintf(stderr, "uri: error: cannot read grammar '%s'\n", grammar_path);
        return 1;
    }

    block = malloc(GRANT);
    if (block == NULL) { fprintf(stderr, "uri: out of memory\n"); return 1; }
    memset(&cfg, 0, sizeof cfg);
    cfg.memory.base = block;
    cfg.memory.size = GRANT;
    cfg.memory.grow = grow;
    engine = tinyparse_open(cfg);
    if (engine == NULL) { fprintf(stderr, "uri: cannot open engine\n"); return 1; }

    source.name  = text(grammar_path);
    source.bytes.pointer = (const uint8_t *)grammar_bytes;
    source.bytes.length  = (uint32_t)grammar_length;
    r = tinyparse_compile_scheme(engine, source, &grammar);
    if (r != TINYPARSE_OK) {
        tinyparse_string f = tinyparse_fault(engine);
        fprintf(stderr, "uri: grammar error: %.*s\n",
                (int)f.length, (const char *)f.pointer);
        return 1;
    }

    {
        tinyparse_string empty; empty.pointer = NULL; empty.length = 0;
        r = tinyparse_parser_new(engine, grammar, empty, &parser);
    }
    if (r != TINYPARSE_OK || parser == NULL) {
        fprintf(stderr, "uri: cannot create parser\n");
        return 1;
    }

    g_input = argv[1];
    tinyparse_push(parser, text(argv[1]));
    r = tinyparse_finish(parser);
    if (r == TINYPARSE_SYNTAX_ERROR || r == TINYPARSE_ERROR) {
        uint32_t count = tinyparse_parser_diagnostic_count(parser), d;
        fprintf(stderr, "uri: error: malformed URI '%s'\n", argv[1]);
        for (d = 0; d < count; d++) {
            const tinyparse_diagnostic *diag = tinyparse_parser_diagnostic(parser, d);
            if (diag)
                fprintf(stderr, "  %.*s\n", (int)diag->message.length,
                        (const char *)diag->message.pointer);
        }
        return 1;
    }

    tree = tinyparse_parser_tree(parser);
    if (tree == NULL) {
        fprintf(stderr, "uri: error: no tree for '%s'\n", argv[1]);
        return 1;
    }
    if (tinyparse_tree_cursor_new(tree, &cursor) != TINYPARSE_OK) {
        fprintf(stderr, "uri: cannot make cursor\n");
        return 1;
    }
    g_tree = tree;

    /* Print the components in a fixed, readable order; absent ones are skipped. */
    print_field(cursor, "scheme");
    print_field(cursor, "userinfo");
    print_field(cursor, "host");
    print_field(cursor, "port");
    print_field(cursor, "path");
    print_field(cursor, "query");
    print_field(cursor, "fragment");

    tinyparse_tree_cursor_free(cursor);
    tinyparse_parser_free(parser);
    tinyparse_grammar_free(grammar);
    tinyparse_close(engine);
    free(block);
    free(grammar_bytes);
    return 0;
}
