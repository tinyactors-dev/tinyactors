# tcl: nested brackets and fused words

`tcl.scm` parses one Tcl-like command line. Command substitutions `[ … ]` and
brace groups `{ … }` are recursive [rules](../../docs/reference/grammar.md#rules)
that nest to any depth, and a single word can join several substitution forms
with no space between them. The grammar uses
[`field`](../../docs/reference/grammar.md#field), `Space` as
[trivia](../../docs/reference/grammar.md#extras), and the order of a
[`choice`](../../docs/reference/grammar.md#matching).

## Prerequisites

Run the commands from the `tinyparse/` directory with `tinyparse` on your
`PATH`, as [Examples](../README.md) describes.

## Run it

`run.sh` passes its argument to
`tinyparse run examples/tcl/tcl.scm --input - --dump-tree`. It runs the program
named by the environment variable `TP`, or `build/bin/tinyparse` of this source
tree when `TP` is unset; when that file is not executable, it runs `tinyparse`
from `PATH`. With no argument it parses the fused word shown below.

Parse a command substitution that contains another:

```sh
examples/tcl/run.sh 'set x [a [b] c]'
```

The inner `[b]` at bytes 9 to 12 is a `CommandSub` inside the outer one at
bytes 6 to 15:

```text
Source [0-15] [0,0]-[0,15]
  Command [0-15] [0,0]-[0,15]
    Word [0-15] [0,0]-[0,15]
      WordPart [0-3] [0,0]-[0,3]
        Bareword [0-3] [0,0]-[0,3]
      Space [3-4] [0,3]-[0,4]
      WordPart [4-5] [0,4]-[0,5]
        Bareword [4-5] [0,4]-[0,5]
      Space [5-6] [0,5]-[0,6]
      WordPart [6-15] [0,6]-[0,15]
        CommandSub [6-15] [0,6]-[0,15]
          open: [ [6-7] [0,6]-[0,7]
          Word [7-14] [0,7]-[0,14]
            WordPart [7-8] [0,7]-[0,8]
              Bareword [7-8] [0,7]-[0,8]
            Space [8-9] [0,8]-[0,9]
            WordPart [9-12] [0,9]-[0,12]
              CommandSub [9-12] [0,9]-[0,12]
                open: [ [9-10] [0,9]-[0,10]
                Word [10-11] [0,10]-[0,11]
                  WordPart [10-11] [0,10]-[0,11]
                    Bareword [10-11] [0,10]-[0,11]
                close: ] [11-12] [0,11]-[0,12]
            Space [12-13] [0,12]-[0,13]
            WordPart [13-14] [0,13]-[0,14]
              Bareword [13-14] [0,13]-[0,14]
          close: ] [14-15] [0,14]-[0,15]
```

Brace groups nest the same way:

```sh
examples/tcl/run.sh 'set y {a {b} c}'
```

```text
Source [0-15] [0,0]-[0,15]
  Command [0-15] [0,0]-[0,15]
    Word [0-15] [0,0]-[0,15]
      WordPart [0-3] [0,0]-[0,3]
        Bareword [0-3] [0,0]-[0,3]
      Space [3-4] [0,3]-[0,4]
      WordPart [4-5] [0,4]-[0,5]
        Bareword [4-5] [0,4]-[0,5]
      Space [5-6] [0,5]-[0,6]
      WordPart [6-15] [0,6]-[0,15]
        Braces [6-15] [0,6]-[0,15]
          open: { [6-7] [0,6]-[0,7]
          Word [7-14] [0,7]-[0,14]
            WordPart [7-8] [0,7]-[0,8]
              Bareword [7-8] [0,7]-[0,8]
            Space [8-9] [0,8]-[0,9]
            WordPart [9-12] [0,9]-[0,12]
              Braces [9-12] [0,9]-[0,12]
                open: { [9-10] [0,9]-[0,10]
                Word [10-11] [0,10]-[0,11]
                  WordPart [10-11] [0,10]-[0,11]
                    Bareword [10-11] [0,10]-[0,11]
                close: } [11-12] [0,11]-[0,12]
            Space [12-13] [0,12]-[0,13]
            WordPart [13-14] [0,13]-[0,14]
              Bareword [13-14] [0,13]-[0,14]
          close: } [14-15] [0,14]-[0,15]
```

Parse a word that joins five forms:

```sh
examples/tcl/run.sh 'echo {*}"a b"${c}d[expr {1+1}]'
```

After `echo`, the parts follow each other with no `Space` between them, and the
`CommandSub` at bytes 18 to 30 holds the brace group `{1+1}`:

```text
Source [0-30] [0,0]-[0,30]
  Command [0-30] [0,0]-[0,30]
    Word [0-30] [0,0]-[0,30]
      WordPart [0-4] [0,0]-[0,4]
        Bareword [0-4] [0,0]-[0,4]
      Space [4-5] [0,4]-[0,5]
      WordPart [5-8] [0,5]-[0,8]
        Expansion [5-8] [0,5]-[0,8]
      WordPart [8-13] [0,8]-[0,13]
        QuotedWord [8-13] [0,8]-[0,13]
      WordPart [13-17] [0,13]-[0,17]
        VariableSub [13-17] [0,13]-[0,17]
      WordPart [17-18] [0,17]-[0,18]
        Bareword [17-18] [0,17]-[0,18]
      WordPart [18-30] [0,18]-[0,30]
        CommandSub [18-30] [0,18]-[0,30]
          open: [ [18-19] [0,18]-[0,19]
          Word [19-29] [0,19]-[0,29]
            WordPart [19-23] [0,19]-[0,23]
              Bareword [19-23] [0,19]-[0,23]
            Space [23-24] [0,23]-[0,24]
            WordPart [24-29] [0,24]-[0,29]
              Braces [24-29] [0,24]-[0,29]
                open: { [24-25] [0,24]-[0,25]
                Word [25-28] [0,25]-[0,28]
                  WordPart [25-28] [0,25]-[0,28]
                    Bareword [25-28] [0,25]-[0,28]
                close: } [28-29] [0,28]-[0,29]
          close: ] [29-30] [0,29]-[0,30]
```

## How it works

Forms with no inner structure are tokens:

| Token | Matches | Example |
|-------|---------|---------|
| `Expansion` | the literal `{*}` | `{*}` |
| `QuotedWord` | `"`, any bytes other than `"`, then `"` | `"a b"` |
| `VariableSub` | `${name}` or `$name` | `${c}`, `$x` |
| `Bareword` | bytes other than whitespace and `{ } " $ [ ]` | `echo`, `d` |

Command substitutions and brace groups are rules whose bodies hold words, and a
word part may be either of them, so both recurse (excerpt; `tcl.scm` writes
`WordPart` one alternative per line):

```scheme
(rule WordPart
  (choice Expansion QuotedWord VariableSub CommandSub Braces Bareword))

(rule CommandSub (seq (field open: "[") (repeat Word) (field close: "]")))
(rule Braces     (seq (field open: "{") (repeat Word) (field close: "}")))
```

- The space inside `"a b"` belongs to the `QuotedWord` token. Trivia is
  consumed only between terminals, never inside a token, so the quoted word
  stays one leaf.
- `Expansion` comes before `Braces` in the ordered choice. In the other order,
  `{*}` would parse as a `Braces` node holding a `Bareword` `*`.

## Limits

- Word boundaries are not in the tree. `Space` is trivia, so it is consumed
  between word parts as well as between words, and the first `Word` takes
  every part of the command. Each `Command` above is one `Word` whose parts are
  separated by `Space` nodes; a program that needs the words splits on those.
- A line feed is whitespace. Two lines parse as one `Command`.
- Backslash escapes are not supported. `puts \[x\]` parses with exit status 0
  as a `Bareword` `\` followed by a `CommandSub` that holds the `Bareword` `x\`.
- An unterminated `[`, `{` or `"`, or a `$` with no name, fails with
  `error: unexpected input [parse.syntax]` and exit status 1.
- The grammar parses only. It does not substitute variables or evaluate
  commands.

## Files

| File | Contents |
|------|----------|
| `tcl.scm` | the grammar |
| `run.sh` | parses its argument and prints the tree |

The tests `test/suite/examples/tcl-*.test` parse a plain command, both
kinds of nesting and the fused word with this grammar.
