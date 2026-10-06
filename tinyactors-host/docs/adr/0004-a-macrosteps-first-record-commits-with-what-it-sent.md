# ADR 0004: A macrostep's first record commits with what it sent

**Status:** Superseded by [kernel ADR 0006](0006-actors-record-their-events-through-the-application-and-are-rebuilt-by-replaying-them.md) (2026-10-01), after kernel ADR 0005. Accepted (2026-10-01). Amends kernel ADR 0002 (when `delivered`, `jump`, `released` and `spawned` are written, and the time a replay publishes at) and closes the window kernel ADR 0003's notes leave open ("A window that remains"). Restores napi ADR 0033 section 7 (exactly once between persisted actors) across a crash between two commits, keeping napi ADR 0007's turn budgets, except for what a macrostep paused across turns sent before its pause, which a crash may repeat (section 3). Superseded by [kernel ADR 0005](0005-an-actors-log-is-an-inbox-with-acknowledgements.md) (2026-10-01): no first record is held; a message is marked `done` in the batch of the turn its macrostep ends in, a message not done is delivered again after a crash (at least once), and the `busy` answers and the per-actor memory of sections 1 and 2 go. Section 3's repeat becomes part of that ADR's contract. Its "back in the mailbox" is core's run queue since 4.2e ([napi ADR 0041](../../../tinyactors-napi/docs/adr/0041-host-input-goes-straight-into-core.md)).
**Date:** 2026-10-01

## Context

napi ADR 0033 section 7 promises mail between persisted actors exactly
once: a message is either delivered and logged, or queued and logged, and
the sender's replay never sends it again, because "sent is what happened"
(core ADR 0089 section 3). Batches commit whole, one at a time, in turn
order (kernel ADR 0002 section 5).

The argument needs a macrostep's input and what the macrostep sent to be
committed together. They are not:

- **A delivery and its macrostep are always in different turns.** A turn
  resumes runnable actors first, then delivers (napi ADR 0007 point 1),
  and resumes only actors that were runnable when the turn began. A
  delivered actor joins the runnable queue
  (`tah_sched_deliver_one()`), so its macrostep runs in the next turn's
  resumes at the earliest. `delivered` is written at the delivery, into
  turn N's batch; an immediate `<send>` of the macrostep is written as the
  receiver's `accepted` into turn N+1's batch, or later.
- **A crash between the two commits loses the send.** The sender's log
  has `delivered`, so its replay runs the macrostep again and drops the
  send as sent; the receiver's log never had it. Kernel ADR 0003's notes
  checked it by hand with both families logged; the spike's test
  `immediate_send_survives_a_crash_between_commits` (below) fails on
  `main` before this decision.
- **A macrostep can span several turns.** `resumeBudget` (64 microsteps)
  and `actionBudget` (1,024 units) pause a resume, and the next resume is
  in the next turn (napi ADR 0007). What a paused macrostep sent in its
  earlier turns is committed before its later turns, whatever its first
  record does.
- **So can a bootstrap.** `spawned` is written at the spawn, and the
  actor's first macrostep (its `<onentry>`) runs in a later turn's resumes
  when more actors wait than `resumesPerTurn`, or when a budget pauses it.
- **Effects that leave the process** (I/O processor sends, invoker starts,
  cancels and forwards) are held until their turn's batch commits (napi
  ADR 0033 section 7). Of a paused macrostep they are released before its
  later turns commit.

## Decision

### 1. The rule

> **A macrostep's first record is written in the batch of the turn the
> macrostep ends in, together with what the macrostep sent in that turn.**

A macrostep's first record is the one a replay starts it from:
- `delivered` or `jump`, for a delivered message or invoker report;
- `released`, for a runaway an operator released (kernel ADR 0002);
- `spawned`, for an actor's bootstrap.

The macrostep ends when a resume leaves the actor stable or finished, or
stops it with a fault (a runaway included): the scheduler's new note
`TAH_NOTE_ENDED`. A crash therefore keeps a macrostep's first record and
what it sent in its last turn together, or loses both: then its message
is still `accepted` and not `delivered` in the log, the replay puts it
back in the mailbox, and the macrostep runs again, once. A macrostep that
ends in the turn it runs in (every macrostep within the budgets) sends
exactly once; one paused across turns is section 3's case.

### 2. The first record is held

The kernel renders the first record when the macrostep starts (the
delivery, the release, the spawn), with that time, and holds it per actor.
At `TAH_NOTE_ENDED` it goes into the turn's entry for the actor's address,
with the actor's next record number, which is patched in then, so record
numbers stay in log order.

**The ordering rule.** Records for the actor written while its first
record is held:
- `accepted`, `dropped` and `fired` may be written before it: mail
  accepted for the actor, mail dropped, and its timers' firings do not
  change what a replay of the macrostep does (a firing is time only);
- anything else follows it. `finished`, `abandoned` and `destroyed` write
  the held record first;
- `evicted` answers `TA_ERR_BUSY` while a macrostep is open (napi ADR
  0032's eviction is between macrosteps anyway); so does `withdrawn`
  (`tah_system_withdraw_send()`), since writing the first record early
  would give up this decision for that macrostep, and a replay could not
  withdraw in the middle of a macrostep either.

A record written for a new actor before its held `spawned` has record
number 0, as mail waiting for a loader has: it comes before the life's
first record. The replay already holds such mail as owed (kernel ADR 0002,
"Deviations in replay").

**Time.** The first record keeps its own time (the delivery's), and may
follow records with later times: a `fired` meanwhile, or mail accepted for
the actor. A replay therefore publishes at the latest record time it read,
not the last record's (`tah_replay_last_time()`).

**Wakes and owed mail.** A delivered message whose first record is held
still counts in its address's wake (napi ADR 0033 section 4) until the
record is written: a batch committed before that one wakes the address
for it, since a crash would leave it owed. `finished` fixes the address's
wake to none.

**Out of memory.** When the held record cannot be written at the end, it
stays held, marked ended; the next turn writes it first
(`tah_inputs_retry()`), and the system is not quiet until it has. A crash
before then repeats the macrostep's sends, as in section 3. A checkpoint of an actor with such a record writes it first,
or answers out of memory.

**Removal.** An actor removed with its first record still held and no
record of the end (a binding that destroyed it without
`tah_inputs_destroying()`, or a replacement) drops the record, so its
message stays owed in the log. napi's `publish()` refuses an incumbent
inside a macrostep (`busy`), so no napi replacement reaches this.

### 3. A paused macrostep may repeat what it sent before the pause, after a crash

A macrostep that a budget pauses (`resumeBudget`, 64 microsteps, or
`actionBudget`, 1,024 units, by default: a long `<foreach>`, a large
script, a chain of eventless transitions) runs in several turns. What it
sent in a turn before its last is committed with that turn: the
receivers' `accepted` records, and its effects released after that
batch. Its first record comes later, with its last turn.

A crash between those commits leaves the message owed: the replay puts it
back, the macrostep runs again from its start, and it sends again what it
sent before the pause. **That is accepted as a fact of logged delivery:**
- nothing is lost (section 1): what the macrostep sent in its last turn,
  and everything after, is sent once;
- the sender's state is never wrong: the log never had the macrostep, so
  the replay rebuilds the sender as it was before it;
- a macrostep that ends in the turn it runs in, which is every macrostep
  within the budgets, stays exactly once;
- what repeats is only what was sent before the pause, once per crash, in
  the order it was first sent;
- outward effects of a paused macrostep (I/O processor sends, invoker
  starts, cancels and forwards) repeat the same way;
- a bootstrap paused across turns is a macrostep like any other.

**A repeat is recognizable.** Its event carries the same origin (the
sender's address, `#_actor_<family>/<name>`) and, when the `<send>` has an
`id`, the same send ID as the original. A `<send>` without an `id` gives
its event no send ID (SCXML 5.10.1), and the log's message identity
differs between the copies, so a receiver that must drop repeats keys
them on the business data they carry.

**Mitigations,** for chart authors (napi's README, "Crashes and repeated
messages"):
- do not send from a long macrostep where a repeat would hurt: loop with
  events instead, the chart sending itself `next` per item, so that each
  item is a short macrostep and each send is exactly once;
- make receivers idempotent on business keys (a reservation keyed by
  order and SKU), which also absorbs a client's HTTP retries;
- guard with states: an event that has no transition in the receiver's
  state is ignored;
- give outward effects an idempotency key built from the business key,
  for example `Idempotency-Key: order-42-charge`.

## Consequences

- **Exactly once between persisted actors holds across a crash between
  commits** for every macrostep that ends in the turn it runs in, sends of
  a bootstrap included. What a macrostep paused across turns sent before
  its pause is at least once: a crash may repeat it (section 3).
- **A logged actor's `delivered` appears in the log after the records of
  the turns its macrostep ran in,** with the delivery's time. A reader
  pairing `accepted` with `delivered` by identity (napi's `readInputLog`)
  is unaffected; one reading times in order is not. A new actor's
  `spawned` can follow mail accepted for it before its bootstrap ended.
- **Operators:** `evict()` and `withdrawSend()` of a logged actor inside a
  macrostep answer `busy`, and so does a checkpoint.
- **Costs:**
  - per logged actor between macrosteps: nothing;
  - per open macrostep of a logged actor: its first record (about 40 to
    80 bytes) in a buffer reused from the log's spares;
  - per message: nothing;
  - with the log off, nothing.

## Considered and rejected

- **A: finish the macrostep in its delivery turn.** Resume a delivered
  logged actor until it is stable before the turn ends. The spike
  measured what it costs: it gives up napi ADR 0007's fairness (a long
  macrostep holds the turn, whatever `resumeBudget` and `actionBudget`
  say), and it breaks features that rely on a macrostep pausing between
  turns: the budgets' pause points, the trace staging bound's stop between
  resumes, and an operator acting on a paused actor. It also does nothing
  for a macrostep that pauses.
- **Hold a paused macrostep's messages and effects until it ends,** and
  release them with its first record. It was built and passed the tests,
  but it batches a long `<foreach>`'s messages instead of streaming them:
  a receiver gets nothing, and waits with its later mail, until the whole
  macrostep ends. Streaming matters more than a repeat after a crash.
- **Count what a paused macrostep sent before each pause, and drop the
  first sends of the re-run.** It needs a record per pause and a match of
  the re-run's sends against it, and the same for effects: much
  complexity for a rare window, which chart authors can close themselves
  (section 3's mitigations).
- **B2: hold whole batches while a macrostep started in them is open.**
  Simple to state, but under steady load some macrostep is always open,
  so the binding could hold batches indefinitely and nothing would commit.
  Scoped per actor, it is the hold above, rejected for the same reason.
- **Log a paused macrostep's progress,** so a replay resumes where it
  paused. Core has no such record, and a macrostep is the unit a replay
  runs.

## Implementation notes

### The first record is held (2026-10-01)

Section 2, from the spike (commit b2ccaab, reviewed and renamed):
- **`inputs.c`:** `tah_inputs_hold()` renders a first record into a spare
  byte buffer with seq 0 and keeps it per actor (`tah_input_held`, found
  by `held_index`); `tah_inputs_flush()` copies it into the turn's entry
  and patches the record number in (the fixed part's `seq`). DELIVERING,
  REPORTING and RELEASING hold; UNDELIVERED lets go (core refused, no
  macrostep started); `tah_inputs_write()` applies the ordering rule
  (`tah_inputs_follows_first()`).
- **The scheduler** notes `TAH_NOTE_ENDED` after a resume that leaves the
  actor stable or finished, or stops it with a fault, before SETTLED or
  FINISHED.
- **Out of memory:** the record stays held and `ended`; `tah_inputs_retry()`
  runs first in each turn, and `tah_system_quiet()` is false while any
  waits. `tah_checkpoint_write()` writes it first
  (`tah_inputs_write_first()`), and answers `TA_ERR_BUSY` inside a
  macrostep.
- **Operators:** `tah_inputs_evicted()` and `tah_system_withdraw_send()`
  answer `TA_ERR_BUSY` inside a logged actor's macrostep.
- **Replay:** `tah_replay_last_time()` is the latest record time.
- **Wakes:** a delivered message whose first record is held keeps counting
  in its address's `mail_count` until the record is written; `finished`
  fixes the wake to none (napi's `input-log.js` caught a stale wake
  without it).

Tests (`replay_test`): `immediate_send_survives_a_crash_between_commits`
(fails before this change: sink/b stays in `h0`), and
`a_macrostep_is_not_evicted` (eviction, withdrawal and a checkpoint answer
BUSY until the macrostep ends; DELIVERED keeps the delivery's time).

### A paused macrostep's repeat (2026-10-01)

`replay_test`'s `a_paused_macrostep_repeats_its_sends_after_a_crash`
replaces the spike's `a_paused_run_sends_twice_after_a_crash`: a
macrostep paused between two microsteps sends n1, the batch with n1's
`accepted` commits, and a crash loses the batch with n2 and the first
record. After the restart sink/b gets n1 twice, then n2. In sink/b's log
the two copies of n1 carry the same event name, send ID and origin
(`#_actor_ping/p1`, with its type), and different message identities. The
same chart with `<send>`s that have no `id` gives neither copy a send ID;
the origin still matches.

### A bootstrap's first record is `spawned` (2026-10-01)

Section 1 for bootstraps:
- **`tah_inputs_spawned()`** renders `spawned` and holds it
  (`tah_inputs_hold()`) instead of writing it: the bootstrap's resume ends
  with `TAH_NOTE_ENDED` like any macrostep's, and the record goes into
  that turn's batch. Its `mark` parameter is gone:
  `tah_hook_actor_spawned()` lets go of the held record with
  `tah_inputs_removed()` when the actor cannot be scheduled.
- **Records before it:** the held record is marked `life`, and a record
  written for the actor meanwhile (mail accepted before the bootstrap
  ended, typically a post right after the spawn) has seq 0, outside the
  life; `spawned` takes 1 when it is written. The replay already held such
  mail as owed. `tah_replay_prepare()` keeps the latest time too, since
  that mail can be later than the `spawned` after it.
- **A crash before the bootstrap's batch** loses the spawn with what the
  bootstrap sent. A log that has only such mail and no life is loaded as
  napi's loader loads any log with no life: a fresh spawn, with the owed
  mail delivered to it.
- **Children started by `<invoke>`** are unnamed and never logged, so they
  have no `spawned` to hold.

Tests (`replay_test`): `a_bootstrap_send_survives_a_crash_between_commits`
(written first, failed before this change: with one resume per turn,
ping/p1's `<onentry>` send was lost with the bootstrap's batch, since its
`spawned` had committed a turn earlier) and
`mail_before_a_bootstrap_ends_is_outside_its_life`. napi's
`test/input-log.js` expects `accepted` (without `seq`) before `spawned`
for mail posted right after a spawn; `readInputLog()`'s and
`index.d.ts`'s comments on `seq` say why.

### Measurements (2026-10-01)

`mise run test:full` passed with the first record held (commit cbd8cc6)
and with `spawned` held (commit e98ae73, the repeat's test included). `scale.js` in it
reports 328.3 native B/actor at a million actors on Node, as before (kernel
ADR 0003's notes): unlogged actors pay nothing.

`mise run bench-compare a42f5f6` (main before the first record was held)
in `tinyactors-napi`, Node machine a99e44a37e2e and Bun machine
c989bd79a51f, 2 alternating runs each, medians:

| | Node | Bun |
|---|---|---|
| input log, logged, ns/event | 3,808 → 3,941 (+3.5%) | 3,384 → 3,172 (-6.3%) |
| input log, logged without data, ns/event | 1,902 → 1,937 (+1.8%) | 1,701 → 1,667 (-2.0%) |
| input log, logged, native B/actor | 1,020 → 1,129 (+10.6%) | 1,020 → 1,129 (+10.6%) |
| input log, logged, log B/event | 184 → 184 | 184 → 184 |
| input log, unlogged, ns/event | 534 → 526 (-1.7%) | 508 → 504 (-0.8%) |
| tracing and send data, native B/actor, every variant | unchanged | unchanged |

- **The logged variants' native memory, +109 B per actor,** is the held
  first records: the input-log bench delivers one message to each of its
  1,000 actors in one turn, so 1,000 macrosteps are open at once, each
  with its record's buffer, an entry in `held` and one in `held_index`.
  The buffers go back to the log's spares, and the list and the map keep
  their size, so the memory stays at the peak of open macrosteps (at most
  a turn's deliveries, `messageBudget`), not per actor: one buffer each
  per concurrently open macrostep. A leaner layout (the records back to
  back in one buffer, compacted each turn) is a ROADMAP follow-up.
- **Throughput** of logged inputs moves within the noise, in opposite
  directions on the two runtimes; log bytes per event are unchanged.
- **Flagged RSS** in the unlogged variants (tracing on Node +7.4% in one
  variant; tracing and send data on Bun +5.7% to +11.0%) comes with
  unchanged native and heap bytes, and nothing of this decision runs
  without a log: it is the runtimes' collectors, as kernel ADR 0003's
  notes found for Bun.
