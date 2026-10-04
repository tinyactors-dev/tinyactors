# sexpr: a recursive grammar with fields and trivia

`sexpr.scm` parses parenthesized s-expressions such as
`(define (square x) (* x x))`. It is a recursive grammar: a `List` holds
`Form`s and a `Form` can be a `List`. It uses the `token`, `regex`, `not`,
`any`, `seq`, `choice`, `repeat`, `field`, `EOF`, `extras` and `trivia` forms,
all described in the [grammar language reference](../../docs/reference/grammar.md).
[Write your first grammar](../../docs/tutorials/first-grammar.md) builds the
same grammar step by step.

## Prerequisites

Run the commands from the `tinyparse/` directory with `tinyparse` on your
`PATH`, as [Examples](../README.md) describes.

## Run it

Parse a definition and print the tree:

```sh
printf '(define (square x) (* x x))' | tinyparse run examples/sexpr/sexpr.scm --input - --dump-tree
```

Each nested list is a `List` under a `Form`, with its parentheses in the
`open` and `close` fields and `Space` nodes between its elements:

```text
Source [0-27] [0,0]-[0,27]
  Form [0-27] [0,0]-[0,27]
    List [0-27] [0,0]-[0,27]
      open: ( [0-1] [0,0]-[0,1]
      Form [1-7] [0,1]-[0,7]
        Atom [1-7] [0,1]-[0,7]
      Space [7-8] [0,7]-[0,8]
      Form [8-18] [0,8]-[0,18]
        List [8-18] [0,8]-[0,18]
          open: ( [8-9] [0,8]-[0,9]
          Form [9-15] [0,9]-[0,15]
            Atom [9-15] [0,9]-[0,15]
          Space [15-16] [0,15]-[0,16]
          Form [16-17] [0,16]-[0,17]
            Atom [16-17] [0,16]-[0,17]
          close: ) [17-18] [0,17]-[0,18]
      Space [18-19] [0,18]-[0,19]
      Form [19-26] [0,19]-[0,26]
        List [19-26] [0,19]-[0,26]
          open: ( [19-20] [0,19]-[0,20]
          Form [20-21] [0,20]-[0,21]
            Atom [20-21] [0,20]-[0,21]
          Space [21-22] [0,21]-[0,22]
          Form [22-23] [0,22]-[0,23]
            Atom [22-23] [0,22]-[0,23]
          Space [23-24] [0,23]-[0,24]
          Form [24-25] [0,24]-[0,25]
            Atom [24-25] [0,24]-[0,25]
          close: ) [25-26] [0,25]-[0,26]
      close: ) [26-27] [0,26]-[0,27]
```

Parse a comment followed by a list:

```sh
printf '; a comment\n(a b)' | tinyparse run examples/sexpr/sexpr.scm --input - --dump-tree
```

The comment is a `Comment` node before the first `Form`, though no rule names
it:

```text
Source [0-17] [0,0]-[1,5]
  Comment [0-11] [0,0]-[0,11]
  Space [11-12] [0,11]-[1,0]
  Form [12-17] [1,0]-[1,5]
    List [12-17] [1,0]-[1,5]
      open: ( [12-13] [1,0]-[1,1]
      Form [13-14] [1,1]-[1,2]
        Atom [13-14] [1,1]-[1,2]
      Space [14-15] [1,2]-[1,3]
      Form [15-16] [1,3]-[1,4]
        Atom [15-16] [1,3]-[1,4]
      close: ) [16-17] [1,4]-[1,5]
```

`broken.scm` is a grammar with an unterminated list. Check it:

```sh
tinyparse check examples/sexpr/broken.scm
```

The check fails and exits with status 1:

```text
error: unterminated list
```

## How it works

The three rules describe the nesting (excerpt of `sexpr.scm`):

```scheme
(rule Source
  (seq (repeat Form) EOF))

(rule Form
  (choice Atom List))

(rule List
  (seq
    (field open: "(")
    (repeat Form)
    (field close: ")")))
```

`List` refers to `Form` and `Form` refers back to `List`, so the parser
descends as deep as the input nests. No token spans a parenthesis: `Atom`
matches `[^() \t\r\n;]+`, `Comment` matches `;` up to the end of the line, and
`Space` matches a run of whitespace.

`(field open: "(")` and `(field close: ")")` name the parentheses, so a program
reading the tree asks for the `open` child by name instead of counting
children. The dump prints a field-bound node as `field: kind`.

`(extras (trivia Space) (trivia Comment))` lets whitespace and comments appear
between any two tokens. They stay in the tree as sibling nodes, as the
`Comment` and `Space` lines in the second dump show.
[How tinyparse shapes the tree](../../docs/explanation/tree-shape.md) explains
where trivia nodes go.

## Limits

- `Atom` is one token kind for every word: `define`, `42` and `x` all parse as
  `Atom`, and nothing tells a keyword, a number or a symbol apart.
- Strings are not modeled. `("a b")` parses with exit status 0 as two atoms,
  `"a` and `b"`.
- Quote, quasiquote and dotted pairs have no syntax of their own: `'x` is one
  `Atom`.
- A `;` inside a list starts a comment that runs to the end of the line, so
  `(a ;b)` on one line leaves the list open and fails with
  `parse.syntax`, exit status 1.
- The grammar has no error recovery. A list left open prints
  `error: unexpected input [parse.syntax]`, exits with status 1 and prints no
  tree; [recover](../recover/README.md) adds recovery for that case.

## Files

| File | Contents |
|------|----------|
| `sexpr.scm` | the grammar |
| `broken.scm` | a grammar that fails `tinyparse check`: its last list is never closed, and its `start:` names a rule it does not define |
