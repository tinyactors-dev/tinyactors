# AGENTS.md

The conformance suite (README.md). The repository root's AGENTS.md applies
too: tasks are Ruby file tasks in `mise/tasks/`, acronyms, Conventional
Commits on `main`. Commits here use the scope `test`.

## What goes here

- **Only tests that are data**, run against a command through
  `docs/driver.md`. What a black box cannot show (memory failures, a call
  refused during a run, threads, garbage collection, liveness) stays in
  the tests of the project it belongs to.
- **No expected effects.** A scenario's charts check what happened and end
  in `pass` or `fail`, as W3C tests do. Do not add a suite that records an
  implementation's effects and compares them: the bytes the suite compares
  are the protocol vectors, which are what core takes and gives at its API.
- **The runner interprets nothing.** It frames calls, compares bytes and
  reads outcomes. It has no scheduler, no SCXML and no knowledge of
  effects; the implementation runs its own loop.

## Adding cases

- **A protocol vector:** write the case's lines with `diag` lines only,
  then `mise run encode` writes the hex; `mise run test` checks that hex
  and diag lines agree. The format and the wildcards are in
  `suites/protocol/README.md`. A new setup is a chart in `setups/`, a row
  in the README's table, and its steps in `lib/runner.rb` (`setup`) built
  with the driver's methods.
- **A scenario:** a directory in `suites/scenarios/` with `<name>.txml`
  documents and a `steps.txt` (`suites/scenarios/README.md`). Use only the
  `conf:` vocabulary of `confEcma.xsl` (fetched into `build/w3c/irp`) for
  expressions and conditions; check each attribute you use is defined
  there. In `steps.txt`, say in comments which expectations a chart cannot
  check and which test covers them. A scenario must fail when what it
  checks is broken: try a mutation (other params, a step left out) before
  committing.
- **A W3C test** is skipped only with a reason in `suites/w3c/skip.txt`.

## Adding a driver method

When a suite needs a core call no method reaches ("Adding a method" in
`docs/driver.md`): name the calls it makes, take their struct fields in
camelCase, answer `status` and the calls' outputs. Add it to the table in
`docs/driver.md`, to `ta-conform` and to each binding's adapter in the same
change, and to `test/fake_implementation.rb` when the runner's tests need
it. An implementation that does not have it answers -32601, and the runner
skips the cases that need it.

## Tests

`mise run test` runs `encode --check` and `test/*_test.rb` (minitest)
against `test/fake_implementation.rb`. A change to the runner comes with a
test there: the fake answers by rules, and the tests write their suites
into a temporary directory. Do not make the tests fetch anything.
