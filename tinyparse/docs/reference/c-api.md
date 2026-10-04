# C API

This reference is for a C developer writing a host program against
`include/tinyparse.h`. It lists every declaration in the header, the result
codes each function returns, the ownership and threading rules, how much memory
a grant needs, and the entry points of a generated parser.

For a guided first program, see [Parse input from a C program](../tutorials/embed-in-c.md).

## Conventions

### Header

`include/tinyparse.h` is the only public header. It includes `<stddef.h>` and
`<stdint.h>` and nothing else, compiles as C99, and wraps its declarations in
`extern "C"` for C++.

Every function is declared with `TINYPARSE_API`, which is empty unless the
including program defines it. The shared-library build defines it as
`__attribute__((visibility("default")))`; that library exports the functions
of this header and no other symbol.

### Strings

Every string the library takes or returns is a `tinyparse_string`:

```c
typedef struct tinyparse_string {
    const uint8_t *pointer;
    uint32_t       length;
} tinyparse_string;
```

`length` is authoritative. The bytes need no terminating NUL and may contain
NUL bytes. An empty string has `length` 0; its `pointer` may be `NULL`. The one
NUL-terminated string in the API is the return value of
[`tinyparse_version_string`](#tinyparse_version_string).

### Result codes

Functions that can fail return a `tinyparse_result`:

| Code | Value | Meaning | Returned by |
|------|-------|---------|-------------|
| `TINYPARSE_OK` | 0 | The call succeeded. | every function returning `tinyparse_result` |
| `TINYPARSE_MORE` | 1 | The parser needs more input. | a generated parser's `P_run_chunk` only ([Generated parser interface](#generated-parser-interface)) |
| `TINYPARSE_DONE` | 2 | Declared; no function returns it. | none |
| `TINYPARSE_OUT_OF_MEMORY` | 3 | The memory grant is spent and `grow` is `NULL` or returned `NULL`. | parsers, cursors, emitters, `tinyparse_grammar_serialize`, `tinyparse_linearize`, `tinyparse_query`, and the registration functions |
| `TINYPARSE_INVALID` | 4 | A bad argument, a grammar that does not compile, a packed grammar that does not load, an unknown start rule, a malformed query, or a call on a finished parser. | every function returning `tinyparse_result` |
| `TINYPARSE_SYNTAX_ERROR` | 5 | The input does not match the grammar. | `tinyparse_finish`, `P_run`, `P_run_chunk` |
| `TINYPARSE_ERROR` | 6 | Declared; no function returns it. | none |

`tinyparse_compile_scheme` and `tinyparse_load_binary_grammar` report a spent
grant as `TINYPARSE_INVALID` with an "out of memory" fault, never as
`TINYPARSE_OUT_OF_MEMORY`.

### Faults

A fault is a short text the engine keeps after some failed calls. Read it with
[`tinyparse_fault`](#tinyparse_fault).

- A call sets the fault only where its entry below names one. A `NULL` engine,
  grammar or parser argument returns `TINYPARSE_INVALID` without setting it.
- `tinyparse_compile_scheme` and `tinyparse_load_binary_grammar` clear the
  fault when they succeed. No other call clears it, so after any other
  successful call the fault can still hold an older message.
- A fault carries no position. Positions belong to
  [diagnostics](#diagnostics), which a parse produces.

### Ownership and lifetime

The engine allocates everything from the host's [memory grant](#memory-grant)
and never frees anything back to the host. The host frees the grant's `base`
block and every block `grow` returned, after its last call on the engine.

| Object | Lives in | Valid until |
|--------|----------|-------------|
| engine, compiled grammar, bindings, parameters, plugs, registered languages | the engine | the host frees the grant |
| bytes from `tinyparse_grammar_serialize`, `tinyparse_emit_c`, `tinyparse_emit_node_types` | the engine | the host frees the grant |
| allocations made by `tinyparse_linearize` and `tinyparse_query` | the engine; each call adds to them | the host frees the grant |
| parser input buffer, tree, diagnostics, cursors made by `tinyparse_tree_cursor_new` and `tinyparse_tree_cursor_clone` | the parser | `tinyparse_parser_free` on that parser |

- `tinyparse_close`, `tinyparse_grammar_free` and `tinyparse_tree_cursor_free`
  release nothing. Call them where the object's use ends; the memory stays
  allocated until the grant is freed.
- `tinyparse_parser_free` hands the parse's memory back to the engine, which
  reuses it for the next parse. The parser, its tree, its diagnostics and every
  cursor over the tree are invalid after the call.
- A grammar compiled from `.scm` text keeps pointers into that text (the names
  of `(class NAME)` forms among them). The source bytes, and the bytes every
  [module resolver](#module-resolver) call returned, must stay valid and
  unchanged while the grammar is in use. A grammar loaded with
  `tinyparse_load_binary_grammar` copies the image; the host may free the image
  after the call returns.
- `tinyparse_push` copies the input bytes; the host may reuse them after the
  call returns. Text that `tinyparse_linearize` and `tinyparse_query` report is
  a slice of the `input` argument and is valid while that input is.

### Threading

The library has no writable static data, so separate engines can run on
separate threads at the same time. It takes no locks: calls on one engine, and
on any grammar, parser, tree or cursor created from it, must not overlap. A
parse writes to its compiled grammar (it caches the lowered bytecode and lookup
tables there on first use), so a compiled grammar cannot be shared between
threads either.

Class functions, module resolvers, plug `emit` functions, and the callbacks of
`tinyparse_linearize` and `tinyparse_query` run on the calling thread, before
the call that triggers them returns.

## Memory grant

```c
typedef struct tinyparse_memory {
    void   *base;
    size_t  size;
    void  *(*grow)(void *user, size_t need, size_t *got);
    void   *user;
    size_t  chunk_minimum;
} tinyparse_memory;

typedef struct tinyparse_configuration {
    tinyparse_memory memory;
    uint32_t         flags;
} tinyparse_configuration;
```

| Field | Meaning |
|-------|---------|
| `base` | The first block. Must not be `NULL`. Give it 16-byte alignment, as `malloc` does on 64-bit Linux and macOS; the library aligns every allocation to 16 bytes relative to the block start. |
| `size` | The size of `base` in bytes. Must hold the engine's bookkeeping plus one chunk; with the default `chunk_minimum`, the smallest grant `tinyparse_open` accepted was 4,224 bytes (x86-64 Linux). |
| `grow` | `NULL` for a fixed budget. Otherwise called when the current block cannot serve a chunk. |
| `user` | Passed to `grow` unchanged. |
| `chunk_minimum` | The smallest chunk the engine takes from a block, in bytes. 0 selects 4,096. |
| `flags` | Reserved. The library stores it and reads no bit of it; set it to 0. |

The `grow` contract:

- `need` is at least `chunk_minimum`, and larger when one allocation needs more.
- `grow` returns a new block of at least `need` bytes, or `NULL` to refuse.
  `*got` holds `need` on entry; a block larger than `need` stores its usable
  size there, and the engine spends all of it before calling `grow` again.
- The engine never resizes, moves or frees a block. When a new block arrives,
  the unused tail of the previous block is not used again.
- A refusal makes the call that needed the memory fail with
  `TINYPARSE_OUT_OF_MEMORY`, or with `TINYPARSE_INVALID` and an
  "out of memory" fault for a compile or load.
- Give each block the same 16-byte alignment as `base`.

### Sizing a grant

The table gives the smallest fixed grant (`grow` = `NULL`, `chunk_minimum` = 0)
with which each step succeeded, found by bisection to 1 KiB. Measured on
2026-10-04 with tinyparse 0.1.0 built by `mise run build`, on x86-64 Linux.
Grammars were compiled from their `grammar.scm` with the parameters their
`package.scm` sets, and loaded from a `.tpg` written by `tinyparse pack`.

| Grammar | Compile `.scm` | Load `.tpg` | Input | Compile + parse | Load + parse |
|---------|---------------:|------------:|-------|----------------:|-------------:|
| `examples/sexpr/sexpr.scm` | 17 KiB | 9 KiB | sexpr, 19,000 bytes | 1,729 KiB | 1,725 KiB |
| std.json | 57 KiB | 25 KiB | JSON, 38,632 bytes | 1,642 KiB | 1,610 KiB |
| std.jsonc | 65 KiB | 29 KiB | JSON, 38,632 bytes | 1,650 KiB | 1,614 KiB |
| std.json5 | 81 KiB | 33 KiB | JSON, 38,632 bytes | 1,667 KiB | 1,618 KiB |
| std.javascript | 12,811 KiB | 430 KiB | JavaScript, 71,800 bytes | 19,801 KiB | 7,419 KiB |
| std.typescript | 16,556 KiB | 658 KiB | TypeScript, 86,700 bytes | 26,426 KiB | 10,528 KiB |
| std.tsx | 16,576 KiB | 662 KiB | TypeScript, 86,700 bytes | 26,446 KiB | 10,532 KiB |
| std.shell | 6,500 KiB | 190 KiB | shell, 73,300 bytes | 10,367 KiB | 4,053 KiB |
| std.fence | 12,896 KiB | 462 KiB | fenced blocks, 9,500 bytes | 22,101 KiB | 9,686 KiB |

The inputs, each parsed with no diagnostic:

| Input | Made from |
|-------|-----------|
| sexpr | `examples/bufferfriendly/sample.sx`, 100 copies |
| JSON | the output of `tinyparse node-types std.javascript` |
| JavaScript | `test/suite/jsfamily/inputs/statements__4.js`, 100 copies |
| TypeScript | `test/suite/tsfamily/inputs/declarations__1.ts`, 100 copies |
| shell | `examples/ebnf/run.sh`, 50 copies |
| fenced blocks | `test/suite/inject/inputs/multi.fence`, 100 copies (200 injected regions) |

What the numbers show:

- Compiling std.shell, std.javascript, std.typescript, std.tsx and std.fence
  takes 6.3 to 16.2 MiB; loading the same grammars packed takes 190 to
  662 KiB.
- A parse needs memory in proportion to its input: 42 bytes per input byte for
  the JSON grammars, 54 for std.shell, 92 for sexpr, 100 for std.javascript and
  117 for std.typescript and std.tsx, on these inputs.
- An injected region costs about 45 KiB of its own, so the fenced-block input
  needed 992 bytes per input byte.
- With a 190-byte sexpr input and a 718-byte JavaScript input, a parse needed
  43 KiB and 305 KiB beyond the loaded grammar.

`tinyparse_parser_free` returns a parse's memory to the engine, and later
parses reuse it, but not all of it: each parser also keeps a small record in
the engine, and reused memory fragments. Parsing the same input over and over,
freeing each parser before creating the next, needed these fixed grants:

| Grammar and input | 1 parse | 10 parses | 1,000 parses |
|-------------------|--------:|----------:|-------------:|
| sexpr, 19,000 bytes | 1,725 KiB | 2,468 KiB | 3,210 KiB |
| std.javascript, 7,180 bytes (`test/suite/jsfamily/inputs/statements__4.js`, 10 copies) | 1,334 KiB | 1,615 KiB | 2,176 KiB |

## Engine

### tinyparse_open

```c
tinyparse_engine *tinyparse_open(tinyparse_configuration configuration);
```

**Parameters:** `configuration`, the [memory grant](#memory-grant) and a
reserved flag word.

**Returns:** a new engine, or `NULL` when `memory.base` is `NULL`,
`memory.size` is 0, or the block cannot hold the engine's bookkeeping plus one
chunk.

**Notes:** the engine's own state is the first allocation from `base`. A new
engine has no module resolver, no class bindings, no parameters, no plugs and
no registered languages.

### tinyparse_close

```c
void tinyparse_close(tinyparse_engine *engine);
```

**Parameters:** `engine`, or `NULL`.

**Notes:** releases nothing. Call it after the last call on the engine, then
free the grant's blocks.

### tinyparse_fault

```c
tinyparse_string tinyparse_fault(const tinyparse_engine *engine);
```

**Parameters:** `engine`, or `NULL`.

**Returns:** the engine's [fault](#faults), or an empty string when none was
set or `engine` is `NULL`.

**Notes:** every fault is constant text in the library; the bytes stay valid
after the next call.

### tinyparse_version_string

```c
const char *tinyparse_version_string(void);
```

**Returns:** the library version as a NUL-terminated string in
`MAJOR.MINOR.PATCH` form, for example `"0.1.0"`.

## Grammars

### tinyparse_compile_scheme

```c
tinyparse_result tinyparse_compile_scheme(
    tinyparse_engine   *engine,
    tinyparse_source    source,
    tinyparse_grammar **out_grammar);

typedef struct tinyparse_source {
    tinyparse_string name;
    tinyparse_string bytes;
} tinyparse_source;
```

**Parameters:**

- `engine`: the engine that owns the grammar.
- `source.bytes`: the grammar text ([Grammar language](grammar.md)).
- `source.name`: a name for the source. The library does not read it.
- `out_grammar`: receives the compiled grammar, or `NULL` on failure. May be
  `NULL`.

**Returns:**

| Code | When | Fault |
|------|------|-------|
| `TINYPARSE_OK` | The grammar compiled. | cleared |
| `TINYPARSE_INVALID` | `engine` is `NULL`. | unchanged |
| `TINYPARSE_INVALID` | The text does not compile, an `import`, `extends` or `inject` name cannot be resolved, or the grant is spent. | the reason, listed under [Compile faults](grammar.md#compile-faults) |

**Notes:**

- Install the module resolver, set the parameters and register the class
  bindings the grammar needs before this call. Parameters are read during the
  compile only; class bindings are looked up when a parse runs.
- `(import NAME)`, `(extends NAME)` and `(inject grammar: NAME ...)` call the
  [module resolver](#module-resolver) during this call. Without one, the
  faults are `import requires a module resolver`,
  `extends requires a module resolver` and
  `inject requires a module resolver to find the embedded grammar`.
- The grammar keeps pointers into `source.bytes`; see
  [Ownership and lifetime](#ownership-and-lifetime).
- The compile memory of std.javascript and the grammars built on it is listed
  under [Sizing a grant](#sizing-a-grant).

### tinyparse_load_binary_grammar

```c
tinyparse_result tinyparse_load_binary_grammar(
    tinyparse_engine   *engine,
    tinyparse_source    source,
    tinyparse_grammar **out_grammar);
```

**Parameters:**

- `engine`: the engine that owns the grammar.
- `source.bytes`: a packed grammar image, as written by
  `tinyparse_grammar_serialize` or `tinyparse pack`
  ([Release artifacts](artifacts.md) describes the format and its
  compatibility).
- `source.name`: not read.
- `out_grammar`: receives the grammar, or `NULL` on failure. May be `NULL`.

**Returns:**

| Code | When | Fault |
|------|------|-------|
| `TINYPARSE_OK` | The image loaded. | cleared |
| `TINYPARSE_INVALID` | `engine` is `NULL`. | unchanged |
| `TINYPARSE_INVALID` | The image is short, has a bad magic number, a different format version or ABI tag, is corrupt, or the grant is spent. | for example `binary grammar too short`, `not a packed grammar (bad magic)`, `packed grammar version mismatch`, `out of memory loading grammar` |

**Notes:**

- The loaded grammar parses as the grammar that was packed. Parameters,
  imported and base grammars, and the grammars named by `inject` forms are part
  of the image, so loading calls no module resolver and reads no parameter.
- `(class NAME)` forms stay names; bind the classes on this engine.
- The grammar holds no pointer into `source.bytes`.

### tinyparse_grammar_serialize

```c
tinyparse_result tinyparse_grammar_serialize(
    const tinyparse_grammar *grammar,
    tinyparse_string        *out_bytes);
```

**Parameters:** `grammar`; `out_bytes` receives the packed image.

**Returns:** `TINYPARSE_OK`; `TINYPARSE_INVALID` when an argument is `NULL`;
`TINYPARSE_OUT_OF_MEMORY` when the grant is spent. Sets no fault.

**Notes:** the image starts with the bytes `TPG1`, holds the bytes
`tinyparse pack` writes for the same grammar, and lives in the engine. The host
writes it wherever it likes; the library does no I/O.

### tinyparse_grammar_free

```c
void tinyparse_grammar_free(tinyparse_grammar *grammar);
```

**Notes:** releases nothing; the grammar's memory stays allocated until the
grant is freed. Compiling many grammars on one engine adds up.

## Emitters

### tinyparse_emit_c

```c
#define TINYPARSE_EMIT_BUNDLE     0x1u
#define TINYPARSE_EMIT_PARSER     0x2u
#define TINYPARSE_EMIT_PARSER_LIB 0x4u

tinyparse_result tinyparse_emit_c(
    tinyparse_engine        *engine,
    const tinyparse_grammar *grammar,
    uint32_t                 flags,
    tinyparse_string        *out_source);
```

**Parameters:**

- `engine`, `grammar`: the grammar to emit.
- `flags`: selects the output.
- `out_source`: receives the C source text.

| `flags` | Output | `tinyparse c` option |
|---------|--------|----------------------|
| 0 | Generated module: the packed grammar and `tinyparse_string tinyparse_embedded_grammar(void)`, which returns it for `tinyparse_load_binary_grammar`. | none |
| `TINYPARSE_EMIT_BUNDLE` | A program whose `main` loads the embedded grammar, parses standard input with the VM and prints the tree. | `--bundle` |
| `TINYPARSE_EMIT_PARSER` | Generated parser as a program: one C function per rule and token, and a `main` that parses standard input and prints the tree. | `--parser` |
| `TINYPARSE_EMIT_PARSER` \| `TINYPARSE_EMIT_PARSER_LIB` | Generated parser as a library: the [generated parser interface](#generated-parser-interface), no `main`. | `--parser-lib` |

**Returns:** `TINYPARSE_OK`; `TINYPARSE_INVALID` when an argument is `NULL`;
`TINYPARSE_OUT_OF_MEMORY` when the grant is spent. Sets no fault.

**Notes:**

- `TINYPARSE_EMIT_PARSER_LIB` without `TINYPARSE_EMIT_PARSER` is ignored.
  `TINYPARSE_EMIT_PARSER` takes precedence over `TINYPARSE_EMIT_BUNDLE`.
- The text lives in the engine; the library does no I/O.
- A generated module defines the external function
  `tinyparse_embedded_grammar`, so a program links one generated module at
  most.
- The `main` that `TINYPARSE_EMIT_BUNDLE` and `TINYPARSE_EMIT_PARSER` write
  binds no class, so a grammar that uses `(class NAME)` fails on input that
  needs the class. The bundle parses within a fixed 8 MiB grant with no
  `grow`; the `--parser` program uses a 1 GiB static array. The output format
  of both programs is described under [`c` in
  tinyparse(1)](../../tinyparse.1.md#c).
- The symbol prefix of a generated parser is the grammar's name;
  `tinyparse_emit_c` cannot set another. `tinyparse c --name` can.

### tinyparse_emit_node_types

```c
tinyparse_result tinyparse_emit_node_types(
    tinyparse_engine        *engine,
    const tinyparse_grammar *grammar,
    tinyparse_string        *out_json);
```

**Parameters:** `engine`, `grammar`; `out_json` receives the document.

**Returns:** `TINYPARSE_OK`; `TINYPARSE_INVALID` when an argument is `NULL`;
`TINYPARSE_OUT_OF_MEMORY` when the grant is spent. Sets no fault.

**Notes:** the document is the JSON that `tinyparse node-types` prints: an array
with one object per named rule and token, in definition order, each with
`"type"`, `"named"`, `"fields"` and, for a rule with unnamed children,
`"children"`. The bytes live in the engine.

## Parsers

### tinyparse_parser_new

```c
tinyparse_result tinyparse_parser_new(
    tinyparse_engine   *engine,
    tinyparse_grammar  *grammar,
    tinyparse_string    start_rule,
    tinyparse_parser  **out_parser);
```

**Parameters:**

- `engine`: the engine that owns `grammar`.
- `grammar`: a compiled or loaded grammar.
- `start_rule`: the name of the rule to parse from. An empty string selects
  the grammar's `start:` rule.
- `out_parser`: receives the parser. Must not be `NULL`.

**Returns:**

| Code | When | Fault |
|------|------|-------|
| `TINYPARSE_OK` | The parser is ready for input. | unchanged |
| `TINYPARSE_INVALID` | `engine` or `grammar` is `NULL`. | unchanged |
| `TINYPARSE_INVALID` | `start_rule` is no name of the grammar. | `start rule not found in grammar` |
| `TINYPARSE_OUT_OF_MEMORY` | The grant is spent. | unchanged |

**Notes:**

- `*out_parser` is `NULL` after a failure.
- A `start_rule` that is a name of the grammar but not a rule, such as a token
  or field name, is accepted here; `tinyparse_finish` then returns
  `TINYPARSE_SYNTAX_ERROR` with the diagnostic `parse.no-start` and no tree.
- A parser parses one input; create a new one for the next.

### tinyparse_push

```c
tinyparse_result tinyparse_push(tinyparse_parser *parser, tinyparse_string bytes);
```

**Parameters:** `parser`; `bytes`, the next part of the input, of any length.

**Returns:**

| Code | When |
|------|------|
| `TINYPARSE_OK` | The bytes were buffered, or `bytes` is empty. |
| `TINYPARSE_INVALID` | `parser` is `NULL`, or the parser has finished. |
| `TINYPARSE_OUT_OF_MEMORY` | The buffer could not grow. The parser drops the input, and `tinyparse_finish` then returns `TINYPARSE_OUT_OF_MEMORY` with the fault `input chunk was dropped before finish`. |

**Notes:** the parser copies the bytes and parses nothing until
`tinyparse_finish`. Splitting the input differently gives the same tree.

### tinyparse_finish

```c
tinyparse_result tinyparse_finish(tinyparse_parser *parser);
```

**Parameters:** `parser`.

**Returns:**

| Code | When | Tree |
|------|------|------|
| `TINYPARSE_OK` | The input matched, possibly after grammar-defined recovery, which adds a diagnostic. | the whole tree |
| `TINYPARSE_SYNTAX_ERROR` | The input did not match the start rule, or input remained after it. | usually a partial tree; see `tinyparse_parser_tree` |
| `TINYPARSE_OUT_OF_MEMORY` | The grant is spent, or a push dropped input. | none |
| `TINYPARSE_INVALID` | `parser` is `NULL`. | none |

**Notes:**

- The parse runs in this call, with the bytecode VM.
- A second call on the same parser does not parse again: it returns
  `TINYPARSE_OK` when the parser has a tree and `TINYPARSE_SYNTAX_ERROR` when it
  has none. A failed parse usually has a partial tree, so a second call can
  return `TINYPARSE_OK` after the first returned `TINYPARSE_SYNTAX_ERROR`; keep
  the result of the first call.
- The VM keeps a rule memo, a cache of rule results that bounds backtracking.
  It switches the memo on by itself once a parse has made more than
  32 × (input length in bytes) + 65,536 rule calls. No API controls it. The
  `TINYPARSE_RULEMEMO` variable described in [tinyparse(1)](../../tinyparse.1.md#environment)
  is read only by the `tinyparse` command.
- With a plug named `trace` installed, the parse sends [trace
  messages](#trace-messages).

### tinyparse_parser_tree

```c
const tinyparse_tree *tinyparse_parser_tree(const tinyparse_parser *parser);
```

**Returns:** the parser's tree, or `NULL` before `tinyparse_finish`, when
`parser` is `NULL`, or when the parse built none.

**Notes:** after `TINYPARSE_SYNTAX_ERROR` the tree is usually not `NULL`: it
holds the root and the children the start rule completed before the failure. Check the result of
`tinyparse_finish` before treating the tree as complete. The tree is valid
until `tinyparse_parser_free`.

### tinyparse_parser_free

```c
void tinyparse_parser_free(tinyparse_parser *parser);
```

**Parameters:** `parser`, or `NULL`.

**Notes:** returns the memory of the parse (input buffer, tree, diagnostics,
cursors) to the engine for later parses. The parser, its tree, its diagnostics
and every cursor over the tree are invalid afterward. Copy out what you need
first.

## Diagnostics

```c
typedef enum tinyparse_severity {
    TINYPARSE_SEVERITY_NOTE = 0,
    TINYPARSE_SEVERITY_WARNING,
    TINYPARSE_SEVERITY_ERROR
} tinyparse_severity;

typedef struct tinyparse_label {
    uint32_t         start_byte;
    uint32_t         end_byte;
    tinyparse_string message;
    uint32_t         flags;
} tinyparse_label;

typedef struct tinyparse_diagnostic {
    tinyparse_severity     severity;
    tinyparse_string       code;
    tinyparse_string       message;
    const tinyparse_label *labels;
    uint32_t               label_count;
    tinyparse_string       help;
    tinyparse_string       note;
} tinyparse_diagnostic;
```

| Field | Meaning |
|-------|---------|
| `severity` | Every diagnostic the library produces has `TINYPARSE_SEVERITY_ERROR`. |
| `code` | The diagnostic code, such as `parse.syntax`. [Diagnostics](diagnostics.md) lists every code. |
| `message` | One line of text. The library's own messages start with `error: `; a `recover` form supplies its own. |
| `labels`, `label_count` | Byte ranges `[start_byte-end_byte]` in the input, end-exclusive, each with a message. Every diagnostic the library produces has one label. A label's `flags` is 0. |
| `help` | Text from a grammar's `recover` form, or empty. |
| `note` | Always empty. |

The library never prints a diagnostic; rendering is the host's.

### tinyparse_parser_diagnostic_count

```c
uint32_t tinyparse_parser_diagnostic_count(const tinyparse_parser *parser);
```

**Returns:** the number of diagnostics the parse produced, 0 for a `NULL`
parser. A parse that returned `TINYPARSE_OK` can have diagnostics from
recovery.

### tinyparse_parser_diagnostic

```c
const tinyparse_diagnostic *tinyparse_parser_diagnostic(
    const tinyparse_parser *parser,
    uint32_t                index);
```

**Returns:** diagnostic `index`, counting from 0, or `NULL` when `index` is out
of range or `parser` is `NULL`. Valid until `tinyparse_parser_free`.

## Tree cursor

A tree is read through a cursor that stands on one node and moves between
nodes. Node kinds and field names are symbols; `tinyparse_tree_symbol_name`
turns one into text. A node's text is the input slice its byte range covers;
the tree does not store it. How the grammar decides the tree's shape is
explained in [How tinyparse shapes the tree](../explanation/tree-shape.md).

```c
typedef struct tinyparse_symbol {
    uint32_t id;
} tinyparse_symbol;

#define TINYPARSE_NO_FIELD     0xFFFFFFFFu
#define TINYPARSE_NODE_MISSING 0x1u
```

- `TINYPARSE_NO_FIELD` is the `id` of [`tinyparse_tree_cursor_field`](#tinyparse_tree_cursor_field)
  for a node that has no field.
- `TINYPARSE_NODE_MISSING` is the bit of
  [`tinyparse_tree_cursor_flags`](#tinyparse_tree_cursor_flags) that marks a
  MISSING node: a zero-width node that a grammar's `recover` form inserted for
  input that was not there.

The functions that read or move a cursor accept a `NULL` cursor: a move returns
0, `tinyparse_tree_cursor_field` returns `TINYPARSE_NO_FIELD`, and every other
read returns 0.

### tinyparse_tree_cursor_new

```c
tinyparse_result tinyparse_tree_cursor_new(
    const tinyparse_tree   *tree,
    tinyparse_tree_cursor **out_cursor);
```

**Returns:** `TINYPARSE_OK` with a cursor on the root; `TINYPARSE_INVALID`
when an argument is `NULL`; `TINYPARSE_OUT_OF_MEMORY`.

**Notes:** the cursor lives in the parser that built the tree and is invalid
after `tinyparse_parser_free`.

### tinyparse_tree_cursor_free

```c
void tinyparse_tree_cursor_free(tinyparse_tree_cursor *cursor);
```

**Notes:** releases nothing; `tinyparse_parser_free` releases the cursor.

### tinyparse_tree_cursor_init

```c
typedef struct tinyparse_tree_cursor_storage {
    const tinyparse_tree *tree;
    uint32_t              current_node_index;
    uint32_t              parent_node_index;
    uint32_t              child_index;
    uint32_t              depth;
    uint32_t              first_child;
    uint32_t              child_count;
    uint32_t              flags;
    uint32_t              reserved[8];
} tinyparse_tree_cursor_storage;

void tinyparse_tree_cursor_init(
    tinyparse_tree_cursor_storage *storage,
    const tinyparse_tree          *tree);
```

**Parameters:** `storage`, memory the host owns, for example a local
variable; `tree`.

**Notes:** puts a cursor on the root of `tree` with no allocation. Pass the
storage to the cursor functions as `(tinyparse_tree_cursor *)&storage`. Do not
read or write the fields. The storage is valid until `tinyparse_parser_free`
on the parser that built `tree`.

### tinyparse_tree_cursor_reset

```c
void tinyparse_tree_cursor_reset(
    tinyparse_tree_cursor *cursor,
    const tinyparse_tree  *tree);
```

**Notes:** moves the cursor to the root of `tree`, which may be another tree.
Depth becomes 0.

### tinyparse_tree_cursor_reset_to_root

```c
void tinyparse_tree_cursor_reset_to_root(tinyparse_tree_cursor *cursor);
```

**Notes:** moves the cursor to the root of its tree. Depth becomes 0.

### tinyparse_tree_cursor_clone

```c
tinyparse_result tinyparse_tree_cursor_clone(
    const tinyparse_tree_cursor *source,
    tinyparse_tree_cursor      **out_cursor);
```

**Returns:** `TINYPARSE_OK` with a new cursor on the same node at the same
depth; `TINYPARSE_INVALID` when an argument is `NULL` or `source` has no tree;
`TINYPARSE_OUT_OF_MEMORY`.

**Notes:** the copy lives in the parser that built the tree.

### tinyparse_tree_cursor_assign

```c
void tinyparse_tree_cursor_assign(
    tinyparse_tree_cursor       *target,
    const tinyparse_tree_cursor *source);
```

**Notes:** puts `target` on the node and depth of `source`.

### tinyparse_tree_cursor_kind

```c
tinyparse_symbol tinyparse_tree_cursor_kind(const tinyparse_tree_cursor *cursor);
```

**Returns:** the node kind of the node under the cursor.

### tinyparse_tree_cursor_field

```c
tinyparse_symbol tinyparse_tree_cursor_field(const tinyparse_tree_cursor *cursor);
```

**Returns:** the field the node holds in its parent, or a symbol whose `id` is
`TINYPARSE_NO_FIELD`.

### tinyparse_tree_symbol_name

```c
tinyparse_string tinyparse_tree_symbol_name(
    const tinyparse_tree *tree,
    tinyparse_symbol      symbol);
```

**Returns:** the name of a node kind or field symbol of `tree`, or an empty
string for an unknown symbol, `TINYPARSE_NO_FIELD`, or a `NULL` tree.

**Notes:** the name lives in the compiled grammar.

### tinyparse_tree_cursor_start_byte

```c
uint32_t tinyparse_tree_cursor_start_byte(const tinyparse_tree_cursor *cursor);
```

**Returns:** the 0-based input offset where the node starts.

### tinyparse_tree_cursor_end_byte

```c
uint32_t tinyparse_tree_cursor_end_byte(const tinyparse_tree_cursor *cursor);
```

**Returns:** the input offset one past the node's last byte. A MISSING node
has the same start and end.

### tinyparse_tree_cursor_child_count

```c
uint32_t tinyparse_tree_cursor_child_count(const tinyparse_tree_cursor *cursor);
```

**Returns:** the number of children of the node under the cursor.

### tinyparse_tree_cursor_depth

```c
uint32_t tinyparse_tree_cursor_depth(const tinyparse_tree_cursor *cursor);
```

**Returns:** the number of levels below the root; the root is at depth 0.

### tinyparse_tree_cursor_flags

```c
uint32_t tinyparse_tree_cursor_flags(const tinyparse_tree_cursor *cursor);
```

**Returns:** the node's flag bits: `TINYPARSE_NODE_MISSING` or 0.

### tinyparse_tree_cursor_index_in_parent

```c
uint32_t tinyparse_tree_cursor_index_in_parent(const tinyparse_tree_cursor *cursor);
```

**Returns:** the node's 0-based position among its parent's children; 0 for
the root.

**Notes:** walks the parent's children from the first, so it costs one step per
earlier sibling.

### Moving the cursor

```c
int tinyparse_tree_cursor_goto_parent(tinyparse_tree_cursor *cursor);
int tinyparse_tree_cursor_goto_first_child(tinyparse_tree_cursor *cursor);
int tinyparse_tree_cursor_goto_last_child(tinyparse_tree_cursor *cursor);
int tinyparse_tree_cursor_goto_next_sibling(tinyparse_tree_cursor *cursor);
int tinyparse_tree_cursor_goto_previous_sibling(tinyparse_tree_cursor *cursor);
int tinyparse_tree_cursor_goto_child(tinyparse_tree_cursor *cursor,
                                     uint32_t child_index);
int tinyparse_tree_cursor_goto_first_child_by_field(tinyparse_tree_cursor *cursor,
                                                    tinyparse_string field);
int tinyparse_tree_cursor_goto_first_child_by_field_id(tinyparse_tree_cursor *cursor,
                                                       tinyparse_symbol field);
```

Each function returns 1 and moves the cursor, or returns 0 and leaves it where
it was.

| Function | Moves to | Cost |
|----------|----------|------|
| `goto_parent` | the parent; 0 at the root | one step |
| `goto_first_child` | the first child | one step |
| `goto_last_child` | the last child | one step per child |
| `goto_next_sibling` | the next sibling | one step |
| `goto_previous_sibling` | the previous sibling; 0 at the root | one step per earlier sibling |
| `goto_child` | child `child_index`, counting from 0 | one step per earlier child |
| `goto_first_child_by_field` | the first child whose field is named `field`; 0 when the grammar has no such name | one step per child |
| `goto_first_child_by_field_id` | the first child whose field symbol is `field` | one step per child |

A move to a child adds 1 to the depth and a move to the parent subtracts 1.

## Bindings and classes

```c
typedef enum tinyparse_binding_kind {
    TINYPARSE_BIND_CLASS,
    TINYPARSE_BIND_PREDICATE,
    TINYPARSE_BIND_ACTION
} tinyparse_binding_kind;

typedef struct tinyparse_binding {
    tinyparse_string       name;
    tinyparse_binding_kind kind;
    void                  *function;
    void                  *user;
} tinyparse_binding;

typedef int (*tinyparse_class_fn)(void *user, uint32_t byte);
```

A class binding gives the grammar form `(class NAME)` its bytes
([Classes](grammar.md#classes)). `TINYPARSE_BIND_PREDICATE` and
`TINYPARSE_BIND_ACTION` are reserved: `tinyparse_bind` stores such a binding,
and nothing uses it.

### tinyparse_bind

```c
tinyparse_result tinyparse_bind(
    tinyparse_engine        *engine,
    const tinyparse_binding *binding);
```

**Parameters:**

- `engine`.
- `binding->name`: the class name, as written in `(class NAME)`. Must not be
  empty.
- `binding->kind`: `TINYPARSE_BIND_CLASS`.
- `binding->function`: a `tinyparse_class_fn`, cast to `void *`.
- `binding->user`: passed to the function unchanged.

**Returns:**

| Code | When | Fault |
|------|------|-------|
| `TINYPARSE_OK` | The binding is stored. | unchanged |
| `TINYPARSE_INVALID` | `engine` is `NULL`. | unchanged |
| `TINYPARSE_INVALID` | `binding` is `NULL` or its name is empty. | `bind needs a named binding` |
| `TINYPARSE_OUT_OF_MEMORY` | The grant is spent. | `out of memory binding` |

**Notes:**

- The engine copies the binding and its name; the host's struct may go away.
- Binding a name again with the same kind replaces the function and `user`.
- The parse looks the class up by name each time it matches `(class NAME)`, so a
  binding made before or after `tinyparse_compile_scheme` applies to every
  later parse. A name with no binding, or a binding whose `function` is `NULL`,
  matches no byte and raises no fault: the compile succeeds, and an input that
  needs the class fails to parse like any input outside the class.
- The class function receives one input byte, 0 to 255, and returns nonzero when
  the byte is in the class. It must return the same answer for the same byte
  every time and be safe to call again before an earlier call has returned:
  the parse calls it again for the same byte when it backtracks.
- ISO C defines no conversion from a function pointer to `void *`; POSIX
  requires it to work. GCC with `-pedantic` warns about the cast.

## Module resolver

```c
typedef tinyparse_result (*tinyparse_module_resolver)(
    void             *user,
    tinyparse_string  name,
    tinyparse_source *out_source);
```

### tinyparse_set_module_resolver

```c
tinyparse_result tinyparse_set_module_resolver(
    tinyparse_engine          *engine,
    tinyparse_module_resolver  resolve,
    void                      *user);
```

**Parameters:** `engine`; `resolve`, or `NULL` to remove the resolver; `user`,
passed to `resolve` unchanged.

**Returns:** `TINYPARSE_OK`, or `TINYPARSE_INVALID` when `engine` is `NULL`.

**Notes on the resolver:**

- `tinyparse_compile_scheme` calls it once for each `(import NAME)`,
  `(extends NAME)` and `(inject grammar: NAME ...)` it compiles, including
  those inside resolved grammars, before the compile returns.
- `name` is the name as written in the grammar, dots included, for example
  `words` or `std.json`. The library gives it no meaning; mapping it to bytes is
  the host's choice.
- On entry `out_source->name` is `name` and `out_source->bytes` is empty. Set
  `out_source->bytes` to the grammar text and return `TINYPARSE_OK`.
- Any other result fails the compile with the fault
  `module resolver could not load an imported module`,
  `module resolver could not load the base grammar` or
  `inject could not resolve the embedded grammar`.
- The returned bytes must stay valid while the compiled grammar is in use; see
  [Ownership and lifetime](#ownership-and-lifetime).
- The library detects cycles: a name that is already being compiled fails with
  `import cycle detected` or `extends cycle detected`.
- The `tinyparse` command's resolver and how to write one are in [Supply
  imports, classes and parameters from a C host](../how-to/host-services.md).

## Parameters

### tinyparse_set_param

```c
tinyparse_result tinyparse_set_param(
    tinyparse_engine *engine,
    tinyparse_string  name,
    int               value);
```

**Parameters:** `engine`; `name`, the parameter a `(when NAME: ...)` form tests
([when](grammar.md#when)); `value`, 0 to clear the parameter and any other
value to set it.

**Returns:**

| Code | When | Fault |
|------|------|-------|
| `TINYPARSE_OK` | The value is stored. | unchanged |
| `TINYPARSE_INVALID` | `engine` is `NULL`. | unchanged |
| `TINYPARSE_INVALID` | `name` is empty. | `set_param needs a parameter name` |
| `TINYPARSE_OUT_OF_MEMORY` | The grant is spent. | `out of memory setting parameter` |

**Notes:**

- Parameters are read while `tinyparse_compile_scheme` runs and apply to the
  whole compile, imported and base grammars included. Set them before the
  compile; a change afterward affects only grammars compiled later.
- Setting a name again replaces its value. The engine copies the name.
- The library does not read `package.scm`. A host that compiles a grammar
  package sets the parameters its manifest lists
  ([package.scm manifest](grammar.md#packagescm-manifest)).

## Registered languages

```c
typedef struct tinyparse_language_entry {
    tinyparse_grammar *grammar;
    tinyparse_result (*parse)(tinyparse_parser *parser);
} tinyparse_language_entry;
```

### tinyparse_register_language

```c
tinyparse_result tinyparse_register_language(
    tinyparse_engine               *engine,
    tinyparse_string                name,
    const tinyparse_language_entry *entry);
```

**Parameters:**

- `engine`.
- `name`: the name an `(inject grammar: NAME ...)` form carries, for example
  `std.json`. Must not be empty.
- `entry->grammar`: the grammar that parses the region, or `NULL` to keep the
  grammar the inject form resolved at compile time.
- `entry->parse`: `NULL` to parse the region with the VM, or the `P_run` entry
  of a [generated parser](#generated-parser-interface) built from
  `entry->grammar`.

**Returns:**

| Code | When | Fault |
|------|------|-------|
| `TINYPARSE_OK` | The language is registered. | unchanged |
| `TINYPARSE_INVALID` | `engine` or `entry` is `NULL`. | unchanged |
| `TINYPARSE_INVALID` | `name` is empty. | `register_language needs a named language` |
| `TINYPARSE_OUT_OF_MEMORY` | The grant is spent. | `out of memory registering language` |

**Notes:**

- When a parse reaches an inject form, a language registered under its name
  replaces the grammar the form resolved at compile time. Registering takes
  effect for parses started afterward and does not need a recompile.
- The inject form's grammar is still resolved when the host grammar compiles
  from `.scm`, so that compile needs the module resolver even when every
  language is registered. A loaded `.tpg` or a generated parser carries the
  injected grammars and needs no resolver.
- When `entry->parse` is set, `entry->grammar` must be the grammar that the same
  generated parser's `P_load` returned: the generated code reads that grammar's
  tables by position.
- The engine copies the name and the entry; the grammar and the function must
  stay valid while parses use them. Registering a name again replaces the entry.
- The region's subtree is placed under a node of kind `injection`. A region
  that fails to parse leaves what its parse built there, and its diagnostics
  are discarded: the host parse reports no diagnostic for it and its result is
  unaffected.

## Plugs

```c
typedef struct tinyparse_message {
    tinyparse_string name;
    tinyparse_string target;
    tinyparse_string data;
    uint32_t         flags;
} tinyparse_message;

typedef struct tinyparse_plug {
    tinyparse_string name;
    tinyparse_result (*emit)(void *user, const tinyparse_message *message);
    void *user;
} tinyparse_plug;
```

### tinyparse_plug_install

```c
tinyparse_result tinyparse_plug_install(
    tinyparse_engine     *engine,
    const tinyparse_plug *plug);
```

**Parameters:** `engine`; `plug->name`, which must not be empty;
`plug->emit`, which must not be `NULL`; `plug->user`, passed to `emit`
unchanged.

**Returns:**

| Code | When | Fault |
|------|------|-------|
| `TINYPARSE_OK` | The plug is installed. | unchanged |
| `TINYPARSE_INVALID` | `engine` is `NULL`. | unchanged |
| `TINYPARSE_INVALID` | `plug` is `NULL`, its name is empty or `emit` is `NULL`. | `plug_install needs a named plug with an emit` |
| `TINYPARSE_OUT_OF_MEMORY` | The grant is spent. | `out of memory plug` |

**Notes:** the engine copies the plug and its name. Installing a name again
replaces `emit` and `user`. The engine sends messages to one plug name,
`trace`; a plug under any other name receives nothing.

### Trace messages

While a plug named `trace` is installed, the VM and generated parsers call its
`emit` during a parse:

| `name` | Sent when | `data` |
|--------|-----------|--------|
| `rule.enter` | a rule call starts | the rule name |
| `rule.exit` | a rule call matches; a call that fails sends no `rule.exit` | the rule name |
| `token` | a token matches | the token name |
| `rule.memo-on` | the VM switches on its rule memo (VM only) | the rule being called |
| `rule.replay` | the VM reuses a remembered rule match (VM only) | the rule name |

`target` is empty and `flags` is 0. The result `emit` returns is ignored. The
start rule sends no `rule.enter` or `rule.exit`. The strings are valid during
the call. `tinyparse run --trace` prints these messages.

## Linearize and query

```c
typedef enum tinyparse_lin_kind {
    TINYPARSE_LIN_ENTER = 0,
    TINYPARSE_LIN_LEAF,
    TINYPARSE_LIN_EXIT
} tinyparse_lin_kind;

typedef struct tinyparse_lin_token {
    tinyparse_lin_kind kind;
    tinyparse_string   node_kind;
    tinyparse_string   field;
    tinyparse_string   text;
    uint32_t           start_byte;
    uint32_t           end_byte;
} tinyparse_lin_token;

typedef struct tinyparse_query_capture {
    tinyparse_string name;
    uint32_t         start_byte;
    uint32_t         end_byte;
    tinyparse_string text;
} tinyparse_query_capture;
```

### tinyparse_linearize

```c
tinyparse_result tinyparse_linearize(
    tinyparse_engine     *engine,
    const tinyparse_tree *tree,
    tinyparse_string      input,
    void (*emit)(void *user, const tinyparse_lin_token *token),
    void                 *user);
```

**Parameters:** `engine`; `tree`; `input`, the bytes that were parsed, or an
empty string to leave `text` empty; `emit`; `user`, passed to `emit`.

**Returns:** `TINYPARSE_OK`; `TINYPARSE_INVALID` when `engine`, `tree` or
`emit` is `NULL`; `TINYPARSE_OUT_OF_MEMORY`. Sets no fault.

**Notes:** calls `emit` once per token in pre-order: `TINYPARSE_LIN_ENTER` and
later `TINYPARSE_LIN_EXIT` for a node with children, `TINYPARSE_LIN_LEAF` for a
node without. `node_kind` is the kind name; `field` is the field name or empty;
`text` is the leaf's slice of `input` and empty for `ENTER` and `EXIT`.
`tinyparse linearize` prints this stream. Each call allocates in the engine
until the grant is freed.

### tinyparse_query

```c
tinyparse_result tinyparse_query(
    tinyparse_engine     *engine,
    const tinyparse_tree *tree,
    tinyparse_string      query_source,
    tinyparse_string      input,
    void (*emit)(void *user, const tinyparse_query_capture *capture),
    void                 *user);
```

**Parameters:** `engine`; `tree`; `query_source`, one or more patterns in the
[query language](query-language.md); `input`, the bytes that were parsed;
`emit`; `user`, passed to `emit`.

**Returns:**

| Code | When |
|------|------|
| `TINYPARSE_OK` | The query ran, with zero or more matches. |
| `TINYPARSE_INVALID` | `engine`, `tree` or `emit` is `NULL`; the query does not read or compile, or names a node kind or field the tree's grammar does not have. |
| `TINYPARSE_OUT_OF_MEMORY` | The grant is spent. |

**Notes:** calls `emit` once per capture of every match. `name` is the capture
name without `@`; `text` is the captured node's slice of `input`. A failed query
sets no fault, and the fault can still hold the message of an earlier call.
The reason is not available; `tinyparse query` reports only `invalid query`. Each call allocates in the engine until the grant
is freed.

## Declared types with no function

The header declares these types; no function takes or returns them.

```c
typedef struct tinyparse_point {
    uint32_t row;
    uint32_t column;
} tinyparse_point;

typedef struct tinyparse_range {
    uint32_t        start_byte;
    uint32_t        end_byte;
    tinyparse_point start_point;
    tinyparse_point end_point;
} tinyparse_range;

typedef struct tinyparse_token {
    uint32_t kind;
    uint32_t start_byte;
    uint32_t end_byte;
    uint32_t flags;
} tinyparse_token;
```

A host that prints `[row,column]` points, as `tinyparse run --dump-tree` does,
computes them from byte offsets.

## Generated parser interface

`tinyparse c GRAMMAR --parser-lib --out FILE.c`, or `tinyparse_emit_c` with
`TINYPARSE_EMIT_PARSER | TINYPARSE_EMIT_PARSER_LIB`, writes a C file that
defines these functions, where `P` is the symbol prefix:

```c
tinyparse_result P_load(tinyparse_engine *engine, tinyparse_grammar **out_grammar);
tinyparse_result P_run(tinyparse_parser *parser);
tinyparse_result P_run_chunk(tinyparse_parser *parser);
```

The file declares nothing in a header; the host declares the prototypes it
calls. How to build and ship one is in [Ship a generated C
parser](../how-to/generated-parser.md).

### Symbol prefix

`P` is the `--name` argument of `tinyparse c`. Without `--name`, and always
through `tinyparse_emit_c`, it is the name in the grammar's
`(grammar NAME ...)` form, with every byte other than a letter, digit or `_`
replaced by `_`, and an `s` added before a leading digit. Every file-scope name
in the file starts with `P_`, so two generated parsers with different prefixes
link into one program.

### Building

The file includes `tinyparse.h` and the library's internal header
`src/internal.h`, so it compiles with `-I include -I src` from a source tree of
the same version. It calls internal library functions, so it links against
`libtinyparse.a` or the amalgamation's `tinyparse.c`; `libtinyparse.so` exports
the public functions only. Compiled with `-Wall -Wextra`, the file can produce
unused-variable, unused-label and unused-function warnings.

### P_load

**Returns:** what `tinyparse_load_binary_grammar` returns for the packed
grammar the file embeds; `TINYPARSE_INVALID` when an argument is `NULL`.

**Notes:** create parsers for `P_run` and `P_run_chunk` from this grammar
only.

### P_run

**Parameters:** a parser from `tinyparse_parser_new` on the grammar `P_load`
returned, with its input pushed.

**Returns:** as `tinyparse_finish`. A push that dropped input gives
`TINYPARSE_OUT_OF_MEMORY`.

**Notes:**

- Use it in place of `tinyparse_finish`; it builds the same tree and the same
  diagnostics as the VM.
- Call it once per parser. A second call parses again.
- Each rule call is a C function call. After 1,000 nested rule calls the parse
  fails with `TINYPARSE_SYNTAX_ERROR` and a `parse.syntax` diagnostic instead
  of exhausting the C stack. sexpr nests two rules per list level, so its
  generated parser accepts 500 nested lists and rejects 501.
- A generated parser has no rule memo; it sends the `rule.enter`, `rule.exit`
  and `token` [trace messages](#trace-messages).

### P_run_chunk

Emitted only when the start rule's body is `(repeat ITEM)`, `(seq (repeat ITEM))`
or `(seq (repeat ITEM) EOF)` with no upper bound on the repeat.

**Parameters:** a parser as for `P_run`. Its `input_complete` field, declared in
`src/internal.h`, is 1 after `tinyparse_parser_new`.

**Returns:** `TINYPARSE_MORE` while `input_complete` is 0, or
`TINYPARSE_OUT_OF_MEMORY`; on the call with `input_complete` set to 1, what
`P_run` returns for the whole input.

**Notes:** push the bytes that have arrived, set `parser->input_complete` to 0,
and call `P_run_chunk`; repeat, and set `input_complete` to 1 before the call
after the last push. Each call parses the top-level items that end before the
buffered input does and resumes at the next item. The finished tree equals the
tree `P_run` builds for the whole input. `examples/bufferfriendly` drives one.
