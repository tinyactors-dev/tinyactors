# How the build works

A reference to the build's parts and how they fit. The rules those parts
keep are in [Design constraints](design-constraints.md).

## Tasks

Every command is a mise task: `mise run <task>`. The root `mise.toml`
declares a monorepo (`config_roots`) of two projects: `tinyactors-core`
and `tinyxml` (`tinyactors-scxml` and `tinyactors-otlp` are parked, see
the README). Each project has its own `mise.toml` and tasks, which
run from the root as `mise run //<project>:<task>` or from inside the
project as `mise run <task>`.

A task is a file task: an executable Ruby script in `mise/tasks/` (or
`<project>/mise/tasks/`), with no extension. Its header tells mise about
it:

```ruby
#!/usr/bin/env ruby
#MISE description="Test core: ..."
#MISE sources=["include/**/*", "src/**/*", "../mise/lib/**/*", "../mise.toml"]
#MISE outputs={auto=true}
#USAGE flag "--full" help="Also run the exhaustive checks"
```

- `sources` and `outputs={auto=true}` let mise skip the task while none of
  its sources changed since it last passed. A failed run is not
  remembered, so it runs again.
- `depends` names tasks to run first.
- `#USAGE` declares flags; mise passes them as `usage_<name>` environment
  variables.

A task directory (`mise/tasks/build/`) holds subtasks: `build/_default`
is `mise run build`, `build/library` is `mise run build:library`.

## The shared Ruby library

The root `mise.toml` puts `mise/lib/` on `RUBYLIB`, so every task in every
project can `require 'toolchain'`, `require 'cache'` and so on without a
relative path. A project's own helpers live in `<project>/mise/lib/` and
are loaded with `require_relative`.

| Library | Purpose |
|---|---|
| `toolchain` | Tools, targets, object formats, binaries and the host (below) |
| `cache` | Content-based skipping and the compile cache |
| `task_stamp` | Forces a task whose declared outputs were deleted |
| `platform_files` | Which `src/` files a target builds, by file name suffix |
| `c_release` | `dist` and the C libraries' release |
| `release` | Tags, notes and the GitHub release every project shares |
| `build_scripts` | The list of build scripts, and their static analysis |
| `c_header` | A C header's declarations, read with ast-grep's C parser |
| `acronym_case` | The acronym check |

## Caching

mise decides freshness by modification time. Two libraries add
decisions by content:

- **`Cache.gate(name, inputs:, outputs:, extra:)`** hashes a task's inputs
  (files and strings) and skips the task when the hash equals the one the
  last successful run wrote. The old stamp is deleted before work starts
  and the new one written only on exit 0, so a failure never leaves a
  success behind.
- **`Cache.compile(command, source, object)`** compiles one translation
  unit and reuses the object while the command, the compiler's identity,
  the source and every header it read are unchanged. A change to comments
  or layout only keeps the object (it compares the preprocessed unit).

`TXML_FORCE=1` or `TA_FORCE=1` ignores every stamp; `rm -rf build` does
the same for one project. Tool identities (`Tool#identity`,
`Cache.tool_id`) are part of the keys, so a new compiler rebuilds.

## The toolchain

Everything that differs between hosts is in `mise/lib/toolchain/`, and
nothing outside it may depend on the host. The toolchain is **LLVM 22**
(the `clang` and `conda:llvm-tools` pins), with **zig cc** for release
binaries, because zig brings every target's C library and SDK.

### `Toolchain::Tool`

A program pinned in the root `mise.toml`, found with `mise which`:

```ruby
Toolchain::Tool[:llvm_nm].run('-g', object)      # stdout, or raises Tool::Failed
Toolchain::Tool[:clang].capture('-E', source)    # [stdout, stderr, status]
Toolchain::Tool[:zig].identity                   # "0.16.0", for cache keys
```

`Tool::PINNED` maps each name to its program and its `mise.toml` entry.
A tool that is not installed raises `Tool::Missing` with the fix.

### `Toolchain::Target`

What the build compiles for. The kind a script asks for says what the
result is for:

| Target | Compiler | Result |
|---|---|---|
| `Target.release(name)` | zig cc, pinned triple, fixed flags | What is shipped: `aarch64-macos`, `x86_64-macos`, `x86_64-linux`, `aarch64-linux`, `wasm32-freestanding` |
| `Target.check` | pinned clang, `x86_64-unknown-linux-gnu`, freestanding | What a check reads; the same on every host |
| `Target.host` | pinned clang, this machine | Programs that run here: tests, development builds |

A target compiles, archives and links, and answers a `Binary`:

```ruby
target = Toolchain::Target.release('x86_64-linux')
object = target.compile(source, into: dir.join('lib.o'), includes: [...])
target.archive([object], into: dir.join('lib.a'))
target.shared_library([object], library: 'txml', version: '0.9', dir: lib)
target.plugin(objects, into: out)                    # a Node-API addon
target.program(inputs, into: out, flags: [...])
target.compile_command(*flags)                       # for Cache.compile
target.configure_environment                         # CC, AR, RANLIB, NM for autotools
```

### `Toolchain::ObjectFormat`

`ELF`, `MachO` and `WASM`: how a C name is spelled in the symbol table,
what a shared library is called and how it names itself, the linker's
flags, and what the platform's runtime adds (libSystem, ld64's own
symbols). A target has a format; a binary finds its own.

### `Toolchain::Binary`

An object, archive, shared library or module on disk. It reads its
format with `llvm-readobj`, its symbols with `llvm-nm` as C names, and
checks itself:

```ruby
Toolchain::Binary.new(path)
  .must_be_for!(target)
  .must_export_exactly!(api)
  .must_leave_undefined_only!(allowed)
  .must_load_only!(needed)
  .must_be_named!(file)
```

Each `must_*!` answers the binary, or raises `Binary::Mismatch` saying
what differs.

### `Toolchain::Host`

The machine the build runs on: `Host::Mac` or `Host::Linux`. It knows the
release target that runs natively (`Host.current.target`), refuses what
needs another host (`must_run!`), checks that the C library is there, and
describes the machine for benchmarks (`hardware`, `power`, `conditions`).

## Releases

A C project's `release` task describes its libraries (`CRelease::Library`:
the project, its packed tests, its smoke program, its source tarball, the
libraries of the same release it builds on) and calls `CRelease.release`;
all of them are released under one tag. The stages:

1. **Check:** a clean `main` in step with its remote, the tag free, the
   project's checks.
2. **Version:** the version macros set, the generated files regenerated.
3. **Build**, for each library after those it uses: each native target's
   release object (the amalgamation and the target's platform
   amalgamations, the vectorized memset and zig's compiler-rt, combined,
   with only the API left global), its static archive and its shared
   library; the wasm module.
4. **Verify:** each library's symbols, dependencies, name and
   architecture (`Binary`); the tests against the packed libraries of this
   machine's target; a smoke test in a Linux container per Linux target;
   the wasm module's imports and exports, and its test.
5. **Pack:** deterministic tarballs (pinned GNU tar and gzip), a manifest
   per library and one `SHA256SUMS`.
6. **Publish** (`--publish`): push the commit and tag, then the GitHub
   release.

`dist` is the host case: each project publishes its header, amalgamation,
API description and a host archive into `dist/`, for the other projects
in the workspace to build against.

## The test pass

`mise run test` (`mise/tasks/test/_default`) runs, in order:

1. the acronym check and its own tests;
2. `check-portability`, the static analysis of the build scripts;
3. `toolchain-test`, which builds every release target, the check target
   and the host target from this host, and reads what it built;
4. `build`;
5. each project's `test` task, which mise skips while its sources are
   unchanged.

`--all` forces every step; `--full` adds the exhaustive checks. See
`AGENTS.md` (Tests) for which change reruns which project.

## Static analysis

`mise run check-portability` runs ast-grep over every Ruby build script.
The rules are in `mise/ast-grep/rules/`, each with a note saying what to
use instead; their tests are in `mise/ast-grep/tests/` and run first.

| Rule | Rejects |
|---|---|
| `host-detection` | `RUBY_PLATFORM`, `RbConfig::CONFIG`, `Etc.uname` |
| `platform-branch` | `macos?`, `linux?`, `wasm?`, `darwin?` |
| `host-toolchain-environment` | reading `CC`, `AR`, `NM`, `CLANG` and similar variables |
| `unpinned-tool` | `nm`, `ar`, `cc`, `tar`, `readelf`, `llvm-*`, `sysctl` and similar as a command |

`mise/lib/toolchain/` is exempt: it is where those things are allowed.
ast-grep picks a language by file extension and mise's task files have
none, so `BuildScripts.scan` copies the scripts under `.rb` names into a
temporary directory, scans that, and reports each finding at the script's
real path and line.
