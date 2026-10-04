# tinyparse documentation

This index lists every tinyparse document, grouped by what the reader needs:
a lesson, a task, a fact to look up, or the reasons behind a design. Each line
says who the document is for.

## Tutorials

- [Write your first grammar](tutorials/first-grammar.md): start here. For a
  developer new to tinyparse; builds an s-expression grammar in six steps
  with the `tinyparse` command.
- [Parse input from a C program](tutorials/embed-in-c.md): for a C developer
  who has a grammar and wants to compile it, parse with it and walk the tree
  from a program.

## How-to guides

- [Install tinyparse](how-to/install.md): for anyone who needs the command or
  the library, from a release tarball, `install.sh`, a source tree or the
  amalgamation.
- [Ship a grammar inside your program](how-to/embed-grammar.md): for a host
  author who wants to ship without grammar text, as a packed grammar or a
  generated module.
- [Ship a generated C parser](how-to/generated-parser.md): for a host author
  whose grammar is settled and who wants it compiled into the program.
- [Run tinyparse without libc](how-to/freestanding.md): for a developer
  targeting bare metal, a kernel or a sandbox.
- [Supply imports, classes and parameters from a C host](how-to/host-services.md):
  for a host author whose grammar works with the command but not in their own
  program.
- [Recover from and report syntax errors](how-to/handle-errors.md): for
  grammar and host authors whose users write broken input.
- [Lex tokens that depend on context](how-to/context-sensitive-lexing.md): for
  a grammar author whose tokens mean different things in different places.
- [Split, extend and combine grammars](how-to/compose-grammars.md): for a
  grammar author who shares rules between files, writes a dialect or parses
  one language inside another.
- [Find nodes with a query](how-to/query.md): for a user who wants parts of a
  parsed file back.
- [Examples](../examples/README.md): worked examples, each a grammar and
  sometimes a C program, run on real input.

## Reference

- [tinyparse(1)](../tinyparse.1.md): the command's subcommands, options,
  environment variables, output formats and exit statuses. The same page is
  installed as a manual page; `tinyparse man | mandoc -a` shows it.
- [Grammar language](reference/grammar.md): every form of a `.scm` grammar,
  the regex dialect and the `package.scm` manifest.
- [Query language](reference/query-language.md): the patterns, captures and
  predicates of `tinyparse query` and `tinyparse_query`.
- [C API](reference/c-api.md): every declaration in `include/tinyparse.h`,
  the result codes, memory sizing and the symbols of a generated parser.
- [Diagnostics](reference/diagnostics.md): every diagnostic code and the
  faults a parser call reports.
- [Standard grammars](reference/std-grammars.md): what each grammar under
  `grammars/std` accepts and where it is wrong.
- [Release artifacts](reference/artifacts.md): what each release tarball and
  the amalgamation contain, the library's symbols, packed grammar
  compatibility and the supported platforms.

## Explanation

- [Design of tinyparse](explanation/design.md): for a developer deciding
  whether tinyparse fits a program; the choices and what each costs.
- [How tinyparse shapes the tree](explanation/tree-shape.md): for grammar and
  host authors who want to predict the tree a grammar builds.
- [Performance](explanation/performance.md): what the benchmarks measure,
  their results and why the numbers come out as they do.

## For contributors

- [Developing tinyparse](../DEVELOPMENT.md): building, testing, the
  correctness gates, benchmarks and releases.
- [Architecture](explanation/architecture.md): how the source is organized and
  which properties a change must keep.
- [Test case format](../test/README.md): the `.test` files of the test suite.

## Records

These documents record past decisions and research. They do not describe the
current code.

- [HISTORY.md](../HISTORY.md): where tinyparse came from.
- [Decision log](decisions.md): the decisions made while building it.
- [PEG versus GLR](peg-vs-glr.md): the research behind the parsing strategy.
- [Archive](archive/README.md): design notes and plans written before or
  during the implementation.
