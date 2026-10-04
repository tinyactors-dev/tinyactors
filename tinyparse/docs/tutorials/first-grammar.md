# Write your first grammar

This tutorial is for a developer who has not used tinyparse before. You write a
grammar for s-expressions such as `(a (b c))` in six steps, and after each step
you run it with the `tinyparse` command and read the tree it prints.

## Prerequisites

You need the `tinyparse` command on your `PATH`.
[Install tinyparse](../how-to/install.md) describes the release tarball. From a
source checkout, build it in the `tinyparse/` directory and put the build output
on your `PATH`:

```sh
mise run build
export PATH="$PWD/build/bin:$PATH"
```

Check that the shell finds it:

```sh
tinyparse version
```

It prints its version:

```text
tinyparse 0.1.0
```

## Make a working directory

Every file in this tutorial goes in a new, empty directory:

```sh
cd "$(mktemp -d)"
```

## Step 1: lists of atoms

A grammar names [tokens](../reference/grammar.md#tokens), which match bytes,
and [rules](../reference/grammar.md#rules), which arrange tokens and other
rules. Save this grammar as `sexpr.scm`:

```scheme
(grammar sexpr
  start: Source

  (token Atom (regex "[^() \t\r\n;]+"))

  (rule Source
    (seq (repeat Form) EOF))

  (rule Form
    (choice Atom List))

  (rule List
    (seq "(" (repeat Form) ")")))
```

`Source` is the start rule: any number of `Form`s
([`repeat`](../reference/grammar.md#repeat) inside a
[`seq`](../reference/grammar.md#seq)), then the end of the input
([`EOF`](../reference/grammar.md#eof)). A `Form` is an `Atom` or a `List`
([`choice`](../reference/grammar.md#choice)), and a `List` holds more `Form`s, so lists
nest as deep as the input does.

Check that the grammar compiles:

```sh
tinyparse check sexpr.scm
```

`tinyparse check` prints nothing and exits with status 0 when the grammar
compiles. Parse a one-element list and print its tree:

```sh
printf '(a)' | tinyparse run sexpr.scm --input - --dump-tree
```

`--input -` reads the input from standard input. The tree:

```text
Source [0-3] [0,0]-[0,3]
  Form [0-3] [0,0]-[0,3]
    List [0-3] [0,0]-[0,3]
      ( [0-1] [0,0]-[0,1]
      Form [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      ) [2-3] [0,2]-[0,3]
```

Each line is one node of the concrete syntax tree (CST), indented under its
parent, in the [tree dump format](../../tinyparse.1.md#tree-dump). It shows the node kind, the byte range `[start-end]` (0-based, end
excluded), and the start and end points as `[row,column]`, both 0-based. A
literal such as `"("` in a rule makes a node whose kind is the literal itself.
The dump does not print a token's text; its byte range locates it in the input.

Give the list two atoms:

```sh
printf '(a b)' | tinyparse run sexpr.scm --input - --dump-tree
```

The parse fails, because no token matches the space between `a` and `b`:

```text
error: unexpected input [parse.syntax]
```

The command prints no tree and exits with status 1.

## Step 2: whitespace as trivia

Add a `Space` token and list it under [`extras`](../reference/grammar.md#extras).
An extra may appear between any two tokens without a rule naming it, and
`trivia` keeps it in the tree. Replace
`sexpr.scm` with:

```scheme
(grammar sexpr
  start: Source

  (token Space (regex "[ \t\r\n]+"))
  (token Atom  (regex "[^() \t\r\n;]+"))

  (extras
    (trivia Space))

  (rule Source
    (seq (repeat Form) EOF))

  (rule Form
    (choice Atom List))

  (rule List
    (seq "(" (repeat Form) ")")))
```

Run the same input again:

```sh
printf '(a b)' | tinyparse run sexpr.scm --input - --dump-tree
```

The space is a `Space` node between the two `Form`s:

```text
Source [0-5] [0,0]-[0,5]
  Form [0-5] [0,0]-[0,5]
    List [0-5] [0,0]-[0,5]
      ( [0-1] [0,0]-[0,1]
      Form [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      Space [2-3] [0,2]-[0,3]
      Form [3-4] [0,3]-[0,4]
        Atom [3-4] [0,3]-[0,4]
      ) [4-5] [0,4]-[0,5]
```

## Step 3: name the parentheses with fields

A [field](../reference/grammar.md#field) gives a child a name in its parent, so a program that reads the tree
can ask for the opening parenthesis by name. Change the `List` rule so that
`sexpr.scm` reads:

```scheme
(grammar sexpr
  start: Source

  (token Space (regex "[ \t\r\n]+"))
  (token Atom  (regex "[^() \t\r\n;]+"))

  (extras
    (trivia Space))

  (rule Source
    (seq (repeat Form) EOF))

  (rule Form
    (choice Atom List))

  (rule List
    (seq
      (field open: "(")
      (repeat Form)
      (field close: ")"))))
```

Parse a nested list:

```sh
printf '(a (b c))' | tinyparse run sexpr.scm --input - --dump-tree
```

A node bound to a field prints as `field: kind`:

```text
Source [0-9] [0,0]-[0,9]
  Form [0-9] [0,0]-[0,9]
    List [0-9] [0,0]-[0,9]
      open: ( [0-1] [0,0]-[0,1]
      Form [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      Space [2-3] [0,2]-[0,3]
      Form [3-8] [0,3]-[0,8]
        List [3-8] [0,3]-[0,8]
          open: ( [3-4] [0,3]-[0,4]
          Form [4-5] [0,4]-[0,5]
            Atom [4-5] [0,4]-[0,5]
          Space [5-6] [0,5]-[0,6]
          Form [6-7] [0,6]-[0,7]
            Atom [6-7] [0,6]-[0,7]
          close: ) [7-8] [0,7]-[0,8]
      close: ) [8-9] [0,8]-[0,9]
```

## Step 4: comments

A comment runs from `;` to the end of the line. Add a `Comment` token and make
it trivia too. [`(not "\n" any)`](../reference/grammar.md#not) matches any one byte other than
a newline.
Replace `sexpr.scm` with:

```scheme
(grammar sexpr
  start: Source

  (token Space   (regex "[ \t\r\n]+"))
  (token Comment (seq ";" (repeat (not "\n" any))))
  (token Atom    (regex "[^() \t\r\n;]+"))

  (extras
    (trivia Space)
    (trivia Comment))

  (rule Source
    (seq (repeat Form) EOF))

  (rule Form
    (choice Atom List))

  (rule List
    (seq
      (field open: "(")
      (repeat Form)
      (field close: ")"))))
```

Parse two lines, a comment followed by a list:

```sh
printf '; greeting\n(hello world)\n' | tinyparse run sexpr.scm --input - --dump-tree
```

The comment is a `Comment` node, and the list starts on row 1:

```text
Source [0-25] [0,0]-[2,0]
  Comment [0-10] [0,0]-[0,10]
  Space [10-11] [0,10]-[1,0]
  Form [11-24] [1,0]-[1,13]
    List [11-24] [1,0]-[1,13]
      open: ( [11-12] [1,0]-[1,1]
      Form [12-17] [1,1]-[1,6]
        Atom [12-17] [1,1]-[1,6]
      Space [17-18] [1,6]-[1,7]
      Form [18-23] [1,7]-[1,12]
        Atom [18-23] [1,7]-[1,12]
      close: ) [23-24] [1,12]-[1,13]
  Space [24-25] [1,13]-[2,0]
```

`examples/sexpr/sexpr.scm` holds the same grammar, with a version header and a module name added.

## Step 5: a syntax error

Leave the outer list open:

```sh
printf '(a (b c)' | tinyparse run sexpr.scm --input - --dump-tree
```

The input ends where the grammar still expects `)`:

```text
error: unexpected input [parse.syntax]
```

`parse.syntax` is the [diagnostic code](../reference/diagnostics.md#parsesyntax). Print the exit status:

```sh
echo $?
```

```text
1
```

## Step 6: recover from an unclosed list

A [`recover`](../reference/grammar.md#recover) form wraps a rule body and says what to do when the input ends
before the body is complete. Replace the `List` rule so that `sexpr.scm` reads:

```scheme
(grammar sexpr
  start: Source

  (token Space   (regex "[ \t\r\n]+"))
  (token Comment (seq ";" (repeat (not "\n" any))))
  (token Atom    (regex "[^() \t\r\n;]+"))

  (extras
    (trivia Space)
    (trivia Comment))

  (rule Source
    (seq (repeat Form) EOF))

  (rule Form
    (choice Atom List))

  (rule List
    (recover
      (seq
        (field open: "(")
        (repeat Form)
        (field close: ")"))
      (on eof
        message: "unterminated list"
        (label here: "expected `)` before end of file")
        help: "add a closing `)`"
        insert: ")"))))
```

Parse the same broken input:

```sh
printf '(a (b c)' | tinyparse run sexpr.scm --input - --dump-tree
```

The parser inserts a zero-width `)` node marked `MISSING`, so the tree is
complete. The tree goes to standard output and the diagnostic, with its help
line, to standard error; on a terminal they appear in this order:

```text
Source [0-8] [0,0]-[0,8]
  Form [0-8] [0,0]-[0,8]
    List [0-8] [0,0]-[0,8]
      open: ( [0-1] [0,0]-[0,1]
      Form [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      Space [2-3] [0,2]-[0,3]
      Form [3-8] [0,3]-[0,8]
        List [3-8] [0,3]-[0,8]
          open: ( [3-4] [0,3]-[0,4]
          Form [4-5] [0,4]-[0,5]
            Atom [4-5] [0,4]-[0,5]
          Space [5-6] [0,5]-[0,6]
          Form [6-7] [0,6]-[0,7]
            Atom [6-7] [0,6]-[0,7]
          close: ) [7-8] [0,7]-[0,8]
      MISSING ) [8-8] [0,8]-[0,8]
unterminated list [parse.recover]
  help: add a closing `)`
```

The `label` text is part of the diagnostic, but the command does not print it;
a C program reads it, as the next tutorial shows. Print the exit status:

```sh
echo $?
```

```text
1
```

The parse built a tree, and the command still exits with status 1 because the
diagnostic is an error.

## Where to go next

- [Parse input from a C program](embed-in-c.md) runs this grammar from your own
  C code and walks the tree.
- [Grammar language](../reference/grammar.md) lists every form used here and the
  ones this tutorial skips.
- [How tinyparse shapes the tree](../explanation/tree-shape.md) explains where
  trivia, fields and `MISSING` nodes go.
- [Examples](../../examples/README.md) has grammars for larger languages.
