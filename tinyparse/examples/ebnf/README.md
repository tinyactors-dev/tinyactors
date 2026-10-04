# ebnf: convert EBNF to a tinyparse grammar

`ebnf2scm.c` reads a grammar written in a subset of EBNF, parses it with a
tinyparse grammar compiled from text embedded in the program, and walks the
concrete syntax tree (CST) to print an equivalent tinyparse grammar. It shows
a host program that reads a tree and writes text: it dispatches on node kinds
and recurses as the EBNF groups nest. It uses
[`tinyparse_compile_scheme`](../../docs/reference/c-api.md#tinyparse_compile_scheme),
the [tree cursor](../../docs/reference/c-api.md#tree-cursor) functions, and
the grammar forms it prints, described in the
[grammar language reference](../../docs/reference/grammar.md). Its output is
the grammar the [uri](../uri/README.md) example reads.

## Prerequisites

Run the commands from the `tinyparse/` directory after `mise run examples`,
as [Examples](../README.md) describes.

## Run it

Convert the URI grammar in `examples/ebnf/uri.ebnf`:

```sh
examples/ebnf/run.sh examples/ebnf/uri.ebnf
```

The script prints the converted grammar and exits with status 0:

```text
(tinyparse version: "0.1")

(module examples.uri)

(grammar uri
  start: Uri

  (token SCHEME (regex "[A-Za-z][A-Za-z0-9+.-]*"))
  (token USERINFO (regex "[A-Za-z0-9._~%!$&'()*+,;=:-]+"))
  (token HOST (regex "[A-Za-z0-9._~%!$&'()*+,;=-]+"))
  (token PORT (regex "[0-9]+"))
  (token SEGMENTS (regex "/[A-Za-z0-9._~%!$&'()*+,;=:@/-]*"))
  (token QUERY (regex "[A-Za-z0-9._~%!$&'()*+,;=:@/?-]*"))
  (token FRAGMENT (regex "[A-Za-z0-9._~%!$&'()*+,;=:@/?-]*"))

  (rule Uri
    (seq (field scheme: SCHEME) "://" Authority (optional (field path: SEGMENTS)) (optional (seq "?" (field query: QUERY))) (optional (seq "#" (field fragment: FRAGMENT)))))
  (rule Authority
    (seq (optional (seq (field userinfo: USERINFO) "@")) (field host: HOST) (optional (seq ":" (field port: PORT)))))
)
```

`run.sh` also saves the output as `examples/uri/uri.scm` whenever it differs
from that file, whatever EBNF file you convert. Running it on another file
replaces the grammar the `uri` example reads; `git checkout
examples/uri/uri.scm` restores it. To convert a file without touching
`uri.scm`, run the program directly:

```sh
build/examples/ebnf/ebnf2scm examples/ebnf/uri.ebnf
```

## How it works

The program accepts this subset of EBNF:

```text
rule        = name "=" alternation ";"
alternation = sequence ("|" sequence)*
sequence    = factor+
factor      = name ":" factor          (* a field *)
            | "[" alternation "]"      (* optional *)
            | "{" alternation "}"      (* zero or more *)
            | "(" alternation ")"      (* grouping *)
            | name                     (* a reference *)
            | "..."                    (* a literal *)
            | /.../                    (* a regular expression *)
```

Whitespace and `(* ... *)` comments may appear between any two tokens. The
EBNF grammar that reads it is the string `EBNF_GRAMMAR` in `ebnf2scm.c`: one
rule per construct (`Rule`, `Alternation`, `Sequence`, `Factor`,
`FieldItem`, `Optional`, `Repetition`, `Group`), with `Space` and `Comment`
as trivia.

The walk has one function per node kind. `emit_alternation` prints a
`choice` when a rule has more than one option, `emit_sequence` prints a
`seq` when an option has more than one factor, and `emit_factor` prints one
factor. For `[...]`, `{...}` and `(...)`, `emit_factor` calls
`emit_alternation` on the group's contents, so the output nests as deeply as
the input. Trivia nodes sit between the structural nodes, so each function
finds its children by kind name with `tinyparse_tree_symbol_name` and skips
the rest.

Names follow one convention: an all-uppercase name is a token, and any other
name is a rule, printed with its first letter capitalized (`authority`
becomes `Authority`). A rule whose first factor is a literal or a regular
expression is printed as a `token` (see Limits). The first rule that is not a token becomes the
`start:` rule, and the program prints every token before every rule.

## Limits

- The module and grammar names are fixed: every output starts with
  `(module examples.uri)` and `(grammar uri`, whatever the input describes.
- The EBNF exception operator `-` is not in the subset. `word = letter - "x" ;`
  prints `ebnf2scm: the EBNF input did not parse` and
  `error: unexpected input`, and exits with status 1.
- A rule is printed as a token when the first factor of its first option is
  a literal or a regular expression, even when more factors follow. With
  `item = DIGIT | list ;` and `DIGIT = /[0-9]/ ;`, the rule
  `list = "(" [ item { "," item } ] ")" ;` becomes
  `(token List (seq "(" (optional (seq Item (repeat (seq "," Item)))) ")"))`,
  a token that refers to a rule. The converter exits with status 0,
  `tinyparse check` accepts the result, and parsing `(1,(2))` with it fails
  with `parse.syntax`. Starting such a rule with a reference to a one-literal
  token (`open = "(" ;`) avoids it.
- Left recursion is not detected. `expr = expr "+" NUM | NUM ;` converts with
  status 0 and passes `tinyparse check`, and parsing `1+2` with the result
  recurses until memory runs out. Under `ulimit -v 2000000` it printed
  `error: out of memory` and exited with status 1 after one second.

## Files

| File | Contents |
|------|----------|
| `ebnf2scm.c` | the converter, with the EBNF grammar embedded as text |
| `uri.ebnf` | an EBNF grammar for URIs of the form `scheme://authority/path?query#fragment` |
| `run.sh` | runs the converter built by `mise run examples`, prints the result and saves it as `examples/uri/uri.scm` |
