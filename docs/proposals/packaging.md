# Packaging and releases

**Status:** Proposed (2026-09-27; first sketched 2026-09-24)
**Touches:** tinyxml, tinyactors-core, tinyactors-scxml (release tasks,
version macros, a wasm allocator), root `mise/lib` (the shared release
library), tinyactors-napi (package layout, loader), root `mise.toml` (zig)
**Not a plan:** nothing here is scheduled. ADRs and roadmaps win. The
decisions below were made in discussion on 2026-09-27; they move into ADRs
when the work starts.

## Goal

- Every library in the repository is released on its own, with its own
  version, by one command run on the maintainer's machine.
- A C embedder downloads a header and a prebuilt library for their
  platform, or the amalgamation, or a WebAssembly build.
- In a Node or Bun project, `npm install tinyactors` installs a prebuilt
  addon: no compiler, no `node-gyp`, no postinstall download.

## Decisions (2026-09-27)

1. **Releases are built locally, not in CI.** The maintainer runs the
   release on their own machines. GitHub hosts the tags, the releases and
   their assets, and nothing else: no GitHub Actions in the release path.
2. **The release is part of the build tasks.** It is a mise file task in
   Ruby like every other task, with the shared logic in root `mise/lib/`,
   and it reuses what `amalgamate` and `dist` already do instead of a
   second build system beside them.
3. **Each project is released on its own**, with its own version and tag.
   This replaces the 2026-09-24 sketch's single version for everything.
4. **What a C-only project ships:**
   - its public header;
   - a source-only amalgamation;
   - a `wasm32-freestanding` build;
   - a static and a dynamic library for Apple silicon macOS
     (`aarch64-macos`), Linux x86-64 and Linux arm64.

   Windows comes later.
5. **Every binary is built with `zig cc`**, pinned in root `mise.toml`,
   from any host, for every target.
6. **The wasm build contains an allocator.** It is built on the linear
   memory builtins that `zig cc` (clang) provides,
   `__builtin_wasm_memory_size` and `__builtin_wasm_memory_grow`, and it is
   linked into the module. A host that loads the module needs to supply
   no memory management of its own.

## What exists today (2026-09-27)

- `tinyxml` and `tinyactors-core` each have an `amalgamate` task (a
  checked-in `include/<name>.c`) and a `dist` task that publishes the
  header, the amalgamation, the API description and a static library for
  the host, for consumers inside the repository. The two `dist` tasks are
  nearly the same file. `dist` already hard-codes its flags and keeps its
  manifest a pure function of content.
- `tinyactors-scxml` builds against both `dist/` directories. It has no
  amalgamation and no `dist` task.
- `txml.h` has `TXML_VERSION_MAJOR/MINOR/PATCH` (0.1.0). Core and the
  SCXML codec have no version macros and no version function.
- Both libraries take memory through an allocator fat pointer, `alloc`
  with size and alignment and `free` with size and alignment, and no
  `realloc` (core ADR 0023, `txml.h`).
- Public headers include only `stdint.h`, `stddef.h` and `stdbool.h`.
  `mise run //tinyxml:freestanding` proves txml needs no C library; core
  has no such check.

### Measured on 2026-09-27

Both amalgamations compile with clang 22 and `-ffreestanding -O2`, with no
sysroot, for `wasm32-unknown-unknown`, `aarch64-linux-gnu`,
`x86_64-linux-gnu` and `arm64-apple-macos11`. Their undefined symbols:

| Object | wasm32 | aarch64-linux | arm64-macos |
|---|---|---|---|
| txml | none (only `__stack_pointer`, `__indirect_function_table`, which the linker provides) | none | `___stack_chk_fail`, `___stack_chk_guard` |
| core | `__multi3` | `memcpy`, `memset` | `_bzero`, `_memcpy`, `___stack_chk_*` |

So:

- **Static archives need nothing else.** Linking a shared library or a
  standalone `.wasm` needs a linker and compiler-rt, which the mise clang
  does not ship (no `lld`, `wasm-ld` or `ld64.lld`). `zig cc` has both.
- **macOS enables the stack protector by default.** Release flags pass
  `-fno-stack-protector`, since the libraries use no C library.
- **The optimizer turns struct copies and zeroing in core into `memcpy`
  and `memset` calls, and core multiplies 128-bit integers** (`__multi3`
  on wasm32). Core needs these from somewhere: compiler-rt through zig, or
  its own definitions. Core should get a freestanding check like
  txml's either way, so that what it needs is chosen on purpose.

From the Mac on 2026-09-24, `zig cc` 0.16.0 built the napi addon for
`aarch64-macos`, `x86_64-macos`, `x86_64-linux-gnu.2.28`,
`aarch64-linux-gnu.2.28` and `x86_64-linux-musl`. The `aarch64-macos`
addon loads in Node and Bun 1.4.1 and passes all 14 napi test files under
both.

## C releases

### Versions and tags

- **Tags are per project:** `tinyxml/v0.2.0`, `tinyactors-core/v0.4.0`,
  `tinyactors-scxml/v0.1.0`. Each tag gets one GitHub release.
- **The public header holds the version.** Core gets `TA_VERSION_*` and
  `ta_version()`, the codec its own macros, like `TXML_VERSION_*`. The
  API description (`*.api.json`) carries the version too.
- **0.x semver** while APIs change freely (root AGENTS.md: the libraries
  are private): the minor version for any breaking change, the patch for
  the rest.
- **Release notes** are the Conventional Commits since the project's last
  tag that touch its directory
  (`git log tinyxml/v0.1.0..HEAD -- tinyxml/`).

### Assets of one release (tinyxml 0.2.0 as the example)

| Asset | Contents |
|---|---|
| `txml-0.2.0-src.tar.gz` | `txml.h`, `txml.c` (the amalgamation), `txml.api.json`, license, README |
| `txml-0.2.0-wasm32.tar.gz` | `txml.h`, `lib/libtxml.a` (wasm32 objects, for linking into a larger module), `txml.wasm` (standalone, allocator included) |
| `txml-0.2.0-aarch64-macos.tar.gz` | `include/txml.h`, `lib/libtxml.a`, `lib/libtxml.0.2.dylib` (install name `@rpath/libtxml.0.2.dylib`) |
| `txml-0.2.0-x86_64-linux.tar.gz` | `include/txml.h`, `lib/libtxml.a`, `lib/libtxml.so.0.2` (the soname) and a `libtxml.so` symlink |
| `txml-0.2.0-aarch64-linux.tar.gz` | the same, for arm64 |
| `txml-0.2.0.json` | manifest: version, commit, targets, flags, the sha256 of every file, the versions of dependencies |
| `SHA256SUMS` | checksums of every asset |

- **Every library is built from the amalgamation**, the one translation
  unit whose private functions are `static`, so each binary exports
  exactly the public API. Shared libraries compile with
  `-fvisibility=hidden` and `<PREFIX>_API` expands to default visibility.
- **One Linux build serves glibc and musl.** The libraries link no C
  library, so the name has no `-gnu` or `-musl`. The verify step checks
  this instead of assuming it.
- **The shared library name follows the minor version** while on 0.x,
  because a minor bump is the breaking one.

### The wasm build

- **Target `wasm32-freestanding`** through `zig cc`. `txml.wasm` exports
  the public API and `memory`, has no entry point (`--no-entry`) and
  imports nothing.
- **The allocator** is a small C file compiled and linked into every
  `.wasm`:
  - It grows linear memory with `__builtin_wasm_memory_grow(0, pages)` and
    reads its size with `__builtin_wasm_memory_size(0)`. It starts at the
    linker's `__heap_base`.
  - It implements the library's allocator vtable, `alloc` and `free` with
    size and alignment. Freed blocks go on free lists by size class, so a
    long-running module reuses memory. Memory is never returned to the
    host, since wasm cannot shrink linear memory.
  - The module exports the allocator, so the host passes it to the
    library's constructors without implementing one. It also exports raw
    `alloc` and `free`, so a JavaScript host can place input bytes in
    linear memory.
- **`libtxml.a` for wasm32 has no allocator**, for embedders who link the
  library into their own module and bring their own.

### The release command

```sh
mise run //tinyxml:release 0.2.0             # check, build, verify, pack; publishes nothing
mise run //tinyxml:release 0.2.0 --publish   # the same, then tag and GitHub release
```

Each project's task only describes the project: name, header,
amalgamation, API description, symbol prefix, dependencies. The work is
in a shared `mise/lib/release.rb`. The compile-and-archive step of the two
`dist` tasks moves into it with a target parameter, so `dist` becomes the
host case of the release build (one way to do each thing, root
AGENTS.md). The stages:

1. **Check:** a clean tree on `main`, in step with `origin`, the tag is
   free, `amalgamate --check` and `mise run test` pass.
2. **Version:** set the header's version macros, commit
   `release(tinyxml): v0.2.0`, tag `tinyxml/v0.2.0`.
3. **Build:** every target with `zig cc` into
   `<project>/build/release/<version>/<target>/`. The flags are fixed per
   target in `release.rb`; `$CFLAGS` is not read.
4. **Verify:**
   - The defined, exported symbols of each binary are exactly the
     functions in `*.api.json`.
   - The undefined symbols are in a per-target allowlist, which is empty
     for txml.
   - Each file is for the architecture its name says.
   - The project's tests pass against the packed static and shared
     library on the host.
   - The other Linux architecture is tested in a container through qemu
     (Docker is installed).
   - `txml.wasm` is loaded with Node's `WebAssembly` and runs a smoke
     test that allocates through the built-in allocator.
5. **Pack:** deterministic tarballs (sorted entries, mtimes from
   `SOURCE_DATE_EPOCH` set to the commit time, owner 0, `gzip -n`), a
   deterministic `ar`, `-ffile-prefix-map`, then the manifest and
   `SHA256SUMS`.
6. **Publish** (with `--publish`): push the commit and tag, then `gh
   release create --verify-tag` with the assets and notes.

**Secrets.** The GitHub token comes from fnox. The task never reads or
prints it.

### Dependencies between releases

`tinyactors-scxml` depends on core and txml. It needs an `amalgamate` task
before it can ship. Its release:

- ships only its own code, and records the core and txml versions it was
  built against in its manifest;
- passes the check only when the `dist` digests of core and txml at HEAD
  match the manifests of their published releases, so the codec never
  ships against an unreleased library.

## npm packages (the napi addon)

Unchanged from the 2026-09-24 sketch except for versions: the addon has
its own version and tag (`tinyactors-napi/v0.1.0`), and its platform
packages share that version. It uses the same toolchain module in
`release.rb`.

| Package | Contents |
|---|---|
| `tinyactors` | `index.js`, `lib/`, `index.d.ts`, README, the loader; `optionalDependencies` on every platform package at the same exact version |
| `@tinyactors/darwin-arm64` | `tinyactors.node`; `"os": ["darwin"], "cpu": ["arm64"]` |
| `@tinyactors/darwin-x64` | same, `x64` |
| `@tinyactors/linux-x64-gnu` | `"os": ["linux"], "cpu": ["x64"], "libc": ["glibc"]` |
| `@tinyactors/linux-arm64-gnu` | `arm64`, `glibc` |
| `@tinyactors/linux-x64-musl` | `x64`, `musl` (Alpine) |
| `@tinyactors/linux-arm64-musl` | `arm64`, `musl` |

- npm, pnpm and Bun install only the optional dependency whose `os`,
  `cpu` and `libc` match.
- **The loader** (`lib/native.js`) picks the package for
  `process.platform`, `process.arch` and the libc (musl detected from
  `process.report`), and falls back to `build/tinyactors_napi.node` in
  the repository. When nothing matches, it throws one error naming the
  platform it looked for and the supported ones.
- **Node-API is ABI-stable**: one binary per platform serves every Node
  with Node-API 10 (22.14 and later; `engines` should say `>=22.14`) and
  Bun.
- **Node-API headers** come from the pinned npm package
  `node-api-headers` under `vendor/`, not from the Node mise installed.
- macOS links with `-undefined dynamic_lookup`; Linux needs no flag.
- Release: `npm pack` each package, smoke test the packed tarballs with
  npm and Bun (Linux ones in a container, glibc and musl), run the whole
  napi suite under Bun, then publish the platform packages, then the main
  package, then the tag and GitHub release. The npm token comes from
  fnox; no `.npmrc` with a token is written to the tree.
- A scope needs an npm organization, `tinyactors`, created once.

## Later

- **Windows**, for the C libraries and the addon.
- **npm provenance and trusted publishing** need GitHub Actions' OIDC,
  which decision 1 rules out for now. Revisit only if provenance becomes
  a requirement.
- **A WebAssembly npm package** (`@tinyactors/wasm`) as the fallback when
  no native package matches, the way esbuild falls back to `esbuild-wasm`.

## Open questions

- **Where the wasm allocator lives.** One C file shared by every project
  (under root `mise/lib/`, or a small `tinywasm` directory), or a copy in
  each project? Core and txml have the same vtable shape but different
  type names.
- **Core's `memcpy`, `memset` and `__multi3`.** Take them from zig's
  compiler-rt, define them in core, or change the code so the compiler
  does not emit them?
- **A signature on `SHA256SUMS`**, for example `ssh-keygen -Y sign` with
  the git signing key.
- **Several machines per release.** Every target cross-compiles from one
  host, so one machine suffices. If a second machine should build a
  target (or compare digests as a reproducibility check), the release
  could be created as a draft that each machine uploads to, and made
  public once every expected asset matches `SHA256SUMS`.
- **Public repository.** While it is private, release assets are only
  visible to collaborators.
- **Package name** for Node: `tinyactors` or `@tinyactors/node`.
- **Oldest glibc** for the addon: 2.28 (Debian 10, RHEL 8) or 2.17
  (RHEL 7). The C libraries link no libc, so they are unaffected.
- **Does the ECMAScript or jq data model ship?** Both live in the test
  host; the npm package does not need them.
