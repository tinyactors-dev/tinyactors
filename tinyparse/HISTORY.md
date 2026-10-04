# How pocketparse became tinyparse

tinyparse began as pocketparse, a tool inside the `ax` repository. This file
tells how it moved into the tinyactors monorepo on the branch `tinyparse`: what
each step changed, why, what the reviews caught, and what is still open. Every
number here comes from a command run during the migration, and every commit
named is on the branch (`git log main..tinyparse`).

## Where it came from

`ax` (github.com/dhamidi/ax) is a tiny embeddable actor system written as
literate C. Its owner regards it as a failed implementation of what
tinyactors is today, and tinyactors is its successor. One part of ax was worth
keeping: `tools/pocketparse`, a freestanding C parser runtime. It turns grammar
data into a streaming parser, a concrete syntax tree and diagnostics, and it
has a bytecode VM, a C code generator and a CLI.

pocketparse depended on ax in two places. Every allocation went through ax's
arena (`ax_alloc`, `ax_arena_child`, ...), and `pocketparse_open` carved an ax
actor system out of the host's memory grant so that each parse ran the VM inside
a "parser actor". The goal of the migration was a base library and tool in the
style of `tinyxml`, depending on neither ax nor tinyactors, with its full test
suite passing.

## The starting point

The source is ax commit `8fa0c2bf` (2026-07-05), `tools/pocketparse/`, plus
ax's top-level `ci/pocketparse-*.sh` scripts, `scripts/pocketparse-setup.sh`
and `.github/workflows/pocketparse.yml`.

In ax, with zig 0.15.1, `bash ci/pocketparse-metric.sh` was green:
`PROGRESS=494/494`, an empty xfail ledger. That suite (`test/suite/**/*.test`,
interpreted by `test/harness.c` against `test/xfail.list`) was the floor for the
whole migration: no case was deleted, skipped, weakened or added to the ledger.
The differential gates in `ci/` (vm-diff, cc-diff, diag-diff, the nocrash
sweeps, multiparser, nesting, bufferfriendly, import-c, reclaim,
reuse-regression, no-statics, hide-ax) counted as part of the suite.

A throwaway clone of ax at that commit ran the metric green in 1m15s on the
machine the migration used; its CLI was kept as the "before" binary for
performance comparisons.

The suite grew from 494 to 497 cases during the migration. Each new case is
named below where it was added.

## Step 1: the verbatim import

`8bf8709` copied every tracked file under `tools/pocketparse` into
`tinyparse/`, unchanged (994 files, 55,376 lines), with the `ci/` scripts under
`tinyparse/ci/` and the workflow under `tinyparse/.github/workflows/`. Nothing
built yet: the build still expected ax's `dist/libax.a`. The import is a
separate commit so every later change shows as a diff against the original.

## Step 2: removing ax

Commits `1945317` (the library), `e93cb52` (the build), `0307dc0` (tests and
gates). Names stayed `pocketparse`/`pp_` until step 3.

**The arena.** `src/` named `ax_alloc` 109 times (103 calls), `ax_alloc_raw` 9
times (8 calls) and the `ax_arena` type 74 times, and used arena open, child and
reset. The new `src/arena.c`
(`pp_pool`, `pp_chunk`, `pp_arena`) copies ax's semantics exactly, read from
ax's `ax.c`: 16-byte alignment, a 4096-byte chunk floor (or `chunk_minimum`), a
fresh block from `grow` and never a `realloc`, first-fit reuse from the pool's
free list, child arenas whose bookkeeping comes from the parent, and reset
threading chunks back onto the free list. It uses no libc and no statics.
Copying the semantics rather than designing a new allocator was deliberate:
any difference in alignment or reuse would have shown up as changed trees or
memory numbers, and the gates compare both.

**The driver state machine.** `src/vmactor.c` spawned one ax actor per parser;
`push` posted an `input.chunk` event and `finish` posted `input.end`, whose
action ran the whole VM. It was deleted. In its place is a four-state lifecycle
in plain C, `pp_parser_state` in `internal.h` (with a diagram in its banner):
`ACCEPTING`, then `DROPPED` when a push fails, then `FINISHED` or `FAILED` at
finish. One function, `pp_parser_run(parser, executor)`, makes the finishing
transition for `pocketparse_finish` (executor: the VM), every generated
parser's `<prefix>_run` (its generated `execute`), the generated standalone
`main`, and an injected sub-parse.

- **Behaviour kept:** push buffers into the parser arena; the first failed push
  is remembered; finish after a dropped chunk sets the fault "input chunk was
  dropped before finish" and returns the push's code without running the VM; a
  second finish returns OK if a tree exists; a push after finish is INVALID.
- **Decision:** the actor's "the VM ran" flag did not become a field. Being in
  `FINISHED` is the proof.
- **Rejected:** rebuilding the parse on tinyactors' actors. tinyparse must
  depend on neither runtime, and the actor carried no behaviour a state field
  cannot.
- **Deleted, since nothing used them:** the engine selectors
  `pp_engine_select_vm` and `pp_engine_select_vm_actor` and their fields;
  every one routed to the same VM. The CLI still accepts `POCKETPARSE_ENGINE`
  (now `TINYPARSE_ENGINE`) and ignores it. `POCKETPARSE_TRACE_ACTOR` (`actor:
  parse ran on ax = 1`) became `TINYPARSE_TRACE_LIFECYCLE` (`lifecycle:
  finished`).
- **Side effect:** `pocketparse_open` used to give a quarter of a grant of 4
  MiB or more (at most 16 MiB) to the ax system. The engine arena now gets the
  whole grant.

**Injection.** `inject.c` made no ax calls; only the throwaway sub-parser's
actor fields and comments changed. The sub-parser starts in `ACCEPTING`, and
both modes (a registered generated parser, or the VM) go through
`pp_parser_run`.

**Generated code and the amalgamation.** Emitted parsers call `pp_alloc` and
`pp_parser_run` and say "link with libpocketparse". The amalgamation folds only
pocketparse: no ax tangle, no hidden-visibility block, no `memcpy`/`memset`
shims. `build.sh` lost `ensure_ax`, its `ax` subcommand, libax linking and the
`.consumes-ax` marker.

**The manual page.** `build.sh` rendered `pocketparse.1.md` with ax's literate
tool `mdc` (5,526 lines of C). The rendered mdoc page was committed beside its
source instead, rendered once by ax's `mdc` at `8fa0c2bf`, and the two have been
edited together since. A renderer in the monorepo is still to do.

**Tests rewritten (count unchanged at 494).** Each rewrite asserts the property
that replaced the one that disappeared:

| Was | Now | Why |
|-----|-----|-----|
| `dist/consumes-ax` | `dist/self-contained` | asserted ax was linked; now asserts the archive leaves undefined only `memcpy`, `memset`, `memmove` and `memcmp` and defines no `ax_` symbol. Red on the old ax-linked archive, red with a stray `libax.a`. |
| `dist/amalgamation-no-ax` | `dist/amalgamation-standalone` | keeps both old checks and adds "no `ax_` identifier in the amalgamation" |
| `dist/amalgamation-hides-ax` | `dist/amalgamation-exports` | "no `ax_` exported" widened to "nothing but `tinyparse_*` and `tp_*` exported" |
| `ci/pocketparse-hide-ax.sh` | `exports` gate | the embeddable shared object exports exactly the parser entries and the public API |
| `ci/pocketparse-vm-diff.sh` | same name, new subject | it compared the VM in process with the VM inside an ax actor, which no longer exists. It now holds the VM with its rule memo off against the memo adaptive and forced on (353/353 each), and checks that `src/` calls no actor runtime. |

`vm-nocrash` runs the one VM once instead of twice, and `nesting` asserts no
internal export instead of no `ax_` export.

**Performance before and after.** Same machine, same session, the CLI's `run`
without `--dump-tree`, 10 runs each. Inputs: the 116 JavaScript inputs of the
suite concatenated 40 times (463,960 bytes) and the TypeScript inputs likewise
(502,800 bytes).

| | JavaScript best / total | TypeScript best / total | Peak RSS (TypeScript) |
|---|---|---|---|
| before (ax actor) | 0.2014 s / 2.63 s | 0.1611 s / 1.72 s | 106,472 KB |
| after (lifecycle) | 0.2022 s / 2.20 s | 0.1617 s / 1.79 s | 106,524 KB |

Best times are within 0.5%, and the `--dump-tree` output on the TypeScript
input is identical. The metric went from 1m15s to 44s, since it no longer built
ax or `mdc`.

### The first review

An adversarial review of step 2 (commits `1d007f2`, `6ea0488`) compared the
arena line by line with ax's allocator and the lifecycle with the actor.

- **A flaw carried over from ax:** `pp_align_up(n)` wrapped for `n` near
  `SIZE_MAX`, so `pp_alloc(a, SIZE_MAX)` returned a valid pointer to a zero-byte
  block. Requests above `SIZE_MAX - 16 - header` now return NULL, and the fit
  test compares `need > capacity - offset` instead of adding.
- **A divergence from the actor:** the old generated `<prefix>_run` checked
  `push_failed` on every call. The new lifecycle returned the failure only from
  `DROPPED`, so a second run on a failed parser found it in `FAILED` and parsed
  the truncated buffer. `FAILED` now behaves like `DROPPED`.
- **A deliberate change:** the generated standalone `main` used to parse even
  when its single push had failed. It now prints `error: out of memory` and
  exits 2.
- **New case `core/lifecycle`** (suite 495): a C program with 13 arena and 25
  lifecycle checks, including 2,000 parses in one fixed 4 MB grant and an 8 MB
  push into it. Against the step-2 code it reported 4 failures. It also passes
  under AddressSanitizer and UBSan.
- Removing libax had left an argument-less `[ -f ]` in 12 `ci/` scripts; it
  always tested the same way. Removed. `emit/c-bundle` and three `binary/pack-*`
  cases wrote fixed `/tmp/pp_*` paths (two shared one); they now use `mktemp`.
- The generated `--parser` output was compared with the old CLI's for sexpr,
  std.json and std.javascript: module and bundle output is byte-identical, and
  the parser output differs only in the run and main bodies (5 and 22 lines).

Metric after the review, from a clean tree: `PROGRESS=495/495`, 35s.

## Step 3: the rename

`fe6dbe7`: one word-boundary-aware, case-sensitive pass over all 997 tracked
files. `pocketparse` became `tinyparse` (3,952 occurrences in 199 files), and
every token starting with `pp`/`PP` at a word boundary became `tp`/`TP`. Before
choosing that rule, every such token in the tree was listed: all came from
pocketparse (`pp_*`, `PP_*`, the harness variable `PP`, the `.ppg` extension,
log tags, bench variables). The word boundary left `cpp_` and `app_` alone, and
case sensitivity kept the roff macro `.Pp`. No `tp_` symbol existed before. 34
files were renamed with `git mv`; 648 files changed.

- The packed-grammar format followed the name: `.ppg`/`PPG1` became
  `.tpg`/`TPG1`. The magic is written as character literals, so `src/pack.c` and
  the CLI's sniff were changed by hand. An image packed by pocketparse is now
  refused; none is committed anywhere.
- Grammar module names (`std.javascript`, ...) did not change. The "pocket
  knife" metaphor stayed in the prose.
- One surprise: the built CLI still contained the string
  `/home/user/ax/tools/pocketparse`. It came from zig 0.15.1's cached libc start
  objects, compiled once when zig first ran from ax. Step 4 strips that debug
  information from what is shipped.

The suite was unchanged apart from the names: `PROGRESS=495/495`, 58s.

## Step 4: the toolchain

Commits `ebaf53e` (warnings) and `40899a5` (the build). tinyparse became a
monorepo project like `tinyxml`: it is listed in the root `mise.toml`
`config_roots`, the root `mise run test` runs it last, and it uses the root's
pins (clang 22.1.8, llvm-tools 22.1.8, zig 0.16.0) instead of its own zig
0.15.1. Every task is a Ruby file task in `mise/tasks/`, sharing
`mise/lib/tinyparse.rb`: `generate`, `lib`, `cli`, `harness`, `examples`,
`build`, `amalgamate`, `dist`, `release`, `test`, `man`, `clean`. The
`build.sh`, `amalgamate.sh`, `release.sh` and nine example build scripts are
gone, along with the GitHub workflow (GitHub reads only the repository root's
`.github/`, and the monorepo has no workflows).

- **Flags.** Everything compiles with `-std=c11 -O2 -Wall -Wextra -pedantic
  -Werror`; the library adds `-ffreestanding -fno-builtin`. The old build was
  `-std=c99` without `-pedantic` or `-Werror`, but `internal.h` uses an
  anonymous union, so C11 is the honest standard.
- **New warnings fixed in code** (`ebaf53e`): four string literals over ISO's
  4,095-byte limit (each split in two), and generated parsers that tested the
  address of a local (`if (&children)`), which clang 22 warns about. The emitter
  now tests a sink for NULL only when it is not a local; other output is
  byte-identical.
- **`TINYPARSE_API`** marks all 48 public declarations, empty by default. `dist`
  defines it as default visibility and compiles everything else hidden, so the
  shared library exports exactly the API, checked with `Toolchain::Binary`. The
  API list comes from the pinned clang's JSON AST of the header: the monorepo's
  ast-grep C parser missed `tinyparse_finish`, whose declaration is ambiguous
  without typedef information.
- **Targets** follow the monorepo's names: `x86_64-linux` and `aarch64-macos`
  (they were `linux-x86_64` and `macos-arm64`). The Linux CLI is a static musl
  binary. The shared library is `libtinyparse.so.0.1` plus a `.so` link, or
  `libtinyparse.0.1.dylib`.
- **Debug info.** What is shipped is compiled with `-g0`, and the CLI gets
  `llvm-objcopy --strip-debug`. `-Wl,-S` was tried first and dropped: zig's
  linker stripped the static CLI's whole symbol table, which would have made the
  "no `ax_` symbol" check vacuous.
- **Tools in tests** come from the environment the test task sets: `TP` (the
  CLI), `TP_LIB`, `TP_AMALGAMATION`, `TP_CC` (the pinned clang), `TP_NM`,
  `TP_AR`, and so on. No case runs a compiler or `nm` from `PATH`.
- **Parallel compiles** cut a cold `dist` from 50s to 15s.
- **Kept:** `install.sh`, an end-user installer rather than a build script; only
  its platform names changed.

Tests rewritten (count unchanged at 495): `dist/*` cases read the new
`dist/<target>/{bin,lib,include}` layout and take the tarballs from the
`release` task instead of running `release.sh`. `dist/ci-workflow` checked a
workflow that no longer exists; it became `dist/ci-wiring`, which asserts the
equivalent wiring (the root lists tinyparse, the root test task runs it, the
test task declares its sources and depends on `release`). `examples/uri-reject`
passed when its grammar file was missing, since it exited 1 either way; it now
also requires the "malformed URI" message.

Timings, before and after: `build.sh dist` took 52.2s with an empty zig cache;
from `mise run clean`, `build` takes 12.2s and `dist` 13.9s, and a second run
with nothing changed takes 0.8s and 0.4s. `mise run test` took 16.3s and
`test --full` 39.9s.

### The second review

An adversarial review of steps 3 and 4 (commit `a802dea`) exported every test
and gate at the import commit, applied the rename mechanically, and diffed
against the branch, so only non-mechanical changes showed: 55 files.

- **`emit/c-bundle` had been weakened.** Commit `0307dc0` dropped its
  `out-contains List` and `out-contains Atom` checks while removing `libax.a`
  from its link line, so any run that exited 0 passed. Restored.
- **The amalgamation did not compile under `-Wall -Wextra -Werror`:** two
  `static inline` helpers that only generated parsers use. They now sit behind
  `#ifndef TINYPARSE_AMALGAMATION`. New case `dist/amalgamation-warnings`
  (suite 496) compiles `tinyparse.c` and `tinyparse_cli.c` with the pinned clang
  under `-std=c11 -pedantic -Werror`; it fails without the guard.
- **A race:** `examples/ebnf/run.sh` rewrote the committed `examples/uri/uri.scm`
  through `tee` while vm-diff, now running beside the suite, read it. It writes
  a temporary file and renames it into place only when the bytes differ.
- `tar` came from `PATH` in one case; it now uses the pinned GNU tar and gzip.
  The harness's four em dashes in printf strings made the build depend on a
  UTF-8 locale; they are `--` now. The shared library and CLI gained
  `must_load_only!` checks, and the macOS ad-hoc code signatures were verified by
  hand after `--strip-debug` (340 of 340 and 21 of 21 pages match).

`mise run test --full` after the review: `PROGRESS=496/496`, every gate green,
40.6s.

## Step 5: the gates

Commits `085c026`, `54028c7`, `546ae44`. The 13 standing gates in `ci/` became
Ruby file tasks in `mise/tasks/gate/`, sharing `mise/lib/gate.rb`, and
`mise run test` depends on `gate:*`, so mise runs them side by side before the
suite. Each gate works in its own temporary directory. In ax, vm-diff wrote
files next to the committed suite inputs and bufferfriendly used fixed `/tmp`
paths.

Each gate has a fast form. The six that also have an exhaustive form declare
`--full`, which `mise run test --full` and the root's `mise run test:full` run:

| Gate | Fast | `--full` |
|------|------|----------|
| vm-diff | json family, examples, fixtures, 2 inputs per corpus extension: 73/73 per memo mode | every corpus input: 353/353 per mode, as in ax |
| cc-diff | 7 generated parsers: 55/55 | 10 parsers over every JavaScript, TypeScript, TSX and shell input: 340/340, as in ax |
| exports | the embedded artifact at `-O0` | at `-O2` (16s) |
| nesting | the shared object at `-O0` | at `-O2` |
| reclaim | 12 parses | 300 parses |
| reuse-regression | generated std.json parser, 40 parses of 958 KB | generated std.tsx parser, 120 parses, as in ax |

What changed in each, and why:

- **vm-diff** runs the memo-off reference once and compares both candidates to
  it in parallel: the full pass went from 21.9s to 4.3s. Its timing loop
  measured time inside a parallel gate and asserted nothing, so it became
  `mise run bench:rule-memo`. The source check now rejects tinyactors' `ta_` and
  `tah_` prefixes as well as `ax_`.
- **cc-diff** generates and compiles its parsers in parallel: 22s to 8s.
- **exports** (was hide-ax) became exact: the shared object must export the 48
  API functions plus the parser entries, nothing more and nothing missing. That
  found `tinyparse_apply_manifest`, a CLI helper with the public prefix that
  leaked into every embedded artifact; it is `tp_apply_manifest` now
  (`085c026`). ax's third step linked a "hardened" executable behind a GNU ld
  version script and read it with `nm -D`, which only works on Linux; it became
  a driver linked against the restricted library and run on the sample.
- **reclaim** had a fixed 1 GiB ceiling for 300 parses. One parse peaks at
  108,520 KB and 12 or 300 parses at 142,568 KB, so the gate now measures one
  parse and requires N parses to stay under twice that (217,040 KB).
- **reuse-regression** moved its C harness out of a bash heredoc into
  `test/gates/reuse.c`, and now also fails if the engine never grew past its
  8 MiB grant (it grows 2,051 times on the json input), so a too-small input
  cannot make it pass.
- **no-statics** moved its ast-grep rule to the root's
  `mise/ast-grep/rules/tinyparse-no-static-mutable.yml`, with a test, and added
  a second check: `libtinyparse.a` must define no writable data symbol. The rule
  cannot see a non-static global or a `static const char *names[]`; the object
  check catches both.
- **bufferfriendly, multiparser, import-c, parser-nocrash, vm-nocrash** were
  ported as they were.

The seven benchmark and coverage scripts became opt-in `bench:*` tasks over bash
scripts moved to `bench/` (`git mv`, so their history follows), never part of
`test`. They stayed bash because most need the network and pinned references,
and a rewrite that could not be run would be worse than a move. A new
`bench:fetch` downloads the references in `reference/PINS.txt` as tarballs. Run
here: `bench:javascript-coverage` reported `JS_COVERAGE=115/116`. The one
failure fails the same way with ax's own build, so it is not a regression.
`bench:typescript-coverage` reported `TS_COVERAGE=112/112`. `bench:javascript`
measured a 3.774 ms median parse against tree-sitter's 2.784 ms (1.36x) and a
walk of 19.19 against 154.16 ns per node, on an Intel Xeon 2.10GHz with 4
cores. `bench:react`, `bench:parsers`, `bench:tsc` and `bench:perf-gate` were
not run.

Of the 27 scripts the import put under `ci/`, 13 became gates, 7 became
benchmarks, and 7 were deleted: `metric.sh` (the `test` task runs the same
harness over the same ledger), `setup.sh` (mise provisions the toolchain), and
the phase 14 to 17 and explore metrics (the finish lines of ax-era plans: each
re-ran the metric and grepped the docs).

`546ae44` added `tinyparse/AGENTS.md`, modelled on tinyxml's: the hard
constraints, each tied to the gate that checks it. The root `AGENTS.md` and
`README.md` list tinyparse. No `CHANGELOG.md` was written: no component has one
yet, and the repository's rule is that the release-changelog skill writes it at
release time.

`mise run test` after step 5: `PROGRESS=496/496`, 38.9s from a changed tree;
`--full` 45.8s.

### The third review: gates that could not fail

A review of step 5 broke the code on purpose in each way a gate exists to
catch, ran the gate, and restored the file. Five gates stayed green under such
a bug:

| Gate | Sabotage | Before | Fix |
|------|----------|--------|-----|
| vm-diff | corrupt a replayed memo subtree; drop the lexical goal from the memo key | green, even `--full` | `19b3dfb` |
| bufferfriendly | commit an item that reached the end of an incomplete buffer | green | `3e9514e` |
| nesting | ignore registered parsers and fall back to the VM | green | `9c94f62` |
| parser-nocrash | double `TP_PARSER_DEPTH_LIMIT` | green | `88b1f3b` |
| no-statics | a generated parser with a mutable static | green | `88b1f3b` |

- **vm-diff** (`19b3dfb`). Instrumenting the VM showed that across all 238
  corpus inputs the forced memo hit 407 cached failures and 31 "seen" marks but
  replayed no cached success, and the adaptive memo never woke. So the adaptive
  mode was identical to memo-off on every input. A new fixture,
  `test/gates/backtrack.scm`, makes six replays happen, tries one rule under two
  lexical goals, and wakes the adaptive memo with an exponential line (about
  89,000 rule calls, 0.03s). Two trace events in `src/vm.c`, `rule.replay` and
  `rule.memo-on`, are emitted only when a trace plug is installed; the gate
  fails if either never fires. vm-diff is now 74/74 fast and 354/354 per mode
  with `--full`.
- **bufferfriendly** (`3e9514e`). Its only input held lists, which cannot grow
  once closed. A second input of top-level atoms fails at 6 of 8 chunk sizes
  under the sabotage; 16 comparisons now instead of 8.
- **nesting** (`9c94f62`). If no generated parser is registered, injection
  falls back to the VM, which builds the same tree. `examples/nesting/nesting.c`
  now counts the regions each registered parser parsed, and the gate requires
  both counts to be positive.
- **parser-nocrash and no-statics** (`88b1f3b`). parser-nocrash ran only
  std.json, whose stack frames are tiny; the fast pass adds std.shell, and a
  new `--full` form (the seventh gate with one) adds std.javascript and std.tsx. Its header no longer claims the
  generated parser is iterative: it recurses, under a depth limit. no-statics
  never looked at generated code; it now compiles the std.json, std.fence and
  std.shell parser libraries and requires no writable data in them.

The review also found a CLI bug (`e37da9b`): `tinyparse run` exited 0, with no
tree and nothing on stderr, when a parse ran out of memory, which made every
exit-code gate blind to it. It now prints `error: out of memory` and exits 1.
New case `cli/run-out-of-memory` (suite 497) exited 0 before the fix.

The other gates went red under their sabotage as written: cc-diff, diag-diff,
import-c, multiparser, exports, vm-nocrash, reclaim (1,273,320 KB against the
217,040 KB ceiling) and reuse-regression, which reproduces the old one-node-tree
bug.

After the review: `PROGRESS=497/497`, `mise run test` about 20s, `--full` 56s.

## Step 6: the documentation

Commits `fbb3b57` and `14a1b1a`, and this file (`2b6cc98`).

- **Archived.** `ax-execution.md`, `ax-topologies.md`, `bytecode-vm.md`,
  `codegen-transform.md`, `build-plan.md` and `explore-plan.md` moved to
  `docs/archive/`, with a README saying they are the pre-migration record. Their
  links were adjusted; links into ax became plain text naming the ax path.
- **Rewritten from the code:** `docs/architecture.md`, `docs/cli.md` and
  `docs/distribution.md`. `cli.md` had listed `pack` and `c` as not yet
  implemented, and claimed exit code 2 for every usage error; only an unknown
  command exits 2.
- **Corrected in `tinyparse-design.md`:** the "Actor model" section is now
  "Parser lifecycle". Two claims were wrong before the migration too: bundle
  mode was said to fold in the engine with no `libtinyparse`, but it links
  against it, and `init` was said to write a `README.md`, which it does not.
- **`README.md`** no longer calls the engine "powered by ax" or says the
  amalgamation folds ax in. It names the eight embedded grammars, dates its
  performance numbers, and states the TypeScript blow-up below.
- **`docs/grammar.md`'s "Deferred to Phase 4"** listed lexical goals, recovery,
  bindings and imports, which had all shipped. It now points each form at its
  design section and example. Binary grammars are still unimplemented.
- **The manual page** (both `tinyparse.1.md` and the committed `tinyparse.1`)
  gained `pack`, `c`, `--chunk`, `--trace` and the exit codes.
- `decisions.md` and `peg-vs-glr.md` are records and stay as written. Each
  gained a note mapping the old names to today's, and `decisions.md` a section
  for the migration's decisions.

Every relative link in tinyparse's markdown resolves; a script checked them
all, anchors included.

## The commits

| Commit | Step | Subject |
|--------|------|---------|
| `8bf8709` | 1 | import pocketparse verbatim from ax |
| `1945317` | 2 | own arena and parser lifecycle replace ax |
| `e93cb52` | 2 | build, amalgamate and ship without ax |
| `0307dc0` | 2 | run every gate from tinyparse/ and assert no ax |
| `1d007f2` | 2 (review) | refuse oversized allocations and keep a failed parser failed |
| `6ea0488` | 2 (review) | drop dead file checks and fixed /tmp paths |
| `fe6dbe7` | 3 | rename pocketparse to tinyparse, pp_ to tp_ |
| `ebaf53e` | 4 | compile clean under clang 22 with -pedantic -Werror |
| `40899a5` | 4 | build with mise file tasks and the monorepo toolchain |
| `a802dea` | 4 (review) | restore weakened checks and close gaps the review found |
| `085c026` | 5 | keep the CLI's manifest helper out of the public prefix |
| `54028c7` | 5 | port the ci/ gates to Ruby file tasks |
| `546ae44` | 5 | an AGENTS.md like tinyxml's |
| `19b3dfb` | 5 (review) | make gate:vm-diff reach the memo's replay and wake paths |
| `3e9514e` | 5 (review) | feed gate:bufferfriendly top-level atoms |
| `9c94f62` | 5 (review) | make gate:nesting see the generated parsers ran |
| `e37da9b` | 5 (review) | `tinyparse run` fails when the parse runs out of memory |
| `88b1f3b` | 5 (review) | hold the generated parsers to the crash bound and no-statics |
| `fbb3b57` | 6 | move the ax-era design documents to docs/archive/ |
| `14a1b1a` | 6 | make the user docs describe tinyparse as it is now |
| `2b6cc98` | 6 | HISTORY.md, the story of the move out of ax |

A final end-to-end check followed (see "The final check" below).

Outside `tinyparse/`, the branch touches only the monorepo's wiring: the root
`mise.toml` (`config_roots`), `mise/tasks/test/_default` (the tinyparse step),
`AGENTS.md` and `README.md` (one row and one line), `acronyms-allow.txt` (three
names tinyparse does not own: callgrind's instructions-read event, and two
mixed-case identifiers in a JSX input copied from tree-sitter-javascript), and the no-statics ast-grep rule
and its test under `mise/ast-grep/`.

## What the reviews caught

Three adversarial reviews ran, one after each of steps 2, 4 and 5, and each
found something the step's own tests had passed:

- a test weakened while removing ax (`emit/c-bundle` lost its assertions);
- a behaviour that differed from the actor (a second run on a failed parser);
- an arena flaw inherited from ax (`SIZE_MAX` wrapping);
- an amalgamation that did not compile under `-Werror`;
- a race between a suite case and a gate over a committed file;
- a public-prefix function leaking into every embedded artifact;
- five gates that stayed green under a bug they should catch;
- a CLI that reported success when a parse ran out of memory.

Every fix came with a test that fails without it.

## What is not done

- **TypeScript blow-up.** Fixed after the migration; see "After the migration"
  below.
- **Linux-only tests.** Several `dist/` cases run the x86_64 Linux CLI or read
  ELF symbol tables, and `cli/run-out-of-memory` relies on `ulimit -v`, which
  Linux enforces. The macOS artifacts are cross-built and symbol-checked, but
  nothing runs them. `examples/embed/embed-lib.sh`, a recipe for users, still
  picks its linker flag by `uname`.
- **Release publishing.** `mise run release` builds deterministic tarballs and
  stops. `install.sh` expects tags named `tinyparse-v<version>`, which does not
  match the monorepo's `<component>/v<version>`; tinyparse is not in
  `Changelog::COMPONENTS`, and the changelog tooling cannot read
  `TINYPARSE_API` declarations with ast-grep (an API description like tinyxml's
  `mise run api` would be needed).
- **The manual page renderer.** `tinyparse.1` is committed rendered and edited
  by hand beside `tinyparse.1.md`.
- **Generated-code warnings.** `--parser` and `--parser-lib` output has the same
  unused-function and unused-label warnings under gcc `-Wall -Wextra` as before
  the migration (8, 3 and 56 for sexpr, std.json and std.javascript; clang 22
  gives 27 for std.javascript). The examples and gates compile generated
  parsers without warning flags for that reason.
- **Benchmarks not run here:** `bench:react`, `bench:parsers`, `bench:tsc` and
  `bench:perf-gate`; they need the React source, npm packages and a Go build.
- **The root `mise run test`** stops at the host kernel's sanitizer task
  (UBSan: a NULL pointer passed to `memcpy` in
  `tinyactors-host/tests/recorder_test.c:158`). The branch changes nothing under
  `tinyactors-host`, so this fails on `main` too. The other projects, tinyparse
  included, pass when run one by one. The core's amalgamate step and the shared
  `mise/lib/cache.rb` also need a UTF-8 locale (`LANG=C.UTF-8`); tinyparse's own
  build no longer does. So does the root `mise/tasks/test/_default`: under the
  POSIX locale it raises `invalid byte sequence in US-ASCII` reading a step's
  output when that output is not ASCII (check-acronyms prints `→` when it
  finds something), instead of reporting the failed step.

## The final check

From `git clean -xfd tinyparse`, with `LANG=C.UTF-8`:

- the root `mise run test` passed acronyms, changelog, portability and build,
  then stopped at the host kernel's sanitizer failure above (490s);
- `tinyactors-test` and `tinyactors-napi`, forced one by one, passed (the test
  host: `449 scenarios: 449 passed`);
- `cd tinyparse && mise run test`: `PROGRESS=497/497  PASS=497  xfail=0  XPASS=0
  FAIL=0  xfail-ledger=0`, `test: GREEN` (suite 18.9s);
- `mise run test --full`: GREEN in 53s, with `RESULT=340/340` (cc-diff) and
  `RESULT[memo-adaptive]=354/354  RESULT[memo-always]=354/354` (vm-diff).

The check compared the suite with the import commit: every one of its 494 cases
is still there under the new name, except the three `dist/` cases step 2
rewrote and the one step 4 rewrote (`dist/ci-wiring`), and three new cases
(`core/lifecycle`, `dist/amalgamation-warnings`, `cli/run-out-of-memory`) bring
it to 497. Of the
mechanical-rename diffs, 16 files differ in more than names, each a change
recorded above (temporary directories, the new `dist/` layout, `$TP_CC` instead
of `cc`, the restored and added assertions). It also found this file
contradicting the acronym check (it spelled the allowed names out), and a
current-state sentence in `docs/performance.md` that still had the benchmark
link `libax.a`; both were fixed.

## After the migration: the TypeScript blow-up

The first gap left open was std.typescript and std.tsx taking exponential time
on unclosed `(`. Measured on the branch as left by the final check: 18 took
0.29s, 22 took 3.4s, 26 took 57s in the VM (TSX: 41s), and doubling held with
the rule memo forced on (22: 9.0s) or off (22: 20s). The generated parsers,
which carry no memo, took 4.3s (TypeScript) and 5.2s (TSX) on 22. std.javascript
rejected the same input in 0.02s at every size. Two separate faults combined.

**The grammar parsed every parenthesized expression twice.** std.typescript
overrode `parenthesized_expression` as `"(" (choice (seq expression (optional
type)) expression_sequence) ")"`. When the first arm failed, the second parsed
the same expression again from the same offset, so each unclosed `(` doubled
the work beneath it. The same rule had a second bug: the first arm commits on
`(a`, its `)` then fails on a comma, and the choice is never retried, so
`x = (a, b);` was a syntax error in TypeScript while JavaScript parsed it. The
rule is now left-factored to `"(" expression_sequence (optional (field type:
type_annotation)) ")"`. expression_sequence is hidden and collapses to a single
child, so `(a)` and `(a: T)` keep their trees; the grammar now also admits
`(a, b: T)`, which TypeScript rejects. This fix alone makes both grammars
linear in the VM with the memo off and in the generated parsers: 200 unclosed
`(` take 0.02s in the VM, and 26 take 8ms in the generated parsers.

**The VM's rule memo stopped recording.** The memo is the VM's backstop against
exactly this kind of grammar, yet forcing it on did not help. Its hash table is
sized from the input length (`2n + 64` slots, rounded up to a power of two) and
refused new keys past three-quarters full. But a key is a rule program, an
offset, a lexical goal, a field and an alias, and std.typescript has about 1,400
programs, so a short input filled the table almost at once. A gdb trace of 8
unclosed `(` showed the same key (rule, offset, goal, field, alias) failing 32
times, each time memoable and each time unrecorded. `tp_rule_memo_store` now
doubles the table (`tp_rule_memo_grow`, `src/vm.c`) and re-inserts the live
keys when it reaches three-quarters full; the old arrays stay in the parser
arena, so the memo costs at most twice its final table, and when the arena
cannot supply a larger table the old behaviour (stop recording) remains. With
this fix alone, and the old grammar, 2,000 unclosed `(` took 0.29s.

Three suite cases hold the fixes, and each was checked to fail without its fix:

- `tsfamily/unclosed-parens-linear`: 3,000 unclosed `(` through both grammars,
  with the memo off, adaptive and forced on, under a 20-second CPU limit; each
  run must end in a syntax error. It fails on the old grammar.
- `tsfamily/parenthesized-sequence`: `(a, b)`, `(a)` and `(document: any)` parse.
  It fails on the old grammar.
- `core/rule-memo-grows`: a nine-rule grammar that tries every rule twice per
  `(` on 80 unclosed `(`, with the memo forced on, under a 20-second CPU limit.
  It fails (killed at the limit) on the old VM.

`gate:parser-nocrash --full` now feeds the generated TSX parser 200,000
unclosed `(` instead of `[`; it had used `[` because `(` did not finish. The
suite is 500/500.

## Next steps

**Its own repository under the tinyactors-dev organization.** tinyparse
depends on no code of the monorepo, but its build does: the tasks use the
monorepo's `mise/lib` (`Toolchain`, `Cache`, `CRelease`), the root `mise.toml`
pins, and the no-statics ast-grep rule. A standalone repository needs those
copied in (or published as a shared build library) and its own `mise.toml` with
the pins. The `dist/ci-wiring` case, which checks the monorepo wiring, would be
rewritten to check the new repository's.

The history can come along in two parts:

1. **The migration** (this branch). On a fresh clone of tinyactors,
   `git filter-repo --subdirectory-filter tinyparse` on the `tinyparse` branch
   keeps the commits that touched `tinyparse/`, with `tinyparse/` as the new
   root. The root-wiring changes drop out, as they should. Commit hashes change,
   so this file's commit table would need the new hashes or a note.
2. **Its history in ax.** On a full (not shallow) clone of ax,
   `git filter-repo --path tools/pocketparse/ --path-rename tools/pocketparse/:`
   gives pocketparse's own history with the same root. The ax top-level scripts
   (`ci/pocketparse-*.sh`, the setup script, the workflow) can be kept with
   further `--path` options if wanted. The import commit `8bf8709` is a
   verbatim copy of ax's tree at `8fa0c2bf`, so the migration's history can be
   rebased onto the filtered ax tip (`git rebase --onto <ax-tip> 8bf8709`), or
   the two joined with `git replace --graft` and made permanent with another
   filter-repo pass. Either way the import's diff (the `ci/` scripts appearing)
   remains the only seam.

**Then:** wire CI in the new repository (`mise run test`, and
`mise run test --full` on a schedule), decide the tag scheme and publish a
release, add a time-and-memory gate for pathological input before fixing the
TypeScript blow-up (the `left-factor` skill describes the usual cure), and run
the remaining benchmarks side by side with ax's build.
