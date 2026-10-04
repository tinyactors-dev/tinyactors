# Diagnostics

This reference is for grammar authors who meet a diagnostic code and host
authors who read diagnostics from C. It lists every code the library reports,
what each puts in the fields of a diagnostic, and the faults a parser call can
leave instead of a diagnostic.

A parse records diagnostics on its `tinyparse_parser`. The structure and the
functions that read it, `tinyparse_parser_diagnostic_count` and
`tinyparse_parser_diagnostic`, are in the [C API reference](c-api.md#diagnostics).
Every code below has severity `TINYPARSE_SEVERITY_ERROR`, one label and an
empty note. A label's byte range is 0-based and end-exclusive, in the input as
pushed; a diagnostic carries no line or column, and [Recover from and report
syntax errors](../how-to/handle-errors.md) converts one. A failed library
call, such as a grammar that does not compile, records a fault on the engine
instead; see [Faults](#faults).

## Codes

Every entry gives the message, the label, the help text, the result code
`tinyparse_finish` returns, what triggers the diagnostic, and a reproduction
run from the `tinyparse/` directory with `tinyparse` on `PATH`
([Install tinyparse](../how-to/install.md)). The `tinyparse` command prints
a diagnostic as `MESSAGE [CODE]` and a help line; it prints no labels
([tinyparse(1)](../../tinyparse.1.md), DIAGNOSTICS).

| Code | Result | Meaning |
|------|--------|---------|
| [`parse.syntax`](#parsesyntax) | `TINYPARSE_SYNTAX_ERROR` | The start rule did not match. |
| [`parse.trailing`](#parsetrailing) | `TINYPARSE_SYNTAX_ERROR` | The start rule matched but left input over. |
| [`parse.no-start`](#parseno-start) | `TINYPARSE_SYNTAX_ERROR` | The start rule passed to `tinyparse_parser_new` is not a rule. |
| [`parse.recover`](#parserecover) | `TINYPARSE_OK` | A `recover` form inserted a missing node at the end of the input. |
| [`extra.error`](#extraerror) | `TINYPARSE_OK` | An `(error TOKEN)` extra consumed bytes. |

### parse.syntax

- **Message:** `error: unexpected input`
- **Label:** `parse failed here`, over the byte where the start rule stopped
  matching (`[P-P+1]`), or zero-width at the end of the input.
- **Help:** empty.
- **Result:** `TINYPARSE_SYNTAX_ERROR`.
- **Trigger:** the start rule fails. P is the position where the start
  rule's body stopped matching, after failed alternatives have backtracked.
  For an error inside a nested construct, P is the start of the outermost
  construct that could not be completed, which can be far from the byte
  where the input went wrong. For `x (a (b c)` and
  `examples/sexpr/sexpr.scm`, the label is `[2-3]`, the `(` of the unclosed
  outer list, although the input runs out at byte 10.
- **Example:**

  ```sh
  printf 'x (a (b c)' | tinyparse run examples/sexpr/sexpr.scm --input -
  ```

  The command prints the diagnostic, prints no tree and exits with status 1:

  ```text
  error: unexpected input [parse.syntax]
  ```

### parse.trailing

- **Message:** `error: unexpected trailing input`
- **Label:** `unexpected here`, over the first byte not consumed (`[P-P+1]`).
- **Help:** empty.
- **Result:** `TINYPARSE_SYNTAX_ERROR`.
- **Trigger:** the start rule matched a prefix of the input, and the input
  continues after it and after any extras. A start rule that ends with `EOF`
  reports `parse.syntax` instead.
- **Example:** this grammar, saved as `trailing.scm`, has a start rule without
  `EOF`:

  ```scheme
  (tinyparse version: "0.1")

  (grammar letters
    start: Word
    (token Letter (regex "[a-z]"))
    (rule Word (repeat1 Letter)))
  ```

  ```sh
  printf 'ab1' | tinyparse run trailing.scm --input -
  ```

  The label is `[2-3]`, the `1`. The command exits with status 1:

  ```text
  error: unexpected trailing input [parse.trailing]
  ```

### parse.no-start

- **Message:** `error: start rule not found`
- **Label:** `missing start rule`, zero-width at byte 0.
- **Help:** empty.
- **Result:** `TINYPARSE_SYNTAX_ERROR`.
- **Trigger:** the `start_rule` passed to `tinyparse_parser_new` is a name the
  grammar uses for something other than a rule: a token, a literal or a
  field. A name the grammar does not use at all fails earlier:
  `tinyparse_parser_new` returns `TINYPARSE_INVALID` with the fault
  `start rule not found in grammar`.
- **Example:** none from the command line: the `tinyparse` command always
  parses from the grammar's `start:` rule. From C, passing `open` as the start
  rule for `examples/recover/list.scm`, where `open` is a field, records this
  diagnostic.

### parse.recover

- **Message:** the `message:` text of the grammar's `(on eof ...)` clause,
  verbatim.
- **Label:** the text of the clause's `(label NAME: "TEXT")`, or an empty
  message without one, zero-width at the position of the inserted node. The
  label's `NAME` is not used, and with several `label` entries the last one
  counts.
- **Help:** the clause's `help:` text, or empty.
- **Result:** `TINYPARSE_OK`. The tree holds a zero-width node flagged
  `TINYPARSE_NODE_MISSING` for each element recovery inserted.
- **Trigger:** a [`recover`](grammar.md#recover) form's body fails at the end
  of the input after consuming at least one byte. One activation of a
  `recover` form records one diagnostic however many nodes it inserts. Nested
  activations each record their own: `examples/recover/list.scm` records 1
  diagnostic for `(a`, 3 for `((a` and 7 for `(((a`.
- **Example:**

  ```sh
  printf '(a (b c)' | tinyparse run examples/recover/list.scm --input -
  ```

  The label is `[8-8]`, at the end of the input. The command exits with
  status 1 because the diagnostic has error severity:

  ```text
  unterminated list [parse.recover]
    help: add a closing `)`
  ```

### extra.error

- **Message:** `error: unexpected input`
- **Label:** `unexpected here`, over the bytes the error extra consumed.
- **Help:** empty.
- **Result:** `TINYPARSE_OK`. The tree holds a leaf of the extra's token kind
  over the same bytes.
- **Trigger:** a token listed as `(error TOKEN)` in the grammar's
  [`extras`](grammar.md#extras) matches between two grammar elements. The
  parser records the diagnostic each time it consumes those bytes, so a parse
  that backtracks over them records it again: one error leaf can come with
  two or more `extra.error` diagnostics with the same label. Count error leaves
  in the tree when the number matters.
- **Example:** this grammar, saved as `stray.scm`, reports any byte that is
  neither a letter nor whitespace:

  ```scheme
  (tinyparse version: "0.1")

  (grammar words
    start: Source
    (token Space (regex "[ \t\r\n]+"))
    (token Word  (regex "[A-Za-z]+"))
    (token Stray (regex "[^A-Za-z \t\r\n]"))
    (extras
      (trivia Space)
      (error Stray))
    (rule Source
      (seq (repeat Word) EOF)))
  ```

  ```sh
  printf 'foo @ bar' | tinyparse run stray.scm --input - --dump-tree
  ```

  The tree goes to standard output, with the `@` as a `Stray` leaf:

  ```text
  Source [0-9] [0,0]-[0,9]
    Word [0-3] [0,0]-[0,3]
    Space [3-4] [0,3]-[0,4]
    Stray [4-5] [0,4]-[0,5]
    Space [5-6] [0,5]-[0,6]
    Word [6-9] [0,6]-[0,9]
  ```

  The diagnostic, labeled `[4-5]`, goes to standard error, and the command
  exits with status 1:

  ```text
  error: unexpected input [extra.error]
  ```

## Injected regions

A region parsed by another grammar through an `inject` form is parsed with its
own diagnostic list, which is discarded: errors inside the region produce no
diagnostic and do not change the result code. With `std.fence`, a `json` block
holding `[1,` produces an `injection [std.json]` node holding one childless
`Document` node, no diagnostic, and exit status 0 from `tinyparse run`.

## Faults

A fault is the reason a library call failed. It is not a diagnostic: the
call returns a result code other than `TINYPARSE_OK` and leaves one line of
text on the engine, which [`tinyparse_fault`](c-api.md#tinyparse_fault)
returns ([Faults](c-api.md#faults) in the C API reference). A fault has no
code, no label and no position.

`tinyparse_compile_scheme` returns `TINYPARSE_INVALID` for a grammar that does
not compile. The [Compile faults](grammar.md#compile-faults) table in the
grammar reference lists each fault text and its cause. These faults come from
creating and finishing a parser:

| Fault | Call | Result | Cause |
|-------|------|--------|-------|
| `start rule not found in grammar` | `tinyparse_parser_new` | `TINYPARSE_INVALID` | `start_rule` is not empty and names nothing in the grammar. |
| `input chunk was dropped before finish` | `tinyparse_finish` | the result of the failed `tinyparse_push` | A push failed, for example out of memory, so the input is incomplete and the parse does not run. |

The `tinyparse` command prints a compile or load fault as `error: FAULT` and
exits with status 1; for a failed parse it prints the messages listed under
`run` in [tinyparse(1)](../../tinyparse.1.md).
