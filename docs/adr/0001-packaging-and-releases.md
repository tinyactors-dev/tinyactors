# ADR 0001: Packaging and releases

**Status:** Accepted (2026-09-28). Supersedes the decisions in `../proposals/packaging.md`, which keeps the measurements and the reasoning behind them.
**Date:** 2026-09-28

## Context

Nothing in the repository is released yet. The C libraries (`tinyxml`,
`tinyactors-core`, `tinyactors-scxml`) have `amalgamate` and `dist` tasks
for consumers inside the repository, and the Node-API addon
(`tinyactors-napi`) loads the build in its own tree. The packaging
proposal (`../proposals/packaging.md`) measured what the toolchains do and
laid out how each library and the addon would ship. It was discussed on
2026-09-27; the work starts now, so its decisions move here.

## Decisions

1. **Releases are built locally, not in CI.** The maintainer runs the
   release on their own machines. GitHub hosts the tags, the releases and
   their assets, and nothing else: no GitHub Actions in the release path.
2. **The release is part of the build tasks.** It is a mise file task in
   Ruby like every other task, with the shared logic in root `mise/lib/`
   (`toolchain.rb`, `platform_files.rb`, `release.rb`), and it reuses what
   `amalgamate` and `dist` already do instead of a second build system.
3. **Each project is released on its own**, with its own version and tag
   (`tinyxml/v0.2.0`, `tinyactors-core/v0.1.0`, `tinyactors-napi/v0.1.0`).
4. **What a C-only project ships:** its public header; a source-only
   amalgamation; a `wasm32-freestanding` build, one standalone `.wasm`
   module and no archive of wasm objects; a static and a dynamic library
   for `aarch64-macos`, Linux x86-64 and Linux arm64. Windows comes later.
5. **Every binary is built with `zig cc`**, pinned in root `mise.toml`,
   from any host, for every target. The flags are fixed per target in
   `mise/lib/toolchain.rb`; `$CFLAGS` is not read.
6. **The wasm build contains an allocator**, built on
   `__builtin_wasm_memory_size` and `__builtin_wasm_memory_grow` and linked
   into the module, so a host supplies no memory management. Each project
   has its own copy, written against its own vtable types.
7. **A release is published directly**, not as a draft, and updating a
   published release (adding a target, replacing an asset, rewriting the
   notes) is a supported step. The tag never moves.
8. **The libraries should not call `memcpy` or `memset`.** No compiler
   option guarantees this on native targets; decision 16 settles it by
   bundling them, hidden, into each binary.
9. **Platform-specific source files are chosen by file name, as in Go.** A
   file named `*_wasm.c` is built only for wasm. The list of suffixes and
   the one function that applies it live in `mise/lib/platform_files.rb`;
   every task selects sources through it.
10. **The Linux addon is built once per architecture and links no C
    library.** It compiles against musl's headers and links with
    `-shared -nostdlib`, so its libc functions bind to whichever libc Node
    uses: one binary serves glibc and musl.

### Decided since the proposal

11. **Node package names.** The addon ships as `@tinyactors/node`, with
    one package per platform: `@tinyactors/node-darwin-arm64`,
    `@tinyactors/node-darwin-x64`, `@tinyactors/node-linux-x64` and
    `@tinyactors/node-linux-arm64`, each with `os` and `cpu` and no `libc`
    field. The main package lists them as `optionalDependencies` at its own
    exact version. This leaves `@tinyactors/wasm` for a fallback later.
12. **Secrets.** The npm token is in fnox as `NPM_TOKEN`: `fnox exec --
    <command>` puts it in that command's environment. A task never prints
    it and never writes it to a file; an `.npmrc` may reference
    `${NPM_TOKEN}`, and only as a temporary file outside the tree. GitHub
    releases go through `gh`, which is already authenticated.
13. **The repository stays private for now.** Release assets are visible
    only to collaborators until it is public; npm packages are public from
    their first publish.
14. **Verification without every machine.** macOS binaries are cross-built
    from Linux and verified by their symbols only, until a Mac runs them.
    Linux arm64 gets a smoke test under qemu, not the full suites, whose
    timing assertions fail there.
15. **Development builds go to the `next` dist-tag** (2026-09-28). `mise
    run //tinyactors-napi:release -- --dev --publish` publishes HEAD as
    `<next minor>-dev.<YYYYMMDD>.<n>.g<sha>`: the commit's date in UTC,
    the commits since the last release, and the commit. Semver compares
    prerelease identifiers left to right, numeric ones as numbers, so on
    linear `main` a later commit always sorts later; the sha only names the
    commit (a sha alone does not sort, and `g` keeps an all-digit one from
    being an invalid number). The same checks, builds and smoke tests run as
    for a release. A development build publishes all five packages under
    `next`, never `latest`, and makes no commit, tag or GitHub release: npm
    keeps every version, so it is published on demand, not per commit.
    `mise run //tinyactors-napi:package -- --dev` stamps the same version
    into local tarballs.

16. **`memcpy` and `memset` are bundled, hidden, as Ghostty does**
    (2026-09-28). The compiler lowers aggregate copies and zeroing to
    `memcpy` and `memset` whatever the options (decision 8). Instead of
    leaving them to the host, each native binary carries zig's compiler-rt,
    which defines `memcpy`, `memset`, `memmove`, `memcmp` and `bcmp`, and
    keeps those definitions private:
    - **One release object per target.** The amalgamation, compiled with
      the API at default visibility, is partial-linked with compiler-rt
      (the object `zig build-lib -fcompiler-rt` puts in its archive). On
      ELF this goes through zig's `ld.lld -r --gc-sections`, rooted at the
      API, so only the compiler-rt functions the library calls stay; zig
      has no `ld64.lld`, so on Mach-O it is `zig cc -r`, without
      collection. Then `llvm-objcopy --strip-debug
      --keep-global-symbols=<the API>` leaves only the API global:
      everything else is local, the five memory functions and the
      amalgamation's own file-scope data (txml's `txp_*` tables) alike.
      Partial-linking first matters: localizing compiler-rt's copies alone
      would leave the library's own calls undefined, for the host's libc
      to resolve. Ghostty localizes the same way on macOS only (`nmedit`);
      here it is done for every target.
    - **Static archives** hold that one object, under a plain member name.
      An archive therefore neither imports `memcpy` nor overrides the
      `memcpy` of the program that links it, and no internal name can clash
      with the program's.
    - **Shared libraries** are linked from the same object: on Linux with
      `-shared -nostdlib --gc-sections`, on macOS with `-dead_strip` and an
      exported-symbols list.
    - **wasm32** needs nothing: zig 0.16's default wasm CPU has bulk
      memory, so copies and zeroing are `memory.copy` and `memory.fill`.
    - **The release's symbol check:** the exported symbols are exactly the
      functions in the API description (plus the linker's own
      `___dso_handle` and `__mh_dylib_header` in a dylib), the memory
      functions are local, and nothing is undefined, except on macOS four
      libSystem symbols every process has: `dyld_stub_binder` (dylibs),
      compiler-rt's `__availability_version_check` and
      `_sys_icache_invalidate`, and `_bzero`, to which Darwin's clang lowers
      zeroing whatever the flags (`-fno-builtin-bzero` does not stop it) and
      which compiler-rt does not define.
    - **To measure before copying Ghostty further:** compiler-rt's `memset`
      is scalar in zig 0.16. Ghostty measured it 24 times slower and ships
      its own vectorized, hidden `memset`; core's release measures its own
      cost first (measured below).
    - **Measured for core, 2026-09-28: the release ships its own
      `memset`.** A workload of 200,000 spawn, start, deliver and destroy
      cycles, then 100,000 resident actors each taking four events
      (x86_64-linux, zig 0.16.0 `-O2`, the same amalgamation linked three
      ways), in seconds:

      | Linked with | Total | Spawn-to-destroy, per actor |
      |---|---|---|
      | glibc's `memcpy` and `memset` | 0.322–0.329 | 688–707 ns |
      | compiler-rt's `memcpy` and `memset` (the packed library) | 0.400–0.407 | 857–893 ns |
      | compiler-rt's `memcpy`, glibc's `memset` | 0.325–0.328 | 692–711 ns |
      | the packed library with the release's `memset` | 0.319–0.332 | 681–747 ns |

      All of the 24% came from `memset`; compiler-rt's `memcpy` matches
      glibc's. Core's calls are small: of 2,040,072 in a shorter run,
      1,860,024 cleared 8 to 15 bytes and 140,004 cleared 256 to 511. So
      every native release object links `mise/lib/c_release/memset.c`: a
      hidden, strong `memset` that takes the place of compiler-rt's weak
      one, made local like the other memory functions. It never loops over
      bytes: two overlapping stores of 1, 2, 4 or 8 bytes up to 16, two
      overlapping 16-byte vector stores (clang's vector extension: SSE2,
      NEON) up to 32, and 64 bytes a round above. A first version with a
      byte loop under 16 bytes was slower than compiler-rt's (0.465 s),
      because of those small calls. The wasm module needs none: bulk
      memory makes zeroing an instruction.
    - **Size**, for txml 0.1.0: the collected Linux archive is 175 KB (the
      joined object is 2 MB before collection and stripping); the macOS
      one, without collection, 379 KB, and its dylib 139 KB after
      `-dead_strip`.

    Sources: ghostty-org/ghostty at `b40acce58dcf`,
    `src/build/GhosttyLibVt.zig` L239-252 (`bundle_compiler_rt` for static
    builds), `src/quirks_memset.zig` (the hidden vectorized `memset`),
    `src/build/libsystem_override.sh` (localizing on macOS); zig 0.16.0
    `lib/compiler_rt.zig` L24-44 (compiler-rt's weak, hidden linkage).
    Measured with zig 0.16.0 and `llvm-objcopy` on 2026-09-28 for
    `x86_64-linux`, `aarch64-linux` and `aarch64-macos`.
17. **A library that builds on others ships only its own code**
    (2026-09-28; the SCXML codec on core and txml). Decision 16's "nothing
    undefined" becomes, for such a dependent:
    - **Built against published releases.** Its release downloads each
      dependency's release for the version its header has at HEAD (`gh
      release download`), checks every file against that release's
      `SHA256SUMS`, and refuses to go on unless the published header,
      amalgamation and API description are byte-identical to HEAD's: it
      never ships against an unreleased library. Its manifest records
      `dependencies: { name: version }`.
    - **Undefined symbols:** its archive and shared library may leave
      undefined exactly the functions of its dependencies' published API
      descriptions (plus decision 16's macOS list), and nothing else.
    - **Its shared library names its dependencies:** it is linked against
      their released shared libraries, so on Linux it needs exactly their
      sonames (`libtinyactors.so.0.1`, `libtxml.so.0.1`) and still no C
      library, and on macOS it loads exactly their `@rpath` install names
      beside libSystem. A program that links it needs nothing more to find
      them than any installed library: without this, a linker that drops
      unreferenced libraries (`--as-needed`, musl's loader in the smoke
      test) leaves the dependencies' functions unresolved.
    - **Its API macro may be a dependency's.** The codec declares its
      functions with core's `TA_API`, so the release compiles it with
      `TA_API` at default visibility (`CRelease::Project#api_macro`); with
      the wrong macro, `-fvisibility=hidden` hides the API, and zig's
      Mach-O linker crashes on a dylib that then exports nothing.
    - **Tests use the released artifacts together:** the codec's test
      program and its container smoke test link the packed codec with
      core's and txml's released libraries, static and shared.
    - **No wasm module of its own.** A module cannot call into another
      module's linear memory (decision 4's reasoning against a wasm
      archive), so a standalone `ta_scxml.wasm` could not use core's or
      txml's. A wasm build that uses the codec compiles the three
      amalgamations from the source tarballs into one module, as the
      repository's own combined builds do.

18. **`@tinyactors/node` is built from `tinyactors-core/bindings/node`**
    (2026-10-06). The standalone runtime (ADR 0004) replaced the addon in
    `tinyactors-napi/` with its own Node-API binding, so that binding is
    what `@tinyactors/node` ships. The packages, names and rules of
    decisions 10, 11, 14 and 15 are unchanged:
    - **The tasks are core's:** `node:build --target`, `node:symbols`,
      `node:package`, `node:smoke`, `node:smoke-containers` and
      `node:release` in `tinyactors-core` (`tinyactors-core/docs/distribution.md`),
      with what they share in `tinyactors-core/mise/node_package.rb`. A
      release addon is the `addon-<target>` build (`Core.addon_variant`):
      the addon build's flags, compiled and linked by the target's `zig cc`.
    - **The release line continues.** Versions are
      `bindings/node/package.json`'s and tags stay `tinyactors-napi/v*`, so
      the next release is above the published 0.1.0 and a development
      build counts from it. The changelog component `napi` (scopes `napi`
      and `node`) now has the binding's directory and keeps that tag
      prefix (`Changelog::Component#tag_prefix`).
    - **The main package keeps the repository's layout:** `node/` and
      `shared/`, so `index.js` imports `../shared/core.mjs` unchanged.
      `index.js` loads this tree's build first, then the platform package.
    - **Containers:** Node on glibc and musl, x64 and arm64, and Bun on
      Debian x64 and on arm64. Bun on musl x64 is left out: its build needs
      AVX, which an emulated x64 lacks, so it crashes before the addon
      loads.

19. **Core ships two C libraries, released together** (2026-10-06). The
    standalone runtime (ADR 0004) has freestanding layers and hosted ones,
    so one library could not keep decision 16's "nothing undefined":
    - **`libtinyactors`** is the freestanding layers (base, image, core,
      system, basic_http), and keeps decision 16 as it is.
    - **`libtinyactors_runtime`** is the compiler, the host and the runtime
      conveniences. It calls nothing of `libtinyactors`'s but its public API
      (the one internal function the two shared, `ta_image_hash`, is inline
      in an internal header), so it is built against `libtinyactors` as
      decision 17 builds a dependent: its libraries leave `libtinyactors`'s
      API undefined, and its shared library names `libtinyactors`'s.
    - **It may import a list of C library functions** (allocation, strings,
      `qsort`, `snprintf`, files and the clock), each with the same name and
      signature in glibc and musl; its Linux libraries link no C library, as
      decision 10 has it for the addon. The list is in core's `release`
      task, and the symbol check fails on anything else.
    - **One tag and version for both** (`tinyactors-core/v<version>`,
      `TA_VERSION_*` in `include/tinyactors/types.h`): the runtime is built
      against the `libtinyactors` of the same release, not a downloaded one.
      `CRelease.release` takes a list of libraries (`CRelease::Library`), each
      after those it `uses`.
    - **Each ships a header, an amalgamation, its API description, a static
      and a shared library per native target, and a wasm module.** The
      amalgamations and headers are generated into `dist/` (`mise run
      amalgamate`); the API descriptions are checked in
      (`include/<library>.api.json`, `mise run api`, `mise/lib/c_api.rb`).
    - **The wasm modules are for browsers, and import nothing**: a browser
      supplies no imports, WASI's least of all. `tinyactors.wasm` is built
      for wasm32-freestanding with its own page allocator (decision 6).
      `tinyactors_runtime.wasm` holds `libtinyactors` too, since a module
      cannot call into another's memory (decision 17's reasoning, which a
      combined module answers). Its C library is wasi-libc, compiled in
      (zig's wasm32-wasi target is only how it gets one), with no entry
      point; the release checks that it imports nothing, and allows its
      `_initialize` export, which runs the C library's constructors and
      which a host calls once first, as the web binding does. Its
      `ta_wasm_alloc` and `ta_wasm_free` are over malloc, in place of
      `libtinyactors`'s page allocator, which would claim the same memory.
    - **A `hosted` platform suffix** (decision 9) names files built for every
      target with a C library and not for freestanding wasm:
      `runtime_hosted.c`, the files and the clock. The header declares those
      functions under `#if !defined(__wasm__) || defined(__wasi__)`, and the
      API description marks them `hosted`.
    - **The release runs anywhere:** the packed-library tests run on this
      machine's target (they ran on x86_64 Linux only), and every native
      release target is built, `x86_64-macos` included.

20. **The ECMAScript data model ships as a library of its own**
    (2026-10-06), as a tree-sitter grammar ships beside the tree-sitter
    runtime: a program opts into it by linking it, and nobody builds
    QuickJS themselves. `libtinyactors_datamodel_ecmascript` is the
    data model with QuickJS inside, released with the other two
    (decision 19):
    - **It is built on `libtinyactors`'s public API alone**, as the runtime
      is, and keeps only its `ta_datamodel_ecmascript_*` API global: QuickJS's
      own symbols are local, so they cannot clash with a program's QuickJS.
    - **QuickJS compiles without C11 atomics** (`-D__STDC_NO_ATOMICS__=1`,
      `Core::QUICKJS_FLAGS`, in every build), so it leaves out `Atomics.wait`
      and calls no pthread function: a data model is single threaded, and a
      library that links no C library cannot count on libpthread.
    - **Its C library imports** (allocation, strings, the clock and local
      time, stdio for QuickJS's debugging dumps, and math) are listed in
      core's `release` task. glibc keeps the math functions in libm, so a
      program links `-lm` beside the libraries.
    - **Every tarball carries QuickJS's license** as `LICENSE.quickjs`
      (`CRelease::Library#notices`).
    - **Its browser module imports nothing either.** wasi-libc reaches the
      system through WASI functions it would import; the module defines
      them instead (`src/datamodel_ecmascript/browser_wasm.c`): files do not
      exist, and the clock is the time the host last set with
      `ta_datamodel_ecmascript_wasm_set_time` (milliseconds since the
      epoch; 0 until it is called), which Date, `performance.now()` and
      QuickJS's random seed read. A host passes `Date.now()` before each
      run. (zig 0.16 implements `close` in its own libc layer, whose
      `fd_close` import is defined there by its linker name; the release's
      import check catches a zig upgrade that adds another.)

## Still open

- **A signature on `SHA256SUMS`**, for example `ssh-keygen -Y sign` with
  the git signing key.

## Consequences

- `mise/lib/toolchain.rb` is the one place that knows targets and flags;
  `dist` becomes the host case of the release build.
- Each project's release task only describes the project (name, header,
  amalgamation, API description, symbol prefix, dependencies).
- A new libc call in the addon, or a new undefined symbol in a library,
  fails the release until it is checked and allowed.
