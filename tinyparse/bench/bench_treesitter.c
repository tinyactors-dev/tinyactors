/*
** bench_treesitter.c -- the tree-sitter side of the Phase 9 benchmark.
**
** It links the tree-sitter runtime (lib/src/lib.c, built into libtreesitter.a)
** with the vendored tree-sitter-javascript parser.c + scanner.c, then parses
** every corpus document with ts_parser_parse_string and immediately deletes the
** resulting tree -- it never walks the TSTree. This is the apples-to-apples
** counterpart to bench_tinyparse.c, which likewise parses to a tree and
** discards it without a post-parse walk (tinyparse builds its CST eagerly at
** finish, so neither side does any traversal). Only the per-document parse calls
** are timed; library load is not.
**
** Two parser-lifecycle modes, selected by the third argument:
**   idiomatic  -- the TSParser is created ONCE and reused for every document,
**                 reset between parses. This is how the tree-sitter API intends
**                 a parser to be used and is the default.
**   lifecycle  -- a fresh TSParser is created (and its language set) and deleted
**                 for EVERY document, mirroring tinyparse's create-a-parser-
**                 per-parse model. This isolates and exposes the parser-setup
**                 cost so the comparison is symmetric.
**
** A third mode, "walk", measures TRAVERSAL instead of parsing. It parses each
** document UNTIMED into a TSTree (held alive), then times ONLY a preorder
** depth-first TSTreeCursor walk that touches every node (reading its symbol and
** start byte into a volatile sink), counting the nodes visited. The tinyparse
** harness runs the identical walk over its own cursor, so the traversal numbers
** are apples-to-apples even though the two trees have different shapes; each side
** reports its own node count and we normalize by it (ns/node).
**
** Usage: bench_treesitter <manifest> <iterations> [idiomatic|lifecycle|walk]
*/
#include "bench_common.h"
#include "tree_sitter/api.h"

/* ** The grammar's exported language constructor, from parser.c. */
const TSLanguage *tree_sitter_javascript(void);

/* ** Idiomatic: parse every document once, reusing one parser, deleting each
** tree as we go so the timed work is parsing, not tree retention. */
static void parse_corpus_reuse(TSParser *parser, const bench_corpus *corpus)
{
    size_t i;
    for (i = 0; i < corpus->count; i++) {
        TSTree *tree = ts_parser_parse_string(
            parser, NULL, corpus->docs[i].bytes,
            (uint32_t)corpus->docs[i].length);
        if (tree != NULL)
            ts_tree_delete(tree);
        /* Reset so consecutive parses do not reuse the previous tree as an
        ** incremental base -- each parse is a fresh full parse. */
        ts_parser_reset(parser);
    }
}

/* ** Symmetric (lifecycle): create a fresh parser per document, set its
** language, parse, delete the tree, delete the parser -- mirroring
** tinyparse's per-parse parser. The whole per-document cost, parser setup
** included, is on the meter. */
static void parse_corpus_per_doc(const TSLanguage *lang,
                                 const bench_corpus *corpus)
{
    size_t i;
    for (i = 0; i < corpus->count; i++) {
        TSParser *parser = ts_parser_new();
        TSTree   *tree;
        if (parser == NULL || !ts_parser_set_language(parser, lang)) {
            if (parser != NULL)
                ts_parser_delete(parser);
            continue;
        }
        tree = ts_parser_parse_string(parser, NULL, corpus->docs[i].bytes,
                                      (uint32_t)corpus->docs[i].length);
        if (tree != NULL)
            ts_tree_delete(tree);
        ts_parser_delete(parser);
    }
}

/* ** A volatile sink so the per-node touch below cannot be optimized away. */
static volatile uint32_t bench_walk_sink;

/* ** Walk one TSTree preorder, depth-first, with a TSTreeCursor, touching every
** node's symbol and start byte and counting it. This is the timed traversal; it
** mirrors the tinyparse side's cursor walk exactly (same order, same touch).
** Returns the number of nodes visited. */
static size_t walk_tree(TSTree *tree)
{
    TSTreeCursor cursor;
    size_t nodes = 0;
    if (tree == NULL)
        return 0;
    cursor = ts_tree_cursor_new(ts_tree_root_node(tree));
    for (;;) {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        /* Touch the node so the walk cannot be optimized away. */
        bench_walk_sink += ts_node_symbol(node);
        bench_walk_sink += ts_node_start_byte(node);
        nodes++;
        if (ts_tree_cursor_goto_first_child(&cursor))
            continue;
        for (;;) {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
                break;
            if (!ts_tree_cursor_goto_parent(&cursor)) {
                ts_tree_cursor_delete(&cursor);
                return nodes;
            }
        }
    }
}

/* ** Walk every document's TSTree once, parsing each UNTIMED (the caller times
** only the walk). Trees are built into an array, then a single preorder DFS walk
** touches every node, then every tree is deleted. Returns the walk's wall time
** through *out_seconds and the total node count through *out_nodes. */
static int time_walk_once(TSParser *parser, const bench_corpus *corpus,
                          double *out_seconds, size_t *out_nodes)
{
    TSTree **trees;
    size_t i, nodes = 0;
    double t0, t1;

    trees = (TSTree **)calloc(corpus->count, sizeof *trees);
    if (trees == NULL)
        return 0;

    /* Build every tree UNTIMED. */
    for (i = 0; i < corpus->count; i++) {
        trees[i] = ts_parser_parse_string(parser, NULL, corpus->docs[i].bytes,
                                          (uint32_t)corpus->docs[i].length);
        ts_parser_reset(parser);
    }

    /* Time ONLY the depth-first cursor walk over the built trees. */
    t0 = bench_now();
    for (i = 0; i < corpus->count; i++)
        nodes += walk_tree(trees[i]);
    t1 = bench_now();

    for (i = 0; i < corpus->count; i++) {
        if (trees[i] != NULL)
            ts_tree_delete(trees[i]);
    }
    free(trees);

    *out_seconds = t1 - t0;
    *out_nodes = nodes;
    return 1;
}

/* ** The walk benchmark: warm up, then for each iteration build the trees untimed
** and time the cursor walk. Reports nodes/sec and ns/node over the median. */
static int run_walk(TSParser *parser, const bench_corpus *corpus,
                    size_t iterations)
{
    double *samples, median;
    size_t i, node_count = 0;

    /* Warm up: two untimed full passes (parse + walk). */
    {
        double s; size_t n;
        time_walk_once(parser, corpus, &s, &n);
        time_walk_once(parser, corpus, &s, &n);
    }

    samples = (double *)malloc(iterations * sizeof(double));
    if (samples == NULL)
        return 2;
    for (i = 0; i < iterations; i++) {
        double seconds = 0.0;
        size_t nodes = 0;
        if (!time_walk_once(parser, corpus, &seconds, &nodes)) {
            free(samples);
            return 2;
        }
        samples[i] = seconds;
        node_count = nodes;
    }

    qsort(samples, iterations, sizeof(double), bench_cmp_double);
    median = samples[iterations / 2];
    bench_report_walk("tree-sitter", median, node_count, iterations);
    free(samples);
    return 0;
}

/* ** Load the corpus, warm up, run the timed loop, report. */
int main(int argc, char **argv)
{
    bench_corpus corpus;
    const TSLanguage *lang;
    TSParser *parser = NULL;
    size_t iterations, i;
    double *samples;
    double median;
    int lifecycle = 0;
    int walk = 0;
    const char *label = "tree-sitter";

    if (argc < 3) {
        fprintf(stderr, "usage: %s <manifest> <iterations> "
                        "[idiomatic|lifecycle|walk]\n", argv[0]);
        return 2;
    }
    iterations = (size_t)strtoul(argv[2], NULL, 10);
    if (iterations == 0)
        iterations = 1;
    if (argc >= 4 && strcmp(argv[3], "lifecycle") == 0) {
        lifecycle = 1;
        label = "tree-sitter-lifecycle";
    } else if (argc >= 4 && strcmp(argv[3], "walk") == 0) {
        walk = 1;
    }

    corpus = bench_load_manifest(argv[1]);
    if (corpus.count == 0) {
        fprintf(stderr, "bench: empty corpus\n");
        return 2;
    }

    lang = tree_sitter_javascript();
    if (walk) {
        int rc;
        parser = ts_parser_new();
        if (parser == NULL || !ts_parser_set_language(parser, lang)) {
            fprintf(stderr, "bench: tree-sitter setup failed\n");
            return 2;
        }
        rc = run_walk(parser, &corpus, iterations);
        ts_parser_delete(parser);
        return rc;
    }
    if (!lifecycle) {
        parser = ts_parser_new();
        if (parser == NULL || !ts_parser_set_language(parser, lang)) {
            fprintf(stderr, "bench: tree-sitter setup failed\n");
            return 2;
        }
    }

    /* Warm up: two untimed passes prime caches and the allocator. */
    if (lifecycle) {
        parse_corpus_per_doc(lang, &corpus);
        parse_corpus_per_doc(lang, &corpus);
    } else {
        parse_corpus_reuse(parser, &corpus);
        parse_corpus_reuse(parser, &corpus);
    }

    samples = (double *)malloc(iterations * sizeof(double));
    if (samples == NULL)
        return 2;
    for (i = 0; i < iterations; i++) {
        double t0 = bench_now();
        if (lifecycle)
            parse_corpus_per_doc(lang, &corpus);
        else
            parse_corpus_reuse(parser, &corpus);
        samples[i] = bench_now() - t0;
    }

    qsort(samples, iterations, sizeof(double), bench_cmp_double);
    median = samples[iterations / 2];

    bench_report(label, median, corpus.total_bytes, iterations,
                 bench_peak_rss_kb());

    if (parser != NULL)
        ts_parser_delete(parser);
    free(samples);
    return 0;
}
