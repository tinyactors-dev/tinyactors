# Add a static check

You want the build scripts checked for a pattern: a call that must go
through the toolchain, a variable no script may read, a helper nobody
should call. Write an ast-grep rule, with tests, and `mise run
check-portability` enforces it with the others.

Checks on the build scripts are ast-grep rules, not `grep` or a regular
expression ([constraint 10](../design-constraints.md#10-static-checks-are-ast-grep-rules-with-tests)).

## 1. Write down what is wrong and what to do instead

A rule is only useful if a person who trips it knows what to write
instead. Write that first; it becomes the rule's `message` and `note`.

## 2. See the syntax tree

Put an example of the code you want to catch in a scratch `.rb` file and
look at how ast-grep parses it:

```sh
ast-grep run -l ruby -p 'Open3.capture2($$$A)' --debug-query=cst example.rb
```

The node kinds you will use most in Ruby: `call` (with fields `receiver`,
`method`, `arguments`), `argument_list`, `array`, `string`,
`string_array` (`%w[]`) with `bare_string` children, `identifier`,
`constant`, `scope_resolution`, `hash`, `simple_symbol`.

## 3. Write the rule

Add `mise/ast-grep/rules/<rule>.yml`. Exempt the toolchain, which is
where host-dependent code is allowed:

```yaml
id: host-detection
language: ruby
severity: error
message: Ask Toolchain::Host.current instead of reading the platform yourself.
note: |
  What differs between machines lives in mise/lib/toolchain/host.rb ...
ignores:
  - mise/lib/toolchain.rb
  - mise/lib/toolchain/**
rule:
  any:
    - pattern: RUBY_PLATFORM
    - pattern: RbConfig::CONFIG
```

Start from a `pattern`; move to `kind` with relational rules (`inside`,
`has`, `follows`, `precedes`) when a pattern cannot say it. Things that
cost time before:

- A `$` in `message` or `note` is read as a metavariable and vanishes.
  Write "the CC variable", not `$CC`.
- `follows` and `precedes` with `stopBy: neighbor` see the unnamed `,`
  token between arguments. Use `nthChild` for positions (it counts named
  nodes only), and `stopBy: end` on the sibling.
- A rule that matches a word anywhere (`'nm'`) catches data too
  (`['file', 'nm']`). Tie it to where it is a command: the first argument
  of a call that runs a process, or an array whose next element is a
  flag. `unpinned-tool.yml` shows how.
- Reuse a sub-rule with `utils:` and `matches:`, and a condition on a
  metavariable with `constraints:`.

## 4. Test it

Add `mise/ast-grep/tests/<rule>-test.yml` with code that must pass and
code that must be reported, including the near misses:

```yaml
id: host-detection
valid:
  - "Toolchain::Host.current.must_run!(target, 'integration')"
  - "system(RbConfig.ruby, script)"
invalid:
  - "abort 'Linux only' unless RUBY_PLATFORM.start_with?('x86_64-linux')"
  - "RbConfig::CONFIG.fetch('host_cpu')"
```

Run the tests alone while you work:

```sh
ast-grep test --skip-snapshot-tests
```

A `Noisy` result is a valid case the rule reported; `Missing` is an
invalid case it did not.

## 5. Scan the workspace

```sh
mise run check-portability
```

It runs the rule tests first, then scans every Ruby build script: the
`.rb` files and the extensionless tasks under `mise/lib/` and
`mise/tasks/` of the root and every project. Fix what it finds, or, if a
finding is right to exist, narrow the rule and add the case to `valid`.

## Checking files other than build scripts

`check-portability` scans the build scripts. To read another language
with ast-grep from a script, run it through the pinned tool with an
inline rule, as `CHeader#functions_declared_with` does for C:

```ruby
Toolchain::Tool[:ast_grep].run('scan', '--inline-rules', rule, '--json=compact', path)
```
