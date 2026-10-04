# heredoc: nested here-documents with deferred slots

`heredoc.scm` parses shell command lines with `<<WORD` here-documents,
including a here-document inside a `$( … )` command substitution that is
itself inside another here-document's body. The input chooses each closing
line, and a body starts on the line after the command that opened it, so the
grammar queues each delimiter with
[deferred slots](../../docs/reference/grammar.md#deferred-slots) and matches the
closing line with a
[parametrized goal](../../docs/reference/grammar.md#parametrized-goals). It also
uses [`not`](../../docs/reference/grammar.md#not) with a rule and the
`extras:` rule modifier ([Rules](../../docs/reference/grammar.md#rules)).
[Lex tokens that depend on context](../../docs/how-to/context-sensitive-lexing.md)
walks through the same forms step by step.

## Prerequisites

Run the commands from the `tinyparse/` directory with `tinyparse` on your
`PATH`, as [Examples](../README.md) describes.

## Run it

Parse a here-document whose body runs a command substitution that opens a
second here-document:

```sh
printf 'cat <<EOF\necho Hello\necho $(\n  cat <<NESTED\nworld\nNESTED\n)\nEOF\n' | tinyparse run examples/heredoc/heredoc.scm --input - --dump-tree
```

The outer `HeredocBody` covers bytes 10 to 63 and ends with the `EOF` line at
bytes 59 to 63. The inner one, under the `CommandSubstitution`, covers bytes 44
to 57 and ends with the `NESTED` line at bytes 50 to 57:

```text
Program [0-63] [0,0]-[8,0]
  CompleteCommand [0-63] [0,0]-[8,0]
    command: Pipeline [0-9] [0,0]-[0,9]
      SimpleCommand [0-9] [0,0]-[0,9]
        name: Word [0-3] [0,0]-[0,3]
        Space [3-4] [0,3]-[0,4]
        redirect: Redirect [4-9] [0,4]-[0,9]
          HeredocOp [4-6] [0,4]-[0,6]
          word: Word [6-9] [0,6]-[0,9]
    Newline [9-10] [0,9]-[1,0]
    HeredocBody [10-63] [1,0]-[8,0]
      BodyCommand [10-21] [1,0]-[2,0]
        command: Pipeline [10-20] [1,0]-[1,10]
          SimpleCommand [10-20] [1,0]-[1,10]
            name: Word [10-14] [1,0]-[1,4]
            Space [14-15] [1,4]-[1,5]
            arg: Argument [15-20] [1,5]-[1,10]
              Word [15-20] [1,5]-[1,10]
        Newline [20-21] [1,10]-[2,0]
      BodyCommand [21-59] [2,0]-[7,0]
        command: Pipeline [21-58] [2,0]-[6,1]
          SimpleCommand [21-58] [2,0]-[6,1]
            name: Word [21-25] [2,0]-[2,4]
            Space [25-26] [2,4]-[2,5]
            arg: Argument [26-58] [2,5]-[6,1]
              CommandSubstitution [26-58] [2,5]-[6,1]
                DollarParen [26-28] [2,5]-[2,7]
                Newline [28-29] [2,7]-[3,0]
                BodyCommand [29-57] [3,0]-[6,0]
                  Space [29-31] [3,0]-[3,2]
                  command: Pipeline [31-43] [3,2]-[3,14]
                    SimpleCommand [31-43] [3,2]-[3,14]
                      name: Word [31-34] [3,2]-[3,5]
                      Space [34-35] [3,5]-[3,6]
                      redirect: Redirect [35-43] [3,6]-[3,14]
                        HeredocOp [35-37] [3,6]-[3,8]
                        word: Word [37-43] [3,8]-[3,14]
                  Newline [43-44] [3,14]-[4,0]
                  HeredocBody [44-57] [4,0]-[6,0]
                    BodyCommand [44-50] [4,0]-[5,0]
                      command: Pipeline [44-49] [4,0]-[4,5]
                        SimpleCommand [44-49] [4,0]-[4,5]
                          name: Word [44-49] [4,0]-[4,5]
                      Newline [49-50] [4,5]-[5,0]
                    end: HeredocDelimLine [50-57] [5,0]-[6,0]
                CloseParen [57-58] [6,0]-[6,1]
        Newline [58-59] [6,1]-[7,0]
      end: HeredocDelimLine [59-63] [7,0]-[8,0]
```

## How it works

The grammar follows the steps in
[Match a closing delimiter the input chose](../../docs/how-to/context-sensitive-lexing.md#match-a-closing-delimiter-the-input-chose):
`Redirect` queues each delimiter with `defer`, and `CompleteCommand` drains the
queue with `parse-deferred` after its newline. The body rule reads command
lines until the closing line:

```scheme
(rule HeredocBody extras: none
  (goal heredoc-body-mode
    (seq
      (repeat (not HeredocDelimLine BodyCommand))
      (field end: HeredocDelimLine))))
```

`parse-deferred` calls `HeredocBody` once per queued word, in order, so
`cat <<A <<B` reads the body that ends at `A` and then the body that ends at
`B`. For each call it pushes the pattern `(seq delimiter "\n")`, with
`delimiter` replaced by the queued bytes, as the pattern of `HeredocDelimLine`
in the goal `heredoc-body-mode`, and pops it when the call returns.

The patterns form a stack. While the inner body runs, the outer call has not
returned, so `NESTED\n` sits on top of `EOF\n`, and `EOF\n` is active again
when the inner body returns. Two nested `EOF` bodies therefore end at the first
and the second `EOF` line. `BodyCommand` has the shape of `CompleteCommand`
with its own `heredoc-bodies` queue, and each call gets a new, empty queue, so
`NESTED` is queued for the line `cat <<NESTED` and its body is read before the
`)` line.

## Limits

- A body line is parsed as a command. An empty line, or a line with a `(` or a
  `<` that is not `<<`, fails with `error: unexpected input [parse.syntax]` and
  exit status 1.
- `|` is read as a word, so `echo a | wc` parses with exit status 0 as one
  command with the arguments `a`, `|` and `wc`.
- The closing line is the delimiter followed by a line feed. A line `EOF  `
  does not close an `EOF` body, and neither does `  EOF` after a body line; the
  body then has no closing line. A line `  EOF` directly after the command
  closes the body, because the spaces after the command's newline are trivia of
  the command.
- A body with no closing line does not fail. The queued delimiter is dropped
  without a diagnostic, and the lines after the command parse as top-level
  commands, with exit status 0.
- A closing line at the end of the input with no line feed fails with
  `error: unexpected input [parse.syntax]` and exit status 1.
- Quoted delimiters and `<<-` are not supported. In `cat <<'EOF'` the delimiter
  is the word `'EOF'`, quotes included, and in `cat <<-EOF` it is `-EOF`; a
  closing line `EOF` closes neither, so they parse as a body with no closing
  line.

## Files

| File | Contents |
|------|----------|
| `heredoc.scm` | the grammar |

The tests `test/suite/lexgoal/heredoc-*.test` parse the nested, repeated and
same-delimiter cases with this grammar.
