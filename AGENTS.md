# Working in this repo

**Layers.** Core decides what inputs mean; the host kernel
(`tinyactors-host`) queues, records and replays them; a language binding
(napi, the test host) owns values and their encoding; the application
owns storage and policy. Before adding code, place it in the right layer:
[README.md, "Layers and responsibilities"](README.md#layers-and-responsibilities).
Code free of any language's API belongs in the kernel, not in a binding,
and core does not handle bytes it has no use for.

- use `mise run <task> <args>` for every task,
- define tasks in ./mise/tasks/ as file tasks using `ruby` (version 4, provided by mise)

## Version control

- **Work on `main`.** This repo does not use feature branches. Do not create
  one, and do not ask to; commit directly to `main`. This overrides any
  default rule about branching before committing to the default branch.
- **Integrate frequently.** Commit each small, working increment as you reach
  it, rather than accumulating a large change and landing it at the end. A
  commit is expected to build and pass `mise run test` (see Tests below).
- Keep history linear: rebase rather than merge, and never create a merge
  commit.

## Tests: what to run

- **`mise run test` before every commit.** It is what "a commit passes"
  means. It checks acronyms and the build scripts' portability, runs the
  toolchain self-test, and builds, then runs the `test` task of each
  project whose sources changed since that task last passed, and ends with
  the time of each step, marking the projects it skipped as up to date.
  mise decides what changed: each project's `test` task lists its sources
  in `#MISE sources` (its own code and tests, and those of the projects it
  builds against), with `#MISE outputs={auto=true}` so mise remembers the
  last pass. A failed run is not remembered, so it runs again.
- **What a change reruns** follows the dependencies:

  | You changed                         | These projects' tests run                |
  |-------------------------------------|------------------------------------------|
  | `tinyactors-core` code or tests     | all of them                              |
  | `tinyxml`, `tinyactors-scxml`       | that one, the scxml codec, test host, napi |
  | `tinyactors-test`                   | test host, and napi when `dap/` or the IRP importer changed |
  | `tinyactors-napi`                   | napi, vscode                             |
  | `tinyactors-vscode`                 | vscode                                   |
  | root `mise/lib`, `mise.toml`        | all of them                              |
  | docs, READMEs, ADRs only            | none (the acronym check still runs)      |

  When you add a directory a project's tests read, add it to that
  project's `#MISE sources`, or a change there will not rerun them.
- **`mise run test --all`** runs every project's tests, changed or not.
- **`mise run test:full`** runs everything, plus what the fast pass leaves
  out:
  - the test host's exhaustive scenarios (below);
  - `scale_test` up to four million actors (the fast pass stops at
    200,000);
  - napi's `scale.js` on Bun as well as Node, and up to a million actors.

  Run it before a release, and before committing a change to core's
  allocation, pause and resume, offload and paging, or trace-writing paths,
  or to per-actor costs. Say in the commit message that it passed.
- **While iterating, run the narrowest thing that covers the change.** A
  whole suite keeps every CPU of a laptop busy for minutes; do not run one
  after each edit.
  - one core test program: `tinyactors-core/build/<name>_test` after
    `mise run build` in `tinyactors-core`; `mise run asan` there after
    changing `src/`;
  - test host scenarios: `mise run //tinyactors-test:scenarios -n
    'PATTERN'` (matches `group: name`; `--fast` skips the exhaustive ones,
    `--timings 10` lists the slowest, `--jobs N` limits the workers);
  - one napi suite: `node tinyactors-napi/test/<suite>.js`, then `bun`;
  - one project's whole suite: `mise run test` inside it.
- Do not skip, weaken or delete a check to make the fast pass faster. Make
  it cheaper, or move it to the full pass as below.

### Where a new test goes

A test is **fast** unless it is exhaustive. It is exhaustive when it

- repeats a run for every allocation it can fail, or every step it can
  pause, unload or capture at (a sweep);
- runs a long random or generated workload: populations of hundreds,
  hundreds of messages or steps; or
- takes more than about five seconds on its own (`--timings` shows it).

Mark a test host scenario, or a whole group, `exhaustive: true`:

```ruby
TinyactorsTest.scenarios 'running out of memory', exhaustive: true do ... end
scenario 'every record is written once when an allocation fails', exhaustive: true do ... end
```

Anything else that is exhaustive (a benchmark-style population, a
program-level sweep) runs in its task's `--full` mode: give the task a
`#USAGE flag "--full"`, let `mise run test:full` pass it down, and keep the
fast mode smaller, as `scale_test` does with `TAT_SCALE_MAX_POPULATION`.

When you make a test exhaustive, keep a small version of the same property
in the fast pass, as the paging scenarios do: `20 actors, at most 3
resident, 120 random messages` is fast, `100 actors, ..., 1000 random
messages` is exhaustive. The fast pass must still exercise every behaviour;
the full pass exercises it harder.

### Tests run in parallel

Test host scenarios run one host process each, in a worker per CPU
(`--jobs N` changes it). The test host's checks, napi's suites (on Node and
Bun), core's test programs and its sanitizer builds also run side by side.
So a test must not depend on another, on the order they run in, or on a
fixed port or file path: listen on port 0, write to a temporary directory.
A test that compares timings (`scale_test`, `scale.js`) runs alone; put a
new one beside them, not in a parallel list.

## The libraries are private: change APIs in place

Every caller of every library in this repo is in this repo:
`tinyactors-core`, `tinyxml`, `tinyactors-scxml`, the test host in
`tinyactors-test`, and the addon in `tinyactors-napi`. Nothing is published,
so no outside code depends on an API or ABI staying the same.

- **Change an API in place, and migrate every caller in the same change.**
  When a function needs a new parameter, a params struct or different
  semantics, change that function. Do not add a second function beside it
  (`_from`, `_ex`, `_v2`), a compatibility wrapper, a deprecated alias, or a
  "the old call is the new one with defaults" shim.
- **"So no caller breaks" is not a reason in this repo.** Don't weigh churn
  for existing callers against a cleaner API in an ADR. Find the callers
  with `git grep` and change them. One way to do each thing is worth more
  than a smaller diff.
- **Remove what nothing uses.** When a change leaves a function, type,
  field or result code unused, delete it in the same change.
- **Another session's directory.** Callers can be in a directory another
  session is working in at the same time, such as `tinyactors-napi` during
  its rewrite. Change the call sites there too, keep the edit to those
  lines, and say in your report which of its files you touched.

## The VS Code extension is a test client

`tinyactors-vscode` is experimental and internal: it exists mainly to test
the debug adapters (the test host's and tinyactors-napi's) against a real
Debug Adapter Protocol client, and to record demos. It is not published or
supported. Keep it to what that needs: do not add features for their own
sake, and change its configuration freely, like any other private API
here. Adapter behaviour belongs in the adapters and the shared DAP
transcripts, not in the extension.

## Acronyms

An acronym in code is spelled in ALL CAPITALS or all lowercase, never
Titlecase or mixedCase: `metricsToOTLP` or `otlpBytes`, never
`metricsToOtlp`. The list is `acronyms.txt` (OTLP, IO, HTTP, ID, URL, URI,
JSON, XML, SCXML, API, SDK, ...); add to it when you introduce a new one.

- **C** is snake_case, so acronyms are lowercase in names and capitals in
  macros: `otlp_writer`, `ta_session_id`, `TA_OTLP_...`.
- **JavaScript and TypeScript:** `OTLPExporter`, `toOTLP`, `sessionID`,
  `actorIDs` (the plural `s` stays lowercase). At the start of a camelCase
  name the acronym is all lowercase: `ioProcessors`, `otlpBytes`, `idPage`.
- **Ruby:** constants in capitals, as the standard library does (`JSON`,
  `URI`, `Net::HTTP`): `module API`, `RPCError`. Methods and variables are
  snake_case, so lowercase: `session_id`.
- **File names** are lowercase: `otlp.js`, `scxml_irp/`.
- Prose follows English: write "OTLP" and "I/O" in sentences.

`mise run check-acronyms` enforces this as part of `mise run test`, and
`mise run check-acronyms -- --fix` rewrites what it finds. Names we do not
own (an SDK's `traceId`, REXML's `CData`) go in `acronyms-allow.txt` with a
reason. ADRs and research notes are not checked: they are a record.

## Build scripts work the same on every host

The build runs on Linux and macOS, and a script must give the same answer
on both. Everything that differs between hosts lives in
`mise/lib/toolchain/` (see `mise/lib/toolchain.rb`). How the build works,
its design constraints, and guides for changing it are in
[`docs/build/`](docs/build/README.md):

- **`Toolchain::Tool[:name]`** runs a program pinned in the root
  `mise.toml`: zig, clang, LLVM's binutils (`llvm-nm`, `llvm-readobj`,
  ...), GNU tar and gzip. Never run `nm`, `readelf`, `otool`, `ar`, `tar`
  or a compiler found on `PATH`; pin a new tool in `mise.toml` and in
  `Tool::PINNED`.
- **The toolchain is LLVM 22** (`clang` and `conda:llvm-tools` in the root
  `mise.toml`), with `zig cc` for releases. Nothing reads `$CC`; move the
  two LLVM pins together (`mise run toolchain-test` checks they match).
- **`Toolchain::Target`** compiles and links: `Target.release(name)` (zig)
  for what is shipped, `Target.check` (the pinned clang, x86_64 Linux ELF)
  for a check that reads objects, `Target.host` (the pinned clang against
  this machine's C library) for programs that run here, and its
  `configure_environment` for an autotools build.
- **`Toolchain::Binary`** reads an object's symbols as C names (no Mach-O
  underscore) and checks itself: `must_export_exactly!`,
  `must_leave_undefined_only!`, `must_load_only!`, `must_be_for!`.
- **`Toolchain::ObjectFormat`** holds what differs between ELF, Mach-O and
  wasm, and **`Toolchain::Host`** what differs between machines. Call a
  method on them; do not branch on `macos?`, `linux?`, `wasm?` or
  `RUBY_PLATFORM`.

`mise run check-portability` enforces this with ast-grep (`sgconfig.yml`,
rules in `mise/ast-grep/rules/`, their tests in `mise/ast-grep/tests/`),
and `mise run toolchain-test` builds and reads every target's objects from
the host it runs on. Both run in `mise run test`. Write new static checks
of the build scripts as ast-grep rules, with a test, not as `grep` or a
regular expression.

## Desired API files

`tinyactors-napi/index.d.ts` is the target, not the implementation; see its
header. Never delete a declaration there because `lib/` lacks it: implement
it, or mark it `// status: unimplemented` (or `// status: partial: <what is
missing>`) on the line above it. `mise run check-api-status` checks the
markers.

## Benchmarks

Benchmark reports give throughput and memory together, name the machine
signature, and compare builds side by side (`mise run bench-compare
<commit>` in `tinyactors-napi`), never against numbers from another day or
machine. See `tinyactors-napi/DEVELOPMENT.md` (Benchmarks).

## Proposals are not instructions

`docs/proposals/` holds outlines of possible future work. They are not
instructions: do not implement anything from a proposal unless asked to.
Where a proposal disagrees with an ADR or a roadmap, the ADR or roadmap wins.
See `docs/proposals/README.md` for what each status means.

`docs/research/` holds background research (what other projects did, with
sources). Read it before repeating the same research. It is not a decision
and not an instruction.
