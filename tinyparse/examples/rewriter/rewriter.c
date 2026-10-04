/*
** rewriter.c -- a #define -> enum codemod driven by a pocketparse CST.
**
** It reads a C source file, parses it with pocketparse against the
** line-oriented grammar in cdefines.scm, and rewrites it: every
** object-like integer macro
**
**     #define NAME 123
**
** is removed, and the whole set is replaced by a single enum
**
**     enum { NAME = 123, ... };
**
** placed where the first macro stood. Every other line passes through
** verbatim.
**
** The rewrite is driven by the concrete syntax tree, not by re-scanning
** the text in C: the program walks the tree with the public cursor API,
** classifies each Line node as a Define or a Raw line, and reads the
** macro name and value out of the Define's `name' and `value' fields. The
** byte spans the tree records are the only thing it copies from the
** original source.
**
** This is a hosted tool, so libc is fine here. The grammar source is
** embedded at build time (see build.sh, which generates cdefines_scm.h),
** so the program needs no path to the .scm file at runtime.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pocketparse.h"
#include "cdefines_scm.h"

/* ** The host memory grant. A few megabytes is ample for the source files
** this codemod handles; the library never allocates outside it unless grow
** is called. */
#define REWRITER_GRANT (16u * 1024u * 1024u)

/* ** grow callback: hand the library a fresh block when its arena runs out.
** The process exit reclaims it; it is never freed individually. */
static void *rewriter_grow(void *user, size_t need, size_t *got)
{
    (void)user;
    if (got != 0) *got = need;
    return malloc(need);
}

/* ** One collected integer macro: the byte spans of its name and value
** within the original source. Spans, not copies, keep the rewrite anchored
** to the bytes the tree actually saw. */
typedef struct macro_define {
    uint32_t name_start, name_end;
    uint32_t value_start, value_end;
} macro_define;

/* ** Everything the tree walk gathers: the source bytes, the list of
** integer macros in source order, and where the first one began (so the
** enum lands in its place). */
typedef struct rewrite_plan {
    const uint8_t *source;
    uint32_t       source_length;

    macro_define  *defines;
    uint32_t       define_count;
    uint32_t       define_capacity;
} rewrite_plan;

/* ** Wrap a NUL-terminated C string as a pocketparse_string. */
static pocketparse_string text_of(const char *s)
{
    pocketparse_string out;
    out.pointer = (const uint8_t *)s;
    out.length  = (uint32_t)strlen(s);
    return out;
}

/* ** Read an entire file into a malloc'd buffer. Writes the length and
** returns the buffer, or NULL on error. */
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
            buffer = grown;
            capacity = nc;
        }
        got = fread(buffer + length, 1, 65536, file);
        length += got;
        if (got < 65536)
            break;
    }
    fclose(file);
    *out_length = length;
    return buffer;
}

/* ** Open an engine over a freshly allocated grant. Returns NULL on
** failure; the grant block is intentionally leaked to the process. */
static pocketparse_engine *open_engine(void)
{
    pocketparse_configuration configuration;
    void *block = malloc(REWRITER_GRANT);
    if (block == NULL)
        return NULL;
    memset(&configuration, 0, sizeof configuration);
    configuration.memory.base = block;
    configuration.memory.size = REWRITER_GRANT;
    configuration.memory.grow = rewriter_grow;
    configuration.flags       = 0;
    return pocketparse_open(configuration);
}

/* ** Does the symbol under the cursor have the given kind name? */
static int cursor_kind_is(const pocketparse_tree *tree,
                          const pocketparse_tree_cursor *cursor,
                          const char *name)
{
    pocketparse_symbol kind = pocketparse_tree_cursor_kind(cursor);
    pocketparse_string have = pocketparse_tree_symbol_name(tree, kind);
    size_t want = strlen(name);
    return have.length == want &&
           memcmp(have.pointer, name, want) == 0;
}

/* ** Append a macro to the plan, growing the array as needed. Returns 0 on
** allocation failure. */
static int plan_add(rewrite_plan *plan, macro_define def)
{
    if (plan->define_count == plan->define_capacity) {
        uint32_t nc = plan->define_capacity == 0 ? 16 : plan->define_capacity * 2;
        macro_define *grown = realloc(plan->defines, nc * sizeof *grown);
        if (grown == NULL)
            return 0;
        plan->defines = grown;
        plan->define_capacity = nc;
    }
    plan->defines[plan->define_count++] = def;
    return 1;
}

/* ** With the cursor on a Define node, read its `name' and `value' fields
** by name and record their byte spans in the plan. Returns 0 on failure
** (a malformed Define missing a field is skipped, not fatal). */
static int collect_define(pocketparse_tree_cursor *cursor,
                          rewrite_plan *plan)
{
    macro_define def;

    /* Move to the `name' field, read its span, then back to the Define. */
    if (!pocketparse_tree_cursor_goto_first_child_by_field(cursor, text_of("name")))
        return 0;
    def.name_start = pocketparse_tree_cursor_start_byte(cursor);
    def.name_end   = pocketparse_tree_cursor_end_byte(cursor);
    pocketparse_tree_cursor_goto_parent(cursor);

    if (!pocketparse_tree_cursor_goto_first_child_by_field(cursor, text_of("value"))) {
        return 0;
    }
    def.value_start = pocketparse_tree_cursor_start_byte(cursor);
    def.value_end   = pocketparse_tree_cursor_end_byte(cursor);
    pocketparse_tree_cursor_goto_parent(cursor);

    return plan_add(plan, def);
}

/* ** Emit the collected macros as a single enum. */
static void emit_enum(const rewrite_plan *plan)
{
    uint32_t i;
    fputs("enum {\n", stdout);
    for (i = 0; i < plan->define_count; i++) {
        const macro_define *d = &plan->defines[i];
        printf("    %.*s = %.*s%s\n",
               (int)(d->name_end - d->name_start),
               (const char *)(plan->source + d->name_start),
               (int)(d->value_end - d->value_start),
               (const char *)(plan->source + d->value_start),
               (i + 1 < plan->define_count) ? "," : "");
    }
    fputs("};\n", stdout);
}

/* ** First pass: walk the Source node's Line children and gather every
** integer macro, in source order, into the plan. The cursor ends back at
** the root. Returns 0 only on an empty tree. */
static int collect_pass(const pocketparse_tree *tree,
                        pocketparse_tree_cursor *cursor,
                        rewrite_plan *plan)
{
    if (!pocketparse_tree_cursor_goto_first_child(cursor))
        return 0; /* empty input */

    do {
        if (!cursor_kind_is(tree, cursor, "Line"))
            continue; /* EOF marker or stray node */
        if (pocketparse_tree_cursor_goto_first_child(cursor)) {
            if (cursor_kind_is(tree, cursor, "Define"))
                collect_define(cursor, plan);
            pocketparse_tree_cursor_goto_parent(cursor); /* back to Line */
        }
    } while (pocketparse_tree_cursor_goto_next_sibling(cursor));

    pocketparse_tree_cursor_reset_to_root(cursor);
    return 1;
}

/* ** Second pass: emit the rewritten source. Raw lines are copied verbatim
** from the bytes their span covers; #define lines are dropped, and the
** single enum is emitted in place of the first one. Runs only after the
** collect pass has filled the plan, so the enum holds every macro. */
static void emit_pass(const pocketparse_tree *tree,
                      pocketparse_tree_cursor *cursor,
                      const rewrite_plan *plan)
{
    int enum_emitted = 0;

    if (!pocketparse_tree_cursor_goto_first_child(cursor))
        return;

    do {
        if (!cursor_kind_is(tree, cursor, "Line"))
            continue;
        if (pocketparse_tree_cursor_goto_first_child(cursor)) {
            if (cursor_kind_is(tree, cursor, "Define")) {
                /* Drop the #define line; emit the enum once, where the
                ** first macro stood. */
                if (!enum_emitted) {
                    emit_enum(plan);
                    enum_emitted = 1;
                }
            } else {
                /* Raw line: copy the bytes its span covers through
                ** unchanged, including the trailing newline. */
                uint32_t span_start = pocketparse_tree_cursor_start_byte(cursor);
                uint32_t span_end   = pocketparse_tree_cursor_end_byte(cursor);
                fwrite(plan->source + span_start, 1,
                       (size_t)(span_end - span_start), stdout);
            }
            pocketparse_tree_cursor_goto_parent(cursor); /* back to Line */
        }
    } while (pocketparse_tree_cursor_goto_next_sibling(cursor));
}

/* ** Parse the input bytes and build the tree. Returns the parser (with its
** tree) or NULL, reporting faults/diagnostics to stderr. */
static pocketparse_parser *parse_input(pocketparse_engine *engine,
                                       const char *source_bytes,
                                       size_t source_length)
{
    pocketparse_grammar *grammar = NULL;
    pocketparse_parser  *parser  = NULL;
    pocketparse_source   grammar_source;
    pocketparse_string   chunk, start;
    pocketparse_result   result;

    grammar_source.name = text_of("cdefines.scm");
    grammar_source.bytes.pointer = cdefines_scm;
    grammar_source.bytes.length  = (uint32_t)cdefines_scm_length;
    result = pocketparse_compile_scheme(engine, grammar_source, &grammar);
    if (result != POCKETPARSE_OK) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "rewriter: grammar error: %.*s\n",
                (int)fault.length, (const char *)fault.pointer);
        return NULL;
    }

    start.pointer = NULL;
    start.length  = 0;
    result = pocketparse_parser_new(engine, grammar, start, &parser);
    if (result != POCKETPARSE_OK || parser == NULL) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "rewriter: parser error: %.*s\n",
                (int)fault.length, (const char *)fault.pointer);
        return NULL;
    }

    chunk.pointer = (const uint8_t *)source_bytes;
    chunk.length  = (uint32_t)source_length;
    pocketparse_push(parser, chunk);

    result = pocketparse_finish(parser);
    if (result == POCKETPARSE_SYNTAX_ERROR || result == POCKETPARSE_ERROR) {
        uint32_t count = pocketparse_parser_diagnostic_count(parser);
        uint32_t d;
        for (d = 0; d < count; d++) {
            const pocketparse_diagnostic *diag =
                pocketparse_parser_diagnostic(parser, d);
            if (diag != NULL)
                fprintf(stderr, "rewriter: %.*s\n",
                        (int)diag->message.length,
                        (const char *)diag->message.pointer);
        }
        return NULL;
    }
    return parser;
}

/* ** Entry point: rewriter <file.c>. */
int main(int argc, char **argv)
{
    pocketparse_engine      *engine;
    pocketparse_parser      *parser;
    const pocketparse_tree  *tree;
    pocketparse_tree_cursor *cursor = NULL;
    rewrite_plan             plan;
    char  *source_bytes;
    size_t source_length;

    if (argc < 2) {
        fprintf(stderr, "usage: rewriter <file.c>\n");
        return 2;
    }
    source_bytes = read_all(argv[1], &source_length);
    if (source_bytes == NULL) {
        fprintf(stderr, "rewriter: cannot read '%s'\n", argv[1]);
        return 1;
    }

    engine = open_engine();
    if (engine == NULL) {
        fprintf(stderr, "rewriter: out of memory\n");
        return 1;
    }

    parser = parse_input(engine, source_bytes, source_length);
    if (parser == NULL)
        return 1;

    tree = pocketparse_parser_tree(parser);
    if (tree == NULL ||
        pocketparse_tree_cursor_new(tree, &cursor) != POCKETPARSE_OK) {
        fprintf(stderr, "rewriter: no tree\n");
        return 1;
    }

    memset(&plan, 0, sizeof plan);
    plan.source        = (const uint8_t *)source_bytes;
    plan.source_length = (uint32_t)source_length;

    /* Two passes over the same tree: collect every macro, then emit the
    ** rewrite with one enum standing in for them all. */
    collect_pass(tree, cursor, &plan);
    emit_pass(tree, cursor, &plan);

    pocketparse_tree_cursor_free(cursor);
    return 0;
}
