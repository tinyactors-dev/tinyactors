# rewriter: rewrite source from spans

`rewriter.c` turns the integer `#define` lines of a C file into one `enum`
and copies every other line through unchanged. It shows how to write a
codemod with tinyparse. Every byte of the input is in some node of the
concrete syntax tree (CST), so the program copies the byte ranges of the
nodes it keeps and writes new text for the nodes it replaces. It uses
[`tinyparse_tree_cursor_goto_first_child_by_field`](../../docs/reference/c-api.md#moving-the-cursor),
[`tinyparse_tree_cursor_start_byte`](../../docs/reference/c-api.md#tinyparse_tree_cursor_start_byte),
[`tinyparse_tree_cursor_end_byte`](../../docs/reference/c-api.md#tinyparse_tree_cursor_end_byte)
and the [`field`](../../docs/reference/grammar.md#field) form.

## Prerequisites

Run the commands from the `tinyparse/` directory after `mise run examples`,
as [Examples](../README.md) describes.

## Run it

`examples/rewriter/sample.c` mixes three macros with a struct and a function.
Rewrite it:

```sh
examples/rewriter/run.sh examples/rewriter/sample.c
```

The rewritten file goes to standard output, and the program exits with
status 0. The three `#define` lines become one `enum` at the place of the
first, and every other line is the input's:

```text
/* sample.c — input for the macro-to-enum rewriter example.
 *
 * It mixes object-like integer macros (which the rewriter folds into a
 * single enum) with ordinary C (a struct and a function) that must pass
 * through unchanged.
 */
#include <stddef.h>

enum {
    MAX_USERS = 100,
    BUFFER_SIZE = 4096,
    TIMEOUT_MS = 250
};

struct session {
    int   user_id;
    char  name[BUFFER_SIZE];
};

int session_count(const struct session *sessions, size_t n)
{
    size_t i;
    int    active = 0;
    for (i = 0; i < n && i < MAX_USERS; i++) {
        if (sessions[i].user_id != 0)
            active++;
    }
    return active;
}
```

## How it works

The grammar, `cdefines.scm`, reads the file as lines. A `Line` is a `Define`
(`#define`, a name and a decimal integer, with the name and the integer bound
to the fields `name` and `value`) or a `Raw` line (any other bytes up to the
line feed). Whitespace is matched by tokens and not declared as trivia, so
every byte, line feeds included, belongs to a node.

The program walks the `Line` children of the root twice:

1. The collect pass moves into each `Define` with
   `tinyparse_tree_cursor_goto_first_child_by_field` for `name` and then for
   `value`, and records the byte range of each.
2. The emit pass writes the bytes of each `Raw` line from its byte range,
   skips each `Define`, and writes the `enum` from the recorded ranges where
   the first `Define` was.

The text of each name and value comes from the input at the recorded byte
range; the program never scans the input itself.

The grammar is compiled into the program. `mise run examples` writes
`cdefines.scm` as a byte array into `build/examples/rewriter/cdefines_scm.h`,
and the program passes those bytes to `tinyparse_compile_scheme`, so it reads
no grammar file at run time. [Ship a grammar inside your
program](../../docs/how-to/embed-grammar.md) covers this technique.

## Limits

The grammar reads lines and knows nothing else about C.

- Only `#define NAME DIGITS` followed by optional spaces is a `Define`. A
  hexadecimal value, a function-like macro, a macro with a trailing comment
  and a line continuation are `Raw` lines and stay as they are.
- A `#define` inside a block comment is rewritten. With a `/* ... */` comment
  that holds `#define A 1`, followed by `#define E 5` outside the comment, the
  `enum` with both `A` and `E` is written inside the comment, and the program
  exits with status 0. The output declares `E` only inside the comment, where a
  compiler never sees it.
- Preprocessor conditionals are not followed. Two definitions of `SIZE` in
  the branches of an `#ifdef` produce one `enum` in the first branch that
  declares `SIZE` twice, which does not compile; the program exits with
  status 0.
- A file with no matching `#define` is written unchanged.

## Files

| File | Contents |
|------|----------|
| `cdefines.scm` | the line grammar |
| `rewriter.c` | the program |
| `sample.c` | the sample input |
| `run.sh` | runs the program built by `mise run examples` on a file |
