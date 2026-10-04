# embed: link two generated parsers into one program

`embed.c` parses a Markdown-like document with a parser generated from
`host.scm`, which finds fenced code blocks, and parses the bytes inside each
block with a second parser generated from std.json. Both generated parsers
live in one program because each was generated with its own symbol prefix.
The example uses `tinyparse c --parser-lib --name` from the
[manual](../../tinyparse.1.md#c), the [generated parser
interface](../../docs/reference/c-api.md#generated-parser-interface) and the
[tree cursor](../../docs/reference/c-api.md#tree-cursor) functions.

## Prerequisites

Run the commands from the `tinyparse/` directory of a source tree, as
[Examples](../README.md) describes. A generated parser builds only from a
source tree: it includes `src/internal.h`.

## Run it

Build the example programs, then run this one on the sample document, which
holds two `json` fences:

```sh
mise run examples
build/examples/embed/embed examples/embed/sample.md
```

The program prints the host tree, then the tree of each fence's contents,
indented under the fence. A line feed token's kind name is a line feed, so each
one prints as an indented empty line followed by a blank line. The second
fence's tree is cut and replaced by `...`:

````text
=== host tree (host.scm) ===
Document
  Fence
    ```
    lang: Info
    

    code: Body
    

    ```
  Space
  Fence
    ```
    lang: Info
    

    code: Body
    

    ```
  Space
=== embedded parses (std.json) ===
--- embedded json parse of code span [8,24) ---
    Document
      Value
        Object
          open: {
          Member
            key: String
            :
            Space
            value: Value
              Array
                open: [
                Value
                  Number
                ,
                Space
                Value
                  Number
                ,
                Space
                Value
                  Number
                close: ]
          close: }
--- embedded json parse of code span [37,49) ---
...
````

The full output is `examples/embed/expected.txt`.

## How it works

`mise run examples` generates the two parsers with different prefixes,
compiles them with `-std=c11 -O2 -I include -I src`, and links them with
`embed.c` and `build/lib/libtinyparse.a` into `build/examples/embed/embed`:

```sh
tinyparse c examples/embed/host.scm --parser-lib --name host_parser --out build/examples/embed/host_parser.c
tinyparse c std.json --parser-lib --name json_parser --out build/examples/embed/json_parser.c
```

Each file defines its entries under its prefix: `host_parser_load`,
`host_parser_run`, `json_parser_load` and `json_parser_run`, plus
`host_parser_run_chunk`, because `host.scm`'s start rule is
`(seq (repeat1 Fence) EOF)`. Without `--name` the prefixes would be the grammar
names, `host` and `json`, which differ too; two parsers collide only when their
prefixes are equal. `embed.c` declares the four entries it calls.

`host.scm` captures each fence's info string in the field `lang` and its
contents in the field `code`. Its `Body` rule matches any byte up to a line
feed followed by three backticks, so the host grammar delimits the contents
without parsing them.

`embed.c` loads the host grammar with `host_parser_load`, pushes the document
and runs `host_parser_run` in place of `tinyparse_finish`. It then walks the
tree with a cursor, and for each node in the `code` field it takes the node's
byte range and parses those bytes with `json_parser_load` and
`json_parser_run`, on a new engine. It never scans the document text itself.

A grammar can also parse a region with another grammar during one parse,
through the [`inject`](../../docs/reference/grammar.md#inject) form, which
builds a single tree. The [nesting](../nesting/README.md) example does that with generated
parsers; here the host program runs the second parser and gets a second tree.

## Limits

- Every fence is parsed as JSON, whatever its info string. A fence such as
  ```` ```python ```` prints `(json parse failed)` under its code span, and the
  program exits with status 1.
- `host.scm` accepts only fences whose info string is letters, digits and
  `_`, with a line of contents between the opening and the closing line, and
  only whitespace between fences. Text outside a fence, a fence with no info
  string, or a closing line right after the opening line makes the program
  print `embed: host parse failed` and exit with status 1.
- The host parse and each fence open their own engine over an 8 MiB `malloc`
  block with a `malloc` grow callback; the program frees neither the blocks nor
  what `grow` returns.

## Files

| File | Contents |
|------|----------|
| `host.scm` | The host grammar: fenced blocks with `lang` and `code` fields. |
| `embed.c` | The program. |
| `sample.md` | A document with two `json` fences. |
| `expected.txt` | The program's output for `sample.md`. |
| `embed-lib.sh` | Builds a shared library that exports only generated parsers' entries and the public API; [Ship a generated C parser](../../docs/how-to/generated-parser.md) shows how to use it. |
