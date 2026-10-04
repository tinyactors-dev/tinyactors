# Developing tinyparse

The contributor's guide: building from source, the test suite, the correctness
gates a change must clear, and how to run the benchmarks. For *using* the library
see [README.md](README.md); for the design see
[tinyparse-design.md](tinyparse-design.md).

## Building from source

tinyparse is plain C11, one project of the tinyactors monorepo, built by
Ruby file tasks in [mise/tasks/](mise/tasks/) with the toolchain the root
`mise.toml` pins (see the monorepo's `docs/build/`): the pinned clang 22 for
what runs on this machine (`Toolchain::Target.host`), zig cc for what is
shipped (`Toolchain::Target.release`). Nothing is taken from `PATH`.

```sh
mise install          # at the monorepo root: clang, llvm-tools, zig, ruby, ast-grep
mise run build        # build/: libtinyparse.a, the CLI, the harness, the examples
mise run dist         # dist/<target>/: libraries and CLI for x86_64-linux, aarch64-macos
mise run amalgamate   # dist/amalgamation/: the single-file distribution
mise run release      # dist/release/: the release tarballs
mise run test         # the suite over the xfail ledger, and the fast gates
mise run test --full  # the same, and every gate in ci/
mise run gate cc-diff # one gate (or several, or all when none is named)
mise run clean        # remove build/ and dist/
```

Every task compiles and links by content (the monorepo's `mise/lib/cache.rb`),
so a second run costs a few hashes. The generated headers the CLI embeds (the
manual page, the standard grammars, the library amalgamation) are written to
`build/generated/` by `mise run generate`.

## The test suite

Progress is a data-driven, ratcheting suite under [test/](test/), interpreted by
`test/harness.c` against `test/xfail.list`: done means every case passes and the
xfail ledger is empty. The plan is in [docs/build-plan.md](docs/build-plan.md).
Each example under [examples/](examples/) is pinned by a case under `test/suite/`
so its lesson — including the nested one, where the lesson is recursion — stays
green.

## Correctness gates

tinyparse runs more than one engine over a grammar, and they must agree byte
for byte. The differential gates enforce that:

- **cc-diff** (`ci/tinyparse-cc-diff.sh`, `mise run gate cc-diff`) — the generated C parser
  (`tinyparse c --parser-lib`) against the bytecode VM.
- **vm-diff** (`ci/tinyparse-vm-diff.sh`) — the bytecode VM against the
  original recursive-descent executor, which survives as the differential oracle
  and a safety opt-out (`TINYPARSE_ENGINE=recursive`).
- **diag-diff** — the generated parser's diagnostics against the VM's on a
  broken-input corpus, so error reporting may be extended but never regressed.

A change that alters the tree updates the goldens deliberately; a change that
claims to be performance-only keeps every gate green unchanged. The project
records the measured floor rather than tuning to a marker: where a generated
parser does not yet beat its target, [docs/peg-vs-glr.md](docs/peg-vs-glr.md) and
[docs/decisions.md](docs/decisions.md) say so and leave the win unrecorded until
it is earned.

## Benchmarks

All are gated behind `BENCH=1` and reproducible per the methodology in
[docs/performance.md](docs/performance.md):

```sh
BENCH=1 mise run bench bench               # JavaScript corpus: VM vs tree-sitter
REGEN=1 BENCH=1 mise run bench react-bench # React: parse + collect; builds the manifest
BENCH=1 mise run bench bench5              # React: six parsers, parse-only
```

The pinned reference clones live under [reference/](reference/) (see
[reference/PINS.txt](reference/PINS.txt)); they are gitignored and fetched on
first run.

## Running the benchmarks under Claude Code on the web

The remote/web execution environment adds friction a normal workstation does not.
These are the blockers and the workarounds, recorded so the next session does not
rediscover them.

- **The git proxy is scoped to this repo.** Outbound `git clone` of any other
  GitHub repository is rewritten to a local proxy that returns 403, so the bench
  scripts' clones of the tree-sitter runtime, `tree-sitter-javascript`,
  `tree-sitter-typescript`, React, and `typescript-go` all fail. Fetch each as a
  tarball through the egress HTTPS proxy instead — `codeload.github.com` is
  reachable:

  ```sh
  curl -sSL --cacert /root/.ccr/ca-bundle.crt -o ref.tar.gz \
    https://codeload.github.com/<owner>/<repo>/tar.gz/<commit>
  ```

  Extract it into the path the script expects under `reference/` or `bench/`, at
  the commit pinned in `reference/PINS.txt`.

- **A tarball runtime is not a git checkout.** The bench scripts record the
  tree-sitter pin with `git -C "$runtime" rev-parse HEAD`; over an extracted
  tarball that command walks up to the `ax` repo and prints the wrong commit.
  Ignore the printed pin and trust `PINS.txt` (the runtime is `d861eab2`).

- **`tsgo` cannot be built here.** The sixth bench5 arm needs a `typescript-go`
  clone (403) plus a full Go build that pulls a large module graph; it is reported
  as not-obtained rather than staged. `tsc` and `meriyah` install fine from npm.

- **`TINYPARSE_RULEMEMO=always` over React does not finish.** Forcing the rule
  memo on from the first call is impractically slow at the default iteration
  count; the published numbers use the adaptive default, which stays dormant until
  a parse goes super-linear.

- **bench5 needs the React manifest first.** Run `REGEN=1 … react-bench.sh`
  before `bench5.sh`, or bench5 aborts on a missing
  `bench/react-corpus/MANIFEST.txt`.

- **Profile the built harness directly.** The old `bench/prof/` driver is gone,
  but `bench/bench_tinyparse` carries function symbols, so run it straight under
  callgrind (see the performance doc's appendix).
