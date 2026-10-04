# uri: read named fields from a tree

`uri.c` parses a URL given on the command line with the grammar in
`examples/uri/uri.scm` and prints each component it finds: scheme, user
information, host, port, path, query and fragment. It shows a host program
that finds values by field name, at any depth of the concrete syntax tree
(CST), and slices their text from the input by byte range. It uses
[`tinyparse_tree_cursor_field`](../../docs/reference/c-api.md#tinyparse_tree_cursor_field),
[`tinyparse_tree_symbol_name`](../../docs/reference/c-api.md#tinyparse_tree_symbol_name),
the other [tree cursor](../../docs/reference/c-api.md#tree-cursor) functions
and the [`field`](../../docs/reference/grammar.md#field) form. The
[ebnf](../ebnf/README.md) example writes `uri.scm` from an EBNF grammar.

## Prerequisites

Run the commands from the `tinyparse/` directory after `mise run examples`,
as [Examples](../README.md) describes.

## Run it

Parse a URL that has every component:

```sh
examples/uri/run.sh 'https://user@example.com:8080/a/b?x=1#frag'
```

The program prints one `name=value` line per component and exits with
status 0:

```text
scheme=https
userinfo=user
host=example.com
port=8080
path=/a/b
query=x=1
fragment=frag
```

A component the URL leaves out is not printed:

```sh
examples/uri/run.sh 'http://example.com/a%20b'
```

```text
scheme=http
host=example.com
path=/a%20b
```

Input the grammar rejects goes to standard error with each diagnostic
message, and the program exits with status 1:

```sh
examples/uri/run.sh ':// not a uri'
```

```text
uri: error: malformed URI ':// not a uri'
  error: unexpected input
```

## How it works

The grammar has two rules. `Uri` binds `scheme`, `path`, `query` and
`fragment` as fields and refers to `Authority`, which binds `userinfo`,
`host` and `port`:

```scheme
(rule Authority
  (seq (optional (seq (field userinfo: USERINFO) "@")) (field host: HOST) (optional (seq ":" (field port: PORT)))))
```

For each component, `print_field` resets the cursor to the root and
`find_field` walks the tree in pre-order until it reaches a node whose field
name, from `tinyparse_tree_cursor_field` and `tinyparse_tree_symbol_name`,
matches. Because the walk descends into every child, it finds `host` inside
the `Authority` node the same way it finds `scheme` at the top. The value is
the input from the node's start byte to its end byte.

The program reads the grammar file at run time. It uses the path in the
environment variable `URI_GRAMMAR` when that is set, and `uri.scm` in the
directory of the executable otherwise. `run.sh` sets `URI_GRAMMAR` to
`examples/uri/uri.scm`, and runs `examples/ebnf/run.sh` first when that file
is missing. To run the program directly:

```sh
URI_GRAMMAR=examples/uri/uri.scm build/examples/uri/uri 'http://example.com'
```

## Limits

- The grammar accepts only `scheme://authority` followed by an optional path,
  query and fragment. A URL without `//`, such as `mailto:a@b.c`, and a
  relative reference, such as `/relative/path`, are rejected with exit status
  1.
- An IPv6 host literal such as `http://[::1]:80/` is rejected with exit
  status 1.
- Percent-escapes are matched as ordinary characters and printed undecoded:
  `/a%20b` stays `/a%20b`.
- A value longer than 2,047 bytes is cut to its first 2,047 bytes.
- Without `URI_GRAMMAR`, the program looks for `uri.scm` beside the
  executable, where `mise run examples` does not put one, and exits with
  status 1 after `uri: error: cannot read grammar
  'build/examples/uri/uri.scm'`.

## Files

| File | Contents |
|------|----------|
| `uri.c` | the program |
| `uri.scm` | the grammar, written by `examples/ebnf/run.sh` from `examples/ebnf/uri.ebnf` |
| `run.sh` | runs the program built by `mise run examples` with `URI_GRAMMAR` set |
