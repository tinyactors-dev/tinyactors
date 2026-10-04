# How tinyparse shapes the tree

This explanation is for grammar authors who want to predict the tree a grammar
builds, and for host authors who want to work from that tree. It covers why the
tree keeps every token, which grammar forms create nodes, why hidden rules
exist, where comments attach, and why a host reads the tree directly instead of
converting it into a structure of its own.

The trees below are printed by `tinyparse run GRAMMAR --input FILE --dump-tree`.
Each line is a node: its field (if any), its kind, its byte range `[start-end]`
and its start and end points `[row,column]`. The format is described in
[tinyparse(1)](../../tinyparse.1.md).

## A concrete tree

The parser builds a concrete syntax tree (CST): a tree that keeps the tokens of
the input, punctuation and trivia included, as leaves in input order. For the
grammar in `examples/sexpr/sexpr.scm` and the input `(a ; note`, a line feed,
` b)` and a line feed, the tree is:

```text
Source [0-14] [0,0]-[2,0]
  Form [0-13] [0,0]-[1,3]
    List [0-13] [0,0]-[1,3]
      open: ( [0-1] [0,0]-[0,1]
      Form [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      Space [2-3] [0,2]-[0,3]
      Comment [3-9] [0,3]-[0,9]
      Space [9-11] [0,9]-[1,1]
      Form [11-12] [1,1]-[1,2]
        Atom [11-12] [1,1]-[1,2]
      close: ) [12-13] [1,2]-[1,3]
  Space [13-14] [1,3]-[2,0]
```

The parentheses, the comment and every run of whitespace are leaves. Reading
the leaves in order gives back the input byte for byte. An abstract syntax tree
would drop all of them, and a program that formats, rewrites or highlights
source would then have to recover them from the text. A CST costs more nodes,
and a host that wants only the abstract shape skips the leaf kinds it does not
need.

Two forms put bytes inside a parent's range that no leaf covers: a `skip` entry
in [`extras`](../reference/grammar.md#extras), which consumes bytes and
discards them, and an [inline token
expression](../reference/grammar.md#inline-token-expressions) such as `(regex
…)` written directly in a rule. An `error` extra keeps its bytes as a leaf and
raises a diagnostic. A grammar that wants every byte back declares its
whitespace and comments as `trivia` and names its tokens.

When a grammar recovers from missing input with
[`recover`](../reference/grammar.md#recover), the parser inserts a MISSING
node: a zero-width node of the kind the grammar names, flagged
`TINYPARSE_NODE_MISSING`
([`tinyparse_tree_cursor_flags`](../reference/c-api.md#tinyparse_tree_cursor_flags)).
The tree keeps its shape, so a host walks a repaired tree with the same code as
a clean one and checks the flag where it cares.

Positions in the tree are byte offsets. The library returns no rows or columns:
`tinyparse_point` is declared in the header and no function returns it. The
command-line tool computes the `[row,column]` pairs above from the bytes. Bytes
are what the parser consumed, while a column depends on a policy the host owns:
an editor speaking the Language Server Protocol counts UTF-16 code units, a
terminal counts display cells.

## Which forms create nodes

Most nodes come from three pattern forms, and the combinators add none:

| Form | Node |
|------|------|
| A rule call | One node whose kind is the rule name, holding what the rule's pattern produced |
| A token reference | One leaf whose kind is the token name |
| A literal such as `"("` | One leaf whose kind is the literal text |
| `seq`, `choice`, `repeat`, `optional` | None; their parts' nodes go to the enclosing rule |

Four other sources add nodes. An [`inject`](../reference/grammar.md#inject)
form produces an `injection` node holding the other grammar's tree.
[`parse-deferred`](../reference/grammar.md#deferred-slot-defer-parse-deferred)
produces one node of its rule per queued item. Extras declared as `trivia`
become leaves wherever the parser consumes them, as the `Space` and `Comment`
leaves above show. [`recover`](../reference/grammar.md#recover) inserts MISSING
nodes.

The rule is where a grammar author names a thing in the language, so the rule is
where the tree gets a node. A combinator describes how to match, which a host
does not need to see: a `(repeat Form)` inside `List` leaves the `Form` nodes as
direct children of `List`, with no list node between them. A group that a host
does need as one node becomes a rule. The exact effect of each form is in the
[grammar reference](../reference/grammar.md#rules).

## Fields and aliases

A child's position among its siblings is a poor interface. Trivia, optional
parts and recovered nodes all shift positions, and a grammar change that adds an
optional keyword renumbers every child after it. A field names a child's role
instead. In std.javascript, `a + b;` gives:

```text
program [0-7] [0,0]-[1,0]
  expression_statement [0-6] [0,0]-[0,6]
    additive_expression [0-5] [0,0]-[0,5]
      left: identifier [0-1] [0,0]-[0,1]
      Space [1-2] [0,1]-[0,2]
      operator: + [2-3] [0,2]-[0,3]
      Space [3-4] [0,3]-[0,4]
      right: identifier [4-5] [0,4]-[0,5]
    semi [5-6] [0,5]-[0,6]
      ; [5-6] [0,5]-[0,6]
  Space [6-7] [0,6]-[1,0]
```

A host that wants the right operand moves to the child in field `right`
([`tinyparse_tree_cursor_goto_first_child_by_field`](../reference/c-api.md#moving-the-cursor))
and does not depend on the whitespace around the operator. The fields and kinds
a grammar can produce are the contract between grammar and host; `tinyparse
node-types` prints them as JSON.

An [alias](../reference/grammar.md#alias) changes the kind of the node it wraps.
Two rules that match differently can then present one kind to the host, and a
rule that exists only to structure matching can show the name the language uses.

## Hidden rules

A parsing expression grammar (PEG) expresses operator precedence as a ladder of
rules, one per level, where each level calls the next. Ordered choice makes a
naive ladder re-parse the same operand once per alternative, so the standard
grammars left-factor it: each level parses its operand once and then an
optional operator tail. ([`docs/peg-vs-glr.md`](../peg-vs-glr.md) records the
measurements behind that choice.) The factored ladder has a side effect on the
tree. When no operator follows, each level still calls the next and still opens
a node with one child.

The effect is visible on the simplest statement. Here is `x;` in a copy of
std.javascript with every `(hidden …)` wrapper removed:

```text
program [0-3] [0,0]-[1,0]
  statement [0-2] [0,0]-[0,2]
    expression_statement [0-2] [0,0]-[0,2]
      expression_sequence [0-1] [0,0]-[0,1]
        expression: expression [0-1] [0,0]-[0,1]
          assignment_expression [0-1] [0,0]-[0,1]
            left: ternary_expression [0-1] [0,0]-[0,1]
              condition: nullish_expression [0-1] [0,0]-[0,1]
                left: logical_or_expression [0-1] [0,0]-[0,1]
                  left: logical_and_expression [0-1] [0,0]-[0,1]
                    left: bitwise_or_expression [0-1] [0,0]-[0,1]
                      left: bitwise_xor_expression [0-1] [0,0]-[0,1]
                        left: bitwise_and_expression [0-1] [0,0]-[0,1]
                          left: equality_expression [0-1] [0,0]-[0,1]
                            left: relational_expression [0-1] [0,0]-[0,1]
                              left: shift_expression [0-1] [0,0]-[0,1]
                                left: additive_expression [0-1] [0,0]-[0,1]
                                  left: multiplicative_expression [0-1] [0,0]-[0,1]
                                    left: exponent_expression [0-1] [0,0]-[0,1]
                                      left: unary_expression [0-1] [0,0]-[0,1]
                                        update_expression [0-1] [0,0]-[0,1]
                                          argument: postfix_expression [0-1] [0,0]-[0,1]
                                            expr: member_or_call [0-1] [0,0]-[0,1]
                                              head: call_head [0-1] [0,0]-[0,1]
                                                primary_expression [0-1] [0,0]-[0,1]
                                                  identifier [0-1] [0,0]-[0,1]
      semi [1-2] [0,1]-[0,2]
        ; [1-2] [0,1]-[0,2]
  Space [2-3] [0,2]-[1,0]
```

The identifier is 25 levels below the root, under 22 nodes that each cover the
same single byte. With the grammar as shipped, the same input gives:

```text
program [0-3] [0,0]-[1,0]
  expression_statement [0-2] [0,0]-[0,2]
    identifier [0-1] [0,0]-[0,1]
    semi [1-2] [0,1]-[0,2]
      ; [1-2] [0,1]-[0,2]
  Space [2-3] [0,2]-[1,0]
```

A [hidden rule](../reference/grammar.md#hidden-rules) leaves its node out of the
tree when the call produced exactly one child and carried no alias. The child
takes the hidden node's place and the field of the hidden rule's call site.
Without hidden rules, about 57% of the nodes in a census of 1,070 React source
files are such single-child wrappers ([`docs/decisions.md`](../decisions.md)
has the census).

The collapse is conditional because a factored ladder rule does two jobs. With
no operator, `additive_expression` is a wrapper around its operand. With one, as
in `a + b;` above, it is the binary expression itself, holding `left:`,
`operator:` and `right:`. A hidden rule keeps its node whenever it produced more
than one child, so the operator case keeps its node and its name.

tree-sitter takes a different route to a similar tree. A tree-sitter grammar
marks a rule hidden by starting its name with an underscore, and a hidden rule
is always inlined. That works because a tree-sitter grammar puts each binary
operator in its own rule with a precedence annotation, so the wrapper rules
never carry an operator. In a PEG, the operator and its operands live in the
same rule as the precedence level, and the rule has to keep its node whenever it
matched one. The `(hidden …)` marker hides the node only in the case where it
added nothing.

## Where trivia attaches

Extras are consumed between grammar elements, so each run of trivia lands in
whichever node is being built when the parser consumes it. That default,
`attach: trailing`, puts a comment in the node that happens to be open. It goes
wrong for a doc comment written after a statement whose terminator is optional.
In this input the first statement has no semicolon:

```text
f(a)
/** b */
let b = 2;
```

Before deciding whether the first statement ends, the parser consumes the
whitespace and the comment, looking for a `;`. With the comments of
std.javascript set to `attach: trailing`, the comment becomes part of the call
statement, and that statement's range grows to byte 14. The children of
`member_or_call` and of `lexical_declaration` are cut:

```text
program [0-25] [0,0]-[3,0]
  expression_statement [0-14] [0,0]-[2,0]
    member_or_call [0-4] [0,0]-[0,4]
      ...
    Space [4-5] [0,4]-[1,0]
    block_comment [5-13] [1,0]-[1,8]
    Space [13-14] [1,8]-[2,0]
    semi [14-14] [2,0]-[2,0]
  lexical_declaration [14-24] [2,0]-[2,10]
    ...
  Space [24-25] [2,10]-[3,0]
```

With `attach: leading`, the comment and the trivia after it are held and placed
at the next point where the parser consumes extras, here in `program` ahead of
the declaration the comment documents. The statement before it ends at its last
real token. std.javascript marks its three comment kinds `attach: leading`, and
std.typescript and std.tsx inherit that. Whitespace stays `trailing`, because
nothing queries where whitespace attaches and holding it costs work at every
token. The shipped grammar gives the tree below; the children of
`member_or_call` and of `lexical_declaration` are cut:

```text
program [0-25] [0,0]-[3,0]
  expression_statement [0-5] [0,0]-[1,0]
    member_or_call [0-4] [0,0]-[0,4]
      ...
    Space [4-5] [0,4]-[1,0]
    semi [5-5] [1,0]-[1,0]
  block_comment [5-13] [1,0]-[1,8]
  Space [13-14] [1,8]-[2,0]
  lexical_declaration [14-24] [2,0]-[2,10]
    ...
  Space [24-25] [2,10]-[3,0]
```

Held trivia goes to the next consumption point, which is not always a sibling.
When that point is the entry of the next rule, the comment becomes that rule's
first child; when it is a terminal inside a rule that then closes, the comment
lands after that rule's node, out of byte order. The [grammar
reference](../reference/grammar.md#extras) states the rule exactly.

## Reading the tree instead of building an AST

The usual first step after parsing is to convert the parse tree into an abstract
syntax tree of the program's own structs. With tinyparse a host reads the CST
directly, because the CST already holds what such structs would hold: a kind
for each node, a field for each role and a byte range for each node and token.
A parallel AST would restate the grammar in C, would have to change with every
grammar change, and would drop the byte ranges that rewriting and error
reporting need.

The examples show the three common uses:

- `examples/jit` generates machine code by recursing over the cursor, reading
  kinds and fields.
- `examples/uri` reads the components of a URI from the fields of its tree.
- `examples/rewriter` produces new source from the old: it copies the bytes of
  every node it does not change through the node's byte range and writes new
  text for the nodes it does.

The tree is read-only. Rewriting is text emission: copy unchanged spans, write
replacements, and parse a fragment again when the program needs a tree for new
text. The design explanation gives the reasoning for having no edit API; see
[Design of tinyparse](design.md).

## How the tree is stored

A tree is one contiguous array of node records, 40 bytes each. A record holds
ten 32-bit fields: the kind, the field, the start and end byte, the indices of
the parent, the first child and the next sibling, the child count, the flags,
and a language tag used by injected regions. Nodes are appended in the order the
parse opens them, so a parent precedes its children and a depth-first walk moves
mostly forward through memory. A cursor is a position in that array: moving to
a child, a sibling or the parent reads one record and stores one index.
[`tinyparse_tree_cursor_init`](../reference/c-api.md#tinyparse_tree_cursor_init)
puts a cursor in storage the host provides, so walking a tree allocates nothing.

The tree lives in the parser's memory and is valid until
`tinyparse_parser_free`. A hidden rule whose body yields exactly one child on
every path writes no record at all; the child goes straight into the caller.
A hidden rule that collapses only sometimes, such as a level of a precedence
ladder, writes its record before it knows how many children it gets. When it
collapses, that record stays in the array with nothing pointing to it, and the
cursor never reaches it. The same holds for any hidden rule while the rule memo
is recording, because the memo stores the rule's node. The walk speed this
layout gives, measured against tree-sitter, is in [Performance](performance.md).
