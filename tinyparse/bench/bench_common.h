/*
** bench_common.h -- shared scaffolding for the tinyparse vs tree-sitter
** benchmark harnesses (Phase 9, W42).
**
** Both harnesses receive, on the command line, a manifest file listing the
** input documents to parse (one path per line) followed by the iteration
** count. Each harness loads every document into memory ONCE, parses them all
** in a warm loop, and reports the median per-iteration wall time, the
** aggregate throughput in MB/s, and the process peak RSS. The one-time cost of
** loading the runtime/library and compiling the grammar is excluded from the
** timed loop, matching the task's fairness requirement.
**
** This is host dev-tool code: libc, getrusage and clock_gettime are all fine.
*/
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

/* strtok_r and getrusage are POSIX; ask for them explicitly under -std=c11. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

/* ** One loaded input document: its bytes and length. */
typedef struct bench_doc {
    char   *bytes;
    size_t  length;
} bench_doc;

/* ** A loaded corpus: an array of documents and their total byte count. */
typedef struct bench_corpus {
    bench_doc *docs;
    size_t     count;
    size_t     total_bytes;
} bench_corpus;

/* ** Read an entire file into a freshly malloc'd, NUL-terminated buffer.
** Returns NULL on failure; *out_len receives the byte length (sans NUL). */
static char *bench_read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    size_t cap = 0, len = 0, got;
    if (f == NULL)
        return NULL;
    buf = NULL;
    for (;;) {
        if (len + 65536 + 1 > cap) {
            size_t nc = cap == 0 ? 131072 : cap * 2;
            char *grown = realloc(buf, nc);
            if (grown == NULL) { free(buf); fclose(f); return NULL; }
            buf = grown;
            cap = nc;
        }
        got = fread(buf + len, 1, 65536, f);
        len += got;
        if (got < 65536)
            break;
    }
    fclose(f);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* ** Load a manifest of document paths (one per line) into a bench_corpus.
** Exits the process on any read failure so a partial corpus never skews a
** result. */
static bench_corpus bench_load_manifest(const char *manifest_path)
{
    bench_corpus corpus;
    char *list;
    size_t list_len, cap = 16, n = 0;
    char *line, *save;

    corpus.docs = NULL;
    corpus.count = 0;
    corpus.total_bytes = 0;

    list = bench_read_file(manifest_path, &list_len);
    if (list == NULL) {
        fprintf(stderr, "bench: cannot read manifest %s\n", manifest_path);
        exit(2);
    }
    corpus.docs = (bench_doc *)malloc(cap * sizeof(bench_doc));
    if (corpus.docs == NULL) { exit(2); }

    for (line = strtok_r(list, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        size_t dl;
        char *db;
        /* Skip blank lines. */
        if (line[0] == '\0')
            continue;
        db = bench_read_file(line, &dl);
        if (db == NULL) {
            fprintf(stderr, "bench: cannot read document %s\n", line);
            exit(2);
        }
        if (n == cap) {
            cap *= 2;
            corpus.docs = (bench_doc *)realloc(corpus.docs,
                                               cap * sizeof(bench_doc));
            if (corpus.docs == NULL) { exit(2); }
        }
        corpus.docs[n].bytes = db;
        corpus.docs[n].length = dl;
        corpus.total_bytes += dl;
        n++;
    }
    free(list);
    corpus.count = n;
    return corpus;
}

/* ** Monotonic wall-clock seconds. */
static double bench_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ** Compare two doubles for qsort. */
static int bench_cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/* ** Peak resident set size in kilobytes (getrusage ru_maxrss is KB on Linux). */
static long bench_peak_rss_kb(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return 0;
    return ru.ru_maxrss;
}

/* ** Report a result line in a machine-parseable KEY=VALUE form the orchestrator
** script greps for. parser is "tinyparse" or "tree-sitter". */
static void bench_report(const char *parser, double median_seconds,
                         size_t total_bytes, size_t iterations,
                         long peak_rss_kb)
{
    double mb = (double)total_bytes / (1024.0 * 1024.0);
    double mbps = median_seconds > 0.0 ? mb / median_seconds : 0.0;
    printf("BENCH parser=%s corpus_bytes=%zu iterations=%zu "
           "median_seconds=%.6f throughput_mbps=%.2f peak_rss_kb=%ld\n",
           parser, total_bytes, iterations, median_seconds, mbps,
           peak_rss_kb);
}

/* ** Report a traversal result in a machine-parseable KEY=VALUE form the
** orchestrator greps for. The walk visits a fixed number of nodes (a property
** of the built tree, not the iteration), so we report that node count once and
** normalize the median walk time by it: nodes per second, and nanoseconds of
** walk work per node. Because tinyparse and tree-sitter build trees of
** different shapes, each side reports its OWN node count, and ns/node is the
** per-node figure that makes the two comparable. */
static void bench_report_walk(const char *parser, double median_seconds,
                              size_t node_count, size_t iterations)
{
    double nodes_per_sec =
        median_seconds > 0.0 ? (double)node_count / median_seconds : 0.0;
    double ns_per_node =
        node_count > 0 ? median_seconds * 1e9 / (double)node_count : 0.0;
    printf("BENCH parser=%s-walk nodes=%zu iterations=%zu "
           "median_seconds=%.6f nodes_per_sec=%.0f ns_per_node=%.2f\n",
           parser, node_count, iterations, median_seconds, nodes_per_sec,
           ns_per_node);
}

#endif /* BENCH_COMMON_H */
