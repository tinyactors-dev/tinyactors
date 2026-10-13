---
name: reasoned-analysis
description: Answer design and analysis questions with checked claims instead of general knowledge. Use when asked for pitfalls, risks, trade-offs, "does X make sense", "what can we leverage", "why", a review of a plan, any claim about memory, lifetimes, ownership, threads or reentrancy, or before proposing a change to core, an ADR, the roadmap or a binding's design. Applies to your own answers in conversation as much as to delegated analyses. Each claim carries its evidence or is marked unverified, every finding has a concrete scenario in this repo's design, and recommendations survive an adversarial check before they reach the user.
---

# Reasoned analysis

General knowledge about a platform (CPython, Node-API, Bun, SCXML) is a
list of candidates, not an answer. An answer says which candidates matter
**here**, in this repository's design, and why. Confident wording on an
unchecked claim costs the maintainer's trust; a smaller answer that holds
is worth more than a long one that does not.

## How it went wrong before

The rules below come from one answer that had to be retracted:

- A generic pitfall (Ctrl-C raising inside an expression) was escalated to
  "the gap" and then to "a request to core". A grep confirmed that core had
  no fatal status for a datamodel, but nobody asked whether it mattered. In
  a server, a signal is an event on the loop, a run is bounded by its
  budget, and graceful shutdown happens between turns. One scenario would
  have dropped the item.
- A platform name (`tp_traverse`, a CPython type slot) was written as if it
  were ours, without saying whose it was. Our functions carry our prefix
  (`ta_python_*`, `ta_napi_*`); the platform's names appear only where the
  platform requires them.
- A table row ("pointers are fine in a binding") contradicted the plan it
  was commenting on (both bindings keep handle tables for what core
  stores). The claim was not checked against `ROADMAP.md` or
  `include/tinyactors/datamodel.h`.

A second set of rules comes from a design discussion about the binding
boundary (2026-10-12) that went back and forth for hours:

- Hazards were imported from general knowledge ("finalizers run code",
  "threads free things", "memory moves", "async I/O keeps buffers")
  without checking that the objects they need exist here. This host is
  owned by one thread; the API hands out integers, not resource objects;
  a binding's `send()` runs its turn before it returns. Each fact was in
  the repository; none was looked up before the claim.
- Positions came first and reasons second: three reasons for "records
  need tail offsets" were produced to defend a sketch, and none survived a
  lifetime trace.
- New mechanisms were proposed (a ring, `ta_host_step`, a posted queue,
  generated accessors) where an existing call already did the job
  (`ta_host_send_many` plus `ta_host_run`; answering a request at the
  call). Each was withdrawn only when the user asked "why doesn't the
  existing call work?".
- A design abandoned in this repository's own history (the ring
  protocol, ADR 0004) was re-proposed without reading why it went.
- The design was rebuilt in every reply from a partial picture, so
  answers contradicted each other; "almost the same" was said without
  saying what differed.

## Steps

0. **Fix the frame first, in writing.** Before answering, write the facts
   the answer depends on and check each in the repository: who owns the
   host (one thread), who calls what and when (the binding's loop, guide
   section 3), what the API hands out (ids, values, objects), which
   layers exist (`AGENTS.md`, `tinyactors-core/AGENTS.md`). Every later
   claim is checked against this frame; a hazard that needs something
   outside it (another thread, another process, a resource object the API
   does not have) is not a finding. When the user corrects the frame,
   rewrite it and redo the answer from it, rather than patching the last
   reply.

0b. **Start from what exists.** Before proposing a mechanism, find the
   existing call, struct or rule that does the job (`include/tinyactors/`,
   `docs/requests.md`, the guide) and say why it does not suffice, with a
   scenario. If you cannot say why, the existing call is the answer.
   Search the history too: `git log`, `docs/adr/`, `docs/proposals/`,
   `docs/research/`; a design tried before carries the reason it went.

1. **List candidates, then test each one.** Brainstorming from general
   knowledge is fine as a first pass. It is not the answer.

2. **Write the scenario.** For each candidate, write the concrete situation
   in this repository where it bites: who calls what, in which order, on
   which loop, with which host state (for example: a server drives three
   hosts and receives SIGTERM). Walk it through the guide
   (`tinyactors-core/docs/bindings.md`), the headers and the ADRs. A
   candidate without a scenario is not a finding: drop it, or list it under
   "not applicable" with the reason.

3. **Attach evidence to every claim.** One of:
   - `path:line` in this repository;
   - a primary source (the platform's own documentation, its source, an
     issue in its tracker), with a link;
   - the result of a spike you ran (below);
   - otherwise the word **unverified**, in the answer, next to the claim.

   A grep proves that something is or is not in the code. It does not prove
   that its absence matters; the scenario does.

4. **Settle runtime behaviour with a spike.** A claim about what a runtime
   does ("Bun lacks `uv_timer_*`", "a `Py_DECREF` during a run reenters the
   host") is decided by a small program, not asserted. Write it in
   `$CLAUDE_JOB_DIR/tmp` or the scratchpad, run it, and quote its output. If
   a spike is too costly now, mark the claim unverified and say what would
   settle it.

4b. **Trace lifetimes in a table before any claim about memory.** For
   every object the claim involves, one row: the object, its owner, when
   it is created, who reads it and when, when it ends, on which thread.
   Use the real calls of this API (`ta_host_send` reads its views during
   the call; effects and the host's outputs live until
   `ta_host_effects_ack`, ADR 0012; a datamodel's output until its next
   call). Make the claim only if a row shows it;
   otherwise say "untraced". A difference ("almost the same") is stated as
   the row that differs.

5. **Check the claim against the plan.** Before saying something is missing
   or wrong, read what `ROADMAP.md`, the ADRs, the guide and `AGENTS.md`
   already say about it. Say where your answer agrees and where it differs.

6. **Say whose name it is.** When you write an identifier, it is either
   ours (with our prefix and naming rules) or the platform's (say so: "the
   CPython slot `tp_traverse`, filled with `ta_python_host_traverse`").

7. **No escalation without a scenario that survives.** Do not propose a
   change to core, an ADR, a request to core or a roadmap edit on a
   candidate that has not passed steps 2 to 5.

8. **Refute before you recommend.** For an answer that recommends changes
   or will be acted on, give the findings to a fresh agent with no stake in
   them, and ask it to refute each one against the repository and the
   primary sources. Report only what survives, marked **confirmed**
   (evidence and scenario hold) or **plausible** (scenario holds, evidence
   incomplete, with what would settle it). Mention what was dropped and why,
   in one line each. A fork cannot spawn agents: when the analysis runs in
   a fork, the session that started it runs this step, and the fork's
   findings stay unconfirmed until it has.

9. **Word it as checked.** Confident wording only for confirmed claims.
   Keep the answer as short as what survives; do not pad it with the
   candidates that did not.

## Shape of the answer

For each finding:

- **Claim**, one sentence.
- **Scenario** in this repository's design.
- **Evidence**: `path:line`, a link, or spike output.
- **Verdict**: confirmed, plausible (and what settles it), or not
  applicable (and why).
- **Action**, if any, and where it goes (a binding's `AGENTS.md`, the
  guide, `ROADMAP.md`, "Requests to core").

When a correction from the user shows a claim was wrong, say what was wrong
and why, plainly, and fix the source of the error (the claim, the
document), not only the wording. Then check the rest of the answer
against the corrected frame: one wrong assumption usually carries others.

A "why?" from the user is a request for the trace, not for more reasons:
answer with the table or the call path that decides it, and withdraw the
claim if they do not.
