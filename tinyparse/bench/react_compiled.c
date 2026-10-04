/*
** react_compiled.c -- the C-COMPILED tinyparse arm of the React benchmark
** (W14-4). It is react_tinyparse.c's twin, driving the parser emitted by
** `tinyparse c std.tsx --parser-lib` instead of the bytecode VM. That
** generated translation unit (linked in at build time) bakes the grammar's rule
** structure into an explicit-stack dispatch loop; this harness calls its two
** entries:
**
**   tp_compiled_parser_load(engine, &grammar)  -- load the EMBEDDED grammar (so
**       the generated loop's baked instruction indices line up with the program
**       it reads at runtime; a separately loaded copy would not match).
**   tp_compiled_parser_run(parser)             -- run the compiled dispatch loop
**       over a parser whose input is already pushed.
**
** Because the compiled parser is byte-identical to the VM (cc-diff proves it),
** this is a like-for-like speed comparison.
**
** The harness measures the SAME corpus two legitimately different ways, because
** how the host drives the engine -- not parse throughput alone -- decides what
** the number means (W18; see scratchpad/bench-clarify.md, docs/peg-vs-glr.md):
**
**   fresh-per-file (RSS-bounded): each file gets a FRESH engine, opened + grammar
**       loaded (UNTIMED) then closed before the next, so peak memory stays bounded
**       to one file. This is the historical bench5 policy and the conservative
**       memory-isolation choice. Only the parse call (push + run) is timed.
**
**   persistent-engine (whole-codebase): ONE engine, grammar loaded ONCE, reused
**       across the whole corpus via the public API -- tinyparse_parser_new per
**       file, tinyparse_parser_free resets the parser's child arena so memory is
**       reclaimed between files (parse.c). This is the way a real host parses a
**       codebase. Only the parse call is timed, exactly as the fresh arm times it.
**
** Both arms parse the same bytes through the same timed region; they differ only
** in whether the engine + grammar are reopened per file. The persistent arm is
** the faster, like-for-like figure (no per-file grammar reload underneath the
** timed region); the fresh arm bounds peak RSS. The harness reports both.
**
** SAFETY: a persistent-engine arm is exactly what once silently corrupted parses
** (commit 86c4317: reuse across an arena grow collapsed trees to one node). So
** before timing, this harness ASSERTS the persistent arm builds a tree
** byte-identical (node count + per-node kind, field, and span fingerprint) to the
** fresh arm, for every file. A divergence -- the corruption signature -- aborts
** the bench rather than letting it ever report a number measured over silently
** wrong parses. The standing oracle for the fix is ci/tinyparse-reuse-regression.sh;
** this assertion is the bench's own in-line guard so corrupt parses can never be
** timed here either.
**
** This is host dev-tool code: libc is fine.
**
** Usage: react_compiled <manifest> <iterations> [parse|persistent|both]
**   parse       -- fresh-per-file arm only (historical default; the bare word
**                  "parse" is kept so existing bench5 callers behave unchanged)
**   persistent  -- persistent-engine arm only
**   both        -- assert byte-identical, then report both arms
*/
#include "bench_common.h"
#include "tinyparse.h"

/* ** The two entries the generated parser (react_compiled_parser.c) exposes when
** emitted with --parser-lib. Declared here, defined in the generated TU. */
tinyparse_result tp_compiled_parser_load(tinyparse_engine *engine,
                                           tinyparse_grammar **out_grammar);
tinyparse_result tp_compiled_parser_run(tinyparse_parser *parser);

/* ** The initial grant. Like react_tinyparse.c, each file is parsed in a FRESH
** engine that is closed before the next, bounding peak memory to one file, with
** the per-file open + grammar load excluded from the timed region. The grant is a
** modest initial size; the grow callback supplies the rest. */
#define REACT_GRANT (8u * 1024u * 1024u)

/* ** A registry of grow blocks so the warm loop's re-opens do not leak (mirrors
** react_tinyparse.c). */
static void   **react_blocks;
static size_t   react_block_count;
static size_t   react_block_capacity;

static void react_blocks_track(void *p)
{
    if (p == NULL)
        return;
    if (react_block_count == react_block_capacity) {
        size_t  nc = react_block_capacity == 0 ? 256 : react_block_capacity * 2;
        void  **grown = (void **)realloc(react_blocks, nc * sizeof *grown);
        if (grown == NULL)
            return;
        react_blocks = grown;
        react_block_capacity = nc;
    }
    react_blocks[react_block_count++] = p;
}

static void react_blocks_free_all(void)
{
    size_t i;
    for (i = 0; i < react_block_count; i++)
        free(react_blocks[i]);
    react_block_count = 0;
}

static void *react_grow(void *user, size_t need, size_t *got)
{
    void *p = malloc(need);
    (void)user;
    if (got != 0) *got = need;
    react_blocks_track(p);
    return p;
}

static tinyparse_string tp_text(const char *s)
{
    tinyparse_string t;
    t.pointer = (const uint8_t *)s;
    t.length = (uint32_t)strlen(s);
    return t;
}

/* ** The ASCII classes the CLI installs, mirrored so the embedded std.tsx grammar
** resolves the same way it does under `tinyparse run' (the cc-diff oracle). */
static int cls_alpha(void *u, uint32_t b)
{ (void)u; return (b>='A'&&b<='Z')||(b>='a'&&b<='z'); }
static int cls_digit(void *u, uint32_t b) { (void)u; return b>='0'&&b<='9'; }
static int cls_alnum(void *u, uint32_t b) { return cls_alpha(u,b)||cls_digit(u,b); }
static int cls_space(void *u, uint32_t b)
{ (void)u; return b==' '||b=='\t'||b=='\n'||b=='\r'; }

static void bind_class(tinyparse_engine *e, const char *name,
                       tinyparse_class_fn fn)
{
    tinyparse_binding binding;
    binding.name = tp_text(name);
    binding.kind = TINYPARSE_BIND_CLASS;
    binding.function = (void *)fn;
    binding.user = NULL;
    tinyparse_bind(e, &binding);
}

/* ** Open an engine over a fresh grant and load the EMBEDDED grammar through the
** generated parser's loader. The grammar is NOT read from a file: the generated
** dispatch loop reads operands from the lowered program of the grammar baked into
** the translation unit, so the host must parse with exactly that grammar. */
static tinyparse_engine *open_and_load(tinyparse_grammar **out_grammar)
{
    tinyparse_configuration cfg;
    tinyparse_engine *engine;
    void *block;
    react_block_count = 0;
    block = malloc(REACT_GRANT);
    if (block == NULL)
        return NULL;
    react_blocks_track(block);
    memset(&cfg, 0, sizeof cfg);
    cfg.memory.base = block;
    cfg.memory.size = REACT_GRANT;
    cfg.memory.grow = react_grow;
    cfg.memory.user = NULL;
    cfg.memory.chunk_minimum = 0;
    cfg.flags = 0;
    engine = tinyparse_open(cfg);
    if (engine == NULL)
        return NULL;
    bind_class(engine, "alpha", cls_alpha);
    bind_class(engine, "digit", cls_digit);
    bind_class(engine, "alnum", cls_alnum);
    bind_class(engine, "space", cls_space);
    if (tp_compiled_parser_load(engine, out_grammar) != TINYPARSE_OK) {
        tinyparse_string fault = tinyparse_fault(engine);
        fprintf(stderr, "react: embedded grammar load failed: %.*s\n",
                (int)fault.length, (const char *)fault.pointer);
        return NULL;
    }
    return engine;
}

/* ** Parse one document in a FRESH engine: open + load the grammar (UNTIMED),
** time ONLY the compiled parse (push + tp_compiled_parser_run), then close the
** engine. Adds the timed parse seconds to *accum. Returns 0 on success, 2 on
** error. Mirrors react_tinyparse.c's parse_one_file_timed exactly, but the
** timed call is the compiled parser instead of tinyparse_finish/the VM. */
static int parse_one_file_timed(const bench_doc *doc, double *accum)
{
    tinyparse_grammar *g = NULL;
    tinyparse_engine *e = open_and_load(&g);
    tinyparse_parser *parser = NULL;
    tinyparse_string empty, bytes;
    double t0, t1;
    if (e == NULL) return 2;
    empty.pointer = NULL; empty.length = 0;
    if (tinyparse_parser_new(e, g, empty, &parser) != TINYPARSE_OK
        || parser == NULL) { tinyparse_close(e); react_blocks_free_all(); return 2; }
    bytes.pointer = (const uint8_t *)doc->bytes;
    bytes.length = (uint32_t)doc->length;
    t0 = bench_now();
    tinyparse_push(parser, bytes);
    tp_compiled_parser_run(parser);
    t1 = bench_now();
    *accum += t1 - t0;
    tinyparse_parser_free(parser);
    tinyparse_close(e);
    react_blocks_free_all();
    return 0;
}

/* ** Parse the whole corpus once, timing only the parse calls (each file in a
** fresh engine). Returns the summed parse seconds through *out. */
static int parse_corpus_timed(const bench_corpus *corpus, double *out)
{
    size_t i;
    double accum = 0.0;
    for (i = 0; i < corpus->count; i++)
        if (parse_one_file_timed(&corpus->docs[i], &accum) != 0)
            return 2;
    *out = accum;
    return 0;
}

/* ** The parse benchmark: warm up, then for each iteration sum the per-file parse
** time (each file parsed in a fresh, immediately-closed engine so peak memory is
** one file and the grammar load is off the meter), reporting the median. The
** report line is the same KEY=VALUE shape the orchestrator greps for. */
static int run_parse(const bench_corpus *corpus, size_t iterations)
{
    double *samples, median, warm;
    size_t i;
    if (parse_corpus_timed(corpus, &warm) != 0)
        return 2;
    samples = (double *)malloc(iterations * sizeof(double));
    if (samples == NULL) return 2;
    for (i = 0; i < iterations; i++) {
        double seconds = 0.0;
        if (parse_corpus_timed(corpus, &seconds) != 0) {
            free(samples); return 2;
        }
        samples[i] = seconds;
    }
    qsort(samples, iterations, sizeof(double), bench_cmp_double);
    median = samples[iterations / 2];
    bench_report("tinyparse-compiled", median, corpus->total_bytes, iterations,
                 bench_peak_rss_kb());
    free(samples);
    return 0;
}

/* ** An order-sensitive fingerprint of a built tree: a node count plus an FNV-1a
** hash folding each node's kind, field, and byte span in pre-order. Two parses
** with the same fingerprint built the same tree down to spans -- the assertion
** the persistent arm must pass against the fresh arm, since the silent-corruption
** bug (86c4317) collapsed reused parses to a degenerate one-node tree while the
** API reported success. Mirrors ci/tinyparse-reuse-regression.sh's walk, with
** field + span folded in for a stricter check. */
typedef struct tree_fingerprint {
    unsigned long nodes;
    unsigned long hash;
} tree_fingerprint;

static void fingerprint_walk(tinyparse_tree_cursor *c, tree_fingerprint *fp)
{
    tinyparse_symbol kind  = tinyparse_tree_cursor_kind(c);
    tinyparse_symbol field = tinyparse_tree_cursor_field(c);
    uint32_t           start = tinyparse_tree_cursor_start_byte(c);
    uint32_t           end   = tinyparse_tree_cursor_end_byte(c);
    fp->nodes++;
    fp->hash = (fp->hash * 1099511628211UL) ^ (unsigned long)kind.id;
    fp->hash = (fp->hash * 1099511628211UL) ^ (unsigned long)field.id;
    fp->hash = (fp->hash * 1099511628211UL) ^ (unsigned long)start;
    fp->hash = (fp->hash * 1099511628211UL) ^ (unsigned long)end;
    if (tinyparse_tree_cursor_goto_first_child(c)) {
        do { fingerprint_walk(c, fp); }
        while (tinyparse_tree_cursor_goto_next_sibling(c));
        tinyparse_tree_cursor_goto_parent(c);
    }
}

/* ** Fingerprint the tree a parser currently holds. Returns 0 on success (a
** real, non-degenerate tree was walked), nonzero if there is no tree. */
static int fingerprint_parser(tinyparse_parser *parser, tree_fingerprint *fp)
{
    const tinyparse_tree *tree = tinyparse_parser_tree(parser);
    tinyparse_tree_cursor *cursor = NULL;
    fp->nodes = 0;
    fp->hash  = 1469598103934665603UL;
    if (tree == NULL)
        return 1;
    if (tinyparse_tree_cursor_new(tree, &cursor) != TINYPARSE_OK)
        return 1;
    fingerprint_walk(cursor, fp);
    tinyparse_tree_cursor_free(cursor);
    return 0;
}

/* ** Parse one file in the SHARED persistent engine: a fresh parser_new over the
** already-open engine + already-loaded grammar, time only push + run, then
** parser_free to hand the parse's child arena back to the engine pool. Adds the
** timed parse seconds to *accum. If out_fp is non-NULL, writes the tree's
** fingerprint (untimed, after the timed region). Returns 0 on success, 2 on
** error. The timed region is byte-for-byte the fresh arm's timed region; only the
** engine + grammar are reused rather than reopened. */
static int parse_one_file_persistent(tinyparse_engine *engine,
                                     tinyparse_grammar *grammar,
                                     const bench_doc *doc, double *accum,
                                     tree_fingerprint *out_fp)
{
    tinyparse_parser *parser = NULL;
    tinyparse_string empty, bytes;
    double t0, t1;
    empty.pointer = NULL; empty.length = 0;
    if (tinyparse_parser_new(engine, grammar, empty, &parser) != TINYPARSE_OK
        || parser == NULL)
        return 2;
    bytes.pointer = (const uint8_t *)doc->bytes;
    bytes.length  = (uint32_t)doc->length;
    t0 = bench_now();
    tinyparse_push(parser, bytes);
    tp_compiled_parser_run(parser);
    t1 = bench_now();
    *accum += t1 - t0;
    if (out_fp != NULL)
        (void)fingerprint_parser(parser, out_fp);
    tinyparse_parser_free(parser);
    return 0;
}

/* ** Parse the whole corpus once through ONE persistent engine, timing only the
** parse calls. Returns the summed parse seconds through *out. */
static int parse_corpus_persistent_timed(tinyparse_engine *engine,
                                         tinyparse_grammar *grammar,
                                         const bench_corpus *corpus, double *out)
{
    size_t i;
    double accum = 0.0;
    for (i = 0; i < corpus->count; i++)
        if (parse_one_file_persistent(engine, grammar, &corpus->docs[i],
                                      &accum, NULL) != 0)
            return 2;
    *out = accum;
    return 0;
}

/* ** Parse one file in a FRESH engine and return its tree fingerprint (untimed).
** Mirrors parse_one_file_timed's open + load + parse, but keeps the tree long
** enough to fingerprint it before closing the engine. Returns 0 on success. */
static int fingerprint_one_file_fresh(const bench_doc *doc,
                                      tree_fingerprint *out_fp)
{
    tinyparse_grammar *g = NULL;
    tinyparse_engine  *e = open_and_load(&g);
    tinyparse_parser  *parser = NULL;
    tinyparse_string   empty, bytes;
    int rc;
    if (e == NULL) return 2;
    empty.pointer = NULL; empty.length = 0;
    if (tinyparse_parser_new(e, g, empty, &parser) != TINYPARSE_OK
        || parser == NULL) {
        tinyparse_close(e); react_blocks_free_all(); return 2;
    }
    bytes.pointer = (const uint8_t *)doc->bytes;
    bytes.length  = (uint32_t)doc->length;
    tinyparse_push(parser, bytes);
    tp_compiled_parser_run(parser);
    rc = fingerprint_parser(parser, out_fp);
    tinyparse_parser_free(parser);
    tinyparse_close(e);
    react_blocks_free_all();
    return rc;
}

/* ** The byte-identical assertion: for every file, fingerprint the FRESH-arm tree
** and the PERSISTENT-arm tree and require they match exactly (node count + the
** kind/field/span hash). This is the in-line guard against the silent reuse
** corruption: a persistent parse that collapsed to a degenerate tree, or dropped
** a chunk, would diverge here and abort the bench before any number is reported.
** Returns 0 if every file matches, 1 on the first divergence (a degenerate fresh
** parse counts as a failure too -- the corpus is clean-parsing by construction). */
static int assert_arms_byte_identical(tinyparse_engine *engine,
                                      tinyparse_grammar *grammar,
                                      const bench_corpus *corpus)
{
    size_t i;
    for (i = 0; i < corpus->count; i++) {
        tree_fingerprint fresh, persistent;
        double sink = 0.0;
        if (fingerprint_one_file_fresh(&corpus->docs[i], &fresh) != 0) {
            fprintf(stderr, "react: fresh fingerprint failed on file %zu\n", i);
            return 1;
        }
        if (parse_one_file_persistent(engine, grammar, &corpus->docs[i],
                                      &sink, &persistent) != 0) {
            fprintf(stderr, "react: persistent parse failed on file %zu\n", i);
            return 1;
        }
        if (fresh.nodes <= 1) {
            fprintf(stderr,
                "react: fresh parse of file %zu is degenerate (%lu nodes) -- "
                "corpus should be clean-parsing\n", i, fresh.nodes);
            return 1;
        }
        if (persistent.nodes != fresh.nodes || persistent.hash != fresh.hash) {
            fprintf(stderr,
                "react: ARM DIVERGENCE on file %zu -- the persistent engine did "
                "NOT build the fresh engine's tree.\n"
                "       fresh:      nodes=%lu fingerprint=%lx\n"
                "       persistent: nodes=%lu fingerprint=%lx\n"
                "       This is the silent-corruption signature (86c4317). The "
                "fix is incomplete; refusing to time corrupt parses.\n",
                i, fresh.nodes, fresh.hash, persistent.nodes, persistent.hash);
            return 1;
        }
    }
    return 0;
}

/* ** The persistent-engine benchmark: open ONE engine, load the grammar ONCE,
** warm up, then for each iteration sum the per-file parse time reusing that one
** engine (parser_new / push / run / parser_free per file), reporting the median.
** Same timed region and KEY=VALUE report shape as run_parse; only the engine is
** persistent. */
static int run_persistent(const bench_corpus *corpus, size_t iterations)
{
    tinyparse_grammar *grammar = NULL;
    tinyparse_engine  *engine = open_and_load(&grammar);
    double *samples, median, warm;
    size_t i;
    if (engine == NULL) return 2;
    if (parse_corpus_persistent_timed(engine, grammar, corpus, &warm) != 0) {
        tinyparse_close(engine); react_blocks_free_all(); return 2;
    }
    samples = (double *)malloc(iterations * sizeof(double));
    if (samples == NULL) {
        tinyparse_close(engine); react_blocks_free_all(); return 2;
    }
    for (i = 0; i < iterations; i++) {
        double seconds = 0.0;
        if (parse_corpus_persistent_timed(engine, grammar, corpus, &seconds) != 0) {
            free(samples); tinyparse_close(engine); react_blocks_free_all();
            return 2;
        }
        samples[i] = seconds;
    }
    qsort(samples, iterations, sizeof(double), bench_cmp_double);
    median = samples[iterations / 2];
    bench_report("tinyparse-persistent", median, corpus->total_bytes,
                 iterations, bench_peak_rss_kb());
    free(samples);
    tinyparse_close(engine);
    react_blocks_free_all();
    return 0;
}

/* ** Run both arms with the byte-identical assertion first. Opens a persistent
** engine for the assertion, closes it, then runs each arm. The assertion is the
** gate: if the persistent arm ever diverges from the fresh arm, abort before any
** number is reported. */
static int run_both(const bench_corpus *corpus, size_t iterations)
{
    tinyparse_grammar *grammar = NULL;
    tinyparse_engine  *engine = open_and_load(&grammar);
    int diverged;
    if (engine == NULL) return 2;
    diverged = assert_arms_byte_identical(engine, grammar, corpus);
    tinyparse_close(engine);
    react_blocks_free_all();
    if (diverged != 0) {
        fprintf(stderr, "react: byte-identical assertion FAILED -- aborting bench\n");
        return 1;
    }
    fprintf(stderr,
        "react: byte-identical assertion PASSED for all %zu files "
        "(persistent engine builds the fresh engine's trees)\n", corpus->count);
    if (run_parse(corpus, iterations) != 0)
        return 2;
    if (run_persistent(corpus, iterations) != 0)
        return 2;
    return 0;
}

int main(int argc, char **argv)
{
    bench_corpus corpus;
    size_t iterations;
    const char *mode;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <manifest> <iterations> [parse|persistent|both]\n",
                argv[0]);
        return 2;
    }
    iterations = (size_t)strtoul(argv[2], NULL, 10);
    if (iterations == 0)
        iterations = 1;
    mode = (argc >= 4) ? argv[3] : "parse";

    corpus = bench_load_manifest(argv[1]);
    if (corpus.count == 0) {
        fprintf(stderr, "react: empty corpus\n");
        return 2;
    }
    if (strcmp(mode, "persistent") == 0)
        return run_persistent(&corpus, iterations);
    if (strcmp(mode, "both") == 0)
        return run_both(&corpus, iterations);
    /* "parse" or any legacy word: the historical fresh-per-file arm, unchanged. */
    return run_parse(&corpus, iterations);
}
