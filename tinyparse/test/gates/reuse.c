/*
** reuse.c -- the engine-reuse harness of `mise run gate:reuse-regression`.
**
** Reuses ONE default engine across many parses of one large input with a
** generated parser, and checks that every tree is the first one (its node
** count and an order-sensitive fingerprint of its kinds), that no push,
** parser_new or run fails, and that the engine's memory grew beyond its
** first grant, so the reuse crossed the grow boundary.
**
** Usage: reuse INPUT REPETITIONS
** The generated parser is linked in as parser_load / parser_run
** (`tinyparse c GRAMMAR --parser-lib --name parser`).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinyparse.h"

/* The generated parser's two entries (emitted with --parser-lib --name parser). */
tinyparse_result parser_load(tinyparse_engine *, tinyparse_grammar **);
tinyparse_result parser_run(tinyparse_parser *);

/* Deliberately small: the size at which the old mailbox copy fragmented and
** failed, and it makes the pool grow and recycle chunks. */
#define GRANT (8u * 1024u * 1024u)

/* How many times the engine asked the host for more memory. */
static unsigned long grows;

static tinyparse_string text(const char *s)
{
    tinyparse_string t;
    t.pointer = (const uint8_t *)s;
    t.length  = (uint32_t)strlen(s);
    return t;
}
static void *grow(void *user, size_t n, size_t *got)
{
    void *p = malloc(n);
    (void)user;
    grows++;
    if (got) *got = n;
    return p;
}
static int class_alpha(void *u, uint32_t b)
{
    (void)u;
    return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '_' || b == '$';
}
static int class_digit(void *u, uint32_t b) { (void)u; return b >= '0' && b <= '9'; }
static int class_alnum(void *u, uint32_t b) { return class_alpha(u, b) || class_digit(u, b); }
static int class_space(void *u, uint32_t b)
{
    (void)u;
    return b == ' ' || b == '\t' || b == '\n' || b == '\r';
}
static void bind_class(tinyparse_engine *e, const char *n, tinyparse_class_fn f)
{
    tinyparse_binding b;
    b.name = text(n); b.kind = TINYPARSE_BIND_CLASS; b.function = f; b.user = NULL;
    tinyparse_bind(e, &b);
}

/* Walk the tree, accumulating a node count and an order-sensitive fingerprint. */
static unsigned long walk(tinyparse_tree_cursor *c, unsigned long *hash)
{
    unsigned long n = 1;
    tinyparse_symbol k = tinyparse_tree_cursor_kind(c);
    *hash = (*hash * 1099511628211UL) ^ (unsigned long)k.id;
    if (tinyparse_tree_cursor_goto_first_child(c)) {
        do { n += walk(c, hash); } while (tinyparse_tree_cursor_goto_next_sibling(c));
        tinyparse_tree_cursor_goto_parent(c);
    }
    return n;
}

static char *read_file(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    char *b = NULL; size_t cap = 0, n = 0, g;
    if (!f) return NULL;
    for (;;) {
        if (n + 65536 + 1 > cap) { cap = cap ? cap * 2 : 131072; b = realloc(b, cap); }
        g = fread(b + n, 1, 65536, f); n += g;
        if (g < 65536) break;
    }
    fclose(f); b[n] = 0; *len = (uint32_t)n;
    return b;
}

int main(int argc, char **argv)
{
    uint32_t flen;
    char *fb;
    tinyparse_configuration cfg;
    void *block;
    tinyparse_engine *engine;
    tinyparse_grammar *grammar = NULL;
    tinyparse_string by;
    unsigned long first_nodes = 0, first_hash = 0;
    int i, repetitions, failures = 0;

    if (argc < 3) { fprintf(stderr, "usage: %s INPUT REPETITIONS\n", argv[0]); return 2; }
    repetitions = atoi(argv[2]);
    if (repetitions < 2) { fprintf(stderr, "reuse: REPETITIONS must be at least 2\n"); return 2; }
    fb = read_file(argv[1], &flen);
    if (!fb) { fprintf(stderr, "could not read %s\n", argv[1]); return 2; }

    memset(&cfg, 0, sizeof cfg);
    block = malloc(GRANT);
    cfg.memory.base = block; cfg.memory.size = GRANT; cfg.memory.grow = grow;
    engine = tinyparse_open(cfg);
    if (engine == 0) { fprintf(stderr, "open failed\n"); return 2; }
    /* A plain tinyparse_open: the default configuration is the shipping path. */
    bind_class(engine, "alpha", class_alpha);
    bind_class(engine, "digit", class_digit);
    bind_class(engine, "alnum", class_alnum);
    bind_class(engine, "space", class_space);

    if (parser_load(engine, &grammar) != TINYPARSE_OK) {
        fprintf(stderr, "grammar load failed\n"); return 2;
    }

    by.pointer = (const uint8_t *)fb; by.length = flen;
    for (i = 0; i < repetitions; i++) {
        tinyparse_parser *p = NULL;
        tinyparse_result rn = tinyparse_parser_new(engine, grammar, text(""), &p);
        tinyparse_result rp = tinyparse_push(p, by);
        tinyparse_result rr = parser_run(p);
        const tinyparse_tree *t = tinyparse_parser_tree(p);
        unsigned long nodes = 0, hash = 1469598103934665603UL;
        if (t) {
            tinyparse_tree_cursor *cur = NULL;
            if (tinyparse_tree_cursor_new(t, &cur) == TINYPARSE_OK) {
                nodes = walk(cur, &hash);
                tinyparse_tree_cursor_free(cur);
            }
        }
        if (i == 0) {
            first_nodes = nodes; first_hash = hash;
            printf("  fresh parse: nodes=%lu fingerprint=%lx\n", first_nodes, first_hash);
            if (first_nodes <= 1) {
                printf("  FAIL: fresh parse is already degenerate (the input did not parse)\n");
                failures++;
            }
        } else if (rn != TINYPARSE_OK || rp != TINYPARSE_OK ||
                   rr != TINYPARSE_OK || nodes != first_nodes || hash != first_hash) {
            tinyparse_string fault = tinyparse_fault(engine);
            printf("  FAIL rep %d: new=%d push=%d run=%d nodes=%lu fingerprint=%lx fault='%.*s'\n",
                   i, rn, rp, rr, nodes, hash,
                   (int)fault.length, (const char *)fault.pointer);
            failures++;
        }
        tinyparse_parser_free(p);
    }
    tinyparse_close(engine);
    free(block); free(fb);

    if (failures != 0) {
        printf("  %d of %d reused parses diverged from the fresh parse\n", failures, repetitions);
        return 1;
    }
    if (grows == 0) {
        printf("  FAIL: the engine never grew past its %u-byte grant; the input is too small to test reuse\n", GRANT);
        return 1;
    }
    printf("  all %d reused parses byte-identical to the fresh parse (the engine grew %lu times)\n",
           repetitions, grows);
    return 0;
}
