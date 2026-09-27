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
   option guarantees this on native targets; how to get there is still
   open (below).
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

## Still open

- **How the native libraries avoid `memcpy` and `memset`** (decision 8):
  change the code, define the functions under the library's names, or
  allow the four functions a freestanding C environment must provide. The
  proposal lists the measurements.
- **A signature on `SHA256SUMS`**, for example `ssh-keygen -Y sign` with
  the git signing key.

## Consequences

- `mise/lib/toolchain.rb` is the one place that knows targets and flags;
  `dist` becomes the host case of the release build.
- Each project's release task only describes the project (name, header,
  amalgamation, API description, symbol prefix, dependencies).
- A new libc call in the addon, or a new undefined symbol in a library,
  fails the release until it is checked and allowed.
