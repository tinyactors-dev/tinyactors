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
      cost first.
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
