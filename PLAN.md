# Plan: complete the ROADMAP (without the browser build)

**Status: phases A to J complete (2026-10-01).** Every commit below is on `main`; the
gaps found on the way are in `ROADMAP.md` under "Follow-ups". Commit 12's
test landed with commit 13, and commit 31's cost was accepted (core ADR
0097, napi ADR 0039).

A series of commits on `main`. Each commit passes `mise run test` and ticks
its item in `ROADMAP.md`. A commit that touches core's hot paths, replay or
allocation also runs `mise run test:full` and says so in its message.

## Decisions taken (2026-09-30)

- **Submitted sends in checkpoints are fixed,** not accepted as
  at-most-once (commits 12 and 13).
- **The trace staging bound is recorded as "the bound plus one call".**
  No per-record check (commit 6).
- **No native durable writer.** The logged-input cost is measured again
  and recorded, and option B is not built (commit 26).
- **A pending `send()` does not hold the event loop open** (commit 5).

## Corrections to the ROADMAP

- **The rest of napi's API** is small. All 16 steps of
  `tinyactors-napi/ROADMAP.md` are done, and `index.d.ts` has no
  `// status:` markers. What remains is the granary items (Phase G), SCXML
  gap G4 and the effect-naming helper (Phase H).
- **"Core's exhaustive pass" is the test host's.** Core has no `--full`
  mode: `scale_test` and `TAT_SCALE_MAX_POPULATION` belong to
  `tinyactors-test`.
- **The `send()` keep-alive note is not in napi ADR 0033.** Napi ADR 0007
  decides what keeps the process alive.
- **No napi ADR in kernel ADR 0001 section 6 has a status-line pointer.**
  The one pointer is the collective one on napi ADR 0037.
- **Checkpoints can lose a message.** The kernel router
  (`tah_router_submit`) undoes `fired` when a send gets RETRY. A send that
  waits as "submitted" when a checkpoint is taken is then in neither the
  checkpoint nor the log. Core ADR 0091's reason to leave it out ("its
  `fired` record is in the log") does not hold under the kernel log.

## Phase A: test infrastructure

1. **`fix(test): build the test host before running scenarios`**
   - Add `#MISE depends=["build"]` to `tinyactors-test/mise/tasks/scenarios`.
   - `build` skips by content hash, so an unchanged tree costs a few hashes.
2. **`fix(mise): mark a step skipped only when its own test task was skipped`**
   - `mise/tasks/test/_default` marks a step "up to date" when any line of
     its output says `sources up-to-date, skipping`.
   - The test host's `build` runs `mise run bindings`, which prints that
     line, so a forced exhaustive run shows as skipped.
3. **`fix(mise): make each project's test:full force its run`**
   - The fast and full modes share one `outputs={auto=true}` stamp, so
     `mise run test:full` inside a project skips after a fast pass.
   - Confirm with `test:full --all` after a trivial core touch, and reword
     the ROADMAP item to name the test host.

## Phase B: records and small fixes

4. **`docs(napi): point moved ADRs to the host kernel`**
   - Status lines of napi ADRs 0003 to 0008, 0012, 0014, 0016 to 0018,
     0020, 0022 and 0032 to 0036, in the existing "Refined by ..." style.
   - Keep the line wraps on 0003 and 0014.
5. **`docs(napi): a send() timeout does not hold the event loop`**
   - Record the decision in napi ADR 0007 (with the delayed-send timer), in
     the `send()` JSDoc and `timeout` option in `index.d.ts`, the README
     and `docs/design.md`.
   - The keep-alive intervals in `test/input-log.js` and `test/replay.js`
     stay; change their comments to cite ADR 0007.
   - Remove the ROADMAP item.
6. **`docs(napi): the trace staging bound is the bound plus one call`**
   - A note in napi ADR 0036 and in the comment above `tah_keep_bytes` in
     `tinyactors-host/src/otlp.c`: the bound is checked between macrosteps
     and at the end of a call, and a per-record check would need the
     JavaScript sink inside a macrostep.
   - `test/tracing.js` (`stagingBoundOutsideTurns`) already asserts
     "bound plus 4 KiB".
7. **`fix(napi): parameterInfo() gives kernel parameters owner "kernel"`**
   - `tna_parameter_table` in `src/napi_system.c` (the `owner` line),
     `lib/params.js` and its JSDoc, `index.d.ts` (`"core" | "kernel" |
     "napi"`), the comment in `src/napi_params.h`, napi ADR 0035 and the
     README.
   - No test checks `owner` today: add `test/params.js`, on Node and Bun.

## Phase C: native replay tests in the kernel

All in a new `tinyactors-host/tests/replay_test.c`. The build finds
`tests/*_test.c`, and the `test` task's sources already include `tests/`.

8. **`test(host): replay prepare, run, finish and redeliver`**
   - Take the helpers from `tests/inputs_test.c` (`make_system`,
     `define_ping`, `spawn`, `post`, `take`, `run`, `read_log`, the failing
     platform) and add a `ta_byte_sink` into a buffer.
   - Cover `tah_replay_push` with the log cut anywhere, `tah_replay_prepare`,
     `tah_replay_run` (MORE, NEED, FINISHED), `tah_replay_finish`,
     `ta_actor_publish` and `tah_replay_redeliver` keeping message IDs.
9. **`test(host): checkpoint round trip and life boundaries`**
   - `tah_checkpoint_write` and prepare from the checkpoint; the LIFE and
     ENDED states; CORRUPT and DEFINITION_MISMATCH.
10. **`test(host): the four timelines of core ADR 0089 section 2`**
    - Timer to self; send to another actor that fired before the stop;
      send still pending at the stop; crash between commits (the
      at-least-once window).
11. **`test(host): replay survives every failed allocation`**
    - A sweep, so it is exhaustive: give the kernel's `test` task a
      `#USAGE flag "--full"`, let `test:full` pass it down, and keep a
      small version (a few allocation points) in the fast pass.
    - Add an implementation note to kernel ADR 0002 that this replaces
      Step 4b's "not repeated per binding".
12. **`test(host): a checkpoint taken while a send waits after RETRY`**
    - A send refused with RETRY (to a busy actor, or a full mailbox)
      waits, submitted, when the checkpoint is taken. Crash, restore,
      publish: the send must be delivered once.
    - Commit it marked as an expected failure, or together with commit 13
      if the test program cannot carry one.
13. **`fix(core,host)!: checkpoints restore submitted sends`**
    - A new core ADR amending 0091 section 1: a submitted send is
      restored, because under the kernel log its `fired` is written only
      by the attempt that succeeds. Implementation notes on kernel ADR
      0002 (Step 4a) and core ADR 0094.
    - Core: `ta_actor_state_restore` (and the state check before it) in
      `src/actor.c` restores submitted sends instead of skipping them; they
      go back to the router at publish.
    - Kernel: `tah_replay_read_checkpoint` in `src/replay.c` hands them
      over (`tah_checkpoint_parts` already writes them with
      `TAH_WAL_SEND_SUBMITTED`).
    - Record in the same ADR that invocations still carry no values: a
      restored invocation is never passed to its invoker again (core ADR
      0089 section 3), so nothing reads them.
    - Every project's tests run. Run `test:full`.

13a. **`fix(host): a queued message keeps its ID when numbering it fails`**
    - Found by commit 11's sweep: when `tah_inputs_number` (`src/inputs.c`)
      runs out of memory, the message is queued without an ID, and
      `tah_checkpoint_owed` leaves it out of a checkpoint, so it is lost
      after a crash; a redelivered message loses its original ID the same
      way. Refuse the input instead (nothing queued, `TA_ERR_NOMEM`), or
      reserve the number before queueing.
    - Extend commit 11's sweep to take the checkpoint while the message is
      still owed.
13b. **`fix(host): an actor whose requeue fails is not left with its mail`**
    - Found by commit 11's sweep: when `tah_sched_requeue` cannot push an
      actor onto the ready queue, the scheduler records a fault, the actor
      keeps its mail, and the system reports itself quiet.
    - Reserve the queue slot before it is needed, or keep the actor
      runnable and retry, so a quiet system never holds undelivered mail.
      Extend the sweep to fail allocations during the turns too.

## Phase D: make the kernel's headers opaque

Napi reads or writes 32 of `tah_system`'s 43 fields about 277 times; the
test host 4 fields, 15 times (`hosted.c`). About 45 to 55 accessors.
`tah_effect` and `tah_turn_record` are data handed to the binding and stay
transparent. This phase comes before Phase E, so new hosted code uses the
accessors.

14. **`refactor(host): accessors for the input log`**
    - Entry count, entry at an index (address and bytes), enabled,
      touched. Migrate `hosted.c` and napi.
15. **`refactor(host): scheduler stats, waiters and envelopes behind functions`**
    - One stats snapshot for napi's `stats()` (faults, waiters, runnable,
      ready, queued, quarantined, delivered, dead letters by reason); the
      turn records; waiter host, generation and state; envelope event and
      jump.
16. **`refactor(host): accessors for loads, OTLP, definitions, metrics, watch and params`**
17. **`refactor(host): the system's parts behind getters`**
    - Getters for core, scratch, allocator, scheduler, loads, offloads,
      inputs, OTLP, trace watch, directory, effects and params.
    - `tah_system_enter_core` and `tah_system_leave_core` replace the
      direct writes to `inside_core`.
18. **`refactor(host): callbacks in the create parameters, and clock accessors`**
    - `wake`, `child_definition`, `unreachable_data`, `quarantine_data`,
      `fault_data`, `after_core` and their contexts; clock kind, arm and
      armed.
19. **`refactor(host)!: the kernel's structs leave include/`**
    - Public headers keep forward typedefs; definitions move to private
      headers in `src/`.
    - An ast-grep rule, with its test, refuses a struct body in
      `include/tinyactors_host/` except the data types named above.
    - `mise run bench-compare <base>` in napi on Node and Bun, and
      `test:full`, since calls that were field reads are on the hot path.

## Phase E: finish the test host's hosted mode

20. **`feat(test): hosted.create takes an allocator`**
    - `hosted.c` hard-codes `tat_libc_allocator()`; accept the counting
      and budget allocators the core scenarios use
      (`AllocationFailure.tighten`).
21. **`feat(test): hosted mode inspects mailboxes and dead letters`**
    - Kernel ADR 0001 section 5. `hosted.turn` returns the turn's dead
      letters (reason via `tah_dead_reason_name`); a new `hosted.mailboxes`
      reports each actor's mailbox depth and the dead-letter counters.
    - Ruby helpers in `scenario.rb` and scenarios in
      `ruby/scenarios/hosted.rb`.
22. **`test(test): hosted timelines for a send to another actor`**
    - The two missing timelines of core ADR 0089: the send that fired
      before the stop, and the send still pending at the stop.
23. **`feat(test): a loader summons an actor in hosted mode`**
    - `hosted.set_loader` per family; `hosted.turn` drains
      `tah_loads_next_request` and answers from the Ruby store through the
      replay calls `hosted.restore` uses.
    - Scenario: mail to an evicted actor loads and delivers it.
24. **`test(test): an input is refused when the log cannot grow`**
    - With commit 20's budget allocator: `post` answers `err_nomem`, and
      the log, the actor and its mailbox are unchanged.
    - The allocation sweep version is exhaustive; keep one failure point
      in the fast pass.
25. **`test(test): hosted values are written in pieces`**
    - Count the largest write at the test host's value writer and report
      it from hosted mode; a 4 MiB value is written in pieces of at most
      8 KiB, and core's peak stays under a quarter of the value.
    - Restores the check lost with `input.recording`.

## Phase F: the cost of a logged input

26. **`test(napi): benchmark a logged input`**
    - A variant of `test/bench/` with an input log installed (1,000 actors
      × 50 messages, data `{ by: 1 }`, as ADR 0034 measured), so
      `bench-compare` covers it.
    - Measure at log version 2 on Node and Bun, with throughput, memory
      and the machine signature, and record the numbers in napi ADR 0034's
      notes with the decision: the native durable writer (option B) is not
      built.
    - Remove the ROADMAP item.

## Phase G: granary's feedback

27. **`feat(napi): typed event maps on the chart builder`**
    - `statechart<Data, Events extends Record<string, unknown> =
      Record<string, unknown>>()`: `on`, `raise` and `send` check event
      names, and `event.data` has the event's type in a transition's
      expressions.
    - `index.d.ts` and `lib/statechart.js` (JSDoc only if the runtime is
      unchanged). Add `@ts-expect-error` cases to `test/types/usage.ts`;
      `mise run check-types` runs them.
28. **`feat(napi): a reply-to-sender target`**
    - `send("reply").toOrigin()`: the target is `event.origin` and the
      processor is the event's `originType`.
    - `lib/statechart.js`, `index.d.ts` (replace the `.to(({ event }) =>
      event.origin)` example), tests on Node and Bun.
29. **`feat(core)!: actor names are percent-encoded in target URIs`**
    - A new core ADR amending 0044: `ta_scxml_uri_write` encodes `/` and
      `%` in a name, and `ta_uri_address` decodes them. `event.origin`
      then round-trips for any name.
    - Core tests; the scxml codec and IRP runs; every project's tests.
      Run `test:full`.
30. **`feat(napi): actorURI() builds an actor's address`**
    - Exported beside `addressOf` in `lib/system.js`, declared in
      `index.d.ts`, tested with names that contain `/`.

## Phase H: the rest of napi's API

31. **`feat(core,napi): event data leaving a session is a copy`**
    - SCXML C.1 (gap G4): a new core ADR for a data-model operation on
      values leaving a session; napi's data model copies them.
    - The IRP suite, `bench-compare` on Node and Bun, `test:full`.
32. **`feat(napi): a helper for naming effects`**
    - A short napi ADR for its shape first; it expands to
      `send(...).via(...)`.

## Phase I: mail between actors is never refused for a full mailbox

Decided 2026-09-30 (option B): the receiver's log accepts a message even
when its mailbox is full. A refused send was durable in neither log, so it
was lost on a replay from `spawned`, after an evict and exit, and after a
crash while its sender was evicted; core ADR 0095 fixed only the
checkpoint path.

Done (2026-10-01): 34 `0c61dd1`, 35 `a9e0be8`, 36 `192b6d5`, 37
`1dc516d`. Commit 37 found an immediate send that a crash between two
commits loses; it is a ROADMAP follow-up.

34. **`docs(host): a full mailbox overflows instead of refusing mail between actors`**
    - A kernel ADR amending napi ADR 0005's limits and kernel ADR 0002:
      the router always accepts mail between actors (writing `accepted`
      for a logged receiver); past `maxQueuedMessages` or
      `maxRetainedBytes` it waits in the receiver's overflow, in arrival
      order, and the scheduler moves it into the mailbox as room appears.
    - Defaults, open to change: no new cap on overflow (memory as before,
      when overflow waited in the senders); one rule for logged and
      unlogged receivers; mail from JavaScript keeps `queue-full`; out of
      memory can still answer RETRY.
35. **`feat(host): the router overflows full mailboxes`**
    - Kernel scheduler and router; overflow counted in the stats snapshot,
      napi's `stats()` and metrics, the test host's `hosted.mailboxes`.
    - Replay and redelivery keep overflowed mail as owed, in order.
    - Tests: kernel, test host scenarios, napi (a full mailbox, a crash,
      a replay from `spawned`, an evict and exit: delivered once).
36. **`refactor(core,host)!: remove the checkpoint's submitted-send special case`**
    - What core ADR 0095 added for full mailboxes goes if nothing needs it
      any more (`ta_actor_drop_submitted()`, the drop at `fired` and
      `evicted`); submitted sends stay only for out-of-memory retries.
      A note on ADR 0095 says what was superseded.
37. **`test(host): a crash loses a delayed send's firing and its receipt together`**
    - Commit 10's fourth timeline dropped only the logged sender while an
      unlogged receiver ran on, a state no crash produces, and asserted two
      deliveries. With both logged and crashing together, the send is
      delivered once. Correct the "at-least-once window" in core ADR 0095's
      notes the same way.

## Phase J: a macrostep's first record commits with what it sent

Found by commit 37: a delivery's `delivered` is always in turn N's batch
and the macrostep it starts runs in turn N+1 (the scheduler resumes before
it delivers), so an immediate send to another logged actor was lost by a
crash between the two commits. Decided 2026-10-01: direction B (spike on
branch `worktree-agent-a978a1fb4b1f680cc`), extended to bootstraps.
Direction A (finish the macrostep in its delivery turn) was rejected: it
gives up napi ADR 0007's fairness. A macrostep paused across turns may
repeat, after a crash, what it sent before the pause: accepted by the
owner, since holding it until the macrostep ends would stop a long
macrostep's sends from streaming.

38. **`docs(host): a macrostep's first record commits with what it sent`**
    - Kernel ADR 0004: the rule, the spike's findings, A rejected and why.
39. **`fix(host): delivered is written when its macrostep ends`**
    - The spike's change: the first record of a macrostep (`delivered`,
      `jump`, `released`) is held and written in the batch of the turn it
      ends in; the ordering rule; replay publishes at the latest record
      time; eviction answers BUSY inside a macrostep; out of memory retries.
40. **`docs(napi): a paused macrostep may repeat its sends after a crash`**
    - Landed as two commits: `test(host)`, a paused macrostep's repeat
      carries the original's origin, and its send ID when the `<send>` has
      an `id`; then the docs: kernel ADR 0004 section 3, napi ADR 0033's
      note rewording section 7, and the README's "Crashes and repeated
      messages". Holding a paused macrostep's sends until it ends was
      built, then dropped (ADR 0004, "Considered and rejected").
41. **`fix(host): a new actor's first macrostep commits with its spawned`**
    - The same rule with `spawned` as the first record, for bootstraps that
      run a turn after the spawn (`resumesPerTurn`, `<invoke>`).
42. **`docs: close the crash windows in the ROADMAP`**
    - `bench-compare` and `test:full` results in ADR 0004's notes.

## Last

33. **`docs: update the ROADMAP`**
    - Move the finished items to "Done recently", with their ADRs.

## Not in this plan

- The browser build, and the host-neutral `lib/` it needs.
- Network handoff of actors: still an open decision in napi's roadmap.
- A raw core module, bulk mutation, porting the scenario suite to the
  addon: "Later" in napi's roadmap, or only when a measurement asks.
