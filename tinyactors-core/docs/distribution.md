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
release-changelog skill; a development build does not. Publishing needs
`NPM_TOKEN` in fnox, a token that bypasses two-factor authentication.

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
