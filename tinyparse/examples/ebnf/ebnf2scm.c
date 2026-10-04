/*
** ebnf2scm.c -- convert an EBNF grammar into a tinyparse .scm grammar.
**
** This is the first stage of the pipeline example. It is a HOSTED program
** (libc is fine): it reads an .ebnf file, parses it WITH tinyparse using an
** EBNF grammar authored in ebnf.scm, walks the resulting concrete syntax tree
** through the public cursor API, and emits an equivalent tinyparse grammar as
** .scm text on stdout.
**
** The EBNF subset it accepts:
**
**   rule    = name "=" alternation ";"
**   alt     = sequence ("|" sequence)*
**   seq     = factor+
**   factor  = name ":" factor      (a named field)
**           | "[" alt "]"          (optional)
**           | "{" alt "}"          (repetition)
**           | "(" alt ")"          (grouping)
**           | name                 (a rule/token reference)
**           | "..."                (a literal terminal)
**           | /.../                (a regular expression terminal)
**
** Naming convention bridging EBNF and .scm: an all-uppercase identifier names a
** token; any other identifier names a rule. A rule whose body is a single
** terminal or regex is emitted as a (token ...); everything else as a (rule ...).
** The first rule in the file becomes the grammar's start rule. Rule names are
** capitalised so the emitted grammar reads in tinyparse's PascalCase style;
** token names are left verbatim. The same normalisation is applied to every
** reference, so definitions and uses stay in agreement.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyparse.h"

/* ** The embedded EBNF grammar source, compiled at startup. Keeping it inline
** makes the converter self-contained: it needs no companion data file. */
static const char *EBNF_GRAMMAR =
"(tinyparse version: \"0.1\")\n"
"(module examples.ebnf)\n"
"(grammar ebnf\n"
"  start: Grammar\n"
"  (token Space    (regex \"[ \\t\\r\\n]+\"))\n"
"  (token Comment  (seq \"(*\" (repeat (not \"*)\" any)) \"*)\"))\n"
"  (token Ident    (regex \"[A-Za-z][A-Za-z0-9_]*\"))\n"
"  (token Terminal (seq \"\\\"\" (repeat (choice (seq \"\\\\\" any) (not \"\\\"\" any))) \"\\\"\"))\n"
"  (token Regex    (seq \"/\" (repeat (choice (seq \"\\\\\" any) (not \"/\" any))) \"/\"))\n"
"  (extras (trivia Space) (trivia Comment))\n"
"  (rule Grammar (seq (repeat1 Rule) EOF))\n"
"  (rule Rule (seq (field name: Ident) \"=\" (field body: Alternation) \";\"))\n"
"  (rule Alternation (seq (field option: Sequence) (repeat (seq \"|\" (field option: Sequence)))))\n"
"  (rule Sequence (repeat1 (field item: Factor)))\n"
"  (rule Factor (choice (field field: FieldItem) (field opt: Optional) (field rep: Repetition) (field group: Group) (field ref: Ident) (field term: Terminal) (field rx: Regex)))\n"
"  (rule FieldItem (seq (field fname: Ident) \":\" (field fval: Factor)))\n"
"  (rule Optional (seq \"[\" (field inner: Alternation) \"]\"))\n"
"  (rule Repetition (seq \"{\" (field inner: Alternation) \"}\"))\n"
"  (rule Group (seq \"(\" (field inner: Alternation) \")\")))\n";

/* ** The host memory grant. A few megabytes covers any EBNF the example feeds. */
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

/*
** Cursor walking helpers. The tree the EBNF grammar produces contains trivia
** (Space, Comment) nodes in source order; the converter cares only about the
** structural nodes, which it locates by kind name.
*/

/* ** The whole input the parser ran over; used to slice node text. */
static const char *g_input;

/* ** The current tree, for symbol-name lookups. */
static const tinyparse_tree *g_tree;

/* ** Return nonzero if the node under the cursor has the given kind name. */
static int kind_is(tinyparse_tree_cursor *cursor, const char *name)
{
    tinyparse_symbol kind = tinyparse_tree_cursor_kind(cursor);
    tinyparse_string n = tinyparse_tree_symbol_name(g_tree, kind);
    size_t len = strlen(name);
    return n.length == len && memcmp(n.pointer, name, len) == 0;
}

/* ** Copy the source text of the node under the cursor into out (NUL-terminated).
** Returns the length written (clamped to cap-1). */
static size_t node_text(tinyparse_tree_cursor *cursor, char *out, size_t cap)
{
    uint32_t a = tinyparse_tree_cursor_start_byte(cursor);
    uint32_t b = tinyparse_tree_cursor_end_byte(cursor);
    size_t n = (b > a) ? (size_t)(b - a) : 0;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, g_input + a, n);
    out[n] = '\0';
    return n;
}

/* ** Move the cursor to the first child whose kind name matches; restore and
** return 0 if none found, else leave the cursor on the match and return 1. */
static int goto_child_kind(tinyparse_tree_cursor *cursor, const char *name)
{
    if (!tinyparse_tree_cursor_goto_first_child(cursor))
        return 0;
    do {
        if (kind_is(cursor, name))
            return 1;
    } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
    tinyparse_tree_cursor_goto_parent(cursor);
    return 0;
}

/*
** Identifier normalisation. An all-uppercase identifier is a token name and is
** kept verbatim; any other identifier is a rule name and is capitalised. The
** same rule applies to definitions and references so they stay in agreement.
*/

/* ** Return nonzero if every letter in s is uppercase (s holds only A-Z/0-9/_). */
static int is_all_upper(const char *s)
{
    int saw_alpha = 0;
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') return 0;
        if (*s >= 'A' && *s <= 'Z') saw_alpha = 1;
    }
    return saw_alpha;
}

/* ** Write the normalised name of identifier s into out. */
static void normalise(const char *s, char *out, size_t cap)
{
    size_t n = strlen(s);
    if (n > cap - 1) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    if (!is_all_upper(out) && out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 'a' + 'A');
}

/*
** Emission. The walk visits a cursor positioned on a structural node and writes
** the equivalent .scm to stdout. Alternation/Sequence collapse to a bare child
** when they hold a single option/item, matching how a human would write it.
*/

static void emit_alternation(tinyparse_tree_cursor *cursor);

/* ** Emit a Factor node: a field, an optional, a repetition, a group, a
** reference, a terminal, or a regex. */
static void emit_factor(tinyparse_tree_cursor *cursor)
{
    char buf[1024];

    if (kind_is(cursor, "FieldItem")) {
        char fname[256];
        /* (field fname: ...) -> first Ident child is the field name, the Factor
        ** child is the value. */
        tinyparse_tree_cursor_goto_first_child(cursor); /* Ident (fname) */
        while (!kind_is(cursor, "Ident"))
            tinyparse_tree_cursor_goto_next_sibling(cursor);
        /* A field label keeps the author's spelling; do not capitalise it. */
        node_text(cursor, fname, sizeof fname);
        printf("(field %s: ", fname);
        /* advance to the Factor value child */
        while (tinyparse_tree_cursor_goto_next_sibling(cursor)) {
            if (kind_is(cursor, "Factor")) break;
        }
        /* descend into the Factor's single concrete child */
        if (tinyparse_tree_cursor_goto_first_child(cursor)) {
            do {
                if (kind_is(cursor, "FieldItem") || kind_is(cursor, "Optional") ||
                    kind_is(cursor, "Repetition") || kind_is(cursor, "Group") ||
                    kind_is(cursor, "Ident") || kind_is(cursor, "Terminal") ||
                    kind_is(cursor, "Regex")) {
                    emit_factor(cursor);
                    break;
                }
            } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
            tinyparse_tree_cursor_goto_parent(cursor);
        }
        tinyparse_tree_cursor_goto_parent(cursor);
        printf(")");
        return;
    }
    if (kind_is(cursor, "Optional")) {
        printf("(optional ");
        if (goto_child_kind(cursor, "Alternation")) {
            emit_alternation(cursor);
            tinyparse_tree_cursor_goto_parent(cursor);
        }
        printf(")");
        return;
    }
    if (kind_is(cursor, "Repetition")) {
        printf("(repeat ");
        if (goto_child_kind(cursor, "Alternation")) {
            emit_alternation(cursor);
            tinyparse_tree_cursor_goto_parent(cursor);
        }
        printf(")");
        return;
    }
    if (kind_is(cursor, "Group")) {
        if (goto_child_kind(cursor, "Alternation")) {
            emit_alternation(cursor);
            tinyparse_tree_cursor_goto_parent(cursor);
        }
        return;
    }
    if (kind_is(cursor, "Ident")) {
        char norm[256];
        node_text(cursor, buf, sizeof buf);
        normalise(buf, norm, sizeof norm);
        printf("%s", norm);
        return;
    }
    if (kind_is(cursor, "Terminal")) {
        /* Terminal text already includes its surrounding quotes and is valid
        ** .scm string syntax verbatim. */
        node_text(cursor, buf, sizeof buf);
        printf("%s", buf);
        return;
    }
    if (kind_is(cursor, "Regex")) {
        /* /.../ -> (regex "...") with the slashes stripped and any \/ unescaped. */
        size_t n = node_text(cursor, buf, sizeof buf);
        size_t i;
        printf("(regex \"");
        for (i = 1; i + 1 < n; i++) {
            if (buf[i] == '\\' && buf[i + 1] == '/') { putchar('/'); i++; }
            else if (buf[i] == '"') { printf("\\\""); }
            else if (buf[i] == '\\') { printf("\\\\"); putchar(buf[i+1]); i++; }
            else putchar(buf[i]);
        }
        printf("\")");
        return;
    }
}

/* ** Emit one Sequence: a (seq ...) of its items, or the bare item when single. */
static void emit_sequence(tinyparse_tree_cursor *cursor)
{
    /* Count item (Factor) children first to decide whether to wrap in (seq ...). */
    int count = 0;
    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        do { if (kind_is(cursor, "Factor")) count++; }
        while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }

    if (count > 1) printf("(seq ");

    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        int first = 1;
        do {
            if (!kind_is(cursor, "Factor")) continue;
            if (!first) printf(" ");
            first = 0;
            /* descend into Factor's concrete child */
            if (tinyparse_tree_cursor_goto_first_child(cursor)) {
                do {
                    if (kind_is(cursor, "FieldItem") || kind_is(cursor, "Optional") ||
                        kind_is(cursor, "Repetition") || kind_is(cursor, "Group") ||
                        kind_is(cursor, "Ident") || kind_is(cursor, "Terminal") ||
                        kind_is(cursor, "Regex")) {
                        emit_factor(cursor);
                        break;
                    }
                } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
                tinyparse_tree_cursor_goto_parent(cursor);
            }
        } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }

    if (count > 1) printf(")");
}

/* ** Emit one Alternation: a (choice ...) of its options, or the bare option
** when single. */
static void emit_alternation(tinyparse_tree_cursor *cursor)
{
    int count = 0;
    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        do { if (kind_is(cursor, "Sequence")) count++; }
        while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }

    if (count > 1) printf("(choice ");

    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        int first = 1;
        do {
            if (!kind_is(cursor, "Sequence")) continue;
            if (!first) printf(" ");
            first = 0;
            emit_sequence(cursor);
        } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }

    if (count > 1) printf(")");
}

/* ** Return nonzero if a Rule's body is a single bare terminal or regex, which
** marks it as a token rather than a rule. Cursor is positioned on the Rule. */
static int body_is_token(tinyparse_tree_cursor *cursor)
{
    int token = 0;

    if (!goto_child_kind(cursor, "Alternation"))
        return 0;
    /* Alternation -> single Sequence -> single Factor -> Terminal|Regex */
    if (goto_child_kind(cursor, "Sequence")) {
        if (goto_child_kind(cursor, "Factor")) {
            if (tinyparse_tree_cursor_goto_first_child(cursor)) {
                do {
                    if (kind_is(cursor, "Terminal") || kind_is(cursor, "Regex"))
                        token = 1;
                } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
                tinyparse_tree_cursor_goto_parent(cursor);
            }
            tinyparse_tree_cursor_goto_parent(cursor); /* back to Sequence */
        }
        tinyparse_tree_cursor_goto_parent(cursor); /* back to Alternation */
    }
    tinyparse_tree_cursor_goto_parent(cursor); /* back to Rule */
    return token;
}

/* ** Emit one Rule as either a (token ...) or a (rule ...) form, returning the
** normalised name through out_name. Cursor is positioned on the Rule node. */
static void emit_rule(tinyparse_tree_cursor *cursor, char *out_name, size_t cap)
{
    char raw[256], name[256];
    int is_token;

    /* the rule name is the first Ident child */
    if (goto_child_kind(cursor, "Ident")) {
        node_text(cursor, raw, sizeof raw);
        tinyparse_tree_cursor_goto_parent(cursor);
    } else {
        raw[0] = '\0';
    }
    normalise(raw, name, sizeof name);
    if (out_name) { strncpy(out_name, name, cap - 1); out_name[cap - 1] = '\0'; }

    is_token = body_is_token(cursor);

    if (is_token) {
        printf("  (token %s ", name);
        if (goto_child_kind(cursor, "Alternation")) {
            emit_alternation(cursor);
            tinyparse_tree_cursor_goto_parent(cursor);
        }
        printf(")\n");
    } else {
        printf("  (rule %s\n    ", name);
        if (goto_child_kind(cursor, "Alternation")) {
            emit_alternation(cursor);
            tinyparse_tree_cursor_goto_parent(cursor);
        }
        printf(")\n");
    }
}

/* ** Walk the whole Grammar node: emit the header, then split rules into the
** token group and the rule group (tokens must be declared, and the start rule
** is the first non-token rule), printing a working .scm grammar. */
static void emit_grammar(tinyparse_tree_cursor *cursor)
{
    char start[256] = {0};
    int found_start = 0;

    printf("(tinyparse version: \"0.1\")\n\n");
    printf("(module examples.uri)\n\n");

    /* First pass: discover the start rule (first non-token rule). */
    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        do {
            if (!kind_is(cursor, "Rule")) continue;
            if (!body_is_token(cursor)) {
                char raw[256];
                if (goto_child_kind(cursor, "Ident")) {
                    node_text(cursor, raw, sizeof raw);
                    tinyparse_tree_cursor_goto_parent(cursor);
                    normalise(raw, start, sizeof start);
                    found_start = 1;
                    break;
                }
            }
        } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }

    printf("(grammar uri\n");
    if (found_start)
        printf("  start: %s\n\n", start);

    /* Emit token rules first. */
    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        do {
            if (kind_is(cursor, "Rule") && body_is_token(cursor))
                emit_rule(cursor, NULL, 0);
        } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }
    printf("\n");

    /* Then the structural rules. */
    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        do {
            if (kind_is(cursor, "Rule") && !body_is_token(cursor))
                emit_rule(cursor, NULL, 0);
        } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }

    printf(")\n");
}

/* ** Entry point: ebnf2scm <file.ebnf>. */
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
    char  *ebnf_bytes;
    size_t ebnf_length;
    void  *block;

    if (argc < 2) {
        fprintf(stderr, "usage: ebnf2scm <file.ebnf>\n");
        return 2;
    }
    ebnf_bytes = read_all(argv[1], &ebnf_length);
    if (ebnf_bytes == NULL) {
        fprintf(stderr, "ebnf2scm: error: cannot read '%s'\n", argv[1]);
        return 1;
    }

    block = malloc(GRANT);
    if (block == NULL) { fprintf(stderr, "ebnf2scm: out of memory\n"); return 1; }
    memset(&cfg, 0, sizeof cfg);
    cfg.memory.base = block;
    cfg.memory.size = GRANT;
    cfg.memory.grow = grow;
    engine = tinyparse_open(cfg);
    if (engine == NULL) { fprintf(stderr, "ebnf2scm: cannot open engine\n"); return 1; }

    /* Compile the EBNF meta-grammar. */
    source.name  = text("ebnf.scm");
    source.bytes = text(EBNF_GRAMMAR);
    r = tinyparse_compile_scheme(engine, source, &grammar);
    if (r != TINYPARSE_OK) {
        tinyparse_string f = tinyparse_fault(engine);
        fprintf(stderr, "ebnf2scm: grammar error: %.*s\n",
                (int)f.length, (const char *)f.pointer);
        return 1;
    }

    /* Parse the EBNF input. */
    {
        tinyparse_string empty; empty.pointer = NULL; empty.length = 0;
        r = tinyparse_parser_new(engine, grammar, empty, &parser);
    }
    if (r != TINYPARSE_OK || parser == NULL) {
        fprintf(stderr, "ebnf2scm: cannot create parser\n");
        return 1;
    }
    {
        tinyparse_string in;
        in.pointer = (const uint8_t *)ebnf_bytes;
        in.length  = (uint32_t)ebnf_length;
        tinyparse_push(parser, in);
    }
    r = tinyparse_finish(parser);
    if (r == TINYPARSE_SYNTAX_ERROR || r == TINYPARSE_ERROR) {
        uint32_t count = tinyparse_parser_diagnostic_count(parser), d;
        fprintf(stderr, "ebnf2scm: the EBNF input did not parse\n");
        for (d = 0; d < count; d++) {
            const tinyparse_diagnostic *diag = tinyparse_parser_diagnostic(parser, d);
            if (diag)
                fprintf(stderr, "  %.*s\n", (int)diag->message.length,
                        (const char *)diag->message.pointer);
        }
        return 1;
    }

    tree = tinyparse_parser_tree(parser);
    if (tree == NULL) { fprintf(stderr, "ebnf2scm: no tree\n"); return 1; }
    if (tinyparse_tree_cursor_new(tree, &cursor) != TINYPARSE_OK) {
        fprintf(stderr, "ebnf2scm: cannot make cursor\n");
        return 1;
    }

    g_input = ebnf_bytes;
    g_tree  = tree;

    /* The root is the Grammar node. */
    emit_grammar(cursor);

    tinyparse_tree_cursor_free(cursor);
    tinyparse_parser_free(parser);
    tinyparse_grammar_free(grammar);
    tinyparse_close(engine);
    free(block);
    free(ebnf_bytes);
    return 0;
}
