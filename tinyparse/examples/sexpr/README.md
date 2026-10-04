# sexpr — recursion, fields, and trivia in one grammar

This is the canonical tinyparse grammar: parenthesized s-expressions parsed
by a rule that calls itself. It is the example to read first, and the one the
core test suite leans on to exercise `seq`, `choice`, `repeat`, `field`, and
`extras`.

## The grammar

A source is a run of forms. A form is either an atom or a list, and a list is a
parenthesis, more forms, and a closing parenthesis. A list contains forms and a
form may be a list, so the rules describe the nesting and the tokens never have
to:

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

`List` references `Form`, and `Form` can resolve back to `List`. That cycle is
the whole point: a grammar describes nesting by naming itself, and the parser
follows the names down as deep as the input goes. There is no "scan to the
matching paren" token doing the structure's job.

Three smaller ideas ride along:

- **Tokens stay lexical.** `Atom` is a regular pattern (`[^() \t\r\n;]+`),
  `Comment` is `;` to end of line, `Space` is a run of whitespace. None of them
  reach across a parenthesis.
- **Fields name the parentheses.** `(field open: "(")` and `(field close: ")")`
  give the delimiters cursor-visible names, so a consumer asks for `open` rather
  than counting children.
- **Extras carry the trivia.** `(extras (trivia Space) (trivia Comment))` lets
  whitespace and comments appear between any two tokens without a rule
  mentioning them; they land in the tree as siblings, out of the rules' way.

## Running it

```sh
printf '(define (square x) (* x x))' \
  | tinyparse run examples/sexpr/sexpr.scm --input - --dump-tree
```

The dump follows the recursion: each nested list is a `List` under a `Form`,
with `open`/`close` fields around its children and `Space` trivia between them.

```
Source
  Form
    List
      open
      Form
        Atom
      Space
      Form
        List
          open
          Form
            Atom
          Space
          Form
            Atom
          close
      Space
      Form
        List
          open
          Form
            Atom
          Space
          Form
            Atom
          Space
          Form
            Atom
          close
      close
```

A leading comment shows the extras at work. It parses as a `Comment` sibling
before the first `Form`, and no rule names it:

```sh
printf '; a comment\n(a b)' \
  | tinyparse run examples/sexpr/sexpr.scm --input - --dump-tree
```

```
Source
  Comment
  Space
  Form
    List
      open
      Form
        Atom
      Space
      Form
        Atom
      close
```

## Limits

The grammar reads structure and leaves meaning alone. `Atom` is one
undifferentiated token, so `define`, `42`, and `x` are all atoms; nothing
distinguishes a keyword, a number, or a string. Quote, quasiquote, and dotted pairs are not modeled. The
goal here is to teach the recursive shape cleanly, so the lexer stays small and
everything interesting happens in the rules.
