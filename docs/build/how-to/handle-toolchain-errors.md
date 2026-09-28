# Handle toolchain errors

What the toolchain raises, where a script rescues it, and what the common
messages mean.

## The error classes

Every error the toolchain raises is a `Toolchain::Error`:

| Class | Raised when | Its message says |
|---|---|---|
| `Toolchain::Tool::Missing` | a pinned program is not installed | which `mise.toml` entry to pin and to run `mise install` |
| `Toolchain::Tool::Failed` | a tool exits non-zero in `Tool#run` | the command and its output |
| `Toolchain::Binary::Mismatch` | a `must_*!` check does not hold | what differs: unexpected and missing names |
| `Toolchain::Error` | anything else: an unknown target, a host that cannot run a target, a missing SDK, a meaningless operation (a wasm shared library) | what is wrong and, where there is one, the fix |

## Raise with the fix in the message

A toolchain error is read by a person who did not write the code, often
on the other host. Say what is wrong and what to do:

```ruby
raise Missing, "#{@program} is not installed: pin #{@package} in the root mise.toml and run `mise install`"
raise Error, 'the pinned clang builds against the macOS SDK, which is missing: run `xcode-select --install`'
raise Error, "#{why}: only on an #{target.name} host; this one is #{self.target.name}"
```

Do not fall back instead of raising. "Use `llvm-nm` if it is there, else
`nm`" turns a missing tool into a different result on another machine
([constraint 2](../design-constraints.md#2-every-tool-is-pinned)).

## Rescue at the task, not in the library

Libraries under `mise/lib/` raise and let the error travel. The task, the
outermost script, rescues `Toolchain::Error` once and turns it into its
own prefixed failure:

```ruby
begin
  Toolchain::Target.check.compile(unit, into: object, includes: includes)
                   .must_export_exactly!(declared)
rescue Toolchain::Error => e
  abort "amalgamate: #{e.message}"
end
```

`CRelease.release` does the same for a whole release, after its `ensure`
has undone a local tag or commit:

```ruby
def release(...)
  ...
rescue Toolchain::Error => e
  release_fail!(e.message)
end
```

Rescue closer to the error only to add context the caller alone has, and
raise again. `mise/tasks/symbols` in napi, for example, adds what to
check before allowing a new libc import:

```ruby
rescue Toolchain::Binary::Mismatch => e
  problems << "#{e.message}#{advice}"
```

A check that collects every problem before failing, as napi's `symbols`
and `freestanding.rb` do, rescues `Mismatch` per check and reports them
all.

## Capture instead of raising, when failure is an answer

When a non-zero exit is an answer rather than a fault (the API extractor
asks clang whether a snippet compiles), use `Tool#capture`, which returns
`[stdout, stderr, status]` and never raises:

```ruby
_out, errors, status = Toolchain::Tool[:clang].capture(*flags, '-fsyntax-only', '-', stdin: source)
status.success? ? '' : errors
```

## Common messages

| Message | Cause | Fix |
|---|---|---|
| `llvm-nm is not installed: pin conda:llvm-tools in the root mise.toml and run mise install` | a new checkout or a new pin | `mise install` at the root |
| `toolchain-test: tool X: is pinned in "…/tinyxml/mise.toml", not the root mise.toml` | a project pinned a tool the toolchain uses | move the pin to the root `mise.toml` |
| `toolchain-test: LLVM: clang and LLVM's tools are different releases` | `clang` and `conda:llvm-tools` pins moved apart | pin both to the same major ([Upgrade](upgrade-the-toolchain.md)) |
| `the pinned clang builds against the macOS SDK, which is missing` | no Command Line Tools on this Mac | `xcode-select --install` |
| `the host tests: only on an x86_64-linux host; this one is aarch64-macos` | a release's host tests run the packed x86_64 Linux libraries natively | release from x86_64 Linux |
| `lib.a exports other functions than it should: unexpected: … missing: …` | a function's visibility or the API description changed | make the function static, or regenerate the API description |
| `… leaves undefined: memcpy (allowed: none)` | the compiler turned a copy into a libc call | see the freestanding rules in the project's `AGENTS.md` |
| `… is not x86_64` | an object for the wrong architecture was linked | check the target the object was compiled for |
| `unknown target foo; known: aarch64-macos, …` | a typo in `--target` or a task | use one of the listed names |
