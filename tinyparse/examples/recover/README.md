# recover: keep a tree when a list is left open

`list.scm` parses parenthesized lists of atoms, and its `List` rule wraps its
body in a `recover` form. When the input ends before a list's closing `)`, the
parser inserts a zero-width `MISSING` `)` node, records a `parse.recover`
diagnostic, and still builds a tree. The example uses the `recover` form from
the [grammar language reference](../../docs/reference/grammar.md) and the
diagnostic fields listed in the [diagnostics reference](../../docs/reference/diagnostics.md).

## Prerequisites

Run the commands from the `tinyparse/` directory with `tinyparse` on your
`PATH`, as [Examples](../README.md) describes.

## Run it

Parse a list whose outer `)` is missing:

```sh
printf '(a (b c)' | tinyparse run examples/recover/list.scm --input - --dump-tree
```

The tree goes to standard output and ends with the inserted `MISSING )` node.
The diagnostic and its help line go to standard error; on a terminal they
appear after the tree:

```text
Source [0-8] [0,0]-[0,8]
  Item [0-8] [0,0]-[0,8]
    List [0-8] [0,0]-[0,8]
      open: ( [0-1] [0,0]-[0,1]
      Item [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      Space [2-3] [0,2]-[0,3]
      Item [3-8] [0,3]-[0,8]
        List [3-8] [0,3]-[0,8]
          open: ( [3-4] [0,3]-[0,4]
          Item [4-5] [0,4]-[0,5]
            Atom [4-5] [0,4]-[0,5]
          Space [5-6] [0,5]-[0,6]
          Item [6-7] [0,6]-[0,7]
            Atom [6-7] [0,6]-[0,7]
          close: ) [7-8] [0,7]-[0,8]
      MISSING ) [8-8] [0,8]-[0,8]
unterminated list [parse.recover]
  help: add a closing `)`
```

The diagnostic has error severity, so the command exits with status 1 although
it printed a tree:

```sh
echo $?
```

```text
1
```

Parse an input from `broken/` that leaves two lists open:

```sh
tinyparse run examples/recover/list.scm --input examples/recover/broken/05-two-open.list --dump-tree
```

Each open list gets its own `MISSING )` node. The diagnostic is printed three
times (see Limits):

```text
Source [0-5] [0,0]-[0,5]
  Item [0-5] [0,0]-[0,5]
    List [0-5] [0,0]-[0,5]
      open: ( [0-1] [0,0]-[0,1]
      Item [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      Space [2-3] [0,2]-[0,3]
      Item [3-5] [0,3]-[0,5]
        List [3-5] [0,3]-[0,5]
          open: ( [3-4] [0,3]-[0,4]
          Item [4-5] [0,4]-[0,5]
            Atom [4-5] [0,4]-[0,5]
          MISSING ) [5-5] [0,5]-[0,5]
      MISSING ) [5-5] [0,5]-[0,5]
unterminated list [parse.recover]
  help: add a closing `)`
unterminated list [parse.recover]
  help: add a closing `)`
unterminated list [parse.recover]
  help: add a closing `)`
```

## How it works

The `List` rule is the only one that differs from a grammar without recovery:

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

The parser first tries the `seq` as written. When the input ends while that
`seq` still expects a token, the `(on eof ...)` clause applies:

- `insert: ")"` adds a zero-width node of kind `)` at the end of the input,
  flagged as missing. The dump prints it as `MISSING )`, and a C program reads
  the flag with `tinyparse_tree_cursor_flags` and `TINYPARSE_NODE_MISSING`.
- `message:` and `help:` become the diagnostic's message and help text, with
  the code `parse.recover`.
- `(label here: "...")` becomes the diagnostic's one label, a zero-width byte
  range at the insertion point. The `tinyparse` command prints the message, code
  and help text but no labels.
  [Parse input from a C program](../../docs/tutorials/embed-in-c.md) prints the
  label from C.

[Recover from and report syntax errors](../../docs/how-to/handle-errors.md)
adds skipping stray bytes and reporting each diagnostic with its line and
column from C.

## Limits

- `eof` is the only trigger. `tinyparse check` rejects any other with
  `error: recover clause must be (on eof ...)`. Input that is wrong before its
  end gets no recovery: `(a))` prints `error: unexpected input [parse.syntax]`,
  no tree, and exits with status 1.
- `recover` must be the whole body of a rule. Placed inside a `seq`, it fails
  `tinyparse check` with `error: unknown pattern combinator`.
- With more than one list open, the diagnostic repeats: two open lists print it
  3 times and three open lists print it 7 times, while the tree holds one
  `MISSING )` per list.
- The command exits with status 1 whenever recovery fires, so a script cannot
  tell a recovered parse from a failed one by the exit status alone; a failed
  parse prints no tree.

## Files

| File | Contents |
|------|----------|
| `list.scm` | the grammar |
| `broken/` | eight inputs that each end with at least one list open, from `(` (`01-open-eof.list`) to `(((` (`08-triple-open.list`) |
