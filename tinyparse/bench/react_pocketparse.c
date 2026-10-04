/*
** react_pocketparse.c -- the pocketparse side of the React real-workload
** benchmark (Project B). Where bench_pocketparse.c measures parse-to-discard
** over the tree-sitter JS test corpus, this harness measures a real task over a
** real codebase: parse every (clean-parsing) ReactJS source file with std.tsx
** and COLLECT every function definition in it -- the kind of pass a linter,
** a doc generator, or a codemod runs.
**
** It reports two medians, measured the same way the tree-sitter side measures
** them so the comparison is like-for-like:
**   parse   -- parse every document to a tree (built eagerly at finish), discard.
**   collect -- over trees parsed UNTIMED, time only a preorder walk that counts
**              every node whose kind names a function definition.
** The total function count is reported too, as a correctness cross-check against
** the tree-sitter side (the two grammars categorize a few constructs at
** different granularity, so the counts are close, not identical).
**
** This is host dev-tool code: libc is fine.
**
** Usage: react_pocketparse <manifest> <iterations> <grammar.scm> [parse|collect]
*/
#include "bench_common.h"
#include "pocketparse.h"

/* ** Library-private rule-memo override (defined in src/engine.c, NOT in the
** public header). */
void pp_engine_force_rule_memo(pocketparse_engine *engine, int mode);

/* ** The initial grant. pocketparse's arena does not reclaim on parser_free, so
** parsing a whole codebase into ONE engine accretes every file's tree (and the
** speculative-backtracking garbage some files produce) until it exhausts RAM.
** This harness therefore parses each file in a FRESH engine and closes it before
** the next -- bounding peak memory to a single file -- and excludes the per-file
** engine-open + grammar-load from the timed region (only the parse call is
** timed), matching the methodology's "one-time setup is not on the meter". The
** grant is a modest initial size; the grow callback supplies the rest. */
#define REACT_GRANT (8u * 1024u * 1024u)

/* ** A registry of grow blocks so the warm loop's re-opens do not leak. */
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

static pocketparse_string pp_text(const char *s)
{
    pocketparse_string t;
    t.pointer = (const uint8_t *)s;
    t.length = (uint32_t)strlen(s);
    return t;
}

/* ** The ASCII classes the CLI installs, mirrored so std.tsx resolves the same. */
static int cls_alpha(void *u, uint32_t b)
{ (void)u; return (b>='A'&&b<='Z')||(b>='a'&&b<='z'); }
static int cls_digit(void *u, uint32_t b) { (void)u; return b>='0'&&b<='9'; }
static int cls_alnum(void *u, uint32_t b) { return cls_alpha(u,b)||cls_digit(u,b); }
static int cls_space(void *u, uint32_t b)
{ (void)u; return b==' '||b=='\t'||b=='\n'||b=='\r'; }

static void bind_class(pocketparse_engine *e, const char *name,
                       pocketparse_class_fn fn)
{
    pocketparse_binding binding;
    binding.name = pp_text(name);
    binding.kind = POCKETPARSE_BIND_CLASS;
    binding.function = (void *)fn;
    binding.user = NULL;
    pocketparse_bind(e, &binding);
}

/* ** Open an engine over a fresh grant and compile std.tsx into it. */
static pocketparse_engine *open_and_compile(const char *grammar_src,
                                            size_t grammar_len,
                                            pocketparse_grammar **out_grammar)
{
    pocketparse_configuration cfg;
    pocketparse_engine *engine;
    pocketparse_source source;
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
    engine = pocketparse_open(cfg);
    if (engine == NULL)
        return NULL;
    {   /* Optional: force the W67 rule memo on/off to measure its full effect. */
        const char *rm = getenv("POCKETPARSE_RULEMEMO");
        if (rm != NULL && strcmp(rm, "always") == 0)
            pp_engine_force_rule_memo(engine, 1);
        else if (rm != NULL && strcmp(rm, "off") == 0)
            pp_engine_force_rule_memo(engine, 2);
    }
    bind_class(engine, "alpha", cls_alpha);
    bind_class(engine, "digit", cls_digit);
    bind_class(engine, "alnum", cls_alnum);
    bind_class(engine, "space", cls_space);

    /* The grammar is supplied PRE-PACKED (a .ppg image), because std.tsx
    ** `extends' std.typescript -> std.javascript and resolving that chain needs a
    ** module resolver + the package params. The orchestrator packs it once with
    ** the CLI (which does the full package resolution), and the harness loads the
    ** self-contained binary, which parses identically to `run std.tsx'. */
    source.name = pp_text("std.tsx.ppg");
    source.bytes.pointer = (const uint8_t *)grammar_src;
    source.bytes.length = (uint32_t)grammar_len;
    if (pocketparse_load_binary_grammar(engine, source, out_grammar)
        != POCKETPARSE_OK) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "react: grammar load failed: %.*s\n",
                (int)fault.length, (const char *)fault.pointer);
        return NULL;
    }
    return engine;
}

/* ** The node kinds counted as a "function definition." The same set is used on
** the tree-sitter side, so the totals are directly comparable. */
static const char *const FUNCTION_KINDS[] = {
    "function_declaration",
    "generator_function_declaration",
    "function_expression",
    "generator_function_expression",
    "arrow_function",
    "method_definition",
};
static const size_t FUNCTION_KIND_COUNT =
    sizeof FUNCTION_KINDS / sizeof FUNCTION_KINDS[0];

static int kind_name_is_function(pocketparse_string name)
{
    size_t i;
    for (i = 0; i < FUNCTION_KIND_COUNT; i++) {
        size_t len = strlen(FUNCTION_KINDS[i]);
        if (name.length == len &&
            memcmp(name.pointer, FUNCTION_KINDS[i], len) == 0)
            return 1;
    }
    return 0;
}

/* ** A lazy id->classification cache. Symbol ids are stable within a tree but the
** symbol table is per-tree, so the cache is rebuilt per tree. Classifying a kind
** costs one string scan the first time its id is seen and an array read after,
** so the per-node hot path is an integer lookup -- matching the tree-sitter side,
** which compares precomputed TSSymbols. Cache slots: 0 unknown, 1 function,
** 2 not-function. */
#define KIND_CACHE_SIZE 4096
static unsigned char kind_cache[KIND_CACHE_SIZE];

static int id_is_function(const pocketparse_tree *tree, uint32_t id)
{
    unsigned slot;
    if (id >= KIND_CACHE_SIZE) {
        /* Out of cache range: classify directly (rare). */
        pocketparse_symbol sym; sym.id = id;
        return kind_name_is_function(pocketparse_tree_symbol_name(tree, sym));
    }
    slot = kind_cache[id];
    if (slot == 0) {
        pocketparse_symbol sym; sym.id = id;
        int yes = kind_name_is_function(pocketparse_tree_symbol_name(tree, sym));
        kind_cache[id] = yes ? 1 : 2;
        return yes;
    }
    return slot == 1;
}

/* ** A volatile sink so the collect walk cannot be optimized away. */
static volatile uint32_t react_sink;

/* ** Walk one CST preorder, depth-first, counting function-definition nodes. */
static size_t collect_tree(const pocketparse_tree *tree)
{
    pocketparse_tree_cursor *cursor = NULL;
    size_t functions = 0;
    if (tree == NULL)
        return 0;
    if (pocketparse_tree_cursor_new(tree, &cursor) != POCKETPARSE_OK)
        return 0;
    for (;;) {
        pocketparse_symbol kind = pocketparse_tree_cursor_kind(cursor);
        react_sink += kind.id;
        if (id_is_function(tree, kind.id))
            functions++;
        if (pocketparse_tree_cursor_goto_first_child(cursor))
            continue;
        for (;;) {
            if (pocketparse_tree_cursor_goto_next_sibling(cursor))
                break;
            if (!pocketparse_tree_cursor_goto_parent(cursor)) {
                pocketparse_tree_cursor_free(cursor);
                return functions;
            }
        }
    }
}

/* ** Parse one document to a tree, returning the parser (kept alive so its tree
** can be walked later). Returns NULL on failure. */
static pocketparse_parser *parse_doc(pocketparse_engine *engine,
                                     pocketparse_grammar *grammar,
                                     const bench_doc *doc)
{
    pocketparse_parser *parser = NULL;
    pocketparse_string empty, bytes;
    empty.pointer = NULL; empty.length = 0;
    if (pocketparse_parser_new(engine, grammar, empty, &parser) != POCKETPARSE_OK
        || parser == NULL)
        return NULL;
    bytes.pointer = (const uint8_t *)doc->bytes;
    bytes.length = (uint32_t)doc->length;
    pocketparse_push(parser, bytes);
    pocketparse_finish(parser);
    return parser;
}

/* ** Parse one document in a FRESH engine: open + load the grammar (UNTIMED),
** time ONLY the parse (push + finish), then close the engine -- reclaiming the
** file's arena before the next. Adds the timed parse seconds to *accum. The walk
** callback, when non-NULL, is run on the built tree (UNTIMED here) and its result
** added to *functions -- used by the warm-up. Returns 0 on success, 2 on error. */
static int parse_one_file_timed(const char *grammar_src, size_t grammar_len,
                                const bench_doc *doc, double *accum)
{
    pocketparse_grammar *g = NULL;
    pocketparse_engine *e = open_and_compile(grammar_src, grammar_len, &g);
    pocketparse_parser *parser = NULL;
    pocketparse_string empty, bytes;
    double t0, t1;
    if (e == NULL) return 2;
    empty.pointer = NULL; empty.length = 0;
    if (pocketparse_parser_new(e, g, empty, &parser) != POCKETPARSE_OK
        || parser == NULL) { pocketparse_close(e); react_blocks_free_all(); return 2; }
    bytes.pointer = (const uint8_t *)doc->bytes;
    bytes.length = (uint32_t)doc->length;
    t0 = bench_now();
    pocketparse_push(parser, bytes);
    pocketparse_finish(parser);
    t1 = bench_now();
    *accum += t1 - t0;
    pocketparse_parser_free(parser);
    pocketparse_close(e);
    react_blocks_free_all();
    return 0;
}

/* ** Parse the whole corpus once, timing only the parse calls (each file in a
** fresh engine). Returns the summed parse seconds through *out. */
static int parse_corpus_timed(const char *grammar_src, size_t grammar_len,
                              const bench_corpus *corpus, double *out)
{
    size_t i;
    double accum = 0.0;
    for (i = 0; i < corpus->count; i++)
        if (parse_one_file_timed(grammar_src, grammar_len, &corpus->docs[i],
                                 &accum) != 0)
            return 2;
    *out = accum;
    return 0;
}

/* ** The parse benchmark: warm up, then for each iteration sum the per-file parse
** time (each file parsed in a fresh, immediately-closed engine so peak memory is
** one file and the grammar load is off the meter), reporting the median. */
static int run_parse(const char *grammar_src, size_t grammar_len,
                     const bench_corpus *corpus, size_t iterations)
{
    double *samples, median, warm;
    size_t i;
    if (parse_corpus_timed(grammar_src, grammar_len, corpus, &warm) != 0)
        return 2;
    samples = (double *)malloc(iterations * sizeof(double));
    if (samples == NULL) return 2;
    for (i = 0; i < iterations; i++) {
        double seconds = 0.0;
        if (parse_corpus_timed(grammar_src, grammar_len, corpus, &seconds) != 0) {
            free(samples); return 2;
        }
        samples[i] = seconds;
    }
    qsort(samples, iterations, sizeof(double), bench_cmp_double);
    median = samples[iterations / 2];
    bench_report("pocketparse", median, corpus->total_bytes, iterations,
                 bench_peak_rss_kb());
    free(samples);
    return 0;
}

/* ** Collect over one file in a fresh engine: open + load + parse (all UNTIMED),
** then time ONLY the function-collecting walk over the built tree, close. Adds
** the timed walk seconds to *accum and the function count to *functions. */
static int collect_one_file_timed(const char *grammar_src, size_t grammar_len,
                                  const bench_doc *doc, double *accum,
                                  size_t *functions)
{
    pocketparse_grammar *g = NULL;
    pocketparse_engine *e = open_and_compile(grammar_src, grammar_len, &g);
    pocketparse_parser *parser;
    const pocketparse_tree *tree;
    double t0, t1;
    if (e == NULL) return 2;
    parser = parse_doc(e, g, doc);
    if (parser == NULL) { pocketparse_close(e); react_blocks_free_all(); return 2; }
    tree = pocketparse_parser_tree(parser);
    /* The symbol table is per tree, so the id cache is per file. */
    memset(kind_cache, 0, sizeof kind_cache);
    (void)collect_tree(tree); /* warm the id cache untimed */
    t0 = bench_now();
    *functions += collect_tree(tree);
    t1 = bench_now();
    *accum += t1 - t0;
    pocketparse_parser_free(parser);
    pocketparse_close(e);
    react_blocks_free_all();
    return 0;
}

/* ** The collect benchmark: for each iteration, sum the per-file function-
** collecting walk time (each file parsed untimed in a fresh engine), reporting
** the total function count and the median collect time. */
static int run_collect(const char *grammar_src, size_t grammar_len,
                       const bench_corpus *corpus, size_t iterations)
{
    double *samples, median;
    size_t i, functions = 0;

    samples = (double *)malloc(iterations * sizeof(double));
    if (samples == NULL) return 2;
    for (i = 0; i < iterations; i++) {
        size_t j; double accum = 0.0, found = 0; size_t fcount = 0;
        (void)found;
        for (j = 0; j < corpus->count; j++)
            if (collect_one_file_timed(grammar_src, grammar_len,
                                       &corpus->docs[j], &accum, &fcount) != 0) {
                free(samples); return 2;
            }
        samples[i] = accum;
        functions = fcount;
    }
    qsort(samples, iterations, sizeof(double), bench_cmp_double);
    median = samples[iterations / 2];
    {
        double fps = median > 0.0 ? (double)functions / median : 0.0;
        printf("BENCH parser=pocketparse-collect functions=%zu iterations=%zu "
               "median_seconds=%.6f functions_per_sec=%.0f\n",
               functions, iterations, median, fps);
    }
    free(samples);
    return 0;
}

int main(int argc, char **argv)
{
    bench_corpus corpus;
    char *grammar_src;
    size_t grammar_len, iterations;
    const char *mode;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <manifest> <iterations> <grammar.scm> "
                        "[parse|collect]\n", argv[0]);
        return 2;
    }
    iterations = (size_t)strtoul(argv[2], NULL, 10);
    if (iterations == 0)
        iterations = 1;
    mode = (argc >= 5) ? argv[4] : "parse";

    corpus = bench_load_manifest(argv[1]);
    if (corpus.count == 0) {
        fprintf(stderr, "react: empty corpus\n");
        return 2;
    }
    grammar_src = bench_read_file(argv[3], &grammar_len);
    if (grammar_src == NULL) {
        fprintf(stderr, "react: cannot read grammar %s\n", argv[3]);
        return 2;
    }

    if (strcmp(mode, "collect") == 0)
        return run_collect(grammar_src, grammar_len, &corpus, iterations);
    return run_parse(grammar_src, grammar_len, &corpus, iterations);
}
