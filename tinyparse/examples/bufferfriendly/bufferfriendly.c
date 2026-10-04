/*
** bufferfriendly.c -- the W17-4 BUFFER-FRIENDLY driver.
**
** ONE LESSON: the GENERATED native parser can be driven over a GROWING buffer
** via the NEED_MORE_INPUT (POCKETPARSE_MORE) signal, and the tree it builds that
** way is byte-identical to parsing the whole input at once -- and to the VM.
**
** Native recursion cannot suspend mid-stack and ax has no stackful fiber (its
** continuation is the journal), so the generated parser suspends ONLY at the
** SHALLOW top-level-item boundary -- the start rule's repeat of top-level forms,
** where the C stack is empty. Within a form it runs at full native speed. The
** generated <prefix>_run_chunk parses every form that completes within the buffer,
** then returns POCKETPARSE_MORE; the driver here appends the next chunk and calls
** again, and the parse resumes at the next form. On the final chunk (input_complete
** set) it finalises into the same tree a single whole run produces.
**
** The explicit-stack VM/actor stays the engine for arbitrary-granularity async
** push (chunks arriving mid-form); ax picks the generated native parser for this
** buffer/pull + per-item streaming. This driver exercises the native path.
**
** The grammar is examples/sexpr -- start rule `Source = (seq (repeat Form) EOF)',
** a repeat of top-level items, exactly the shape the buffer-friendly boundary
** suspends at.
**
** This is host dev-tool code: libc is fine.
**
** Usage: bufferfriendly <document.sx> [chunk-size]
**   Prints a pre-order dump of the CST built by driving the generated parser over
**   the document in fixed-size chunks. Exit 0 on a clean parse.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pocketparse.h"
/* internal.h gives the parser struct so the host can set input_complete -- the
** buffer-friendly contract field. A host that drives the generated parser over a
** growing buffer owns this flag (it knows when its input is complete). */
#include "internal.h"

/* The generated sexpr parser-lib entries, under the --name prefix `sx'. */
pocketparse_result sx_load(pocketparse_engine *engine,
                           pocketparse_grammar **out_grammar);
pocketparse_result sx_run(pocketparse_parser *parser);
pocketparse_result sx_run_chunk(pocketparse_parser *parser);

/* The internal language-tag accessor --dump-tree uses (sexpr has no injection, so
** it is always PP_NO_SYMBOL here, but the dump stays byte-identical to the CLI's). */
uint32_t pp_tree_cursor_language_tag(const pocketparse_tree_cursor *cursor);

/* A generous parse arena (BSS: untouched pages cost nothing). */
static unsigned char grant[(size_t)512u * 1024u * 1024u];

/* A source line index, identical to src/cli.c's dump_line_index, so the span this
** example prints matches the CLI / VM byte-for-byte (the bufferfriendly gate
** diffs them). */
typedef struct { uint32_t *starts; uint32_t count;
                 const unsigned char *src; uint32_t src_len; } line_index;

static void line_index_build(line_index *ix, const unsigned char *src, uint32_t len)
{
    uint32_t i, lines = 1;
    ix->src = src; ix->src_len = len; ix->starts = NULL; ix->count = 0;
    for (i = 0; i < len; i++) if (src[i] == '\n') lines++;
    ix->starts = (uint32_t *)malloc(lines * sizeof *ix->starts);
    if (ix->starts == NULL) return;
    ix->starts[0] = 0; ix->count = 1;
    for (i = 0; i < len; i++) if (src[i] == '\n') ix->starts[ix->count++] = i + 1;
}

static void line_col(const line_index *ix, uint32_t byte, uint32_t *ol, uint32_t *oc)
{
    if (byte > ix->src_len) byte = ix->src_len;
    if (ix->starts == NULL) {
        uint32_t i, line = 0, ls = 0;
        for (i = 0; i < byte; i++) if (ix->src[i] == '\n') { line++; ls = i + 1; }
        *ol = line; *oc = byte - ls; return;
    }
    {
        uint32_t lo = 0, hi = ix->count - 1, line = 0;
        while (lo <= hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (ix->starts[mid] <= byte) { line = mid; lo = mid + 1; }
            else { if (mid == 0) break; hi = mid - 1; }
        }
        *ol = line; *oc = byte - ix->starts[line];
    }
}

/* ** Dump one node exactly as the CLI / generated standalone main does: indent by
** depth, MISSING marker, `field: ' prefix, kind name, optional `[language]' tag,
** and the `[s-e] [l,c]-[l,c]' span. */
static void dump_node(const pocketparse_tree *tree, pocketparse_tree_cursor *cursor,
                      const line_index *ix)
{
    uint32_t depth = pocketparse_tree_cursor_depth(cursor);
    uint32_t i;
    pocketparse_symbol kind  = pocketparse_tree_cursor_kind(cursor);
    pocketparse_symbol field = pocketparse_tree_cursor_field(cursor);
    uint32_t flags = pocketparse_tree_cursor_flags(cursor);
    pocketparse_string kn = pocketparse_tree_symbol_name(tree, kind);
    for (i = 0; i < depth; i++) fputs("  ", stdout);
    if (flags & POCKETPARSE_NODE_MISSING) fputs("MISSING ", stdout);
    if (field.id != POCKETPARSE_NO_FIELD) {
        pocketparse_string fn = pocketparse_tree_symbol_name(tree, field);
        printf("%.*s: ", (int)fn.length, (const char *)fn.pointer);
    }
    printf("%.*s", (int)kn.length, (const char *)kn.pointer);
    {
        uint32_t lang = pp_tree_cursor_language_tag(cursor);
        if (lang != (uint32_t)-1) {
            pocketparse_symbol ls; pocketparse_string ln;
            ls.id = lang; ln = pocketparse_tree_symbol_name(tree, ls);
            if (ln.length > 0)
                printf(" [%.*s]", (int)ln.length, (const char *)ln.pointer);
        }
    }
    {
        uint32_t sb = pocketparse_tree_cursor_start_byte(cursor);
        uint32_t eb = pocketparse_tree_cursor_end_byte(cursor);
        uint32_t sl, sc, el, ec;
        line_col(ix, sb, &sl, &sc);
        line_col(ix, eb, &el, &ec);
        printf(" [%u-%u] [%u,%u]-[%u,%u]", sb, eb, sl, sc, el, ec);
    }
    printf("\n");
}

/* ** Pre-order dump of the whole tree, iteratively (cursor-driven). */
static void dump_tree(const pocketparse_tree *tree, const line_index *ix)
{
    pocketparse_tree_cursor *cursor = NULL;
    if (tree == NULL ||
        pocketparse_tree_cursor_new(tree, &cursor) != POCKETPARSE_OK)
        return;
    for (;;) {
        dump_node(tree, cursor, ix);
        if (pocketparse_tree_cursor_goto_first_child(cursor))
            continue;
        for (;;) {
            if (pocketparse_tree_cursor_goto_next_sibling(cursor))
                break;
            if (!pocketparse_tree_cursor_goto_parent(cursor)) {
                pocketparse_tree_cursor_free(cursor);
                return;
            }
        }
    }
}

static pocketparse_engine *open_engine(void)
{
    pocketparse_configuration cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.memory.base = grant;
    cfg.memory.size = sizeof grant;
    return pocketparse_open(cfg);
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "-";
    uint32_t chunk = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 4u;
    FILE *f;
    unsigned char *input = NULL;
    size_t len = 0, cap = 0;
    int ch;
    pocketparse_engine  *engine;
    pocketparse_grammar *grammar = NULL;
    pocketparse_parser  *parser  = NULL;
    pocketparse_string   empty;
    pocketparse_result   result = POCKETPARSE_MORE;
    size_t off;
    int calls = 0, mores = 0;

    if (chunk == 0) chunk = 1;

    f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
    if (f == NULL) { fprintf(stderr, "error: cannot open %s\n", path); return 2; }
    while ((ch = fgetc(f)) != EOF) {
        if (len + 1 > cap) { cap = cap ? cap * 2 : 1024; input = realloc(input, cap); }
        input[len++] = (unsigned char)ch;
    }
    if (f != stdin) fclose(f);

    engine = open_engine();
    if (engine == NULL) { fputs("out of memory\n", stderr); return 1; }
    if (sx_load(engine, &grammar) != POCKETPARSE_OK) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "error: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        return 1;
    }
    empty.pointer = NULL; empty.length = 0;
    if (pocketparse_parser_new(engine, grammar, empty, &parser) != POCKETPARSE_OK ||
        parser == NULL) {
        fputs("error: parser_new failed\n", stderr);
        return 1;
    }

    /* Drive the generated parser over the input in fixed-size chunks. Each chunk:
    ** push the bytes, mark whether the input is now complete, and run. While the
    ** parser still needs more it returns POCKETPARSE_MORE; on the final chunk it
    ** finalises and freezes the tree. */
    for (off = 0; off < len; ) {
        size_t take = (off + chunk < len) ? chunk : (len - off);
        int last = (off + take >= len);
        pocketparse_string bytes;
        bytes.pointer = input + off;
        bytes.length  = (uint32_t)take;
        pocketparse_push(parser, bytes);
        off += take;
        parser->input_complete = last ? 1 : 0;
        result = sx_run_chunk(parser);
        calls++;
        if (result == POCKETPARSE_MORE) mores++;
        if (last) break;
    }
    /* Empty input: one final run with input_complete to finalise an empty tree. */
    if (len == 0) {
        parser->input_complete = 1;
        result = sx_run_chunk(parser);
        calls++;
    }

    fprintf(stderr, "bufferfriendly: %d calls, %d POCKETPARSE_MORE suspensions, "
            "result=%d\n", calls, mores, (int)result);

    if (result == POCKETPARSE_OUT_OF_MEMORY) {
        fprintf(stderr, "error: out of memory\n");
        free(input);
        return 2;
    }
    if (result == POCKETPARSE_SYNTAX_ERROR || result == POCKETPARSE_ERROR) {
        uint32_t count = pocketparse_parser_diagnostic_count(parser), d;
        if (count == 0) fprintf(stderr, "error: parse failed\n");
        for (d = 0; d < count; d++) {
            const pocketparse_diagnostic *diag =
                pocketparse_parser_diagnostic(parser, d);
            if (diag != NULL)
                fprintf(stderr, "%.*s\n", (int)diag->message.length,
                        (const char *)diag->message.pointer);
        }
        free(input);
        return 1;
    }
    if (result == POCKETPARSE_MORE) {
        fprintf(stderr, "error: parser still wants input after the final chunk\n");
        free(input);
        return 1;
    }

    {
        line_index ix;
        line_index_build(&ix, input, (uint32_t)len);
        dump_tree(pocketparse_parser_tree(parser), &ix);
        free(ix.starts);
    }
    free(input);
    return 0;
}
