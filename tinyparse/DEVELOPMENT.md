# Developing tinyparse

This guide is for a contributor changing tinyparse. It gives the commands
that build the project, run the tests and the correctness gates, run the
benchmarks, edit the manual page, add an example and prepare a release. How
the library is put together, and the constraints a change has to keep, are in
[Architecture](docs/explanation/architecture.md).

## Prerequisites

- A checkout of the tinyactors monorepo. tinyparse is its `tinyparse/`
  directory, and every command below runs there unless a step says otherwise.
- [mise](https://mise.jdx.dev). Run `mise install` once at the monorepo root:
  it installs the pinned clang 22, LLVM's binutils, zig, GNU tar and gzip,
  Ruby and ast-grep. No task uses a compiler or binutil from `PATH`.
- An x86-64 host. `mise run build`, and with it `mise run test`, builds the
  examples, and `examples/jit/jit.c` stops with an error on any other
  architecture (see [Examples](examples/README.md#prerequisites)).

The benchmarks need more; their section lists it.

## Build

Each task is a Ruby file task in [`mise/tasks/`](mise/tasks/):

```sh
mise run build        # build/: libtinyparse.a, the CLI, the test harness, the examples
mise run examples     # build/examples/<name>/: the example programs alone
mise run generate     # build/generated/: the manual page, standard grammars and amalgamation the CLI embeds
mise run dist         # dist/<target>/: library and CLI for x86_64-linux and aarch64-macos, with zig cc
mise run amalgamate   # dist/amalgamation/: tinyparse.h, tinyparse.c, tinyparse_cli.c
mise run release      # dist/release/: the release tarballs (runs dist and amalgamate first)
mise run man          # print the manual page the CLI embeds
mise run clean        # remove build/ and dist/
```

Compiling and linking are cached by content, so a second run of an unchanged
build only hashes its inputs. To use the CLI you built from the command line,
put it on your `PATH`:

```sh
export PATH="$PWD/build/bin:$PATH"
```

## Run the tests

```sh
mise run test
```

The `test` task depends on `build`, `amalgamate`, `dist`, `release` and every
gate under [`mise/tasks/gate/`](mise/tasks/gate/), so the first run also
cross-builds the release targets with zig. mise runs the gates side by side,
then the task runs the suite: every `test/suite/**/*.test` case, interpreted
by `build/bin/harness` against the ledger `test/xfail.list`. The last lines
of the output give the suite's time and result, then `test: GREEN` (the
build and gate output before them is cut):

```text
...
[//tinyparse:test] ok     20.2s  suite              PROGRESS=500/500  PASS=500  xfail=0  XPASS=0  FAIL=0  xfail-ledger=0  RESULT: GREEN -- every test passes and the xfail ledger is empty.
[//tinyparse:test] test: the suite and every gate (--full adds the exhaustive cc-diff, exports, nesting, parser-nocrash, reclaim, reuse-regression, vm-diff)
[//tinyparse:test] test: GREEN
[//tinyparse:test] Finished in 20.38s
Finished in 29.08s
```

The task is green only when every case passes and the ledger is empty.

mise skips the suite when none of the task's sources (listed in `#MISE
sources` in [`mise/tasks/test`](mise/tasks/test)) changed since its last
green run, and prints `sources up-to-date, skipping`; the gates still run. To
run the suite anyway:

```sh
mise run --force test
```

`mise run --force test` took 29 seconds on 2026-10-04 on a 4-core 2.10 GHz
Xeon running x86-64 Linux.

`mise run test --full` adds the exhaustive version of each gate that has one,
listed under Run the correctness gates below. The monorepo's
`mise run test:full` passes `--full` down.

Some cases assume an x86-64 Linux host: the cases under `test/suite/dist/`
that run the Linux CLI or read ELF symbol tables, and `cli/run-out-of-memory`,
which relies on `ulimit -v`. The macOS artifacts are cross-built and their
symbols checked, but nothing runs them.

## Run part of the suite

The harness takes a suite directory, so you can point it at one
subdirectory. It reads the tools from the environment the `test` task
normally sets (see the environment table in
[Test case format](test/README.md)); the cases under `core/` and `examples/`
need these four, and `build/tools/` exists after one `mise run test`:

```sh
export TP="$PWD/build/bin/tinyparse" TP_LIB="$PWD/build/lib/libtinyparse.a" \
  TP_EXAMPLES="$PWD/build/examples" TP_CC="$PWD/build/tools/cc"
build/bin/harness --suite test/suite/core --xfail test/xfail.list --cwd "$PWD"
```

The harness prints one line per case and a summary, and exits with status 0:

```text
  PASS   empty-input
  PASS   extras-comment
  PASS   field-named
  PASS   lifecycle
  PASS   repeat-many
  PASS   rule-memo-grows
  PASS   sexpr-nested
  PASS   streaming-chunks
  PASS   syntax-error

PROGRESS=9/9  PASS=9  xfail=0  XPASS=0  FAIL=0  xfail-ledger=0
RESULT: GREEN -- every test passes and the xfail ledger is empty.
```

A case's name is its path below the `--suite` directory, so a name in
`test/xfail.list` (written below `test/suite/`) does not match when you point
`--suite` at a subdirectory.

## Add a test case

1. Pick the directory under `test/suite/` for the behavior (`core/`, `cli/`,
   `examples/`, `query/` and others) and write a `.test` file there. The
   format is in [Test case format](test/README.md). This case parses nested lists with
   an example grammar:

   ```text
   # Three nested lists parse as three List nodes, with no ERROR node.
   <<<grammar
   examples/sexpr/sexpr.scm
   <<<input
   (a (b (c d)) e)
   <<<assert
   exit 0
   out-order Source ~ List ~ Atom
   out-count 3 List
   out-absent ERROR
   ```

2. Run the directory with the harness, as Run part of the suite shows, until
   the case passes.
3. If the case reads a directory that is not yet in the `#MISE sources` list
   of [`mise/tasks/test`](mise/tasks/test), add it there, or a change to that
   directory will not rerun the suite.
4. Run `mise run test`.

Do not delete, skip or weaken a case to get green. A case for a behavior
that is not implemented yet can be listed in `test/xfail.list` while you work
on it; the harness then reports it as `xfail` and exits with status 2, so
`mise run test` stays red until the line is removed.

## Update a golden file

Some gates compare a program's output with a committed file. When a change
alters the tree on purpose, regenerate the file with the command the gate
runs, read the difference, and commit it with the change:

| Golden file | Gate | Command that writes it |
|-------------|------|------------------------|
| `examples/import/fixtures/expected.tree` | `import-c` | `tinyparse run examples/import/main.scm --input examples/import/fixtures/sample.words --dump-tree > examples/import/fixtures/expected.tree` |
| `examples/embed/expected.txt` | `multiparser`, `exports` | `build/examples/embed/embed examples/embed/sample.md > examples/embed/expected.txt 2>&1` |
| `examples/nesting/expected.txt` | `nesting` | `build/examples/nesting/nesting examples/nesting/sample.md > examples/nesting/expected.txt` |

```sh
git diff --stat examples/
```

A change that claims to change performance alone leaves every golden file and
every gate as they were.

## Run the correctness gates

The gates check properties across many inputs that a single case cannot,
mostly that every engine builds the same tree. `mise run test` runs all of
them. To run one:

```sh
mise run gate:cc-diff
mise run gate:cc-diff --full
```

Each gate works in its own temporary directory. The Time column is the fast
pass, with all gates running side by side, over two runs on 2026-10-04 on
the machine above; the time in the `--full` column is one run of
`mise run --force test --full`, which took 58 seconds in all.

| Gate | Property | Inputs | `--full` | Time |
|------|----------|--------|----------|------|
| `bufferfriendly` | a generated parser fed a growing buffer builds the VM's tree at every chunk size | `examples/bufferfriendly` on its sample and a document of top-level atoms, in chunks of 1, 2, 3, 5, 8, 17, 64 and 1,000,000 bytes | none | 0.4–0.5 s |
| `cc-diff` | parsers generated with `tinyparse c --parser` print the VM's tree, diagnostics and exit status | std.json, std.jsonc and std.json5, std.fence, the heredoc grammars, the family base fixture, two inputs of the shell corpus | every input of the JavaScript, TypeScript and shell corpora (27 s) | 5.4–7.3 s |
| `diag-diff` | a generated parser reports the VM's diagnostics (message, code, help, severity, `MISSING` nodes) | broken inputs to `examples/recover/list.scm` | none | 1.1–1.6 s |
| `exports` | a shared object built with `examples/embed/embed-lib.sh` exports its parsers' entries and the public API, nothing else | std.json alone; the two parsers of `examples/embed` | builds at `-O2` instead of `-O0` (36 s) | 4.3–6.0 s |
| `import-c` | a grammar that imports another compiles to a C parser that builds the golden tree and the VM's | `examples/import` | none | 1.1–1.2 s |
| `multiparser` | two generated parsers with distinct `--name` prefixes link into one program and build the golden trees | `examples/embed` | none | 0.6–0.9 s |
| `nesting` | a generated std.fence parser with generated std.json and std.javascript parsers registered builds the VM's tree, and the registered parsers parse the regions | `examples/nesting` | builds the shared object at `-O2` (30 s) | 3.2–4.2 s |
| `no-statics` | the library and generated parsers keep no mutable static data | `src/*.c` through an ast-grep rule; `libtinyparse.a`; parser-lib objects for std.json, std.fence and std.shell | none | 4.6–6.5 s |
| `parser-nocrash` | generated parsers end deep input with a syntax error instead of a crash, under a 512 KiB stack | std.json and std.shell, 200,000 bytes nested | also std.javascript and std.tsx (21 s) | 3.4–4.5 s |
| `reclaim` | parses in one engine peak at under twice the memory of one parse | a 264,060-byte JavaScript file, 12 parses | 300 parses (51 s) | 4.9–6.6 s |
| `reuse-regression` | one engine reused across many parses builds the same tree every time | 20,000 JSON objects, 40 parses with the generated std.json parser | 1,500 lines of TSX, 120 parses with the generated std.tsx parser (26 s) | 5.1–6.9 s |
| `vm-diff` | the VM builds the same tree with its rule memo off, adaptive and forced on | std.json, std.jsonc and std.json5, every example and fixture grammar, `test/gates/backtrack.scm`, two inputs each of the corpora | every input of the JavaScript, TypeScript and shell corpora (21 s) | 5.1–6.2 s |
| `vm-nocrash` | the VM survives deep input and a long token under a 512 KiB stack | 200,000 unclosed `[`; a 400,000-byte number | none | 1.5–2.1 s |

`vm-diff` also checks that no library source calls into another project's
runtime; [Architecture](docs/explanation/architecture.md) explains why.

## Run the benchmarks

Benchmarks measure time, so they are opt-in tasks under
[`mise/tasks/bench/`](mise/tasks/bench/) and never part of `mise run test`.
Run each one with nothing else running on the machine. Each runs a script in
[`bench/`](bench/) with the test environment; the method behind the numbers
is in [Performance](docs/explanation/performance.md).

Prerequisites:

- network access, to download the pinned references, clone the tree-sitter
  runtime and install npm packages;
- Go, for the `tsgo` arm of `bench:tsc` and `bench:parsers` (Node.js and npm
  come from the monorepo's pinned `node`);
- valgrind, to profile;
- `python3`, which extracts the corpora;
- `git`, which clones the tree-sitter runtime and typescript-go.

Steps:

1. Download the references pinned in [`reference/PINS.txt`](reference/PINS.txt)
   into `reference/`, each as a tarball at its commit:

   ```sh
   mise run bench:fetch
   ```

2. Run the benchmark you need:

   ```sh
   mise run bench:javascript             # std.javascript against tree-sitter, in process, over tree-sitter's JavaScript corpus
   REGEN=1 mise run bench:react          # std.tsx against tree-sitter on the React codebase; writes bench/react-corpus/MANIFEST.txt
   mise run bench:tsc                    # tsc and tsgo against tinyparse, parse only, on the React corpus; builds the tsgo harness
   mise run bench:parsers                # six parsers, parse only, on the React corpus
   mise run bench:rule-memo              # the CLI's time on one JSON document with the rule memo off and in its default mode
   mise run bench:javascript-coverage    # JS_COVERAGE=parsed/total over tree-sitter's JavaScript corpus
   mise run bench:typescript-coverage    # TS_COVERAGE=parsed/total over tree-sitter's TypeScript corpus
   mise run bench:perf-gate              # mise run test, then bench:javascript against test/perf-baseline.txt
   ```

   `bench:react` with `REGEN=1` writes the corpus manifest that `bench:tsc`
   and `bench:parsers` read; without it they stop with `FATAL: corpus
   manifest missing`. `bench:parsers` reports its `tsgo` arm as not obtained
   unless `bench:tsc` built the harness first. `ITERS` sets the iteration
   count (200 for `bench:javascript`, 50 for `bench:react`, 30 for
   `bench:parsers`), and `bench:rule-memo` takes the number of runs as an
   argument (400 by default).

3. Check for a slowdown with `bench:perf-gate`. It fails when the median
   parse time of `bench:javascript` is more than 5% above the ceiling in
   [`test/perf-baseline.txt`](test/perf-baseline.txt). When a change makes
   parsing faster, lower the ceiling in that file; it is never raised.

The first benchmark clones the tree-sitter runtime's current HEAD into
`bench/tree-sitter` and writes its commit to `bench/tree-sitter-runtime.pin`.
The runtime is not pinned in `reference/PINS.txt`, so two machines can
benchmark different tree-sitter commits; compare builds only against the same
clone.

`TINYPARSE_RULEMEMO=always` turns the rule memo on from the first rule call.
Over the React corpus a benchmark run with it set does not finish in a
practical time; the published numbers use the default adaptive mode.

To profile the VM, run the benchmark harness that `bench:javascript` builds,
`bench/bench_tinyparse`, under callgrind. It carries function symbols, so no
special build is needed:

```sh
out=$(mktemp -d)/callgrind.out
valgrind --tool=callgrind --callgrind-out-file="$out" \
  bench/bench_tinyparse bench/corpus/MANIFEST.txt 2 grammars/std/javascript/grammar.scm
callgrind_annotate --threshold=92 "$out"
```

When you report a benchmark, give throughput and memory together, name the
machine, and compare two builds run side by side on that machine; never
compare with numbers from another day or machine.

## Edit the manual page

The manual page has two committed files: [`tinyparse.1.md`](tinyparse.1.md),
the source you read, and [`tinyparse.1`](tinyparse.1), the mdoc page the CLI
embeds and `tinyparse man` prints. No task converts one into the other, so
make every change in both files. Then rebuild the CLI, which embeds the page,
and render it with an mdoc renderer such as `mandoc`, which the pinned
toolchain does not include:

```sh
mise run cli
build/bin/tinyparse man | mandoc -a
```

The `cli/man` case runs `tinyparse man`, and the `dist/` cases check that
the tarballs ship `tinyparse.1`.

## Add an example

An example teaches one technique, shows it running on real input, and stays
correct because a test pins it.

1. Write down the one lesson. If an existing example in
   [Examples](examples/README.md) already teaches it, extend that example.
2. Create `examples/<name>/` with the grammar and, for a host program, its C
   file. The grammar matches nested input with recursive rules, keeps tokens
   to regular lexemes, and puts the parts a reader needs in fields. A host
   program reads node kinds, fields and byte ranges through the cursor
   functions and does not re-scan the source or build a second tree.
   [How tinyparse shapes the tree](docs/explanation/tree-shape.md) explains
   why the tree is meant to be used directly.
3. For a host program, add an entry to the `EXAMPLES` table in
   [`mise/tasks/examples`](mise/tasks/examples), then run `mise run examples`.
   The program is compiled with `-Wall -Wextra -pedantic -Werror`.
4. Pin the lesson with a case under `test/suite/examples/` or a gate, and
   cover the nested input when the lesson is about nesting.
5. Write `examples/<name>/README.md` in the shape the other example READMEs
   use: what it shows, Prerequisites, Run it, How it works, Limits, Files. The
   output in it is pasted from a real run, and Limits states where the example
   gives a wrong answer or fails.
6. Add a row to the right table in [`examples/README.md`](examples/README.md),
   then run `mise run test`.

## Prepare a release

The version is in [`VERSION`](VERSION). To build what a release ships:

```sh
mise run release
```

It writes `tinyparse-<version>-x86_64-linux.tar.gz`,
`tinyparse-<version>-aarch64-macos.tar.gz` and
`tinyparse-<version>-amalgamation.tar.gz` into `dist/release/`. Their
contents are listed in [Release artifacts](docs/reference/artifacts.md). The
tarballs are deterministic: sorted entries, one modification time (the
commit's), owner 0, no timestamp in the gzip header, packed with the pinned
GNU tar and gzip.

Before packing, `mise run dist` builds each target with zig cc, compiles the
shipped files with `-g0` and strips the CLI's debug information, so no path
of the build machine ends up in them, and then checks:

- each file is built for its target's architecture;
- `libtinyparse.a` leaves undefined only `memcpy`, `memset`, `memmove` and
  `memcmp`;
- the shared library is compiled with `-fvisibility=hidden` and
  `TINYPARSE_API` set to default visibility, exports exactly the functions
  `include/tinyparse.h` declares with `TINYPARSE_API`, names itself by its
  versioned file and loads no other library;
- the CLI loads no library beside the platform's runtime: none on Linux,
  where it is a static musl binary, and libSystem on macOS.

`mise run test` depends on `release`, and the cases under `test/suite/dist/`
unpack and run what it built, so the tarballs are the files the suite tested.

Nothing publishes a release. `mise run release` builds the tarballs and
stops. A published release would be tagged `tinyparse/v<version>` in the
monorepo and released as `v<version>` in tinyparse's release repository,
`tinyactors-dev/tinyparse`, which is where [`install.sh`](install.sh)
downloads from (the monorepo's `Release.push_and_release!` pushes a tag
made before it, then publishes it there). Two pieces it needs are missing:

- **A changelog.** A component release in the monorepo needs a section in the
  component's `CHANGELOG.md` that `mise run changelog:check` accepts.
  tinyparse has no `CHANGELOG.md` and is not listed in `COMPONENTS` in the
  monorepo's `mise/lib/changelog/components.rb`.
- **A publish step.** tinyparse's `mise run release` packs the tarballs but
  neither tags it nor calls `Release.push_and_release!`.
