# pocketparse test suite — data, not code

The finish line for `pocketparse` is this suite, interpreted by
[`harness.c`](harness.c) and ratcheted against [`xfail.list`](xfail.list). See
[`../docs/build-plan.md`](../docs/build-plan.md).

Every behaviour `pocketparse` must exhibit is a **data file** here, never a
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

Inside `cmd`/`args`, `$PP` is the built CLI, `$IN` the default input file,
`$GRAMMAR` the grammar path, and `$NAME` each named input file. Commands run
with the working directory set to `tools/pocketparse`.

### The grammar template

When a test gives `grammar` (and no `cmd`), the harness runs the standard
invocation, so tests don't repeat the plumbing:

- `mode run` (default): `"$PP" run "$GRAMMAR" --input "$IN" --dump-tree <args>`
- `mode check`:         `"$PP" check "$GRAMMAR" <args>`

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
remove it), and **FAIL** (failed, not listed → a regression; metric goes red).

`PROGRESS=k/n` is `passing/total` and only rises. The metric exits `0` only
when `k == n` **and** `xfail.list` is empty. Landing a workstream means making
its tests pass and deleting their lines from `xfail.list`.
