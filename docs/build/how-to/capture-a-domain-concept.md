# Capture a domain concept in Ruby

You need the build to do something that differs between platforms,
object formats or machines: a new linker flag on one format, a file a
release installs on one OS, a fact about the machine. This guide says
where it goes and how to shape it so no script has to ask which platform
it is on.

The rule behind it is [constraint 5](../design-constraints.md#5-callers-tell-objects-know):
callers tell, objects know.

## 1. Name what it is, not where it differs

Write down what the build needs in the words of the domain, without a
platform in it:

| Not this | This |
|---|---|
| "on Linux, add a `lib<name>.so` symlink" | "a library gets its development link" |
| "on macOS, strip the leading underscore" | "a symbol's C name" |
| "on macOS, allow `_bzero`" | "what the platform's runtime provides" |
| "if the host is x86_64 Linux, run the tests" | "the host must run this target's programs" |

The second column is a method name. If you cannot name it without a
platform, you have not found the concept yet.

## 2. Pick the object that owns it

| The difference depends on | It belongs to | Examples |
|---|---|---|
| the object format (ELF, Mach-O, wasm) | `Toolchain::ObjectFormat::*` | `c_name`, `shared_library_file`, `install_links`, `runtime_undefined` |
| what the result is for (release, check, host) | the compiler inside `Target` (`Zig`, `Clang`) | `flags`, `archive`, `environment` |
| the machine the build runs on | `Toolchain::Host::Mac`, `Host::Linux` | `must_run!`, `hardware`, `must_have_c_library!` |
| a property of a file that was built | `Toolchain::Binary` | `exports`, `dependencies`, `must_be_for!` |
| which program to run | `Toolchain::Tool` | `Tool[:llvm_nm]` |

When it depends on the format, it goes on the format even if only one
format needs it: the others answer "nothing".

## 3. Implement it on every variant

Add the method to each variant, so a new variant cannot forget it. The
ELF development link, for example:

```ruby
module ELF
  # The development name beside a versioned library (lib<name>.so).
  def install_links(file, library, dir)
    File.symlink(file, dir.join("lib#{library}.so"))
  end
end

module MachO
  def install_links(_file, _library, _dir) = nil
end
```

Where a variant has no meaning for it, raise `Toolchain::Error` with
what to do instead, as `WASM#shared_library_file` does ("link a module").

## 4. Expose it as a verb on `Target` or `Binary`

A script should not call the format directly for something the target
does. The target uses the format inside its own verb:

```ruby
def shared_library(objects, library:, version:, dir:, exports: nil)
  file = @format.shared_library_file(library, version)
  link_shared(objects, dir.join(file), file, @format.shared_flags(exports: exports))
  @format.install_links(file, library, dir)
  Binary.new(dir.join(file))
end
```

The caller says what it wants and gets a `Binary` back:

```ruby
target.shared_library([object], library: project.name, version: '0.9', dir: lib)
```

Before, the caller held both branches:

```ruby
if target.macos?
  name = "lib#{project.name}.#{major}.#{minor}.dylib"
  Toolchain.shared(target, ..., flags: ['-Wl,-dead_strip', ...])
else
  name = "lib#{project.name}.so.#{major}.#{minor}"
  Toolchain.shared(target, ..., flags: %w[-Wl,--gc-sections -Wl,-S])
  File.symlink(name, lib.join("lib#{project.name}.so"))
end
```

## 5. Make a check a `must_*!` that answers `self`

A property the build verifies is a method on `Binary` (or `Host`) that
returns `self` when it holds and raises when it does not, saying what
differs:

```ruby
def must_keep_local!(names)
  global = globals & names
  return self if global.empty?

  raise Mismatch, "#{basename}: #{global} must be local"
end
```

Returning `self` lets checks chain. What the platform adds on its own
(libSystem, ld64's symbols) is subtracted inside the check, from the
format, so the caller passes only what its own code needs:

```ruby
binary.must_export_exactly!(api).must_leave_undefined_only!(external)
```

A set of names is a `Toolchain::Symbols`: sorted, unique, and with a
`difference` that reads well in the error message.

## 6. Keep the platform predicates inside the toolchain

`Target#macos?`, `#linux?` and `#wasm?` exist for `mise/lib/toolchain/`
alone. If your new method needs one, it is inside the toolchain and may
use it; if a script needs one, the concept is not captured yet. `mise run
check-portability` rejects the call outside the toolchain.

## 7. Test it on every target

Add a case to `mise/lib/toolchain/selftest.rb`. zig cross-compiles, so
the self-test builds Mach-O, ELF and wasm on any host; a Linux machine
tests `MachO#c_name` and a Mac tests `ELF#install_links`. Break the
method on purpose once and check that `mise run toolchain-test` fails.

## Checklist

- [ ] The method is named for the concept, with no platform in the name.
- [ ] Every variant implements it, or raises with what to do instead.
- [ ] Scripts call a verb on `Target`, `Binary` or `Host`, not the format.
- [ ] A check is a `must_*!` that answers `self` and raises `Mismatch`.
- [ ] `mise run toolchain-test` covers it, and fails when it is broken.
- [ ] `mise run check-portability` passes.
