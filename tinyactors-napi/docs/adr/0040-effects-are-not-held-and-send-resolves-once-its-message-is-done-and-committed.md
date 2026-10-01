# ADR 0040: Effects are not held, and send() resolves once its message is done and committed

**Status:** Superseded in part by [kernel ADR 0006](../../../tinyactors-host/docs/adr/0006-actors-record-their-events-through-the-application-and-are-rebuilt-by-replaying-them.md) (2026-10-01): `send()` resolves when its message's macrostep ended, not once committed; effects not held and no `until` stand. Accepted (2026-10-01); implementation planned in [`PLAN.md`](../../../PLAN.md) phase K. Applies [kernel ADR 0005](../../../tinyactors-host/docs/adr/0005-an-actors-log-is-an-inbox-with-acknowledgements.md) (an actor's log is an inbox with acknowledgements) to napi's API. Supersedes in part ADR 0005 point 11 (`until`), ADR 0032 sections 6 and 8, ADR 0033 sections 1 and 7, and ADR 0034 section 5; see "Superseded decisions". Points marked "(open)" are not settled.
**Date:** 2026-10-01

## Context

Kernel ADR 0005 makes an actor's log an inbox with acknowledgements:
`accepted`, then `done` when the message's macrostep ends, and after a
crash every message not done is delivered again, at least once. Its
Context has the reasons: the end-to-end argument, the loss windows of
phases I and J, and the two holds that were traps.

What napi's API promised until now:
- **`send()`** (ADR 0005 point 11) resolves with `until: "completed"`
  (the default) when the message's macrostep ends, and with `until:
  "accepted"` when the message is queued. Neither waits for a commit.
  `tna_turn_callback()` (`src/pump.c`) settles send() promises right
  after `tah_system_turn()` and before the turn's batch is handed to the
  log; `until: "accepted"` resolves inside `send()` itself
  (`lib/system.js`), before the `accepted` record is in any batch. So a
  resolved `send()` was never durable.
- **Effects** (I/O processor sends, invoker starts, cancels and forwards,
  `<log>` records) wait, while an input log is installed, for their
  turn's batch to commit: `#service()` pushes them on `#held`,
  `#commitInputs()` seals them behind the batch, and `#drainCommits()`
  runs them when the commit resolves (ADRs 0032 section 6 and 0033
  section 1, `index.d.ts`'s `setInputLog` JSDoc). Effects are late by a
  commit and at most once.
- **Operator calls** answer `busy` inside a logged actor's macrostep:
  `evict()` (always), `withdrawSend()` (kernel ADR 0004), a checkpoint.

## Decision

### 1. Effects run when their turn asks for them

The write-ahead hold is removed. A turn's services run after its hooks,
in the order core asked for them (ADR 0007 point 5), whether or not an
input log is installed. `#held`, `#release()` and the held list in
`#commits` go; `#service()` becomes `#runService()`.

- Batches still commit one at a time, in turn order, while later turns
  run.
- **A commit that throws or rejects** still stops the system's effects
  from then on, quarantines every actor with inputs in that batch with an
  `input-log` fault (ADR 0008), and stops later commits, which cannot
  commit in order any more. What changes: the effects of that turn, and
  of the turns before it whose commits were still pending, have already
  run. Every `send()` waiting for that batch or a later one rejects with
  `input-log` (section 2).
- **`setInputLog(null)`** has nothing held to release.

After a crash, a macrostep that was not done runs again, and its effects
run again: effects are **at least once** (kernel ADR 0005 section 9,
guarantee 4), where they were at most once.

### 2. `send()` resolves once its message is done and committed

> `await system.send(target, event, data)` resolves when the message is
> done (its macrostep ended, stable or finished) and, while an input log
> is installed, the batch of the turn it was done in has committed.

- **One rule for every receiver while a log is installed:** a settlement
  waits for the commit of the batch of the turn it settled in, whether or
  not the receiver is logged. A macrostep of an unlogged actor may have
  sent to logged ones, and the rule stays one sentence. With no log
  installed it resolves when the message is done, as `until:
  "completed"` did.
- **The result is unchanged:** `{ actor, status: "stable" | "done",
  consumed, completion }`, built when the message is done (so a finished
  actor's done data is read before `finished: "destroy"` removes it, as
  ADR 0005 point 11 requires), and resolved after the commit.
- **Rejections:**
  - a dead letter and a fault reject after the commit of the batch with
    the message's `done`, as a resolution does, so that a rejection is not
    contradicted by a replay;
  - a runaway rejects with `runaway` when core stops it, as today. Its
    macrostep stays open (kernel ADR 0005 section 3): a later release
    still runs it to its end, and the rejection promises nothing durable;
  - `timeout`, `aborted` and `closed` reject at once. A message still
    queued is withdrawn (`done` with reason `withdrawn`); a message
    already taken, or done and waiting for its commit, is not undone. A
    timed-out `send()` may therefore have been processed, durably or
    not: the caller retries, and the receiver is idempotent (kernel ADR
    0005 section 9);
  - `input-log` when the commit the settlement waits for fails, or an
    earlier one did.
- **A pending send() still does not hold the process** (ADR 0007).
  (open) Whether `flush()` should also wait for pending commits, so that
  `await system.flush()` means "quiet and durable"; today it resolves
  when the native system is quiet, whatever the commits.

### 3. `until` is removed; fire and forget is `post()`

`SendOptions.until` and its validation go. A caller that wanted
`until: "accepted"` (the admission check, with a promise) calls `post()`,
which throws `queue-full` synchronously, or `send()` and does not await
it. There is one durability signal, and it is `send()`.

### 4. Operator calls

- **`evict(actor)`** works inside a macrostep: the actor is removed, its
  open macrostep's message stays owed, and the load delivers it again
  (kernel ADR 0005 section 5). It still answers `busy` for a quarantined
  actor and, for now, for an actor with mail waiting (open in kernel ADR
  0005).
- **`withdrawSend(actor, sendID)`** returns a promise: the withdrawal is
  an input taken between the actor's macrosteps, and it resolves when it
  is done and committed, or rejects with `not-found` or `done` as the
  call throws today. Misuse (an ID that is not a string, an actor of
  another system) still throws. (open) The exact shape (`Promise<void>`
  or `Promise<boolean>`), and whether a withdrawal waits behind queued
  mail.
- **`abandon(actor)`** and **`release(actor)`** keep their API. What they
  write changes: the macrostep's `done` with reason `abandoned`, or with
  `released` set (kernel ADR 0005 section 5). `abandon()` keeps its
  `busy` while the actor is paused inside a microstep: core cannot
  abandon there (core ADR 0082 section 6).
- **`send(actor, event, data, { transition })`** (an operator
  transition) is a message like any other; its `accepted` carries the
  transition.
- **A checkpoint** still answers `busy` inside a macrostep.

### 5. `readInputLog()` reads layout version 3

`InputRecord`'s kinds become `spawned` (with an `id`), `accepted` (with
`transition` for a jump, `withdraw` for a withdrawal), `done` (`id`,
`reason`, `released`, `microsteps`), `fired`, `destroyed`, and the
checkpoint's kinds. `delivered`, `dropped`, `jump`, `withdrawn`,
`abandoned`, `released`, `finished` and `evicted` go. An audit reader
pairs `accepted` with `done` by `id`.

### 6. What chart authors and applications are told

napi's README, "Crashes and repeated messages", is reworded around the
contract (kernel ADR 0005 section 9) and cites it: a message not done is
delivered again after a crash; a macrostep run again may repeat its sends
and effects; a repeat has the same `origin`, and the same `sendid` when
its `<send>` has an `id`; so time out and retry end to end, make
receivers idempotent on business keys, give outward effects idempotency
keys, and guard with states. Looping with events stays the advice for
long work: sends to persisted actors from a macrostep that ends within
the turn budgets are exactly once, since its sends and its `done` commit
together.

## Consequences

- **Effects are no longer late by a commit,** and no longer lost by a
  crash between a commit and their release; they can be repeated instead.
- **An awaited `send()` is durable,** and costs the caller the commit's
  latency. A caller that does not need durability calls `post()`.
- **Breaking API changes,** all inside this repository (AGENTS.md: change
  in place): `SendOptions.until` removed; `withdrawSend()` returns a
  promise; `evict()` no longer throws `busy` inside a macrostep;
  `InputRecord` at version 3. `index.d.ts`, `lib/system.js`, the README,
  `docs/design.md`, `test/types/usage.ts` and the tests change together.
- **Tests that change:** `test/send.js` (the `until` cases), 
  `test/input-log.js` (the held services and "no sink, no hold" become
  "effects run before the commit"; the record kinds), `test/replay.js`
  ("outward effects at most once across a crash" becomes "at least
  once"), `test/operate.js` and `test/runaway.js` (withdrawal, eviction,
  abandon and release records).
- **The `docs/research/dasp.md` and `docs/proposals/debug-adapter.md`
  mentions of `until`** are a record and a proposal; they stay as written.

## Considered and rejected

- **`until: "committed"` beside the other two** (kernel ADR 0005,
  "Options considered"): three meanings for one call, two of them not
  durable.
- **Resolve `send()` at `done` and let the caller await a separate
  `system.committed()`.** Two steps where every caller that cares wants
  both, and one that does not has `post()`.
- **Keep the write-ahead hold for effects only.** It keeps effects at most
  once, which the end-to-end measures do not repair (a lost payment
  request is not retried by anyone), and keeps them late.

## Superseded decisions

- **ADR 0005, point 11:** `until: "completed"` and `until: "accepted"`;
  the settlement now waits for the commit (section 2).
- **ADR 0032, section 6:** the "Write-ahead" points (effects held until
  commit); **section 8:** `busy` inside a macrostep.
- **ADR 0033, section 1:** "nothing the turn caused leaves the process
  until the application has made that batch durable"; **section 7** and
  its 2026-10-01 rewording: effects at least once, mail at least once
  (kernel ADR 0005 section 9).
- **ADR 0034, section 5:** the vocabulary `readInputLog()` decodes.
- **`index.d.ts`:** `setInputLog`'s JSDoc on held services; `send()`'s
  JSDoc and `SendOptions.until`; `withdrawSend()` and `evict()`; the
  `InputRecord` comment citing kernel ADR 0004.
