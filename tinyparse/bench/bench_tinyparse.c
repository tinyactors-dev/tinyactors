/*
** bench_tinyparse.c -- the tinyparse side of the Phase 9 benchmark.
**
** It links libtinyparse.a, compiles the std.javascript grammar
** ONCE from its .scm source (the one-time cost the task asks us to exclude),
** then for every corpus document creates a parser, pushes the whole input, and
** finishes. tinyparse_finish builds the whole contiguous CST eagerly (node
** records, spans, child links are all materialized before it returns -- the
** bytecode VM builds and freezes the whole tree in tp_execute_vm), so there is
** NOTHING to "realize"
** afterwards: the tree is fully built the moment finish returns. We therefore
** do NOT walk it -- the tree-sitter side never walks its TSTree either, it
** parses and deletes. Both sides measure the same thing: parse to a tree, then
** discard. Only the per-document parse work is timed; grammar compilation and
** library load are not.
**
** The engine is re-opened each iteration: tinyparse carves parsers and trees
** from an arena that a parser_free does not return to the engine, so a long
** warm loop would otherwise exhaust the grant. Re-opening (a malloc + a fresh
** grammar compile) happens OUTSIDE the timed region; only the parse loop over
** the already-compiled grammar is measured. This keeps every timed iteration
** starting from the same clean arena, which is the honest thing to measure and
** avoids conflating arena pressure with parse speed.
**
** Fair footprint. getrusage's peak RSS reflects the whole process -- the
** corpus buffers, libc, and the fixed grant block whether or not tinyparse
** touches all of it -- so it over-reports what tinyparse actually consumes.
** This harness fronts a SMALL base grant and supplies the grow callback, then
** sums the bytes tinyparse genuinely pulled (the base grant plus every grown
** block) into a high-water figure, split into the one-time grammar-compile
** phase and the per-corpus-pass parse phase. That sum, not the grant size and
** not process RSS, is the honest measure of tinyparse's footprint.
**
** A second mode, selected by a trailing "walk" argument, measures TRAVERSAL
** instead of parsing. It parses each document UNTIMED to build the full CST,
** then times ONLY a preorder depth-first cursor walk that touches every node
** (reading its kind and start byte into a volatile sink so the walk cannot be
** optimized away), counting the nodes visited. The tree-sitter harness has the
** identical walk over a TSTreeCursor, so the traversal comparison is
** apples-to-apples even though the two trees have different shapes; each side
** reports its OWN node count and we normalize by it (ns/node).
**
** Usage: bench_tinyparse <manifest> <iterations> <grammar.scm> [walk]
*/
#include "bench_common.h"
#include "tinyparse.h"

/* ** The initial grant. We re-open the engine each iteration (so each pass starts
** from a clean arena) and supply a grow callback as the real allocator; the base
** holds the arena bookkeeping and the first chunks before the first grow. A
** 4 MiB base (the size this harness has always used) still forces the bulk of
** each parse through the tracked grow callback, so the high-water footprint stays
** measured, not assumed. */
#define BENCH_GRANT (4u * 1024u * 1024u)

/* ** A registry of blocks the engine owns this iteration: the base grant plus
** any blocks the grow callback handed out, each with its size. We free them all
** when the engine closes (so peak RSS reflects a single pass, not accumulated
** leaks across the warm loop's many re-opens) and we sum their sizes to report
** the true high-water bytes tinyparse consumed. The registry itself grows
** geometrically (the bench is hosted, so libc is fair game) rather than stopping
** at a fixed cap: a grammar or corpus that pulls many grow blocks must still have
** every block reclaimed, or the warm loop would leak across re-opens. */
static void   **bench_blocks;
static size_t   bench_block_count;
static size_t   bench_block_capacity;

/* ** Footprint accounting, in bytes. base is the initial grant; grown_compile
** is the sum of grown blocks tinyparse pulled while compiling the grammar;
** grown_parse is the sum pulled while parsing the corpus. A phase marker tells
** the grow callback which bucket to charge. */
static size_t bench_grant_base;
static size_t bench_grown_compile;
static size_t bench_grown_parse;
static int    bench_phase_parse; /* 0 while compiling, 1 while parsing */

static void bench_blocks_reset(void)
{
    bench_block_count   = 0;
    bench_grant_base    = 0;
    bench_grown_compile = 0;
    bench_grown_parse   = 0;
    bench_phase_parse   = 0;
}

static void bench_blocks_track(void *p)
{
    if (p == NULL)
        return;
    if (bench_block_count == bench_block_capacity) {
        size_t  new_capacity = (bench_block_capacity == 0)
                                   ? 4096 : bench_block_capacity * 2;
        void  **grown = (void **)realloc(bench_blocks,
                                         new_capacity * sizeof *grown);
        if (grown == NULL)
            return; /* cannot record this block; it will be reclaimed at exit */
        bench_blocks         = grown;
        bench_block_capacity = new_capacity;
    }
    bench_blocks[bench_block_count++] = p;
}

static void bench_blocks_free_all(void)
{
    size_t i;
    for (i = 0; i < bench_block_count; i++)
        free(bench_blocks[i]);
    bench_block_count = 0;
}

/* ** Grow callback: hand back a fresh malloc'd block (never a realloc), matching
** the CLI's policy so existing arena allocations keep their addresses. The block
** is tracked (so the host can reclaim it) and its size charged to the active
** phase's footprint bucket. */
static void *bench_grow(void *user, size_t need, size_t *got)
{
    void *p = malloc(need);
    (void)user;
    if (got != 0)
        *got = need;
    bench_blocks_track(p);
    if (p != NULL) {
        if (bench_phase_parse)
            bench_grown_parse += need;
        else
            bench_grown_compile += need;
    }
    return p;
}

/* ** Wrap a C string as a tinyparse_string. */
static tinyparse_string tp_text(const char *s)
{
    tinyparse_string t;
    t.pointer = (const uint8_t *)s;
    t.length = (uint32_t)strlen(s);
    return t;
}

/* ** ASCII character classes the CLI installs before compiling, mirrored here so
** a grammar that writes (class NAME) resolves identically. The std.javascript
** grammar does not currently use them, but installing them keeps the engine
** setup byte-for-byte the same as the CLI's. */
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

/* ** Open an engine over a fresh grant and compile the grammar source into it.
** Returns the engine (with *out_grammar set) or NULL on failure. The grant is
** intentionally leaked across re-opens -- a benchmark process is short-lived.
** The grammar compile runs in the "compile" footprint phase; on return the
** caller switches to the "parse" phase. */
static tinyparse_engine *open_and_compile(const char *grammar_src,
                                            size_t grammar_len,
                                            tinyparse_grammar **out_grammar)
{
    tinyparse_configuration cfg;
    tinyparse_engine *engine;
    tinyparse_source source;
    void *block;
    bench_blocks_reset();
    block = malloc(BENCH_GRANT);
    if (block == NULL)
        return NULL;
    bench_grant_base = BENCH_GRANT;
    bench_blocks_track(block);
    memset(&cfg, 0, sizeof cfg);
    cfg.memory.base = block;
    cfg.memory.size = BENCH_GRANT;
    cfg.memory.grow = bench_grow;
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

    source.name = tp_text("std.javascript");
    source.bytes.pointer = (const uint8_t *)grammar_src;
    source.bytes.length = (uint32_t)grammar_len;
    if (tinyparse_compile_scheme(engine, source, out_grammar) != TINYPARSE_OK) {
        tinyparse_string fault = tinyparse_fault(engine);
        fprintf(stderr, "bench: grammar compile failed: %.*s\n",
                (int)fault.length, (const char *)fault.pointer);
        return NULL;
    }
    return engine;
}

/* ** Parse one document end-to-end: new parser, push the whole input, finish.
** finish builds the full CST before it returns, so there is no post-parse walk
** -- the work charged here is exactly "parse to a tree", which is what the
** tree-sitter side charges too. The parser is then freed. */
static void parse_doc(tinyparse_engine *engine, tinyparse_grammar *grammar,
                      const bench_doc *doc)
{
    tinyparse_parser *parser = NULL;
    tinyparse_string empty, bytes;
    empty.pointer = NULL; empty.length = 0;
    if (tinyparse_parser_new(engine, grammar, empty, &parser) != TINYPARSE_OK
        || parser == NULL)
        return;
    bytes.pointer = (const uint8_t *)doc->bytes;
    bytes.length = (uint32_t)doc->length;
    tinyparse_push(parser, bytes);
    tinyparse_finish(parser);
    tinyparse_parser_free(parser);
}

/* ** Parse every document in the corpus once against an already-compiled grammar. */
static void parse_corpus_once(tinyparse_engine *engine,
                              tinyparse_grammar *grammar,
                              const bench_corpus *corpus)
{
    size_t i;
    for (i = 0; i < corpus->count; i++)
        parse_doc(engine, grammar, &corpus->docs[i]);
}

/* ** A volatile sink so the per-node touch below cannot be optimized away. */
static volatile uint32_t bench_walk_sink;

/* ** Walk one already-built CST preorder, depth-first, touching every node's
** kind and start byte and counting it. This is the timed traversal; it mirrors
** the tree-sitter side's TSTreeCursor walk exactly (same order, same touch).
** Returns the number of nodes visited. */
static size_t walk_tree(const tinyparse_tree *tree)
{
    tinyparse_tree_cursor *cursor = NULL;
    size_t nodes = 0;
    if (tree == NULL)
        return 0;
    if (tinyparse_tree_cursor_new(tree, &cursor) != TINYPARSE_OK)
        return 0;
    for (;;) {
        /* Touch the node so the walk cannot be optimized away. */
        bench_walk_sink += tinyparse_tree_cursor_kind(cursor).id;
        bench_walk_sink += tinyparse_tree_cursor_start_byte(cursor);
        nodes++;
        if (tinyparse_tree_cursor_goto_first_child(cursor))
            continue;
        for (;;) {
            if (tinyparse_tree_cursor_goto_next_sibling(cursor))
                break;
            if (!tinyparse_tree_cursor_goto_parent(cursor)) {
                tinyparse_tree_cursor_free(cursor);
                return nodes;
            }
        }
    }
}

/* ** Walk every document's CST once. The trees are built by parse_doc, but here
** the parse is OUTSIDE the timed region (the caller times only this); we re-parse
** each document, walk its tree, free the parser. Returns the total nodes walked
** across the corpus. */
static size_t walk_corpus_once(tinyparse_engine *engine,
                               tinyparse_grammar *grammar,
                               const bench_corpus *corpus)
{
    size_t i, nodes = 0;
    for (i = 0; i < corpus->count; i++) {
        tinyparse_parser *parser = NULL;
        tinyparse_string empty, bytes;
        empty.pointer = NULL; empty.length = 0;
        if (tinyparse_parser_new(engine, grammar, empty, &parser)
                != TINYPARSE_OK || parser == NULL)
            continue;
        bytes.pointer = (const uint8_t *)corpus->docs[i].bytes;
        bytes.length = (uint32_t)corpus->docs[i].length;
        tinyparse_push(parser, bytes);
        tinyparse_finish(parser);
        nodes += walk_tree(tinyparse_parser_tree(parser));
        tinyparse_parser_free(parser);
    }
    return nodes;
}

/* ** Time only the cursor walk over a corpus already parsed into trees. The
** engine/grammar is opened and every document parsed UNTIMED to build the trees;
** then we time a single preorder DFS walk that touches every node. Because the
** trees are arena-owned and parser_free does not return them, we hold one parser
** per document alive for the duration of the timed walk. Returns the walk's
** wall time through *out_seconds and the node count through *out_nodes. */
static int time_walk_once(tinyparse_engine *engine,
                          tinyparse_grammar *grammar,
                          const bench_corpus *corpus,
                          double *out_seconds, size_t *out_nodes)
{
    tinyparse_parser **parsers;
    size_t i, nodes = 0;
    double t0, t1;

    parsers = (tinyparse_parser **)calloc(corpus->count, sizeof *parsers);
    if (parsers == NULL)
        return 0;

    /* Build every tree UNTIMED. */
    for (i = 0; i < corpus->count; i++) {
        tinyparse_string empty, bytes;
        empty.pointer = NULL; empty.length = 0;
        if (tinyparse_parser_new(engine, grammar, empty, &parsers[i])
                != TINYPARSE_OK)
            parsers[i] = NULL;
        if (parsers[i] == NULL)
            continue;
        bytes.pointer = (const uint8_t *)corpus->docs[i].bytes;
        bytes.length = (uint32_t)corpus->docs[i].length;
        tinyparse_push(parsers[i], bytes);
        tinyparse_finish(parsers[i]);
    }

    /* Time ONLY the depth-first cursor walk over the built trees. */
    t0 = bench_now();
    for (i = 0; i < corpus->count; i++) {
        if (parsers[i] != NULL)
            nodes += walk_tree(tinyparse_parser_tree(parsers[i]));
    }
    t1 = bench_now();

    for (i = 0; i < corpus->count; i++) {
        if (parsers[i] != NULL)
            tinyparse_parser_free(parsers[i]);
    }
    free(parsers);

    *out_seconds = t1 - t0;
    *out_nodes = nodes;
    return 1;
}

/* ** The walk benchmark: warm up, then for each iteration open a clean engine,
** build the trees untimed, and time the cursor walk. Reports nodes/sec and
** ns/node over the median walk. */
static int run_walk(const char *grammar_src, size_t grammar_len,
                    const bench_corpus *corpus, size_t iterations)
{
    double *samples, median;
    size_t i, node_count = 0;

    /* Warm up. */
    {
        tinyparse_grammar *g = NULL;
        tinyparse_engine *e = open_and_compile(grammar_src, grammar_len, &g);
        if (e == NULL)
            return 2;
        bench_phase_parse = 1;
        (void)walk_corpus_once(e, g, corpus);
        (void)walk_corpus_once(e, g, corpus);
        tinyparse_close(e);
        bench_blocks_free_all();
    }

    samples = (double *)malloc(iterations * sizeof(double));
    if (samples == NULL)
        return 2;
    for (i = 0; i < iterations; i++) {
        tinyparse_grammar *g = NULL;
        tinyparse_engine *e = open_and_compile(grammar_src, grammar_len, &g);
        double seconds = 0.0;
        size_t nodes = 0;
        if (e == NULL)
            return 2;
        bench_phase_parse = 1;
        if (!time_walk_once(e, g, corpus, &seconds, &nodes)) {
            tinyparse_close(e);
            bench_blocks_free_all();
            return 2;
        }
        samples[i] = seconds;
        node_count = nodes;
        tinyparse_close(e);
        bench_blocks_free_all();
    }

    qsort(samples, iterations, sizeof(double), bench_cmp_double);
    median = samples[iterations / 2];
    bench_report_walk("tinyparse", median, node_count, iterations);
    free(samples);
    return 0;
}

int main(int argc, char **argv)
{
    bench_corpus corpus;
    char *grammar_src;
    size_t grammar_len, iterations, i;
    double *samples, median;
    size_t foot_compile = 0, foot_parse = 0;
    int walk = 0;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <manifest> <iterations> <grammar.scm> "
                        "[walk]\n", argv[0]);
        return 2;
    }
    iterations = (size_t)strtoul(argv[2], NULL, 10);
    if (iterations == 0)
        iterations = 1;
    if (argc >= 5 && strcmp(argv[4], "walk") == 0)
        walk = 1;

    corpus = bench_load_manifest(argv[1]);
    if (corpus.count == 0) {
        fprintf(stderr, "bench: empty corpus\n");
        return 2;
    }
    grammar_src = bench_read_file(argv[3], &grammar_len);
    if (grammar_src == NULL) {
        fprintf(stderr, "bench: cannot read grammar %s\n", argv[3]);
        return 2;
    }

    if (walk)
        return run_walk(grammar_src, grammar_len, &corpus, iterations);

    /* Warm up: a fresh engine/grammar, two untimed passes. */
    {
        tinyparse_grammar *g = NULL;
        tinyparse_engine *e = open_and_compile(grammar_src, grammar_len, &g);
        if (e == NULL)
            return 2;
        bench_phase_parse = 1;
        parse_corpus_once(e, g, &corpus);
        parse_corpus_once(e, g, &corpus);
        tinyparse_close(e);
        bench_blocks_free_all();
    }

    samples = (double *)malloc(iterations * sizeof(double));
    if (samples == NULL)
        return 2;
    for (i = 0; i < iterations; i++) {
        tinyparse_grammar *g = NULL;
        tinyparse_engine *e = open_and_compile(grammar_src, grammar_len, &g);
        double t0, t1;
        if (e == NULL)
            return 2;
        /* The footprint of compiling the grammar (one-time, excluded from the
        ** timed parse loop) is the base grant plus the compile-phase grown
        ** blocks; capture it once. */
        foot_compile = bench_grant_base + bench_grown_compile;
        bench_phase_parse = 1;
        t0 = bench_now();
        parse_corpus_once(e, g, &corpus);
        t1 = bench_now();
        samples[i] = t1 - t0;
        /* The parse phase's grown bytes are the additional footprint one corpus
        ** pass costs on top of the compiled grammar. */
        foot_parse = bench_grown_parse;
        tinyparse_close(e);
        bench_blocks_free_all();
    }

    qsort(samples, iterations, sizeof(double), bench_cmp_double);
    median = samples[iterations / 2];

    bench_report("tinyparse", median, corpus.total_bytes, iterations,
                 bench_peak_rss_kb());
    /* The fair footprint, reported separately from process RSS: the bytes
    ** tinyparse actually pulled, split into the one-time grammar compile and
    ** the per-corpus-pass parse arena (which accretes -- parser_free does not
    ** return memory to the arena). */
    printf("BENCH parser=tinyparse-footprint "
           "compile_bytes=%zu parse_bytes=%zu high_water_bytes=%zu\n",
           foot_compile, foot_parse, foot_compile + foot_parse);

    free(samples);
    return 0;
}
