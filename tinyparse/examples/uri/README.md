# URI parser

This example pulls named components out of a parse by reading the tree's
fields, never re-scanning the text.

It is also the second stage of the pipeline example: the generated parser,
used by another program. It loads `uri.scm` (produced by the EBNF converter in
`../ebnf`) at runtime, parses the URL given on the command line, walks the
concrete syntax tree through the public cursor API, and prints the RFC 3986
components. A recursive pre-order walk finds each field by name, so it reaches
the `userinfo`, `host`, and `port` fields nested inside the `Authority`
sub-rule.

## Files

- `uri.scm` — the generated pocketparse grammar. A copy is committed so this
  example runs standalone; `../ebnf/run.sh` regenerates it.
- `uri.c` — the parser driver. Hosted (libc), linked against the dist archives.
- `build.sh` — compiles `uri` against `$PP_DIST`.
- `run.sh` — builds if needed (regenerating `uri.scm` if absent) and runs.

## Use

```sh
# from tinyparse/
examples/uri/run.sh 'https://user@example.com:8080/a/b?x=1#frag'
```

Output:

```
scheme=https
userinfo=user
host=example.com
port=8080
path=/a/b
query=x=1
fragment=frag
```

A component that the URL omits is left out. A malformed URI prints a diagnostic
to stderr and exits 1:

```sh
examples/uri/run.sh ':// not a uri'   # -> exit 1
```

The grammar covers the common hierarchical form (`scheme://authority/path`)
rather than all of RFC 3986: there is no IPv6 host literal, no relative
reference, and percent-escapes are matched as ordinary characters, not
decoded.

## The pipeline

```
uri.ebnf --(ebnf2scm, parses EBNF with pocketparse)--> uri.scm
uri.scm  --(uri.c, parses the URL with pocketparse)--> components
```
