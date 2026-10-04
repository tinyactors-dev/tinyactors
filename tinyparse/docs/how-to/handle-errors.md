# Recover from and report syntax errors

This guide is for grammar authors and C host authors whose users write broken
input. It ends with a grammar that still builds a tree when a list is left
open or holds stray bytes, and a C program that prints each error with its
line, column and the line of input it points at.

## Prerequisites

- A source checkout with the library and the command built, and `tinyparse` on
  your `PATH`. From the `tinyparse/` directory:

  ```sh
  mise run build
  export PATH="$PWD/build/bin:$PATH"
  ```

  [Install tinyparse](install.md) covers the other ways to install it. The
  commands below run from `tinyparse/`.
- A C99 compiler, called `cc` below.
- A directory for this guide's files:

  ```sh
  mkdir -p errors
  ```

## Recover when the input ends early

A [`recover`](../reference/grammar.md#recover) form makes a rule finish when
the input ends inside it: the parser inserts a zero-width `MISSING` node for
each element still expected and records a `parse.recover` diagnostic. Wrap
the whole body of the rule in it. Save this grammar as `errors/list.scm`:

```scheme
(tinyparse version: "0.1")

(grammar list
  start: Source

  (token Space (regex "[ \t\r\n]+"))
  (token Atom  (regex "[^() \t\r\n]+"))

  (extras
    (trivia Space))

  (rule Source
    (seq (repeat Item) EOF))

  (rule Item
    (choice Atom List))

  (rule List
    (recover
      (seq
        (field open: "(")
        (repeat Item)
        (field close: ")"))
      (on eof
        message: "error: unterminated list"
        (label here: "expected `)` before end of file")
        help: "add a closing `)`"
        insert: ")"))))
```

The `(on eof ...)` clause fills the diagnostic. `message:` is required and is
used as written; the library's own messages start with `error: `, so this one
does too. `insert:` names the kind of the inserted node. The `label` text
points at the insertion and `help:` tells the user what to do.

Parse a list whose outer `)` is missing:

```sh
printf '(a (b c)' | tinyparse run errors/list.scm --input - --dump-tree
```

The tree goes to standard output and ends with the inserted node:

```text
Source [0-8] [0,0]-[0,8]
  Item [0-8] [0,0]-[0,8]
    List [0-8] [0,0]-[0,8]
      open: ( [0-1] [0,0]-[0,1]
      Item [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      Space [2-3] [0,2]-[0,3]
      Item [3-8] [0,3]-[0,8]
        List [3-8] [0,3]-[0,8]
          open: ( [3-4] [0,3]-[0,4]
          Item [4-5] [0,4]-[0,5]
            Atom [4-5] [0,4]-[0,5]
          Space [5-6] [0,5]-[0,6]
          Item [6-7] [0,6]-[0,7]
            Atom [6-7] [0,6]-[0,7]
          close: ) [7-8] [0,7]-[0,8]
      MISSING ) [8-8] [0,8]-[0,8]
```

The diagnostic goes to standard error, and the command exits with status 1:

```text
error: unterminated list [parse.recover]
  help: add a closing `)`
```

`eof` is the only trigger. Input that goes wrong before its end gets no
recovery and no tree: `(a))` prints `error: unexpected input [parse.syntax]`
and exits with status 1.

## Skip stray bytes

An `(error TOKEN)` entry in [`extras`](../reference/grammar.md#extras) lets
the parser step over bytes the grammar has no place for: it keeps them as a
leaf of kind `TOKEN` and records an `extra.error` diagnostic. The error token
must not match the first byte of anything the grammar expects, because the
parser tries extras before every token. Narrow `Atom` to the characters an
atom may hold, and add a `Stray` token for one byte of anything else.
`errors/list.scm` becomes:

```scheme
(tinyparse version: "0.1")

(grammar list
  start: Source

  (token Space (regex "[ \t\r\n]+"))
  (token Atom  (regex "[A-Za-z0-9_]+"))
  (token Stray (regex "[^A-Za-z0-9_() \t\r\n]"))

  (extras
    (trivia Space)
    (error Stray))

  (rule Source
    (seq (repeat Item) EOF))

  (rule Item
    (choice Atom List))

  (rule List
    (recover
      (seq
        (field open: "(")
        (repeat Item)
        (field close: ")"))
      (on eof
        message: "error: unterminated list"
        (label here: "expected `)` before end of file")
        help: "add a closing `)`"
        insert: ")"))))
```

Write an input with a stray `@` in its first list and an open second list:

```sh
printf '(a @ b)\n(c (d e)' > errors/input.txt
tinyparse run errors/list.scm --input errors/input.txt --dump-tree
```

The tree holds the `@` as a `Stray` leaf and the inserted `)`:

```text
Source [0-16] [0,0]-[1,8]
  Item [0-7] [0,0]-[0,7]
    List [0-7] [0,0]-[0,7]
      open: ( [0-1] [0,0]-[0,1]
      Item [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      Space [2-3] [0,2]-[0,3]
      Stray [3-4] [0,3]-[0,4]
      Space [4-5] [0,4]-[0,5]
      Item [5-6] [0,5]-[0,6]
        Atom [5-6] [0,5]-[0,6]
      close: ) [6-7] [0,6]-[0,7]
  Space [7-8] [0,7]-[1,0]
  Item [8-16] [1,0]-[1,8]
    List [8-16] [1,0]-[1,8]
      open: ( [8-9] [1,0]-[1,1]
      Item [9-10] [1,1]-[1,2]
        Atom [9-10] [1,1]-[1,2]
      Space [10-11] [1,2]-[1,3]
      Item [11-16] [1,3]-[1,8]
        List [11-16] [1,3]-[1,8]
          open: ( [11-12] [1,3]-[1,4]
          Item [12-13] [1,4]-[1,5]
            Atom [12-13] [1,4]-[1,5]
          Space [13-14] [1,5]-[1,6]
          Item [14-15] [1,6]-[1,7]
            Atom [14-15] [1,6]-[1,7]
          close: ) [15-16] [1,7]-[1,8]
      MISSING ) [16-16] [1,8]-[1,8]
```

Standard error has one diagnostic for each:

```text
error: unexpected input [extra.error]
error: unterminated list [parse.recover]
  help: add a closing `)`
```

With `(token Stray any)` in place of the narrow token, `Stray` also matches the
first byte of every atom. The same input then parses into 11 `Stray` leaves,
no `Atom`, and 22 `extra.error` diagnostics.

## Report errors with line and column

The `tinyparse` command prints no positions. A host program reads them from
the diagnostics: each one has labels, and each label has a byte range in the
input ([C API](../reference/c-api.md#diagnostics) lists every field). This
program parses a file, turns the start of each label into a 1-based line and
byte column by counting newlines, and prints the input line with a caret under the
label. The column counts bytes: each byte of a multi-byte UTF-8 character
before the label adds 1 to the column and one space before the caret. A parse
that backtracks over an error extra records its diagnostic again, so the
program skips a diagnostic whose code and label match an earlier one. A result
code other than `TINYPARSE_OK` or `TINYPARSE_SYNTAX_ERROR`, such as
`TINYPARSE_OUT_OF_MEMORY` when the 4 MiB grant runs out, goes to standard error
with the engine's fault when it has one. Save it as `errors/report.c`:

```c
/* report.c: parse a file and print each diagnostic with its line and column. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinyparse.h"

#define GRANT_SIZE (4u * 1024u * 1024u)

/* Read a whole file into a malloc'd buffer. */
static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *bytes = NULL;
    long size;

    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0 &&
        fseek(file, 0, SEEK_SET) == 0) {
        bytes = malloc((size_t)size + 1);
        if (bytes != NULL &&
            fread(bytes, 1, (size_t)size, file) != (size_t)size) {
            free(bytes);
            bytes = NULL;
        }
        *length = (size_t)size;
    }
    fclose(file);
    return bytes;
}

/* Find the 0-based row of a byte offset and the offset where that row starts. */
static void locate(const char *input, uint32_t offset, uint32_t *row,
                   uint32_t *row_start)
{
    uint32_t i;
    *row = 0;
    *row_start = 0;
    for (i = 0; i < offset; i++) {
        if (input[i] == '\n') {
            (*row)++;
            *row_start = i + 1;
        }
    }
}

/* True when an earlier diagnostic has the same code and the same first label. */
static int seen_before(const tinyparse_parser *parser, uint32_t index)
{
    const tinyparse_diagnostic *d = tinyparse_parser_diagnostic(parser, index);
    uint32_t i;
    for (i = 0; i < index; i++) {
        const tinyparse_diagnostic *e = tinyparse_parser_diagnostic(parser, i);
        if (e->code.length == d->code.length &&
            memcmp(e->code.pointer, d->code.pointer, d->code.length) == 0 &&
            e->label_count > 0 && d->label_count > 0 &&
            e->labels[0].start_byte == d->labels[0].start_byte &&
            e->labels[0].end_byte == d->labels[0].end_byte)
            return 1;
    }
    return 0;
}

/* Print one diagnostic: file, line and column of its first label, the message
** and code, the input line with a caret under each label, then the help. */
static void report(const char *path, const char *input, size_t input_length,
                   const tinyparse_diagnostic *diagnostic)
{
    uint32_t row = 0, row_start = 0, row_end, i, j;
    uint32_t at = diagnostic->label_count > 0 ? diagnostic->labels[0].start_byte
                                              : 0;

    locate(input, at, &row, &row_start);
    printf("%s:%u:%u: %.*s [%.*s]\n", path, (unsigned)row + 1,
           (unsigned)(at - row_start) + 1, (int)diagnostic->message.length,
           (const char *)diagnostic->message.pointer,
           (int)diagnostic->code.length,
           (const char *)diagnostic->code.pointer);

    for (i = 0; i < diagnostic->label_count; i++) {
        const tinyparse_label *label = &diagnostic->labels[i];
        locate(input, label->start_byte, &row, &row_start);
        row_end = row_start;
        while (row_end < input_length && input[row_end] != '\n')
            row_end++;
        printf("  | %.*s\n  | ", (int)(row_end - row_start), input + row_start);
        for (j = row_start; j < label->start_byte; j++)
            putchar(input[j] == '\t' ? '\t' : ' ');
        printf("^ %.*s\n", (int)label->message.length,
               (const char *)label->message.pointer);
    }
    if (diagnostic->help.length > 0)
        printf("  help: %.*s\n", (int)diagnostic->help.length,
               (const char *)diagnostic->help.pointer);
}

/* Name a result code for a message. */
static const char *result_name(tinyparse_result result)
{
    switch (result) {
    case TINYPARSE_OK:            return "TINYPARSE_OK";
    case TINYPARSE_MORE:          return "TINYPARSE_MORE";
    case TINYPARSE_DONE:          return "TINYPARSE_DONE";
    case TINYPARSE_OUT_OF_MEMORY: return "TINYPARSE_OUT_OF_MEMORY";
    case TINYPARSE_INVALID:       return "TINYPARSE_INVALID";
    case TINYPARSE_SYNTAX_ERROR:  return "TINYPARSE_SYNTAX_ERROR";
    case TINYPARSE_ERROR:         return "TINYPARSE_ERROR";
    }
    return "unknown result";
}

/* Count the nodes recovery inserted. */
static uint32_t count_missing(tinyparse_tree_cursor *cursor)
{
    uint32_t missing =
        (tinyparse_tree_cursor_flags(cursor) & TINYPARSE_NODE_MISSING) ? 1 : 0;
    if (tinyparse_tree_cursor_goto_first_child(cursor)) {
        do {
            missing += count_missing(cursor);
        } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
        tinyparse_tree_cursor_goto_parent(cursor);
    }
    return missing;
}

int main(int argc, char **argv)
{
    tinyparse_configuration configuration;
    tinyparse_engine *engine = NULL;
    tinyparse_grammar *grammar = NULL;
    tinyparse_parser *parser = NULL;
    tinyparse_tree_cursor *cursor = NULL;
    tinyparse_source source;
    tinyparse_string bytes, start;
    tinyparse_result result;
    char *grammar_text = NULL, *input = NULL;
    size_t grammar_length = 0, input_length = 0;
    uint32_t i, errors = 0, missing = 0;
    void *grant = NULL;
    int status = 2;

    if (argc != 3) {
        fprintf(stderr, "usage: report GRAMMAR INPUT\n");
        return 2;
    }
    grammar_text = read_file(argv[1], &grammar_length);
    input = read_file(argv[2], &input_length);
    if (grammar_text == NULL || input == NULL) {
        fprintf(stderr, "report: cannot read %s\n",
                grammar_text == NULL ? argv[1] : argv[2]);
        goto done;
    }
    grant = malloc(GRANT_SIZE);
    if (grant == NULL) {
        fprintf(stderr, "report: cannot allocate the memory grant\n");
        goto done;
    }

    memset(&configuration, 0, sizeof configuration);
    configuration.memory.base = grant;
    configuration.memory.size = GRANT_SIZE;
    engine = tinyparse_open(configuration);
    if (engine == NULL) {
        fprintf(stderr, "report: the memory grant is too small\n");
        goto done;
    }

    source.name.pointer = (const uint8_t *)argv[1];
    source.name.length = (uint32_t)strlen(argv[1]);
    source.bytes.pointer = (const uint8_t *)grammar_text;
    source.bytes.length = (uint32_t)grammar_length;
    start.pointer = NULL;
    start.length = 0;
    if (tinyparse_compile_scheme(engine, source, &grammar) != TINYPARSE_OK ||
        tinyparse_parser_new(engine, grammar, start, &parser) != TINYPARSE_OK) {
        tinyparse_string fault = tinyparse_fault(engine);
        fprintf(stderr, "report: %s: %.*s\n", argv[1], (int)fault.length,
                (const char *)fault.pointer);
        goto done;
    }

    bytes.pointer = (const uint8_t *)input;
    bytes.length = (uint32_t)input_length;
    result = tinyparse_push(parser, bytes);
    if (result == TINYPARSE_OK)
        result = tinyparse_finish(parser);

    for (i = 0; i < tinyparse_parser_diagnostic_count(parser); i++) {
        const tinyparse_diagnostic *diagnostic =
            tinyparse_parser_diagnostic(parser, i);
        if (diagnostic->severity == TINYPARSE_SEVERITY_ERROR)
            errors++;
        if (!seen_before(parser, i))
            report(argv[2], input, input_length, diagnostic);
    }

    if (result == TINYPARSE_SYNTAX_ERROR) {
        printf("failed: no tree\n");
        status = 1;
        goto done;
    }
    if (result != TINYPARSE_OK) {
        tinyparse_string fault = tinyparse_fault(engine);
        fprintf(stderr, "report: %s: %s%s%.*s\n", argv[2], result_name(result),
                fault.length > 0 ? ": " : "", (int)fault.length,
                (const char *)fault.pointer);
        goto done;
    }
    if (tinyparse_tree_cursor_new(tinyparse_parser_tree(parser), &cursor) !=
        TINYPARSE_OK) {
        fprintf(stderr, "report: out of memory\n");
        goto done;
    }
    missing = count_missing(cursor);
    if (errors > 0) {
        printf("recovered: missing nodes: %u\n", (unsigned)missing);
        status = 1;
    } else {
        printf("clean\n");
        status = 0;
    }

done:
    if (cursor != NULL)
        tinyparse_tree_cursor_free(cursor);
    if (parser != NULL)
        tinyparse_parser_free(parser);
    if (grammar != NULL)
        tinyparse_grammar_free(grammar);
    if (engine != NULL)
        tinyparse_close(engine);
    free(grant);
    free(grammar_text);
    free(input);
    return status;
}
```

Compile it against the library:

```sh
cc -std=c99 -Wall -Wextra -pedantic -Werror -I include errors/report.c build/lib/libtinyparse.a -o errors/report
```

Run it on the input from the previous step:

```sh
errors/report errors/list.scm errors/input.txt
```

It prints both errors with their positions and exits with status 1:

```text
errors/input.txt:1:4: error: unexpected input [extra.error]
  | (a @ b)
  |    ^ unexpected here
errors/input.txt:2:9: error: unterminated list [parse.recover]
  | (c (d e)
  |         ^ expected `)` before end of file
  help: add a closing `)`
recovered: missing nodes: 1
```

For `((a` the library records the `parse.recover` diagnostic 3 times at
byte 3; the program prints it once:

```sh
printf '((a' > errors/nested.txt
errors/report errors/list.scm errors/nested.txt
```

```text
errors/nested.txt:1:4: error: unterminated list [parse.recover]
  | ((a
  |    ^ expected `)` before end of file
  help: add a closing `)`
recovered: missing nodes: 2
```

400,000 nested open lists spend the 4 MiB grant. The parse returns
`TINYPARSE_OUT_OF_MEMORY` with an empty fault, and the program exits with
status 2:

```sh
awk 'BEGIN { for (i = 0; i < 400000; i++) printf "(a " }' > errors/deep.txt
errors/report errors/list.scm errors/deep.txt
```

```text
report: errors/deep.txt: TINYPARSE_OUT_OF_MEMORY
```

## Tell a clean parse from a recovered one

The result of `tinyparse_finish` and the diagnostics together give three
outcomes, and the last lines of `main` above act on them:

| `tinyparse_finish` returns | Error diagnostics | Outcome | Tree |
|----------------------------|-------------------|---------|------|
| `TINYPARSE_OK` | none | clean | the input as written |
| `TINYPARSE_OK` | one or more | recovered | every byte of the input, `MISSING` nodes where recovery inserted elements (`TINYPARSE_NODE_MISSING` in `tinyparse_tree_cursor_flags`), and error-extra leaves over stray bytes |
| `TINYPARSE_SYNTAX_ERROR` or another failure | `parse.syntax`, `parse.trailing` or `parse.no-start`, or none | failed | none, or the nodes built before the failure; the host must not use it |

A recovered tree is safe to walk, query and print. Whether to act on it, for
example to keep highlighting a file while the user types, or to refuse it, is
the host's decision. The program above exits with status 0 for a clean parse,
1 for a recovered or failed one, and 2 when it cannot parse at all:

```sh
printf '(a b)\n' > errors/clean.txt
errors/report errors/list.scm errors/clean.txt
printf '(a))\n' > errors/extra.txt
errors/report errors/list.scm errors/extra.txt
```

```text
clean
errors/extra.txt:1:4: error: unexpected input [parse.syntax]
  | (a))
  |    ^ parse failed here
failed: no tree
```

The `parse.syntax` label points where the start rule stopped matching, here
the second `)`; for an error inside a nested construct that is the start of
the outermost construct that failed
([parse.syntax](../reference/diagnostics.md#parsesyntax)).

The `tinyparse` command reports these outcomes through its exit status
([tinyparse(1)](../../tinyparse.1.md), EXIT STATUS).
