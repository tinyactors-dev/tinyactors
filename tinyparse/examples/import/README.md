# import: split a grammar across files

`main.scm` uses a `Word` token and whitespace trivia that it does not define:
`(import words)` brings them in from `words.scm`. The example uses the
[`import`](../../docs/reference/grammar.md#import) form and the module
resolution that the `tinyparse` command performs, described under
[Composition](../../docs/reference/grammar.md#composition).

## Prerequisites

Run the commands from the `tinyparse/` directory with `tinyparse` on your
`PATH`, as [Examples](../README.md) describes.

## Run it

Parse three words:

```sh
printf 'a b c' | tinyparse run examples/import/main.scm --input - --dump-tree
```

The `Word` nodes and the `Space` trivia come from definitions in `words.scm`:

```text
Source [0-5] [0,0]-[0,5]
  Word [0-1] [0,0]-[0,1]
  Space [1-2] [0,1]-[0,2]
  Word [2-3] [0,2]-[0,3]
  Space [3-4] [0,3]-[0,4]
  Word [4-5] [0,4]-[0,5]
```

Parse the committed sample:

```sh
tinyparse run examples/import/main.scm --input examples/import/fixtures/sample.words --dump-tree
```

The output equals `fixtures/expected.tree`:

```text
Source [0-17] [0,0]-[0,17]
  Word [0-5] [0,0]-[0,5]
  Space [5-6] [0,5]-[0,6]
  Word [6-11] [0,6]-[0,11]
  Space [11-12] [0,11]-[0,12]
  Word [12-17] [0,12]-[0,17]
```

## How it works

`main.scm` imports at the top level, outside its `grammar` form, and names
`Word` in its only rule. The file without its comment header:

```scheme
(tinyparse version: "0.1")

(module examples.import.main)

(import words)

(grammar main
  start: Source

  (rule Source
    (seq (repeat Word) EOF)))
```

`words.scm` defines the token, the trivia and no `start:`. The file without its
comment header:

```scheme
(tinyparse version: "0.1")

(module examples.import.words)

(grammar words
  (token Word  (regex "[A-Za-z]+"))
  (token Space (regex "[ \t\r\n]+"))

  (extras
    (trivia Space)))
```

- The library reads no files. While it compiles `main.scm` it asks the module
  resolver for the text of `words`.
- The `tinyparse` command's resolver turns the name into a path in the
  directory of the grammar named on the command line: each `.` becomes `/` and
  `.scm` is appended, so `words` is `examples/import/words.scm` and `a.b` would
  be `examples/import/a/b.scm`. Imports inside imported files resolve against
  the same directory.
- When no such file exists, the resolver looks the name up among the
  [standard grammars](../../docs/reference/std-grammars.md) built into the
  command, so `(import std.json)` works with no file present.
- The imported tokens, rules and extras merge into the grammar before
  `main.scm`'s own rules are checked. A grammar packed with `tinyparse pack` or
  generated with `tinyparse c` contains them and needs no `words.scm`.
- A C program supplies its own resolver; see [Supply imports, classes and
  parameters from a C host](../../docs/how-to/host-services.md).
  [Split, extend and combine grammars](../../docs/how-to/compose-grammars.md)
  covers `extends` and larger layouts.

## Limits

- `import` takes effect only at the top level. Inside the `grammar` form it is
  ignored without a message, and the compile then fails with
  `error: rule references an undefined name` for the first imported name a rule
  uses.
- A module that the resolver cannot find fails with
  `error: module resolver could not load an imported module`, which does not
  name the module. The command exits with status 1.
- Two files that import each other, or a file that imports itself, fail with
  `error: import cycle detected` and exit status 1.
- Imported names share one namespace with the importing grammar. A grammar
  that defines a token of the same name as an imported one compiles without a
  message, and the imported definition is the one used.

## Files

| File | Contents |
|------|----------|
| `main.scm` | the importing grammar |
| `words.scm` | the imported module |
| `fixtures/sample.words` | an input of three words |
| `fixtures/expected.tree` | the tree for `sample.words`; `mise run gate:import-c` checks that a generated parser and `tinyparse run` both print it |
