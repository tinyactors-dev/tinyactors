# recover — grammar-authored error recovery

`list.scm` is a tiny grammar for parenthesized lists of atoms whose `List` rule
asks the parser to recover when a closing parenthesis never arrives.

```scheme
(rule List
  (recover
    (seq
      (field open: "(")
      (repeat Item)
      (field close: ")"))
    (on eof
      message: "unterminated list"
      (label here: "expected `)` before end of file")
      help: "add a closing `)`"
      insert: ")")))
```

The `(recover BODY (on eof ...))` form wraps an ordinary rule body. The parser
first tries `BODY` as written. If the body fails because the input ended where a
terminal was still expected, recovery fires: the `insert:` token is synthesized
as a zero-width node flagged *missing*, the `message:`/`help:`/`label:` text
becomes an error-severity diagnostic, and the parse completes structurally so a
tree is still produced.

## Try it

```sh
printf '(a (b c)' | tinyparse run examples/recover/list.scm --input - --dump-tree
```

The input is missing one closing parenthesis. The dump shows the recovered tree
with a `MISSING )` node where the parenthesis should have been:

```
Source
  Item
    List
      open
      Item
        Atom
      Space
      Item
        List
          open
          Item
            Atom
          Space
          Item
            Atom
          close
      MISSING )
```

and the diagnostic goes to standard error:

```
unterminated list [parse.recover]
  help: add a closing `)`
```

The command exits non-zero because an error-severity diagnostic was produced,
even though a tree was built. Recovery reports the problem; it does not hide it.
