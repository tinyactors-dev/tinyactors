# Grammar language

This reference describes every form of the tinyparse grammar language: the
`.scm` files a grammar is written in, and the `package.scm` manifest of a
grammar package. It is for grammar authors who need the exact syntax and effect
of a form. For a guided first grammar, see
[Write your first grammar](../tutorials/first-grammar.md).

Commands on this page assume the current directory is the `tinyparse/`
directory of a source checkout and that `tinyparse` is on `PATH`
([Install tinyparse](../how-to/install.md)). Every output sample is a real run
of `tinyparse run GRAMMAR --input FILE --dump-tree`, whose format is described
in [tinyparse(1)](../../tinyparse.1.md#tree-dump).

## Lexical syntax

A grammar file is a sequence of data written in a Scheme-like notation. The
reader builds lists, symbols, keywords, strings, integers, and byte literals; it
evaluates nothing.

| Datum | Written as | Notes |
|-------|------------|-------|
| List | `( … )` | Items are separated by whitespace. |
| Symbol | a run of bytes up to whitespace, `(`, `)`, `;` or `"` | `Source`, `any`, `std.json`, `@cap`, `#eq?` |
| Keyword | a symbol ending in `:` | `start:`, `min:`, `jsx:`. The colon is not part of the name. |
| String | `"…"` | Escapes are listed in the next table. |
| Integer | an optional `+` or `-`, then decimal digits | `1`, `-3`. Used by `repeat` bounds, which must not be negative, and by manifest values. |
| Byte literal | `#x` or `#X`, then hexadecimal digit pairs | `#x0A`, `#xEFBBBF`. Reading stops at the first byte that does not complete a pair. The bytes after the last complete pair begin the next datum: `#x414` reads as `#x41` and the integer `4`, and `#x41zz` as `#x41` and the symbol `zz`. `#x` alone is an empty byte string. |
| Comment | `;` to the end of the line | Discarded. |

A `#` that is not followed by `x` or `X` starts an ordinary symbol, so `#t`,
`#f` and `#eq?` are symbols.

String escapes:

| Escape | Byte |
|--------|------|
| `\n` | line feed (0x0A) |
| `\t` | tab (0x09) |
| `\r` | carriage return (0x0D) |
| `\0` | NUL (0x00) |
| `\\` | backslash |
| `\"` | double quote |
| `\x` followed by up to two hexadecimal digits | that byte; `\x` with no digit is NUL |
| `\` followed by any other byte | that byte, without the backslash: `\f` is `f`, `\]` is `]` |

The reader decodes string escapes before a regex sees the string. To pass a
backslash escape through to a [regex](#regex-dialect), double the backslash:
`(regex "\\x80")`, `(regex "[^\\]]")`.

A malformed file fails to compile with one of the faults `unterminated list`,
`unterminated string` or `unexpected ')'`.

## File structure

A grammar file holds top-level forms. The compiler reads these and ignores any
other top-level form without a fault.

| Form | Effect |
|------|--------|
| `(grammar NAME BODY…)` | The grammar. Required. Only the first `grammar` form in a file is compiled. |
| `(import NAME)`, `(import NAME as: ALIAS)` | Merges another grammar file into this one. See [import](#import). |
| `(extends NAME)` | Builds this grammar on a base grammar. See [extends and override](#extends-and-override). |
| `(tinyparse version: "0.1")` | Accepted and ignored. |
| `(module NAME)` | Accepted and ignored. A module is resolved by the name its importer gives. |

`import` and `extends` take effect only at the top level. Written inside the
`grammar` body they are ignored without a fault, and a later reference to a name
they would have supplied fails with `rule references an undefined name`, or with
`start: names a missing rule` when `start:` names it.

The `grammar` body holds these items, in any order:

| Item | Effect |
|------|--------|
| `start: RULE` | The rule a parse starts at. Required in the root grammar, and must name a rule (a token is rejected with `start: names a missing rule`). |
| `(token NAME PATTERN…)` | Defines a token. See [Tokens](#tokens). |
| `(rule NAME [extras: none\|all] PATTERN)` | Defines a rule. See [Rules](#rules). |
| `(hidden (rule …))`, `(hidden (override (rule …)))` | Defines a hidden rule. See [Hidden rules](#hidden-rules). |
| `(override (rule …))`, `(override (token …))` | Replaces a base definition. See [extends and override](#extends-and-override). |
| `(extras ENTRY…)` | Declares the extras. See [Extras](#extras). |
| `(lex-goal GOAL TRIGGER: TARGET…)` | Declares a lexical goal. See [Lexical goals](#lexical-goals). |

Any other list or keyword in the body is ignored without a fault, so a
misspelled head such as `(rul …)` drops that definition silently.

Rules and tokens share one namespace. A name defined twice in one file, or
defined both locally and by an `import`, keeps its first definition without a
fault. Imported definitions are merged first, so the import wins unless the
local definition is wrapped in [`override`](#extends-and-override). For a name
the base grammar defines, see [extends and override](#extends-and-override):
redefining it without `override` is a fault.

## Matching

Rules are matched as a parsing expression grammar (PEG):

- `choice` is ordered. The first alternative that matches is taken, and later
  alternatives are never tried at that position for that call. Alternatives
  that share a prefix must list the longer one first:
  `(choice ">>>" ">>" ">")`, `(choice "typeof" "type")`.
- When an alternative fails part-way, the parser returns to the alternative's
  start and tries the next one.
- `repeat` and `optional` are greedy and do not give input back:
  `(seq (repeat "a") "a")` never matches.
- A literal matches its bytes with no check of what follows. `(choice "let"
  Word)` reads `letter` as `let` followed by `ter`. A word-boundary check is a
  zero-width negative lookahead after the literal: see [`not`](#not).
- Before each terminal (a literal, a token reference or an inline token
  expression) and at each rule entry, the parser consumes [extras](#extras),
  unless the rule runs under `extras: none`.

Tokens are matched by a separate byte matcher. Inside a token, `choice` commits
to the first alternative that matches and `repeat` never gives bytes back:
`(seq (regex "[a-z]*") "x")` never matches, because the class consumes the `x`.
Extras are never consumed inside a token.

## Tokens

`(token NAME PATTERN)` defines a token: a terminal matched by the byte matcher.
A reference to the token in a rule produces one leaf node whose kind is `NAME`.
With more than one pattern, `(token NAME P1 P2 …)` matches them in sequence.
`(token NAME)` with no pattern matches nothing by itself; it is the placeholder
a [parametrized goal](#parametrized-goals) supplies a pattern for.

Token expressions:

| Form | Matches |
|------|---------|
| `"text"`, `#xHH…` | Exactly these bytes. |
| `any` | Any one byte. Fails at end of input. |
| `(regex "PATTERN")` | The [regex](#regex-dialect). |
| `(class NAME)` | One byte that the host class binding `NAME` accepts. See [Classes](#classes). |
| `(seq P …)` | Each pattern in turn. |
| `(choice P …)` | The first pattern that matches. |
| `(repeat P [min: N] [max: M])` | `P` as many times as it matches, at least `N` (default 0) and at most `M` (default no limit). A repetition that consumes nothing ends the loop. |
| `(repeat1 P …)` | `repeat` with a minimum of 1 (a larger `min:` is kept). |
| `(optional P)` | `P`, or nothing. |
| `(not GUARD)` | One byte, when `GUARD` does not match here. Fails at end of input. |
| `(not GUARD P …)` | `P …` in sequence, when `GUARD` does not match here. `GUARD` consumes nothing. |
| `(reserved P "word" …)` | `P`, unless the matched bytes equal one of the words. `P` must be a token expression; a token reference as `P` matches nothing. Words may be strings or symbols. `(reserved (regex "[a-z]+") "if")` rejects `if` and accepts `iffy`. |
| `(when PARAM: P)` | `P` when the [parameter](#parameters) is set; see [`when`](#when). |

A reference to another token or rule inside a token pattern matches nothing,
and `check` reports no fault for it. `EOF` inside a token pattern also matches
nothing.

Every token expression may also appear in a rule; see
[Inline token expressions](#inline-token-expressions).

## Regex dialect

`(regex "PATTERN")` compiles a small regular-expression dialect into a sequence
of atoms, each optionally followed by one quantifier.

| Syntax | Meaning |
|--------|---------|
| `c` | The byte `c`. Every byte not listed in this table is literal. |
| `.` | Any one byte, including a line feed. |
| `[…]` | One byte from the class. |
| `[^…]` | One byte not in the class. |
| `a-z` inside a class | The bytes from `a` to `z`. A range whose end is below its start is empty. A `-` before the closing `]` is literal. |
| `X+` | One or more `X`. |
| `X*` | Zero or more `X`. |
| `\` escape | See the next table. Allowed inside and outside classes. |

Escapes, as the regex receives them (write `\\n` in the `.scm` string, or let
the string escape produce the byte directly):

| Escape | Byte |
|--------|------|
| `\n` `\t` `\r` `\f` `\v` `\0` | line feed, tab, carriage return, form feed, vertical tab, NUL |
| `\xHH` | the byte with hexadecimal value `HH`; exactly two digits. With fewer, `\x` is the literal `x`. |
| `\` followed by any other byte | that byte, literally: `\\`, `\]`, `\^`, `\-`, `\.`, `\[` |

Quantifiers repeat greedily and never give bytes back, so `[a-z]*z` never
matches.

The dialect has no other syntax. These characters are literal bytes and
produce no fault: `?`, `|`, `{`, `}`, `(`, `)`, `^` outside a class, `$`, and a
quantifier that follows another quantifier. `\d`, `\w` and `\s` match the
letters `d`, `w` and `s`. A `]` directly after `[` or `[^` closes the class, so
`[]a]` is an empty class, which matches no byte, followed by `a]`, so the
regex never matches; write `[\\]a]`.

| Want | Write |
|------|-------|
| optional part | `(optional …)` around a token expression |
| alternation | `(choice …)` |
| bounded repetition | `(repeat … min: N max: M)` |
| a digit | `[0-9]` |
| a UTF-8 lead or continuation byte | `[\\x80-\\xFF]` |

An unterminated class fails to compile with
`unterminated character class in regex`.

## Rules

`(rule NAME PATTERN)` defines a rule. Each successful call of a rule produces
one node whose kind is `NAME`, holding the nodes its pattern produced, in
order. The node's byte range covers its children.

An optional modifier sits between the name and the pattern:

| Modifier | Effect |
|----------|--------|
| `extras: none` | The rule and every rule it calls consume no extras, until a rule with `extras: all` is reached. |
| `extras: all` | Restores extras for a rule reached inside an `extras: none` rule. |

Any other value fails with ``extras: on a rule accepts only `none` or `all` ``.

The entries below describe the patterns a rule body may use. Each gives the
syntax, the arguments, the nodes it produces and an example excerpt.

### Literal

- **Syntax:** `"text"` or `#xHH…`
- **Produces:** one leaf whose kind is the literal text: `"("` produces a leaf
  of kind `(`.
- **Example:** `(seq "(" Items ")")`

### Reference

- **Syntax:** `NAME`
- **Arguments:** the name of a rule or token. A name that is neither fails with
  `rule references an undefined name`.
- **Produces:** the node of the rule or the leaf of the token.
- **Example:** `(seq Name "=" Value)`

### EOF

- **Syntax:** `EOF`
- **Produces:** nothing. Matches only at the end of input.
- **Example:** `(rule Source (seq (repeat Form) EOF))`

### seq

- **Syntax:** `(seq P …)`
- **Produces:** the nodes of each part, in order, with no node of its own.
  `(seq)` matches the empty string.
- **Example:** `(seq Key ":" Value)`

### choice

- **Syntax:** `(choice P …)`
- **Produces:** the nodes of the alternative that matched. See
  [Matching](#matching) for the order rule.
- **Example:** `(choice Object Array String)`

### repeat

- **Syntax:** `(repeat P [min: N] [max: M])`
- **Arguments:** `N` and `M` are integers; `max:` absent or `0` means no limit.
  A negative bound is not checked: it is read as an unsigned 32-bit count, so
  `min: -1` never matches and `max: -1` caps the count at 4,294,967,295. A
  `min:` greater than a nonzero `max:` never matches. Any other keyword fails
  with `unknown repeat keyword`. A single item after the bounds, or after `P`,
  is ignored without a fault: `(repeat "a" "b")` and `(repeat "a" min: 2 "b")`
  both match `aa`. `unexpected form in repeat` is reported only for a
  non-keyword followed by another item, as in `(repeat "a" "b" "c")`.
- **Produces:** the nodes of every repetition, with no node of its own.
- **Example:** `(repeat (seq "," Value))`

### repeat1

- **Syntax:** `(repeat1 P [min: N] [max: M])`
- **Produces:** as `repeat`, with a minimum of 1.
- **Example:** `(repeat1 Block)`

### optional

- **Syntax:** `(optional P)`
- **Produces:** the nodes of `P` when it matched, else nothing.
- **Example:** `(optional (seq "=" Value))`

### field

- **Syntax:** `(field NAME: P)`
- **Arguments:** `NAME` is a keyword.
- **Produces:** the node `P` produced, labeled with the field `NAME`. The label
  passes through `choice`, `optional`, `goal` and `alias`. It is dropped when
  `P` is a `seq` or `repeat`, or produces no node.
- **Example:** `(field key: String)`

### alias

- **Syntax:** `(alias NAME P)`
- **Arguments:** `NAME` is a symbol or a keyword; the tree shows it without the
  colon.
- **Produces:** the node `P` produced, with kind `NAME`. Like `field`, it passes
  through `choice`, `optional`, `goal` and `field`, and is dropped by `seq` and
  `repeat`.
- **Example:** `(alias Word: Name)`

This grammar shows `field` and `alias` on a single node and `field` dropped
through `repeat`:

```scheme
(grammar forms
  start: Source
  (token Space (regex "[ \n]+"))
  (token Num (regex "[0-9]+"))
  (token Name (regex "[a-z]+"))
  (extras (trivia Space))
  (rule Source (seq (repeat Item) EOF))
  (rule Item
    (choice
      (field number: Num)
      (alias Word: Name)
      (seq "(" (field items: (repeat Num)) ")"))))
```

For the input `7 ab (1 2)` followed by a line feed, the tree is:

```text
Source [0-11] [0,0]-[1,0]
  Item [0-1] [0,0]-[0,1]
    number: Num [0-1] [0,0]-[0,1]
  Space [1-2] [0,1]-[0,2]
  Item [2-4] [0,2]-[0,4]
    Word [2-4] [0,2]-[0,4]
  Space [4-5] [0,4]-[0,5]
  Item [5-10] [0,5]-[0,10]
    ( [5-6] [0,5]-[0,6]
    Num [6-7] [0,6]-[0,7]
    Space [7-8] [0,7]-[0,8]
    Num [8-9] [0,8]-[0,9]
    ) [9-10] [0,9]-[0,10]
  Space [10-11] [0,10]-[1,0]
```

### not

- **Syntax:** `(not GUARD P)`, `(not GUARD P1 P2 …)` or `(not GUARD)`
- **Arguments and produces:** what the form does depends on its operands.

  | Operands | Matched as | `GUARD` | Produces |
  |----------|------------|---------|----------|
  | Exactly one `P`, and `P` contains a token or rule reference | a lookahead, then `P` as a rule pattern | a token expression or a token reference; a rule reference never matches | the nodes of `P` |
  | `(not GUARD)`, two or more `P`, or one `P` made only of token expressions | an [inline token expression](#inline-token-expressions) | a token expression; a token or rule reference never matches | nothing |

  In the first case, `GUARD` is tried at the current byte, before extras are
  consumed, and consumes nothing. In the second case, a token or rule
  reference anywhere in the form matches nothing, so a form with two or more
  `P` that names a token or rule never matches. The one-argument form
  consumes one byte.
- **Example:** a zero-width word boundary after a keyword,
  `(seq "if" (not (regex "[a-z]") (repeat (regex "[a-z]"))))`: the guard
  rejects a following letter, and the `repeat` then matches nothing. The form
  produces no node.
- **Example:** `(repeat (not HeredocDelimLine BodyCommand))` stops a repetition
  at a closing line. `BodyCommand` is a rule, so `HeredocDelimLine`, a token,
  works as the guard and each `BodyCommand` node appears in the tree.

This grammar shows that a token-only `not` leaves no node, even for bytes a
literal matched:

```scheme
(grammar notleaf
  start: S
  (rule S (seq (not "x" "ab") EOF)))
```

For the input `ab` with no line feed, the tree is:

```text
S [0-2] [0,0]-[0,2]
```

### Inline token expressions

- **Syntax:** `any`, `(regex …)`, `(class …)`, `(reserved …)`, and the `(not …)`
  forms of the second case under [not](#not), written directly in a rule
- **Produces:** nothing. The matched bytes lie inside the parent's range but
  belong to no node. Define a token to get a leaf.
- **Example:** `(seq "<" (regex "[ \t]*") Name ">")`

### goal

- **Syntax:** `(goal GOAL P)`
- **Arguments:** `GOAL` names a [lexical goal](#lexical-goals). A name that no
  `lex-goal` declares is accepted and has no effect.
- **Produces:** the nodes of `P`.
- **Example:** `(goal expr (choice slash Ident))`

### when

- **Syntax:** `(when PARAM: P)`
- **Arguments:** `PARAM` is a keyword naming a [parameter](#parameters).
- **Produces:** the nodes of `P` when the parameter is set when the grammar is
  compiled. When it is not set, the form is removed from the enclosing `seq`,
  `choice` or `not`; anywhere else it matches the empty string.
- **Example:** `(choice Identifier Literal (when jsx: JsxElement))`

### recover

- **Syntax:** `(recover P (on eof message: "TEXT" [help: "TEXT"] [insert: "KIND"] [(label NAME: "TEXT")]))`
- **Arguments:** valid only as the whole body of a rule; nested anywhere else
  it fails with `unknown pattern combinator`. `message:` is required. `eof` is
  the only trigger.
- **Produces:** the nodes of `P`. Recovery applies to every element of a `seq`
  inside `P`, or inside a rule `P` calls, that fails at the end of input after
  `P` has consumed at least one byte. For each such element, the parser inserts
  a zero-width MISSING node of kind `KIND` and continues; without `insert:`, the
  kind is empty. The recovery raises a `parse.recover` diagnostic with
  the message, the help, and the label text on a zero-width span; the label's
  `NAME` is not used. Nested recoveries repeat the diagnostic: with
  [`examples/recover/list.scm`](../../examples/recover/list.scm), `(` reports
  it once, `((` three times and `(((` seven times. The diagnostic is described under
  [`parse.recover`](diagnostics.md#parserecover).
- **Example:** [`examples/recover/list.scm`](../../examples/recover/list.scm)

### inject

- **Syntax:** `(inject grammar: NAME content: REGION)`
- **Arguments:** `NAME` is resolved and compiled through the module resolver
  when the host grammar is compiled; see [Composition](#composition). `REGION`
  is a pattern.
- **Produces:** one `injection` node over the bytes `REGION` matched, holding
  the tree `NAME` builds for those bytes. Every node in it carries the language
  tag `NAME`, which `--dump-tree` prints as `[NAME]`. Nodes `REGION` produced
  are replaced, so a `field` inside `REGION` does not appear. When the injected
  parse fails, the `injection` node holds only an empty root node and no
  diagnostic is raised.
- **Example:** `(inject grammar: std.json content: (field content: FenceBody))`
  in `grammars/std/fence/grammar.scm`

### deferred-slot, defer, parse-deferred

- **Syntax:** `(deferred-slot SLOT)`, `(defer SLOT P)`,
  `(parse-deferred SLOT RULE)`
- **Produces:** `deferred-slot` and `parse-deferred` produce no node of their
  own; `defer` produces the nodes of `P`; `parse-deferred` produces one `RULE`
  node per queued item.
- See [Deferred slots](#deferred-slots).

## Hidden rules

`(hidden (rule NAME …))` and `(hidden (override (rule NAME …)))` define a rule
whose node is left out of the tree when the call produced exactly one child
node and carried no `alias`. The child takes the hidden node's place in the
parent and takes the field of the call site; when the call site has no field,
the child has none either. When the call produced no child, produced two or more
children, or carried an `alias`, the node is kept. The start rule's node is
always kept.

`hidden` accepts only a rule; `(hidden (token …))` fails with
``hidden expects a (rule ...) or (override (rule ...)) form``.

```scheme
(grammar ladder
  start: Source
  (token Space (regex "[ \n]+"))
  (token Num (regex "[0-9]+"))
  (extras (trivia Space))
  (rule Source (seq (repeat Stmt) EOF))
  (rule Stmt (seq (field value: Expr) ";"))
  (hidden (rule Expr (choice Sum)))
  (hidden (rule Sum
    (seq (field left: Num) (repeat (seq "+" (field right: Num)))))))
```

For the input `1;`, `1+2;` on two lines, `Expr` is always left out and `Sum` is
left out only where no `+` matched:

```text
Source [0-8] [0,0]-[2,0]
  Stmt [0-2] [0,0]-[0,2]
    value: Num [0-1] [0,0]-[0,1]
    ; [1-2] [0,1]-[0,2]
  Space [2-3] [0,2]-[1,0]
  Stmt [3-7] [1,0]-[1,4]
    value: Sum [3-6] [1,0]-[1,3]
      left: Num [3-4] [1,0]-[1,1]
      + [4-5] [1,1]-[1,2]
      right: Num [5-6] [1,2]-[1,3]
    ; [6-7] [1,3]-[1,4]
  Space [7-8] [1,4]-[2,0]
```

Why the rule is conditional is explained in
[How tinyparse shapes the tree](../explanation/tree-shape.md).

## Extras

Extras are tokens the parser consumes between grammar elements without the
grammar naming them. This excerpt declares one entry of each kind:

```scheme
(extras
  (trivia Space)
  (trivia Comment attach: leading)
  (skip Bom)
  (error Junk))
```

| Entry | Effect |
|-------|--------|
| `(trivia TOKEN)` | Consumed and kept in the tree as a leaf of kind `TOKEN`. |
| `(skip TOKEN)` | Consumed and discarded. |
| `(error TOKEN)` | Consumed, kept as a leaf, and reported as an `extra.error` diagnostic. |

`TOKEN` names a token. An entry naming a rule or an undefined name is accepted
and never matches. The entries are tried in order at each consumption point,
and consumption repeats until none matches. `extras` forms from imported and
base grammars are appended to the grammar's own, and no form removes an
inherited entry.

A `trivia` entry takes an optional `attach:` modifier:

| `attach:` | Where the run is placed |
|-----------|-------------------------|
| `trailing` (default) | In the node being built when the trivia is consumed. |
| `leading` | Held, together with any trivia consumed after it, and placed at the next consumption point (the next rule entry or terminal) in the node being built there. |

The modifier accepts no other value
(`unknown attach value (want leading or trailing)`).

A `leading` run placed at a terminal inside a rule that then closes is emitted
after that rule's node, out of byte order. For example, with
`(trivia Comment attach: leading)` and a rule `(seq "(" Num ")")`, a comment
between `2` and `)` in `(2 #c` followed by a line feed and `)` appears as a
sibling after the closing node.

This grammar uses all three policies:

```scheme
(grammar extras
  start: Source
  (token Space (regex "[ \n]+"))
  (token Bom   "\xEF\xBB\xBF")
  (token Junk  "@")
  (token Num   (regex "[0-9]+"))
  (extras
    (trivia Space)
    (skip Bom)
    (error Junk))
  (rule Source (seq (repeat Num) EOF)))
```

For a byte-order mark followed by `1 @ 2` and a line feed, the CLI prints the
diagnostic on standard error, then the tree, and exits with status 1. The
byte-order mark at bytes 0 to 3 appears in no node:

```text
error: unexpected input [extra.error]
Source [0-9] [0,0]-[1,0]
  Num [3-4] [0,3]-[0,4]
  Space [4-5] [0,4]-[0,5]
  Junk [5-6] [0,5]-[0,6]
  Space [6-7] [0,6]-[0,7]
  Num [7-8] [0,7]-[0,8]
  Space [8-9] [0,8]-[1,0]
```

## Lexical goals

A lexical goal makes one token scan as a different token in some parts of the
grammar.

```scheme
(lex-goal GOAL TRIGGER: TARGET …)
```

- **Arguments:** `GOAL` is a symbol. Each `TRIGGER:` is a keyword naming a
  token, and each `TARGET` is a token name or a [`param`](#parametrized-goals)
  form. A trigger, or a target given by name, that is not a token fails with
  `lex-goal trigger and target must name defined tokens`. At least one pair is
  required (`lex-goal expects trigger: Target pairs`).
- **Effect:** while `(goal GOAL P)` is active, a reference to `TRIGGER` scans
  the `TARGET` pattern instead and produces a leaf of kind `TARGET`. Outside
  every goal that redirects it, `TRIGGER` scans its own pattern.

A goal stays active in every rule `P` calls, until a nested `goal` replaces it;
the outer goal is restored when the nested one ends. Several `lex-goal` forms may
name the same goal.

This excerpt of
[`examples/lexgoal/mini-js.scm`](../../examples/lexgoal/mini-js.scm) reads `/`
as a regex literal in one goal and as division in another:

```scheme
(token slash        "/")
(token RegexLiteral (regex "/[^/]*/"))
(token Div          "/")
(lex-goal expr slash: RegexLiteral)
(lex-goal op   slash: Div)
(rule Primary (goal expr (choice slash Ident)))
(rule BinOp   (seq (goal op slash) Ident))
```

For a task-oriented walk-through, see
[Lex tokens that depend on context](../how-to/context-sensitive-lexing.md).

### Parametrized goals

A `param` target builds the target pattern from bytes captured earlier in the
parse. It is how a here-document's closing line is matched against the
delimiter its opening line named. This excerpt is from
[`examples/heredoc/heredoc.scm`](../../examples/heredoc/heredoc.scm):

```scheme
(token HeredocDelimLine)
(lex-goal heredoc-body-mode
  HeredocDelimLine: (param delimiter (seq delimiter "\n")))
```

- **Syntax:** `(param NAME BODY)` as a `lex-goal` target. The trigger is
  usually a token with no pattern.
- **Arguments:** inside `BODY`, the symbol `NAME` stands for the captured bytes.
  `BODY` may use only literals, regexes, `any`, `seq`, `choice`, `repeat`,
  `optional`, `not` and `NAME`; anything else, including a token reference or
  `(class …)`, fails with `param body must contain only token-level patterns`.
- **Effect:** each item `parse-deferred` drains pushes `BODY` with `NAME`
  replaced by that item's bytes; the pattern is popped when the item's rule
  returns. The pattern is active wherever the goal is active.

### Deferred slots

Three forms queue a match and parse something for it later in the input:

| Form | Effect |
|------|--------|
| `(deferred-slot SLOT)` | Declares a first-in, first-out queue named `SLOT`. Each call of the enclosing rule gets a fresh, empty queue, so a rule that calls itself through nesting has one queue per call. Matches the empty string. |
| `(defer SLOT P)` | Matches `P` and appends the matched bytes to the queue `SLOT` of the innermost rule call that declared it. If the parser backtracks past the `defer`, the item is removed. |
| `(parse-deferred SLOT RULE)` | Removes each queued item in order and calls `RULE` once for it, with the item's bytes pushed onto the parametrized goal `RULE` reaches. An empty queue does nothing. |

Compile-time checks:

| Fault | Cause |
|-------|-------|
| `defer/parse-deferred references an undeclared slot` | No rule declares `SLOT` with `deferred-slot`. The check is grammar-wide; it does not verify that the declaring rule encloses the use. |
| `parse-deferred names an undefined rule` | `RULE` is not a rule. |
| `parse-deferred reaches conflicting parametrized goals` | `RULE`'s body contains `goal` forms for two different parametrized goals. |

The grammar author must keep these conditions, which the compiler does not
check:

- `deferred-slot` and `parse-deferred` sit in a rule other than the start rule.
  In the start rule, a nonempty queue makes the parse fail with
  `parse.syntax`.
- `parse-deferred` is the last element of a `seq` that cannot fail after it,
  and does not sit inside a `choice` alternative or an `optional` that can fail
  after it ran. A drained item is not restored on backtracking.
- When `RULE` tries the parametrized pattern, the cursor is where the closing
  pattern must start. A here-document grammar keeps the cursor at a line start
  by ending every body line with its line feed.

Behavior of [`examples/heredoc/heredoc.scm`](../../examples/heredoc/heredoc.scm)
on the edge cases:

| Input | Result |
|-------|--------|
| A command with no `<<` | The queue is empty; `HeredocBody` is never called. |
| `cat <<A <<B` | Two items; the body ending at `A` is parsed first, then the body ending at `B`. |
| An inner `cat <<EOF` inside `$( … )` within an outer body that also ends at `EOF` | The inner body ends at the first `EOF` line and the outer body at the second. |
| A body with no closing line | `HeredocBody` fails, the queued item is dropped without a diagnostic, and the body lines parse as ordinary commands. Exit status 0. |
| A closing line with no line feed at end of input | The closing pattern requires the line feed, so the parse fails with `parse.syntax`. Exit status 1. |

## Composition

A grammar is composed from several files at compile time. The library never
reads files itself: it asks the host's module resolver for a name's bytes. A C
host installs one with
[`tinyparse_set_module_resolver`](c-api.md#tinyparse_set_module_resolver);
without one, `import`, `extends` and `inject` fail with
`import requires a module resolver`, `extends requires a module resolver` or
`inject requires a module resolver to find the embedded grammar`.

The resolver of the `tinyparse` command is described under
[GRAMMAR ARGUMENTS in tinyparse(1)](../../tinyparse.1.md#grammar-arguments).

For worked steps, see
[Split, extend and combine grammars](../how-to/compose-grammars.md).

### import

- **Syntax:** `(import NAME)` or `(import NAME as: ALIAS)`, at the top level.
- **Effect:** compiles the resolved file and merges its tokens, rules, extras
  and lexical goals into this grammar. The imported file's `start:` is ignored.
  `as:` is accepted and has no effect; all merged names share one namespace.
- **Faults:** `module resolver could not load an imported module`,
  `import cycle detected`.

### extends and override

- **Syntax:** `(extends NAME)` at the top level; `(override (rule …))` or
  `(override (token …))` in the grammar body.
- **Effect:** `extends` merges the base grammar as `import` does and records
  every token and rule name it defined. The extending grammar must have its own
  `start:`, and the base grammar's `start:` is ignored. Redefining a base name
  without `override` fails with `redefining a base name requires (override ...)`. An `override`
  replaces the base definition; an `override` of a name the base does not define
  adds it. `(hidden (override (rule …)))` replaces a base rule with a hidden one.
- **Faults:** `module resolver could not load the base grammar`,
  `extends cycle detected`.

### Parameters

A parameter is a name that is set or unset when a grammar is compiled. Only
[`when`](#when) reads parameters. They are set by the `params:` field of a
[package manifest](#packagescm-manifest), by `--param NAME[=VALUE]` on the
command line ([tinyparse(1)](../../tinyparse.1.md#parameters)), or by
[`tinyparse_set_param`](c-api.md#tinyparse_set_param). A command-line `--param`
overrides the manifest. A parameter applies to the whole compile, including
imported and base grammars. The `params:` of an imported or extended package's
manifest are not applied: a grammar that writes `(extends std.typescript)` does
not get the `jsx` parameter that `std.typescript`'s manifest sets.

## Classes

`(class NAME)` matches one byte that a host class binding accepts. The binding
is looked up when the parser runs; a name with no binding matches nothing and
raises no fault.

The classes the `tinyparse` command binds are listed under
[HOST SERVICES in tinyparse(1)](../../tinyparse.1.md#host-services). A C host
has no classes until it binds them with [`tinyparse_bind`](c-api.md#tinyparse_bind);
see [Supply imports, classes and parameters from a C host](../how-to/host-services.md).
A packed grammar or a generated parser keeps `(class NAME)` by name, so the
program that runs it must bind the classes too.

## package.scm manifest

A grammar package is a directory that holds a `package.scm` manifest and a
`grammar.scm` grammar. The `tinyparse` command accepts the directory, or the
path of its `package.scm`, wherever it accepts a grammar; it then compiles
`grammar.scm` with the manifest's parameters set.

The manifest is one `(package …)` form whose body is keyword and value pairs:

| Field | Effect |
|-------|--------|
| `name:` | The package name. Read and not used. |
| `version:` | A version string. Read and not used; nothing selects a package by version. |
| `params:` | A list of `NAME: VALUE` pairs, each set as a [parameter](#parameters) before `--param` overrides are applied. A trailing `NAME:` with no value sets `NAME`. |
| `extends:` | Read and not used. The base grammar comes from the `(extends NAME)` form in `grammar.scm`. |
| `provides:` | Not read. |

Other fields and other top-level forms are ignored.

A `params:` value sets or clears its parameter:

| Value | Parameter |
|-------|-----------|
| a nonzero integer, a nonempty string, a symbol, a keyword, a nonempty list | set |
| `0`, `""`, `#x`, `()` | cleared |
| `#t` | set |
| `#f` | set; the reader reads `#f` as a symbol |

To turn a parameter off in a manifest, write `0`.

The manifest of `std.typescript`, with its comment lines left out:

```scheme
(tinyparse version: "0.1")

(package
  name: std.typescript
  version: "0.1.0"
  extends: (std.javascript)
  params: (types: #t jsx: #f)
  provides: (program))
```

Its `jsx: #f` sets `jsx`; the effect is listed under
[std.typescript](std-grammars.md#stdtypescript).

| Fault | Cause |
|-------|-------|
| `package manifest is not valid .scm` | The reader failed. |
| `package manifest has no (package ...) form` | No top-level `package` form. |

## Compile faults

A grammar that fails to compile leaves one fault message on the engine
([`tinyparse_fault`](c-api.md#tinyparse_fault); the `tinyparse` command prints
it as `error: MESSAGE` and exits with status 1). A fault carries no position
([Faults](diagnostics.md#faults)). Parse-time diagnostics are listed under
[Codes](diagnostics.md#codes).

| Fault | Cause |
|-------|-------|
| `grammar form not found` | No top-level `grammar` form. |
| `grammar needs a name` | `(grammar)` has no symbol after the head. |
| `grammar has no start: rule` | The root grammar has no `start:`. |
| `start: needs a rule name` | `start:` is not followed by a symbol. |
| `start: names a missing rule` | `start:` names a token or an undefined name. |
| `rule references an undefined name` | A rule refers to a name that is no rule or token. |
| `unknown pattern combinator` | A list pattern's head is not a form listed on this page. |
| `pattern head must be a symbol` | A list pattern starts with something other than a symbol. |
| `expected a pattern` | An empty list, an integer or a keyword where a pattern belongs. |
| `malformed rule form`, `rule has no body` | `(rule …)` lacks a name or a pattern. |
| `malformed token form` | `(token …)` lacks a name. |
| `regex expects one string` | `(regex …)` with other than one string. |
| `class expects one name`, `class name must be a symbol` | Malformed `(class …)`. |
| `repeat needs a child`, `unexpected form in repeat`, `repeat bound must be an integer`, `unknown repeat keyword` | Malformed `repeat` or `repeat1`. |
| `optional needs one child` | `(optional)` with other than one pattern. |
| `not needs operands` | `(not)` with no guard. |
| `field expects name: and a child`, `field name must be a keyword` | Malformed `field`. |
| `alias expects a name and a child`, `alias name must be a symbol or keyword` | Malformed `alias`. |
| `reserved expects an inner pattern and word(s)`, `reserved word must be a string or symbol` | Malformed `reserved`. |
| `goal expects a goal name and one child` | Malformed `goal`. |
| `when expects PARAM: and one element`, `when parameter must be a keyword` | Malformed `when`. |
| `recover expects a body and an (on eof ...) clause`, `recover clause must be (on eof ...)`, `recover clause expects keyword: "string" pairs`, `unknown keyword in recover clause`, `malformed label in recover clause`, `recover clause needs a message:` | Malformed `recover`. |
| `inject expects grammar: NAME content: REGION`, `inject needs grammar: NAME`, `inject needs content: REGION`, `inject could not resolve the embedded grammar`, `inject could not compile the embedded grammar` | Malformed `inject`, or its grammar could not be loaded. |
| ``extras: on a rule accepts only `none` or `all` `` | A rule's `extras:` has another value. |
| `malformed extras entry`, `unknown extras policy`, `extras entry expects an attach: modifier`, `unknown attach value (want leading or trailing)` | Malformed `extras` entry. |
| `hidden expects one (rule ...) or (override (rule ...)) form`, `hidden expects a (rule ...) or (override (rule ...)) form`, `hidden override expects an (override (rule ...)) form` | Malformed `hidden`. |
| `override expects one (rule ...) or (token ...) form`, `override expects a (rule ...) or (token ...) form` | Malformed `override`. |
| `lex-goal needs a goal name`, `lex-goal expects trigger: Target pairs`, `lex-goal trigger and target must name defined tokens` | Malformed `lex-goal`. |
| `param expects NAME and a body`, `param body must contain only token-level patterns` | Malformed `param`. |
| `deferred-slot needs a slot name`, `defer needs a slot name and a pattern`, `parse-deferred needs a slot name and a rule name` | Malformed deferred-slot forms. |
| `too many distinct deferred-slot declarations in one rule` | One rule body declares more than 64 slot names. |
| The faults under [Deferred slots](#deferred-slots), [import](#import), [extends and override](#extends-and-override), and [Composition](#composition) | As described there. |
| `unterminated list`, `unterminated string`, `unexpected ')'`, `unterminated character class in regex` | The file or a regex is malformed. |
| `out of memory` and messages starting `out of memory` | The engine's memory grant is exhausted; see [Sizing a grant](c-api.md#sizing-a-grant). |
