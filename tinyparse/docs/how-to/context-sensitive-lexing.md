# Lex tokens that depend on context

This guide is for a grammar author whose language has tokens that one fixed
token definition reads wrong: a keyword that an identifier pattern swallows, a
`/` that is a regular expression in one place and division in another, a
here-document whose closing line the input chooses. It maps each problem to the
grammar form that solves it and shows how to write that form.

## Prerequisites

- A source checkout with the shell in its `tinyparse/` directory, built and on
  your `PATH`:

  ```sh
  mise run build
  export PATH="$PWD/build/bin:$PATH"
  ```

  For a release, see [Install tinyparse](install.md).
- A grammar you can already run, as in
  [Write your first grammar](../tutorials/first-grammar.md).
- A scratch directory:

  ```sh
  WORK="$(mktemp -d)"
  ```

## Choose the mechanism

| Problem | Form | Reference |
|---------|------|-----------|
| A word token also matches a keyword, so a repetition of words runs past it | `(reserved P "word" …)` | [Tokens](../reference/grammar.md#tokens) |
| A keyword literal matches the start of a longer word (`if` in `iffy`) | a keyword token ending in a zero-width `(not …)` | [not](../reference/grammar.md#not) |
| The same bytes are a different token depending on where the parser is | `(lex-goal …)` and `(goal …)` | [Lexical goals](../reference/grammar.md#lexical-goals), [goal](../reference/grammar.md#goal) |
| A closing delimiter is chosen by the input, and the body comes after the line that names it | deferred slots and a parametrized goal | [Deferred slots](../reference/grammar.md#deferred-slots), [Parametrized goals](../reference/grammar.md#parametrized-goals) |
| A repetition of rules must stop before a line that ends it | `(not GUARD RULE)` | [not](../reference/grammar.md#not) |

## Keep keywords out of identifiers

A command language reads `if ready then go` as a conditional. With a plain
`(token Word (regex "[a-z]+"))`, the command after `if` takes `then` as an
argument and the conditional never matches. Guard the word token with
`reserved`, and give each keyword its own token that refuses to end inside a
longer word. Save this grammar as `$WORK/commands.scm`:

```scheme
(grammar commands
  start: Script

  (token Space (regex "[ \t\n]+"))
  (token If    (seq "if"   (not (regex "[a-z]") (repeat (regex "[a-z]")))))
  (token Then  (seq "then" (not (regex "[a-z]") (repeat (regex "[a-z]")))))
  (token Word  (reserved (regex "[a-z]+") "if" "then"))

  (extras (trivia Space))

  (rule Script (seq (repeat Statement) EOF))
  (rule Statement (choice Conditional Command))
  (rule Conditional
    (seq If (field test: Command) Then (field body: Command)))
  (rule Command
    (seq (field name: Word) (repeat (field argument: Word)))))
```

- `(reserved (regex "[a-z]+") "if" "then")` matches a whole word and then
  rejects it when the word is exactly `if` or `then`. `iffy` and `thence` stay
  words.
- In `If` and `Then`, the guard `(regex "[a-z]")` rejects a following letter,
  and the `repeat` after it then matches nothing, so the check consumes no
  byte. A bare `"if"` literal would match the first two bytes of `iffy`.

Parse a line that uses both keywords and words that start with them:

```sh
printf 'if ready then go iffy thence\n' > "$WORK/script.txt"
tinyparse run "$WORK/commands.scm" --input "$WORK/script.txt" --dump-tree
```

The command after `if` stops at `then`, and `iffy` and `thence` are arguments:

```text
Script [0-29] [0,0]-[1,0]
  Statement [0-28] [0,0]-[0,28]
    Conditional [0-28] [0,0]-[0,28]
      If [0-2] [0,0]-[0,2]
      Space [2-3] [0,2]-[0,3]
      test: Command [3-8] [0,3]-[0,8]
        name: Word [3-8] [0,3]-[0,8]
      Space [8-9] [0,8]-[0,9]
      Then [9-13] [0,9]-[0,13]
      Space [13-14] [0,13]-[0,14]
      body: Command [14-28] [0,14]-[0,28]
        name: Word [14-16] [0,14]-[0,16]
        Space [16-17] [0,16]-[0,17]
        argument: Word [17-21] [0,17]-[0,21]
        Space [21-22] [0,21]-[0,22]
        argument: Word [22-28] [0,22]-[0,28]
  Space [28-29] [0,28]-[1,0]
```

Without `reserved`, the same input parses with exit status 0 as a single
`Command` named `if` with five arguments. Reserve only the words the language
forbids as names; where a keyword is a legal name, such as a property name,
use a second, unguarded word token there. `grammars/std/javascript/grammar.scm`
and `grammars/std/shell/grammar.scm` use both tokens this way.

## Read one spelling as two tokens

In JavaScript, `/` starts a regular-expression literal where a value is
expected and is the division operator after a value. Declare a lexical goal for
each context and select the goal in the rules. The steps below use
[`examples/lexgoal/mini-js.scm`](../../examples/lexgoal/mini-js.scm).

1. Define the ambiguous token, the trigger, and one token per reading. The
   trigger's own pattern is used only outside every goal that redirects it.

   ```scheme
   (token slash        "/")
   (token RegexLiteral (regex "/[^/]*/"))
   (token Div          "/")
   ```

2. Declare one `lex-goal` per context. Each maps the trigger to the token it
   scans as in that goal.

   ```scheme
   (lex-goal expr slash: RegexLiteral)
   (lex-goal op   slash: Div)
   ```

3. Refer to the trigger inside a `goal` form in each context. The goal stays
   active in every rule the wrapped pattern calls, until a nested `goal`
   replaces it.

   ```scheme
   (rule Primary
     (goal expr (choice slash Ident)))

   (rule BinOp
     (seq (goal op slash) Ident))
   ```

Parse a statement with a regex literal followed by a division:

```sh
printf 'a = /b/ / c ;\n' > "$WORK/slash.txt"
tinyparse run examples/lexgoal/mini-js.scm --input "$WORK/slash.txt" --dump-tree
```

The first `/` starts a `RegexLiteral` and the second is a `Div`. Neither leaf
has the trigger's kind `slash`:

```text
Source [0-14] [0,0]-[1,0]
  Stmt [0-13] [0,0]-[0,13]
    Ident [0-1] [0,0]-[0,1]
    Space [1-2] [0,1]-[0,2]
    Eq [2-3] [0,2]-[0,3]
    Space [3-4] [0,3]-[0,4]
    Expr [4-11] [0,4]-[0,11]
      Primary [4-7] [0,4]-[0,7]
        RegexLiteral [4-7] [0,4]-[0,7]
      Space [7-8] [0,7]-[0,8]
      BinOp [8-11] [0,8]-[0,11]
        Div [8-9] [0,8]-[0,9]
        Space [9-10] [0,9]-[0,10]
        Ident [10-11] [0,10]-[0,11]
    Space [11-12] [0,11]-[0,12]
    Semi [12-13] [0,12]-[0,13]
  Space [13-14] [0,13]-[1,0]
```

std.javascript declares the same pair as the goals `expr` and `div` in
`grammars/std/javascript/grammar.scm`. The example's README,
[lexgoal: tell a regex from a division](../../examples/lexgoal/README.md),
lists the inputs this small grammar rejects.

## Match a closing delimiter the input chose

A shell here-document names its closing line on the command line (`cat <<EOF`),
and its body starts on the line after the command. Two mechanisms work together:
a deferred slot queues the delimiter until the command line ends, and a
parametrized goal builds the closing-line token from the queued bytes. The
steps below use
[`examples/heredoc/heredoc.scm`](../../examples/heredoc/heredoc.scm).

1. Declare the closing-line token with no pattern, and a goal whose target is a
   `param` form. Inside the body, `delimiter` stands for the queued bytes:

   ```scheme
   (token HeredocDelimLine)

   (lex-goal heredoc-body-mode
     HeredocDelimLine: (param delimiter (seq delimiter "\n")))
   ```

2. Declare a slot in the rule that owns a command line, and drain it as the
   last element of that rule, after the line's newline:

   ```scheme
   (rule CompleteCommand
     (seq
       (deferred-slot heredoc-bodies)
       (field command: Pipeline)
       Newline
       (parse-deferred heredoc-bodies HeredocBody)))
   ```

   Each call of the rule gets its own empty queue. `parse-deferred` calls
   `HeredocBody` once per queued item, in the order they were queued.

3. Queue the delimiter where the operator reads it. `defer` produces the same
   node as the pattern it wraps:

   ```scheme
   (rule Redirect
     (seq HeredocOp
          (field word: (defer heredoc-bodies Word))))
   ```

4. Write the body rule under the goal, reading lines until the closing line:

   ```scheme
   (rule HeredocBody
     (goal heredoc-body-mode
       (seq
         (repeat (not HeredocDelimLine BodyCommand))
         (field end: HeredocDelimLine))))
   ```

   `heredoc.scm` also gives this rule the modifier `extras: none`. The trees
   below, and those in the example, are the same with and without it.

The compiler does not check where `deferred-slot` and `parse-deferred` sit.
Keep them out of the start rule, and keep `parse-deferred` out of any position
that can be backtracked after it ran; the conditions are listed under
[Deferred slots](../reference/grammar.md#deferred-slots).

Parse a one-line here-document:

```sh
printf 'cat <<EOF\nhello\nEOF\n' > "$WORK/hello.sh"
tinyparse run examples/heredoc/heredoc.scm --input "$WORK/hello.sh" --dump-tree
```

The body and its closing line are children of the command that opened it:

```text
Program [0-20] [0,0]-[3,0]
  CompleteCommand [0-20] [0,0]-[3,0]
    command: Pipeline [0-9] [0,0]-[0,9]
      SimpleCommand [0-9] [0,0]-[0,9]
        name: Word [0-3] [0,0]-[0,3]
        Space [3-4] [0,3]-[0,4]
        redirect: Redirect [4-9] [0,4]-[0,9]
          HeredocOp [4-6] [0,4]-[0,6]
          word: Word [6-9] [0,6]-[0,9]
    Newline [9-10] [0,9]-[1,0]
    HeredocBody [10-20] [1,0]-[3,0]
      BodyCommand [10-16] [1,0]-[2,0]
        command: Pipeline [10-15] [1,0]-[1,5]
          SimpleCommand [10-15] [1,0]-[1,5]
            name: Word [10-15] [1,0]-[1,5]
        Newline [15-16] [1,5]-[2,0]
      end: HeredocDelimLine [16-20] [2,0]-[3,0]
```

The nested and repeated cases, and the inputs this grammar reads wrong, are in
[heredoc: nested here-documents with deferred slots](../../examples/heredoc/README.md).
std.shell parses here-documents with the same forms
([std.shell](../reference/std-grammars.md#stdshell)).

## Stop a repetition before a terminator

`(not GUARD P …)` tries `GUARD` at the current byte without consuming it, and
matches `P …` only when `GUARD` fails. `P` may be a rule, so the form stops a
repetition of rules at a terminator that the rule itself would accept. In step 4
above, a `BodyCommand` reads the line `EOF` as a command named `EOF`;
the guard ends the repetition there instead:

```scheme
(repeat (not HeredocDelimLine BodyCommand))
```

With `(repeat BodyCommand)` in its place, the repetition consumes the closing
line, `HeredocBody` fails, and `hello.sh` parses with exit status 0 as three
top-level commands. `GUARD` must be a token or a token expression; a rule
reference as the guard never matches ([not](../reference/grammar.md#not)).

## Check that the grammar survives packing

Goals, deferred slots and parametrized goals are part of a packed grammar.
To confirm it for your grammar, pack it and compare the trees:

```sh
tinyparse pack examples/lexgoal/mini-js.scm --out "$WORK/mini-js.tpg"
tinyparse run examples/lexgoal/mini-js.scm --input "$WORK/slash.txt" --dump-tree > "$WORK/source.tree"
tinyparse run "$WORK/mini-js.tpg" --input "$WORK/slash.txt" --dump-tree > "$WORK/packed.tree"
cmp "$WORK/source.tree" "$WORK/packed.tree"
```

`cmp` prints nothing and exits with status 0 when the trees are equal. The
`pack` command is described in [tinyparse(1)](../../tinyparse.1.md).
