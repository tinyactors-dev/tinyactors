# Embedding one parser inside another

This example links **two separately compiled parsers into one program** and runs
both: a host grammar finds fenced code blocks in a Markdown-ish document, and the
bytes inside each fence are parsed by a second, independently generated parser.

That is the lesson — *two grammars compiled to C coexist in one binary* — and no
other example teaches it. It is the embedding use case: a host markup format that
carries code in another language, where the code is a different parser's job.

## Not the same as injection

The standard grammar `std.fence` also parses fenced code blocks of mixed
languages, but it does so inside **one** grammar with the runtime
`(inject grammar: NAME content: REGION)` form, yielding a single composite tree.
This example does the opposite: there is no `inject`. The host grammar
(`host.scm`) treats a fence's body as **opaque bytes** and only delimits it; the
*host program* then runs a wholly separate parser on those bytes. Two grammars,
two generated parsers, two trees, one binary.

## How two parsers avoid colliding

`pocketparse c <grammar> --parser-lib` emits a parser as a C translation unit
with a fixed set of file-scope symbols — `pp_parser_p<N>`, `pp_compiled_parser_load`,
`pp_compiled_parser_run`, and the emitted statics. Link two such units into one
program and those symbols collide.

The `--name <prefix>` option (W16-3) renames **every** emitted file-scope symbol
to `<prefix>_…`. With no `--name`, the parser keeps its historical default names,
so existing callers are unchanged. `build.sh` generates the two parsers with
distinct prefixes:

```sh
pocketparse c host.scm  --parser-lib --name host_parser --out host_parser.c
pocketparse c std.json  --parser-lib --name json_parser --out json_parser.c
```

so their entries become `host_parser_load` / `host_parser_run` and
`json_parser_load` / `json_parser_run`, and the two units link cleanly. The
shared runtime (`libpocketparse` + `libax`, which the leaf helpers, the CALL
prologue, and the tree builder live in) is one archive set, linked once, so it is
shared — not duplicated — between the two parsers.

## How the host program embeds

`embed.c` parses the whole document with the host parser, then walks the host
tree through the cursor API. For every node bound to the `code` field it reads
the node's byte span (`start_byte`/`end_byte`) straight off the tree — it never
re-scans the document — and hands exactly those bytes to the json parser. The CST
is the data structure: the host tree locates the spans, and the json parser turns
each span into its own tree.

## Files

- `host.scm` — the host fenced-block grammar; exposes `lang` and `code` fields.
- `embed.c` — the driver. Hosted (libc), linked against the dist archives and
  the two generated parser units.
- `sample.md` — a sample document with two json fences.
- `expected.txt` — the golden output (host tree + both embedded json trees).
- `build.sh` — generates both parsers (distinct `--name`) and links the program.

The generated `host_parser.c` / `json_parser.c` and the `embed` binary are build
artifacts (gitignored); `build.sh` regenerates them.

## Use

```sh
# from tools/pocketparse
bash examples/embed/build.sh
examples/embed/embed examples/embed/sample.md
```

Output (abridged): the host tree, then the json tree of each fence's contents,
each indented under the fence it came from.

```
=== host tree (host.scm) ===
Document
  Fence
    ```
    lang: Info
    code: Body
    ```
  ...
=== embedded parses (std.json) ===
--- embedded json parse of code span [8,24) ---
    Document
      Value
        Object
          ...
```

`ci/pocketparse-multiparser.sh` builds this example and asserts the output equals
`expected.txt`, and that the linked binary carries both parsers' distinct entries.

## Hiding ax in the embeddable artifact (W16-4)

The generated parser uses the ax actor engine internally, but a host that links the
parser into its own application should not inherit ax: the embeddable object or
library must export ONLY the parser's own entries (`<prefix>_load` / `<prefix>_run`)
plus the public `pocketparse_*` API the host calls — no `ax_*`, and no
pocketparse-internal `pp_*`.

`hide-ax.sh` is the recipe. It compiles the pocketparse **amalgamation**
(`dist/amalgamation/pocketparse.c`, which folds ax in under hidden visibility) plus
each generated parser-lib unit, and links behind a linker version script generated
from the `--name` prefixes that exports exactly the parser entries and
`pocketparse_*` and localizes everything else:

```sh
# from tools/pocketparse
bash examples/embed/hide-ax.sh libembed.so host_parser:host.scm json_parser:std.json
nm -D --defined-only libembed.so | grep -c ' ax_'   # -> 0
nm -D --defined-only libembed.so | grep -cE ' pp_'  # -> 0
nm -D --defined-only libembed.so | grep -E ' (host_parser|json_parser)_'
#   T host_parser_load / _run, T json_parser_load / _run
```

The emitter is unchanged — this is a link-time recipe — so the generated C is
byte-for-byte what `--name` already produces. `ci/pocketparse-hide-ax.sh` is the
gate: it builds the artifact (single parser and the two-parser case), asserts with
`nm` that no `ax_*` (or internal `pp_*`) escapes while the parser API survives, and
checks that a hardened executable still parses the sample to the golden tree.

## Scope

The host grammar recognizes ```` ``` ````-delimited fences whose info string is a
single identifier; the body runs up to the closing fence. The embedded language
here is `std.json`, but any generated parser works the same way — the embedding is
the host program reading a span and running a second parser on it, not anything
specific to JSON.
