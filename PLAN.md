# Plan: complete the ROADMAP (without the browser build)

**Status: phases A to J complete; phase K superseded by kernel ADR 0006 and to be rewritten as its implementation (2026-10-01).** Every commit of phases A to J is on `main`; the
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

## Phase K: an actor's log is an inbox with acknowledgements

Decided 2026-10-01 (the owner: "Level 1"): kernel ADR 0005 and napi ADR
0040 (commit `0244ac0`). With at-least-once delivery accepted, a logged
actor's log is `accepted` messages and a `done` per message, written when
its macrostep ends; a replay runs the done messages in done order and
delivers the rest again. Phase J's held first record and napi's
write-ahead hold go, with the operator records, `finished`, `evicted`,
the `busy` answers Phase J added, and `send()`'s `until`.

Rules for every commit of this phase:
- **Migrate every caller in the same commit** (AGENTS.md): a change of
  record kinds updates the kernel, napi (`lib/inputlog.js`,
  `readInputLog()`, `index.d.ts`'s `InputRecord`, its tests) and the test
  host's hosted mode (`hosted.c`, `scenario.rb`, `ruby/scenarios/hosted.rb`)
  together, so each commit passes `mise run test`.
- **A commit that changes replay, the scheduler's delivery path or
  allocation in `inputs.c`** also runs `mise run test:full` and says so.
- **Tests first where a commit closes a window:** write the crash test,
  see it fail on the commit before, then change the code.

43. **`feat(host)!: done replaces dropped, at log version 3`**
    - `TAH_WAL_DONE` with its reason in the header's `flags` (`dead-letter`
      with the dead reason, `withdrawn`); `TAH_WAL_VERSION` 3, and a
      version-2 log refused as unknown. `dropped` goes.
    - The replay forgets a message on a `done` whose reason is not
      `processed`.
    - Tests: `wal_test` (the new kind's golden bytes, version 2 refused),
      `inputs_test` (a dead letter and a withdrawn `send()` write `done`),
      `replay_test` (a dead-lettered message is not redelivered), napi's
      `input-log.js` and `replay.js`.
44. **`test(host): a delayed send's due time is the same in the replay`**
    - Kernel ADR 0005 section 3 (open): core takes a macrostep's time at
      its first resume and a live `<send delay>` reads the system clock,
      while the replay runs the macrostep at the logged delivery time.
      A `replay_test` (and a hosted scenario) with a clock that moves
      between a delivery and its first resume, and between two resumes of
      a paused macrostep: the restored actor's pending send has the live
      due time.
    - If it differs, stop and report: the fix is a core decision (the due
      time read from the macrostep's time, core ADR 0094 section 1), and
      `done`'s time follows from it. Record the finding in kernel ADR
      0005's notes either way.
45. **`feat(host)!: an operator transition is an accepted message`**
    - `accepted` carries the transition (source, targets, by state ID);
      the `jump` record goes. While Phase J's hold remains, the held first
      record of a jump is `delivered`.
    - Tests: `replay_test` (a jump replayed, a jump whose target no longer
      resolves answers `invalid`), napi `replay.js` ("a jump replayed",
      by state names in `readInputLog()`).
46. **`fix(host): a bootstrap is a message, and spawned is written at the spawn`**
    - `spawned` gains an ID and is written at the spawn, not held; the
      bootstrap's end writes the held `delivered` naming it.
    - The replay holds the bootstrap as the life's first owed message; a
      life whose bootstrap is not delivered is prepared without `at` from
      the logged binding, so the bootstrap runs live at publish, and no
      second `spawned` is written.
    - Tests (`replay_test`): `a_bootstrap_send_survives_a_crash_between_commits`
      stays green; new `a_crash_before_the_bootstrap_ends_runs_it_once_after_the_restart`
      (its `<onentry>` send reaches the receiver once, its owed mail is
      delivered after it); `mail_before_a_bootstrap_ends_is_outside_its_life`
      becomes "is in its life, owed".
47. **`fix(host)!: done replaces delivered, and nothing is held`**
    - Remove Phase J's machinery: `tah_inputs_hold()`, `tah_input_held`,
      `held`, `held_index`, `tah_inputs_flush()`'s patched record numbers,
      `tah_inputs_follows_first()`, `tah_inputs_write_first()`. Records are
      numbered in the order they are written.
    - A map of open macrosteps of logged actors (actor -> message ID and
      time, as the scheduler's `inflight`); `done` written at
      `TAH_NOTE_ENDED` into that turn's entry.
    - Out of memory: the entry is marked ended and written first in the
      next turn (`tah_inputs_retry()` stays, smaller); the actor gets no
      new message before, and the system is not quiet meanwhile; a
      checkpoint writes it first.
    - Wakes: a message counts until its `done`. The replay applies `done`
      (deliver and run). `evict()`, `withdrawSend()` and checkpoints keep
      their `busy` until commits 49 and 51.
    - Tests (`replay_test`, `inputs_test`):
      `immediate_send_survives_a_crash_between_commits` and
      `a_paused_macrostep_repeats_its_sends_after_a_crash` stay green;
      new `a_done_that_waits_for_memory_holds_the_next_message`; the
      golden life of records; the out-of-memory sweep over spawn to finish
      (exhaustive in `--full`). napi `input-log.js`: `done` follows its
      macrostep's records; the wake of a delivered, unfinished macrostep.
    - `test:full`; `mise run bench-compare <commit 46>` in napi: logged
      native B/actor should fall back towards 1,020 (kernel ADR 0004's
      notes: 1,129).
48. **`feat(host)!: a runaway's macrostep stays open until an operator ends it`**
    - A runaway stop no longer ends the macrostep for the log. Release
      sets `released` on its `done`; abandon writes `done` with reason
      `abandoned` and the count of whole microsteps it ran. The
      `released` and `abandoned` kinds go.
    - The replay runs a `released` macrostep without the limit, and an
      `abandoned` one for exactly its count (a resume with that microstep
      budget, then `ta_actor_abandon()`); count 0 delivers nothing.
    - `send()`'s `runaway` rejection is unchanged (napi ADR 0040
      section 2).
    - Tests: `replay_test` (released, abandoned after 0, 1 and several
      microsteps, a crash while a runaway waits: after the restart the
      macrostep runs again and stops at its limit again); napi
      `runaway.js` and `operate.js` record kinds.
49. **`feat(host,napi)!: a withdrawal is an input taken between macrosteps`**
    - Settle first (kernel ADR 0005 section 5, open): ahead of the
      actor's mail or behind it; a withdrawal of a runaway's send; the
      promise's result type.
    - `tah_system_withdraw_send()` queues an `accepted` withdrawal; the
      scheduler applies it between macrosteps (`ta_actor_withdraw_send()`)
      and writes `done`. The `withdrawn` kind and Phase J's `busy` go.
    - napi: `withdrawSend()` returns a promise that settles as `send()`
      does (done and committed), rejecting `not-found` or `done`;
      `index.d.ts`, README, `test/operate.js`, `test/types/usage.ts`.
    - Tests: `replay_test` (a withdrawal inside a paused macrostep waits
      for its end; a macrostep that arms the same send ID before and after
      the call replays to the live set of sends).
50. **`refactor(host)!: no finished or evicted records`**
    - The replay stops at a final state by itself; the wake knows a
      finished actor; the replay publishes at the latest record time,
      `fired` covering the timers an eviction dispatched. `evict()` still
      returns the eviction time.
    - Tests: `replay_test` (a finished history ends the load `done`; an
      evicted actor's due timers do not fire twice), hosted
      `an evicted actor is rebuilt from its input log, with its timer`,
      napi `replay.js`.
51. **`feat(host,napi): evict inside a macrostep`**
    - The open macrostep's message stays owed; the load waits for the
      commit of the batch of the eviction's turn (kernel ADR 0005 section
      5, open: settle here); redelivery skips only the IDs the kernel still
      holds queued, not every message of this incarnation (napi ADR
      0033's notes).
    - napi: `evict()` no longer throws `busy` inside a macrostep;
      `index.d.ts`.
    - Tests: napi `replay.js` (evict a paused macrostep, load: it runs
      again from its start and repeats what it sent before the pause, with
      the same origin and send ID); a hosted scenario of the same.
52. **`fix(host): a message removed without a decision stays owed`**
    - ROADMAP follow-up "A partly failed redelivery can lose mail later":
      an actor removed by an eviction, a failed load or redelivery, or
      `close()` writes no `done` for its mail.
    - Test: `replay_test`, a redelivery that fails halfway, the actor
      destroyed, a restore: every message is delivered once.
53. **`feat(napi)!: effects run when their turn asks for them`**
    - Remove `#held`, `#release()` and the held list in `#commits`;
      `#service()` runs at once. A failed commit stops later commits and
      the system's effects, and quarantines the batch's actors, as before.
    - `index.d.ts`: `setInputLog`'s and `InputLog`'s JSDoc; README.
    - Tests: `input-log.js` ("held services" and "no sink, no hold" become
      "effects run before the commit"); `replay.js` ("outward effects at
      most once across a crash" becomes "at least once").
54. **`feat(napi)!: send() resolves once its message is done and committed`**
    - The pump builds a settlement's result when it settles, and lib
      resolves it after the commit of that turn's batch, while a log is
      installed; dead letters and faults likewise; `timeout`, `aborted`
      and `closed` at once; `input-log` when the commit fails.
    - Settle first (napi ADR 0040 section 2, open): whether `flush()`
      waits for pending commits.
    - Tests (`send.js`, `input-log.js`): a commit held open keeps
      `send()` pending, and resolving it resolves the send; a failed
      commit rejects with `input-log`; a timeout while the commit is
      pending rejects with `timeout`; no log installed: unchanged.
55. **`feat(napi)!: remove send()'s until`**
    - `SendOptions.until` and its validation; `index.d.ts`, README,
      `docs/design.md`, `test/send.js`, `test/types/usage.ts`
      (`@ts-expect-error` on `until`); `mise run check-types`.
56. **`test(test): hosted scenarios for the inbox's contract`**
    - Kernel ADR 0005 section 9 across crashes at every batch boundary of
      a two-actor exchange: an immediate send (once), a paused
      macrostep's send (repeated, same origin and send ID), a bootstrap's
      send (once), a timer (fires once), a resolved send's message (done
      in the restored log).
    - The sweep over every boundary is exhaustive; the fast pass keeps
      three boundaries.
57. **`fix(host): a macrostep with a submitted send is not done until the router takes it`**
    - Only if the owner accepts kernel ADR 0005 section 8's proposal; the
      last loss window under out of memory (kernel ADR 0003 section 3).
    - Test: `inputs_test`'s sweep with a router RETRY between two logged
      actors and a crash after the sender's turn: the send arrives once.
    - `test:full`.
58. **`docs: the inbox in the READMEs, design notes and ROADMAP`**
    - napi README "Crashes and repeated messages" reworded around the
      contract, citing kernel ADR 0005 and napi ADR 0040; `docs/design.md`;
      `index.d.ts`'s `InputRecord` comment.
    - Implementation notes on kernel ADR 0005 and napi ADR 0040.
    - ROADMAP: "Done recently"; follow-ups closed ("Hold first records in
      less memory", "A partly failed redelivery", the out-of-memory window
      if commit 57 landed) or restated ("A macrostep a publish starts has
      no first record" becomes "has no message", kernel ADR 0005
      Consequences).
59. **`docs: bench-compare and test:full for phase K`**
    - `mise run bench-compare <commit before 43>` in napi on Node and Bun
      (throughput, native B/actor, log B/event, the machine signatures);
      `mise run test:full --all`. Results in kernel ADR 0005's notes and
      the ROADMAP.

Open points, and the commit that settles each (kernel ADR 0005 and napi
ADR 0040 mark them "(open)"):
- `done`'s time, and whether due times already differ between a live run
  and its replay: commit 44.
- A withdrawal's place in the queue, a runaway's withdrawal, and
  `withdrawSend()`'s result: commit 49.
- An eviction's load waiting for the eviction turn's commit; `evict()`
  with mail waiting: commit 51.
- `flush()` and pending commits: commit 54.
- A submitted send keeping its macrostep open: commit 57.
- A macrostep that `publish()` starts has no message: not in this phase;
  a ROADMAP follow-up.

## Last

33. **`docs: update the ROADMAP`**
    - Move the finished items to "Done recently", with their ADRs.

## Not in this plan

- The browser build, and the host-neutral `lib/` it needs.
- Network handoff of actors: still an open decision in napi's roadmap.
- A raw core module, bulk mutation, porting the scenario suite to the
  addon: "Later" in napi's roadmap, or only when a measurement asks.
