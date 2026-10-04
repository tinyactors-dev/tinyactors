/*
** nesting.c -- the W17-3 NATIVE NESTING driver.
**
** ONE LESSON: a GENERATED host parser nests GENERATED embedded parsers, in one
** binary, coordinated by the engine's language registry -- and the composite tree is
** byte-identical to the VM's.
**
** This is the inject hook the codegen blueprint describes. The host grammar
** std.fence carries (inject grammar: std.javascript ...) and (inject grammar:
** std.json ...): a fenced block's content is re-parsed by the named embedded
** language and spliced into one composite tree, every node tagged with the
** language that produced it. Here ALL THREE parsers are SEPARATELY-COMPILED
** generated C:
**
**   - fence_host_*  drives std.fence   (the host),
**   - json_lang_*   drives std.json    (an embedded language),
**   - js_lang_*     drives std.javascript (an embedded language).
**
** The host binary NAMES the embedded languages (the inject form's `grammar:'
** constant) but is NOT linked against their parsers directly. The wiring happens
** at RUNTIME: the driver registers each embedded language on the engine with
** tinyparse_register_language(engine, "std.json", {grammar, json_lang_run}).
** When the generated host parser reaches an inject site, the inject runtime
** resolves the language by name in that registry and drives the registered
** generated parser over the region, sharing the host arena so the subtree splices
** in place. A bare function-pointer table could not do the by-name, separately-
** compiled composition; the registry can, on the engine that owns the arena.
**
** The point of the gate around this driver: the tree this prints must match,
** byte for byte, what the VM prints for the SAME std.fence grammar on the SAME
** document (`tinyparse run std.fence --dump-tree'). Generated host nesting
** generated embedded yields the same composite tree as the dynamic VM -- nothing
** about native nesting changes the CST.
**
** This is host dev-tool code: libc is fine.
**
** Usage: nesting <document.md>   (or stdin when the path is "-")
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyparse.h"

/* ** The generated load/run entries each parser-lib TU exposes, under its own
** --name prefix (so three full parser symbol sets coexist with no collision --
** the W17-3 default prefix would already disjoint them, but the example names them
** explicitly for clarity). fence_host_* is the host; the other two are embedded
** languages registered at runtime. */
tinyparse_result fence_host_load(tinyparse_engine *engine,
                                   tinyparse_grammar **out_grammar);
tinyparse_result fence_host_run(tinyparse_parser *parser);
tinyparse_result json_lang_load(tinyparse_engine *engine,
                                  tinyparse_grammar **out_grammar);
tinyparse_result json_lang_run(tinyparse_parser *parser);
tinyparse_result js_lang_load(tinyparse_engine *engine,
                                tinyparse_grammar **out_grammar);
tinyparse_result js_lang_run(tinyparse_parser *parser);

/* ** The internal language-tag accessor the CLI's --dump-tree uses (W62). It is
** a tp_ symbol the static library exports, declared here exactly as src/cli.c
** declares it, so this driver can print the SAME `[lang]' annotation the VM dump
** prints and the gate can compare the two byte for byte. */
uint32_t tp_tree_cursor_language_tag(const tinyparse_tree_cursor *cursor);

#define NESTING_NO_LANGUAGE_TAG 0xFFFFFFFFu
#define NESTING_GRANT (16u * 1024u * 1024u)

static void *nesting_grow(void *user, size_t need, size_t *got)
{
    (void)user;
    if (got != 0) *got = need;
    return malloc(need);
}

/* ** Open a fresh engine over its own grant. */
static tinyparse_engine *nesting_open(void)
{
    tinyparse_configuration cfg;
    void *block = malloc(NESTING_GRANT);
    if (block == NULL)
        return NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.memory.base = block;
    cfg.memory.size = NESTING_GRANT;
    cfg.memory.grow = nesting_grow;
    cfg.memory.user = NULL;
    cfg.memory.chunk_minimum = 0;
    cfg.flags = 0;
    return tinyparse_open(cfg);
}

/* ** A source line index, identical to src/cli.c's dump_line_index: the byte
** offset at which each line begins, so a node byte offset converts to a 0-based
** (line, column) without rescanning the source per node. Both this driver and the
** CLI dumper print the SAME span shape, so the nesting gate's byte-identity holds. */
typedef struct nesting_line_index {
    uint32_t *starts;
    uint32_t  count;
    const uint8_t *src;
    uint32_t  src_len;
} nesting_line_index;

/* ** Build the line index over `src' (matches src/cli.c's dump_line_index_build). */
static void nesting_line_index_build(nesting_line_index *index,
                                     const uint8_t *src, uint32_t src_len)
{
    uint32_t i, lines = 1;
    index->src = src;
    index->src_len = src_len;
    index->starts = NULL;
    index->count = 0;
    for (i = 0; i < src_len; i++)
        if (src[i] == '\n')
            lines++;
    index->starts = (uint32_t *)malloc(lines * sizeof *index->starts);
    if (index->starts == NULL)
        return;
    index->starts[0] = 0;
    index->count = 1;
    for (i = 0; i < src_len; i++)
        if (src[i] == '\n')
            index->starts[index->count++] = i + 1;
}

/* ** Release the line index. */
static void nesting_line_index_free(nesting_line_index *index)
{
    free(index->starts);
    index->starts = NULL;
}

/* ** Byte offset -> 0-based (line, column) (matches src/cli.c's dump_line_col). */
static void nesting_line_col(const nesting_line_index *index, uint32_t byte,
                             uint32_t *out_line, uint32_t *out_col)
{
    if (byte > index->src_len)
        byte = index->src_len;
    if (index->starts == NULL) {
        uint32_t i, line = 0, line_start = 0;
        for (i = 0; i < byte; i++)
            if (index->src[i] == '\n') { line++; line_start = i + 1; }
        *out_line = line;
        *out_col  = byte - line_start;
        return;
    }
    {
        uint32_t lo = 0, hi = index->count - 1, line = 0;
        while (lo <= hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (index->starts[mid] <= byte) { line = mid; lo = mid + 1; }
            else { if (mid == 0) break; hi = mid - 1; }
        }
        *out_line = line;
        *out_col  = byte - index->starts[line];
    }
}

/* ** Print a node span in the same `[s-e] [l,c]-[l,c]' shape as src/cli.c. */
static void nesting_span(const nesting_line_index *index,
                         uint32_t start_byte, uint32_t end_byte)
{
    uint32_t sl, sc, el, ec;
    nesting_line_col(index, start_byte, &sl, &sc);
    nesting_line_col(index, end_byte, &el, &ec);
    printf(" [%u-%u] [%u,%u]-[%u,%u]",
           start_byte, end_byte, sl, sc, el, ec);
}

/* ** Dump the subtree under the cursor in pre-order, one node per line, indented by
** depth, printing "field: kind" when a field is bound, a trailing "[lang]" for
** an embedded-language node, and the node's span -- the SAME shape src/cli.c's
** dump_subtree prints, so this driver's tree compares byte-identical to
** `run --dump-tree'. */
static void nesting_dump(const tinyparse_tree *tree,
                         tinyparse_tree_cursor *cursor,
                         const nesting_line_index *index)
{
    uint32_t depth = tinyparse_tree_cursor_depth(cursor);
    uint32_t i;
    tinyparse_symbol kind  = tinyparse_tree_cursor_kind(cursor);
    tinyparse_symbol field = tinyparse_tree_cursor_field(cursor);
    uint32_t flags = tinyparse_tree_cursor_flags(cursor);
    tinyparse_string kn = tinyparse_tree_symbol_name(tree, kind);

    for (i = 0; i < depth; i++) fputs("  ", stdout);
    if (flags & TINYPARSE_NODE_MISSING)
        fputs("MISSING ", stdout);
    if (field.id != TINYPARSE_NO_FIELD) {
        tinyparse_string fn = tinyparse_tree_symbol_name(tree, field);
        printf("%.*s: ", (int)fn.length, (const char *)fn.pointer);
    }
    printf("%.*s", (int)kn.length, (const char *)kn.pointer);
    {
        uint32_t lang_id = tp_tree_cursor_language_tag(cursor);
        if (lang_id != NESTING_NO_LANGUAGE_TAG) {
            tinyparse_symbol ls;
            tinyparse_string ln;
            ls.id = lang_id;
            ln = tinyparse_tree_symbol_name(tree, ls);
            if (ln.length > 0)
                printf(" [%.*s]", (int)ln.length, (const char *)ln.pointer);
        }
    }
    nesting_span(index,
                 tinyparse_tree_cursor_start_byte(cursor),
                 tinyparse_tree_cursor_end_byte(cursor));
    printf("\n");

    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        do {
            nesting_dump(tree, cursor, index);
        } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }
}

/* ** Register one embedded language: load its grammar through the generated loader
** (so the registered grammar is the one the generated parser's baked indices read)
** and register that grammar plus the generated run function under the language name
** the host grammar's inject form carries. Returns 0 on success. */
/* ** How many regions each registered parser parsed. The driver registers these
** counting wrappers rather than the generated run entries themselves and reports
** the counts on stderr, so the gate can tell native nesting from the inject
** runtime's fallback to the VM, which builds the same tree. */
static unsigned json_regions, js_regions;

static tinyparse_result count_json_run(tinyparse_parser *parser)
{
    json_regions++;
    return json_lang_run(parser);
}

static tinyparse_result count_js_run(tinyparse_parser *parser)
{
    js_regions++;
    return js_lang_run(parser);
}

static int register_language(tinyparse_engine *engine, const char *name,
                             tinyparse_result (*load)(tinyparse_engine *,
                                                        tinyparse_grammar **),
                             tinyparse_result (*run)(tinyparse_parser *))
{
    tinyparse_language_entry entry;
    tinyparse_grammar       *grammar = NULL;
    tinyparse_string         lang;

    if (load(engine, &grammar) != TINYPARSE_OK || grammar == NULL) {
        fprintf(stderr, "nesting: cannot load embedded grammar '%s'\n", name);
        return 1;
    }
    entry.grammar = grammar;
    entry.parse   = run;            /* the GENERATED embedded parser's run entry */
    lang.pointer  = (const uint8_t *)name;
    lang.length   = (uint32_t)strlen(name);
    if (tinyparse_register_language(engine, lang, &entry) != TINYPARSE_OK) {
        fprintf(stderr, "nesting: cannot register language '%s'\n", name);
        return 1;
    }
    return 0;
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
    tinyparse_engine  *engine;
    tinyparse_grammar *host_grammar = NULL;
    tinyparse_parser  *parser  = NULL;
    tinyparse_string   empty, doc_bytes;
    const tinyparse_tree  *tree;
    tinyparse_tree_cursor *cursor = NULL;
    char  *doc;
    size_t doc_length = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <document.md>\n", argv[0]);
        return 2;
    }
    doc = read_all(argv[1], &doc_length);
    if (doc == NULL) {
        fprintf(stderr, "nesting: cannot read '%s'\n", argv[1]);
        return 2;
    }

    engine = nesting_open();
    if (engine == NULL) { fputs("nesting: out of memory\n", stderr); return 2; }

    /* Load the host grammar through its generated loader, then register the two
    ** embedded languages so the host parser's inject sites resolve them at runtime
    ** to the SEPARATELY-COMPILED generated parsers. The host parser names the
    ** languages; the registry links them. */
    if (fence_host_load(engine, &host_grammar) != TINYPARSE_OK) {
        fputs("nesting: cannot load host grammar\n", stderr);
        return 2;
    }
    if (register_language(engine, "std.json", json_lang_load, count_json_run) ||
        register_language(engine, "std.javascript", js_lang_load, count_js_run))
        return 2;

    empty.pointer = NULL; empty.length = 0;
    if (tinyparse_parser_new(engine, host_grammar, empty, &parser)
            != TINYPARSE_OK || parser == NULL) {
        fputs("nesting: cannot create host parser\n", stderr);
        return 2;
    }
    doc_bytes.pointer = (const uint8_t *)doc;
    doc_bytes.length  = (uint32_t)doc_length;
    tinyparse_push(parser, doc_bytes);

    /* Run the GENERATED host parser. Its baked inject sites call the inject
    ** runtime, which resolves each fence's language in the registry and drives the
    ** GENERATED embedded parser over the fence content, splicing the subtree in
    ** place -- native nesting, no VM in the loop. */
    if (fence_host_run(parser) != TINYPARSE_OK) {
        fputs("nesting: host parse failed\n", stderr);
        return 1;
    }

    tree = tinyparse_parser_tree(parser);
    if (tree == NULL ||
        tinyparse_tree_cursor_new(tree, &cursor) != TINYPARSE_OK) {
        fputs("nesting: host parse produced no tree\n", stderr);
        return 1;
    }
    {
        nesting_line_index index;
        nesting_line_index_build(&index, (const uint8_t *)doc,
                                 (uint32_t)doc_length);
        nesting_dump(tree, cursor, &index);
        nesting_line_index_free(&index);
    }

    fprintf(stderr, "nesting: generated parsers ran on std.json=%u std.javascript=%u regions\n",
            json_regions, js_regions);
    tinyparse_tree_cursor_free(cursor);
    tinyparse_parser_free(parser);
    tinyparse_close(engine);
    free(doc);
    return 0;
}
