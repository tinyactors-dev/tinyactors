# `tinyparse`: runtime parser generation for freestanding C

## Core value proposition

`tinyparse` is a small, embeddable C library that compiles grammar data into running parsers at runtime.

It is a pocket knife for C programs that need parsing without adopting a generator-heavy toolchain, a scripting runtime, libc, malloc, filesystems, or OS assumptions.

The core promise:

> Give `tinyparse` grammar bytes and input bytes; it gives you a streaming parser, a concrete syntax tree, and structured diagnostics.

`tinyparse` sits at Tree-sitter's level of abstraction but centers itself elsewhere:

- Tree-sitter: ahead-of-time language modules plus a compact parser runtime.
- `tinyparse`: runtime grammar compilation and execution, in ordinary freestanding C.

## Design mantra

`tinyparse` is not primarily a compiler-compiler.

It is:

> a freestanding C runtime for creating and running parsers from grammar data.

Generated C is an optimization. Runtime grammar execution is the default.

## Non-goals

`tinyparse` should not become, at least in its core:

- a linting platform
- a codemod framework
- a language server framework
- a semantic analysis engine
- a build-system-dependent generator
- a scripting environment
- a Tree-sitter clone
- a product stack around code intelligence

Those things may be built on top later. They do not belong in the core.

## Core constraints

The core library must be:

- freestanding C
- compiled with Zig as a freestanding target
- no libc dependency
- no IO
- no filesystem
- no stdout/stderr
- no `malloc`, `realloc`, or `free`
- no threads
- no clock
- no locale
- no global mutable state
- host-allocated
- host-pumped
- deterministic by default

Public headers should depend only on:

```c
#include <stddef.h>
#include <stdint.h>
```

No public API should require null-terminated strings.

## Public naming

The library name is:

```text
tinyparse
```

The public C symbol prefix is:

```text
tinyparse_
```

Public names should avoid cryptic abbreviations.

Prefer:

```c
tinyparse_result
tinyparse_diagnostic
tinyparse_configuration
tinyparse_source
tinyparse_parser
```

Avoid:

```c
tinyparse_rc
tinyparse_diag
tinyparse_cfg
tinyparse_src
```

The public API should optimize for clarity over short names.

## String model

Since `tinyparse` does not depend on libc, the public string type should be length-prefixed/slice-based.

```c
typedef struct tinyparse_string {
    const uint8_t *pointer;
    uint32_t length;
} tinyparse_string;
```

Length is authoritative. A trailing NUL may exist, but is never required.

Use `tinyparse_string` for:

- source names
- grammar bytes
- start rule names
- module names
- token names
- rule names
- field names
- node kind names
- diagnostic messages
- trace messages
- plug names
- binding names

Avoid `const char *` in the core API.

## Memory model

The host grants memory to `tinyparse`.

```c
typedef struct tinyparse_memory {
    void   *base;
    size_t  size;
    void  *(*grow)(void *user, size_t need, size_t *got);
    void   *user;
    size_t  chunk_minimum;
} tinyparse_memory;
```

`grow` may be `NULL`. When present it is asked for at least `need` bytes and
writes the block's true usable size back through `got`; a host that returns
exactly `need` simply sets `*got = need`. The grant goes straight to the
library's arena allocator (`src/arena.c`), which asks `grow` for a fresh block
when the current one is spent. It never asks for a `realloc`, so no address the
engine handed out ever moves.

`tinyparse` never silently allocates outside this grant.

On WebAssembly the grant is linear memory itself. The host-injected
`WebAssembly.Memory` is the backing store: `grow` calls
`__builtin_wasm_memory_grow` (the `memory.grow` instruction), reads the current
extent with `__builtin_wasm_memory_size`, and starts the bump pointer at the
linker's `__heap_base`. A statically reserved buffer would cap the parser and
waste pages, so the grant uses the growable injected memory directly.

Suggested internal arenas:

```text
system arena
  engine state
  intern tables
  compiled grammar registry

grammar arena
  grammar intermediate representation
  symbol table
  token tables
  rule tables
  bytecode

parser arena
  parser instance
  concrete syntax tree node array
  token records
  trivia records
  error records
  diagnostics

scratch/step arena
  temporary work during compile/parse steps
```

## Public shape

The central public API should be compact and explicit.

```c
typedef struct tinyparse_engine  tinyparse_engine;
typedef struct tinyparse_grammar tinyparse_grammar;
typedef struct tinyparse_parser  tinyparse_parser;
typedef struct tinyparse_tree    tinyparse_tree;

typedef struct tinyparse_source {
    tinyparse_string name;
    tinyparse_string bytes;
} tinyparse_source;

typedef enum tinyparse_result {
    TINYPARSE_OK = 0,
    TINYPARSE_MORE,
    TINYPARSE_DONE,
    TINYPARSE_OUT_OF_MEMORY,
    TINYPARSE_INVALID,
    TINYPARSE_SYNTAX_ERROR,
    TINYPARSE_ERROR
} tinyparse_result;

typedef struct tinyparse_configuration {
    tinyparse_memory memory;
    uint32_t flags;
} tinyparse_configuration;

tinyparse_engine *tinyparse_open(
    tinyparse_configuration configuration
);

void tinyparse_close(
    tinyparse_engine *engine
);

tinyparse_result tinyparse_compile_scheme(
    tinyparse_engine  *engine,
    tinyparse_source   source,
    tinyparse_grammar **out_grammar
);

tinyparse_result tinyparse_load_binary_grammar(
    tinyparse_engine  *engine,
    tinyparse_source   source,
    tinyparse_grammar **out_grammar
);

void tinyparse_grammar_free(
    tinyparse_grammar *grammar
);

tinyparse_result tinyparse_parser_new(
    tinyparse_engine  *engine,
    tinyparse_grammar *grammar,
    tinyparse_string   start_rule,
    tinyparse_parser **out_parser
);

void tinyparse_parser_free(
    tinyparse_parser *parser
);

tinyparse_result tinyparse_push(
    tinyparse_parser *parser,
    tinyparse_string  bytes
);

tinyparse_result tinyparse_finish(
    tinyparse_parser *parser
);

const tinyparse_tree *tinyparse_parser_tree(
    const tinyparse_parser *parser
);

uint32_t tinyparse_parser_diagnostic_count(
    const tinyparse_parser *parser
);

const tinyparse_diagnostic *tinyparse_parser_diagnostic(
    const tinyparse_parser *parser,
    uint32_t index
);

tinyparse_string tinyparse_fault(
    const tinyparse_engine *engine
);
```

Typical usage:

```c
tinyparse_engine *engine = tinyparse_open(configuration);

tinyparse_grammar *grammar = 0;
tinyparse_compile_scheme(engine, grammar_source, &grammar);

tinyparse_parser *parser = 0;
tinyparse_parser_new(engine, grammar, start_rule, &parser);

tinyparse_push(parser, input_chunk);
tinyparse_push(parser, next_chunk);
tinyparse_finish(parser);

const tinyparse_tree *tree = tinyparse_parser_tree(parser);
```

## Core data model

`tinyparse` produces a concrete syntax tree by default.

The concrete syntax tree should preserve enough information for tooling, diagnostics, and later transformations:

- node kind
- child order
- named fields
- token spans
- byte offsets
- trivia
- error extras
- missing/inserted recovery tokens
- diagnostic links

Canonical position is byte offset.

Line/column should be derived from a line map, not treated as the primary coordinate system.

```c
typedef struct tinyparse_point {
    uint32_t row;
    uint32_t column;
} tinyparse_point;

typedef struct tinyparse_range {
    uint32_t start_byte;
    uint32_t end_byte;
    tinyparse_point start_point;
    tinyparse_point end_point;
} tinyparse_range;
```

Byte columns are the shipped behaviour. UTF-16/LSP/display-cell column
policies are [next steps](#next-steps).

## Concrete syntax tree storage

Concrete syntax tree nodes live in a contiguous array. The cursor API, the
binary packer, the C emitter, and the later pattern engines all read that one
layout, so it is fixed by design rather than free for the engine to change.

Internally, the tree stores records like:

```c
typedef struct tinyparse_node_record {
    uint32_t kind;
    uint32_t start_byte;
    uint32_t end_byte;

    uint32_t first_child;
    uint32_t child_count;

    uint32_t first_field;
    uint32_t field_count;

    uint32_t flags;
} tinyparse_node_record;
```

Properties of this model:

- nodes are compact
- traversal is cache-friendly
- cursor navigation is fast
- tree ownership is simple
- no per-node allocation
- serialization is straightforward
- packed grammars and generated parsers can use the same layout
- later pattern engines can scan nodes efficiently

The tree may also contain contiguous side arrays:

```text
node records
child indices
field records
token records
trivia records
error records
diagnostic records
string table
symbol table
line map
```

Node identity is represented internally by an index into the node array, but the public access API is cursor-based only.

## Cursor-only tree access

Tree navigation should be cursor-based only.

This is deliberate. A cursor can cache the current node index, parent position, child offset, sibling ranges, and tree pointer, which makes navigation faster than repeatedly looking up node handles through random-access functions.

There should be no public `tinyparse_node` type in the main API.

Instead:

```c
typedef struct tinyparse_tree_cursor tinyparse_tree_cursor;

typedef struct tinyparse_symbol {
    uint32_t id;
} tinyparse_symbol;
```

Create and release a cursor:

```c
tinyparse_result tinyparse_tree_cursor_new(
    const tinyparse_tree *tree,
    tinyparse_tree_cursor **out_cursor
);

void tinyparse_tree_cursor_free(
    tinyparse_tree_cursor *cursor
);
```

Position it:

```c
void tinyparse_tree_cursor_reset(
    tinyparse_tree_cursor *cursor,
    const tinyparse_tree *tree
);

void tinyparse_tree_cursor_reset_to_root(
    tinyparse_tree_cursor *cursor
);
```

Inspect current node:

```c
tinyparse_symbol tinyparse_tree_cursor_kind(
    const tinyparse_tree_cursor *cursor
);

tinyparse_string tinyparse_tree_symbol_name(
    const tinyparse_tree *tree,
    tinyparse_symbol symbol
);

uint32_t tinyparse_tree_cursor_start_byte(
    const tinyparse_tree_cursor *cursor
);

uint32_t tinyparse_tree_cursor_end_byte(
    const tinyparse_tree_cursor *cursor
);

uint32_t tinyparse_tree_cursor_child_count(
    const tinyparse_tree_cursor *cursor
);

uint32_t tinyparse_tree_cursor_depth(
    const tinyparse_tree_cursor *cursor
);

uint32_t tinyparse_tree_cursor_index_in_parent(
    const tinyparse_tree_cursor *cursor
);
```

Navigate:

```c
int tinyparse_tree_cursor_goto_parent(
    tinyparse_tree_cursor *cursor
);

int tinyparse_tree_cursor_goto_first_child(
    tinyparse_tree_cursor *cursor
);

int tinyparse_tree_cursor_goto_last_child(
    tinyparse_tree_cursor *cursor
);

int tinyparse_tree_cursor_goto_next_sibling(
    tinyparse_tree_cursor *cursor
);

int tinyparse_tree_cursor_goto_previous_sibling(
    tinyparse_tree_cursor *cursor
);

int tinyparse_tree_cursor_goto_child(
    tinyparse_tree_cursor *cursor,
    uint32_t child_index
);

int tinyparse_tree_cursor_goto_first_child_by_field(
    tinyparse_tree_cursor *cursor,
    tinyparse_string field
);

int tinyparse_tree_cursor_goto_first_child_by_field_id(
    tinyparse_tree_cursor *cursor,
    tinyparse_symbol field
);
```

Copy cursor state:

```c
tinyparse_result tinyparse_tree_cursor_clone(
    const tinyparse_tree_cursor *source,
    tinyparse_tree_cursor **out_cursor
);

void tinyparse_tree_cursor_assign(
    tinyparse_tree_cursor *target,
    const tinyparse_tree_cursor *source
);
```

This makes traversal code look like:

```c
tinyparse_tree_cursor *cursor = 0;
tinyparse_tree_cursor_new(tree, &cursor);

tinyparse_tree_cursor_reset_to_root(cursor);

if (tinyparse_tree_cursor_goto_first_child(cursor)) {
    do {
        tinyparse_symbol kind =
            tinyparse_tree_cursor_kind(cursor);

        /* inspect current node */
    } while (tinyparse_tree_cursor_goto_next_sibling(cursor));
}

tinyparse_tree_cursor_free(cursor);
```

For allocation-sensitive hosts, provide a stack-allocated cursor variant:

```c
typedef struct tinyparse_tree_cursor_storage {
    const tinyparse_tree *tree;
    uint32_t current_node_index;
    uint32_t parent_node_index;
    uint32_t child_index;
    uint32_t depth;
    uint32_t first_child;
    uint32_t child_count;
    uint32_t flags;
    uint32_t reserved[8];
} tinyparse_tree_cursor_storage;

void tinyparse_tree_cursor_init(
    tinyparse_tree_cursor_storage *storage,
    const tinyparse_tree *tree
);
```

The opaque heap-allocated cursor is ergonomic. The storage form is freestanding-friendly and avoids cursor allocation.

## Diagnostics

Diagnostics are structured data. The core never prints.

```c
typedef enum tinyparse_severity {
    TINYPARSE_SEVERITY_NOTE = 0,
    TINYPARSE_SEVERITY_WARNING,
    TINYPARSE_SEVERITY_ERROR
} tinyparse_severity;

typedef struct tinyparse_label {
    uint32_t start_byte;
    uint32_t end_byte;
    tinyparse_string message;
    uint32_t flags;
} tinyparse_label;

typedef struct tinyparse_diagnostic {
    tinyparse_severity severity;
    tinyparse_string code;
    tinyparse_string message;

    const tinyparse_label *labels;
    uint32_t label_count;

    tinyparse_string help;
    tinyparse_string note;
} tinyparse_diagnostic;
```

The CLI may render rustc-style diagnostics. Embedded hosts can inspect the data directly.

Diagnostics may come from:

- malformed grammar
- unexpected token
- unexpected EOF
- missing expected token
- skipped token
- error extras
- recovery insertion
- invalid byte/character
- unterminated lexical construct

## Native grammar format

The native grammar surface is `.scm`.

It is Scheme-shaped data, not executable Scheme.

The reader should be tiny:

- lists
- symbols
- keyword symbols ending in `:`
- strings
- integers
- byte/hex literals
- line comments
- whitespace

No general Scheme evaluation. No reader macros.

Example:

```scheme
(tinyparse version: "0.1")

(module examples.sexpr)

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

Keyword style is consistently suffix-based:

```scheme
start:
name:
as:
names:
field:
message:
help:
insert:
```

## Grammar forms

The top-level forms:

```scheme
(tinyparse version: "0.1")
(module name)

(grammar name
  start: RuleName
  ...)

(token Name ...)
(extras ...)
(rule Name ...)
```

Rule combinators:

```scheme
(seq ...)
(choice ...)
(repeat X)
(repeat X min: 1)
(repeat X min: 0 max: 3)
(optional X)
(field name: X)
(alias public-name: X)
```

Token primitives:

```scheme
(token Plus "+")
(token Ident (regex "[A-Za-z_][A-Za-z0-9_]*"))
(token String
  (seq
    "\""
    (repeat
      (choice
        (seq "\\" any)
        (not "\"" any)))
    "\""))
```

The regex engine, if present in core, must be owned by `tinyparse`: no libc regex, no locale, no backreferences, no lookbehind. The character-class escapes are `\n \t \r \f \v \0`, the class metacharacters `\\ \] \^ \-`, and `\xHH` — a raw byte by two hex digits. `\xHH` is how a class names a byte the `.scm` string syntax cannot hold directly: a high byte such as `\x80`, which is how a UTF-8 lead or continuation byte enters an identifier class. So an identifier that admits UTF-8 letters is `(regex "[A-Za-z_$\\x80-\\xFF]")` followed by its continuation class — a coarse rule (it accepts any non-ASCII byte) but enough for a byte-oriented lexer, and it keeps the engine free of a Unicode property database.

### Ordered choice of literals is first-match, not longest-match

A `(choice "a" "ab")` of bare-literal tokens takes the first listed literal that
matches at the cursor, not the longest. Two literals that share a prefix must be
ordered longest-first, or the shorter one wins and the longer is never reached:
write `(choice "typeof" "type")`, `(choice ">>>" ">>" ">")`, `(choice "{|" "{")`.
This is the one non-obvious rule a grammar author must hold; a future engine
longest-match for literal choices would retire it.

### Reserved words

A word token must not swallow a reserved spelling. Without a guard, an `identifier` pattern matches `case` or `default` as a plain identifier, and the keyword-led rule that needs the word — an empty fall-through `switch` arm, say — is never reached. The `(reserved …)` token combinator closes that gap:

```scheme
(token identifier
  (reserved
    (seq (regex "[A-Za-z_$]") (repeat (regex "[A-Za-z0-9_$]")))
    "break" "case" "catch" "class" "const" "..."))
```

`(reserved INNER "w1" "w2" …)` matches `INNER`, then rejects the match if its byte slice spells one of the reserved words exactly. Equality is whole-slice — `class` is rejected but `classy` is not — so a longer identifier with a reserved prefix still matches. A grammar typically reserves only the words the language forbids as identifiers and leaves the contextual keywords (a JavaScript `of`, `as`, `get`, `set`, `async`, `static`, `let`) matchable, since the language also admits those as names. Where a reserved word is itself a legal name — a property key, a member name, an import/export name — the grammar uses a second, unguarded word token for that position.

## Extras

`extras` are implicit tokens consumed between grammar elements.

They are used for things like whitespace, comments, BOMs, invalid bytes, and recoverable junk.

Extra policies:

```scheme
(extras
  (trivia Space)
  (trivia Comment)
  (skip Bom)
  (error InvalidByte))
```

Meanings:

```text
trivia X
  consume implicitly and preserve as trivia

skip X
  consume implicitly and discard

error X
  consume implicitly, preserve as error material, and emit a diagnostic
```

No `trivia-detached` in core. Whether a comment is leading, trailing, detached, doc comment, or directive is a tooling/formatter policy, not a grammar-level extra kind.

Extras are useful, but they must be scoped/disableable because some languages have significant whitespace or context-sensitive comments. A rule carries an optional `extras:` modifier between its name and its body:

```scheme
(rule TclWord
  extras: none
  (repeat1 WordPart))
```

`extras: none` suppresses implicit trivia/skip/error consumption for the rule and the whole subtree it parses, so a token-like rule holds its bytes literally — a `//` or a run of spaces inside a JavaScript template literal is content, not a comment or whitespace. The setting nests and a failed match restores it, so it never leaks past the rule.

`extras: all` is its inverse: it re-enables ordinary extras for a rule reached inside an `extras: none` scope. A JavaScript template literal is `extras: none` over its parts, but the expression inside a `${ … }` substitution is `extras: all`, so the hole's expression gets whitespace and comments back while the surrounding text does not. The same pair scopes JSX: the child list and attribute strings are `extras: none` (their whitespace is significant text), while a `{ … }` JSX expression hole is `extras: all`.

> A rule's `extras:` modifier accepts only `none` (suppress) or `all` (re-enable). The two compose by nesting; the active setting is the innermost one a rule selected.

## Error extras

`(error X)` is for known-bad material that should be consumed and diagnosed without derailing the parse.

Examples:

- invalid byte
- invalid UTF-8
- illegal character
- unterminated block comment
- malformed escape
- junk between otherwise valid forms

Example:

```scheme
(token InvalidByte any)

(extras
  (trivia Space)
  (error InvalidByte))
```

Input:

```text
foo @ bar
```

Can produce:

```text
Identifier "foo"
ERROR InvalidByte "@"
Identifier "bar"
```

and a diagnostic:

```text
error: unexpected character `@`
```

This is distinct from grammar recovery.

- Error extras handle bad material between grammar elements.
- Recovery handles missing or malformed structure inside grammar rules.

## Recovery

Recovery should eventually be grammar-authored.

Example:

```scheme
(rule List
  (recover
    (seq
      (field open: "(")
      (repeat Form)
      (field close: ")"))
    (on eof
      message: "unterminated list"
      (label open: "list starts here")
      (label here: "expected `)` before end of file")
      help: "add a closing `)`"
      insert: ")")))
```

The data model supports the full recovery vocabulary:

- missing token
- inserted token
- skipped token
- error node
- diagnostic label
- help text

## Lexing model

`tinyparse` should not copy Tree-sitter's external scanner model as its default escape hatch.

Instead:

> hard lexical constructs should be grammar-defined state machines.

That means strings, templates, regex literals, nested comments, heredocs, indentation, and similar constructs should be expressible inside the grammar/runtime.

Avoid this as the normal model:

```scheme
(extern-token String scanner: "c.string")
```

Prefer:

```scheme
(token String
  (seq
    "\""
    (repeat
      (choice
        (seq "\\" any)
        (not "\"" any)))
    "\""))
```

The scanner is not just a regex matcher. It runs lexical machines the grammar defines.

Useful lexical primitives:

```scheme
any
(byte #x00)
(bytes "PNG")
(range #x30 #x39)
(not "\"" any)
(until "\n")
(delimited open: "\"" close: "\"" escape: "\\")
(balanced open: "{" close: "}")
```

Of these the engine recognizes `any` and `(not …)` today; the rest are
[next steps](#next-steps).

## Host bindings

`tinyparse` may still need host-provided primitives, but they should not be the normal way to parse tokens.

Host bindings are for things that are genuinely host-specific or too large for the freestanding core:

- Unicode category tables
- identifier classification policy
- semantic predicates
- host-defined validation
- optional intrinsic actions

Example:

```scheme
(token Ident
  (seq
    (class xid-start)
    (repeat (class xid-continue))))
```

`xid-start` and `xid-continue` may be built-ins or host-bound classes.

Binding kinds:

```c
typedef enum tinyparse_binding_kind {
    TINYPARSE_BIND_CLASS,
    TINYPARSE_BIND_PREDICATE,
    TINYPARSE_BIND_ACTION
} tinyparse_binding_kind;
```

The engine dispatches `TINYPARSE_BIND_CLASS`; `TINYPARSE_BIND_PREDICATE`
and `TINYPARSE_BIND_ACTION` are reserved and covered under
[Next Steps](#next-steps).

A last-resort host-token escape hatch may exist later, but it should be non-idiomatic and clearly less portable.

## Lexical goals

Parser-directed lexing should be a major differentiator.

Some languages require parser context to decide how to scan the next token.

JavaScript `/` is the classic example:

- division operator in one context
- regex literal opener in another

So the parser should be able to select a lexical goal before scanning.

Sketch:

```scheme
(lex-goal expr
  slash: RegexLiteral)

(lex-goal div
  slash: "/")
```

Template strings, heredocs, Tcl words, and shell contexts benefit from the same idea.

The intermediate representation and runtime carry multiple lexical goals, and the goal stack nests, so a `${ … }` hole inside a template can push and pop its own goal.

## Heredocs and deferred bodies

Heredocs are not special-cased. They fall out of three generic grammar
primitives plus a parametrized lexical goal:

- `(deferred-slot NAME)` declares a per-invocation FIFO slot.
- `(defer SLOT PATTERN)` matches `PATTERN` in place, captures its bytes, and
  enqueues them on `SLOT` as a delayed obligation.
- `(parse-deferred SLOT RULE)` drains `SLOT`, invoking `RULE` once per queued
  item — the point where the earlier delimiter's body is finally parsed and
  attached under its concrete-syntax-tree owner.
- `(lex-goal GOAL key: (param NAME BODY))` is the parametrized goal: at drain
  time `parse-deferred` instantiates `BODY` by substituting the captured
  delimiter bytes for `NAME`, so the closing-delimiter line is matched by the
  bytes seen at the opening.

A shell grammar uses them like this (the full example is `examples/heredoc`):

```scheme
(token HeredocDelimLine)   ; no static pattern; instantiated per invocation

(lex-goal heredoc-body-mode close
  HeredocDelimLine: (param delimiter (seq delimiter "\n")))

(rule Redirect
  (seq
    (field op: "<<")
    (field word: (defer heredoc-bodies Word))))

(rule CompleteCommand
  (seq
    (deferred-slot heredoc-bodies)
    List
    Terminator
    (parse-deferred heredoc-bodies HeredocBody)))
```

Each `deferred-slot` is a fresh instance, so a `$(cat <<INNER … INNER)` nested
inside a heredoc body gets its own queue and never interferes with the outer
one. This keeps hard lexical structure declarative, in the grammar rather than in a hand-written C scanner.

## Operational model

A grammar is compiled once, then run over input many times. The two phases
have very different shapes.

**Compilation** turns `.scm` source into a runnable program:

```text
1. read .scm forms
2. validate the grammar
3. intern names (kinds, fields, tokens)
4. compile tokens and lexical machines
5. compile grammar rules to an intermediate representation
6. lower the intermediate representation to FLAT BYTECODE
7. return tinyparse_grammar
```

Step 6 is the load-bearing one: a rule does not become a state machine or a
long-lived object. It is lowered to a flat array of bytecode instructions — sequence, choice,
repeat, call, emit, and the rest — that a small virtual machine executes.
The bytecode is compact and cache-friendly, which is what makes the parse
fast. The analysis that chose the instruction set is archived in
[`docs/archive/bytecode-vm.md`](bytecode-vm.md).

**Parsing** runs that bytecode over the buffered input:

```text
1. host calls tinyparse_push(parser, bytes)
2. the bytes are appended to the parser's input buffer, in its arena
3. host calls tinyparse_finish(parser)
4. the bytecode VM runs its dispatch loop over the whole buffer
5. the VM scans tokens, backtracks, and appends node records to the
   contiguous concrete-syntax-tree arrays as it reduces
6. parser returns TINYPARSE_OK / TINYPARSE_DONE / TINYPARSE_SYNTAX_ERROR
```

The dispatch loop is the engine. It runs on an arena-backed frame stack rather
than C recursion, so deeply nested input fails with a diagnostic or runs out of
its memory grant instead of overflowing the C stack (`gate:vm-nocrash` parses
200,000 nested `[` under a 512 KiB stack). An adaptive rule memo stays dormant
until a parse goes super-linear, then caches rule results by rule, offset and
lexical goal; `TINYPARSE_RULEMEMO=always` or `off` forces it, and
`gate:vm-diff` holds all three settings to the same tree. An earlier
recursive-descent executor served as the reference oracle during bring-up and
was retired once the VM matched it byte for byte.

The host owns the outside world throughout. The engine never blocks, reads,
writes, sleeps, or allocates outside its memory grant.

## Parser lifecycle

The public surface is four opaque handles:

```c
tinyparse_engine
tinyparse_grammar
tinyparse_parser
tinyparse_tree
```

A parser moves through four states, declared as `tp_parser_state` in
`src/internal.h` and driven by `src/parse.c`:

```text
               push ok                     push fails
(parser_new) ACCEPTING --------> ACCEPTING ----------> DROPPED
                  |                                      |
                  | finish / run                         | finish / run
                  v                                      v
              FINISHED                                FAILED
  (the executor ran over the buffer;       (the executor never ran; finish
   the tree and diagnostics are built)      returns the push's failure)
```

`ACCEPTING` buffers each pushed chunk. A push that cannot buffer its bytes moves
the parser to `DROPPED` and records the result; later pushes still buffer, but
the input is known incomplete, so finishing never parses it. `FINISHED` and
`FAILED` are final: a push there returns `TINYPARSE_INVALID`, and a second
finish reports whether a tree was built.

One function, `tp_parser_run(parser, executor)`, makes the finishing transition,
and three callers share it with different executors: `tinyparse_finish` runs the
bytecode VM, a generated parser's `<prefix>_run` runs its generated code, and an
injected sub-parse runs whichever of the two is registered for its language. So
every way of parsing refuses a truncated input the same way.

The conceptual lowering is "a rule becomes bytecode the VM runs":

```scheme
(rule List
  (seq "(" (repeat Form) ")"))
```

compiles to a bytecode fragment (match `"("`, loop matching `Form`, match `")"`,
emit a `List` node) that the VM executes inline, appending records to the
contiguous tree arrays as it reduces.

This shape replaced an actor. In the `ax` repository, where tinyparse began as
pocketparse, the parse ran inside one `ax` actor: `tinyparse_push` posted an
`input.chunk` event and `tinyparse_finish` posted `input.end`, whose action ran
the whole VM. An earlier experiment with one actor per grammar rule had measured
5 to 15 times slower than a plain loop. The one-actor design cost about 1%, but
it tied the library to `ax`. The four-state machine keeps every behaviour the
actor had and needs no runtime; [`HISTORY.md`](../../HISTORY.md) records the change,
and [`docs/archive/ax-topologies.md`](ax-topologies.md) the
topologies that were weighed.

### Language injection

A region of one grammar's source can be parsed by a second grammar and spliced
into one composite tree, with each region carrying the language it was parsed by.
A grammar marks the region declaratively:

```scheme
(inject grammar: std.javascript content: REGION)
```

`REGION` is an ordinary sub-pattern that delimits the embedded span; `grammar:`
names the embedded grammar, resolved and compiled once at compile time through
the engine's module resolver, so the parse path never reads the filesystem. When
the VM reaches the inject node it first matches `REGION` (advancing the host
parser over the span exactly as for any sub-pattern), then re-parses those same
bytes with the embedded grammar and splices the embedded tree under an
`injection` node. Three pieces of plumbing keep the splice sound: byte spans are
shifted by the span's absolute start (the child parses as if the span began at
byte 0); the child's kind and field names are re-interned into the host grammar's
one name table; and every spliced node is stamped with a **language tag** naming
the embedded grammar, which `--dump-tree` surfaces as a `[lang]` annotation. The
tag is internal CST metadata, not a public accessor — the public API is
unchanged. The result is a single composite, language-tagged tree (a JavaScript
subtree under a `js` fence, JSON under a `json` fence); `std.fence` is the
demonstrator grammar.

The embedded parse runs inline, as a synchronous sub-parse over the span. It
uses a throwaway sub-parser that goes through the same lifecycle as any other
(`tp_parser_run`). When the host registered a generated parser for the embedded
language, that parser runs; otherwise the VM does, and both build the same
tree. `gate:nesting` checks that the registered parsers did run, since a
fallback to the VM would build an identical tree and pass unnoticed.

## Plugs and bindings

Two extension mechanisms cover the host boundary:

```text
plugs
  message-style host boundaries: the engine calls a named plug's emit
  useful for module loading, tracing, diagnostics, package registries

bindings
  direct named host functions
  useful for hot predicates, byte/classes, actions
```

Public plug sketch:

```c
typedef struct tinyparse_message {
    tinyparse_string name;
    tinyparse_string target;
    tinyparse_string data;
    uint32_t flags;
} tinyparse_message;

typedef struct tinyparse_plug {
    tinyparse_string name;
    tinyparse_result (*emit)(
        void *user,
        const tinyparse_message *message
    );
    void *user;
} tinyparse_plug;

tinyparse_result tinyparse_plug_install(
    tinyparse_engine *engine,
    const tinyparse_plug *plug
);
```

Use plugs for:

- module loading
- package resolution
- trace sink
- diagnostic sink
- async/offloaded host services

Do not use plugs for:

- normal token delivery
- ordinary rule transitions
- ordinary concrete syntax tree construction
- hot scanner loops

Use direct bindings for host-provided primitives:

```c
typedef struct tinyparse_binding {
    tinyparse_string name;
    tinyparse_binding_kind kind;
    void *function;
    void *user;
} tinyparse_binding;

tinyparse_result tinyparse_bind(
    tinyparse_engine *engine,
    const tinyparse_binding *binding
);
```

## Module loading

The core has no filesystem, so imports are host-mediated.

Grammar:

```scheme
(import std.lex)
(import languages.javascript as: js)
```

The compiler asks the module plug:

```text
module.load "std.lex"
```

The host may resolve this from:

- static ROM table
- embedded blobs
- packed grammar archive
- filesystem, in CLI builds
- application registry
- network, outside core

The core only sees bytes.

## Packed grammars

Runtime `.scm` compilation is the default, but fast loading should be possible.

```c
tinyparse_result tinyparse_compile_scheme(
    tinyparse_engine  *engine,
    tinyparse_source   source,
    tinyparse_grammar **out_grammar
);

tinyparse_result tinyparse_load_binary_grammar(
    tinyparse_engine  *engine,
    tinyparse_source   source,
    tinyparse_grammar **out_grammar
);
```

The binary grammar format contains:

- version
- ABI
- string table
- symbol table
- token tables
- rule intermediate representation
- extras and their policies
- lexical goals
- node metadata

Still no IO. The host supplies the bytes.

CLI:

```sh
tinyparse pack grammar.scm --out grammar.tpg
```

## Optional C emitter

Generated C is useful, but not central.

CLI:

```sh
tinyparse c grammar.scm --out grammar.c
```

Two possible modes:

```text
module mode
  emits grammar descriptor/tables
  links against libtinyparse

bundle mode
  emits single-file parser convenience artifact
```

Docs should lead with runtime grammar compilation, not code generation.

## Grammar packages

A grammar travels as a package: a directory the compiler resolves by name,
version, and feature set. The core still sees only bytes, so a package is a
convention layered over the module plug rather than a new core concept.

A package has a fixed layout:

| File | Role |
|------|------|
| `package.scm` | manifest: name, version, parameters, `extends` |
| `grammar.scm` | the grammar source |
| `node-types.json` | generated node schema, the typed surface tools read |
| `queries/*.scm` | bundled queries (highlights, locals, and so on) |
| `corpus/*.txt` | input-to-expected-tree cases, the conformance suite |

A package holds our own work and nothing else. `grammars/std` is for the
grammars we author and own; a third-party grammar we port from is a build-time
reference, kept outside the package tree and never copied into `grammars/std`.

The manifest is ordinary `.scm` data:

```scheme
(package
  name: std.javascript
  version: "0.1.0"
  params: (jsx: #f types: #f)
  provides: (Program))
```

Name resolution draws on two sources the host supplies: a search path and an
embedded registry. The search path is filesystem directories, used by CLI
builds. The embedded registry is a table compiled into the binary, so a
freestanding or single-binary build resolves `std.javascript` with no
filesystem at all. The build bakes each package into that table as a generated C
byte array, the same mechanism that embeds the manual page, so the registry
ships inside the binary with no external files. A C codebase built through
`zig cc` cannot use Zig's `@embedFile` (that is a `.zig` feature), and C23
`#embed` would leave the package as a separate file the single-file
amalgamation cannot carry, so the generated array is the portable choice.
A package found on the path shadows the embedded copy, so a host overrides a
built-in grammar without a rebuild.

A port is reproducible because its reference is pinned. The third-party grammar
is a build-time porting reference, never linked or shipped, so it is kept under
a separate `reference/` tree, pinned by commit (a git submodule, or a pinned
tarball where the network policy forbids submodules). Its `test/corpus` is
imported once into the package's own `corpus/` as our test data. "Did the port
stay faithful" is then a script that exits zero rather than a judgement call,
and the package under `grammars/std` carries no foreign code.

## Grammar composition

The standard grammars are one parameterized family. JavaScript, JSX,
TypeScript, and TSX are a single grammar with two feature parameters, not four
hand-maintained forks.

`extends` pulls in a base package; `params` selects features:

```scheme
(package name: std.tsx version: "0.1.0"
  extends: (std.javascript jsx: #t)
  params: (types: #t jsx: #t))
```

The manifest fields are sugar over two grammar-level primitives. A grammar
pulls a base in with a top-level `(extends NAME)` form, resolved through the
same host module resolver as `(import …)`, and parameters are supplied by the
host before compilation — `tinyparse_set_param(engine, name, value)`, or the
CLI's `--param NAME[=VALUE]`. The same source compiles to different grammars
under different parameters.

A parameter gates productions when the grammar compiles:

```scheme
(rule PrimaryExpression
  (choice
    Identifier
    Literal
    (when jsx: JsxElement)))
```

A name an extension introduces merges into the base, the way `import` already
merges. Redefining a name the base already defines is a compile error unless the
extension marks it `override`, so a typo cannot silently shadow a base rule:

```scheme
(override (rule FunctionDeclaration
  (seq (optional (field modifier: "macro")) "function" ...)))
```

> An extension MUST wrap any rule or token it redefines in `(override …)`. A
> bare redefinition of a base name is a compile error.

Composition clears two obstacles that tree-sitter hands to a written C scanner
and to its GLR table. tinyparse clears both inside the grammar, so a ported
grammar carries no external scanner and no generator-specific conflict table.

**Automatic semicolon insertion.** A newline ends a statement only when leaving
the semicolon out would fail to parse. A line-terminator lexical goal emits a
newline token that statement rules consume as an optional terminator; where the
terminator is absent, the recovery machinery supplies the implied `;` as a
MISSING node. ASI lives in the grammar, written with primitives the engine
already ships: lexical goals, `optional`, and recovery.

> A standard grammar MUST NOT require a host-supplied token scanner. Every
> lexing decision a tree-sitter external scanner makes is expressed with
> lexical goals and grammar rules.

**Ambiguity and precedence.** Where tree-sitter declares a conflict and lets
the GLR runtime explore both parses, tinyparse layers the productions into a
precedence ladder of ordered choices, the pattern the `jit` example already
uses for expression precedence (ternary over relational over additive, on down
the ladder). The ladder is grammar, so it ports and packs like any other rule
and adds no machinery to the core.

regex-versus-division is the lexical-goal case: the `expr` goal redirects a `/`
trigger to a regex literal where a primary is expected, the `div` goal to the
division operator elsewhere, and the goal stack nests.

Template literals and JSX text are the `extras: none` case, not lexical goals.
Their content is significant — a `//` inside backticks is text, and the spaces
between JSX tags are text — so the rule that scans the content runs under
`extras: none`, and a `${ … }` template hole or a `{ … }` JSX expression hole
re-enters `extras: all` for the JavaScript expression it carries.

**Keyword reservation.** A JavaScript `identifier` must not match a reserved
word, or an empty fall-through `switch` arm — `case 1: case 2:` — parses the
second `case` as an identifier statement and the arm is lost. The `(reserved …)`
token combinator (see *Reserved words*) is the engine primitive for this; the
grammar reserves the forbidden words and uses a separate unguarded word token in
the positions where a reserved word is a legal name (property keys, member
names, import/export names).

**Unicode, within reason.** The corpus needs three Unicode behaviours, all
expressible without a property database. UTF-8 letters in identifiers
(`últimaVez`, `県`) come from admitting any high byte (`\x80-\xFF`) as an
identifier byte. The `\u`-escape identifier forms (`a`, `\u{62}`) are extra
alternatives in the identifier token. The Unicode whitespace and line
terminators — U+00A0, U+FEFF, U+2060, U+200B, U+2028, U+2029 — are literal UTF-8
byte runs in the whitespace token (written with `\xHH`), and U+2028/U+2029 also
end a `//` comment. The rule is coarse (it treats every non-ASCII byte as an
identifier byte) but it keeps the freestanding core free of Unicode tables.

> A standard grammar MAY rely on `\xHH` byte classes for a coarse UTF-8
> identifier rule. Full Unicode identifier classification is a host binding
> (`xid-start`/`xid-continue`), not a core feature.

This confirms the boundary the design bet on: the first real grammar needs a
precedence ladder, reserved-word guarding, and token-scoped extras expressed in
the grammar, and it leaves external token scanners unnecessary.

## Queries as parsers over linearized trees

A query is a parser. The tree to search is linearized to a token stream, and
the compiled query is a tinyparse grammar over that stream. Matching is
parsing, so the query language and the languages it queries run on one engine.

The linearization is a pre-order walk:

| Token | Carries |
|-------|---------|
| `ENTER kind field?` | node kind, optional field name |
| `LEAF text` | leaf token text |
| `EXIT kind` | closes the node |

A tree-sitter pattern compiles to rules over those tokens. The pattern

```scheme
(call_expression function: (identifier) @fn)
```

becomes

```scheme
(seq (enter call_expression)
     (field function: (seq (enter identifier) (leaf) (exit identifier)))
     (repeat any-subtree)
     (exit call_expression))
```

Captures are fields, so the field mechanism already in the engine reports them.
The predicates tree-sitter writes as `#eq?` and `#match?` are byte comparison
and regex, both built into the engine, so the common predicates need no host
binding and a packed query stays self-contained.

A tree-sitter pattern matches at any depth. Descendant matching compiles to an
explicit "skip one balanced subtree" rule built from `repeat` over the
ENTER/EXIT-balanced stream, and the `.` anchor turns that skip off between two
named children. The skip rule is grammar, so it packs with the query.

The query surface is `.scm` data, the same reader the grammars use. A query file
is a sequence of patterns:

```scheme
(Item word: (Word) @w)                       ; field-bound child, captured
(List (Atom) @first . (Atom) @second)        ; positional, `.` anchors adjacency
(_ @any)                                      ; `_` matches any node
((Item word: (Word) @w) (#eq? @w "hi"))      ; a predicate filters the match
```

A `field:` keyword binds a child through its field; `@name` captures the node it
follows; `_` is the wildcard node; `.` anchors two children as adjacent; a
`(#eq? @cap "text")` or `(#match? @cap "regex")` form, written beside the
pattern in a wrapping list, filters a match by byte comparison or the engine's
regex. Two commands drive it: `tinyparse query GRAMMAR QUERY --input FILE`
prints a line per capture (capture name, byte span, text), and `tinyparse
linearize GRAMMAR --input FILE` dumps the ENTER/LEAF/EXIT stream a query parses,
as a debugging view.

## Embedding without a runtime dependency

> A grammar compiles to C that, together with the amalgamation, is a
> self-contained set of translation units. A consumer who has run that one
> generation step depends on no tinyparse tooling to build, link, or ship.

Two generation steps combine. The amalgamation (`tinyparse vendor`, `tinyparse
init`, or `mise run amalgamate`) is the whole library as `tinyparse.c` and
`tinyparse.h`. `tinyparse c` emits the grammar as C in one of four modes:

```text
module mode
  the packed grammar as a byte array, loaded with tinyparse_load_binary_grammar
  and run by the VM

bundle mode
  module mode plus a main() that parses stdin and dumps the tree

--parser / --parser-lib
  a recursive-descent parser generated from the grammar, with or without main()
```

Each links against `libtinyparse` or `tinyparse.c`; none needs the `.scm`
reader at run time, and none needs the CLI. So the supported shipping shapes
are:

- **static binary**: compile the generated `.c` and `tinyparse.c` into the
  executable.
- **napi addon**: compile the same two files into the addon `.node`; the only
  build dependency is the node-api headers.
- **amalgamation drop-in**: add both files to an existing C or C++ tree.

Generation is the single moment tinyparse tooling is required. After it, the
artifact is ordinary C. Queries pack the same way, since a packed query is data
plus predicates already inside the engine, so a tool that ships a grammar and a
set of highlight queries still ships as C units.

## CLI

The CLI is outside the freestanding core and may use libc/OS APIs.

Useful commands:

```sh
tinyparse init my-parser
tinyparse check grammar.scm
tinyparse run grammar.scm --input sample.txt --dump-tree
tinyparse grammars
tinyparse node-types grammar.scm
tinyparse query grammar.scm query.scm --input sample.txt
tinyparse linearize grammar.scm --input sample.txt
tinyparse pack grammar.scm --out grammar.tpg
tinyparse c grammar.scm --out grammar.c
tinyparse vendor my-parser
```

The importers are [next steps](#next-steps), not yet built:

```sh
tinyparse ingest tree-sitter ./tree-sitter-ruby --out ruby.scm
tinyparse abnf import rfc9110.abnf --out http.scm
```

## Seeding a project

`tinyparse init` drops the engine into a new project and writes a starter that
builds with one `cc`. It is the on-ramp from the distribution artifacts: a host
installs tinyparse, then seeds a project from it.

Two modes, matching the two distribution shapes:

| Mode | What it drops | Build |
|------|---------------|-------|
| `--amalgamation` (default) | `tinyparse.c` + `tinyparse.h` copied in | `cc main.c tinyparse.c -o app` |
| `--lib` | nothing copied; the project includes `<tinyparse.h>` | `cc main.c -ltinyparse` |

Amalgamation mode is the self-contained drop:

```text
tinyparse init my-parser
my-parser/
  tinyparse.c     the whole engine
  tinyparse.h     the only header
  grammar.scm     a starter grammar
  main.c          opens the engine, compiles grammar.scm, parses stdin, dumps the tree
  build.sh        cc main.c tinyparse.c -o app
```

Lib mode wires against an installed `libtinyparse` instead of copying source:

```text
tinyparse init my-parser --lib
my-parser/
  grammar.scm
  main.c          #include <tinyparse.h>
  build.sh        cc main.c -ltinyparse -o app
```

The CLI carries the amalgamation as an embedded asset: `tinyparse.c` and
`tinyparse.h` are baked into the binary at build time as a generated byte
array, the same mechanism that embeds the manual page and the grammar registry,
so `init --amalgamation` needs no network and no install prefix to find the
engine source. The starter `main.c` compiles the embedded
`grammar.scm` at runtime through `tinyparse_compile_scheme`, so once seeded
the project depends on no tinyparse tooling, which keeps the embedding
guarantee.

`--grammar std.javascript` seeds from a standard grammar instead of the hello
grammar, copying the package and pointing `main.c` at its start rule. For an
existing project, `tinyparse vendor` drops or refreshes the engine source,
amalgamation or lib headers, without writing a scaffold.

## Next Steps

Everything above this section is built and shipping: the freestanding
`libtinyparse`, the `.scm` reader, runtime compilation, the streaming
`push`/`finish` API, the contiguous cursor-navigated tree, structured
diagnostics, the parser lifecycle, plugs and class bindings, grammar
composition, lexical goals, deferred bodies, language injection, packed
grammars, the C emitter, and the query engine. What follows is the surface the
design anticipates but has not yet implemented, grouped so each item stands on
its own as a next step.

### Grammar and lexer surface

The [Lexing model](#lexing-model) sketches a fuller set of lexical primitives
than the engine recognizes today. Shipped as grammar forms are `any`,
`(not …)`, `EOF`, string literals, `(regex …)`, `(reserved …)`, and
`(class …)`. Not yet exposed are `(byte #xHH)`, `(bytes "…")`, `(range …)`,
`(until …)`, `(delimited open: … close: … escape: …)`, and a general
`(balanced open: … close: …)` — balanced matching exists only inside the query
engine's subtree skip. Each is sugar over patterns the engine already runs, so
adding one is a reader and compiler change rather than an engine change.

Host bindings are half-wired. `tinyparse_bind` and the binding vocabulary
ship, but only `TINYPARSE_BIND_CLASS` is dispatched: a host names a byte
class such as `xid-start` and the token engine calls it per candidate byte.
`TINYPARSE_BIND_PREDICATE` and `TINYPARSE_BIND_ACTION` are declared in the
enum and reserved for a later tranche; their host-function signatures are not
yet defined, so the semantic predicates and intrinsic actions described under
[Host bindings](#host-bindings) wait on them.

### Binary parsing

The same engine can operate over byte fields instead of text tokens, but does
not yet. `mode: binary`, `(endian …)`, the `u32` and `bytes N` field types, and
length-prefixed field references are design sketches with no reader or runtime
support. The intended shape:

```scheme
(grammar png
  mode: binary
  start: PngFile

  (endian big)

  (rule PngFile
    (seq
      (bytes #x89 "PNG" #x0D #x0A #x1A #x0A)
      (repeat Chunk)
      EOF))

  (rule Chunk
    (seq
      (field length: u32)
      (field type: (bytes 4))
      (field data: (bytes length))
      (field crc: u32))))
```

### Importers: ABNF and tree-sitter

Two importers are named in the [CLI](#cli) but unbuilt: `tinyparse abnf
import` and `tinyparse ingest tree-sitter`. IETF ABNF maps naturally into
`.scm` rules —

```abnf
request-line = method SP request-target SP HTTP-version CRLF
```

becomes

```scheme
(rule request-line
  (seq method SP request-target SP HTTP-version CRLF))
```

— but RFC grammars often carry prose constraints, so the importer should
preserve provenance and admit patch modules rather than pretend every ABNF
grammar is executable as written. Tree-sitter ingestion is the analogous path
for the grammars tinyparse ports from; the porting reference lives under
`reference/` and its corpus is imported as test data, but the automated
grammar-to-`.scm` translation is not written.

### Pattern engine: search, lint, codemod, macros

The most compelling extension is running patterns over concrete syntax trees:

```text
bytes -> concrete syntax tree
concrete syntax tree -> pattern matches / diagnostics / rewrites
```

This is the layer beneath structural search, linting, safe rewrites,
migrations, macro expansion, and framework conventions. The lessons worth
carrying from ast-grep and GritQL: snippet-shaped patterns onboard best,
rewrite and fix should be first-class, rule files need composition,
project-scale execution eventually matters, grammar-owned snippet parsing is
the differentiator, and lossless trees make safer patches possible.

The rewrite half already has its shape fixed. Rewriting is text emission, not
tree mutation: the public tree API is cursor-only and read-only, so a rewrite
walks the lossless tree, copies the bytes it does not touch through their
recorded spans, emits replacement text, and re-parses a fragment when it needs
a subtree. The `rewriter` example is the whole pattern, and a `sweetjs`-style
macro expander is the same pattern run to a fixpoint. There is no
tree-construction or edit API, and there should not be one: lossless spans plus
re-parse cover macros, codemods, and migrations, so a reader weighing a "build
a tree and mutate it" feature should stop here.

### Tree fidelity and conformance

Three gaps separate the current output from tree-sitter parity:

- **Conformance is measured at accept level.** A corpus case counts as covered
  when `run` accepts it with no `ERROR`/`MISSING` node. Matching tree-sitter's
  exact S-expression is the stronger bar, still open.
- **Error-recovery trees are detected but not shaped to parity.** tinyparse
  rejects the malformed cases that tree-sitter recovers into an
  `(ERROR)`/`(MISSING)` tree. Both engines catch the fault; the recovered tree
  shapes differ.
- **Comment attachment differs in a handful of cases.** A comment lands under a
  different parent than tree-sitter chooses. The comment is present either way;
  only its attachment differs, a limit of the extras model rather than a
  grammar bug.

### Packaging, columns, and Unicode

A package ships `package.scm` and `grammar.scm` today. `node-types.json` is
generated on demand by `tinyparse node-types` rather than stored, the
`queries/` and `corpus/` directories in the [package layout](#grammar-packages)
are not yet populated (corpus data lives under `test/` and `reference/`), and
the manifest's `provides:` field is parsed but not consumed.

Byte offsets are canonical and byte columns are what the line map derives.
UTF-16, display-cell, and LSP column policies are deferred, as is full Unicode
identifier classification behind the `xid-start`/`xid-continue` host bindings;
the coarse `\xHH` byte-class rule covers the standard grammars' Unicode needs
until then.

### Injection that streams

Language injection runs the embedded grammar as an inline sub-parse over a span
the host parser has already matched. An embedded region that arrives across
several pushes is parsed at finish, like the rest of the input. Parsing it as it
arrives would need a sub-parser the host VM can suspend and resume mid-dispatch,
which the VM does not have.

### Also deferred

Incremental parsing, LSP integrations, project-wide analysis, and external
token scanners as a normal concept remain out of scope. Complex
precedence/conflict machinery is added only where a real grammar needs it; the
precedence-ladder-of-ordered-choices pattern has covered the standard grammars
so far.

## Why `tinyparse` is distinct

`tinyparse` is not "Tree-sitter but in C."

It differs in several core ways:

1. Runtime generation is primary.
   The host can compile grammar bytes into a parser at runtime.

2. The parse is a bytecode VM with an explicit lifecycle.
   Rule calls run on an arena-backed frame stack, so nesting depth is bounded by memory, not by the C stack.

3. The core is freestanding.
   No libc, no malloc, no IO, no OS assumptions.

4. The public API is ordinary C.
   One header, length-prefixed strings, opaque handles; the library depends on no other code.

5. Grammar data is embeddable.
   `.scm` and binary grammar blobs can live in ROM, firmware, application resources, or memory buffers.

6. Lexing is part of the grammar.
   Hard lexical constructs should be grammar-defined machines, not handwritten C scanners by default.

7. Concrete syntax trees and diagnostics are first-class.
   The output is not merely accept/reject; it is structured, spanned, inspectable data.

8. Concrete syntax tree nodes are contiguous.
   Node storage is compact, cache-friendly, serializable, and suitable for later pattern engines.

9. Tree access is cursor-only.
   Navigation is optimized around mutable cursors over contiguous arrays, rather than exposing node handles as the primary public API.

## One-sentence pitch

`tinyparse` is a freestanding C library that compiles grammar data into streaming parsers at runtime, producing contiguous-array concrete syntax trees with cursor-only navigation and structured diagnostics without libc, malloc, IO, or a generator step.

## Short README pitch

`tinyparse` is a pocket-knife parser runtime for C.

It compiles `.scm` grammar data into running parsers at runtime, behind a plain C API. The core is freestanding: no libc, no malloc, no filesystem, no threads, no clock, and no IO. The host provides memory and input bytes; `tinyparse` provides a streaming parser, a contiguous-array concrete syntax tree with cursor-only navigation, and structured diagnostics.

Generated C is available as a deployment optimization. Runtime grammar execution is the main feature.

## Appendix: sweet.js-style macros, end to end

A worked example that exercises composition, rewriting, and the
no-dependency embedding path together. The goal is Common-Lisp-style macros in
JavaScript, declared with a `macro` modifier on a function and returning a new
tree from a template literal:

```js
macro function defrecord(name, ...fields) {
  return `class ${name} { constructor(${fields}) { /* ... */ } }`;
}
```

Three pieces, each already present in the design:

1. **The surface extends the base grammar.** A package `extends: std.javascript`
   adds the `macro` modifier to the function-declaration rule behind a feature
   parameter, so the host grammar is the standard JavaScript port plus one
   production.

2. **Expansion is a tree rewrite.** The rewriter layer (the `cdefines` example
   shows its shape) walks the tree; a `macro` declaration registers a rewrite
   that replaces each matching call site with the macro's result.

3. **The template literal is a quasiquote.** Its content is parsed by the same
   JavaScript grammar as a nested parse, with `${...}` holes spliced in as
   subtrees. Producing a tree from a template literal is parsing the literal
   with a grammar that is already loaded.

Shipping it stays within the embedding guarantee. Run `tinyparse c` once on
the extended package to get a bundle `.c`, then compile that into a static
binary or a napi addon. The macro author's tool carries the grammar, the
rewrite rules, and the engine in one compiled unit, and depends on no
tinyparse install at its own users' build time.
