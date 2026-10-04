# tinyparse test suite — data, not code

The finish line for `tinyparse` is this suite, interpreted by
[`harness.c`](harness.c) and ratcheted against [`xfail.list`](xfail.list).
`mise run test` runs it after the correctness gates; the plan it was first
built against is archived in
[`../docs/archive/build-plan.md`](../docs/archive/build-plan.md).

Every behaviour `tinyparse` must exhibit is a **data file** here, never a
hand-written C test. A test is a `*.test` file under `suite/`; its name is its
path below `suite/` without the extension (e.g. `suite/cli/help.test` →
`cli/help`).

## File format

Sections begin with a line whose first three bytes are `<<<` followed by the
section name. Bodies run until the next `<<<` line or end of file. A test
declares an **invocation** plus a list of **assertions** — and a test passes
only if *every* assertion holds.

| Section          | Meaning                                                              |
|------------------|---------------------------------------------------------------------|
| `input`          | bytes for the input, written to a temp file exposed as `$IN`         |
| `input NAME`     | a named input blob, exposed as `$NAME` (a file path)                 |
| `grammar`        | a grammar path; the harness builds the standard invocation           |
| `mode`           | `run` or `check` (default `run`) for the grammar template            |
| `args`           | extra arguments appended to the templated command                    |
| `cmd`            | an explicit shell command (overrides the grammar template)          |
| `stdin`          | bytes fed to the command's standard input                            |
| `assert`         | one assertion per line; **all** must hold (see below)                |
| `stdout`         | exact expected standard output (golden; trailing whitespace ignored) |
| `stderr`         | exact expected standard error (golden)                              |
| `end`            | optional terminator                                                  |

Inside `cmd`/`args`, `$TP` is the built CLI, `$IN` the default input file,
`$GRAMMAR` the grammar path, and `$NAME` each named input file. Commands run
with the working directory set to `tinyparse/`.

`mise run test` builds what the cases read and names it in their environment
(`mise/lib/tinyparse.rb`, `test_environment`), so a case never runs a
compiler or binutil from `PATH`:

| Variable          | What it names                                                    |
|-------------------|------------------------------------------------------------------|
| `TP`, `TP_CLI`    | the CLI, `build/bin/tinyparse`                                  |
| `TP_LIB`          | the library archive, `build/lib/libtinyparse.a`                 |
| `TP_AMALGAMATION` | the single-file distribution, `dist/amalgamation/`              |
| `TP_EXAMPLES`     | the built examples, `build/examples/`                           |
| `TP_CC`           | the pinned clang for this machine                               |
| `TP_LINUX_CC`     | zig cc for x86_64 Linux (an ELF object reads the same anywhere) |
| `TP_NM`, `TP_AR`  | the pinned `llvm-nm` and `llvm-ar`                              |

The `dist/` cases read what `mise run dist`, `amalgamate` and `release` wrote.

### The grammar template

When a test gives `grammar` (and no `cmd`), the harness runs the standard
invocation, so tests don't repeat the plumbing:

- `mode run` (default): `"$TP" run "$GRAMMAR" --input "$IN" --dump-tree <args>`
- `mode check`:         `"$TP" check "$GRAMMAR" <args>`

### Assertions

One per line in `<<<assert` (`#` comments and blank lines ignored):

| Directive               | Holds when                                              |
|-------------------------|--------------------------------------------------------|
| `exit N`                | the exit code equals `N`                                |
| `exit-not N`            | the exit code is not `N`                                |
| `out-contains TEXT`     | stdout contains `TEXT` (literal substring)             |
| `out-absent TEXT`       | stdout does not contain `TEXT`                          |
| `out-regex RE`          | stdout matches POSIX extended regex `RE`               |
| `out-order A ~ B ~ C`   | the substrings occur, in this order, in stdout         |
| `out-count N TEXT`      | stdout contains exactly `N` occurrences of `TEXT`      |
| `err-contains TEXT`     | stderr contains `TEXT`                                  |
| `err-absent TEXT`       | stderr does not contain `TEXT`                          |
| `err-regex RE`          | stderr matches POSIX extended regex `RE`               |

Every failing assertion is reported individually (run the harness with `-v` to
see the reasons for an expected failure too).

Example:

```
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

## The ratchet

`xfail.list` lists every test currently allowed to fail (one name per line,
`#` comments allowed). The harness reports **PASS** (passed, not listed),
**xfail** (failed, listed — pending), **XPASS** (passed but still listed →
remove it), and **FAIL** (failed, not listed → a regression; `mise run test` goes red).

`PROGRESS=k/n` is `passing/total` and only rises. The test task exits `0` only
when `k == n` **and** `xfail.list` is empty. Landing a workstream means making
its tests pass and deleting their lines from `xfail.list`.
