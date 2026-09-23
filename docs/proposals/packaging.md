# Packaging and releases

**Status:** Sketch (2026-09-24)
**Touches:** tinyactors-napi (package layout, loader), tinyactors-core (release artifacts), root `mise/tasks` (release tasks)
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

In a Node or Bun project, one command installs Tinyactors and it works:

```sh
npm install tinyactors      # or: bun add tinyactors / pnpm add tinyactors
```

No compiler, no `node-gyp`, no Python, no postinstall download. The
release is built on the maintainer's Mac or a Linux VM with one command,
and published to npm and to GitHub Releases.

## What exists today

- The addon is built by `tinyactors-napi/mise/tasks/build` with the
  system `cc`, against the Node headers of the mise-installed Node, with
  `-DNAPI_VERSION=10`. The result is `build/tinyactors_napi.node`, loaded
  by `lib/native.js` from that fixed path.
- `package.json` is named `tinyactors-napi`, version `0.0.0`, and lists
  the one `.node` file for the machine that built it.
- Core publishes `dist/` (header, amalgamation, API description, static
  library) for consumers inside the repo.
- The names `tinyactors`, `tinyactors-napi` and `@tinyactors/core` are
  free on npm (checked 2026-09-24). The GitHub repository is private.

## Checked on 2026-09-24

From the Mac, `zig cc` (0.16.0, already installed through mise) builds
the addon, all of `src/**/*.c` plus the core amalgamation, as one shared
library for five targets, with no other toolchain:

| Target | Result |
|---|---|
| `aarch64-macos` | builds; 449 KB unstripped |
| `x86_64-macos` | builds; 425 KB |
| `x86_64-linux-gnu.2.28` | builds; 1.75 MB with debug info |
| `aarch64-linux-gnu.2.28` | builds; 1.74 MB with debug info |
| `x86_64-linux-musl` | builds; 1.75 MB with debug info |
| `aarch64-linux-musl` | not tried |

macOS links with `-undefined dynamic_lookup`, since Node-API symbols come
from the host process. Linux needs no flag. The `aarch64-macos` build
loads in Node and in Bun 1.4.1, and all 14 napi test files pass under
both with it. The Linux builds were not loaded yet: no Linux VM was
running.

## Proposed shape

### npm packages

The pattern esbuild, SWC and napi-rs use: one main package, and one small
package per platform holding only the binary.

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
  `cpu` and `libc` match, so a user downloads one binary.
- **The loader** (`lib/native.js`) picks the package for
  `process.platform`, `process.arch` and the libc (musl is detected from
  `process.report`), and falls back to `build/tinyactors_napi.node` so
  development in the repo keeps working. When nothing matches, it throws
  one error naming the platform it looked for and the supported ones.
- **Node-API is ABI-stable**, so one binary per platform serves every
  Node version that has Node-API 10 (Node 22.14 and later; `engines`
  says `>=22` and should say `>=22.14`), and Bun. No per-Node-version builds.
- **Windows** is left out of the first release. Linking an addon there
  needs Node's import library; zig can produce it, but it needs a test
  machine first.
- A scope needs an npm organization, `tinyactors`, created once.

### GitHub Releases

Each release tag `vX.Y.Z` gets:

- the six `.node` binaries, named by target;
- a C tarball, `tinyactors-c-X.Y.Z.tar.gz`: `tinyactors.h`,
  `tinyactors.c` (the amalgamation), `tinyactors.api.json`, and
  `libtinyactors.a` per target, for embedders who are not using Node;
- `SHA256SUMS`;
- notes from the Conventional Commits since the last tag.

While the repository is private, release assets are not public. npm is
where users get the binaries, so that does not block the first goal; it
matters once someone downloads the C tarball.

### Versions

- One version for everything in a release: the main package, the
  platform packages, and the C tarball. The main package pins its
  platform packages to exactly that version.
- `0.x` while APIs change freely (root AGENTS.md: the libraries are
  private). A minor bump for any breaking change, a patch for the rest.
- `version()` in the addon (`TINYACTORS_NAPI_VERSION`) reports the
  package version. Core has no version function today; a `ta_version()`
  would let the smoke test check that the binary and the JavaScript
  match.

### The release command

```sh
mise run release 0.1.0            # build, test, pack; publishes nothing
mise run release 0.1.0 --publish  # the same, then npm and GitHub
```

It runs on macOS or Linux, arm64 or x64, and does the same thing on each,
because every binary is cross-compiled:

1. **Check:** a clean tree on `main`, the tag doesn't exist, `mise run
   test` passes.
2. **Build:** `zig cc` for every target, in release flags (`-O2`,
   stripped, debug info kept aside as a separate file per target). The
   Node-API headers come from the pinned npm package `node-api-headers`
   under `vendor/`, not from whichever Node mise installed, so the
   build does not depend on the machine.
3. **Pack:** `npm pack` each package into `dist/release/X.Y.Z/`, and
   build the C tarball and `SHA256SUMS`.
4. **Smoke test the packed tarballs**, not the tree: in a temporary
   project, install the main tarball and the host's platform tarball with
   npm, then with Bun, and run a short script that compiles a chart,
   spawns an actor, sends an event and checks the result. On a Mac, run
   the same smoke test for the Linux binaries in a container (Docker,
   Colima and Lima are installed), for glibc and for musl.
5. **Publish** with `--publish`: platform packages first, then the main
   package, then the git tag and the GitHub release (`gh release
   create`).

**Secrets.** The npm token and the GitHub token come from fnox (global
agent rule); the task never reads or prints them, and no `.npmrc` with a
token is written to the tree.

**Reproducible enough.** Pinned zig, pinned Node-API headers and fixed
flags mean a Mac and a Linux VM produce equivalent binaries. Byte-for-byte
reproducibility is not a goal of the first release.

### Later: CI

npm provenance and trusted publishing (no long-lived token) need GitHub
Actions' OIDC. The release task is written so a workflow can run it
unchanged on a Linux runner. Moving there is a later step; the first
releases are local, as asked.

## Open questions

- **Package name.** `tinyactors` for the Node package,
  or `@tinyactors/node`, keeping `tinyactors` for
  a future package that picks Node or the browser build?
- **Bun and Node-API 10.** The test suite passes under Bun 1.4.1 today,
  but Bun's Node-API support has gaps. The release should run the whole
  napi suite under Bun, not just the smoke test.
- **Oldest glibc.** 2.28 covers Debian 10, RHEL 8 and Ubuntu 18.10 and
  later. Going older costs nothing with zig; is 2.17 (RHEL 7) wanted?
- **Does the ECMAScript or jq data model ship?** They live in the test
  host. The npm package uses JavaScript values through napi's own data
  model, so neither is needed for Node users.
- **Public repository.** Needed before the C tarball and release notes
  are useful to anyone outside.
- **Relation to the browser build.** A WebAssembly package
  (`@tinyactors/wasm`) could later be the fallback when no native
  package matches, the way esbuild falls back to `esbuild-wasm`.
