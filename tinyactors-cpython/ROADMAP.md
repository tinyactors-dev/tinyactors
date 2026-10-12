# Roadmap: tinyactors-cpython

The Python session's plan and progress, milestone by milestone, and its
requests to core. The plan of both bindings is the root
[ROADMAP.md](../ROADMAP.md), step 8: its milestones, gates and rules
decide; this file says how this binding gets there and how far it is.

**Who writes what.** The Python session keeps this file up to date as it
works and commits it with its own paths (`git commit --
tinyactors-cpython/`). It never edits the root `ROADMAP.md`. The
coordinator reads this file, moves each request below into the root's
"Requests to core" when it takes it up, and marks a milestone done there
after its audit and review.

**The milestone now: M1** (`MILESTONE` in `mise/cpython.rb`; `mise run
test` runs the suite's set for it, `--milestone M1`).

## M1: walking skeleton with the native datamodel

From the root [ROADMAP.md](../ROADMAP.md), step 8, "M1: walking skeletons
with native datamodels"; read it there, with the gates table and the
pitfalls, before changing this copy.

**Scope.** The `tinyactors` module and its `Host` type; `ta_host_create`
from Python options; images, boot, spawn, send, terminate; the loop with
every run status; `DONE`, `ERROR`, `LOG`; `request` with
`TinyactorsError`; the CBOR conversion with the value table (big
integers, bytes, `Address`, `UNDEFINED`), exposed as
`tinyactors.encode(value) -> bytes` and `tinyactors.decode(data) ->
object`; the `"python"` datamodel for `<data>`, `cond`, `assign`,
`<script>`, `_event`, `In()`, `<send>` with params, `<foreach>`; quickjs
wired in beside it; `SimulatedClock` with `increment(ms)`; and the
suite's adapter (`conform/`, `python -m conform`), which frames the
driver's calls with `tinyactors.encode` and `tinyactors.decode` and
imports nothing but `tinyactors` and the standard library.

**Gates** (each in `mise run test`):

- the suite's M1 set (`tinyactors-test/suites/milestones.txt`): every
  protocol vector file (`requests`, `config`, `envelopes`, `values`), the
  `basics` scenario and the outcome canaries (`outcome-compile`,
  `outcome-fail`, `outcome-timeout`), on the native datamodel and on
  quickjs;
- `values.txt` round trips on the native datamodel (in the set above);
- the mapping table (`docs/api-map.md`);
- the layer lints, the prefix, no mutable globals;
- the pitfalls of `AGENTS.md` ("Pitfalls M1 handles"), each with a test
  of its own in `tests/`;
- the value table's own tests, against `values.txt`'s `python` lines: the
  vectors go through this binding's codec both ways, so they cannot catch
  a value mapped wrong the same way in both directions (the root
  ROADMAP.md's "Known limit").

Then the API review, both bindings side by side (the coordinator).

### Plan and progress

The session writes its plan here as a list of steps, each with the commit
that finished it, and keeps it current. Empty until the session starts.

## Requests to core

Gaps in core or in the suite this session found. Each says what is
missing, why, and which suite case or test shows it; the coordinator
moves it into the root ROADMAP.md's "Requests to core" and answers there.

None yet.
