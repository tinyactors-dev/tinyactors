# Query language

This reference describes the pattern language that `tinyparse query` and
`tinyparse_query` accept: how a pattern matches nodes of a concrete syntax tree
(CST), what captures and predicates do, and which query texts are rejected. For
a task-oriented introduction, see [Find nodes with a query](../how-to/query.md).

Commands on this page assume the current directory is the `tinyparse/`
directory of a source checkout and that `tinyparse` is on `PATH`
([Install tinyparse](../how-to/install.md)). The examples query the tree that
[`examples/sexpr/sexpr.scm`](../../examples/sexpr/sexpr.scm) builds for one
line of input:

```sh
printf '%s\n' '(a (b) c)' > input.txt
```

## What a query matches against

A query runs over the tree flattened in pre-order into a stream of three token
kinds:

| Token | Emitted for |
|-------|-------------|
| enter | a node with children, before its children |
| leaf | a node with no children: a token, a literal, a trivia node, a MISSING node, or a rule node without child nodes |
| exit | a node with children, after its children |

Each enter and leaf token carries the node's kind and its field, if any.
`tinyparse linearize` prints this stream; the format is under
[Linearized tree in tinyparse(1)](../../tinyparse.1.md#linearized-tree), and it prints kinds and leaf text but not
fields. For the input above:

```sh
tinyparse linearize examples/sexpr/sexpr.scm --input input.txt
```

Each `Space` leaf prints its text after the kind: a single space twice, and
finally a line feed, which shows as an empty line:

```text
enter Source
enter Form
enter List
leaf ( (
enter Form
leaf Atom a
exit Form
leaf Space  
enter Form
enter List
leaf ( (
enter Form
leaf Atom b
exit Form
leaf ) )
exit List
exit Form
leaf Space  
enter Form
leaf Atom c
exit Form
leaf ) )
exit List
exit Form
leaf Space 

exit Source
```

Trivia nodes are part of the stream. They count as siblings for the
[anchor](#anchor).

## Query text

A query is read with the same reader as a grammar
([Lexical syntax](grammar.md#lexical-syntax)), so `;` starts a comment. It is a
sequence of top-level forms. Each form is either a pattern or a pattern with
predicates:

```text
form        = pattern
            | "(" pattern predicate { predicate } ")"
pattern     = "(" head { child } [ "." ] ")"
head        = KIND | "_"
child       = [ "." ] [ FIELD ":" ] ( pattern | "_" ) [ "@" NAME ]
predicate   = "(" ( "#eq?" | "#match?" ) "@" NAME STRING ")"
```

`((pattern))`, a wrapping list with no predicate, is also accepted and behaves
as the bare pattern.

The forms are compiled and run one at a time, in order. A form is tried at
every node of the tree, in pre-order, and each successful match reports its
captures before the next node is tried.

## Patterns

### Node pattern

- **Syntax:** `(KIND CHILD…)`
- **Matches:** a node of kind `KIND`. With no child patterns it matches a leaf
  or a node with children; with child patterns it matches only a node with
  children.
- **Arguments:** `KIND` must be a name the grammar defines; any other name
  rejects the query. See [Names](#names).

### Wildcard

- **Syntax:** `(_ CHILD…)` as a pattern, or a bare `_` as a child
- **Matches:** a node of any kind. A bare `_` child has no child patterns.

### Child patterns

- **Syntax:** the patterns after the head
- **Matches:** direct children of the matched node, in order. Before each child
  pattern, any number of siblings may be skipped. A child pattern matches only
  a direct child, never a deeper descendant; to reach a descendant, nest
  patterns.
- **Notes:** each child pattern takes the first sibling it matches. When a
  later child pattern then fails, the match fails; earlier choices are not
  revised.

```sh
printf '%s\n' '(List (Form) @first)' > query.scm
tinyparse query examples/sexpr/sexpr.scm query.scm --input input.txt
```

Each `List` matches once, and the capture is that list's first `Form`:

```text
@first 1-2 a
@first 4-5 b
```

`(List (Atom) @atom)` reports nothing, because no `Atom` is a direct child of a
`List`. `(Source (Form (List (Form (List (Form (Atom) @deep))))))` reports
`@deep 4-5 b`.

### Field

- **Syntax:** `FIELD: CHILD`, before a child pattern
- **Matches:** the child only when the child node carries the field `FIELD`.
- **Arguments:** `FIELD` must be a name the grammar defines; any other name
  rejects the query. See [Names](#names).

`(List open: (_) @open (Form) @item)` reports:

```text
@open 0-1 (
@item 1-2 a
@open 3-4 (
@item 4-5 b
```

### Names

The check on `KIND` and `FIELD` accepts every name the grammar defines, in
either position: rule, token and field names, literal texts, and lexical goal
names. A name that is no node kind, or no field, compiles and matches nothing.
`(List (open) @o)` and `(List Form: (_) @x)` report nothing and exit with status
0.

A literal kind whose text contains `(`, `)`, `;`, `"` or whitespace cannot be
written in a query: the reader splits it, and `(List ("(") @o)` is rejected.
Match such a node with `_` and its field, as `open: (_)` does under
[Field](#field).

### Capture

- **Syntax:** `@NAME`, directly after a child pattern
- **Effect:** records the byte range of the node that child pattern matched,
  under `NAME`. A capture must follow a child; it cannot follow the head of a
  pattern or stand at the top level, so `(_ @any)` and `(Form) @f` are
  rejected. To capture a node of any kind, wrap it in a parent pattern:
  `(Form (_) @any)`.
- **Notes:** a pattern without captures can match and reports nothing. The same
  name may be used more than once in a pattern, and every use is reported.

### Anchor

- **Syntax:** `.`, before a child pattern
- **Matches:** the child pattern only against the sibling directly after the
  previous child's match, or against the first child when the anchored pattern
  is the first. A `.` after the last child pattern has no effect.
- **Notes:** trivia nodes are siblings, so whitespace between two nodes breaks
  an anchor between their patterns.

`(List open: (_) . (Form) @first)` reports `@first 1-2 a` and `@first 4-5 b`.
`(List (Form) @x . (Form) @y)` reports nothing for this input, because a
`Space` node separates every pair of `Form` siblings.

## Predicates

A predicate filters a whole match. It is written after the pattern inside a
wrapping list, and every predicate must hold for the match to be reported.

| Predicate | Holds when |
|-----------|------------|
| `(#eq? @NAME "text")` | the bytes captured as `NAME` equal `text` |
| `(#match? @NAME "regex")` | the regex matches the captured bytes starting at some offset |

`#match?` uses the regex dialect of the grammar language
([Regex dialect](grammar.md#regex-dialect)). The dialect has no anchors: `^` and
`$` are literal bytes. A predicate on a name the match did not capture does not
hold. When a name is captured more than once, the predicate tests the first
binding.

```sh
printf '%s\n' '((Form (Atom) @atom) (#match? @atom "[ac]"))' > query.scm
tinyparse query examples/sexpr/sexpr.scm query.scm --input input.txt
```

```text
@atom 1-2 a
@atom 7-8 c
```

## Results

Each match reports its captures in the order they were recorded. A capture is
recorded when its node has matched, so a capture inside a nested pattern comes
before the capture of the node that contains it:
`(List (Form (List) @inner) @outer)` reports `@inner 3-6 (b)` and then
`@outer 3-6 (b)`. Matches are reported in pre-order of the node they start at.

The line format of `tinyparse query` is under
[Query captures in tinyparse(1)](../../tinyparse.1.md#query-captures).

[`tinyparse_query`](c-api.md#tinyparse_query) reports the same captures, in
the same order, through its callback; its parameters and result codes are
listed there.

## Errors

A malformed form stops the query at that form. Captures of earlier forms have
already been reported. A reader error rejects the whole query text before any
form runs, so it reports no captures. What `tinyparse_query` returns is under
[`tinyparse_query`](c-api.md#tinyparse_query). What the command prints is
under [query in tinyparse(1)](../../tinyparse.1.md#query).

These shapes are rejected:

| Shape | Example |
|-------|---------|
| A kind or field name the grammar does not define | `(Nope (Atom) @a)`, `(List nofield: (_) @a)` |
| A capture with no child before it | `(_ @any)`, `(List @a)` |
| A capture or anchor at the top level | `(Form) @f` |
| A pattern that does not begin with a kind or `_` | `(@a)`, `(.)` |
| A child that is a bare kind name | `(Form Atom)` |
| A kind written as a string | `(List ("(") @o)` |
| A field keyword with nothing after it | `(List open:)` |
| A predicate other than `#eq?` and `#match?` | `((List (Form) @a) (#not-eq? @a "x"))` |
| A predicate without exactly a capture and a string | `((List (Form) @a) (#eq? @a))` |
| A list beside a pattern that is not a predicate | `((List (Form) @a) (Form))` |
| A reader error | `(List (Form) @a` |
