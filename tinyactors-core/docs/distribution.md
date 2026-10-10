# Distribution: what is released, and what is planned

Two releases are built today, each by a task in `tinyactors-core`, as
[ADR 0001](../../docs/adr/0001-packaging-and-releases.md) lays out:

- **`@tinyactors/node`**, the Node.js binding on npm, tagged
  `tinyactors-napi/v<version>` (`mise run node:release`);
- **the C libraries** libtinyactors, libtinyactors_runtime and
  libtinyactors_datamodel_ecmascript, released together on GitHub and
  tagged `tinyactors-core/v<version>` (`mise run release`).

Version 0.2.0 of both was released on 2026-10-06.

Two packages are not built yet: the web npm package and the PostgreSQL
source archive. Their sections at the end are the plan, not a description
of anything that exists.

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

## @tinyactors/node

The Node.js binding ships on npm as `@tinyactors/node`, with one package
per native target (decisions 10, 11, 15 and 18). The tasks:

| Command | What it does |
|---|---|
| `mise run node:build --target <t>` | The addon for a native release target, by the pinned `zig cc`, into `build/node/<t>/tinyactors.node`; without `--target`, the addon for this machine, into `bindings/node/build/tinyactors.node` |
| `mise run node:symbols` | Checks each built addon: its architecture, that it exports only the Node-API entry points, imports only Node-API and an allowlist of libc functions, and on Linux needs no library |
| `mise run node:package` | Builds and checks every addon, and packs the five packages into `build/node/npm/tarballs/`; `--dev` stamps HEAD's development version, `--version` another, `--targets` packs a subset |
| `mise run node:smoke` | Installs the tarballs into a fresh project with npm and with Bun, and runs `bindings/node/test/smoke/smoke.js` |
| `mise run node:smoke-containers` | The same in Docker: Node on glibc and musl, x64 and arm64, and Bun |
| `mise run node:release -- <version>` | A dry run of the release: the checks (a clean `main` in step with `origin`, a free tag, the changelog section, no such version on npm, `node:test`), then `node:package`, both smoke tests, and the notes and `SHA256SUMS` in `build/node/npm/release/`; `--skip-containers` leaves out the Docker smoke tests |
| `mise run node:release -- <version> --publish` | The same, then the commit `release(napi): v<version>`, the tag `tinyactors-napi/v<version>`, the npm publish, the push and the GitHub release |
| `mise run node:release -- --dev --publish` | Publishes HEAD as `<next minor>-dev.<date>.<n>.g<sha>` under the `next` dist-tag; no commit, tag or GitHub release |

The packages are `@tinyactors/node` and `@tinyactors/node-<os>-<cpu>` for
`darwin-arm64`, `darwin-x64`, `linux-x64` and `linux-arm64`. A platform
package holds its target's addon as `tinyactors.node`, with `os` and `cpu`
fields and no `libc` field. `@tinyactors/node` holds `bindings/node/` and
`bindings/shared/` as `node/` and `shared/`, so its relative imports are
the repository's, and names every platform package as an optional
dependency at exactly its own version. It needs Node.js 22.12 or later. The
version is `bindings/node/package.json`'s; a release commits the new one. A
release needs a `CHANGELOG.md` section in `bindings/node/` (component
`napi`, `mise run changelog:check napi <version>`), written with the
release-changelog skill; a development build does not.

### Publishing: with a token, or with your own npm login

**With a token (CI, or any machine without a person at it).** `node:release
-- <version> --publish` publishes with `NPM_TOKEN`, which `fnox exec` puts
in npm's environment; on a CI runner, set `NPM_TOKEN` as a secret the same
way. The token must be allowed to publish without two-factor
authentication (a granular access token with "bypass 2FA" for the
`@tinyactors` scope): npm cannot ask a CI job for a one-time password.
`--dev --publish` works the same way for the `next` dist-tag.

**With your own npm login (a maintainer at a terminal, no token).** npm
restricts tokens that bypass two-factor authentication, so from a laptop
you may publish with `npm login` instead, approving each publish in the
browser. The release task cannot do this for you, since it publishes with
the token, so run its dry run and then the publishing steps yourself
(this is how 0.2.0 was released):

1. Write and commit the changelog (release-changelog skill), and push
   `main`: the release task refuses a tree that is not in step with
   `origin/main`.
2. `mise run node:release -- <version>` (in `tinyactors-core`): the checks,
   the four addons, the five tarballs at `<version>`, the smoke tests here
   and in containers, and `build/node/npm/release/notes.md` and
   `SHA256SUMS`. Nothing is committed or published.
3. Set `"version"` to `<version>` in `bindings/node/package.json` and in the
   first two `"version"` fields of `bindings/node/package-lock.json`, commit
   them as `release(napi): v<version>`, and tag that commit:
   `git tag --annotate --cleanup=verbatim tinyactors-napi/v<version> --file build/node/npm/release/notes.md`
   (path relative to `tinyactors-core`). The tarballs do not change: they
   already carry `<version>`.
4. `npm login`, then publish the platform packages before the main package
   that depends on them, from `tinyactors-core/build/node/npm/tarballs/`:

   ```sh
   for t in darwin-arm64 darwin-x64 linux-x64 linux-arm64; do
     npm publish tinyactors-node-$t-<version>.tgz --access public --tag latest
   done
   npm publish tinyactors-node-<version>.tgz --access public --tag latest
   ```

   npm may ask you to approve the first publish in the browser. It then
   reports "being processed": a new version can take ten minutes before
   `npm install` finds it (`npm install --prefer-online` skips npm's own
   cache of the old list).
5. `git push origin main tinyactors-napi/v<version>`, then create the GitHub
   release from `tinyactors-core`:
   `gh release create tinyactors-napi/v<version> --verify-tag --title "@tinyactors/node <version>" --notes-file build/node/npm/release/notes.md build/node/npm/tarballs/*-<version>.tgz build/node/npm/release/SHA256SUMS`

If a publish fails part way, publish the remaining tarballs in the same
order before pushing; npm does not let a version be published twice, so
never repack a version that is already partly on npm.

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
one (the C library's constructors, as the web binding does), place bytes
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

## Planned: the web npm package

Not built. `mise run web:build` builds `bindings/web/tinyactors.wasm` and
`mise run web:test` tests it, but no task packs or publishes a package.
The pipeline that built one before (`tools/dist.py`, with CMake and Zig)
was removed when the build moved to the repository's Ruby toolchain. What
follows is the plan, as that pipeline built the package.

The package keeps `bindings/shared/` **inside** its package root, so its
relative imports are the repository's, and holds the CommonJS Node
transports and WebSocket implementation, with an explicit CommonJS package
scope. It has no npm runtime dependencies. Do not publish `bindings/web`
directly; it is a development layout.

The package's main export is conditional: `node.mjs` in Node, `index.mjs`
in browsers. With an unbundled static server:

```js
import { scxml, createSystem, createActor } from
  './node_modules/tinyactors-web/bindings/web/index.mjs';
```

Serve the **whole** package, not just `index.mjs`; the entry locates
`tinyactors.wasm` relative to itself, and imports shared JS. Serve wasm as
`application/wasm` for streaming compilation (the byte fallback also works).
Bundlers must preserve or copy that wasm URL. It is a JS-hosted reactor
with its own adapter, not a standalone WASI executable.

## Planned: the PostgreSQL source archive

Not built. `mise run pg:build` builds the extension with PGXS against the
position-independent layers (`build/addon/lib`) and installs it into the
pinned PostgreSQL 17, and `mise run pg:test` tests it, but no task packs a
source archive. What follows is the plan.

A ZIP that follows the [PGXN metadata specification](https://pgxn.org/spec/),
with a top-level `META.json`, the PGXS `Makefile`, the control file, SQL,
tests, and the C sources it builds from, QuickJS's included. PostgreSQL 17
is the tested major. No per-major binary compatibility is promised: the
extension is built against the server's **own** PGXS and headers. A source
archive avoids one binary per PG major, OS, architecture and distribution;
PGXN clients run the standard PGXS targets.

```sh
unzip tinyactors-<version>.zip
cd tinyactors-<version>
make PG_CONFIG=/path/to/pg_config
make PG_CONFIG=/path/to/pg_config install
psql -c 'CREATE EXTENSION tinyactors;'
make PG_CONFIG=/path/to/pg_config installcheck
```

It needs a C compiler and the server's development headers. For a cross
build, supply the **target** `PG_CONFIG` and headers; zig does not supply
PostgreSQL's headers or PGXS.
