# The build tooling

How the workspace builds, checks and releases its C libraries and the
Node-API addon, and how to change that machinery. The build is a set of
Ruby scripts run by mise; this documentation is about those scripts, not
about the libraries they build.

## Reference

- [How the build works](how-it-works.md): tasks, the shared Ruby library,
  caching, the toolchain objects, releases, the test pass and the static
  analysis.
- [Design constraints](design-constraints.md): the rules the build holds
  itself to, and why each exists.

## How-to guides

- [Capture a domain concept in Ruby](how-to/capture-a-domain-concept.md):
  where a new difference between platforms, formats or hosts goes, and how
  to shape it so callers tell instead of ask.
- [Handle toolchain errors](how-to/handle-toolchain-errors.md): the error
  classes, where to rescue them, and what each common message means.
- [Pin a new tool](how-to/pin-a-tool.md): add a program the build runs.
- [Add a static check](how-to/add-a-static-check.md): write an ast-grep
  rule with tests.
- [Upgrade LLVM or zig](how-to/upgrade-the-toolchain.md): move the pins
  and prove nothing else moved.
- [Fix a difference between Linux and macOS](how-to/fix-a-host-difference.md):
  find where a script depends on the host, and move that into the
  toolchain.

## Where the code is

| Path | What it holds |
|---|---|
| `mise.toml` | Every pinned tool, and `RUBYLIB` for the shared library |
| `mise/tasks/` | Workspace tasks: `build`, `test`, `check-portability`, `toolchain-test`, ... |
| `mise/lib/toolchain.rb`, `mise/lib/toolchain/` | Everything that differs between hosts |
| `mise/lib/cache.rb`, `mise/lib/task_stamp.rb` | Content-based caching |
| `mise/lib/c_release.rb`, `mise/lib/c_release/` | dist and the C libraries' release |
| `mise/lib/release.rb` | What every project's release shares: tags, notes, GitHub |
| `mise/lib/build_scripts.rb`, `sgconfig.yml`, `mise/ast-grep/` | Static analysis of the build scripts |
| `<project>/mise/tasks/`, `<project>/mise/lib/` | One project's tasks and helpers |

The decisions behind the release are in
[ADR 0001](../adr/0001-packaging-and-releases.md).
