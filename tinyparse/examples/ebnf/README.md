# EBNF to pocketparse grammar

This example parses a grammar language with pocketparse, then walks the tree to
emit another grammar.

It is the first stage of the pipeline example. An EBNF meta-grammar, embedded in
`ebnf2scm.c` as `.scm` text, is compiled at runtime; the input EBNF is parsed
into a concrete syntax tree; and the tree is walked through the public cursor
API to print an equivalent pocketparse `.scm` grammar. The EBNF nests through
`[...]`, `{...}`, and `(...)` groups, so the walk recurses: each group is its
own rule, and `emit_factor` calls back into `emit_alternation` for the
alternation it holds.

## Files

- `uri.ebnf` — an EBNF grammar for the RFC 3986 URI: scheme, authority (with
  optional userinfo, host, optional port), path, optional query, optional
  fragment.
- `ebnf2scm.c` — the converter. Hosted (libc), linked against the pocketparse
  dist archives.
- `build.sh` — compiles `ebnf2scm` against `$PP_DIST`.
- `run.sh` — runs the converter, prints the `.scm`, and saves a copy to
  `../uri/uri.scm` so the URI example consumes a freshly generated grammar.

## The EBNF subset

```
rule    = name "=" alternation ";"
alt     = sequence ("|" sequence)*
seq     = factor+
factor  = name ":" factor      (* a named field *)
        | "[" alt "]"          (* optional      *)
        | "{" alt "}"          (* repetition    *)
        | "(" alt ")"          (* grouping      *)
        | name                 (* a reference   *)
        | "..."                (* a terminal    *)
        | /.../                (* a regex       *)
```

An all-uppercase name is a token; any other name is a rule. A rule whose body is
a single terminal or regex is emitted as a `(token ...)`. The first rule becomes
the grammar's start rule.

## Use

```sh
# from tinyparse/
examples/ebnf/run.sh examples/ebnf/uri.ebnf
```

The `uri.ebnf` rule

```
authority =
  [ userinfo: USERINFO "@" ]
  host: HOST
  [ ":" port: PORT ] ;
```

becomes

```
  (rule Authority
    (seq (optional (seq (field userinfo: USERINFO) "@")) (field host: HOST) (optional (seq ":" (field port: PORT)))))
```

`run.sh` also writes the full grammar to `../uri/uri.scm`, where the URI example
picks it up.

## Limits

The converter accepts the subset above and nothing more: no character-class or
exception syntax (`-`), no inline comments inside a factor, no left-recursion
checking. It trusts the EBNF to name tokens in all caps and rules otherwise,
and emits a rule whose body is a single terminal or regex as a `(token ...)`.
