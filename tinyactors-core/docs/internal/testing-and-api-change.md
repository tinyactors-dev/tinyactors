# Testing behaviour, and changing the API

For contributors who add coverage or change the public surface. The
mechanics of the test host — the scenario DSL, instruments, traces — are in
`tinyactors-test/README.md` and `tinyactors-test/docs/protocol.md`. This
page is about *method*: how to write a test that can actually catch a bug,
the traps that make tests look like they do when they don't, and when a
failure means the API should change instead of the test.

## The failure mode this guards against

A suite with one shallow test per public function goes green while the
library stays unproven. Every function is "covered" and nothing is
demonstrated: a test that calls `ta_actor_destroy()` and checks it returns
`TA_OK` passes just as happily against an implementation that leaks every
timer the actor owned.

Tests here are therefore organized around **claims**, not functions. API
coverage is a by-product worth measuring, never the target.

## 1. Pick a claim

A claim is something already written down:

- an ADR decision (`docs/adr/`), which is the primary record of intent;
- a numbered rule in `docs/internal/invariants.md`;
- a contract sentence in `include/tinyactors.h`;
- a mandatory W3C assertion, via the imported IRP suite.

Prefer the claim to your own intuition. When two plausible behaviours exist,
the SCXML suite usually already decides: `test194` fixes that an invalid
send target is `error.execution`, and `test496` that an inaccessible session
is `error.communication`. Quote the source in the scenario.

## 2. Write a story that can fail

One scenario should exercise several APIs in a sequence a real host would
perform, and end in an observation a plausible bug would change.

- Give every scenario a `# Catches:` comment naming the wrong
  implementation it detects. If you cannot name one, the test is decoration.
- Arrange the inputs so the bug is *visible*. The traffic-light repair
  scenario advances time so that a leaked timer would turn the light green
  while it must still be red; a scenario that merely re-entered the state
  would catch nothing.
- Assert exactly. `assert result != 'ok'` passes for a crash, a wrong error
  code, and the right one. Prefix matching is acceptable only where a
  specification allows a suffix (ADR 0027) — say so in the comment.
- Never assert "either shape is fine". If you cannot decide which is
  correct, the contract is unclear: fix the contract first.

## 3. Use an oracle stronger than the final state

In rough order of strength:

| Oracle | Catches |
|---|---|
| Equivalence between runs (`assert_equivalent_runs`) | anything an interruption or a different budget perturbs |
| A recorded trace compared step by step | state that drifts mid-run, not just at the end |
| Instrument journals | what core asked the host to do, and in what order |
| Invariants checked after every step | violations anywhere in a long run |
| The teardown leak check | memory and lifetime bugs, in every scenario |
| A single final assertion | little |

Equivalence is the workhorse for "X is invisible" claims: pausing, residency
changes, and capture/reconstruct round trips are all the same test shape
with a different interruption.

## 4. Prove the test can fail

**A new test that passes first time has not been checked.** Break core on
purpose, confirm the test fails, restore core. Every round of work in this
repo has done this, and it has repeatedly found tests that looked right:

- A scenario meant to prove that a paused actor refuses events passed
  against an implementation that accepted them — the actor's own self-send
  had already taken the single pending-event slot, so the refusal came from
  an unrelated rule. Holding the self-send in the router fixed the test.
- A test that asserted the same target text behaves identically as a literal
  and as an expression only had value once both spellings were run through
  the same table; a one-sided test would have kept passing through the
  drift.

Two traps in the mutation loop itself:

- **Commit or stash before mutating.** `git checkout -- src` after a
  mutation also discards uncommitted implementation work. This has happened.
- **Rebuild what you are testing.** `mise run amalgamate` alone does not
  rebuild core's test binaries; run the build task, or you will read a stale
  result and believe a mutant was caught (or missed).

## 5. Specifying behaviour core has not implemented

Write the scenario as the behaviour must be, then mark it:

```ruby
expect_failure 'ta_actor_capture is unimplemented', message: /: -32001 /
```

Rules that keep such markers honest:

- The message names the **real cause**, so a *different* failure is a FAIL
  rather than silently accepted.
- An XPASS is a suite failure: when core implements the function, the marker
  must be removed in the same change.
- Everything after the first failing call **has never run**. Keep helpers
  thin and exercise them against the scripted fake host in
  `tinyactors-test/ruby/runner_selftest.rb`, which checks argument names
  against the real schemas.
- Do not use an expected failure to park a harness limitation. See §7.

A C test cannot carry that marker, and not every public function is reachable
from a scenario: `tinyactors-test/bindings.rb` skips the spec-construction
tree (`ta_tree*`), because JSON specifications decode straight into the spec
tree. Write the C test anyway and list it in `PENDING_TESTS` in
`tinyactors-core/mise/tasks/build`. It is compiled against the public header
like every other test, so a renamed type or a changed signature still fails
the build and the test cannot rot; only its link may fail, and the build says
it is pending. `mise run test` runs it — and holds it to every check it makes
— from the day `src/` implements the API. `tests/tree_test.c` is the worked
example, start to finish: it was written against an unimplemented `ta_tree`,
sat in `PENDING_TESTS`, and left the list in the change that added
`src/tree.c`. Clearing the entry is part of implementing the API, exactly as
an XPASS obliges a scenario to drop its `expect_failure`. Prove such a test
the way §4 requires: write a throwaway implementation outside the repo, see
it pass, then break the implementation and see which checks fire.

A pending C test is written before the storage it describes exists, so it can
leave paths the implementation has and the test does not reach — `tree_test`
never fills a list past its first staging chunk, and fresh pages read as zero,
so it cannot tell a missing terminator from a zeroed one. Both were checked
with a throwaway program at implementation time. Cover them in the test when
the next change touches that code.

## 6. Conformance tests

`tinyactors-test/ruby/scxml_irp/baseline.json` records known IRP failures
with their exact messages. A listed test that fails differently is a FAIL; a
listed test that passes is an XPASS. After a core change, re-baseline and
read the diff: removed entries are progress, new entries need a reason.

Skips are debt with a reason attached. Prefer removing the blocker to
widening the skip list: a null data model recovered one mandatory test, and
a log sink recovered five more that were skipped only because nobody could
read the log output. Never weaken a chart to make a mandatory test pass.

## 7. Common traps

1. **Everything green on the first run.** See §4.
2. **Right symptom, wrong cause.** The assertion fires for a reason other
   than the one under test. Only a mutant distinguishes them.
3. **Vague assertions.** `!= 'ok'`, a prefix where the spec is exact, or
   filtering a journal before comparing it (which hides extra attempts, such
   as a compare-and-swap retried in a loop).
4. **Documenting a harness gap in a test comment.** If the host cannot
   express something, give the host that ability. A comment explaining the
   workaround rots and the claim stays untested.
5. **Letting the host invent a dialect.** The test host accepted `#_self`,
   which SCXML does not define, because each adapter parsed targets itself.
   Inventions diverge from core silently.
6. **The same rule implemented twice.** Core parsed literal targets while
   every data model parsed expression targets; core matches event names by
   trie ancestry while transitions match compiled patterns. Duplicates
   drift. Test both paths, and prefer deleting one (§9).
7. **Stale artifacts.** Deleted build outputs, and unrebuilt binaries.
8. **Ruby shadowing.** `allocator = recording_allocator` inside a scenario
   block shadows the DSL method of that name for the whole block.
9. **Size-query conventions.** A list read with `capacity: 0` answers
   `TA_ERR_NOMEM` with the count. That is the contract, not a failure.
10. **Shape surprises.** A finished actor has an empty configuration; some
    generated results nest as `out.out`; an instrument exists only once the
    system it backs has been created.

## 8. When to change the public API

Change core when:

- **A claim has no reachable path.** Named send targets were refused by
  core, so no test could exercise a named address; `<log>` had no sink, so
  nine mandatory tests stayed manual.
- **The same rule lives in core and in every host.** Core owns such a rule
  once, and in both directions — if core prints a URI, core parses it.
- **A host must re-implement what core already knows**, such as resolving a
  state's author ID to its `ta_state_id`, which every data model used to scan
  for (ADR 0049). Splitting a dotted event name is *not* an example: names are
  structured token paths, a segment may never contain a dot, and core takes an
  atom list by design (ADR 0049).
- **Inspection cannot show what the documentation promises.**

Do not change core when:

- the host legitimately owns the concern — where an actor lives and whether
  it is resident (ADRs 0037, 0040), storage, threads, clocks, queueing;
- the change only makes a test more convenient.

The boundary test is ADR 0031's: **semantic, not syntactic**. Parsing a URI
is syntax, so it belongs to core, which owns the resulting semantics.
"Where does this actor live right now" is host knowledge, and core cannot
answer it.

## 9. How to change the public API

1. **Write the ADR first**, and amend or supersede the one that said
   otherwise (ADR 0044 amends ADR 0040 this way).
2. **Edit `include/tinyactors.h`.** It is the whole public surface
   (ADR 0041); annotate with the `TA_*` macros so the description can record
   what C cannot express.
3. **Regenerate:** `mise run amalgamate` and `mise run api` in
   `tinyactors-core`; the `--check` variants run in the build.
4. **Regenerate the bindings** by building `tinyactors-test`, and keep
   `bindings.rb` policy honest — the generator fails on a policy entry that
   matches nothing, so new API cannot go silently unbound.
5. **Test at the right level:** pure core logic gets a C unit test in
   `tinyactors-core/tests/`; behaviour gets scenarios.
6. **Update the docs you invalidate:** `docs/internal/`, `docs/manual/`,
   `tinyactors-test/docs/protocol.md`, and the READMEs.
7. **Delete what the change makes redundant.** A fix that leaves the old
   copy in place recreates the drift it was meant to remove.
8. **Run `mise run test` at the repo root and in `tinyactors-test`.** Both
   must pass, with no XPASS.

## Checklist

- [ ] The scenario names the claim it proves (ADR, invariant, header, or W3C test).
- [ ] It carries a `# Catches:` comment.
- [ ] Assertions are exact.
- [ ] A deliberate mutation makes it fail; core was restored afterwards.
- [ ] Expected failures name the real cause, and no marker is stale.
- [ ] Docs and ADRs updated; redundant code deleted.
- [ ] Both suites pass.
