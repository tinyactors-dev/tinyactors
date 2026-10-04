# Split, extend and combine grammars

This guide is for a grammar author who wants to share tokens and rules between
files, build a dialect on an existing grammar, or parse a region of the input
with a second grammar. The sections before the last each end with a grammar
you can run with `tinyparse run`; the last shows how a C host loads it.

## Prerequisites

- A source checkout with the shell in its `tinyparse/` directory, built and on
  your `PATH`:

  ```sh
  mise run build
  export PATH="$PWD/build/bin:$PATH"
  ```

  For a release, see [Install tinyparse](install.md).
- A grammar you can already run, as in
  [Write your first grammar](../tutorials/first-grammar.md).
- A scratch directory:

  ```sh
  WORK="$(mktemp -d)"
  ```

## Split a grammar across files

Put the shared definitions in their own file and name that file with
[`import`](../reference/grammar.md#import) at the top level of the grammar that
uses them, before its `grammar` form. Inside the `grammar` form, `import` is
ignored, so the names it would bring in are undefined and the compile fails
with `error: rule references an undefined name`.
[`examples/import`](../../examples/import/README.md) does this: `main.scm`
declares `(import words)` before its `grammar` form and uses the `Word` token
and the `Space` trivia that `words.scm` defines.

```sh
printf 'a b c' | tinyparse run examples/import/main.scm --input - --dump-tree
```

Every node below `Source` comes from a definition in `words.scm`:

```text
Source [0-5] [0,0]-[0,5]
  Word [0-1] [0,0]-[0,1]
  Space [1-2] [0,1]-[0,2]
  Word [2-3] [0,2]-[0,3]
  Space [3-4] [0,3]-[0,4]
  Word [4-5] [0,4]-[0,5]
```

`(import lib.words)` reads `lib/words.scm`, relative to the directory of the
grammar named on the command line. The other resolution rules, including the
fallback to the standard grammars, are under
[GRAMMAR ARGUMENTS in tinyparse(1)](../../tinyparse.1.md).

The importing grammar and the files it imports share one namespace. When both
define a name, the imported definition is used and no fault is raised; wrap
the local definition in `(override …)` to use it instead.

## Derive a dialect from a base grammar

[`extends`](../reference/grammar.md#extends-and-override) builds a grammar on a
base grammar, and `override` replaces one of the base's rules or tokens. A
[`when`](../reference/grammar.md#when) form keeps part of a rule only when a
[parameter](../reference/grammar.md#parameters) is set, so one grammar file can
describe several dialects.

This dialect of std.json accepts hexadecimal numbers when the parameter `hex`
is set. Save it as `$WORK/config/grammar.scm`:

```scheme
(extends std.json)

(grammar config
  start: Document

  (token Hex (seq "0x" (regex "[0-9A-Fa-f]+")))

  (override (rule Value
    (choice Object Array String (when hex: Hex) Number True False Null))))
```

- `extends` takes the base's tokens, rules, extras and lexical goals. The base's `start:` is
  not inherited, so the dialect names its own.
- `Value` exists in std.json, so redefining it needs `override`. Without it,
  the compile fails with `error: redefining a base name requires (override ...)`.
- `Hex` comes before `Number` in the ordered choice, because `Number` also
  matches the `0` of `0xFF`.

Write an input and parse it without the parameter:

```sh
printf '{"mask": 0xFF}\n' > "$WORK/mask.json"
tinyparse run "$WORK/config/grammar.scm" --input "$WORK/mask.json" --dump-tree
```

The `when` form is left out of the choice, so the grammar is std.json, and the
parse fails with exit status 1:

```text
error: unexpected input [parse.syntax]
```

Set the parameter with `--param`:

```sh
tinyparse run "$WORK/config/grammar.scm" --input "$WORK/mask.json" --dump-tree --param hex
```

```text
Document [0-15] [0,0]-[1,0]
  Value [0-14] [0,0]-[0,14]
    Object [0-14] [0,0]-[0,14]
      open: { [0-1] [0,0]-[0,1]
      Member [1-13] [0,1]-[0,13]
        key: String [1-7] [0,1]-[0,7]
        : [7-8] [0,7]-[0,8]
        Space [8-9] [0,8]-[0,9]
        value: Value [9-13] [0,9]-[0,13]
          Hex [9-13] [0,9]-[0,13]
      close: } [13-14] [0,13]-[0,14]
  Space [14-15] [0,14]-[1,0]
```

std.jsonc, std.json5, std.typescript and std.tsx are built the same way; their
sources are under `grammars/std/` and their parameters are listed in
[Standard grammars](../reference/std-grammars.md).

## Set parameters in a package manifest

A grammar package is a directory with a `grammar.scm` and a
[`package.scm`](../reference/grammar.md#packagescm-manifest) manifest whose
`params:` the command sets before compiling. Add a manifest beside the
dialect:

```sh
cat > "$WORK/config/package.scm" <<'EOF'
(package
  name: config
  version: "0.1.0"
  params: (hex: 1))
EOF
```

Name the directory as the grammar:

```sh
tinyparse run "$WORK/config" --input "$WORK/mask.json" --dump-tree
```

The tree is the one above, with no `--param`. A `--param` on the command line
overrides the manifest: with `--param hex=0` the parse fails as it did without
the parameter. A grammar reached through `import`, `extends` or `inject` is compiled
without its own manifest; see
[GRAMMAR ARGUMENTS in tinyparse(1)](../../tinyparse.1.md).

## Parse an embedded language

[`inject`](../reference/grammar.md#inject) parses the bytes a pattern matched
with another grammar and puts that grammar's tree in the host grammar's tree.
This grammar reads `name = value` lines and parses each value as JSON. Save it
as `$WORK/settings.scm`:

```scheme
(grammar settings
  start: Source

  (token Space (regex "[ \t\r\n]+"))
  (token Name  (regex "[a-z]+"))
  (token Line  (regex "[^\n]+"))

  (extras (trivia Space))

  (rule Source (seq (repeat Setting) EOF))

  (rule Setting
    (seq (field name: Name) "=" (inject grammar: std.json content: Line))))
```

`grammar:` names the injected grammar and is resolved like an `import`.
`content:` is the pattern whose bytes it parses; its own nodes are replaced by
the injected tree.

```sh
printf 'retries = 3\nhosts = ["a", "b"]\n' > "$WORK/app.settings"
tinyparse run "$WORK/settings.scm" --input "$WORK/app.settings" --dump-tree
```

Each value is an `injection` node holding a std.json `Document`, and every node
std.json built carries the tag `[std.json]`:

```text
Source [0-31] [0,0]-[2,0]
  Setting [0-11] [0,0]-[0,11]
    name: Name [0-7] [0,0]-[0,7]
    Space [7-8] [0,7]-[0,8]
    = [8-9] [0,8]-[0,9]
    Space [9-10] [0,9]-[0,10]
    injection [std.json] [10-11] [0,10]-[0,11]
      Document [std.json] [10-11] [0,10]-[0,11]
        Value [std.json] [10-11] [0,10]-[0,11]
          Number [std.json] [10-11] [0,10]-[0,11]
  Space [11-12] [0,11]-[1,0]
  Setting [12-30] [1,0]-[1,18]
    name: Name [12-17] [1,0]-[1,5]
    Space [17-18] [1,5]-[1,6]
    = [18-19] [1,6]-[1,7]
    Space [19-20] [1,7]-[1,8]
    injection [std.json] [20-30] [1,8]-[1,18]
      Document [std.json] [20-30] [1,8]-[1,18]
        Value [std.json] [20-30] [1,8]-[1,18]
          Array [std.json] [20-30] [1,8]-[1,18]
            open: [ [std.json] [20-21] [1,8]-[1,9]
            Value [std.json] [21-24] [1,9]-[1,12]
              String [std.json] [21-24] [1,9]-[1,12]
            , [std.json] [24-25] [1,12]-[1,13]
            Space [std.json] [25-26] [1,13]-[1,14]
            Value [std.json] [26-29] [1,14]-[1,17]
              String [std.json] [26-29] [1,14]-[1,17]
            close: ] [std.json] [29-30] [1,17]-[1,18]
  Space [30-31] [1,18]-[2,0]
```

A value std.json rejects raises no diagnostic, and the run exits with status 0.
The injected root still spans the whole region, so a host compares it with its
children instead: the children of the injected `Document` cover the
`injection` node's whole byte range only when std.json accepted the region. For
`retries = [1,` the `Document` at `[10-13]` has no children. For `retries = 3 4`
it holds `Value [10-11]` and `Space [11-12]`, and byte 12 is covered by no
child. std.fence
(`grammars/std/fence/grammar.scm`) injects std.javascript and std.json into
Markdown code fences with the same form.

## Use the composed grammar from a C host

The library reads no files and sets no parameters, so a C host gets the
composed grammar in one of two ways.

Pack it with the command. The packed grammar holds the imported, base and
injected grammars and the parameter values it was packed with, and
[`tinyparse_load_binary_grammar`](../reference/c-api.md#tinyparse_load_binary_grammar)
loads it with no module resolver:

```sh
tinyparse pack "$WORK/config" --out "$WORK/config.tpg"
tinyparse pack "$WORK/settings.scm" --out "$WORK/settings.tpg"
```

The parameters are fixed at that point: `tinyparse run "$WORK/config.tpg"`
parses `0xFF` even with `--param hex=0`.

Or compile the `.scm` files in the host. `tinyparse_compile_scheme` fails
with `extends requires a module resolver` or
`inject requires a module resolver to find the embedded grammar` until the host
installs a resolver with `tinyparse_set_module_resolver`. Set each parameter
with `tinyparse_set_param` before compiling.
[Supply imports, classes and parameters from a C host](host-services.md) shows
both.
