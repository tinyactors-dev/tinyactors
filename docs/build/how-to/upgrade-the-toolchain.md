# Upgrade LLVM or zig

Move a toolchain pin to a new release, and show that what the build
produces moved only where it should.

The toolchain is LLVM (the `clang` and `conda:llvm-tools` pins) for
everything that runs or is checked here, and zig for release binaries
([constraint 3](../design-constraints.md#3-the-toolchain-is-llvm-22-with-zig-cc-for-releases)).
The two are upgraded separately.

## Upgrade LLVM

### 1. Move both pins together

In the root `mise.toml`:

```toml
clang = "23"
"conda:llvm-tools" = "23"
```

Check both exist for every platform first (`mise ls-remote clang`,
`mise ls-remote conda:llvm-tools`), then `mise install`.
`mise run toolchain-test` fails while the two are different releases.

### 2. Regenerate the API descriptions

The API extractor reads clang's JSON syntax tree, whose shape changes
between major releases (clang 22 dropped the `ElaboratedType` wrapper
around a typedef's tag). Check each description:

```sh
mise run //tinyactors-core:api --check
mise run //tinyxml:api --check
```

If a check fails, look at the difference before regenerating. A new node
shape is a change to `mise/lib/api/` (in core and tinyxml); a real API
change is not something an upgrade may cause.

### 3. Run everything that uses clang

```sh
mise run test:full   # the host builds, the checks, every project,
                     # core's sanitizer build and its Node-API addon
```

The host compile commands name clang's path, so every host object
rebuilds once.

### 4. Confirm the releases did not move

A release is built by zig, but `llvm-objcopy` makes its final object and
`llvm-nm`, `llvm-readobj` and `llvm-otool` check it. Build the release
libraries before and after, and compare bytes (below). They should be
identical; if not, find which tool's output changed.

## Upgrade zig

### 1. Move the pin

```toml
zig = "0.17.0"
```

`mise install`, then `mise run toolchain-test`: every release target
compiles, archives and links, and reads back as expected.

### 2. Check the release against its rules, not its bytes

A new zig changes the release's bytes by design. What must hold is what
the release verifies: each library exports exactly the API, leaves only
the allowed symbols undefined, keeps the memory functions local, loads
only its dependencies, and is for its architecture. Build and verify
every target (below):

```sh
mise run //tinyactors-core:check:cross     # every layer, for each release target
mise run //tinyactors-core:check:wasm      # the tests, as wasm32-wasi, under wasmtime
```

Record in the commit how the release's size and speed changed (the
torture chamber, `mise run torture` in `tinyactors-core`); zig's compiler-rt
`memset` is why the release carries its own (ADR 0001, decision 16).

## Building and comparing the release libraries

The release task needs a clean tree and tags; for an upgrade, build and
verify the libraries directly. Save this as a script outside the
repository and run it from the root, once per project:

```ruby
# release-check.rb <project dir> <library> <prefix>
require 'c_release'
dir, name, prefix = ARGV
Dir.chdir(dir)
project = CRelease::Project.new(name: name, prefix: prefix)
out = Pathname.new(ENV.fetch('OUT'))
CRelease::NATIVE_TARGETS.each do |target|
  CRelease.verify_native!(project, CRelease.build_native(project, target, '0.9.1', out))
end
CRelease.verify_wasm!(CRelease.build_wasm(project, out))
```

```sh
TA_CACHE_ROOT=$PWD/tinyxml OUT=/tmp/after/tinyxml mise exec -- ruby -Imise/lib release-check.rb tinyxml txml txml
TA_CACHE_ROOT=$PWD/tinyactors-core OUT=/tmp/after/core mise exec -- ruby -Imise/lib release-check.rb tinyactors-core tinyactors ta
```

To build the "before" side, check out the previous commit in a worktree
and run the same script there. The worktree's `mise.toml` is not trusted
yet; trust it for that shell only:

```sh
git worktree add /tmp/before HEAD~1
export MISE_TRUSTED_CONFIG_PATHS=/tmp/before
(cd /tmp/before && TA_CACHE_ROOT=$PWD/tinyxml OUT=/tmp/before-out/tinyxml mise exec -- ruby -Imise/lib /path/to/release-check.rb tinyxml txml txml)
```

Compare, leaving out the intermediate objects:

```sh
(cd /tmp/before-out/tinyxml && find . -path '*/work' -prune -o -type f -print | sort | xargs shasum) > before.txt
(cd /tmp/after/tinyxml && find . -path '*/work' -prune -o -type f -print | sort | xargs shasum) > after.txt
diff before.txt after.txt && echo identical
git worktree remove --force /tmp/before
```
