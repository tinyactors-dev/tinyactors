/*
** embed.c -- the W16-3 multi-parser embedding driver.
**
** ONE LESSON: two pocketparse parsers, each compiled to its OWN C translation
** unit with its OWN symbol prefix, link into ONE program and run together. A
** host fenced-block grammar (host.scm) finds the code spans in a Markdown-ish
** document; a separately compiled std.json parser then parses the bytes inside
** each fence. Two grammars, two generated parsers, two trees, one binary.
**
** This is NOT the W62 injection demo (std.fence), which parses both languages
** inside one grammar. Here the embedding is done by the HOST PROGRAM: it reads
** the `code' field's byte span straight off the host tree (never re-scanning the
** source) and feeds exactly those bytes to the json parser.
**
** The two generated parsers are emitted with `pocketparse c ... --parser-lib
** --name P'. Without distinct prefixes their exported entries
** (pp_compiled_parser_load / _run) and every emitted file-scope symbol would
** collide at link time; with `--name host_parser' and `--name json_parser' they
** become host_parser_load / json_parser_load and link cleanly. build.sh
** generates both and links them with this driver against libpocketparse.
**
** This is host dev-tool code: libc is fine.
**
** Usage: embed <document.md>   (or stdin when the path is "-")
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pocketparse.h"

/* ** The two callable entries each generated parser-lib TU exposes, under its
** own --name prefix. host_parser_* drives host.scm; json_parser_* drives
** std.json. Declared here, defined in the two generated translation units. The
** prefixes are exactly what proves the no-collision property: two complete sets
** of parser symbols coexist in this one program. */
pocketparse_result host_parser_load(pocketparse_engine *engine,
                                    pocketparse_grammar **out_grammar);
pocketparse_result host_parser_run(pocketparse_parser *parser);
pocketparse_result json_parser_load(pocketparse_engine *engine,
                                    pocketparse_grammar **out_grammar);
pocketparse_result json_parser_run(pocketparse_parser *parser);

/* ** A modest grant; the grow callback supplies the rest if a parse needs it. */
#define EMBED_GRANT (8u * 1024u * 1024u)

static void *embed_grow(void *user, size_t need, size_t *got)
{
    (void)user;
    if (got != 0) *got = need;
    return malloc(need);
}

/* ** Open a fresh engine over its own grant. Each parse gets a fresh engine so
** the host parse and an embedded parse never share arena state. */
static pocketparse_engine *embed_open(void)
{
    pocketparse_configuration cfg;
    void *block = malloc(EMBED_GRANT);
    if (block == NULL)
        return NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.memory.base = block;
    cfg.memory.size = EMBED_GRANT;
    cfg.memory.grow = embed_grow;
    cfg.memory.user = NULL;
    cfg.memory.chunk_minimum = 0;
    cfg.flags = 0;
    return pocketparse_open(cfg);
}

/* ** Print the subtree under the cursor in pre-order, one node per line, indented
** by `base' + the node's depth. Reads the tree through the cursor only -- node
** kinds, field names, byte spans -- and builds no parallel structure of its own.
** Shared by both the host dump and each embedded dump, so the two trees print in
** the same shape. */
static void embed_dump(const pocketparse_tree *tree,
                       pocketparse_tree_cursor *cursor, uint32_t base)
{
    uint32_t depth = pocketparse_tree_cursor_depth(cursor);
    uint32_t i;
    pocketparse_symbol kind  = pocketparse_tree_cursor_kind(cursor);
    pocketparse_symbol field = pocketparse_tree_cursor_field(cursor);
    pocketparse_string kn    = pocketparse_tree_symbol_name(tree, kind);

    for (i = 0; i < base + depth; i++) fputs("  ", stdout);
    if (field.id != POCKETPARSE_NO_FIELD) {
        pocketparse_string fn = pocketparse_tree_symbol_name(tree, field);
        printf("%.*s: ", (int)fn.length, (const char *)fn.pointer);
    }
    printf("%.*s\n", (int)kn.length, (const char *)kn.pointer);

    if (pocketparse_tree_cursor_goto_first_child(cursor)) {
        do {
            embed_dump(tree, cursor, base);
        } while (pocketparse_tree_cursor_goto_next_sibling(cursor));
        pocketparse_tree_cursor_goto_parent(cursor);
    }
}

/* ** Parse `bytes' with the json parser and dump its tree, indented under the
** host fence by `base'. A FRESH engine loads the embedded std.json grammar
** through json_parser_load (so the generated loop's baked indices match the
** grammar it reads), pushes the span, and runs json_parser_run -- the second
** compiled parser, the whole point of the example. Returns 0 on a clean parse. */
static int parse_embedded(const uint8_t *bytes, uint32_t length, uint32_t base)
{
    pocketparse_engine  *engine = embed_open();
    pocketparse_grammar *grammar = NULL;
    pocketparse_parser  *parser  = NULL;
    pocketparse_string   empty, span;
    const pocketparse_tree  *tree;
    pocketparse_tree_cursor *cursor = NULL;
    pocketparse_result   result;
    int rc = 0;

    if (engine == NULL) { fputs("embed: out of memory\n", stderr); return 2; }
    if (json_parser_load(engine, &grammar) != POCKETPARSE_OK) {
        fputs("embed: cannot load embedded json grammar\n", stderr);
        pocketparse_close(engine);
        return 2;
    }
    empty.pointer = NULL; empty.length = 0;
    if (pocketparse_parser_new(engine, grammar, empty, &parser) != POCKETPARSE_OK
        || parser == NULL) {
        pocketparse_close(engine);
        return 2;
    }
    span.pointer = bytes;
    span.length  = length;
    pocketparse_push(parser, span);
    result = json_parser_run(parser);
    if (result == POCKETPARSE_OK) {
        tree = pocketparse_parser_tree(parser);
        if (tree != NULL &&
            pocketparse_tree_cursor_new(tree, &cursor) == POCKETPARSE_OK) {
            embed_dump(tree, cursor, base);
            pocketparse_tree_cursor_free(cursor);
        }
    } else {
        printf("(json parse failed)\n");
        rc = 1;
    }
    pocketparse_parser_free(parser);
    pocketparse_close(engine);
    return rc;
}

/* ** Walk the host tree; for every node bound to the `code' field, run the json
** parser on that node's byte span. The span is read straight off the tree
** (start/end byte), so the host program never re-scans the document text -- the
** CST is the data structure. */
static int run_embedded_for_code_fields(const pocketparse_tree *host_tree,
                                        pocketparse_tree_cursor *cursor,
                                        const uint8_t *doc, uint32_t doc_length)
{
    pocketparse_symbol field = pocketparse_tree_cursor_field(cursor);
    int rc = 0;

    if (field.id != POCKETPARSE_NO_FIELD) {
        pocketparse_string fn = pocketparse_tree_symbol_name(host_tree, field);
        if (fn.length == 4 && memcmp(fn.pointer, "code", 4) == 0) {
            uint32_t start = pocketparse_tree_cursor_start_byte(cursor);
            uint32_t end   = pocketparse_tree_cursor_end_byte(cursor);
            uint32_t depth = pocketparse_tree_cursor_depth(cursor);
            if (end <= doc_length && start <= end) {
                printf("--- embedded json parse of code span [%u,%u) ---\n",
                       (unsigned)start, (unsigned)end);
                rc |= parse_embedded(doc + start, end - start, depth);
            }
        }
    }
    if (pocketparse_tree_cursor_goto_first_child(cursor)) {
        do {
            rc |= run_embedded_for_code_fields(host_tree, cursor, doc, doc_length);
        } while (pocketparse_tree_cursor_goto_next_sibling(cursor));
        pocketparse_tree_cursor_goto_parent(cursor);
    }
    return rc;
}

static char *read_all(const char *path, size_t *out_length)
{
    FILE  *file = (strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
    char  *buffer = NULL;
    size_t length = 0, capacity = 0;
    int    own = (file != stdin);
    if (file == NULL) return NULL;
    for (;;) {
        size_t got;
        if (length + 65536 > capacity) {
            size_t nc = capacity == 0 ? 65536 : capacity * 2;
            char *grown = realloc(buffer, nc);
            if (grown == NULL) { free(buffer); if (own) fclose(file); return NULL; }
            buffer = grown; capacity = nc;
        }
        got = fread(buffer + length, 1, 65536, file);
        length += got;
        if (got < 65536) break;
    }
    if (own) fclose(file);
    *out_length = length;
    return buffer;
}

int main(int argc, char **argv)
{
    pocketparse_engine  *engine;
    pocketparse_grammar *grammar = NULL;
    pocketparse_parser  *parser  = NULL;
    pocketparse_string   empty, doc_bytes;
    const pocketparse_tree  *tree;
    pocketparse_tree_cursor *cursor = NULL;
    char  *doc;
    size_t doc_length = 0;
    int    rc = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <document.md>\n", argv[0]);
        return 2;
    }
    doc = read_all(argv[1], &doc_length);
    if (doc == NULL) {
        fprintf(stderr, "embed: cannot read '%s'\n", argv[1]);
        return 2;
    }

    /* Parse the whole document with the HOST parser. */
    engine = embed_open();
    if (engine == NULL) { fputs("embed: out of memory\n", stderr); return 2; }
    if (host_parser_load(engine, &grammar) != POCKETPARSE_OK) {
        fputs("embed: cannot load host grammar\n", stderr);
        return 2;
    }
    empty.pointer = NULL; empty.length = 0;
    if (pocketparse_parser_new(engine, grammar, empty, &parser) != POCKETPARSE_OK
        || parser == NULL) {
        fputs("embed: cannot create host parser\n", stderr);
        return 2;
    }
    doc_bytes.pointer = (const uint8_t *)doc;
    doc_bytes.length  = (uint32_t)doc_length;
    pocketparse_push(parser, doc_bytes);
    if (host_parser_run(parser) != POCKETPARSE_OK) {
        fputs("embed: host parse failed\n", stderr);
        return 1;
    }

    tree = pocketparse_parser_tree(parser);
    if (tree == NULL ||
        pocketparse_tree_cursor_new(tree, &cursor) != POCKETPARSE_OK) {
        fputs("embed: host parse produced no tree\n", stderr);
        return 1;
    }

    /* 1. The host tree. */
    printf("=== host tree (host.scm) ===\n");
    embed_dump(tree, cursor, 0);

    /* 2. For each code span the host found, the embedded json tree. */
    printf("=== embedded parses (std.json) ===\n");
    pocketparse_tree_cursor_reset_to_root(cursor);
    rc = run_embedded_for_code_fields(tree, cursor,
                                      (const uint8_t *)doc, (uint32_t)doc_length);

    pocketparse_tree_cursor_free(cursor);
    free(doc);
    return rc;
}
