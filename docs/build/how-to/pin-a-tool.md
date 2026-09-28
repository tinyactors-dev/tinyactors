# Pin a new tool

The build needs a program it does not run yet, and the program behaves
differently between hosts, or is missing on one: a binutil, an archiver,
a compressor, a code generator. Pin it and run it through
`Toolchain::Tool`.

A program that behaves the same everywhere (git, mise, gh, docker, sh,
curl) needs none of this; run it directly.

## 1. Find it in mise

Prefer a backend that ships the same version on Linux and macOS, on both
architectures:

```sh
mise registry | grep -i <name>         # a short name, e.g. ast-grep
mise ls-remote conda:<package>         # conda-forge builds for every platform
mise ls-remote aqua:<owner>/<repo>     # GitHub releases
```

The existing pins show the pattern: `clang` and `conda:llvm-tools` for
LLVM, `conda:tar` and `conda:gzip` for GNU tar and gzip, `ast-grep` and
`zig` from the registry. A conda package may hold several programs;
`conda:llvm-tools` holds every `llvm-*` tool.

Check the program runs from the pin on this host before going on:

```sh
mise exec conda:<package>@<version> -- <program> --version
```

## 2. Pin it in the root `mise.toml`

Add it under `[tools]` with an exact or major version, and a comment
saying what uses it and why it must be pinned:

```toml
# GNU tar and gzip, so a release tarball is the same bytes on every host
# (macOS ships bsdtar and another gzip).
"conda:tar" = "1.35"
```

It goes in the root file, not a project's: `Tool` resolves programs from
the root, and `toolchain-test` fails for a tool pinned anywhere else. Run
`mise install`.

## 3. Add it to `Tool::PINNED`

In `mise/lib/toolchain/tool.rb`, map a name to the program and the
`mise.toml` entry that provides it:

```ruby
PINNED = {
  ...
  llvm_ranlib: %w[llvm-ranlib conda:llvm-tools],
  tar: %w[tar conda:tar],
}.freeze
```

If the program prints its version with something other than `--version`,
add it to `VERSION_ARGUMENT` (zig uses `version`). `Tool#identity` takes
the first line of that output that holds a version number; cache keys use
it, so a new version of the tool invalidates what it built.

## 4. Run it through `Tool`, or give it a home

For a one-off call, run it directly:

```ruby
Toolchain::Tool[:tar].run('xzf', archive, '-C', install)
```

If the call encodes knowledge about the tool's output or flags that other
scripts will need, put that in the toolchain instead: a method on
`Binary` for reading objects, on `Target` for building, on an
`ObjectFormat` for what differs between formats. See
[Capture a domain concept](capture-a-domain-concept.md).

## 5. Keep the static check honest

If the program was run by name anywhere (`Open3.capture2('readelf', …)`),
add its name to the program list of `mise/ast-grep/rules/unpinned-tool.yml`
and an invalid case to `mise/ast-grep/tests/unpinned-tool-test.yml`, so
the next script that runs it from `PATH` fails the check. See
[Add a static check](add-a-static-check.md).

## 6. Check

```sh
mise run toolchain-test      # every pinned tool resolves, runs, and is pinned in the root mise.toml
mise run check-portability   # nothing runs it by name
```
