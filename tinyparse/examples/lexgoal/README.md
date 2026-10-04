# lexgoal: tell a regex from a division

`mini-js.scm` parses assignments such as `a = /ab/ ;` and `b = c / d ;`, in
which one byte, `/`, is two different tokens: a regular-expression literal
where a value starts, and the division operator after a value. The grammar
selects the reading with the [`lex-goal`](../../docs/reference/grammar.md#lexical-goals)
and [`goal`](../../docs/reference/grammar.md#goal) forms.
[Lex tokens that depend on context](../../docs/how-to/context-sensitive-lexing.md)
covers when to use them.

## Prerequisites

Run the commands from the `tinyparse/` directory with `tinyparse` on your
`PATH`, as [Examples](../README.md) describes.

## Run it

Parse two statements:

```sh
printf 'a = /ab/ ; b = c / d ;' | tinyparse run examples/lexgoal/mini-js.scm --input - --dump-tree
```

In the first statement, `/ab/` at bytes 4 to 8 is one `RegexLiteral` leaf. In
the second, the `/` at byte 17 is a `Div` leaf between two identifiers:

```text
Source [0-22] [0,0]-[0,22]
  Stmt [0-10] [0,0]-[0,10]
    Ident [0-1] [0,0]-[0,1]
    Space [1-2] [0,1]-[0,2]
    Eq [2-3] [0,2]-[0,3]
    Space [3-4] [0,3]-[0,4]
    Expr [4-8] [0,4]-[0,8]
      Primary [4-8] [0,4]-[0,8]
        RegexLiteral [4-8] [0,4]-[0,8]
    Space [8-9] [0,8]-[0,9]
    Semi [9-10] [0,9]-[0,10]
  Space [10-11] [0,10]-[0,11]
  Stmt [11-22] [0,11]-[0,22]
    Ident [11-12] [0,11]-[0,12]
    Space [12-13] [0,12]-[0,13]
    Eq [13-14] [0,13]-[0,14]
    Space [14-15] [0,14]-[0,15]
    Expr [15-20] [0,15]-[0,20]
      Primary [15-16] [0,15]-[0,16]
        Ident [15-16] [0,15]-[0,16]
      Space [16-17] [0,16]-[0,17]
      BinOp [17-20] [0,17]-[0,20]
        Div [17-18] [0,17]-[0,18]
        Space [18-19] [0,18]-[0,19]
        Ident [19-20] [0,19]-[0,20]
    Space [20-21] [0,20]-[0,21]
    Semi [21-22] [0,21]-[0,22]
```

The goals are part of a packed grammar. Pack the grammar into a scratch
directory and parse the same input with the packed file:

```sh
WORK="$(mktemp -d)"
tinyparse pack examples/lexgoal/mini-js.scm --out "$WORK/mini-js.tpg"
printf 'a = /ab/ ; b = c / d ;' | tinyparse run "$WORK/mini-js.tpg" --input - --dump-tree
```

The tree is the same as above.

## How it works

The grammar defines the ambiguous token `slash` and one token for each
reading, then declares two goals that redirect `slash`:

```scheme
(token slash        "/")
(token RegexLiteral (regex "/[^/]*/"))
(token Div          "/")

(lex-goal expr slash: RegexLiteral)
(lex-goal op   slash: Div)
```

The two rules that read a `/` refer to `slash` inside a `goal` form:

```scheme
(rule Primary
  (goal expr (choice slash Ident)))

(rule BinOp
  (seq (goal op slash) Ident))
```

- In `Primary`, the `expr` goal is active, so `slash` scans the pattern of
  `RegexLiteral` and the leaf has the kind `RegexLiteral`.
- In `BinOp`, the `op` goal is active, so `slash` scans `/` and the leaf has the
  kind `Div`.
- Every reference to `slash` is inside a goal that redirects it, so no node of
  kind `slash` appears in the tree.

std.javascript reads `/` with the same two forms; see
`grammars/std/javascript/grammar.scm`.

## Limits

Each of these inputs fails with `error: unexpected input [parse.syntax]` and
exit status 1:

- `a = /a\/b/ ;`: a `RegexLiteral` ends at the first `/` after the opening
  one, so an escaped slash ends it early.
- `a = 1 ;`: a value is an identifier or a regex literal; the grammar has no
  number token.
- `a = / ;`: a `/` where a value starts must open a regex literal, and this
  one has no closing `/`.

## Files

| File | Contents |
|------|----------|
| `mini-js.scm` | the grammar |

`test/suite/lexgoal/js-slash.test` and `test/suite/binary/pack-lexgoal.test`
run the commands under Run it.
