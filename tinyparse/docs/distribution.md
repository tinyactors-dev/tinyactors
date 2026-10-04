# tinyparse distribution

How `tinyparse` is packaged, released, and installed. Current version:
**0.1.0** (single source of truth: [`../VERSION`](../VERSION)).

The release artifacts are built by mise tasks in
[`../mise/tasks/`](../mise/tasks/), and `mise run test` depends on them: the
`dist/` cases under `test/suite/dist/` unpack and run what the tasks produced,
so a release tarball is the thing the suite tested.

## Building the artifacts

```sh
mise run dist         # dist/<target>/{bin,lib,include} for every release target
mise run amalgamate   # dist/amalgamation/
mise run release      # dist/release/*.tar.gz (runs the two above first)
```

`mise run dist` cross-builds both release targets, `x86_64-linux` and
`aarch64-macos`, from any host with the zig cc the monorepo pins
(`Toolchain::Target.release`). Then it checks what it built with
`Toolchain::Binary`:

- each file is for its target's architecture;
- `libtinyparse.a` leaves undefined only `memcpy`, `memset`, `memmove` and
  `memcmp`;
- the shared library exports exactly the 48 functions `include/tinyparse.h`
  declares with `TINYPARSE_API` (the library is compiled with
  `-fvisibility=hidden` and the macro set to default visibility), names itself
  by its versioned file, and loads no other library;
- the CLI loads no library beside the platform's runtime (none on Linux, where
  it is a static musl binary; libSystem on macOS).

What is shipped is compiled with `-g0`, and the CLI's debug info is stripped, so
no path of the build machine ends up in it.

The static archive keeps the internal `tp_*` functions global (with hidden
visibility), because the parsers `tinyparse c --parser-lib` generates call them.

## Artifacts

For release `X.Y.Z`, `mise run release` writes three tarballs into
`dist/release/`. They are deterministic: sorted entries, one mtime (the commit's),
owner 0, packed with the pinned GNU tar and gzip.

1. `tinyparse-X.Y.Z-x86_64-linux.tar.gz`
2. `tinyparse-X.Y.Z-aarch64-macos.tar.gz`

   Each unpacks to a prefix-shaped tree:

   ```
   tinyparse-X.Y.Z-<target>/
     bin/tinyparse                  the CLI
     lib/libtinyparse.a             static library
     lib/libtinyparse.so.0.1        shared library on Linux (libtinyparse.so links to it)
     lib/libtinyparse.0.1.dylib     shared library on macOS
     include/tinyparse.h            the public header
     share/man/man1/tinyparse.1     the manual page (mdoc)
     INSTALL.md                     how to install and link
   ```

3. `tinyparse-X.Y.Z-amalgamation.tar.gz`, the single-file distribution:

   ```
   tinyparse-X.Y.Z-amalgamation/
     tinyparse.h        the public header
     tinyparse.c        the whole library in one translation unit
     tinyparse_cli.c    the CLI, with the manual page, grammars and amalgamation inlined
     README.md          how to build the three files
   ```

   `tinyparse.c` is a concatenation in a fixed order: the public header,
   `src/internal.h`, then every library source but the CLI, with the `#include`
   lines of what it folds in removed. It needs a C11 compiler and the
   freestanding headers `stddef.h` and `stdint.h`, and the suite compiles it with
   `-std=c11 -Wall -Wextra -pedantic -Werror`
   (`dist/amalgamation-warnings`):

   ```sh
   cc -c tinyparse.c                                   # the library object
   cc tinyparse.c tinyparse_cli.c -o tinyparse         # the CLI
   cc -shared -fPIC -fvisibility=hidden tinyparse.c -o libtinyparse.so
   ```

   The amalgamation wraps itself in `#pragma GCC visibility push(default)`, so
   a shared library built from it exports the `tinyparse_*` API and the `tp_*`
   internals, and nothing else (`dist/amalgamation-exports`).

The CLI carries the amalgamation too: `tinyparse init` and `tinyparse vendor`
write `tinyparse.c` and `tinyparse.h` from bytes compiled into the binary.

## Installing

[`../install.sh`](../install.sh) detects the OS and architecture, downloads the
matching tarball from a GitHub release, and copies the tree into a prefix
(`$HOME/.local` by default). `--from-file TARBALL` installs a local tarball with
no network access:

```sh
sh install.sh --from-file dist/release/tinyparse-0.1.0-x86_64-linux.tar.gz --prefix "$HOME/.local"
```

Or unpack a tarball and copy its tree by hand, as the tarball's `INSTALL.md`
says. For the amalgamation, drop `tinyparse.c` and `tinyparse.h` into a project.

## Not wired yet

Nothing publishes a release. `mise run release` builds the tarballs and stops.
Three things are missing before it can:

- **A tag scheme.** `install.sh` downloads from a release tagged
  `tinyparse-v<version>`, the scheme pocketparse used in ax. The monorepo tags
  components as `<component>/v<version>` (`tinyxml/v0.1.0`), so one of the two
  has to change.
- **A changelog.** A component release in this monorepo needs a `CHANGELOG.md`
  section that `mise run changelog:check` accepts, and tinyparse is not yet in
  `Changelog::COMPONENTS` (root `mise/lib/changelog/components.rb`).
- **CI.** pocketparse had a GitHub workflow that ran its gates on each push and
  published on a tag. The migration deleted it, since GitHub reads only the
  repository root's `.github/`; `mise run test` at the root now runs
  tinyparse's tests, and nothing runs the release on a tag.
