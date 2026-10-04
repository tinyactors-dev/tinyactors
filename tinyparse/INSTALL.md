<!--
In the repository this file is a template: `mise run release` copies it into
each release tarball with the version and the platform filled in. To install
from a repository checkout, read docs/how-to/install.md.
-->

# tinyparse @VERSION@ for @PLATFORM@

This directory holds the tinyparse command, the C library, its header and the
manual page, built for @PLATFORM@. Copy it into a prefix to install, then link
your program against the library.

## Contents

| File | What it is |
|------|------------|
| `bin/tinyparse` | The command. On x86_64-linux it is a static executable; on aarch64-macos it loads only the system's `libSystem`. |
| `include/tinyparse.h` | The public header. It compiles as C99 and includes only `<stddef.h>` and `<stdint.h>`. |
| `lib/libtinyparse.a` | The static library. |
| `lib/libtinyparse.so.0.1`, `lib/libtinyparse.so` | x86_64-linux only: the shared library, and a symbolic link to it. |
| `lib/libtinyparse.0.1.dylib` | aarch64-macos only: the shared library. Its install name is `@rpath/libtinyparse.0.1.dylib`. |
| `share/man/man1/tinyparse.1` | The manual page. |
| `INSTALL.md` | This file. |

## Install

Run these commands in this directory. They copy everything but this file into
`$HOME/.local`; set `PREFIX` to install elsewhere:

```sh
PREFIX="$HOME/.local"
mkdir -p "$PREFIX"
cp -R bin include lib share "$PREFIX/"
```

Check the command:

```sh
"$PREFIX/bin/tinyparse" version
```

```text
tinyparse @VERSION@
```

Add `$PREFIX/bin` to `PATH` to run `tinyparse` by name, and
`$PREFIX/share/man` to `MANPATH` for `man tinyparse`.

## Link a program

Link the static library by its path:

```sh
cc -I"$PREFIX/include" myprog.c "$PREFIX/lib/libtinyparse.a" -o myprog
```

On x86_64-linux, link the shared library and record where it is, so the
program finds it at run time:

```sh
cc -I"$PREFIX/include" -L"$PREFIX/lib" -Wl,-rpath,"$PREFIX/lib" myprog.c -ltinyparse -o myprog
```

On aarch64-macos, `-ltinyparse` links the static library, because this
directory has no `libtinyparse.dylib`. Name the shared library's file to link it:

```sh
cc -I"$PREFIX/include" myprog.c "$PREFIX/lib/libtinyparse.0.1.dylib" -Wl,-rpath,"$PREFIX/lib" -o myprog
```

A parser generated with `tinyparse c --parser` or `--parser-lib` does not
build from this directory: it needs the private header and the internal
functions of a source tree.

The full installation guide, including the `install.sh` script, is
`tinyparse/docs/how-to/install.md` in the repository
(https://github.com/dhamidi/tinyactors).
