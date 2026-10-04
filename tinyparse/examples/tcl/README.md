# Tcl-ish words

A small tinyparse grammar for one Tcl command line. It teaches two things
at once: how `[ … ]` and `{ … }` recurse to any depth, and how a single word
can fuse several substitution forms with no whitespace between them.

## The grammar

[`tcl.scm`](tcl.scm) reads a command as a run of words, and a word as a run
of adjacent *parts*. The parts split into two kinds.

The forms with no inner structure stay single greedy tokens:

| Token         | Matches              | Example        |
|---------------|----------------------|----------------|
| `Expansion`   | the literal `{*}`    | `{*}`          |
| `QuotedWord`  | `"` … `"`            | `"a b"`        |
| `VariableSub` | `${name}` or `$name` | `${c}`, `$x`   |
| `Bareword`    | ordinary bytes       | `echo`, `d`    |

Command substitution and brace groups are rules whose delimiters are literal
fields and whose bodies hold more words:

```scheme
(rule CommandSub (seq (field open: "[") (repeat Word) (field close: "]")))
(rule Braces     (seq (field open: "{") (repeat Word) (field close: "}")))
```

Because a word can be a `CommandSub` or a `Braces`, both nest. This is the
same recursion the [sexpr example](../sexpr/sexpr.scm) uses for parenthesized
lists.

Whitespace between words is declared as `extras` trivia (`Space`), so it is
consumed implicitly and preserved in the tree.

## Nesting

Feed `run.sh` a command substitution that contains another:

```sh
./run.sh 'set x [a [b] c]'
```

The inner `[b]` is a `CommandSub` child of the outer one:

```text
Source
  Command
    Word
      WordPart
        Bareword
      Space
      WordPart
        Bareword
      Space
      WordPart
        CommandSub
          open
          Word
            WordPart
              Bareword
            Space
            WordPart
              CommandSub
                open
                Word
                  WordPart
                    Bareword
                close
            Space
            WordPart
              Bareword
          close
```

Brace groups nest the same way: `set y {a {b} c}` yields a `Braces` inside a
`Braces`.

## The fused word

The harder input jams five forms into one word with no whitespace:

```sh
./run.sh 'echo {*}"a b"${c}d[expr {1+1}]'
```

The dump shows a single `Command` whose parts appear in source order, and the
`[expr {1+1}]` substitution carries a nested `{1+1}` brace group:

```text
Source
  Command
    Word
      WordPart
        Bareword
      Space
      WordPart
        Expansion
      WordPart
        QuotedWord
      WordPart
        VariableSub
      WordPart
        Bareword
      WordPart
        CommandSub
          open
          Word
            WordPart
              Bareword
            Space
            WordPart
              Braces
                open
                Word
                  WordPart
                    Bareword
                close
          close
```

Two details make the fusion work. The space inside `"a b"` belongs to the
`QuotedWord` token, so trivia never sees it: trivia fires only between tokens,
never inside one match, and the word stays whole. And `Expansion` is matched
before `Braces` in the `WordPart` choice, so `{*}` reads as the expansion
marker rather than an empty-ish brace group.

## What this leaves out

This models a single command line, not a Tcl program. A newline is ordinary
whitespace here rather than a command separator, and the grammar tracks no
semantics: it does not expand variables, evaluate substitutions, or interpret
backslash escapes inside a word.
