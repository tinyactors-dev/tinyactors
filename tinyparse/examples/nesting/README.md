# nesting: generated parsers for injected regions

`nesting.c` parses a document of fenced code blocks with a parser generated
from std.fence, whose grammar hands the contents of each `json` block to
std.json and of each `js` block to std.javascript through
[`inject`](../../docs/reference/grammar.md#inject) forms. The program
registers parsers generated from those two grammars on the engine, so all
three grammars run as generated C in one parse, and it prints one tree, the
tree `tinyparse run std.fence` prints for the same document. The example uses
[`tinyparse_register_language`](../../docs/reference/c-api.md#tinyparse_register_language),
the [generated parser
interface](../../docs/reference/c-api.md#generated-parser-interface) and the
[tree cursor](../../docs/reference/c-api.md#tree-cursor) functions.

## Prerequisites

Run the commands from the `tinyparse/` directory of a source tree, as
[Examples](../README.md) describes. A generated parser builds only from a
source tree: it includes `src/internal.h`.

## Run it

Build the example programs, then run this one on the sample document, which
holds a `json` block and a `js` block:

```sh
mise run examples
build/examples/nesting/nesting examples/nesting/sample.md
```

The tree goes to standard output in the format of `tinyparse run
--dump-tree`; each node of an injected region carries its language in
brackets. A line feed token's kind name is a line feed, so its line breaks
before the byte range. The output is 94 lines; the lines between the start of
the `json` block's tree and the end of the `js` block are cut and replaced by
`...`:

````text
Document [0-92] [0,0]-[6,0]
  Block [0-46] [0,0]-[2,3]
    JsonFence [0-46] [0,0]-[2,3]
      ```json [0-7] [0,0]-[0,7]
      
 [7-8] [0,7]-[1,0]
      injection [std.json] [8-42] [1,0]-[1,34]
        Document [std.json] [8-42] [1,0]-[1,34]
...
      
 [87-88] [4,34]-[5,0]
      ``` [88-91] [5,0]-[5,3]
  Space [91-92] [5,3]-[6,0]
````

Standard error gets the number of regions each registered parser parsed:

```text
nesting: generated parsers ran on std.json=1 std.javascript=1 regions
```

The full tree is `examples/nesting/expected.txt`.

## How it works

`mise run examples` generates three parsers with distinct prefixes, compiles
them with `-std=c11 -O2 -I include -I src`, and links them with `nesting.c`
and `build/lib/libtinyparse.a` into `build/examples/nesting/nesting`:

```sh
tinyparse c std.fence --parser-lib --name fence_host --out build/examples/nesting/fence_host.c
tinyparse c std.json --parser-lib --name json_lang --out build/examples/nesting/json_lang.c
tinyparse c std.javascript --parser-lib --name js_lang --out build/examples/nesting/js_lang.c
```

The `JsonFence` rule of std.fence wraps the block's contents in
`(inject grammar: std.json content: (field content: FenceBody))`, and
`JsFence` does the same with `std.javascript`. The generated `fence_host`
parser knows those names but calls neither embedded parser directly.

`nesting.c` opens one engine and loads the host grammar with
`fence_host_load`. For each embedded language it calls the generated loader,
`json_lang_load` or `js_lang_load`, and registers the grammar it returned
together with the matching run function under the name the `inject` form
carries, `std.json` or `std.javascript`. The registered run function is a
wrapper that counts regions and calls `json_lang_run` or `js_lang_run`. Then
it creates a parser on the host grammar, pushes the document and calls
`fence_host_run`. When the host parser reaches an `inject` form, the engine
finds the language by name and runs the registered generated parser over the
region, and the region's subtree goes under an `injection` node in the host
tree.

The bracketed language tag comes from `tp_tree_cursor_language_tag`, an
internal function the program declares itself; the public cursor functions do
not report it.

## Limits

- A region whose language has no registered parser is parsed by the VM into
  the same tree. The tree alone does not show which ran; the counts on
  standard error do.
- A region the injected grammar rejects leaves an empty root node under its
  `injection` node, adds no diagnostic, and the program exits with status 0.
  A `json` block holding `{"a": }` prints
  `Document [std.json] [8-15] [1,0]-[1,7]` with no children.
- Input that [std.fence](../../docs/reference/std-grammars.md#stdfence) does
  not accept, such as a block with the info string `python` or text outside a
  block, makes the program print `nesting: host parse failed` and exit with
  status 1.
- The engine's 16 MiB block and every block its `grow` callback returns come
  from `malloc` and are never freed.

## Files

| File | Contents |
|------|----------|
| `nesting.c` | The program. |
| `sample.md` | A document with one `json` block and one `js` block. |
| `expected.txt` | The tree the program prints for `sample.md`. |
