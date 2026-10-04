#!/usr/bin/env bash
# ci/tinyparse-reuse-regression.sh — the engine-reuse oracle.
#
# A benchmark's byte-identical assertion once caught a SILENT data-corruption bug
# on the default parse path. Back then the bytecode VM ran inside an actor of the
# actor runtime tinyparse was born in, and tinyparse_push marshalled each input
# chunk through that runtime's mailbox, which copied the bytes into a FIXED slice
# tinyparse_open carved off the grant. Reusing ONE engine across many parses of a
# large file fragmented that slice (its first-fit free list let a tiny allocation
# claim a freed file-sized chunk) until a chunk-sized copy failed, push returned
# TINYPARSE_INVALID — and finish STILL produced a one-node tree with no fault. A
# 90 KB file that yields ~96k nodes collapsed to a 1-node root, silently.
#
# The actor and its slice are gone: a push buffers straight into the parser's own
# arena (a child of the growable engine pool, reset by tinyparse_parser_free),
# and the parser lifecycle (src/parse.c) moves a parser whose push failed to a
# state from which finish/run return the failure instead of a degenerate tree.
#
# This gate is the standing oracle that both properties hold. It reuses ONE
# default engine across enough parses of a pool-growing file to cross the grow
# boundary many times over, and asserts every tree is byte-identical (node count +
# structural fingerprint) to the FIRST parse, with no push silently failing. A
# regression — a dropped chunk, a degenerate tree, an unsurfaced failure — turns it
# red. The input is generated here (a large, valid .tsx of repeated statements), so
# the gate is self-contained and does not depend on any vendored corpus.
#
# Run it with:
#     mise run gate reuse-regression
#
# Exit 0 only when every reused parse is byte-identical to a fresh parse.
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TP_DIR="$REPO"
cd "$REPO" || exit 1

: "${TP:?run the gates through mise run gate, which builds what they need and sets TP, TP_LIB, TP_CC and TP_NM}"
LIB="$TP_LIB"
INC="$TP_DIR/include"
SRC="$TP_DIR/src"

echo "== tinyparse engine-reuse regression =="

# 1. The CLI and the library archive `mise run gate` built (the generated parser links against it).
if [ ! -x "$TP" ] || [ ! -f "$LIB" ]; then
  echo "FATAL: CLI ($TP) or libtinyparse ($LIB) not built"
  echo "RESULT=red"
  exit 1
fi

CC="$TP_CC"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

# 2. Generate a compiled parser for std.tsx (the parse path the bug was found on).
GEN="$SCRATCH/tsx_parser.c"
if ! "$TP" c std.tsx --parser-lib --out "$GEN" 2>"$SCRATCH/gen.err"; then
  echo "FATAL: could not emit std.tsx parser-lib"
  sed 's/^/  /' "$SCRATCH/gen.err" | head
  echo "RESULT=red"
  exit 1
fi

# 3. Generate a large, valid .tsx input — big enough that parsing it grows the
#    engine arena pool, so a reused engine crosses the grow boundary every parse.
INPUT="$SCRATCH/reuse_input.tsx"
awk 'BEGIN { for (i = 0; i < 1500; i++)
  printf "const x%d = [1, 2, 3].map((v) => v * %d + foo.bar(%d));\n", i, i, i }' > "$INPUT"
echo "  generated input: $(wc -c < "$INPUT") bytes of valid .tsx"

# 4. A harness that reuses ONE default engine across many parses and asserts every
#    tree is byte-identical to the first, with no push silently failing. The 8 MB
#    grant is deliberately small: it is the size at which the old mailbox copy
#    fragmented and failed, and it forces the pool to grow and recycle chunks.
HARNESS="$SCRATCH/reuse_harness.c"
cat > "$HARNESS" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinyparse.h"

/* The generated std.tsx parser's two entries (emitted with --parser-lib). */
tinyparse_result tsx_load(tinyparse_engine *, tinyparse_grammar **);
tinyparse_result tsx_run(tinyparse_parser *);

#define GRANT (8u * 1024u * 1024u)
#define REPS  120

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
    int i, failures = 0;

    if (argc < 2) { fprintf(stderr, "usage: %s <input.tsx>\n", argv[0]); return 2; }
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

    if (tsx_load(engine, &grammar) != TINYPARSE_OK) {
        fprintf(stderr, "grammar load failed\n"); return 2;
    }

    by.pointer = (const uint8_t *)fb; by.length = flen;
    for (i = 0; i < REPS; i++) {
        tinyparse_parser *p = NULL;
        tinyparse_result rn = tinyparse_parser_new(engine, grammar, text(""), &p);
        tinyparse_result rp = tinyparse_push(p, by);
        tinyparse_result rr = tsx_run(p);
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
        printf("  %d of %d reused parses diverged from the fresh parse\n", failures, REPS);
        return 1;
    }
    printf("  all %d reused parses byte-identical to the fresh parse\n", REPS);
    return 0;
}
EOF

BIN="$SCRATCH/reuse_harness"
if ! "$CC" -std=c11 -O2 -I"$INC" -I"$SRC" \
      "$HARNESS" "$GEN" "$LIB" -o "$BIN" 2>"$SCRATCH/cc.err"; then
  echo "FATAL: harness did not compile"
  sed 's/^/  /' "$SCRATCH/cc.err" | head -20
  echo "RESULT=red"
  exit 1
fi

if "$BIN" "$INPUT"; then
  echo "== reuse-regression: GREEN =="
  echo "RESULT=green"
  exit 0
fi
echo "== reuse-regression: RED =="
echo "RESULT=red"
exit 1
