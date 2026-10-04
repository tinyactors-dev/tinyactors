# tinyparse grammar reference (`.scm`)

The native grammar surface is `.scm`: Scheme-shaped *data*, not executable
Scheme. This page documents the core forms; the rest of the surface is in
[`../tinyparse-design.md`](../tinyparse-design.md), listed at the
[end of this page](#forms-documented-in-the-design). Every form here is exercised by a grammar
under [`../examples/`](../examples/); [`../examples/sexpr/sexpr.scm`](../examples/sexpr/sexpr.scm)
is the smallest complete example.

## Reader

The reader accepts: lists `( … )`, symbols, `keyword:` symbols (a trailing
colon), strings `"…"` with C-style escapes (`\n`, `\t`, `\"`, `\\`, `\xHH`),
decimal integers, `#xHH` byte/hex literals, `;` line comments, and whitespace.
An unterminated list or string is a reader error.

## Document forms

```scheme
(tinyparse version: "0.1")     ; optional header
(module a.b.c)                   ; optional module name

(grammar NAME
  start: RuleName                ; the entry rule
  (token …) (extras …) (rule …) …)
```

## Tokens

A `token` names a terminal. Its body is a token expression:

```scheme
(token Plus  "+")                                     ; a literal
(token Atom  (regex "[^() \t\r\n;]+"))                ; a regex
(token Str   (seq "\"" (repeat (not "\"" any)) "\"")) ; a lexical machine
```

Token/lexical expressions: literal strings, `(regex "…")`, `(seq …)`,
`(repeat X)`, `(choice …)`, `(not X any)` (any byte not matching `X`), and
`any` (any one byte). The matcher is greedy.

### Regex subset

Character classes `[...]` with ranges (`a-z`) and negation (`[^...]`), the
quantifiers `+` and `*`, the wildcard `.`, and literal characters. No
backreferences, no lookaround, no locale — the engine owns the matcher.

## Rules

A `rule` names a non-terminal. Its body is a rule expression:

| Form                         | Meaning                                            |
|------------------------------|----------------------------------------------------|
| `RuleName` / `TokenName`     | reference another rule or token                    |
| `"literal"`                  | an inline literal terminal                         |
| `(seq A B …)`                | A then B then …                                     |
| `(choice A B …)`             | the first alternative that matches                 |
| `(repeat X)`                 | zero or more `X` (`min:`/`max:` bound it)          |
| `(repeat X min: 1)`          | one or more                                        |
| `(optional X)`               | zero or one                                        |
| `(field name: X)`            | match `X` and tag the child with the field `name`   |
| `(alias public: X)`          | match `X` but expose it under `public`             |
| `EOF`                        | end of input                                       |

```scheme
(rule List
  (seq (field open: "(") (repeat Form) (field close: ")")))
```

A field name is readable from a cursor via `tinyparse_tree_cursor_field`;
the CLI tree dump prints it.

## Hidden rules

A rule wrapped in `(hidden …)` is tinyparse's equivalent of tree-sitter's
leading-underscore rule-name convention: its node is **omitted from the tree
when the rule was a pure pass-through**, with its single child promoted into the
parent in its place.

```scheme
(hidden (rule expression
  (choice assignment_expression)))

(hidden (override (rule primary_expression …)))
```

A *pure pass-through* is the case where the rule matched but added no structure
of its own: it contributed exactly one child, the call carried no alias, and no
operator or second child was produced. That child is re-tagged with the field the
hidden rule's own call site wanted (so a precedence ladder's inner `left:` is
replaced by whatever the outer caller named) and spliced into the parent.

The collapse is conditional on purpose. tinyparse's left-factored precedence
rules do double duty — `additive_expression ::= mult (('+'|'-') mult)*` is a bare
wrapper when no operator matched but a real binary node when one did. A hidden
rule therefore **collapses only in its pass-through case; when it produced an
operator, two-plus children, or its own structure it still emits its node**. A
pure dispatch rule (`(choice …)` with no operators) is always a single-child
wrapper, so it always collapses; an operator-ladder rule collapses only when the
input took its no-operator path. This is the difference from tree-sitter's hidden
rule, which always inlines: ours fits a PEG, where the operator and its operands
live in the same rule, so it must keep the node whenever the rule earned one.

`(hidden …)` wraps a `(rule …)` or an `(override (rule …))`. It is preferred over
a leading-underscore name because the rule keeps its descriptive name — that name
still shows in the non-collapsed (operator) case, where a `_additive_expression`
would only obscure it. What is hidden is the *node when empty*, not the name.

Some nodes never collapse regardless of the marker: the start/root node,
injection (language-tagged) nodes, aliased nodes, recovery/`MISSING` nodes, and
any leaf or token.

## Extras

`extras` are implicit terminals consumed between grammar elements:

```scheme
(extras
  (trivia Space)       ; consumed implicitly, preserved as trivia nodes
  (skip Bom)           ; consumed implicitly, discarded
  (error InvalidByte)) ; consumed implicitly, preserved, and diagnosed
```

Spaces inside a single token (e.g. a quoted string) are never touched by
extras — trivia only fires between elements, never inside a token match.

### Comment attachment — `attach:`

A `(trivia …)` entry may carry an `attach:` modifier naming where a run of that
trivia binds in the tree:

| `attach:` | The trivia binds to |
|-----------|---------------------|
| `trailing` *(default)* | the node being built when the trivia is scanned — historical behaviour |
| `leading` | the next sibling: the run is held and re-emitted ahead of the following node, in the parent |

`trailing` is the default, so a grammar that omits `attach:` parses exactly as
before. `leading` is for doc comments: a `/** … */` written above a declaration
becomes a sibling ahead of that declaration, and the declaration's extent stops
at its last real token, instead of the comment being swept into whichever node is
closing. The `std.javascript` family marks its comment kinds `attach: leading`;
`std.typescript` and `std.tsx` inherit it through `(extends …)`.

```scheme
(extras
  (trivia Space)                          ; attach: trailing (the default)
  (trivia line_comment  attach: leading)
  (trivia block_comment attach: leading))
```

Held `leading` trivia is restored on backtracking and across the rule memo, and a
comment at end of input is still emitted. The mechanism — a held-trivia buffer and
the `content_end` extent mark — lives in `tp_vm_consume_extras`, `tp_vm_flush_held`,
and the node lifecycle in `src/vm.c`.

## Forms documented in the design

The engine implements more than this page covers. Each of these forms has a
section in the design document and an example that runs it:

| Form | Design section | Example |
|------|----------------|---------|
| `(error …)` extras, `(recover …)` | [Error extras](../tinyparse-design.md#error-extras), [Recovery](../tinyparse-design.md#recovery) | [`recover`](../examples/recover/) |
| lexical goals | [Lexical goals](../tinyparse-design.md#lexical-goals) | [`lexgoal`](../examples/lexgoal/) |
| `(deferred-slot …)`, `(defer …)`, `(parse-deferred …)` | [Heredocs and deferred bodies](../tinyparse-design.md#heredocs-and-deferred-bodies) | [`heredoc`](../examples/heredoc/) |
| `class` and `predicate` host bindings | [Host bindings](../tinyparse-design.md#host-bindings) | [`bindings`](../examples/bindings/) |
| `(import …)` | [Module loading](../tinyparse-design.md#module-loading) | [`import`](../examples/import/) |
| `(extends …)`, `(override …)`, `(when …)` | [Grammar composition](../tinyparse-design.md#grammar-composition) | `grammars/std/`: std.jsonc extends std.json; std.tsx turns on std.typescript's `(when jsx: …)` |
| `(inject …)` | [Language injection](../tinyparse-design.md#language-injection) | [`nesting`](../examples/nesting/), `std.fence` |

Binary-mode grammars (parsing bytes rather than text) are not implemented; the
design's [Binary parsing](../tinyparse-design.md#binary-parsing) section
sketches them.
