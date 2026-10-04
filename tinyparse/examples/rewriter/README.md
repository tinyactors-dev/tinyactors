# rewriter — a `#define` → `enum` codemod

This example rewrites source code from its parse tree, copying the bytes it does
not touch straight from the original through their recorded spans.

It turns object-like integer macros into a single C `enum`, driven entirely by a
pocketparse concrete syntax tree.

Given input like

```c
#define MAX_USERS 100
#define BUFFER_SIZE 4096
#define TIMEOUT_MS 250
```

it emits

```c
enum {
    MAX_USERS = 100,
    BUFFER_SIZE = 4096,
    TIMEOUT_MS = 250
};
```

in place of the first macro, and passes every other line through untouched.

## How it works

The rewrite is *not* a regex over the file. It parses the source with
pocketparse and walks the tree through the public cursor API.

1. **Grammar** ([`cdefines.scm`](cdefines.scm)) — a deliberately small,
   line-oriented grammar. A `Line` is either a `Define`
   (`#define NAME INTEGER`, with `name` and `value` tagged as fields) or a
   `Raw` line (anything else, taken verbatim up to the newline). Whitespace
   is *not* declared as trivia, so every byte — newlines included — stays
   reachable through the tree; the rewriter reconstructs its output from the
   byte spans the tree records.

2. **Collect pass** ([`rewriter.c`](rewriter.c)) — the program descends to
   the `Source` node's `Line` children, and for each `Line` whose child is a
   `Define`, jumps to the `name` and `value` fields with
   `pocketparse_tree_cursor_goto_first_child_by_field` and records their byte
   spans, in source order.

3. **Emit pass** — a second walk over the same tree. `Raw` lines are copied
   straight from the source bytes their span covers; `#define` lines are
   dropped; and the single `enum` is emitted where the first macro stood.

The grammar is embedded into the binary at build time (`build.sh` turns
`cdefines.scm` into `cdefines_scm.h`), so the program needs no grammar path
at runtime.

## Limits

The grammar reads only `#define NAME INTEGER`: a decimal value, no expression,
no function-like macro, no line continuation, and no `#define` written inside a
comment or string. A macro it does not recognize stays a `Raw` line and passes
through untouched, so the codemod is conservative by construction.

## Build and run

From `tools/pocketparse` (after `bash build.sh dist`):

```sh
examples/rewriter/run.sh examples/rewriter/sample.c
```

`run.sh` builds the program on first use (or when its sources change), then
runs it on the given file and prints the rewritten source to stdout. The
build links the host dist archives — `libpocketparse.a` and `libax.a` — and
includes `pocketparse.h` from the dist directory.

You can also build directly:

```sh
cd examples/rewriter && bash build.sh && ./rewriter sample.c
```

## Files

| File             | Role                                               |
| ---------------- | -------------------------------------------------- |
| `cdefines.scm`   | the line-oriented grammar                          |
| `rewriter.c`     | the codemod: parse, walk the CST, emit the rewrite |
| `sample.c`       | example input mixing macros with ordinary C        |
| `build.sh`       | compiles the program against the host dist         |
| `run.sh`         | builds if needed, then runs the rewriter on a file |
