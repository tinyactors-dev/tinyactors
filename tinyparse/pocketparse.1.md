---
date: 2026-06-20
os: ax
---

# NAME

pocketparse - compile grammar data into running parsers at runtime

# SYNOPSIS

- pocketparse check GRAMMAR
- pocketparse run GRAMMAR --input FILE [--dump-tree]
- pocketparse linearize GRAMMAR --input FILE
- pocketparse query GRAMMAR QUERY --input FILE
- pocketparse node-types GRAMMAR
- pocketparse init DIR [--amalgamation|--lib] [--grammar NAME]
- pocketparse vendor DIR [--amalgamation|--lib]
- pocketparse grammars
- pocketparse man
- pocketparse version
- pocketparse --help

# DESCRIPTION

pocketparse is a small, embeddable C library that compiles grammar data
into running parsers at runtime. It is a pocket knife for C programs that
need parsing without adopting a generator-heavy toolchain, a scripting
runtime, libc, malloc, a filesystem, or OS assumptions.

The core promise is simple: give pocketparse grammar bytes and input
bytes, and it gives back a streaming parser, a concrete syntax tree, and
structured diagnostics. The library itself is freestanding — no libc, no
IO, no allocation outside a host-granted memory pool — and is powered
internally by the ax actor system. This program is the hosted command-line
shell around that library: it reads files, renders diagnostics, and prints.

The native grammar surface is Scheme-shaped data, the `.scm` format. It is
data, not executable Scheme: lists, symbols, keyword symbols ending in a
colon, strings, integers, byte and hex literals, and line comments. A
grammar declares tokens, extras, and rules built from the combinators
`seq`, `choice`, `repeat`, `optional`, and `field`.

# COMMANDS

check GRAMMAR [--param NAME[=VALUE]]
: Read and validate a grammar without parsing any input. Report malformed
  grammar as a diagnostic on standard error and exit non-zero; a valid
  grammar exits zero.

run GRAMMAR --input FILE [--dump-tree] [--param NAME[=VALUE]]
: Compile a grammar, parse the bytes of the input file against it, and,
  with --dump-tree, print the resulting concrete syntax tree to standard
  output -- one node per line, indented by depth, each line ending with the
  node's span as a byte range and a 0-based [line, column] range, tree-sitter
  style. Diagnostics are rendered to standard error.

  --param sets a compile-time grammar parameter, the value the (when NAME:
  X) combinator gates a production on: a bare --param NAME sets it truthy,
  --param NAME=0 clears it. The flag may be repeated to set several
  parameters. A grammar that extends a base with (extends NAME) and gates
  productions on parameters compiles differently for each set of --param
  values.

  When GRAMMAR names a directory (or a package.scm file), it is read as a
  grammar package: the package.scm manifest is parsed, its params: are
  bound as compile-time parameters, and the sibling grammar.scm is compiled
  with those parameters in scope. A --param on the command line overrides a
  manifest default. check accepts a package the same way.

  A GRAMMAR may also be a bare package name, such as std.json. When no
  filesystem search path resolves the name, pocketparse falls back to the
  registry of standard grammars embedded in the binary, so a standard
  grammar parses with no package on disk. A filesystem package of the same
  name found first shadows the embedded copy. The same fallback applies to a
  base named by (extends NAME) or a module named by (import NAME): the
  resolver tries the filesystem first and, failing that, looks the name up in
  the embedded registry, so an embedded package such as std.jsonc resolves
  its (extends std.json) base with no files at all.

linearize GRAMMAR --input FILE [--param NAME[=VALUE]]
: Compile a grammar, parse the input file against it, and print the parse
  tree's linearization to standard output: one ENTER/LEAF/EXIT token per line.
  An internal node prints as `enter KIND` then, after its children, `exit KIND`;
  a leaf (token) node prints as `leaf KIND TEXT`. This is the flattened stream a
  query parses — a query matches a pattern by parsing this token stream — and the
  command is the debugging view of it. GRAMMAR is resolved exactly as for run.

query GRAMMAR QUERY --input FILE [--param NAME[=VALUE]]
: Compile a grammar, parse the input file against it, then run the QUERY file
  (a `.scm` query) over the resulting tree and print one line per capture as
  `@NAME START-END TEXT`. A query is a parser over the linearized tree: a
  pattern such as `(Item word: (Word) @w)` matches by parsing the ENTER/LEAF/EXIT
  stream the linearize command prints. A pattern matches at any depth (descendant
  matching skips one balanced subtree); a `.` anchor between two children forbids
  that skip, requiring them adjacent; `_` is the wildcard node; a `field:` keyword
  binds a child through its field; `@name` captures the node it follows. A
  wrapping list pairs a pattern with predicates: `(#eq? @c "text")` keeps a match
  only when the capture's bytes equal the text, and `(#match? @c "regex")` only
  when the engine's regex matches them. Both predicates are built in and need no
  host binding. GRAMMAR is resolved exactly as for run.

node-types GRAMMAR [--param NAME[=VALUE]]
: Compile a grammar and print its node-types schema to standard output as a
  JSON document, the typed surface tools read. The schema follows
  tree-sitter's node-types.json shape: a JSON array of node entries, one per
  named rule and named token. Each entry carries its "type" name, a "named"
  flag, and a "fields" object mapping each field name to the named types that
  field may hold; a rule with unnamed positional children also carries a
  "children" object. A rule with no fields still emits an empty "fields"
  object, so the key is always present. GRAMMAR is resolved exactly as for
  check and run, so a package directory or an embedded grammar name works too.

init DIR [--amalgamation|--lib] [--grammar NAME]
: Seed a new project in DIR. In --amalgamation mode, the default, it drops
  the whole engine as two files — pocketparse.c, with the ax actor system
  folded in and hidden, and pocketparse.h — alongside a starter grammar.scm,
  a main.c that compiles that grammar at runtime and dumps the parse of
  standard input, and a one-line build.sh. The seeded project then builds
  with a single `cc main.c pocketparse.c -o app` and needs no pocketparse
  tooling on the path. The embedded engine source is baked into the program
  at build time, like the manual page and the grammar registry, so init needs
  no install prefix and no network. In --lib mode it writes the same main.c
  and grammar.scm but copies no engine source, leaving the project to link an
  installed libpocketparse. With --grammar NAME the starter grammar is the
  named standard grammar (for example std.json) instead of the built-in hello
  grammar.

vendor DIR [--amalgamation|--lib]
: Drop or refresh just the engine source in an existing project DIR, writing
  no scaffold. In --amalgamation mode it writes pocketparse.c and
  pocketparse.h; in --lib mode it writes only pocketparse.h, for a project
  that links an installed libpocketparse.

grammars
: List the standard grammar packages embedded in the binary, one name per
  line. These are the grammars resolvable by name with no filesystem present
  (for example std.json); the registry is baked into the program at build
  time, like the manual page.

man
: Print this manual page, in mdoc form, to standard output. The page is
  embedded in the program at build time, so it is available with no
  filesystem lookup.

version
: Print the program name and version, then exit zero.

--help
: Print a short usage summary and exit zero. Running pocketparse with no
  arguments prints the same usage summary.

# EXIT STATUS

pocketparse exits 0 on success, and non-zero when a grammar fails to
validate, an input fails to parse, or the command line is malformed.

# EXAMPLES

Validate a grammar:

```
pocketparse check examples/sexpr/sexpr.scm
```

Parse an input file and dump its tree:

```
pocketparse run examples/sexpr/sexpr.scm --input sample.txt --dump-tree
```

Read this manual:

```
pocketparse man | mandoc
```

# SEE ALSO

cc(1), mandoc(1), mdc(1)

# AUTHORS

The ax project
