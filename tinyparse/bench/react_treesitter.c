/*
** react_treesitter.c -- the tree-sitter side of the React real-workload
** benchmark (Project B). It links the tree-sitter runtime with the vendored
** tree-sitter-typescript TSX parser.c + scanner.c, then runs the same task the
** pocketparse harness runs: parse every (clean-parsing) ReactJS source file and
** COLLECT every function definition.
**
** Two modes, selected by the third argument:
**   parse   -- parse every document to a TSTree and delete it (per-document
**              parser lifecycle, matching the pocketparse harness which creates a
**              parser per parse). Only the parse calls are timed.
**   collect -- parse every document UNTIMED into live trees, then time only a
**              preorder TSTreeCursor walk that counts nodes whose symbol names a
**              function definition. The function symbols are precomputed once via
**              ts_language_symbol_for_name, so the per-node hot path is an integer
**              membership test -- exactly what the pocketparse side does.
**
** Usage: react_treesitter <manifest> <iterations> [parse|collect]
*/
#include "bench_common.h"
#include "tree_sitter/api.h"

/* ** The grammar's exported language constructor, from the TSX parser.c. */
const TSLanguage *tree_sitter_tsx(void);

/* ** The node kinds counted as a "function definition." The same set is used on
** the pocketparse side, so the totals are directly comparable. */
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

/* ** Precompute the set of TSSymbols that name a function definition. Symbols are
** small integers; a 2^16 membership bitmap keyed by symbol makes the walk's test
** a single array read. */
static unsigned char function_symbol[1 << 16];

static void build_function_symbol_set(const TSLanguage *lang)
{
    size_t i;
    memset(function_symbol, 0, sizeof function_symbol);
    for (i = 0; i < FUNCTION_KIND_COUNT; i++) {
        TSSymbol sym = ts_language_symbol_for_name(
            lang, FUNCTION_KINDS[i], (uint32_t)strlen(FUNCTION_KINDS[i]), true);
        if (sym != 0)
            function_symbol[sym] = 1;
    }
}

/* ** Parse the whole corpus once, reusing ONE parser (reset between files) and
** deleting each tree as we go. Parser creation/teardown is one-time setup and is
** off the meter -- matching the pocketparse harness, which times only the parse
** call and excludes its per-file engine open + grammar load. */
static void parse_corpus(TSParser *parser, const bench_corpus *corpus)
{
    size_t i;
    for (i = 0; i < corpus->count; i++) {
        TSTree *tree = ts_parser_parse_string(parser, NULL, corpus->docs[i].bytes,
                                              (uint32_t)corpus->docs[i].length);
        if (tree != NULL)
            ts_tree_delete(tree);
        ts_parser_reset(parser);
    }
}

static volatile uint32_t react_sink;

/* ** Walk one TSTree preorder, counting function-definition nodes. */
static size_t collect_tree(TSTree *tree)
{
    TSTreeCursor cursor;
    size_t functions = 0;
    if (tree == NULL)
        return 0;
    cursor = ts_tree_cursor_new(ts_tree_root_node(tree));
    for (;;) {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        TSSymbol sym = ts_node_symbol(node);
        react_sink += sym;
        if (function_symbol[sym])
            functions++;
        if (ts_tree_cursor_goto_first_child(&cursor))
            continue;
        for (;;) {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
                break;
            if (!ts_tree_cursor_goto_parent(&cursor)) {
                ts_tree_cursor_delete(&cursor);
                return functions;
            }
        }
    }
}

/* ** The parse benchmark. */
static int run_parse(const TSLanguage *lang, const bench_corpus *corpus,
                     size_t iterations)
{
    double *samples, median;
    TSParser *parser;
    size_t i;
    parser = ts_parser_new();
    if (parser == NULL || !ts_parser_set_language(parser, lang)) {
        fprintf(stderr, "react: tree-sitter setup failed\n");
        return 2;
    }
    parse_corpus(parser, corpus);
    parse_corpus(parser, corpus);
    samples = (double *)malloc(iterations * sizeof(double));
    if (samples == NULL) return 2;
    for (i = 0; i < iterations; i++) {
        double t0 = bench_now();
        parse_corpus(parser, corpus);
        samples[i] = bench_now() - t0;
    }
    qsort(samples, iterations, sizeof(double), bench_cmp_double);
    median = samples[iterations / 2];
    bench_report("tree-sitter", median, corpus->total_bytes, iterations,
                 bench_peak_rss_kb());
    ts_parser_delete(parser);
    free(samples);
    return 0;
}

/* ** The collect benchmark: parse into live trees untimed, time only the walk. */
static int run_collect(const TSLanguage *lang, const bench_corpus *corpus,
                       size_t iterations)
{
    TSTree **trees;
    TSParser *parser;
    double *samples, median;
    size_t i, functions = 0;

    parser = ts_parser_new();
    if (parser == NULL || !ts_parser_set_language(parser, lang)) {
        fprintf(stderr, "react: tree-sitter setup failed\n");
        return 2;
    }
    trees = (TSTree **)calloc(corpus->count, sizeof *trees);
    if (trees == NULL) return 2;
    for (i = 0; i < corpus->count; i++) {
        trees[i] = ts_parser_parse_string(parser, NULL, corpus->docs[i].bytes,
                                          (uint32_t)corpus->docs[i].length);
        ts_parser_reset(parser);
    }

    for (i = 0; i < corpus->count; i++)
        (void)collect_tree(trees[i]);

    samples = (double *)malloc(iterations * sizeof(double));
    if (samples == NULL) return 2;
    for (i = 0; i < iterations; i++) {
        size_t j, found = 0; double t0;
        t0 = bench_now();
        for (j = 0; j < corpus->count; j++)
            found += collect_tree(trees[j]);
        samples[i] = bench_now() - t0;
        functions = found;
    }
    qsort(samples, iterations, sizeof(double), bench_cmp_double);
    median = samples[iterations / 2];
    {
        double fps = median > 0.0 ? (double)functions / median : 0.0;
        printf("BENCH parser=tree-sitter-collect functions=%zu iterations=%zu "
               "median_seconds=%.6f functions_per_sec=%.0f\n",
               functions, iterations, median, fps);
    }
    for (i = 0; i < corpus->count; i++)
        if (trees[i] != NULL) ts_tree_delete(trees[i]);
    ts_parser_delete(parser);
    free(trees);
    free(samples);
    return 0;
}

int main(int argc, char **argv)
{
    bench_corpus corpus;
    const TSLanguage *lang;
    size_t iterations;
    const char *mode;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <manifest> <iterations> [parse|collect]\n",
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

    lang = tree_sitter_tsx();
    build_function_symbol_set(lang);

    if (strcmp(mode, "collect") == 0)
        return run_collect(lang, &corpus, iterations);
    return run_parse(lang, &corpus, iterations);
}
