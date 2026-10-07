# Distribution: build artifacts, then install them

## Built today: @tinyactors/node

The Node.js binding ships on npm as `@tinyactors/node`, with one package
per platform, as [ADR 0001](../../docs/adr/0001-packaging-and-releases.md)
(decisions 10, 11, 15 and 18) lays out. The tasks, in `tinyactors-core`:

| Command | What it does |
|---|---|
| `mise run node:build --target <t>` | The addon for a release target (`aarch64-macos`, `x86_64-macos`, `x86_64-linux`, `aarch64-linux`), by the pinned `zig cc`, into `build/node/<t>/tinyactors.node`; any host builds every target |
| `mise run node:symbols` | Checks each built addon: its architecture, that it exports only the Node-API entry points, imports only Node-API and an allowlist of libc functions, and on Linux needs no library |
| `mise run node:package` | Builds and checks every addon, and packs the five packages into `build/node/npm/tarballs/`; `--dev` stamps HEAD's development version, `--version` another |
| `mise run node:smoke` | Installs the tarballs into a fresh project with npm and with Bun, and runs `bindings/node/test/smoke/smoke.js` |
| `mise run node:smoke-containers` | The same in Docker: Node on glibc and musl, x64 and arm64, and Bun |
| `mise run node:release -- <version>` | All of the above after the release checks, as a dry run; `--publish` publishes, commits the version, tags `tinyactors-napi/v<version>` and creates the GitHub release |
| `mise run node:release -- --dev --publish` | Publishes HEAD as `<next minor>-dev.<date>.<n>.g<sha>` under the `next` dist-tag |

`@tinyactors/node` holds `bindings/node/` and `bindings/shared/` as
`node/` and `shared/`, so its relative imports are the repository's, and
depends on every platform package at exactly its own version. The version
is `bindings/node/package.json`'s; a release commits the new one. A release
needs a `CHANGELOG.md` section in `bindings/node/` (component `napi`,
`mise run changelog:check napi <version>`), written with the
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

## Built today: the C libraries

Core ships three C libraries, released together under one tag
(`tinyactors-core/v<version>`) and version (`TA_VERSION_*` in
`include/tinyactors/types.h`), as [ADR 0001](../../docs/adr/0001-packaging-and-releases.md)
decisions 19 and 20 lay out:

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

Each library's release assets, in `build/release/<version>/assets/`:

| Asset | Contents |
|---|---|
| `<library>-<version>-src.tar.gz` | its header, amalgamation and platform amalgamations (`_hosted.c`, `_wasm.c`), and API description |
| `<library>-<version>-<target>.tar.gz`, for `aarch64-macos`, `x86_64-macos`, `x86_64-linux`, `aarch64-linux` | `include/<library>.h`, `lib/lib<library>.a` and its shared library (`.so.<major>.<minor>` with a `.so` link, or `.<major>.<minor>.dylib`) |
| `<library>-<version>-wasm32.tar.gz` | `<library>.wasm`, which imports nothing, and its headers |
| `<library>-<version>.json` | the manifest: commit, zig, flags and every file's hash |

plus one `SHA256SUMS`. A Linux library links no C library, so one serves
glibc and musl; the runtime's and the data model's shared libraries name
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

The tasks, in `tinyactors-core`:

| Command | What it does |
|---|---|
| `mise run amalgamate` | `dist/<library>.h`, `.c` and `_<platform>.c`, each compiled on its own |
| `mise run api [--check]` | `include/<library>.api.json`: what each library exports, and what the changelog compares |
| `mise run wasm:test` | Builds the three wasm modules as the release does and runs `tests/wasm/release_test.mjs` against each, instantiated with no imports as a browser would |
| `mise run release -- <version> --build-only` | Only build, verify and pack (no checks, no version set): a look at the packaging before a release |
| `mise run release -- <version>` | The release as a dry run: the checks (core's whole test task), every target of the three libraries, their symbol checks, the smoke programs (`tests/release/`) and `test_runtime` and `test_e2e` against the packed libraries of this machine, the smoke programs in Linux containers, the wasm tests, and the assets |
| `mise run release -- <version> --publish` | The same, then the commit `release(tinyactors-core): v<version>`, the tag and the GitHub release |

A release needs a section in `tinyactors-core/CHANGELOG.md` (component
`core`), written with the release-changelog skill.

**The rest: not built at the moment.** The pipeline this document describes
(`tools/dist.py`, `tools/dist/`, the npm package's source fallback) built
every artifact with CMake and its Zig toolchain file. It was removed when
the build moved to the repository's Ruby toolchain, and is kept at the tag
`standalone-import`. It is to be rebuilt on the repository's release
machinery ([ADR 0001](../../docs/adr/0001-packaging-and-releases.md):
`Toolchain::Target.release`, `mise/lib/release.rb`), which already builds
prebuilt addons with no compiler on the user's machine. Until then, only
`mise run amalgamate` works. The rest of this document is what the rebuilt
pipeline should produce, as the old one did.

From the repository root, with the pinned mise toolchain installed:

| Command | Artifacts under `dist/` |
|---|---|
| `mise run dist:node` | `tinyactors-0.1.0.tgz`: npm package, nine N-API 8 addons, source fallback |
| `mise run dist:web` | `tinyactors-web-0.1.0.tgz`: npm package, JS + wasm |
| `mise run dist:c` | `tinyactors-amalgamation-0.1.0.tar.gz`, `tinyactors-source-0.1.0.tar.gz`, nine `tinyactors-c-0.1.0-<target>.tar.gz` install prefixes |
| `mise run dist:pg` | `tinyactors-0.1.0.zip`: PGXN/PGXS source distribution |
| `mise run dist` | All of the above |
| `mise run amalgamate` | Standalone `dist/tinyactors.c` + `dist/tinyactors.h` (also checks compilation) |
| `mise run dist:test` | Builds artifacts, installs in clean directories outside the repo, tests them |

These commands **do not publish**. `VERSION` is the release version source
of truth. The builder stamps npm manifests, PG control/SQL/META filenames,
CMake/pkg-config metadata, archive names and amalgamation banners from it.
Versions in the bindings' development manifests/control file are not release
inputs. Every archive contains `VERSION`, `LICENSE` and `distribution.json`
(version, license, ABI from `TA_ABI_VERSION`, targets; N-API level for Node).
QuickJS's license accompanies its sources and static libraries.

## Native target matrix

`tools/dist.py` owns this matrix; every binary is built with the existing Zig
CMake toolchain, including the host binary. No host-compiled addon is reused.

| Package target | Zig target | Minimum/runtime constraint |
|---|---|---|
| `linux-x64-gnu` | `x86_64-linux-gnu.2.28` | glibc ≥ 2.28 |
| `linux-arm64-gnu` | `aarch64-linux-gnu.2.28` | glibc ≥ 2.28 |
| `linux-x64-musl` | `x86_64-linux-musl` | musl |
| `linux-arm64-musl` | `aarch64-linux-musl` | musl |
| `linux-arm-musl` | `arm-linux-musleabihf` | 32-bit ARM hard float, musl |
| `darwin-x64` | `x86_64-macos.12.0` | macOS ≥ 12 |
| `darwin-arm64` | `aarch64-macos.12.0` | macOS ≥ 12 |
| `win32-x64` | `x86_64-windows-gnu` | Windows x64 |
| `win32-arm64` | `aarch64-windows-gnu` | Windows ARM64 |

Node additionally requires Node ≥ 22.12 for synchronous `require(ESM)`.
N-API 8 avoids a binary per Node major; it does not make binaries portable
between OSes, architectures or C libraries. Windows builds download the
installed Node version's official `node.lib` import libraries and verify
them against that release's `SHASUMS256.txt`; nothing is downloaded at npm
install time on a prebuilt target. Distribution builds need Internet on
their first Windows import-library download.

## npm: the web package

The Node.js package is built as above, not as this section describes: per
platform packages replaced the bundled prebuilts and the source fallback
(ADR 0001, decision 11). What follows is still the plan for the web package.

### The plan, as the old pipeline built it

```sh
mise run dist:node
npm install /absolute/path/to/dist/tinyactors-0.1.0.tgz
node -e "const ta = require('tinyactors'); console.log(typeof ta.createMachine)"

mise run dist:web
npm install /absolute/path/to/dist/tinyactors-web-0.1.0.tgz
```

Both packages preserve `bindings/shared/` **inside** their package root.
The web package also includes the CommonJS Node transports and WebSocket
implementation, with an explicit CommonJS package scope. There are no npm
runtime dependencies. `build/dist-stage/{node,web}` are also installable
package directories (use `npm install --install-links <directory>` to copy
rather than symlink). Do not publish `bindings/node` or `bindings/web`
directly; those are development layouts.

The Node tarball uses bundled prebuilts plus a small install script. This
follows the [Node-API prebuilt model](https://nodejs.org/api/n-api.html#uploading-precompiled-binaries):
one addon per platform, stable across supported Node majors. Bundling all
targets costs download bytes but permits offline installs, one publishable
artifact and no optional-dependency/registry ordering issues. Per-platform
npm packages are not justified at this size. The installer detects glibc vs
musl with Node's report API, copies the selected binary, or compiles the
vendored sources using CMake. The source fallback includes the whole C build
description and QuickJS sources/licenses (QuickJS is not linked into Node).
The freestanding amalgamation is included for embedders; the fallback uses
layered sources because it also needs the compiler and hosted layer.

To force the tested fallback:

```sh
npm_config_build_from_source=true npm install ./tinyactors-0.1.0.tgz
```

Fallback prerequisites: CMake ≥ 3.25, a C11 compiler and its build tool,
Node headers. Defaults assume headers at `../../include/node` relative to
`node`; set `NODE_INCLUDE_DIR` for distro-managed Node. On Windows also set
`NODE_LIBRARY` to the target's `node.lib`. Missing build tools/headers fail
the install rather than silently using a stale binary. npm lifecycle scripts
must be enabled for native installation. `TINYACTORS_NATIVE` remains a
development override, not a dependency of the release package.

The web package exports `tinyactors-web` conditionally: `node.mjs` in Node,
`index.mjs` in browsers. `tinyactors-web/browser` explicitly selects the
browser entry. With an unbundled static server:

```js
import { createMachine, createActor } from
  './node_modules/tinyactors-web/bindings/web/index.mjs';
```

Serve the **whole** package, not just `index.mjs`; the entry locates
`tinyactors.wasm` relative to itself, and imports shared JS. Serve wasm as
`application/wasm` for streaming compilation (the byte fallback also works).
Bundlers must preserve/copy that wasm URL. It is a JS-hosted reactor with
its own adapter, not a standalone WASI executable.

## C: install prefixes and an embeddable source release

The C libraries are built as above, two libraries without the CMake and
pkg-config files this section describes; what follows is the plan the old
pipeline built.

The amalgamation is the freestanding runtime (base/image/core/system),
including the null datamodel, addresses and frames. It deliberately omits
the hosted compiler, runtime conveniences, host and QuickJS. Copy both files
and compile `cc -std=c11 -c tinyactors.c`. Compile charts separately or use the full
source/static-library release when compilation or ECMAScript is needed.

Extract a target archive anywhere. The result is a relocatable install
prefix, with all public headers, static archives, licenses, CMake config and
pkg-config metadata. In a consumer:

```cmake
find_package(tinyactors 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE tinyactors::runtime tinyactors::datamodel_ecmascript)
```

Pass `-DCMAKE_PREFIX_PATH=/path/to/extracted-prefix`. Per-layer targets retain
their transitive dependencies; `tinyactors::system` does not pull in QuickJS.
`tinyactors::tinyactors` (`libtinyactors.a`) is every layer in one archive.
Alternatively, set `PKG_CONFIG_PATH=<prefix>/lib/pkgconfig` and use
`pkg-config --cflags --libs --static tinyactors` (`-ltinyactors`: every
layer in one archive). When cross-compiling, use `PKG_CONFIG_LIBDIR` to isolate
target metadata and a matching compiler target. Windows archives target the
GNU ABI/toolchain; do not assume MSVC archive compatibility.

The source archive has the same CMake install rules and a Conan 2 recipe:

```sh
tar xf tinyactors-source-0.1.0.tar.gz
cd tinyactors-source-0.1.0
conan profile detect                 # once per user/toolchain
conan create . --build=missing       # local Conan cache, no upload
conan install --requires=tinyactors/0.1.0 -g CMakeToolchain -g CMakeDeps --output-folder=deps
```

Conan uses the shipped CMake targets (`find_package(tinyactors)`), not a second
hand-maintained layer graph. A vcpkg port can use `vcpkg_cmake_configure`,
`vcpkg_cmake_install` and `vcpkg_cmake_config_fixup(PACKAGE_NAME tinyactors)` on this
source archive with `TA_BUILD_TESTS=OFF`, `TA_BUILD_TOOLS=OFF`. A public
vcpkg/Conan registry entry is a publishing concern; none is claimed here.

## PostgreSQL: PGXN source rather than ABI-fragile binaries

The ZIP follows the [PGXN metadata specification](https://pgxn.org/spec/)
and has a top-level `META.json`, PGXS `Makefile`, control file, SQL, tests,
and vendored C/QuickJS sources and amalgamation. PostgreSQL 17 is the tested
major. No per-major binary compatibility is promised: build against the
server's **own** PGXS and headers. Source packaging avoids multiplying
artifacts by PG major, OS, architecture and distro; PGXN clients invoke the
standard PGXS build/install targets.

```sh
unzip tinyactors-0.1.0.zip
cd tinyactors-0.1.0
make PG_CONFIG=/path/to/pg_config
make PG_CONFIG=/path/to/pg_config install
psql -c 'CREATE EXTENSION tinyactors;'
make PG_CONFIG=/path/to/pg_config installcheck
```

Requires CMake, a C compiler/build tool and PG server development headers.
For cross builds supply the **target** `PG_CONFIG`/headers, `CC='zig cc
-target <triple>'`, and `CMAKE_ARGS='-DCMAKE_TOOLCHAIN_FILE=<absolute
path>/vendor/cmake/toolchains/zig.cmake -DTA_ZIG_TARGET=<triple>'`. Zig does
not supply PostgreSQL's headers or PGXS. `dist:pg` creates sources only;
the installation test builds both extension and libraries with Zig.

## Incrementality and the completion gate

Mise `depends` connects only needed inputs: Node/C/PG depend on amalgamation;
web depends on `web:build`. `sources`/`outputs` include the version, packaging
scripts, manifests, source/header/build files and vendored dependencies.
Every expected versioned archive is an explicit output. A leaf never runs
the general debug build, another binding's tests, or the full test suite.
CMake/Ninja reuse object files when a changed input causes repackaging.
An unchanged second `mise run dist` skips all producers, including wasm and
amalgamation. Tests deliberately always run.

`tools/dist/test.py`, wired into `tools/gate.py` and completion gate 23,
checks actual installed consumers, not just archive manifests:

- npm install from copied tarballs; run a machine through initial, assigned
  context and final output, for native, forced source fallback and wasm;
- all nine addon formats/architectures, native selection byte-for-byte;
- installed wasm through Chromium/agent-browser with asserted DOM output;
- standalone amalgamation compilation, CMake and pkg-config consumer links
  for all nine targets, native execution with the ECMAScript datamodel,
  Conan 2.22 create/install and a CMake consumer from its isolated cache;
- PGXS install into a **copied** PG toolchain, fresh `initdb`, SQL smoke
  machine and the eight regression suites; no shared database is touched;
- unchanged artifact hashes **and mtimes** after a second `dist` invocation.

Only native Linux execution is proved in this Linux orb. Other platforms'
format/architecture and link checks are not claims of runtime testing there.
Registry uploads, npm access/provenance, PGXN accounts, release signing,
checksums for published downloads, CI release jobs and release tags remain
publishing-workflow work; artifact production needs none of those credentials.
