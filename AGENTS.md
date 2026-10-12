# Working in this repo

**Layers.** Core decides what inputs mean and runs actors in one block of
memory; the host layer runs many systems as one; a language binding
owns values and their encoding; the application owns storage and policy. Before adding code, place it in the
right layer:
[README.md, "Layers and responsibilities"](README.md#layers-and-responsibilities).
Code free of any language's API belongs in a C layer, not in a binding,
and core does not handle bytes it has no use for. `tinyactors-core/AGENTS.md`
has the rules of the C layers themselves (freestanding layers, no pointers
in system memory, no allocation after init).

**Bindings expose the core, nothing else.** The bindings are built in
[ADR 0009](docs/adr/0009-one-host-two-queues.md)'s step 8 from
`tinyactors-core/docs/bindings.md`, in `tinyactors-napi` and
`tinyactors-cpython`, each with an `AGENTS.md` of its own
([ROADMAP.md](ROADMAP.md), step 8). Before
designing a binding's API: inventory the public headers, write the table
mapping each API item to the core functions behind it, borrow names and
call shapes from other libraries (xstate) but never their semantics, and
invent nothing. The core
is a complete SCXML implementation: read `src/core`, `src/system` and
`docs/SCXML.md` before claiming it lacks something.

**Roles.** Before designing a feature or a change, take the perspectives
in [docs/roles.md](docs/roles.md) into account: the application developer,
the operator, the capacity planner, the C embedder, the binding author,
the maintainers and the observability backend. Say whom the change serves
first, what already answers their questions, who pays for it (per event,
per actor, in memory, code size, API surface, work in every binding), and
what is out of scope. A proposal or ADR states this near its start.

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

## Releases and changelogs

Each component (core, napi, cpython, tinyxml) has a `CHANGELOG.md` in
its directory (`tinyactors-napi`, `tinyactors-cpython` for the bindings), and a release of it needs a section there that passes
`mise run changelog:check <component>`
([ADR 0002](docs/adr/0002-changelogs.md)). To prepare a release, use the
**`release-changelog` skill**: it reads the evidence
(`mise run changelog:draft <component>`), writes the summary and the
upgrading text, has them reviewed, and stops for the maintainer's approval.
Do it at release time, not between releases.

- **Commit subjects and scopes are the input.** Scope a commit with the
  component it changes (`feat(core)`, `fix(napi)`, `fix(cpython)`,
  `feat(core,napi)`); a
  breaking change has `!` (`refactor(core,napi)!:`), and its body says what a
  caller does. A subject that says only an ADR number gives the changelog
  nothing to summarize.
- **Do not edit a changelog below its `<!-- Generated ... -->` line**, and do
  not commit one by hand: `mise run changelog:write` writes it.

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
  | `tinyactors-core` (code, bindings, tests, tools) | core, napi, cpython (each binding builds core's amalgamation and lints with its rules) |
  | `tinyactors-test` (`lib`, `suites`, `mise`, `mise.toml`) | tinyactors-test, core, napi, cpython (each runs the suite against its implementation) |
  | `tinyactors-test/lint` (the adapters' lint) | tinyactors-test, napi, cpython       |
  | `tinyactors-test/test` (the runner's own tests) | tinyactors-test                  |
  | `tinyactors-napi`                   | napi                                     |
  | `tinyactors-cpython`                | cpython                                  |
  | `tinyxml`                           | tinyxml                                  |
  | `tinyparse`                         | tinyparse                                |
  | `staticjs`                          | staticjs                                 |
  | root `mise/lib`, `mise.toml`        | all of them                              |
  | docs, READMEs, ADRs only            | none (the acronym check still runs)      |

  When you add a directory a project's tests read, add it to that
  project's `#MISE sources`, or a change there will not rerun them.
- **`mise run test --all`** runs every project's tests, changed or not.
- **`mise run test:full`** runs everything, plus what the fast pass leaves
  out:
  - every torture chamber scenario (the fast pass runs one);
  - the cross builds for each release target (`mise run check:cross`);
  - tinyparse's exhaustive gates: every corpus input through the VM and
    the generated parsers, and the larger reuse and reclaim workloads.

  Run it before a release, and before committing a change to core's
  memory layout, scheduling, hibernation or migration, or to per-actor
  costs. Say in the commit message that it passed.
- **While iterating, run the narrowest thing that covers the change.** A
  whole suite keeps every CPU of a laptop busy for minutes; do not run one
  after each edit.
  - one core test program: `tinyactors-core/build/dev/bin/<name>` after
    `mise run build` in `tinyactors-core` (the sanitizer build);
  - one part of core's suite, as a task in `tinyactors-core`: `conform`
    (the conformance suite against `ta-conform`), `wasm:test`, `torture:test`, `check:wasm`, `check:freestanding`
    (`mise tasks` lists them);
  - one tinyparse gate: `mise run //tinyparse:gate:cc-diff` (`--full` for
    its exhaustive version);
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

An exhaustive test runs in its task's `--full` mode: give the task a
`#USAGE flag "--full"`, let `mise run test:full` pass it down, and keep the
fast mode smaller, as core's `test` does with the torture chamber (one
scenario in the fast pass, every scenario in the full one).

When you make a test exhaustive, keep a small version of the same property
in the fast pass. The fast pass must still exercise every behaviour; the
full pass exercises it harder.

### Tests run in parallel

Core's test programs run side by side, one per CPU, each from its own
directory under `build/dev/scratch/`, and node's test runner runs the
binding suites in parallel. So a test must not depend on another, on the
order they run in, or on a fixed port or file path: listen on port 0,
write to a temporary directory. A test that compares timings runs alone,
in a task of its own, not in a parallel list.

## The libraries are private: change APIs in place

Every caller of every library in this repo is in this repo: the C layers
of `tinyactors-core`, its tools, `tinyxml`, `tinyparse` and `staticjs`.
Nothing is published, so no outside code depends on an API or ABI staying
the same.

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
  session is working in at the same time. Change the call sites there too,
  keep the edit to those lines, and say in your report which of its files
  you touched.

## At most three parameters

A function in `tinyactors-core`'s public headers takes at most three
parameters, and so does a callback they declare (a function pointer: a
datamodel vtable entry, a typedef). Group the rest in a struct, as
`ta_cbor_document`, `ta_buffer`, `ta_datamodel_call` and the host's request
calls do
([ADR 0009](docs/adr/0009-one-host-two-queues.md)). When a function needs
a fourth, change it in place to take a struct. Bytes cross the public API,
callbacks included, as `ta_bytes` (input) or `ta_buffer *` (output), never
as a pointer and a length.

The rule holds for every C function we write from now on, not only the
public headers: internal functions, static helpers, and all of a
binding's C. A function whose signature a platform's typedef fixes
(CPython's `PyCMethod`, a `METH_FASTCALL | METH_KEYWORDS` method, a
Node-API callback) is exempt by that typedef. It is a rule for C, not for
the API a binding gives its language.

`mise run lint` in `tinyactors-core` enforces both on the public headers,
with the ast-grep rules `at-most-three-parameters` and
`buffers-are-ta-buffer` (`tinyactors-core/tools/lint/rules/`); a binding's
lint enforces them on all of its C. Core's existing internal functions
predate the wider rule: change one to follow it when you change it.

## No serialization on the hot path

The calls made per event take and give C structs: spawn, send, send_many,
terminate, deliver, advance_time, resume, run, and the effects and their ack
([ADR 0009](docs/adr/0009-one-host-two-queues.md), decision 2 and its cost
table). CBOR is for two things only:

- **rarely called calls:** `ta_host_request` and the host's config;
- **a process boundary:** bytes that leave or enter the process (an
  envelope for another system, OTLP, blobs). The encoding sits beside the
  struct call (`ta_host_envelope` writes a PEER effect as CBOR,
  `ta_host_envelope_read` fills a `ta_envelope`, and `ta_host_deliver`
  still takes the struct), so a caller in the same process never encodes.

Before changing a call's input or output to bytes, find its callers inside
core (`git grep`, ast-grep). If core calls it per event, as the router calls
`ta_host_deliver` for every routed send, it stays a struct. A brief for
such a change states the cost per event, which must be nothing.

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

## Benchmarks

Benchmark reports give throughput and memory together, name the machine
signature, and compare builds side by side, never against numbers from
another day or machine. The torture chamber (`mise run torture` in
`tinyactors-core`) measures both.

## Proposals are not instructions

`docs/proposals/` holds outlines of possible future work. They are not
instructions: do not implement anything from a proposal unless asked to.
Where a proposal disagrees with an ADR or a roadmap, the ADR or roadmap wins.
See `docs/proposals/README.md` for what each status means.

`docs/research/` holds background research (what other projects did, with
sources). Read it before repeating the same research. It is not a decision
and not an instruction.
