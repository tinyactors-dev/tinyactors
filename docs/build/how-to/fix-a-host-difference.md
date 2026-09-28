# Fix a difference between Linux and macOS

A task passes on one host and fails on the other. This guide finds where
the script depends on the host, and moves that dependence into the
toolchain so it cannot come back.

## 1. Run the checks that look for it

```sh
mise run check-portability   # a script that runs a host tool, reads CC, or branches on the platform
mise run toolchain-test      # the tools, every target's objects, the host's C library
```

`check-portability` names the file, line and rule. Each rule's `note`
in `mise/ast-grep/rules/` says what to use instead.

## 2. If neither finds it, recognise the kind

The differences met so far, and how each shows:

| Symptom | Cause | Where it was fixed |
|---|---|---|
| `No such file or directory - llvm-nm` | a tool on one host's `PATH` and not the other's | pinned in `mise.toml`, run through `Tool` |
| `exports ["_f"], declares ["f"]` | Mach-O spells every C name with a leading `_` | `ObjectFormat::MachO#c_name`, read through `Binary` |
| `readelf: No such file or directory` | a GNU binutil that macOS lacks | `llvm-readobj` and `llvm-otool` through `Binary` |
| `tar is not GNU tar` | macOS's `tar` is bsdtar | GNU tar pinned (`conda:tar`) |
| a check passes on Linux and fails on macOS, or the reverse, with the same code | it compiled for the host: ELF on one, Mach-O on the other | the check compiles for `Target.check` |
| a path lookup finds nothing on macOS | the temporary directory is a symlink (`/var` is `/private/var`) and paths were compared as strings | compare `SourceMap.canonical` paths (resolved with `File.realpath`) |
| `Mismatch` on `bzero`, `dyld_stub_binder`, `__dso_handle` | the platform's runtime or linker adds them | `ObjectFormat#runtime_undefined`, `#linker_defined` |

Other things that differ between the hosts, to suspect next: the C
library's headers, case-insensitive file names on macOS, `sed -i` and
other GNU and BSD flags, `/proc` (Linux only), the default shell, and
file ordering from `Dir[]` (sort it).

## 3. Reproduce it on the host you have

Most differences can be shown from either host, because the toolchain
builds every format on both:

```ruby
# A Mach-O object's symbols on a Linux machine, and the reverse:
target = Toolchain::Target.release('aarch64-macos')
object = target.compile('seven.c', into: '/tmp/seven.o')
object.exports.to_a
```

For a difference in the machine itself (paths, the C library, what is
installed), run the failing task with the other host's condition forced
where you can: a path through a symlink, a missing tool (`PATH` without
it), a fresh `mise install`.

## 4. Move the dependence into the toolchain

Decide what the difference is in domain words and put it on the object
that owns it: an `ObjectFormat` for a difference between formats, `Host`
for one between machines, `Tool` for a program. See
[Capture a domain concept](capture-a-domain-concept.md). The script then
calls a method, and has no branch.

A difference that is not about the toolchain (the debug adapter's paths)
is fixed where it is, by comparing what the file system resolves rather
than what was typed.

## 5. Keep it from coming back

- Add the case to `mise/lib/toolchain/selftest.rb`, so `toolchain-test`
  fails on either host if it breaks. Break it on purpose once to see it
  fail.
- If a script could reintroduce it by writing the old pattern, add an
  ast-grep rule ([Add a static check](add-a-static-check.md)).
- Say in the commit message which host failed and why, so the next person
  who meets the symptom finds it in `git log`.
