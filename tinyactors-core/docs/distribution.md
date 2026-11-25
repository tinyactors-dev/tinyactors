# Distribution: what is released, and what is planned

Core's release is built by `mise run release` in `tinyactors-core`, as
[ADR 0001](../../docs/adr/0001-packaging-and-releases.md) lays out: **the C
libraries** libtinyactors, libtinyactors_runtime and
libtinyactors_datamodel_ecmascript, and **the `tinyactors` command**
([cli.md](cli.md)), released together, tagged
`tinyactors-core/v<version>` in the monorepo and published as `v<version>`
on the GitHub releases page of `tinyactors-dev/tinyactors-core`. Each
release and its date is a section of [CHANGELOG.md](../CHANGELOG.md).

The language bindings are projects of their own, and each packages and
releases itself with its own `mise run release`: `tinyactors-cpython`
publishes the wheels of `tinyactors` on PyPI (tags
`tinyactors-cpython/v<version>`), and `tinyactors-napi` publishes
`@tinyactors/node`, its platform packages and `@tinyactors/wasm` on npm
(tags `tinyactors-napi/v<version>`). Their releases are in their own
`CHANGELOG.md`.

## The tinyactors command

Each release carries the command for every native target, as
`tinyactors-core-<version>-<target>.tar.gz`: one directory holding
`bin/tinyactors`, `LICENSE` and `README.md`. On Linux the program is
static (musl), so it runs on any distribution, glibc or musl; on macOS it
loads only libSystem, and the arm64 one is signed ad hoc. It runs charts
too (`tinyactors run`, [cli.md](cli.md#run)), so it carries the host and
the ECMAScript datamodel with quickjs inside: about 2 MB (2,080,424 bytes
for x86_64-linux, measured 2026-11-25; about 170 KB before `run`), built
without symbols or debug information, and the same bytes for the same
commit. `tinyactors run --schema` writes the protocol's JSON Schema, which
the program carries.

The archive is named after the release repository, so mise's `github:`
backend picks it among the release's archives with no options (an asset
whose name starts with the repository's name scores above the libraries'
`tinyactors-<version>-...`), puts its `bin/` on `PATH`, and checks it
against the release's `SHA256SUMS`:

```sh
mise use github:tinyactors-dev/tinyactors-core            # the latest release
mise use github:tinyactors-dev/tinyactors-core@0.4.0
```

or in a project's `mise.toml`:

```toml
[tools]
"github:tinyactors-dev/tinyactors-core" = "latest"
```

The release builds it from the same sources as `mise run build` (the
tool layer and the layers it uses, and `tools/tinyactors/platform.c`;
QuickJS's sources with its own flags)
with the pinned zig, checks that each binary is for its architecture and
loads nothing but the platform's runtime, runs a few commands with this
machine's (version, compile, inspect, transitions, graph, run --schema), and runs them
again in an Alpine container of each Linux architecture unless
`--skip-containers`.

## Release targets

`Toolchain::Target.releases` (`mise/lib/toolchain/target.rb`) lists the
targets, and `Toolchain::Target.native_releases` the native ones. Every
release binary is built by the pinned `zig cc`.

| Target | zig triple | Runs on |
|---|---|---|
| `aarch64-macos` | `aarch64-macos.11.0` | macOS 11 or later, Apple silicon |
| `x86_64-macos` | `x86_64-macos.11.0` | macOS 11 or later, Intel |
| `x86_64-linux` | `x86_64-linux-musl` | Linux x86_64, glibc or musl |
| `aarch64-linux` | `aarch64-linux-musl` | Linux arm64, glibc or musl |
| `wasm32-freestanding` | `wasm32-freestanding` | a browser, or any WebAssembly host (the C libraries' modules) |

A Linux binary links no C library, so one serves glibc and musl alike
(decision 10); musl's headers are the ones it is compiled against. There
are no Windows and no 32-bit ARM builds.

## The C libraries

Core ships three C libraries, released together under one tag
(`tinyactors-core/v<version>`) and version (`TA_VERSION_*` in
`include/tinyactors/types.h`), as decisions 19 and 20 lay out:

| Library | What it holds | Needs |
|---|---|---|
| `libtinyactors` | the freestanding layers: base, image, core, system, host, basic_http | no C library; base through system never allocate; the host allocates only through the `ta_allocator` it was given ([ADR 0005](../../docs/adr/0005-freestanding-host.md)) |
| `libtinyactors_runtime` | the compiler and the runtime conveniences (`ta_runtime_malloc`, a `ta_allocator` for the host) | `libtinyactors`'s public API, and the C library's allocator, strings, `qsort` and `snprintf` (and files and the clock, except in wasm) |
| `libtinyactors_datamodel_ecmascript` | the ECMAScript data model, QuickJS inside | `libtinyactors`'s public API, and the C library, its math included: a program links `-lm` |

The ECMAScript data model is opted into as a tree-sitter grammar is beside
the tree-sitter runtime: link `libtinyactors_datamodel_ecmascript` and pass
`ta_datamodel_ecmascript_binding()` to a system. QuickJS's own symbols stay
inside it, and every one of its tarballs carries QuickJS's license as
`LICENSE.quickjs`.

Each library is built from its amalgamation, as one object per target in
which only the public API is global (decision 16). The release task
describes the libraries (`mise/tasks/release`); the stages are in
`mise/lib/c_release/build.rb`.

### The assets

Each library's release assets, in `build/release/<version>/assets/`:

| Asset | Contents |
|---|---|
| `<library>-<version>-src.tar.gz` | its header, amalgamation, platform amalgamations (`_wasm.c`, and `_hosted.c` for the runtime) and API description, with `LICENSE` and `README.md` |
| `<library>-<version>-<target>.tar.gz`, for each native target | `include/<library>.h`, `lib/lib<library>.a` and its shared library (`.so.<major>.<minor>` with a `.so` link, or `.<major>.<minor>.dylib`), and `LICENSE` |
| `<library>-<version>-wasm32.tar.gz` | `<library>.wasm`, which imports nothing, the headers in `include/`, and `LICENSE` |
| `<library>-<version>.json` | the manifest: version, commit, source date, zig, the API description's hash, each target's triple, flags and file hashes, and every asset's hash |

and the command's: `tinyactors-core-<version>-<target>.tar.gz` for each
native target and its manifest `tinyactors-core-<version>.json`;
plus one `SHA256SUMS` for all of them. The tarballs are deterministic
(sorted entries, the commit's time, no owners). They hold no CMake or
pkg-config files. The runtime's and the data model's libraries leave
`libtinyactors`'s API undefined, and their shared libraries name
`libtinyactors`'s, so a program links them with it.

The wasm modules are for browsers, and import nothing. `tinyactors.wasm` is
freestanding; `tinyactors_runtime.wasm` and
`tinyactors_datamodel_ecmascript.wasm` hold `libtinyactors` too, since a
module cannot call into another's memory, and have a C library compiled in
(wasi-libc, with no WASI import left). Load one with
`WebAssembly.instantiate(bytes, {})`, call its `_initialize` once if it has
one (the C library's constructors), place bytes
with `ta_wasm_alloc` and give them back with `ta_wasm_free`. The data
model's module has no clock of its own: call
`ta_datamodel_ecmascript_wasm_set_time(Date.now())` before each run, or
Date reads the epoch.

### The tasks

| Command | What it does |
|---|---|
| `mise run amalgamate` | `dist/<library>.h`, `.c` and `_<platform>.c`, each compiled on its own |
| `mise run api [--check]` | `include/<library>.api.json`: what each library exports, and what the changelog compares |
| `mise run wasm:test` | Builds the three wasm modules as the release does and runs `tests/wasm/release_test.mjs` against each, instantiated with no imports as a browser would |
| `mise run release -- <version> --build-only` | Only build, verify and pack (no checks, no version set, nothing committed): a look at the packaging before a release |
| `mise run release -- <version>` | The release as a dry run: the checks (a clean `main` in step with `origin`, a free tag, the changelog section, core's whole test task, the release build), every target of the three libraries, their symbol checks, the smoke programs (`tests/release/`) and `test_runtime` and `test_e2e` against the packed libraries of this machine, the smoke programs in Linux containers, the wasm tests, and the assets; nothing is committed, tagged or uploaded |
| `mise run release -- <version> --publish` | The same, then the commit `release(tinyactors-core): v<version>`, the tag, the push and the GitHub release with the assets |

`--skip-containers` leaves out the container smoke tests (no Docker), and
`--skip-tests` leaves core's test task out of the checks, for a commit
whose tests passed already. The notes are written to
`build/release/<version>/notes.md`. A failure after the commit and tag,
before anything is pushed, undoes them. A release needs a section in
`tinyactors-core/CHANGELOG.md` (component `core`), written with the
release-changelog skill.
