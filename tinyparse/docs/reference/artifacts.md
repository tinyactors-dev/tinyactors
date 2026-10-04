# Release artifacts

This reference is for a developer or packager who needs to know what each
artifact tinyparse ships contains, what it depends on, and what the build
checks about it. The facts below were read from the artifacts of tinyparse
0.1.0 with the pinned LLVM 22 tools on x86-64 Linux, on 2026-10-04; how to
install them is in [Install tinyparse](../how-to/install.md).

## Release tarballs

`mise run release` writes one tarball per release target and one for the
amalgamation into `dist/release/`. Every tarball unpacks to one top-level
directory named after the file. The tarballs are deterministic: sorted
entries, every mtime set to the commit's time, owner and group 0, packed with
the pinned GNU tar and gzip.

| Tarball | Top-level directory |
|---------|---------------------|
| `tinyparse-<version>-x86_64-linux.tar.gz` | `tinyparse-<version>-x86_64-linux/` |
| `tinyparse-<version>-aarch64-macos.tar.gz` | `tinyparse-<version>-aarch64-macos/` |
| `tinyparse-<version>-amalgamation.tar.gz` | `tinyparse-<version>-amalgamation/` |

### Target tarball contents

| Path | x86_64-linux | aarch64-macos |
|------|--------------|---------------|
| `bin/tinyparse` | Static executable linked with musl; loads no library. | Loads `/usr/lib/libSystem.B.dylib` only. |
| `include/tinyparse.h` | The public header. | The public header. |
| `lib/libtinyparse.a` | Static library. | Static library. |
| `lib/libtinyparse.so.0.1` | Shared library; SONAME `libtinyparse.so.0.1`; loads no library. | Absent. |
| `lib/libtinyparse.so` | Symbolic link to `libtinyparse.so.0.1`. | Absent. |
| `lib/libtinyparse.0.1.dylib` | Absent. | Shared library; install name `@rpath/libtinyparse.0.1.dylib`; loads `libSystem` only. |
| `share/man/man1/tinyparse.1` | The manual page, in mdoc. | The manual page, in mdoc. |
| `INSTALL.md` | Installation steps, with the version and the target filled in. | Same. |

- The shared library's version is the major and minor number of
  [`VERSION`](../../VERSION): `0.1` for tinyparse 0.1.0.
- The macOS tarball has no unversioned `libtinyparse.dylib`, so `-ltinyparse`
  on macOS links `libtinyparse.a`.
- The command and the libraries are built with zig cc (`mise run dist`) and
  compiled with `-g0`, and the command's debug information is stripped, so no
  path of the build machine appears in them.

### Amalgamation tarball contents

| Path | Contents |
|------|----------|
| `tinyparse.h` | The public header, byte for byte `include/tinyparse.h`. |
| `tinyparse.c` | The library in one translation unit (see Amalgamation). |
| `tinyparse_cli.c` | The command, with the manual page, the standard grammars and the amalgamation compiled in. |
| `README.md` | How to build the three files. |

## Amalgamation

`mise run amalgamate` writes `dist/amalgamation/`: the same four files as the
amalgamation tarball.

`tinyparse.c` concatenates, in a fixed order, the public header,
`src/internal.h`, and every file in `src/` except `cli.c`, with the `#include`
lines of the files it folds in removed.

| Property | Value |
|----------|-------|
| Language | C11. It has one anonymous union, which `-std=c99 -pedantic` warns about and `-std=c99 -pedantic-errors` rejects. |
| Headers used by `tinyparse.c` | `<stddef.h>`, `<stdint.h>` |
| Headers used by `tinyparse_cli.c` | `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<errno.h>`, `<sys/stat.h>` and `tinyparse.h` |
| Warnings | None with clang 22 and `-std=c11 -Wall -Wextra -pedantic -Werror`, which the test suite uses. GCC 13.3 with `-pedantic` reports three in `tinyparse.c`: "ISO C forbids conversion of object pointer to function pointer type". |
| Symbol visibility | Wrapped in `#pragma GCC visibility push(default)`. A shared library built from it with `-fvisibility=hidden` exports the 48 `tinyparse_*` functions and the 84 internal `tp_*` functions of tinyparse 0.1.0, and nothing else. |

`tinyparse init` and `tinyparse vendor` write `tinyparse.c` and `tinyparse.h`
from a copy compiled into the command, so they write the amalgamation of the
command's own version.

## Library symbols

| Symbol group | `libtinyparse.a` | `libtinyparse.so.0.1`, `libtinyparse.0.1.dylib` |
|--------------|------------------|-----------------------------------------------|
| Public API: every function `tinyparse.h` declares with `TINYPARSE_API` (48 in tinyparse 0.1.0) | Defined, global | Exported; the only exported functions |
| Internal `tp_*` functions (84 in tinyparse 0.1.0) | Defined, global; hidden visibility in the release archives, default in `build/lib/libtinyparse.a` | Not exported |
| Writable data (data, BSS or common symbols) | None | None |

- Generated parsers call the internal `tp_*` functions, so they link against
  `libtinyparse.a` or the amalgamation's `tinyparse.c`, never against the
  shared library.
- The library sources are compiled with `-std=c11 -O2 -ffreestanding
  -fno-builtin`. The release builds add `-fvisibility=hidden` and define
  `TINYPARSE_API` as `__attribute__((visibility("default")))`.
- The library calls no allocator, does no I/O and starts no thread. Its only
  undefined symbols are the memory functions the compiler emits calls to:

  | Build | Undefined symbols |
  |-------|-------------------|
  | `dist/x86_64-linux/lib/libtinyparse.a`, `build/lib/libtinyparse.a` on x86-64 Linux | `memcpy`, `memset` |
  | `dist/aarch64-macos/lib/libtinyparse.a` | `memcpy`, `bzero` |

  `mise run dist` fails the build when the archive leaves anything undefined
  beyond `memcpy`, `memset`, `memmove` and `memcmp`, plus `bzero` and
  `__bzero` on macOS, where clang turns zeroing into calls to `bzero`.
- `mise run dist` also fails the build when a shared library exports anything
  but the public API or names itself by any file other than its versioned one.
  It fails when the shared library or the command loads a library other than
  the platform's C runtime: none on Linux, `libSystem` on macOS.

## Packed grammars

A packed grammar (`.tpg`) is the image `tinyparse pack` and
[`tinyparse_grammar_serialize`](c-api.md#tinyparse_grammar_serialize) write and
[`tinyparse_load_binary_grammar`](c-api.md#tinyparse_load_binary_grammar)
reads. Every integer in it is a 32-bit little-endian value.

| Offset | Size | Field | Value in tinyparse 0.1.0 |
|-------:|-----:|-------|--------------------------|
| 0 | 4 | Magic | The bytes `TPG1` |
| 4 | 4 | Format version | 4 |
| 8 | 4 | ABI tag: the sizes of six internal structures plus three internal enumeration values, each multiplied by a constant | 1985 for 64-bit targets (x86-64, AArch64); 1897 for 32-bit targets (RISC-V 32, wasm32) |
| 12 | | The grammar | |

The image holds the composed grammar: imported and extended rules, the
grammars an `inject` form names (each as a nested image), and the result of
every `(when NAME: ...)` form under the parameters set when it was packed.
Loading it calls no module resolver and reads no parameter.

A load fails with `TINYPARSE_INVALID` and one of these faults:

| Image | Fault |
|-------|-------|
| Shorter than 16 bytes | `binary grammar too short` |
| First four bytes are not `TPG1` | `not a packed grammar (bad magic)` |
| Another format version | `packed grammar version mismatch` |
| Another ABI tag | `packed grammar ABI mismatch` |

A `.tpg` loads only into a library with the same format version and ABI tag
as the library that wrote it. The ABI tag changes when the internal
structures change, which a new tinyparse version can do without a change to
the format version; regenerate every `.tpg`, generated module and generated
parser (each embeds an image) when you change the tinyparse version.

The command reads a grammar argument as a packed grammar when its name ends
in `.tpg` or its first four bytes are `TPG1`:

| Command | Accepts a `.tpg` |
|---------|------------------|
| `run`, `linearize`, `query`, `pack` | Yes |
| `check`, `node-types`, `c` | No: they read the file as `.scm` text and do not return on a `.tpg`. |

## Generated C files

`tinyparse c` and [`tinyparse_emit_c`](c-api.md#tinyparse_emit_c) write one C
file. What it needs depends on the mode:

| Mode | Includes | Defines | Builds with |
|------|----------|---------|-------------|
| Module (no flag) | `tinyparse.h` | `tinyparse_embedded_grammar()`, which returns the embedded `.tpg`; `--name` does not change this name | `tinyparse.h` and any of `libtinyparse.a`, the shared library, or the amalgamation. Compiles with `-std=c99 -pedantic`. |
| Bundle (`--bundle`) | `tinyparse.h`, `<stdio.h>`, `<stdlib.h>` | `tinyparse_embedded_grammar()` and `main`, with an 8 MiB static grant | Same as a module, and a hosted C library |
| Generated parser (`--parser`) | `tinyparse.h`, `internal.h`, `<stdio.h>`, `<stdlib.h>` | `main`, with a 1 GiB static grant, and one function per rule and token | `-I include -I src` from a source tree of the same version, and `libtinyparse.a` or the amalgamation's `tinyparse.c` |
| Generated parser library (`--parser-lib`) | Same as `--parser` | `P_load`, `P_run`, and [`P_run_chunk`](c-api.md#p_run_chunk) for a start rule that is a top-level repetition | Same as `--parser` |

The entry points of a generated parser and the prefix `P` are in [Generated
parser interface](c-api.md#generated-parser-interface). No header is
generated; the host declares the prototypes it calls.

## Platforms

| Platform | Built | Run by the test suite |
|----------|-------|-----------------------|
| x86_64-linux | `mise run dist`, with zig cc | The tarball's contents are checked, and `install.sh` installs it and runs the installed command |
| aarch64-macos | `mise run dist`, with zig cc, cross-compiled | No: its files are checked by reading their headers and symbol tables |
| The host running `mise run build` | `build/`, with the pinned clang | Yes |

No other target is built or tested. The library's flags and its undefined
symbols are listed above for a port; [Run tinyparse without
libc](../how-to/freestanding.md) builds it for another target.
