# Test case format

This reference is for a contributor who writes or debugs a case of the
tinyparse test suite. It describes the `.test` file format, the environment a
case runs in, and how the harness reports and ledgers results. How to run the
suite and add a case is in [Developing tinyparse](../DEVELOPMENT.md).

A case is a data file, `test/suite/<group>/<name>.test`. It runs one shell
command and checks the command's exit status and output. The command can do
anything a shell script can, including compiling and running a C fixture
kept beside the case, as `core/lifecycle.test` does with `core/lifecycle.c`.
The files in `test/gates/` are not cases: the gates under `mise/tasks/gate/`
read them.

A case's name is its path below the suite directory without `.test`:
`test/suite/cli/help.test` is `cli/help`.

## Sections

A line whose first three bytes are `<<<` starts a section, and the rest of
the line names it. The section's body is every following line up to the next
`<<<` line or the end of the file. The harness ends each line of a body with
a line feed, so a non-empty `input` body always ends with one. A line before the
first section, and the body of a section with an unknown name, are ignored.

| Section | Body |
|---------|------|
| `input` | the default input, written to a temporary file named by `$IN` |
| `input NAME` | a named input, written to a temporary file named by `$NAME` |
| `grammar` | a grammar path or name; the harness builds the command from the template below |
| `mode` | `run` or `check`, the template to use; default `run` |
| `args` | arguments appended to the templated command |
| `cmd` | a shell command, run with `/bin/sh`; it takes precedence over `grammar` |
| `stdin` | bytes fed to the command's standard input; without it, standard input is empty |
| `assert` | one assertion per line |
| `stdout` | the exact expected standard output |
| `stderr` | the exact expected standard error |
| `end` | ends the current section; optional |

A case needs `cmd` or `grammar`; with neither, it fails.

## The grammar template

With `grammar` and no `cmd`, the harness runs one of two commands, with the
`grammar` and `args` bodies trimmed of surrounding whitespace:

| `mode` | Command |
|--------|---------|
| `run` | `"$TP" run "$GRAMMAR" --input "$IN" --dump-tree ARGS` |
| `check` | `"$TP" check "$GRAMMAR" ARGS` |

`$IN` is `/dev/null` when the case has no `input` section.

## Assertions

One assertion per line of the `assert` body. A line starting with `#` and a
blank line are ignored. Every assertion must hold for the case to pass, and
each one that fails is reported on its own line. An unknown directive fails.

| Directive | Holds when |
|-----------|------------|
| `exit N` | the exit status is `N` |
| `exit-not N` | the exit status is not `N` |
| `out-contains TEXT` | standard output contains `TEXT` |
| `out-absent TEXT` | standard output does not contain `TEXT` |
| `out-regex RE` | standard output matches the POSIX extended regular expression `RE`, where `^` and `$` also match at line breaks |
| `out-order A ~ B ~ C` | each `~`-separated part occurs in standard output after the previous one; a part cannot contain `~` |
| `out-count N TEXT` | standard output contains exactly `N` non-overlapping occurrences of `TEXT` |
| `err-contains TEXT` | standard error contains `TEXT` |
| `err-absent TEXT` | standard error does not contain `TEXT` |
| `err-regex RE` | standard error matches `RE`, as for `out-regex` |

`TEXT` is the rest of the line after the directive and its following spaces,
matched literally. A command killed by a signal has exit status 128 plus the
signal number.

The `stdout` and `stderr` sections compare the whole stream. Spaces, tabs,
carriage returns and line feeds at the end of the stream are ignored on both
sides; whitespace at the end of an inner line is compared.

## Environment

The command runs in the directory given by the harness's `--cwd` (the
`tinyparse/` directory under `mise run test`), with standard input, output
and error redirected to files in a temporary directory that the harness
removes afterward. The harness sets these variables for one case:

| Variable | Value |
|----------|-------|
| `IN` | the default input file, when the case has an `input` section |
| `NAME` | each named input file |
| `GRAMMAR` | the `grammar` body, when the case uses the template |

`mise run test` sets these for every case, from `test_environment` in
[`mise/lib/tinyparse.rb`](../mise/lib/tinyparse.rb), so that no case runs a
compiler or binary tool from `PATH`:

| Variable | Value |
|----------|-------|
| `TP`, `TP_CLI` | the CLI, `build/bin/tinyparse` |
| `TP_ROOT` | the `tinyparse/` directory |
| `TP_LIB` | the library archive, `build/lib/libtinyparse.a` |
| `TP_AMALGAMATION` | the amalgamation directory, `dist/amalgamation/` |
| `TP_EXAMPLES` | the built example programs, `build/examples/` |
| `TP_CC` | the pinned clang for this machine (`build/tools/cc`) |
| `TP_LINUX_CC` | zig cc for x86-64 Linux (`build/tools/x86_64-linux-cc`) |
| `TP_NM` | the pinned `llvm-nm` (`build/tools/nm`) |
| `TP_AR` | the pinned `llvm-ar` (`build/tools/ar`) |
| `TP_TAR` | the pinned GNU tar (`build/tools/tar`) |
| `TP_GZIP` | the pinned GNU gzip (`build/tools/gzip`) |
| `TP_AST_GREP` | the pinned `ast-grep` |

The cases under `dist/` read what `mise run dist`, `mise run amalgamate` and
`mise run release` wrote.

## The harness

```text
build/bin/harness [--suite DIR] [--xfail FILE] [--cwd DIR] [-v | --verbose]
```

| Option | Default | Meaning |
|--------|---------|---------|
| `--suite DIR` | `test/suite` | the directory searched, recursively, for `.test` files |
| `--xfail FILE` | `test/xfail.list` | the ledger of cases allowed to fail |
| `--cwd DIR` | `.` | the directory each command runs in |
| `-v`, `--verbose` | off | also print why a listed case failed |

The harness ignores an argument it does not recognize. It runs the cases one
at a time in name order and prints one line per case, with the reasons a case
failed under it, then a summary:

```text
PROGRESS=passing/total  PASS=passing  xfail=x  XPASS=u  FAIL=f  xfail-ledger=l
```

`passing` counts `PASS` and `XPASS` cases, and the `PASS` field repeats it,
`XPASS` cases included. `xfail-ledger` is the number of names in the
ledger. A last line starting with `RESULT:` names the outcome.

| Exit status | `RESULT` | When |
|-------------|----------|------|
| 0 | `GREEN` | every case passes and the ledger is empty |
| 1 | `RED` | at least one case failed that the ledger does not list |
| 2 | `RATCHET` | no unlisted case failed, and a listed case passed |
| 2 | `pending` | no unlisted case failed, no listed case passed, and the ledger is not empty |

`mise run test` treats any status other than 0 as a failure.

## The ledger

`test/xfail.list` names the cases allowed to fail, one name per line, with
`#` comments and blank lines ignored.

| The case | Listed in the ledger | Reported as | Effect on the exit status |
|----------|----------------------|-------------|---------------------------|
| passes | no | `PASS` | none |
| fails | no | `FAIL` | 1 |
| fails | yes | `xfail` | 2 |
| passes | yes | `XPASS`, with `(remove from xfail.list)` | 2 |

A case for a behavior not implemented yet may be listed while the work is in
progress. `mise run test` is green only when the ledger is empty, so a listed
case keeps the task red until its line is removed. A name in the ledger that
matches no case also keeps the exit status at 2.

## Example

`test/suite/core/sexpr-nested.test` parses three nested lists with the
`sexpr` example grammar and checks the order and number of nodes in the
dumped tree:

```text
<<<grammar
examples/sexpr/sexpr.scm
<<<input
(a (b (c d)) e)
<<<assert
exit 0
out-order Source ~ List ~ Atom
out-count 3 List
```
