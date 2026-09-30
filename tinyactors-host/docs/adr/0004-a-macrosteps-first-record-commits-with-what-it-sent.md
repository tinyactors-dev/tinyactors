# ADR 0004: A macrostep's first record commits with what it sent

**Status:** Accepted (2026-10-01). Amends kernel ADR 0002 (when `delivered`, `jump`, `released` and `spawned` are written, and the time a replay publishes at) and closes the window kernel ADR 0003's notes leave open ("A window that remains"). Restores napi ADR 0033 section 7 (exactly once between persisted actors) across a crash between two commits, keeping napi ADR 0007's turn budgets.
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
  later turns commit, so a crash re-runs the macrostep and performs them
  again: at least once, where section 7 promises at most once.

## Decision

### 1. The rule

> **A macrostep's first record is written in the batch of the turn the
> macrostep ends in, together with everything the macrostep sent.**

A macrostep's first record is the one a replay starts it from:
- `delivered` or `jump`, for a delivered message or invoker report;
- `released`, for a runaway an operator released (kernel ADR 0002);
- `spawned`, for an actor's bootstrap.

The macrostep ends when a resume leaves the actor stable or finished, or
stops it with a fault (a runaway included): the scheduler's new note
`TAH_NOTE_ENDED`. A crash therefore keeps a macrostep's first record and
what it sent together, or loses both: then its message is still
`accepted` and not `delivered` in the log, the replay puts it back in the
mailbox, and the macrostep runs again, once.

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
  the held record, and what the macrostep sent (section 3), first;
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
stays held, marked ended, with the macrostep's outbox (section 3); the next
turn writes both first (`tah_inputs_retry()`), and the system is not quiet
until it has. A checkpoint of an actor with such a record writes it first,
or answers out of memory.

**Removal.** An actor removed with its first record still held and no
record of the end (a binding that destroyed it without
`tah_inputs_destroying()`, or a replacement) drops the record, so its
message stays owed in the log; its outbox is released, as the actor sent
it. napi's `publish()` refuses an incumbent inside a macrostep (`busy`),
so no napi replacement reaches this.

### 3. A paused macrostep holds what it sends: the outbox

While a logged actor's macrostep runs, the kernel notes what it sends:
- every message the kernel accepts into a mailbox, or into the wait for a
  loader, while the actor is resumed: mail between actors from the router,
  mail to itself by its name, an SCXML child's forwards, a report of an
  invocation that failed to start. Mail to unlogged receivers is included,
  so a macrostep's mail arrives in the order it was sent;
- every effect the resume queues except `<log>`: I/O processor sends and
  JavaScript invokers' starts, cancels and forwards.

**When the resume ends the macrostep,** nothing moves: the first record is
written into the same batch, beside the `accepted` records and before the
effects are handed to the binding. This is the common case, and costs one
entry in a reused list per message sent.

**When the resume pauses,** what the resume sent moves to the actor's
**outbox**:
- the `accepted` records it wrote are cut out of the turn's entries;
- the messages stay in their receivers' mailboxes, in order, but are
  **held**: a receiver whose next message is held is not delivered it,
  and waits (it is offered again on later turns, as after out of memory);
- the effects leave the turn's effect queue, copied out of the turn arena.

When the macrostep ends, its outbox is released with its first record, in
the order it was sent: the `accepted` records go into that turn's entries
with their receivers' next record numbers, the messages can be delivered,
and the effects join that turn's queue, so napi's write-ahead hold holds
them until that batch commits.

- **A held message that leaves undelivered** (its receiver destroyed,
  unloaded or finished, its load failed) leaves no record at all: its
  `accepted` was never written, and after a crash the sender's replay does
  not send it again.
- **A checkpoint** of a receiver leaves held messages out of its owed
  mail; their `accepted` follows in the log when they are released.
- **A fault ends the macrostep:** a faulted or runaway macrostep's outbox
  is released with its first record. Live behaviour does not change: the
  receivers get what the macrostep sent before the fault, and a replay
  faults at the same place and drops those sends as sent.
- **Abandoned or destroyed:** the outbox is released before `abandoned`
  or `destroyed`, as the first record is.
- **Unlogged actors are unchanged:** nothing is noted for them, and the
  log off installs no observer.

## Consequences

- **Exactly once between persisted actors holds across a crash between
  commits,** for immediate sends, sends of paused macrosteps and of
  bootstraps. Effects of a paused macrostep are at most once again.
- **Receivers of a long macrostep's sends get them when it ends,** not
  when they were sent. SCXML allows it: an external event may be delivered
  at any later time, and the sender cannot observe the difference. A
  receiver whose next message is held waits for it, so mail behind it
  waits too (first in, first out at the receiver, kernel ADR 0003).
- **A logged actor's `delivered` appears in the log after the records of
  the turns its macrostep ran in,** with the delivery's time. A reader
  pairing `accepted` with `delivered` by identity (napi's `readInputLog`)
  is unaffected; one reading times in order is not.
- **Operators:** `evict()` and `withdrawSend()` of a logged actor inside a
  macrostep answer `busy`.
- **Costs:**
  - per logged actor between macrosteps: nothing;
  - per open macrostep of a logged actor: its first record (about 40 to
    80 bytes) in a buffer reused from the log's spares;
  - per message a logged actor sends: one entry in a list reused by every
    resume;
  - a paused macrostep's outbox is bounded by what that one macrostep
    sends; its records use the log's spare buffers and its effects are
    allocated each, and freed when released;
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
- **B1 alone (the first record held, no outbox):** the spike on branch
  `worktree-agent-a978a1fb4b1f680cc`. It closes the window for a macrostep
  that ends in one resume, and turns a paused macrostep's loss into a
  duplicate: its test `a_paused_run_sends_twice_after_a_crash` delivered a
  message twice after a crash. The outbox (section 3) completes it.
- **B2: hold whole batches while a macrostep started in them is open.**
  Simple to state, but under steady load some macrostep is always open,
  so the binding could hold batches indefinitely and nothing would commit.
  Scoped per actor, it becomes this decision's outbox.
- **Stage mail at the router, and post it when the macrostep ends.** The
  router answers a chart synchronously (an unreachable target, data the
  receiver cannot log), and those answers need the post. Posting at once
  and holding the envelope keeps every answer where it is today.
- **Log a paused macrostep's progress,** so a replay resumes where it
  paused. Core has no such record, and a macrostep is the unit a replay
  runs.
