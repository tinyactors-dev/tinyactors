# Install tinyparse

This guide is for a developer who wants the `tinyparse` command and the C
library on their machine or inside their project. Each section below is one
route, and each ends with a check that the route worked. What every artifact
contains is listed in [Release artifacts](../reference/artifacts.md).

A generated parser (`tinyparse c --parser-lib`) includes a private header of
the source tree and calls functions the shared library does not export, so
only a source tree builds one; [Ship a generated C
parser](generated-parser.md) covers what it needs.

## Prerequisites

Pick a route; each needs different tools:

| Route | What you get | What you need |
|-------|--------------|---------------|
| Release tarball | The command, `libtinyparse.a`, the shared library, `tinyparse.h` and the manual page, for x86-64 Linux or Apple silicon macOS | `tar`, `gzip` and a tarball for your target |
| install.sh | The same tree, copied into a prefix by a script | A POSIX `sh` and `tar`; `curl` or `wget` to download |
| Source tree | The command and `libtinyparse.a` in `build/`, the examples, and generated parsers | `git` and [mise](https://mise.jdx.dev), which installs the pinned clang 22, zig and Ruby |
| Amalgamation | `tinyparse.h`, `tinyparse.c` and `tinyparse_cli.c`, to compile yourself | A C11 compiler |
| `tinyparse init` and `tinyparse vendor` | `init`: a starter project, with the amalgamation unless `--lib` is given. `vendor`: the amalgamation, or with `--lib` only the header | A `tinyparse` command from another route and a C compiler |

A tarball comes from a GitHub release of the repository or from a source
tree, where `mise run release` writes one per target to `dist/release/`.
Each route below says which directory its commands run in.

## Install a release tarball

1. In the directory that holds the tarball, name it and the prefix. Use the
   file name of your tarball; the targets are `x86_64-linux` and
   `aarch64-macos`:

   ```sh
   TARBALL="$PWD/tinyparse-0.1.0-x86_64-linux.tar.gz"
   PREFIX="$HOME/.local"
   ```

2. Unpack the tarball and change to the one directory it holds,
   `tinyparse-<version>-<target>/`:

   ```sh
   cd "$(mktemp -d)"
   tar xzf "$TARBALL"
   cd tinyparse-*/
   ```

3. Copy its four subdirectories into the prefix. The tarball's `INSTALL.md`
   stays behind:

   ```sh
   mkdir -p "$PREFIX"
   cp -R bin include lib share "$PREFIX/"
   ```

4. Check the command and the libraries:

   ```sh
   "$PREFIX/bin/tinyparse" version
   ls "$PREFIX/lib"
   ```

   On x86-64 Linux this prints the version and the three library files;
   `libtinyparse.so` is a symbolic link to `libtinyparse.so.0.1`:

   ```text
   tinyparse 0.1.0
   libtinyparse.a
   libtinyparse.so
   libtinyparse.so.0.1
   ```

   On macOS the library files are `libtinyparse.a` and
   `libtinyparse.0.1.dylib`.

The manual page is `$PREFIX/share/man/man1/tinyparse.1`; `man tinyparse` finds
it when `$PREFIX/share/man` is on your `MANPATH`. `tinyparse man` prints the
same page's source.

## Install with install.sh

`install.sh` unpacks a tarball and copies `bin`, `include`, `lib` and `share`
into a prefix, as the steps above do. It is in the `tinyparse/` directory of
a source tree.

Without `--from-file`, the script downloads
`tinyparse-<version>-<target>.tar.gz` from the GitHub release tagged
`tinyparse-v<version>`, so the download works only once such a release is
published. On 2026-10-04 the repository had none, and `--from-file` with a
tarball built by `mise run release` was the working route. The script picks
the target from `uname`, supports Linux on x86-64 and macOS on arm64, and
exits with status 1 on any other system.

| Option | Default | Meaning |
|--------|---------|---------|
| `--prefix DIR` | `$HOME/.local` | Where `bin`, `include`, `lib` and `share` go. |
| `--version VER` | the latest release of the repository | The version to download, such as `0.1.0`. Without it the script takes the tag of the repository's latest release, which can belong to another component of the repository. |
| `--repo OWNER/REPO` | `tinyactors-dev/tinyactors` | The GitHub repository to download from. |
| `--from-file FILE` | none | Install `FILE` and download nothing. |

1. From the `tinyparse/` directory, install a tarball you have:

   ```sh
   sh install.sh --from-file "dist/release/tinyparse-$(cat VERSION)-x86_64-linux.tar.gz" --prefix "$HOME/.local"
   ```

   It writes each installed file to standard error, prefixed with
   `[tp-install]`, and a hint to add `$HOME/.local/bin` to `PATH` when it is
   not there.

2. Check it:

   ```sh
   "$HOME/.local/bin/tinyparse" version
   ```

   ```text
   tinyparse 0.1.0
   ```

## Build from a source tree

1. Clone the repository and install its pinned tools. mise asks you to trust
   the repository's `mise.toml` the first time; `mise trust` does it ahead of
   time:

   ```sh
   git clone https://github.com/tinyactors-dev/tinyactors
   cd tinyactors
   mise trust
   mise install
   cd tinyparse
   ```

2. Build the library, the command, the test harness and the examples:

   ```sh
   mise run build
   ```

   The command is `build/bin/tinyparse` and the static library is
   `build/lib/libtinyparse.a`. The build makes no shared library;
   `mise run dist` builds the release targets' libraries, the shared ones among
   them, into `dist/<target>/lib/`.

3. Put the command on `PATH` and check it:

   ```sh
   export PATH="$PWD/build/bin:$PATH"
   tinyparse version
   ```

   ```text
   tinyparse 0.1.0
   ```

Every other guide assumes this setup: the shell in `tinyparse/` and
`tinyparse` on `PATH`. To link a program against this tree, use
`-I include` and `build/lib/libtinyparse.a`.

## Build the amalgamation

The amalgamation is the library in one C file, `tinyparse.c`, the header
`tinyparse.h`, and the command in `tinyparse_cli.c`. A source tree writes it
to `dist/amalgamation/` with `mise run amalgamate`; `mise run release` also
packs it as `dist/release/tinyparse-<version>-amalgamation.tar.gz`.

1. Copy the three files into a directory of your project and change to it:

   ```sh
   mise run amalgamate
   AMALGAMATION="$(mktemp -d)"
   cp dist/amalgamation/tinyparse.h dist/amalgamation/tinyparse.c dist/amalgamation/tinyparse_cli.c "$AMALGAMATION"
   cd "$AMALGAMATION"
   ```

2. Build what you need. Each line is independent:

   ```sh
   cc -c tinyparse.c
   cc tinyparse.c tinyparse_cli.c -o tinyparse
   cc -shared -fPIC -fvisibility=hidden tinyparse.c -o libtinyparse.so
   ```

   The first line writes the library object `tinyparse.o`, the second the
   command, the third a shared library. `tinyparse.c` is C11: it has an
   anonymous union, which `-std=c99 -pedantic-errors` rejects.

3. Check the command:

   ```sh
   ./tinyparse version
   ```

   ```text
   tinyparse 0.1.0
   ```

## Seed a project with tinyparse init

`tinyparse init DIR` writes a starter project into `DIR`: the amalgamation,
a greeting grammar `grammar.scm`, a program `main.c` that compiles that
grammar at run time and prints the tree of its standard input, and
`build.sh`. The manual's [init](../../tinyparse.1.md#init) entry lists the
files, the options and the memory grant of `main.c`. Run these commands in the
directory that is to hold the project.

1. Seed and build the project:

   ```sh
   tinyparse init hello
   cd hello
   sh build.sh
   ```

   `build.sh` is written without the execute bit, so run it with `sh`.

2. Run it from the project directory:

   ```sh
   echo 'hello world' | ./app
   ```

   It prints one node kind per line. The last `Space` is the line feed `echo`
   appends:

   ```text
   Greeting
     Hello
     Space
     Word
     Space
   ```

Before you edit the seeded files, know three things about them:

- `main.c` opens `grammar.scm` in the working directory; run `./app` from the
  project directory.
- `tinyparse init` replaces files of the same names without asking.
- `--grammar NAME` parses only with std.json and std.shell; the
  [init](../../tinyparse.1.md#init) entry lists why the other six fail, and
  [Supply imports, classes and parameters from a C host](host-services.md)
  adds what `main.c` lacks.

`tinyparse init DIR --lib` writes no amalgamation, and its `build.sh` runs
`cc main.c -ltinyparse -o app`, which finds the header and the library only
in the compiler's default directories. For any other prefix, build with the
lines under "Link your program" below.

`tinyparse vendor DIR` writes the amalgamation the installed `tinyparse`
carries into `DIR`; see [vendor](../../tinyparse.1.md#vendor).

## Link your program

This program prints the version of the library it is linked against. Save it
as `version.c`:

```c
#include <stdio.h>

#include "tinyparse.h"

int main(void)
{
    printf("linked against tinyparse %s\n", tinyparse_version_string());
    return 0;
}
```

`PREFIX` below is the prefix you installed into.

- Link the static library by its path. The program then needs nothing at run
  time:

  ```sh
  cc -I"$PREFIX/include" version.c "$PREFIX/lib/libtinyparse.a" -o version
  ./version
  ```

  ```text
  linked against tinyparse 0.1.0
  ```

- On Linux, link the shared library with `-L` and `-ltinyparse`, and record
  where it is with `-rpath`:

  ```sh
  cc -I"$PREFIX/include" -L"$PREFIX/lib" -Wl,-rpath,"$PREFIX/lib" version.c -ltinyparse -o version
  ./version
  ```

  ```text
  linked against tinyparse 0.1.0
  ```

  Without `-Wl,-rpath`, a prefix outside the loader's search path makes
  `./version` fail with `error while loading shared libraries:
  libtinyparse.so.0.1: cannot open shared object file` and status 127. Setting
  `LD_LIBRARY_PATH="$PREFIX/lib"` when you run it works too.

- On macOS, `-ltinyparse` links `libtinyparse.a`, because the tarball has no
  unversioned `libtinyparse.dylib`. To link the shared library, name its file;
  its install name is `@rpath/libtinyparse.0.1.dylib`, so add the rpath:

  ```sh
  cc -I"$PREFIX/include" version.c "$PREFIX/lib/libtinyparse.0.1.dylib" -Wl,-rpath,"$PREFIX/lib" -o version
  ```

The header compiles as C99; `tinyparse.h` includes only `<stddef.h>` and
`<stdint.h>`. The next step is [Parse input from a C
program](../tutorials/embed-in-c.md).
