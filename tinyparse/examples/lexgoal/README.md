# lexgoal — parser-directed lexical goals

Some languages cannot be tokenized without knowing where the parser is. The
canonical case is JavaScript's `/`: at the start of an expression it opens a
**regular-expression literal** (`/ab/`), but after a value it is the **division
operator** (`c / d`). The same byte is two different tokens, and no ordering of
grammar rules settles which — the *scanner* has to ask the parser.

`tinyparse` answers with **lexical goals**. A goal is a named lexing mode; a
rule selects one while it matches, and the scanner consults the active goal when
it meets an ambiguous lexeme.

## The mechanism

Two grammar forms, both used in [`mini-js.scm`](mini-js.scm):

```scheme
(lex-goal expr slash: RegexLiteral)   ; in goal `expr`, scan `slash` as RegexLiteral
(lex-goal op   slash: Div)            ; in goal `op`,   scan `slash` as Div
```

`(lex-goal GOAL trigger: Target …)` binds, *within* `GOAL`, a trigger token to a
target token. While `GOAL` is active, a terminal reference to `trigger` is
scanned with `Target`'s pattern and the leaf is emitted under `Target`'s kind.

```scheme
(rule Primary
  (goal expr (choice slash Ident)))   ; first value: a leading `/` is a regex

(rule BinOp
  (seq (goal op slash) Ident))        ; a trailing `/ value`: `/` divides
```

`(goal NAME X)` makes `NAME` the active lexical goal while matching `X`, then
restores the previous goal. Goals nest; a failed match leaks none. `(goal …)`
may appear anywhere a pattern may, including inside `seq`/`choice`/`repeat`.

Here `slash` is the ambiguous trigger token (a bare `/`). It never appears in the
tree: every goal redirects it. In `expr` it becomes `RegexLiteral`; in `op` it
becomes `Div`.

## Running it

```sh
printf 'a = /ab/ ; b = c / d ;' \
  | tinyparse run examples/lexgoal/mini-js.scm --input - --dump-tree
```

The first statement reads its right-hand side in the `expr` goal, so `/ab/` is a
`RegexLiteral`. The second reads `c / d` with a `/` between two values in the
`op` goal, so that `/` is a `Div`:

```
Source
  Stmt
    Ident
    Eq
    Expr
      Primary
        RegexLiteral        ; /ab/  — scanned in the `expr` goal
    Semi
  Stmt
    Ident
    Eq
    Expr
      Primary
        Ident               ; c
      BinOp
        Div                 ; /     — scanned in the `op` goal
        Ident               ; d
    Semi
```

(`Space` trivia nodes are elided above for clarity.)

## Notes

- A grammar that names no `(lex-goal …)` form has an empty goal table and behaves
  exactly as before; the active goal starts unset and the redirect lookup is a
  no-op, so the hot path pays nothing.
- Lexical goals round-trip through the binary grammar format: `tinyparse pack
  mini-js.scm --out mini-js.tpg` then `run mini-js.tpg` parses identically. The
  goal table is a gated trailing section of the `.tpg` image, so images written
  by grammars without goals stay byte-for-byte compatible.
- The intermediate representation carries a full table of `(goal, trigger →
  target)` redirects, so although this grammar uses two goals the design admits
  any number — template strings, heredocs, and shell word contexts fit the same
  shape.
