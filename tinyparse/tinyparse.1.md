---
date: 2026-10-04
os: tinyparse
---

# tinyparse(1)

## NAME

tinyparse - compile grammars and parse input from the command line

## SYNOPSIS

- tinyparse check GRAMMAR [--param NAME[=VALUE]]...
- tinyparse run GRAMMAR --input FILE [--dump-tree] [--chunk N] [--trace] [--param NAME[=VALUE]]...
- tinyparse pack GRAMMAR --out FILE [--param NAME[=VALUE]]...
- tinyparse c GRAMMAR --out FILE [--bundle | --parser | --parser-lib] [--name PREFIX] [--param NAME[=VALUE]]...
- tinyparse linearize GRAMMAR --input FILE [--param NAME[=VALUE]]...
- tinyparse query GRAMMAR QUERY --input FILE [--param NAME[=VALUE]]...
- tinyparse node-types GRAMMAR [--param NAME[=VALUE]]...
- tinyparse init DIR [--amalgamation | --lib] [--grammar NAME]
- tinyparse vendor DIR [--amalgamation | --lib]
- tinyparse grammars
- tinyparse man
- tinyparse version | --version
- tinyparse [help | --help | -h]

## DESCRIPTION

The tinyparse library compiles a grammar, written as S-expression data in a
`.scm` file, into a parser at run time. The parser turns input bytes into a
concrete syntax tree (CST) and a list of diagnostics. The library performs no
I/O and allocates memory only from the memory grant its host program gives
it: a block of memory and, optionally, a callback that supplies more.

The `tinyparse` command is a host program for that library. It reads grammar
and input files, parses, and prints the tree, the diagnostics, query captures
or the node types of a grammar. It also writes packed grammars, C source
generated from a grammar, and starter projects. The first argument names the
command and the positional arguments follow it; options come after the
positional arguments, in any order.

## GRAMMAR ARGUMENTS

Every command that takes GRAMMAR resolves it in this order:

1. A path to a file named `package.scm`, or to a directory that contains
   `package.scm`, names a grammar package. The command reads the manifest,
   sets each entry of its `params:` as a parameter, and compiles `grammar.scm`
   from the same directory.
2. Otherwise, a path to a readable file names a grammar file. Relative paths
   are relative to the working directory. A directory without `package.scm`
   is read as an empty file and fails with `error: grammar form not found`.
3. Otherwise, GRAMMAR is looked up by name among the standard grammars built
   into the program, such as `std.json`; `tinyparse grammars` lists them. A
   built-in grammar is used with the parameters of its own manifest.
4. Otherwise, the command prints `tinyparse: error: cannot read grammar
   'GRAMMAR'` and exits with status 1.

A file in the working directory therefore takes the place of a built-in
grammar of the same name.

`run`, `linearize`, `query` and `pack` also accept a packed grammar written by
`pack`: a file whose name ends in `.tpg`, or whose first four bytes are `TPG1`,
is loaded instead of compiled. `check`, `node-types` and `c` read every file as
`.scm` source; see BUGS.

A grammar can name other grammars with `(import NAME)`, `(extends NAME)` and
`(inject grammar: NAME ...)`. The command resolves NAME by replacing each `.`
with `/`, appending `.scm`, and reading that path relative to the directory of
the grammar named on the command line (for a built-in grammar, the working
directory). `words` resolves to `DIR/words.scm` and `lib.words` to
`DIR/lib/words.scm`, also from inside an imported file. If no such file exists,
NAME is looked up among the built-in standard grammars. A grammar reached this
way is compiled without its manifest.

## PARAMETERS

`--param NAME[=VALUE]` sets a parameter, which the grammar's `(when NAME: ...)`
forms test. It is accepted by every command that compiles a grammar and may
be repeated; for the same NAME the last one wins. Parameters from a package
manifest are set first, so `--param` overrides them.

- `--param NAME`: sets NAME.

- `--param NAME=` and `--param NAME=0`: clear NAME.

- `--param NAME=VALUE`: sets NAME for any other VALUE, including `false` and `00`.

## COMMANDS

### check

`tinyparse check GRAMMAR [--param NAME[=VALUE]]...`

Compiles GRAMMAR and prints nothing when it compiles. When it does not, prints
`error: ` followed by the fault text to standard error and exits with status 1.
The fault text has no line or column; see DIAGNOSTICS.

### run

`tinyparse run GRAMMAR --input FILE [--dump-tree] [--chunk N] [--trace] [--param NAME[=VALUE]]...`

Compiles or loads GRAMMAR and parses the bytes of FILE. Without `--dump-tree`
a successful parse prints nothing.

- `--input FILE`: The input to parse. `-` reads standard input. Required.

- `--dump-tree`: Prints the tree to standard output in the Tree dump format under OUTPUT FORMATS.

- `--chunk N`: Hands the input to the parser N bytes at a time. The parse runs once the
  last chunk has arrived, and the tree is the same for every N. N is read as a
  decimal integer, as strtol(3) reads it; when that gives 0 or less, the whole
  input is handed over at once.

- `--trace`: Prints one line per parser event to standard error, as `trace: EVENT NAME`.
  The start rule produces no events. EVENT is one of:

  - `rule.enter`: A rule named NAME is tried.

  - `rule.exit`: The rule NAME matched. A rule that fails produces no `rule.exit`.

  - `token`: A token matched and became a leaf. A literal token's NAME is its text.
    Extras produce no event.

  - `rule.memo-on`: The rule memo switched on while entering NAME (see `TINYPARSE_RULEMEMO`).

  - `rule.replay`: The result of NAME at this position was copied from the rule memo instead
    of parsed again.

After the parse, `run` prints its diagnostics to standard error, after the
tree when there is one. It exits with status 1 when the parse fails or when
any diagnostic has error severity, including a parse that recovered and
printed a tree. A failed parse prints no tree; when it recorded no
diagnostics, `run` prints `error: parse failed`. A parse that runs out of
memory prints `error: out of memory`, and any other failure prints
`error: parse failed (result N)`, where N is the number of the
`tinyparse_result`.

### pack

`tinyparse pack GRAMMAR --out FILE [--param NAME[=VALUE]]...`

Compiles GRAMMAR, with its imports and base grammar resolved and its
parameters applied, and writes it to FILE as a packed grammar, conventionally
named with `.tpg`. A packed grammar loads without its `.scm` sources:
`run`, `linearize` and `query` accept it as GRAMMAR, and a host program loads
it with `tinyparse_load_binary_grammar`. `--out` is required.

### c

`tinyparse c GRAMMAR --out FILE [--bundle | --parser | --parser-lib] [--name PREFIX] [--param NAME[=VALUE]]...`

Compiles GRAMMAR and writes C source to FILE. `--out` is required. The mode
flag selects what FILE contains; with several, `--parser-lib` wins over
`--parser`, and both win over `--bundle`.

- (no mode flag): A generated module: the packed grammar as a byte array and the function
  `tinyparse_string tinyparse_embedded_grammar(void)`, which returns it for
  `tinyparse_load_binary_grammar`. Parsing runs in the library's VM.

- `--bundle`: The generated module plus a `main` that parses standard input and prints
  one node kind per line, indented by depth, without fields or byte ranges. On
  a failed parse it prints each diagnostic's message and exits with status 1.
  A recovered parse prints its inserted nodes like any other node, without
  `MISSING`, prints no diagnostics and exits with status 0.

- `--parser`: A generated parser: one C function per rule and token, plus a `main` that
  parses standard input, prints the tree in the Tree dump format and the
  diagnostics like `run`, and exits with status 1 when the parse fails or any
  diagnostic has error severity.

- `--parser-lib`: The generated parser without `main`. It defines
  `tinyparse_result PREFIX_load(tinyparse_engine *, tinyparse_grammar **)`,
  and `tinyparse_result PREFIX_run(tinyparse_parser *)`. When the start
  rule's body is `(repeat ITEM)`, `(seq (repeat ITEM))` or
  `(seq (repeat ITEM) EOF)`, it also defines
  `tinyparse_result PREFIX_run_chunk(tinyparse_parser *)` for an input buffer
  that is still growing.

- `--name PREFIX`: Sets the prefix of every file-scope symbol in a generated parser. The
  default prefix is the grammar's name, the symbol after `(grammar`, so two
  grammars with different names link into one program without `--name`. A
  module or bundle ignores it.

Every mode links against `libtinyparse` or the amalgamation. A generated
parser (`--parser`, `--parser-lib`) also includes the library's private header
`internal.h`, which only a source tree provides, so compile it from the
`tinyparse` directory with `-I include -I src`. The C API reference describes
the generated symbols; see SEE ALSO.

### linearize

`tinyparse linearize GRAMMAR --input FILE [--param NAME[=VALUE]]...`

Parses FILE and prints the tree as the token stream a query matches against,
in the Linearized tree format under OUTPUT FORMATS. `--input` is required; `-` reads
standard input. A parse that fails prints its diagnostics and exits with
status 1. A parse that recovers prints the stream, prints no diagnostics and
exits with status 0.

### query

`tinyparse query GRAMMAR QUERY --input FILE [--param NAME[=VALUE]]...`

Parses FILE, runs the query in the file QUERY over the tree, and prints one
line per capture in the Query captures format under OUTPUT FORMATS. The query language is
described in the query language reference; see SEE ALSO. A query the library
rejects prints `tinyparse: error: invalid query` and exits with status 1,
after any captures of the forms before the rejected one. Parse failures and
recovered parses are handled as by `linearize`.

### node-types

`tinyparse node-types GRAMMAR [--param NAME[=VALUE]]...`

Compiles GRAMMAR and prints its node types to standard output as JSON, in the
format under OUTPUT FORMATS, Node types.

### init

`tinyparse init DIR [--amalgamation | --lib] [--grammar NAME]`

Creates DIR, if it does not exist, and writes a starter project into it,
replacing files of the same names:

- `grammar.scm`: A grammar for a greeting such as `hello world`, or with `--grammar NAME` the
  `grammar.scm` of the built-in standard grammar NAME.

- `main.c`: A program that reads `grammar.scm` from the working directory, compiles it,
  parses standard input and prints one node kind per line. It does not install
  a module resolver, class bindings or parameters, and gives the engine a fixed
  8 MiB block.

- `build.sh`: `cc main.c tinyparse.c -o app`, or with `--lib`
  `cc main.c -ltinyparse -o app`.

- `tinyparse.c`, `tinyparse.h`: The amalgamation, copied from the program. `--amalgamation` is the default;
  `--lib` writes neither file.

Because `main.c` provides no module resolver and no parameters, a project made
with `--grammar` builds and parses only for `std.json` and `std.shell`. For
the other standard grammars `./app` exits with status 1 and prints
`error: extends requires a module resolver` (`std.json5`, `std.jsonc`,
`std.typescript`, `std.tsx`), `error: inject requires a module resolver to find
the embedded grammar` (`std.fence`) or `error: out of memory reading string`
(`std.javascript`).

`init` rejects any other argument that begins with `-` with `tinyparse: error:
init: unknown option 'OPTION'`, and an unknown `--grammar` NAME with `tinyparse: error:
unknown grammar 'NAME'`; both exit with status 1 before writing anything.

### vendor

`tinyparse vendor DIR [--amalgamation | --lib]`

Creates DIR, if it does not exist, and writes the amalgamation into it:
`tinyparse.c` and `tinyparse.h`, or with `--lib` only `tinyparse.h`. Existing
files of the same names are replaced. Any other argument that begins with `-`
is rejected with `tinyparse: error: vendor: unknown option 'OPTION'` and exit
status 1, before anything is written.

### grammars

`tinyparse grammars`

Prints the names of the standard grammars built into the program, one per
line.

### man

`tinyparse man`

Prints this manual page in mdoc format to standard output.

### version

`tinyparse version`, `tinyparse --version`

Prints `tinyparse`, a space and the version, then a newline.

### help

`tinyparse help`, `tinyparse --help`, `tinyparse -h`, `tinyparse`

Prints a usage summary to standard output and exits with status 0.

## HOST SERVICES

Before it compiles a grammar, every command gives the library:

- a memory grant of 16 MiB whose grow callback takes more memory from
  `malloc` when a parse needs it;
- the module resolver described under GRAMMAR ARGUMENTS;
- four character classes for `(class NAME)`, each matching ASCII bytes only:
  `alpha` (`A-Z`, `a-z`), `digit` (`0-9`), `alnum` (`alpha` or `digit`)
  and `space` (space, tab, newline, carriage return).

A `(class NAME)` with any other NAME compiles and matches no byte.

## OUTPUT FORMATS

### Tree dump

`run --dump-tree` and the `main` of a `--parser` program print the tree in
pre-order, one node per line. This line is a `close` field holding a `)`
token at byte 8:

```text
      close: ) [8-9] [0,8]-[0,9]
```

A line holds, in this order:

- Indentation: Two spaces per level below the root.

- `MISSING `: Present when recovery inserted the node: a zero-width node for input that
  is not there.

- `FIELD: `: Present when the node is bound to a field of its parent.

- KIND: The name of the rule or token. A literal token's kind is its text, printed
  as is, so a literal newline token ends the line early.

- ` [LANGUAGE]`: Present on every node of an injected region: the name of the grammar that
  parsed it, such as `[std.json]`.

- ` [START-END]`: The byte range: 0-based, end-exclusive.

- ` [ROW,COLUMN]-[ROW,COLUMN]`: The start and end points: 0-based rows, and columns counted in bytes from
  the start of the row.

Trivia and error extras appear in place as nodes of their token kind. Skipped
extras and `EOF` produce no node.

### Linearized tree

`linearize` prints one line per event of a depth-first walk:

- `enter KIND`: A node with children starts.

- `leaf KIND TEXT`: A node without children, followed by its bytes as they appear in the input.
  The text of a `MISSING` node is empty.

- `exit KIND`: The node of the matching `enter` ends.

Fields and language tags are not printed.

### Query captures

`query` prints one line per capture, in the order the query reports them:

```text
@NAME START-END TEXT
```

NAME is the capture name, START-END the captured node's byte range (0-based,
end-exclusive), and TEXT the captured bytes, printed as they are, including
any newlines.

### Node types

`node-types` prints a JSON array with one object per rule, hidden rules
included, then one per named token. Literal tokens are not listed. Each object
has:

- `"type"`: The node kind.

- `"named"`: Always `true`.

- `"fields"`: An object with one member per field the rule uses. Each member is
  `{ "types": [...] }`, listing `{ "type": KIND, "named": true }` for every
  named kind the field can hold; a field that holds only literal tokens has an
  empty list. Tokens have an empty object.

- `"children"`: Present on a rule with named children outside any field:
  `{ "multiple": BOOL, "required": false, "types": [...] }`. `"multiple"` is
  `true` when more than one kind can appear there.

The layout follows tree-sitter's `node-types.json`, but the meanings differ:
tree-sitter's `"multiple"` says whether more than one child can appear, its
`"required"` is computed, and its field members carry `"multiple"` and
`"required"` and list unnamed types.

## ENVIRONMENT

- `TINYPARSE_RULEMEMO`: Controls the rule memo, which records each rule's result at each position
  so a backtracking parse does not parse it again. `always` turns it on from
  the start of every parse; `off` turns it off. Unset or set to any other
  value, it switches on once a parse has made more than 32 × L + 65,536 rule
  calls, where L is the input length in bytes. Every command that compiles a
  grammar reads it. The tree is the same in every mode; the setting exists to
  test the memo.

- `TINYPARSE_TRACE_LIFECYCLE`: When set and not empty, `run` prints `lifecycle: STATE` to standard error
  after the parse, before the tree and the diagnostics. STATE is `finished`
  when the parser ran over the whole input, whether or not the input matched,
  and `failed` when a chunk was dropped before the parse. Other commands
  ignore it.

## FILES

- `package.scm`: The manifest of a grammar package; see GRAMMAR ARGUMENTS.

- `grammar.scm`: The grammar of a package, in the same directory as `package.scm`.

- `*.tpg`: A packed grammar written by `pack`. Its first four bytes are `TPG1`.

- `DIR/grammar.scm`, `DIR/main.c`, `DIR/build.sh`, `DIR/tinyparse.c`, `DIR/tinyparse.h`: The files `init` and `vendor` write.

## EXIT STATUS

- `0`: Success, including `help`, `version`, `man` and `grammars`, and a
  `linearize` or `query` whose parse recovered.

- `1`: A grammar that does not compile, a parse that fails, a `run` with any
  error-severity diagnostic, an unreadable file, a missing required option, a
  rejected query, an unknown option for `init` or `vendor`, an unknown
  `--grammar` name for `init`, and running out of memory.

- `2`: An unknown command. The usage summary goes to standard error.

Commands other than `init` and `vendor` ignore options they do not know and
extra arguments.

## EXAMPLES

Parse JSON with the built-in `std.json` grammar:

```sh
printf '{"a": [1, 2]}' > in.json
tinyparse run std.json --input in.json --dump-tree
```

Parse standard input in 1-byte chunks:

```sh
printf '(a (b c))' | tinyparse run examples/sexpr/sexpr.scm --input - --chunk 1 --dump-tree
```

Pack a grammar once and parse with the packed file:

```sh
tinyparse pack std.json --out json.tpg
tinyparse run json.tpg --input in.json --dump-tree
```

Print the first item of every list:

```sh
printf '(a (b c))' > input.txt
printf '%s\n' '(List open: (_) . (Form) @first)' > first.scm
tinyparse query examples/sexpr/sexpr.scm first.scm --input input.txt
```

Generate a parser library with the prefix `sexpr` and compile it from the
`tinyparse` directory:

```sh
tinyparse c examples/sexpr/sexpr.scm --out sexpr_parser.c --parser-lib
cc -std=c99 -I include -I src -c sexpr_parser.c
```

Start a project that builds with one C compiler:

```sh
tinyparse init hello
cd hello && sh build.sh && echo 'hello world' | ./app
```

Read this page in a terminal:

```sh
tinyparse man | mandoc -a
```

## DIAGNOSTICS

Errors from the command itself are printed as `tinyparse: error: MESSAGE`. A
grammar that fails to compile or load prints `error: FAULT`, the text the
library reports, with no position. A parse prints each diagnostic as

```text
MESSAGE [CODE]
  help: HELP
```

The help line appears only when the diagnostic has help text. The library's
own messages begin with `error: `, as in `error: unexpected input
[parse.syntax]`; a message from a grammar's `recover` form is printed as the
grammar wrote it. Labels, notes and positions are not printed. Diagnostics of
an injected region are not reported. The codes and fields are listed in the
diagnostics reference; see SEE ALSO.

## SEE ALSO

cc(1), mandoc(1)

The references in the `tinyparse` directory of the source repository,
https://github.com/tinyactors-dev/tinyactors:

- [`docs/reference/grammar.md`](docs/reference/grammar.md): the grammar language and `package.scm`
- [`docs/reference/query-language.md`](docs/reference/query-language.md): the query language
- [`docs/reference/diagnostics.md`](docs/reference/diagnostics.md): diagnostic codes and faults
- [`docs/reference/c-api.md`](docs/reference/c-api.md): the C API and the symbols of generated C
- [`docs/reference/std-grammars.md`](docs/reference/std-grammars.md): the standard grammars

## AUTHORS

Dario Hamidi

## BUGS

`check`, `node-types` and `c` read a packed grammar as `.scm` source and use
memory until an allocation fails.

A diagnostic from an `(error NAME)` extra is recorded again each time the
parser backtracks over the same bytes, so one error node can have several
`extra.error` diagnostics. A `recover` form that applies at several nesting
levels at the end of the input reports its diagnostic more than once:
`examples/recover/list.scm` reports it 3 times for `((a` and 7 times for
`(((a`.
