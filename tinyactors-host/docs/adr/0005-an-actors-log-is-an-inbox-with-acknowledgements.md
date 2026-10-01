# ADR 0005: An actor's log is an inbox with acknowledgements

**Status:** Superseded by [kernel ADR 0006](0006-actors-record-their-events-through-the-application-and-are-rebuilt-by-replaying-them.md) (2026-10-01): the runtime records events and the application journals and replays them; this ADR was never implemented. Accepted (2026-10-01); implementation planned in [`PLAN.md`](../../../PLAN.md) phase K. Supersedes kernel ADR 0004 as a whole, and kernel ADR 0002 in part (sections 2, 5 and 6; see "Superseded decisions" below). napi ADR 0040 decides what napi's API does with it (`send()`, effects, operator calls). Points marked "(open)" are not settled; PLAN.md phase K says where each is settled.
**Date:** 2026-10-01

## Context

### Where the log stands

Kernel ADR 0002 gave every message for a logged actor an identity and
logged it as `accepted`, then `delivered` or `dropped`. The operator's
actions (`jump`, `released`, `withdrawn`, `abandoned`) and the lifecycle
(`spawned`, `finished`, `destroyed`, `evicted`) are records of their own.
napi ADR 0033 section 7 promised, on top of it:
- mail between persisted actors **exactly once**;
- effects that leave the process (I/O processor sends, invoker starts,
  cancels and forwards) **at most once**, because a turn's effects wait
  for its batch's commit (the write-ahead hold, napi ADRs 0032 section 6
  and 0033 section 1).

Two phases of work went into keeping those promises, and each found a
window where a crash loses a message:
- **Phase I** (kernel ADR 0003): a full mailbox refused mail between
  actors, and the refused send was durable in neither log. Fixed by
  accepting it into an overflow. That fix stays: it prevents loss while
  the system runs, crash or no crash.
- **Phase J** (kernel ADR 0004): a delivery's `delivered` was in turn N's
  batch and the macrostep it starts ran in turn N+1, so an immediate send
  was lost by a crash between the two commits; the same for a bootstrap
  and its `spawned`. Fixed by holding the macrostep's first record in
  memory and writing it when the macrostep ends, with an ordering rule,
  patched record numbers, `busy` answers from `evict()`, `withdrawSend()`
  and checkpoints inside a macrostep, a retry for when the held record
  cannot be written, and +109 B per logged actor at the peak of open
  macrosteps. It still let a macrostep paused across turns repeat what it
  sent before the pause (ADR 0004 section 3): exactly once had become
  "exactly once, except".

### The two holds were each a trap

- **The macrostep hold.** Holding what a paused macrostep sends until it
  ends would have made it exactly once. It was built, and dropped: it
  batches a long `<foreach>`'s messages instead of streaming them (kernel
  ADR 0004, "Considered and rejected"). The first-record hold that
  replaced it is the machinery listed above.
- **The write-ahead hold.** It delays every effect of a turn by a commit.
  It makes effects at most once: a crash after the commit and before the
  effect ran loses it, and replay does not repeat it, since its cause is
  committed. And it does not cover the acknowledgement callers actually
  use, the `send()` promise:
  - **Finding (2026-10-01): `send()` resolves before its batch commits.**
    napi's turn (`tna_turn_callback()` in `tinyactors-napi/src/pump.c`)
    settles send() promises (`tna_settle_sends()`) right after
    `tah_system_turn()`, before it hands the turn's batch to the input
    log (`tna_inputs_dispatch()`), and the commit resolves later still,
    asynchronously. A caller that saw `await send()` resolve can lose, in
    a crash, the record that says the message was processed.
  - **`until: "completed"`** (the default) resolves when the macrostep the
    message starts ends. Under kernel ADR 0004 its `delivered` is in that
    turn's batch, which has not committed yet.
  - **`until: "accepted"`** resolves synchronously in `send()`
    (`Promise.resolve` in `lib/system.js`), once the message is queued:
    its `accepted` record is not even in a batch yet.

  So the hold made effects late and at most once, and the promise that
  tells a caller "done" was never durable.

### The end-to-end argument

Reliability that the endpoints must implement anyway is only an
optimization in a lower layer (Saltzer, Reed and Clark, 1984). A client
of an application built on tinyactors already has to time out and retry,
because the network loses requests and replies; a receiver of its
retries already has to recognize repeats; an outward effect (a payment, an
email) already needs an idempotency key, because the service at the other
end can fail after acting. Those end-to-end measures also absorb a
message or an effect repeated after a crash. They cannot absorb a message
that is lost, or an actor state that is wrong.

The owner accepted at-least-once delivery on 2026-10-01 ("Level 1", "the
radical simplification we needed"). Storage stays the application's
(napi ADR 0033). What the log must then guarantee is small: nothing an
actor received is lost, and its state is rebuilt exactly. That is an
inbox with acknowledgements.

## Decision

### 1. What the log is

> An actor's log is its **inbox with acknowledgements**. A message goes
> in (`accepted`, with its data) and is marked **done** once processed,
> in the batch of the turn its macrostep ends in. After a crash, every
> message that is not done is delivered again. A replay rebuilds the
> actor by running the done messages again, in the order they were marked
> done, and then delivers the rest.

Replay stays core ADR 0089's: a replaying actor crosses nothing, and "sent
is what happened" (section 3) holds for every done message, because its
sends and effects happened before its `done` was committed (section 9).

### 2. The records, at layout version 3

| Record | When it is written | Carries |
|---|---|---|
| `spawned` | a logged actor is spawned: the life's first message, its bootstrap | **id**, family, name, revision; value: binding |
| `accepted` | a message enters the actor's inbox: mail between actors, a `post()` or `send()`, a delayed send that fired, an invoker report, an operator's withdrawal (section 5) | id, event name, type, send ID, origin, origin type, invoke ID, report kind, reason, ends, **the operator transition** (source, targets) when it carries one, **the send ID to withdraw** for a withdrawal; value: data |
| `done` | message `id` is processed, or will never be: section 3 | id, **reason**, **released** flag, **microsteps** (for `abandoned` only); the header's time is the time the replay delivers it at |
| `fired` | a logged actor's delayed send was dispatched, to any target | send ID |
| `destroyed` | `destroy()` of a logged actor: the life ends | none |

The checkpoint unit (`checkpoint`, `checkpoint-send`, `checkpoint-invoke`,
then an `accepted` per owed message) is unchanged (kernel ADR 0002
section 7), except that "owed" now means "not done".

**Removed kinds:** `delivered` and `dropped` (both are `done`), `jump`
(an `accepted` with a transition, then `done`), `withdrawn` (an
`accepted` withdrawal, then `done`), `abandoned` and `released` (the
reason and flag of the macrostep's `done`), `finished` (a replay reaches
the final state by itself, and the wake knows it), `evicted` (the replay
publishes at the latest record time, and `fired` records say which timers
went: an evicted actor's due sends were dispatched, each with its
`fired`).

**`done`'s reason:**

| Reason | Meaning | Replay |
|---|---|---|
| `processed` (0) | the message's macrostep ended (stable, finished, or a fault) | delivers it and runs the actor until idle |
| `abandoned` | an operator abandoned its macrostep after `microsteps` whole microsteps (0: before it was taken) | delivers it, runs exactly `microsteps` microsteps, abandons |
| `dead-letter` | it became a dead letter (the dead reason is in the reason's low bits) | forgets it |
| `withdrawn` | a `send()` timeout or abort withdrew it while it was queued | forgets it |

`released` is set on a `processed` or `abandoned` done whose macrostep an
operator released from its microstep limit (a runaway): the replay runs
it without the limit.

The version is 3. **No stored log is read across the change:** nothing is
published, so a version-2 log is refused as an unknown version, as a
version-1 log is today.

### 3. When `done` is written

- **A delivered message, a report, a jump, a bootstrap:** when its
  macrostep ends (`TAH_NOTE_ENDED`: the actor is stable or finished, or
  the macrostep stopped with a fault), into that turn's batch, in the
  order records are written. Nothing is held rendered, no record number
  is patched, and no ordering rule applies: records are numbered in the
  order they are written.
- **A runaway's macrostep does not end at its limit.** Core stops it
  between two microsteps (core ADR 0075) and it stays open until an
  operator releases it (it then ends as any macrostep does, with
  `released` set) or abandons it (`abandoned`, section 5). Its message
  is not done meanwhile.
- **A message that is never delivered:** when it becomes a dead letter
  (`dead-letter`), or a `send()` timeout or abort withdraws it
  (`withdrawn`), as `dropped` was written.
- **A message whose actor is removed without a decision to end it** (an
  eviction, a failed load or redelivery, `close()`) gets **no** `done`: it
  stays owed in the log, and the next load delivers it. This closes the
  ROADMAP follow-up "a partly failed redelivery can lose mail later",
  where such messages were logged as `dropped`.

**What the kernel keeps per open macrostep** of a logged actor: the
message's ID and the time `done` carries, 16 bytes, in a map keyed by
actor, as the scheduler's `inflight` map keeps a `send()` waiter. It
replaces Phase J's rendered record, buffer, list and index.

**The time `done` carries** is the delivery's time, the one
`ta_actor_deliver()` was given, as `delivered` carried. (open) Core takes
a macrostep's time at its first resume (`ta_take_macrostep_time()`), and
a live actor's delayed sends read the system's clock at the `<send>`
(`ta_actor_now()`), which under a realtime clock can be later than the
delivery's time, since the macrostep runs a turn after its delivery and a
paused one runs over several turns. A replay runs the whole macrostep at
the logged time. Whether that already makes due times differ between the
live run and the replay, against core ADR 0094 section 1's "the
macrostep's time is the input's time", is to be checked (PLAN.md
commit 44); it is not caused by this decision, and a fix belongs in core.

### 4. The replay rule

> **Replay the done messages in the order they were marked done, then
> deliver the rest.**

The driver reads the log in order, one record at a time, as today:
- **`spawned`** prepares the actor (`ta_actor_prepare()`) with its
  binding, at its time, and holds the bootstrap as the life's first owed
  message;
- **`accepted`** holds the message as owed;
- **`done`** takes its message and applies it by its reason (section 2's
  table): a delivery (with its transition, for a jump), an invoker report
  (`ta_actor_report_invoke()`), a withdrawal (`ta_actor_withdraw_send()`),
  or the bootstrap; then runs the actor until idle, within the pump's
  budgets;
- **`fired`** passes time only;
- **`destroyed`** ends the life: the prepared actor is dropped, and the
  replay starts again at the next `spawned`;
- **a checkpoint** gives the state and the owed messages (kernel ADR 0002
  section 7);
- **a final state** reached by a done message ends the replay (`done` for
  the load, as today);
- **at the end:** publish at the latest record time (records can still
  follow records of later times, since `done` is written after its
  macrostep's later records), then deliver the owed messages again, in
  identity order, then the mail that waited for the load.

Since an actor runs one macrostep at a time and `done` is written when it
ends, **done order is log order**. The replay needs nothing else to order
by, and holds only the owed messages, as today.

**A bootstrap that is not done.** When a life's `spawned` has no `done`
(a crash came before its first macrostep's batch), the actor is prepared
**without** `at` from the logged binding, so its bootstrap runs live at
publish (core ADR 0089 section 6: a prepared actor without `at` releases
its held effects at publish), and its `done` is written when that
macrostep ends. No second `spawned` is written. Every message accepted in
that life is owed, since none can be done before the bootstrap.

**Redelivery skips only mail still in memory.** napi ADR 0033's notes
redeliver only another incarnation's messages. Under this decision a
message of this incarnation can be owed too (its actor was evicted inside
its macrostep, section 5), so the replay skips exactly the identities the
kernel still holds queued (its ID maps), and redelivers every other
message that is not done.

### 5. Operator actions

Each operator action is an input in the inbox (accepted, then done) where
it can wait for its turn, and a reason on the `done` of the macrostep it
acts on where it cannot.

| Action | Before | Now | Why |
|---|---|---|---|
| operator transition (jump) | `jump` record replacing `delivered` | an `accepted` carrying the transition, then `done` | it already was a message in the mailbox (core ADR 0082 section 1); only its record was special |
| release a runaway | `released`, the continued macrostep's first record | the `released` flag on that macrostep's `done` | it acts on a macrostep in progress, which a message cannot do: an input waits for the macrostep to end, and a runaway's never ends by itself |
| abandon a macrostep | `abandoned` | `done` with reason `abandoned` and the number of whole microsteps it ran | the same: it ends a macrostep in progress. The count lets the replay stop the macrostep where it was abandoned (resume with a microstep budget, as core ADR 0082 section 6 describes, then `ta_actor_abandon()`). A macrostep abandoned before it was taken has count 0 and is not delivered |
| withdraw a pending delayed send | `withdrawn`, written at once; `busy` inside a logged actor's macrostep (Phase J) | an `accepted` withdrawal, taken between macrosteps ahead of the actor's mail, then `done` | applied at once inside a paused macrostep, no position in the log reproduces it: the macrostep may arm the same send ID before and after the withdrawal, and a replay that withdraws before or after the whole macrostep keeps a different set of sends. Taken between macrosteps, its place in done order is exact |
| evict | `evicted`; `busy` inside a macrostep | no record; allowed inside a macrostep | the message of the open macrostep is not done, so the load delivers it again and the macrostep runs again from its start (section 9, guarantee 4) |
| destroy | `destroyed` | `destroyed`, unchanged | it ends a life rather than feeding the chart, and the replay must see where the life ends to start a fresh one |
| checkpoint | `busy` inside a macrostep | `busy` inside a macrostep, unchanged | core cannot export a macrostep in progress (core ADR 0091) |

**A withdrawal's answer.** Taken between macrosteps, a withdrawal can no
longer answer synchronously whether the send was pending: `withdrawSend()`
becomes asynchronous (napi ADR 0040). (open) Whether a withdrawal jumps
ahead of the actor's queued mail (proposed: yes, as an operator's action
on a timer that may otherwise fire first), and whether a withdrawal of a
runaway's pending send, which now waits until the operator releases or
abandons the runaway, needs a way around that wait (core ADR 0084 section
3 allowed it inside a stopped macrostep, "the moment the operator is
looking at it").

**Eviction inside a macrostep.** (open) The load that follows must not
read the store before the batches written up to the eviction have
committed, or it can miss the open macrostep's `accepted` and not
redeliver it. Proposed: an eviction's load waits for the commit of the
batch of the turn it was evicted in. Whether `evict()` should also stop
answering `busy` for an actor with mail waiting (that mail is owed in the
log too) is left open; this decision removes only the `busy` inside a
macrostep.

### 6. What is removed

- **Phase J's first-record hold** (kernel ADR 0004 sections 1 and 2): the
  held rendered record, `tah_inputs_hold()`, the `held` list and
  `held_index` map, `tah_inputs_flush()`'s patched record numbers, the
  ordering rule (`tah_inputs_follows_first()`), holding `spawned`, and
  its per-actor memory (+109 B per logged actor at the peak of open
  macrosteps). What remains of the retry is section 8's.
- **The write-ahead hold** (napi ADRs 0032 section 6 and 0033 section 1,
  kernel ADR 0002 section 5's third point): a turn's effects run when the
  turn asks for them. The hold's purpose, never acknowledging what the log
  then forgets, moves to the one durability signal, `send()` (napi ADR
  0040).
- **`send()`'s `until` variants** (napi ADR 0005 point 11): `send()`
  resolves when its message is done and, with a log installed, committed;
  fire and forget is `post()` (napi ADR 0040).
- **The operator records** `jump`, `released`, `withdrawn`, `abandoned`,
  and the `busy` answers Phase J added to `evict()` and `withdrawSend()`
  inside a macrostep (section 5).
- **`finished` and `evicted`** (section 2).

### 7. Wakes

A wake stays the earlier of an actor's first pending delayed send and its
oldest message that is **not done** (napi ADR 0033 section 4 said "not
delivered"; Phase J made a delivered message count until its first record
was written, which is the same rule). A message counts from its
`accepted` until its `done`: a runaway's open macrostep keeps its message
counting, so after a crash the actor is loaded and the macrostep runs
again, stops at its limit again, and waits for an operator again. An
actor that finished or was destroyed gets `null`, as before; the kernel
knows it finished without a `finished` record.

### 8. Out of memory

Without the holds, fewer paths can fail, and none of those that remain
loses a message:
- **`accepted` cannot be written:** the input is refused, as today
  (`post()` answers out of memory, the router RETRY), and nothing changed.
- **`fired` cannot be written:** the router answers RETRY, and the send
  stays due, as today.
- **`done` cannot be written** when the macrostep ends: the kernel keeps
  the message's ID and time (they are in the open-macrostep map already),
  marks it ended, and writes it first in the next turn; the actor is not
  given its next message before, since a later `done` must not precede
  it, and the system is not quiet meanwhile. A crash meanwhile runs the
  macrostep again (guarantee 4). This is what Phase J's retry did, without
  a rendered record.
- **A checkpoint, a replay or a redelivery** answers out of memory and
  changes nothing, as today; a redelivery that fails no longer loses mail
  (section 3).
- **Gone:** the held first record that could not be written, and effects
  lost because the hold's batch failed.
- **The one window left: a send the router refuses for memory.** It
  stays submitted in the sender, logged nowhere (kernel ADR 0003 section
  3), while the sender's `done` commits; a crash then loses it, since the
  replay treats it as sent. (open) Proposed: a macrostep that leaves a
  submitted send is not done until the router has taken every such send,
  as a `done` that cannot be written waits: the actor waits too, and a
  crash runs the macrostep again. That closes the last loss window at the
  cost of an actor waiting while memory is short.

### 9. The contract

What the kernel guarantees for a named actor of a logged family, with an
input log installed and every batch committed in order:

1. **An actor's state is never corrupted.** A replay runs exactly the
   done messages, in done order, deterministically (core ADR 0089, within
   its Scope), and a checkpoint is exact (core ADR 0091). A message not
   done never contributed to the rebuilt state.
2. **What `send()` resolved is durable.** It resolves once its message is
   done and the batch holding its `done` has committed (napi ADR 0040).
3. **Timers survive a crash.** A pending delayed send is rebuilt by the
   replay or kept in a checkpoint; `fired` says which ones went, so none
   fires twice because of the replay, and the wake brings the actor back
   when the first is due.
4. **Nothing an actor received is lost.** A message not done is delivered
   again after a crash, a failed load, or an eviction inside its
   macrostep: **at least once**. A macrostep run again may repeat what it
   sent or did before the crash:
   - mail to another persisted actor, when the macrostep was paused
     across turns (what it sent in its last turn commits with its `done`,
     so a macrostep within the turn budgets sends to persisted actors
     exactly once);
   - effects that leave the process (I/O processor sends, invoker starts,
     cancels and forwards, `<log>` records), which now run before their
     batch commits. The kernel guarantees that an effect of a done
     macrostep was handed to its I/O processor or invoker before the
     `done` committed; what the processor does after the hand-off is its
     own.
5. **Repeats are recognizable.** A repeated event carries the same
   `origin`, and the same `sendid` when its `<send>` has an `id`
   (verified in commit `7b5e543`, `replay_test`'s
   `a_paused_macrostep_repeats_its_sends_after_a_crash`). Its log
   identity differs.

The exception (section 8, open): a send the router refused for memory,
until that proposal is built.

**What applications do,** end to end, for what matters (napi's README,
"Crashes and repeated messages"):
- clients time out and retry;
- receivers are idempotent on business keys (a reservation keyed by
  order and SKU), which also absorbs clients' retries;
- outward effects carry idempotency keys built from business keys
  (`Idempotency-Key: order-42-charge`);
- charts guard with states: an event with no transition in the current
  state is ignored.

## Options considered

- **Level 2: store an actor's state after each macrostep, instead of
  replaying its inputs.** Rejected for now:
  - write amplification: an actor with a large data model writes all of
    it for every message, where the log writes the message;
  - it reverses core ADRs 0089 to 0091 (replay, the log's layout,
    checkpoints as the log's compaction), which are built and tested;
  - it loses the log as an audit trail of what each actor received.

  Checkpoints already give a bounded-length log where one is wanted.
- **Keep the holds** (Phase J's first-record hold, napi's write-ahead
  hold). Rejected: they are the machinery section 6 lists, they still
  left "exactly once, except", the write-ahead hold made effects at most
  once and late, and neither made the `send()` promise durable.
- **An `ended` record beside `delivered`, and `send({ until: "committed"
  })`.** Rejected: two records per message where one says the same, and a
  third `until` beside two that did not mean durable. One durability
  signal is simpler to explain and to keep.
- **Keep `delivered` and add `done`.** Rejected: a replay must not
  deliver a message whose macrostep never ended (guarantee 1), so it
  would read `delivered` for nothing; an audit reader learns the same
  from `done`. It would cost 40 bytes per message.
- **Every operator action as an inbox input.** Rejected for release and
  abandon: they act on a macrostep in progress, and an input waits for
  its end (section 5).

## Consequences

- **Log version 3.** Record sizes, from kernel ADR 0002's measurement
  (header 32 bytes, fields 8 each):
  - `done` is 40 bytes, as `delivered` was: the header and the ID, with
    the reason and the `released` flag in the header's `flags` (0 until
    now); an `abandoned` done adds its microstep count, 48 bytes;
  - a logged message stays 184 bytes (`accepted` 144, `done` 40), to be
    measured again in `inputs_test`;
  - a jump's `accepted` carries its transition (its texts), and the
    `jump` record goes: about 40 bytes fewer per jump;
  - `spawned` grows by its ID, 8 bytes; `finished` and `evicted` (40
    bytes each and more) are no longer written.
- **Memory:** 16 bytes per open macrostep of a logged actor in a map
  (about 32 with the map's key and slack), not per actor, against Phase
  J's +109 B per actor in the input-log bench; nothing without a log.
- **The replay driver** (`replay.c`) loses its per-kind operator cases
  and gains a switch on `done`'s reason, the bootstrap as an owed message,
  and the undone-bootstrap path. `inputs.c` loses the hold and its
  ordering rule. The scheduler's `TAH_NOTE_ENDED` stays, and a runaway no
  longer ends a macrostep.
- **napi's API changes** (napi ADR 0040): `send()` resolves when its
  message is done and committed, without `until`; effects are not held;
  `withdrawSend()` is asynchronous; `evict()` works inside a macrostep;
  `readInputLog()` decodes version 3.
- **The test host's hosted mode** writes and replays version 3; its
  scenarios check the contract across crashes (PLAN.md phase K). It has
  no effect hold, so only its record kinds and crash scenarios change.
- **Checkpoints:** unchanged in layout; owed means not done; a checkpoint
  of an actor whose `done` waits for memory writes it first, as today.
- **Chart authors and applications** lose "exactly once, except" and get
  "at least once, recognizable", which the README's advice already
  assumes. The advice to loop with events stays useful: it keeps sends to
  persisted actors exactly once in practice, and makes a repeat after a
  crash short.
- **Migration:** nothing is published, so no stored log is read across
  the change; version-2 logs are refused.
- **Open follow-up, unchanged by this decision:** a macrostep that
  `publish()` starts (a replayed actor whose invocations are retired,
  `tah_hook_actor_published()`) has no message, so it has no `done`, and
  a later replay does not run it at that place. (open) Under the replay
  rule that is a state the replay does not rebuild, not only a repeat:
  it probably needs an input of its own (an `accepted` "retired"
  message, done when that macrostep ends). ROADMAP follow-up.

## Superseded decisions

- **Kernel ADR 0004, whole:** sections 1 and 2 (the first record held
  and written at the macrostep's end, the ordering rule, record numbers
  patched, `busy` from eviction, withdrawal and checkpoints inside a
  macrostep, the held record's retry and removal), section 3 (the paused
  macrostep's repeat is now part of guarantee 4, not an exception), and
  its implementation notes' machinery.
- **Kernel ADR 0002:**
  - section 2: `delivered` and `dropped` become `done`; the operator
    records and `finished` and `evicted` go (section 2 here);
  - section 5, third point: "a turn's outward effects are held until its
    commit resolves" is removed;
  - section 6: `delivered`, operator actions and publishing at the last
    record's time are replaced by section 4 here;
  - implementation notes, Step 4a: `released` at the first resume without
    the limit, `withdrawn` and `abandoned` recorded before core is
    called.
- **Kernel ADR 0003:** its implementation notes' "A window that remains"
  is closed by this decision instead of kernel ADR 0004; section 3's
  out-of-memory window is section 8's proposal (open).
- **napi ADR 0005, point 11:** `until: "accepted"` and `until:
  "completed"` (napi ADR 0040).
- **napi ADR 0007:** its status line's reference to kernel ADR 0004; the
  budgets stay, and a paused macrostep's repeat is guarantee 4.
- **napi ADR 0032:** section 6's "Write-ahead" points (effects held until
  commit); section 8's `busy` inside a macrostep.
- **napi ADR 0033:** section 1's "nothing the turn caused leaves the
  process until the application has made that batch durable"; section 3's
  `dropped` and `jump` records; section 4's "undelivered" (now "not
  done"); section 7 and its 2026-10-01 rewording (now section 9's
  contract).
- **napi ADR 0034:** section 5's record vocabulary for `readInputLog()`.
- **Core ADR 0089:** section 2's last clock move "after an eviction: the
  eviction time" (no `evicted` record; the latest record time); section
  7's operator operations (a withdrawal is an input taken between
  macrosteps; release and abandon are carried by the macrostep's `done`);
  Scope's third condition ("every input was committed before its effects
  were released") no longer holds, and the replay stays exact anyway: a
  message whose effects ran and whose `done` did not commit is not
  replayed but run again live.
- **Core ADR 0084, section 3,** for logged actors: a withdrawal waits for
  the macrostep in progress to end (open whether that needs a way around
  it for a runaway).
- **Core ADR 0091, section 5:** the messages the host still owes are the
  ones not done.
