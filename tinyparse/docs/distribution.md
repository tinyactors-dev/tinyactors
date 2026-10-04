# tinyparse distribution

How `tinyparse` is packaged, released, and installed. Current release:
**0.1.0** (single source of truth: [`../VERSION`](../VERSION)).

The releasable artifacts are produced by scripts under `tools/tinyparse/`,
which are the *tested* deliverables (the data tests under `test/suite/dist/`
exercise them). CI is a thin workflow that runs those same scripts on a tagged
build and attaches their output to a GitHub Release — so what CI ships is
exactly what the metric proves locally.

## Artifacts

For release `X.Y.Z`, three artifacts:

1. `tinyparse-X.Y.Z-linux-x86_64.tar.gz`
2. `tinyparse-X.Y.Z-macos-arm64.tar.gz`

   Each platform tarball unpacks to a prefix-shaped tree:

   ```
   tinyparse-X.Y.Z-<platform>/
     bin/tinyparse                  the CLI
     lib/libtinyparse.a             static library
     lib/libtinyparse.{so,dylib}    shared library
     include/tinyparse.h            the public header
     share/man/man1/tinyparse.1     the manual page (mdoc)
     INSTALL.md                       a short install note
   ```

3. `tinyparse-X.Y.Z-amalgamation.tar.gz` — the single-file distribution:

   ```
   tinyparse-X.Y.Z-amalgamation/
     tinyparse.h        the public header (no ax visible)
     tinyparse.c        the whole library, with ax.c folded in and hidden
     tinyparse_cli.c    the CLI (with the manpage embedded)
     README.md
   ```

   `tinyparse.c` is built by a dumb concatenation script
   ([`../amalgamate.sh`](../amalgamate.sh)): it inlines `ax.h`/`ax.c` and every
   library source into one translation unit. The folded `ax` code is wrapped in
   `#pragma GCC visibility push(hidden)` … `pop`, so a shared library built from
   the amalgamation exports only `tinyparse_*` symbols — `ax` is an
   implementation detail, invisible to users of the library, who only ever see
   `tinyparse.h`. Building it is as simple as:

   ```sh
   cc -c tinyparse.c                       # the library object
   cc tinyparse.c tinyparse_cli.c -o tinyparse   # the CLI
   cc -shared -fPIC -fvisibility=hidden tinyparse.c -o libtinyparse.so
   ```

The cross-build for both platforms runs on a single Linux host with Zig 0.15.1
(`build.sh dist`); the macOS artifacts are Zig-cross Mach-O.

## Release scripts

| Script | Role |
|--------|------|
| [`../amalgamate.sh`](../amalgamate.sh) | concatenate sources → `dist/amalgamation/{tinyparse.c,tinyparse.h,tinyparse_cli.c}` |
| [`../release.sh`](../release.sh) | build `dist/`, then assemble all three tarballs into `dist/release/` |
| [`../install.sh`](../install.sh) | download the matching tarball from the latest GitHub Release and install into a prefix (`--from-file` and `--prefix` for offline/custom installs) |

## CI

`.github/workflows/tinyparse.yml`, scoped to `tools/tinyparse/**`:

- **on push to `tools/tinyparse/**`** (and PRs): build + run `ci/tinyparse-metric.sh` (the gate).
- **on tag `tinyparse-v*`** (and `workflow_dispatch`): provision Zig 0.15.1,
  run `release.sh`, and publish a GitHub Release with the three tarballs +
  amalgamation attached as assets.

## Installing on a laptop

```sh
# one-liner: detect OS/arch, fetch the latest release tarball, install to ~/.local
curl -fsSL https://raw.githubusercontent.com/dhamidi/ax/main/tools/tinyparse/install.sh | sh

# or grab a tarball from the Releases page and extract it into a prefix
tar xzf tinyparse-0.1.0-macos-arm64.tar.gz
sudo cp -r tinyparse-0.1.0-macos-arm64/* /usr/local/

# or the amalgamation: drop tinyparse.c + tinyparse.h into your project
```
