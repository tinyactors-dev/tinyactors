# Design constraints

The rules the build holds itself to. Each one exists because breaking it
cost something; the reason is given so a change can weigh it.

## 1. A script gives the same answer on every host

The workspace is developed on Linux and on macOS, and a script written on
one must work on the other without anyone noticing a switch. So every
difference between hosts lives in one place, `mise/lib/toolchain/`, and
no other script may:

- run a host-dependent program by name (`nm`, `ar`, `cc`, `readelf`,
  `otool`, `tar`, `sysctl`, ...);
- read `CC`, `AR`, `NM` or another tool variable;
- read `RUBY_PLATFORM` or `RbConfig::CONFIG`;
- branch on `macos?`, `linux?`, `wasm?` or `darwin?`.

`mise run check-portability` enforces this. **Why:** each of these once
broke a build on the other host. The host's `nm` could not read wasm; a
Mach-O object spells `seven` as `_seven`; macOS has no `readelf`; its
`tar` is bsdtar; its temporary directory is a symlink.

## 2. Every tool is pinned

Every program whose behaviour differs between hosts is pinned in the
root `mise.toml` and run through `Toolchain::Tool`. A missing tool fails
with the fix; there is no fallback to what is on `PATH`, and no "use
llvm-nm if it is there, else nm". **Why:** a fallback makes the result
depend on what happens to be installed, and hides the difference until
the other host.

Programs that behave the same everywhere (git, mise, gh, docker, sh,
curl) are run directly.

## 3. The toolchain is LLVM 22, with zig cc for releases

- **clang 22** compiles everything that runs or is checked here: the
  host's programs (`Target.host`), the checks (`Target.check`), the
  sanitizer builds and the API extractor.
- **llvm-tools 22** reads and archives every object: `llvm-nm`,
  `llvm-readobj`, `llvm-ar`, `llvm-objcopy`. They read ELF, Mach-O and
  wasm alike.
- **zig cc** builds what is shipped (`Target.release`), because it brings
  every target's C library and SDK: a release does not depend on the
  host's.

Nothing reads `$CC`. **Why not one tool:** zig has no sanitizer runtime,
no `nm` or `readobj`, and its clang (21) is not the release the API
extractor is pinned to; clang alone compiles against the host's C
library and SDK. **Why these two pins move together:** the API
extractor reads clang's JSON syntax tree, whose shape changes between
major releases, and `toolchain-test` fails when clang and the tools are
different releases.

What is still the host's: the C library and SDK a host program compiles
against (glibc, or the Command Line Tools' macOS SDK), `make` and `sh`
for the vendored autotools build, and docker for the release's container
tests.

## 4. A check reads a fixed target

A check that reads objects (the API extractor, the freestanding check,
the amalgamation's export check, the recursion check) compiles for
`Target.check`: the pinned clang, x86_64 Linux ELF, freestanding. **Why:**
compiled for the host, the same check reads Mach-O on a Mac and ELF on
Linux, and passes or fails for reasons that have nothing to do with the
code.

## 5. Callers tell, objects know

A script tells a target to compile, a binary to check itself, the host
to refuse what it cannot run. It does not ask what platform it is on and
decide. What differs is a method of `ObjectFormat`, `Target` or `Host`.
**Why:** a branch on the platform in a script is a second place that
must know every platform, and the first to forget one. A method on the
format is written once per format, and a new format must implement it.
See [Capture a domain concept](how-to/capture-a-domain-concept.md).

## 6. Releases are reproducible

A release is a function of the commit and the pinned tools:

- flags are fixed per target and profile; `$CFLAGS` is never read;
- archives are written with `D` (no timestamps, owners or modes);
- tarballs are sorted, with one mtime, owner 0 and `gzip -n`;
- paths in debug info are relative to the repository.

**Why:** a release anyone can rebuild byte for byte can be checked, and a
change to the build scripts can be proven harmless by comparing bytes
(see [Upgrade LLVM or zig](how-to/upgrade-the-toolchain.md)).

## 7. Skipping is by content

A task skips work when the content it depends on is unchanged, not when
a timestamp says so (`Cache`). Every key includes the tools' identities.
**Why:** an editor that rewrites a file with the same bytes, or a
comment-only edit, should cost a few hashes, and a new compiler must
rebuild everything.

## 8. Every script is Ruby, run by mise

Tasks are mise file tasks written in Ruby, in `mise/tasks/`; shared code
is in `mise/lib/`. There is one runtime (Ruby 4, pinned) and one task
runner. **Why:** one language for every script means one way to
structure and check them; static analysis only needs one grammar.

## 9. The libraries are private

Every caller of every build library is in this repository. Change an API
in place and move every caller in the same change; no `_v2`, no wrapper,
no shim (`AGENTS.md`, "The libraries are private").

## 10. Static checks are ast-grep rules with tests

A new check on the build scripts is an ast-grep rule in
`mise/ast-grep/rules/`, with valid and invalid cases in
`mise/ast-grep/tests/`, not a `grep` or a regular expression. **Why:** a
rule reads the syntax tree, so it does not match a comment or a string
that happens to contain the word, and its tests say what it means. See
[Add a static check](how-to/add-a-static-check.md).
