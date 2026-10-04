# Parse input from a C program

This tutorial is for a C developer who has written a grammar with the
`tinyparse` command, for example in [Write your first grammar](first-grammar.md).
You build one program, `walk.c`, in three stages: it compiles a grammar, then
parses input pushed in two chunks and prints the diagnostics, then walks the
concrete syntax tree (CST). You compile and run it at the end of every stage.

## Prerequisites

You need a C99 compiler and a built source tree: `include/tinyparse.h` and
`build/lib/libtinyparse.a` under the `tinyparse/` directory of the checkout.
Build them there:

```sh
mise run build
```

The program also builds against an installed release; [Install
tinyparse](../how-to/install.md) shows where the header and library go.

## Make a working directory

From the `tinyparse/` directory, remember where the source tree is, then move
to a new, empty directory:

```sh
TINYPARSE="$PWD"
cd "$(mktemp -d)"
```

## Write the grammar

The program parses s-expressions with the grammar the first tutorial ends with.
Save it as `sexpr.scm`:

```scheme
(grammar sexpr
  start: Source

  (token Space   (regex "[ \t\r\n]+"))
  (token Comment (seq ";" (repeat (not "\n" any))))
  (token Atom    (regex "[^() \t\r\n;]+"))

  (extras
    (trivia Space)
    (trivia Comment))

  (rule Source
    (seq (repeat Form) EOF))

  (rule Form
    (choice Atom List))

  (rule List
    (recover
      (seq
        (field open: "(")
        (repeat Form)
        (field close: ")"))
      (on eof
        message: "unterminated list"
        (label here: "expected `)` before end of file")
        help: "add a closing `)`"
        insert: ")"))))
```

## Stage 1: compile the grammar

Every string the library takes or returns is a
[`tinyparse_string`](../reference/c-api.md#strings): a pointer and a length,
with no terminating NUL required. The helper `bytes` builds one.

An engine takes all the memory the library uses from a
[memory grant](../reference/c-api.md#memory-grant) you give `tinyparse_open`.
`base` is the start of the block and `size` its length in bytes. `grow` is a
callback the engine calls when the block is full; `NULL` makes the block a
fixed budget. This program grants 4 MiB from `malloc`. `tinyparse_open`
returns `NULL` when the grant is too small to hold the engine.

`tinyparse_compile_scheme` takes a `tinyparse_source`, which pairs a name with
the grammar text, and writes a compiled grammar to its last argument. When a
call fails, `tinyparse_fault` returns the reason. The compiled grammar keeps
pointers into `grammar_text`, so the program frees the text only after it is
done with the grammar. `read_file` is an ordinary helper that reads a whole
file with `stdio`; the library itself does no I/O.

The program frees what it created in the reverse order of creation, and frees
the grant last. `tinyparse_grammar_free` and `tinyparse_close` mark where the
grammar's and the engine's use ends; the engine's memory goes back to the
system when the host frees the grant.

Save this as `walk.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyparse.h"

#define GRANT_SIZE (4u * 1024u * 1024u)

static tinyparse_string bytes(const char *pointer, size_t length)
{
    tinyparse_string out;
    out.pointer = (const uint8_t *)pointer;
    out.length = (uint32_t)length;
    return out;
}

static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *buffer = NULL;
    long size;

    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0) {
        rewind(file);
        buffer = malloc((size_t)size + 1);
        if (buffer != NULL)
            *length = fread(buffer, 1, (size_t)size, file);
    }
    fclose(file);
    return buffer;
}

int main(int argc, char **argv)
{
    tinyparse_configuration configuration;
    tinyparse_engine *engine;
    tinyparse_grammar *grammar = NULL;
    tinyparse_source source;
    tinyparse_result result;
    char *grammar_text;
    size_t grammar_length = 0;
    void *grant;
    int status = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s GRAMMAR INPUT\n", argv[0]);
        return 2;
    }

    /* Open an engine over a fixed memory grant. */
    grant = malloc(GRANT_SIZE);
    if (grant == NULL)
        return 1;
    memset(&configuration, 0, sizeof configuration);
    configuration.memory.base = grant;
    configuration.memory.size = GRANT_SIZE;
    configuration.memory.grow = NULL;
    engine = tinyparse_open(configuration);
    if (engine == NULL) {
        fprintf(stderr, "tinyparse_open: the grant is too small\n");
        free(grant);
        return 1;
    }

    /* Compile the grammar. */
    grammar_text = read_file(argv[1], &grammar_length);
    if (grammar_text == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        goto close_engine;
    }
    source.name = bytes(argv[1], strlen(argv[1]));
    source.bytes = bytes(grammar_text, grammar_length);
    result = tinyparse_compile_scheme(engine, source, &grammar);
    if (result != TINYPARSE_OK) {
        tinyparse_string fault = tinyparse_fault(engine);
        fprintf(stderr, "compile: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        goto free_text;
    }
    printf("grammar compiled\n");
    status = 0;

    /* Free in reverse order of creation. */
    tinyparse_grammar_free(grammar);
free_text:
    free(grammar_text);
close_engine:
    tinyparse_close(engine);
    free(grant);
    return status;
}
```

Compile it against the header and the static library:

```sh
cc -std=c99 -Wall -Wextra -pedantic -I"$TINYPARSE/include" walk.c "$TINYPARSE/build/lib/libtinyparse.a" -o walk
```

Run it:

```sh
./walk sexpr.scm '(a (b c))'
```

This stage reads the grammar only:

```text
grammar compiled
```

## Stage 2: parse and print the diagnostics

`tinyparse_parser_new` takes the name of the rule to start at. An empty string
selects the grammar's `start:` rule.

`tinyparse_push` hands the parser the next chunk of input, and
`tinyparse_finish` marks the end. The parser copies the chunks and parses when
you call `tinyparse_finish`. This program splits its input in the middle to
show two pushes. `tinyparse_finish` returns `TINYPARSE_OK` when the input
matched, possibly after recovery, and `TINYPARSE_SYNTAX_ERROR` when it did not.

A parser records [diagnostics](../reference/diagnostics.md) in both cases. Each
one has a message, a code, labels that point at byte ranges, and optional help
text. `print_diagnostics` prints them all, and `main` prints the result code.
`tinyparse_parser_free` returns the parser's memory to the engine.

Replace `walk.c` with:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyparse.h"

#define GRANT_SIZE (4u * 1024u * 1024u)

static const char *const result_names[] = {
    "TINYPARSE_OK", "TINYPARSE_MORE", "TINYPARSE_DONE",
    "TINYPARSE_OUT_OF_MEMORY", "TINYPARSE_INVALID",
    "TINYPARSE_SYNTAX_ERROR", "TINYPARSE_ERROR"
};

static tinyparse_string bytes(const char *pointer, size_t length)
{
    tinyparse_string out;
    out.pointer = (const uint8_t *)pointer;
    out.length = (uint32_t)length;
    return out;
}

static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *buffer = NULL;
    long size;

    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0) {
        rewind(file);
        buffer = malloc((size_t)size + 1);
        if (buffer != NULL)
            *length = fread(buffer, 1, (size_t)size, file);
    }
    fclose(file);
    return buffer;
}

static void print_diagnostics(const tinyparse_parser *parser)
{
    uint32_t count = tinyparse_parser_diagnostic_count(parser);
    uint32_t i, j;

    for (i = 0; i < count; i++) {
        const tinyparse_diagnostic *diagnostic =
            tinyparse_parser_diagnostic(parser, i);
        printf("diagnostic: %.*s [%.*s]\n",
               (int)diagnostic->message.length,
               (const char *)diagnostic->message.pointer,
               (int)diagnostic->code.length,
               (const char *)diagnostic->code.pointer);
        for (j = 0; j < diagnostic->label_count; j++) {
            const tinyparse_label *label = &diagnostic->labels[j];
            printf("  label [%u-%u]: %.*s\n", (unsigned)label->start_byte,
                   (unsigned)label->end_byte, (int)label->message.length,
                   (const char *)label->message.pointer);
        }
        if (diagnostic->help.length > 0)
            printf("  help: %.*s\n", (int)diagnostic->help.length,
                   (const char *)diagnostic->help.pointer);
    }
}

int main(int argc, char **argv)
{
    tinyparse_configuration configuration;
    tinyparse_engine *engine;
    tinyparse_grammar *grammar = NULL;
    tinyparse_parser *parser = NULL;
    tinyparse_source source;
    tinyparse_result result;
    char *grammar_text;
    size_t grammar_length = 0, input_length, half;
    const char *input;
    void *grant;
    int status = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s GRAMMAR INPUT\n", argv[0]);
        return 2;
    }
    input = argv[2];
    input_length = strlen(input);

    /* Open an engine over a fixed memory grant. */
    grant = malloc(GRANT_SIZE);
    if (grant == NULL)
        return 1;
    memset(&configuration, 0, sizeof configuration);
    configuration.memory.base = grant;
    configuration.memory.size = GRANT_SIZE;
    configuration.memory.grow = NULL;
    engine = tinyparse_open(configuration);
    if (engine == NULL) {
        fprintf(stderr, "tinyparse_open: the grant is too small\n");
        free(grant);
        return 1;
    }

    /* Compile the grammar. */
    grammar_text = read_file(argv[1], &grammar_length);
    if (grammar_text == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        goto close_engine;
    }
    source.name = bytes(argv[1], strlen(argv[1]));
    source.bytes = bytes(grammar_text, grammar_length);
    result = tinyparse_compile_scheme(engine, source, &grammar);
    if (result != TINYPARSE_OK) {
        tinyparse_string fault = tinyparse_fault(engine);
        fprintf(stderr, "compile: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        goto free_text;
    }

    /* Create a parser that starts at the grammar's start: rule. */
    result = tinyparse_parser_new(engine, grammar, bytes(NULL, 0), &parser);
    if (result != TINYPARSE_OK) {
        tinyparse_string fault = tinyparse_fault(engine);
        fprintf(stderr, "parser: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        goto free_grammar;
    }

    /* Push the input in two chunks, then finish. */
    half = input_length / 2;
    result = tinyparse_push(parser, bytes(input, half));
    if (result == TINYPARSE_OK)
        result = tinyparse_push(parser, bytes(input + half, input_length - half));
    if (result == TINYPARSE_OK)
        result = tinyparse_finish(parser);

    /* Report the result and the diagnostics. */
    print_diagnostics(parser);
    printf("result: %s\n", result_names[result]);
    if (result == TINYPARSE_OK && tinyparse_parser_diagnostic_count(parser) == 0)
        status = 0;

    /* Free in reverse order of creation. */
    tinyparse_parser_free(parser);
free_grammar:
    tinyparse_grammar_free(grammar);
free_text:
    free(grammar_text);
close_engine:
    tinyparse_close(engine);
    free(grant);
    return status;
}
```

Compile it with the same command:

```sh
cc -std=c99 -Wall -Wextra -pedantic -I"$TINYPARSE/include" walk.c "$TINYPARSE/build/lib/libtinyparse.a" -o walk
```

Leave the outer list open:

```sh
./walk sexpr.scm '(a (b c)'
```

Recovery completes the parse, so the result is `TINYPARSE_OK`, and the
diagnostic carries the label that the `tinyparse` command does not print:

```text
diagnostic: unterminated list [parse.recover]
  label [8-8]: expected `)` before end of file
  help: add a closing `)`
result: TINYPARSE_OK
```

## Stage 3: walk the tree

You read the tree through a cursor. `tinyparse_tree_cursor_new` places one at
the root; `goto_first_child`, `goto_next_sibling` and `goto_parent` move it and
return 0 when there is nowhere to go. At each node `print_node` prints
`MISSING` for a node that recovery inserted, the field name if the node has
one, the node kind, the byte range, and, for a node with no children, the input
text that range covers.

After `TINYPARSE_SYNTAX_ERROR` the tree holds only the nodes built before the
failure, and this program does not print it. The tree, its cursors and the
diagnostics belong to the parser, and `tinyparse_parser_free` invalidates
them, so the program reads them before it frees the parser.
[Ownership and lifetime](../reference/c-api.md#ownership-and-lifetime) lists
what each object's memory belongs to.

Replace `walk.c` with the whole program:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyparse.h"

#define GRANT_SIZE (4u * 1024u * 1024u)

static const char *const result_names[] = {
    "TINYPARSE_OK", "TINYPARSE_MORE", "TINYPARSE_DONE",
    "TINYPARSE_OUT_OF_MEMORY", "TINYPARSE_INVALID",
    "TINYPARSE_SYNTAX_ERROR", "TINYPARSE_ERROR"
};

static tinyparse_string bytes(const char *pointer, size_t length)
{
    tinyparse_string out;
    out.pointer = (const uint8_t *)pointer;
    out.length = (uint32_t)length;
    return out;
}

static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *buffer = NULL;
    long size;

    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0) {
        rewind(file);
        buffer = malloc((size_t)size + 1);
        if (buffer != NULL)
            *length = fread(buffer, 1, (size_t)size, file);
    }
    fclose(file);
    return buffer;
}

static void print_diagnostics(const tinyparse_parser *parser)
{
    uint32_t count = tinyparse_parser_diagnostic_count(parser);
    uint32_t i, j;

    for (i = 0; i < count; i++) {
        const tinyparse_diagnostic *diagnostic =
            tinyparse_parser_diagnostic(parser, i);
        printf("diagnostic: %.*s [%.*s]\n",
               (int)diagnostic->message.length,
               (const char *)diagnostic->message.pointer,
               (int)diagnostic->code.length,
               (const char *)diagnostic->code.pointer);
        for (j = 0; j < diagnostic->label_count; j++) {
            const tinyparse_label *label = &diagnostic->labels[j];
            printf("  label [%u-%u]: %.*s\n", (unsigned)label->start_byte,
                   (unsigned)label->end_byte, (int)label->message.length,
                   (const char *)label->message.pointer);
        }
        if (diagnostic->help.length > 0)
            printf("  help: %.*s\n", (int)diagnostic->help.length,
                   (const char *)diagnostic->help.pointer);
    }
}

static void print_node(const tinyparse_tree *tree,
                       tinyparse_tree_cursor *cursor, const char *input)
{
    tinyparse_string kind =
        tinyparse_tree_symbol_name(tree, tinyparse_tree_cursor_kind(cursor));
    tinyparse_symbol field = tinyparse_tree_cursor_field(cursor);
    uint32_t start = tinyparse_tree_cursor_start_byte(cursor);
    uint32_t end = tinyparse_tree_cursor_end_byte(cursor);

    printf("%*s", (int)(2 * tinyparse_tree_cursor_depth(cursor)), "");
    if (tinyparse_tree_cursor_flags(cursor) & TINYPARSE_NODE_MISSING)
        printf("MISSING ");
    if (field.id != TINYPARSE_NO_FIELD) {
        tinyparse_string name = tinyparse_tree_symbol_name(tree, field);
        printf("%.*s: ", (int)name.length, (const char *)name.pointer);
    }
    printf("%.*s [%u-%u]", (int)kind.length, (const char *)kind.pointer,
           (unsigned)start, (unsigned)end);
    if (tinyparse_tree_cursor_child_count(cursor) == 0)
        printf(" \"%.*s\"", (int)(end - start), input + start);
    printf("\n");

    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        do {
            print_node(tree, cursor, input);
        } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }
}

int main(int argc, char **argv)
{
    tinyparse_configuration configuration;
    tinyparse_engine *engine;
    tinyparse_grammar *grammar = NULL;
    tinyparse_parser *parser = NULL;
    tinyparse_tree_cursor *cursor = NULL;
    tinyparse_source source;
    tinyparse_result result;
    const tinyparse_tree *tree;
    char *grammar_text;
    size_t grammar_length = 0, input_length, half;
    const char *input;
    void *grant;
    int status = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s GRAMMAR INPUT\n", argv[0]);
        return 2;
    }
    input = argv[2];
    input_length = strlen(input);

    /* Open an engine over a fixed memory grant. */
    grant = malloc(GRANT_SIZE);
    if (grant == NULL)
        return 1;
    memset(&configuration, 0, sizeof configuration);
    configuration.memory.base = grant;
    configuration.memory.size = GRANT_SIZE;
    configuration.memory.grow = NULL;
    engine = tinyparse_open(configuration);
    if (engine == NULL) {
        fprintf(stderr, "tinyparse_open: the grant is too small\n");
        free(grant);
        return 1;
    }

    /* Compile the grammar. */
    grammar_text = read_file(argv[1], &grammar_length);
    if (grammar_text == NULL) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        goto close_engine;
    }
    source.name = bytes(argv[1], strlen(argv[1]));
    source.bytes = bytes(grammar_text, grammar_length);
    result = tinyparse_compile_scheme(engine, source, &grammar);
    if (result != TINYPARSE_OK) {
        tinyparse_string fault = tinyparse_fault(engine);
        fprintf(stderr, "compile: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        goto free_text;
    }

    /* Create a parser that starts at the grammar's start: rule. */
    result = tinyparse_parser_new(engine, grammar, bytes(NULL, 0), &parser);
    if (result != TINYPARSE_OK) {
        tinyparse_string fault = tinyparse_fault(engine);
        fprintf(stderr, "parser: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        goto free_grammar;
    }

    /* Push the input in two chunks, then finish. */
    half = input_length / 2;
    result = tinyparse_push(parser, bytes(input, half));
    if (result == TINYPARSE_OK)
        result = tinyparse_push(parser, bytes(input + half, input_length - half));
    if (result == TINYPARSE_OK)
        result = tinyparse_finish(parser);

    /* Print the diagnostics, then walk the tree. */
    print_diagnostics(parser);
    tree = tinyparse_parser_tree(parser);
    if (result != TINYPARSE_OK || tree == NULL) {
        printf("parse failed: %s\n", result_names[result]);
        goto free_parser;
    }
    if (tinyparse_tree_cursor_new(tree, &cursor) != TINYPARSE_OK) {
        fprintf(stderr, "out of memory for a cursor\n");
        goto free_parser;
    }
    print_node(tree, cursor, input);
    tinyparse_tree_cursor_free(cursor);
    if (tinyparse_parser_diagnostic_count(parser) == 0)
        status = 0;

    /* Free in reverse order of creation. */
free_parser:
    tinyparse_parser_free(parser);
free_grammar:
    tinyparse_grammar_free(grammar);
free_text:
    free(grammar_text);
close_engine:
    tinyparse_close(engine);
    free(grant);
    return status;
}
```

Compile it with the same command:

```sh
cc -std=c99 -Wall -Wextra -pedantic -I"$TINYPARSE/include" walk.c "$TINYPARSE/build/lib/libtinyparse.a" -o walk
```

Parse a nested list:

```sh
./walk sexpr.scm '(a (b c))'
```

There are no diagnostics, and every leaf shows its text:

```text
Source [0-9]
  Form [0-9]
    List [0-9]
      open: ( [0-1] "("
      Form [1-2]
        Atom [1-2] "a"
      Space [2-3] " "
      Form [3-8]
        List [3-8]
          open: ( [3-4] "("
          Form [4-5]
            Atom [4-5] "b"
          Space [5-6] " "
          Form [6-7]
            Atom [6-7] "c"
          close: ) [7-8] ")"
      close: ) [8-9] ")"
```

Leave the outer list open again:

```sh
./walk sexpr.scm '(a (b c)'
```

The tree ends with the `MISSING )` node that recovery inserted:

```text
diagnostic: unterminated list [parse.recover]
  label [8-8]: expected `)` before end of file
  help: add a closing `)`
Source [0-8]
  Form [0-8]
    List [0-8]
      open: ( [0-1] "("
      Form [1-2]
        Atom [1-2] "a"
      Space [2-3] " "
      Form [3-8]
        List [3-8]
          open: ( [3-4] "("
          Form [4-5]
            Atom [4-5] "b"
          Space [5-6] " "
          Form [6-7]
            Atom [6-7] "c"
          close: ) [7-8] ")"
      MISSING ) [8-8] ""
```

Add a closing parenthesis too many:

```sh
./walk sexpr.scm '(a))'
```

Recovery covers only an input that ends early, so this parse fails. The label
points at the extra `)`:

```text
diagnostic: error: unexpected input [parse.syntax]
  label [3-4]: parse failed here
parse failed: TINYPARSE_SYNTAX_ERROR
```

```sh
echo $?
```

```text
1
```

## Where to go next

- [C API](../reference/c-api.md) lists every function, result code and
  structure used here, and how to size a memory grant.
- [Supply imports, classes and parameters from a C host](../how-to/host-services.md)
  connects a grammar to services your program provides.
- [Run tinyparse without libc](../how-to/freestanding.md) builds the library
  for a target with no C library.
- [Ship a generated C parser](../how-to/generated-parser.md) turns `sexpr.scm`
  into C source you compile into your program.
