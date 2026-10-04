/*
** lifecycle.c -- direct checks of the arena allocator (src/arena.c) and the
** parser lifecycle (tp_parser_run and friends in src/parse.c), the two pieces
** that replaced the ax runtime tinyparse was born in.
**
** Built and run by test/suite/core/lifecycle.test against the static library,
** with src/ on the include path for the internal declarations. Every check
** prints one "ok <name>" line; a broken property prints "FAIL <name>" and the
** program exits 1.
*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static int failures;

/* ** Record one property: print ok or FAIL with its name. */
static void check(int holds, const char *name)
{
    printf("%s %s\n", holds ? "ok" : "FAIL", name);
    if (!holds)
        failures++;
}

/* ** A grow callback that counts its calls, records the last request, and
** hands back a block twice the size asked for (as a host that maps in coarse
** units does), or NULL once its budget of blocks is spent. */
typedef struct grow_state {
    int    calls;
    int    budget;
    size_t last_need;
    void  *blocks[64];
} grow_state;

static void *grow_counting(void *user, size_t need, size_t *got)
{
    grow_state *state = (grow_state *)user;
    void       *block;
    state->calls++;
    state->last_need = need;
    if (state->budget == 0 || state->calls > 64)
        return NULL;
    state->budget--;
    block = aligned_alloc(16, ((need * 2) + 15) & ~(size_t)15);
    if (block == NULL)
        return NULL;
    state->blocks[state->calls - 1] = block;
    *got = need * 2;
    return block;
}

/* ** A memory grant over `base', growable through `grow' when it is given. */
static tinyparse_memory memory_over(void *base, size_t size, grow_state *grow)
{
    tinyparse_memory memory;
    memset(&memory, 0, sizeof memory);
    memory.base = base;
    memory.size = size;
    if (grow != NULL) {
        memory.grow = grow_counting;
        memory.user = grow;
    }
    return memory;
}

/* ** The arena's contract: alignment, zeroing, recycling, the fixed and the
** growable grant, and NULL as the single out-of-memory signal. */
static void arena_checks(void)
{
    static unsigned char grant[64 * 1024] __attribute__((aligned(16)));
    grow_state           grow;
    tp_arena            *root;
    tp_arena            *child;
    unsigned char       *first;
    unsigned char       *again;
    size_t               size;
    int                  aligned = 1;
    int                  zero    = 1;
    int                  i;

    /* A grant too small for the bookkeeping plus one chunk opens nothing. */
    check(tp_arena_open(memory_over(grant, 256, NULL)) == NULL, "arena-open-too-small");
    check(tp_arena_open(memory_over(NULL, 4096, NULL)) == NULL, "arena-open-null-base");

    root = tp_arena_open(memory_over(grant, sizeof grant, NULL));
    check(root != NULL, "arena-open");
    if (root == NULL)
        return;

    /* Every hand-out is 16-byte aligned, whatever the request size. */
    for (size = 1; size < 200; size += 7) {
        void *p = tp_alloc(root, size);
        if (p == NULL || ((uintptr_t)p & 15u) != 0)
            aligned = 0;
    }
    check(aligned, "arena-alignment");

    /* A child's chunks return to the pool on reset and are reused first-fit:
    ** the same request after a reset lands on the same bytes. tp_alloc_raw sees
    ** what the last user wrote; tp_alloc clears it. */
    child = tp_arena_child(root);
    check(child != NULL, "arena-child");
    if (child == NULL)
        return;
    first = (unsigned char *)tp_alloc(child, 1000);
    memset(first, 0xAB, 1000);
    tp_arena_reset(child);
    again = (unsigned char *)tp_alloc_raw(child, 1000);
    check(again == first, "arena-reset-recycles");
    check(again != NULL && again[0] == 0xAB && again[999] == 0xAB, "arena-raw-not-zeroed");
    memset(again, 0xCD, 1000);
    tp_arena_reset(child);
    again = (unsigned char *)tp_alloc(child, 1000);
    for (i = 0; again != NULL && i < 1000; i++)
        if (again[i] != 0)
            zero = 0;
    check(again == first && zero, "arena-alloc-zeroes-recycled");

    /* A request larger than the chunk floor gets a chunk of its own. */
    tp_arena_reset(child);
    check(tp_alloc(child, 20000) != NULL, "arena-oversized-request");

    /* With no grow callback the grant is fixed: allocation fails with NULL. */
    tp_arena_reset(child);
    for (i = 0; i < 1000; i++)
        if (tp_alloc(child, 4096) == NULL)
            break;
    check(i < 1000, "arena-fixed-grant-exhausts-to-null");
    check(tp_alloc(child, 4096) == NULL, "arena-spent-stays-null");

    /* A request so large its size cannot be rounded is refused, not wrapped. */
    check(tp_alloc(root, SIZE_MAX) == NULL, "arena-overflow-refused");
    check(tp_alloc_raw(root, SIZE_MAX - 8) == NULL, "arena-overflow-raw-refused");

    /* The grow callback. The grant holds one 4 KiB chunk, so the second
    ** allocation grows, asking for at least the chunk floor. The host reports
    ** twice what was asked, so the third allocation's chunk is carved from the
    ** same block without asking again. A refusal surfaces as NULL. */
    memset(&grow, 0, sizeof grow);
    grow.budget = 1;
    root = tp_arena_open(memory_over(grant, 8192, &grow));
    check(root != NULL, "arena-open-growable");
    if (root == NULL)
        return;
    for (i = 0; i < 2; i++)
        (void)tp_alloc(root, 3000);
    check(grow.calls == 1 && grow.last_need >= 4096, "arena-grow-asked-for-a-chunk");
    check(tp_alloc(root, 3000) != NULL && grow.calls == 1, "arena-grow-got-kept");
    for (i = 0; i < 100; i++)
        if (tp_alloc(root, 3000) == NULL)
            break;
    check(i < 100 && grow.calls >= 2, "arena-grow-refusal-is-null");
    for (i = 0; i < 64; i++)
        free(grow.blocks[i]);
}

/* ** An executor that counts how often the lifecycle runs it. */
static int executor_calls;
static tinyparse_result counting_executor(tinyparse_parser *parser)
{
    (void)parser;
    executor_calls++;
    return TINYPARSE_OK;
}

static tinyparse_string text(const char *s)
{
    tinyparse_string out;
    out.pointer = (const uint8_t *)s;
    out.length  = (uint32_t)strlen(s);
    return out;
}

static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *bytes;
    long  size;
    if (file == NULL)
        return NULL;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    bytes = (char *)malloc((size_t)size + 1);
    if (bytes == NULL || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(bytes);
        return NULL;
    }
    fclose(file);
    *length = (size_t)size;
    return bytes;
}

/* ** Open an engine over a fixed grant (no grow) and compile the grammar. */
static tinyparse_engine *open_engine(void *block, size_t size,
                                       tinyparse_grammar **grammar,
                                       const char *source, size_t length)
{
    tinyparse_configuration configuration;
    tinyparse_engine       *engine;
    tinyparse_source        grammar_source;
    memset(&configuration, 0, sizeof configuration);
    configuration.memory.base = block;
    configuration.memory.size = size;
    engine = tinyparse_open(configuration);
    if (engine == NULL)
        return NULL;
    grammar_source.name          = text("sexpr.scm");
    grammar_source.bytes.pointer = (const uint8_t *)source;
    grammar_source.bytes.length  = (uint32_t)length;
    if (tinyparse_compile_scheme(engine, grammar_source, grammar) != TINYPARSE_OK)
        return NULL;
    return engine;
}

/* ** The parser lifecycle: accepting, dropped, finished, failed. */
static void lifecycle_checks(const char *grammar_path)
{
    size_t               length = 0;
    char                *source = read_file(grammar_path, &length);
    size_t               size   = 4u * 1024u * 1024u;
    void                *block  = aligned_alloc(16, size);
    tinyparse_engine  *engine;
    tinyparse_grammar *grammar = NULL;
    tinyparse_parser  *parser  = NULL;
    tinyparse_result   result;
    tinyparse_string   big_bytes;
    char                *big;
    int                  i;
    int                  reuse_ok = 1;

    check(source != NULL && block != NULL, "grammar-read");
    if (source == NULL || block == NULL)
        return;
    engine = open_engine(block, size, &grammar, source, length);
    check(engine != NULL, "engine-open-and-compile");
    if (engine == NULL)
        return;

    /* The happy path: accepting -> finished, a tree, and finish is idempotent. */
    check(tinyparse_parser_new(engine, grammar, text(""), &parser) == TINYPARSE_OK,
          "parser-new");
    check(strcmp(tp_parser_state_name(parser), "accepting") == 0, "state-accepting");
    check(tinyparse_push(parser, text("(a ")) == TINYPARSE_OK, "push-1");
    check(tinyparse_push(parser, text("")) == TINYPARSE_OK, "push-empty");
    check(tinyparse_push(parser, text("(b c))")) == TINYPARSE_OK, "push-2");
    check(tinyparse_finish(parser) == TINYPARSE_OK, "finish");
    check(strcmp(tp_parser_state_name(parser), "finished") == 0, "state-finished");
    check(tinyparse_parser_tree(parser) != NULL, "tree-built");
    check(tinyparse_push(parser, text("x")) == TINYPARSE_INVALID, "push-after-finish-invalid");
    check(tinyparse_finish(parser) == TINYPARSE_OK, "finish-twice-ok");
    tinyparse_parser_free(parser);

    /* Free without finish is legal and leaves the engine usable. */
    tinyparse_parser_new(engine, grammar, text(""), &parser);
    tinyparse_push(parser, text("(a b)"));
    tinyparse_parser_free(parser);

    /* Many parses in one engine reuse the parser arenas: a fixed 4 MB grant
    ** with no grow survives 2000 of them. */
    for (i = 0; i < 2000; i++) {
        if (tinyparse_parser_new(engine, grammar, text(""), &parser) != TINYPARSE_OK ||
            tinyparse_push(parser, text("(a (b c) d (e (f g)))")) != TINYPARSE_OK ||
            tinyparse_finish(parser) != TINYPARSE_OK ||
            tinyparse_parser_tree(parser) == NULL) {
            reuse_ok = 0;
            break;
        }
        tinyparse_parser_free(parser);
    }
    check(reuse_ok, "engine-reuse-2000-parses");

    /* Out of memory during push: the fixed grant cannot hold 8 MB of input.
    ** The parser moves to dropped, later pushes still buffer, and running it --
    ** as a generated <prefix>_run does, once or twice -- returns the push's
    ** failure without ever calling the executor. */
    big = (char *)malloc(8u * 1024u * 1024u);
    if (big == NULL)
        return;
    memset(big, ' ', 8u * 1024u * 1024u);
    big_bytes.pointer = (const uint8_t *)big;
    big_bytes.length  = 8u * 1024u * 1024u;
    tinyparse_parser_new(engine, grammar, text(""), &parser);
    tinyparse_push(parser, text("(a "));
    result = tinyparse_push(parser, big_bytes);
    check(result == TINYPARSE_OUT_OF_MEMORY, "push-oom");
    check(strcmp(tp_parser_state_name(parser), "dropped") == 0, "state-dropped");
    check(tinyparse_push(parser, text("b)")) == TINYPARSE_OK, "push-after-drop-buffers");
    check(strcmp(tp_parser_state_name(parser), "dropped") == 0, "state-still-dropped");
    executor_calls = 0;
    check(tp_parser_run(parser, counting_executor) == TINYPARSE_OUT_OF_MEMORY,
          "run-after-drop-returns-failure");
    check(strcmp(tp_parser_state_name(parser), "failed") == 0, "state-failed");
    check(tp_parser_run(parser, counting_executor) == TINYPARSE_OUT_OF_MEMORY,
          "run-again-after-failure-returns-failure");
    check(executor_calls == 0, "executor-never-runs-after-drop");
    check(tinyparse_finish(parser) == TINYPARSE_SYNTAX_ERROR, "finish-after-failure-no-tree");
    check(tinyparse_push(parser, text("x")) == TINYPARSE_INVALID, "push-after-failure-invalid");
    tinyparse_parser_free(parser);

    /* The same through tinyparse_finish: the fault names the dropped chunk. */
    tinyparse_parser_new(engine, grammar, text(""), &parser);
    tinyparse_push(parser, big_bytes);
    check(tinyparse_finish(parser) == TINYPARSE_OUT_OF_MEMORY, "finish-after-drop-oom");
    {
        tinyparse_string fault = tinyparse_fault(engine);
        check(fault.length > 0 && memmem(fault.pointer, fault.length, "dropped", 7) != NULL,
              "finish-after-drop-fault");
    }
    check(tinyparse_parser_tree(parser) == NULL, "no-tree-after-drop");
    tinyparse_parser_free(parser);

    /* After all of that the engine still parses. */
    tinyparse_parser_new(engine, grammar, text(""), &parser);
    tinyparse_push(parser, text("(ok)"));
    check(tinyparse_finish(parser) == TINYPARSE_OK && tinyparse_parser_tree(parser) != NULL,
          "engine-usable-after-oom");
    tinyparse_parser_free(parser);

    tinyparse_close(engine);
    free(big);
    free(block);
    free(source);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s GRAMMAR.scm\n", argv[0]);
        return 2;
    }
    arena_checks();
    lifecycle_checks(argv[1]);
    printf("failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
