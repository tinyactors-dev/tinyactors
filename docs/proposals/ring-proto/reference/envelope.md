# The envelope and the pool

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

An **envelope** is one input that a session has not finished with yet. That
covers an external event admitted from h2c, mail from another local session,
a delayed send that is not yet due, a local invocation's completion or stop
token, and the event whose execution has not yet completed. Internal events
(`<raise>`, `done.state.*`, `error.*`) use separate interpreter storage.

All envelopes come from one **pool** per system, with a fixed bound. The pool
is the queue core gains in this design. Sending to a local session, delaying a
send, starting a local invocation and admitting external input take an envelope
from it. Raising an internal event does not. Running out of envelopes is a
refusal the sender can see.

This replaces several structures in core today:

| Today | In the pool |
|---|---|
| The host's mailboxes and router (kernel `scheduler.h`, `router.h`) | envelopes in the run queue |
| `TA_ERR_BUSY` from `ta_actor_deliver` (core ADR 0055) | gone as an answer to the host: mail for an active session waits on its deferred list in core ([below](#mail-for-an-active-session), [macrostep.md](macrostep.md#macrostep-slots)) |
| An actor's event block and its cache (core ADRs 0069, 0093) | the taken envelope, retained through startup and internal follow-up until input completion |
| The internal queue lent to a running actor (core ADR 0064) | not the pool: an ordinary quota and a compiler-sized mandatory floor per interpreter ([below](#the-internal-queue-is-not-in-the-pool)) |
| Delayed sends living with their owner (core ADR 0054) and the global deadline index | envelopes with a due time, in the pool's deadline index |
| Waiting mail for unloaded actors (kernel `loads.h`) | envelopes on the waiting list of one [waiting entry](#the-waiting-entry-one-copy-of-the-address) per address |

## The envelope

It has a fixed size, so the pool is an array. The variable parts (an event
name, an origin URI, a delayed send's target) are bytes in the name area
([names.md](names.md)), and the envelope points at them. The address of a
target that is not resident is not among them: it is in the address's
[waiting entry](#the-waiting-entry-one-copy-of-the-address), once per
address.

| Field | Bytes | Meaning |
|---|---|---|
| `next` | 4 | the next envelope on whatever list holds this one: the run queue, a macrostep's outgoing list, a waiting list, or the free list |
| `owner_next` | 4 | the next pending delayed send of the same sender (for `<cancel>` and unloading) |
| `heap_index` | 4 | while a timer, where it sits in the deadline index; while runnable, its arrival number ([below](#mail-for-an-active-session)) |
| `state` | 1 | free, outgoing, runnable, waiting, taken, timer, invocation token, child stop |
| `type`, `source`, `flags` | 3 | `_event.type`; where it came from (`input_source` in the catalog); recorded/replaying flags, original invocation report kind/ends bits, and whether `target` names a waiting entry |
| `target` | 8 | the target session. While the envelope is `waiting`, the index of its address's [waiting entry](#the-waiting-entry-one-copy-of-the-address) instead, and the same for outgoing mail whose target is not resident at its `<send>` (a flag says which). A timer for an I/O processor holds 0: its target is text. |
| `from` | 8 | the sending session, or 0 for external input |
| `due` | 8 | timer deadline, or replay input header `at` to apply when taken |
| `data` | 8 | `_event.data`, a vref the envelope holds a reference to |
| `content` | 8 | a delayed I/O send's evaluated content, retained separately from its data, or 0 |
| `invoke_instance` | 8 | the invocation-instance token for a child event, completion or stop, or 0 |
| `send_id` | 8 | a local send's `ta_send_id` for owner-list cancellation; external send ID bytes are in `text` |
| `source_seq` | 8 | the h2c seq it was admitted from, or 0 |
| `text` | 8 | its text in the name area: the first chunk and total length ([names.md](names.md#the-name-area)). It holds the name, origin, origintype, invokeid, a delayed send's target (an I/O processor's target, or the local address it was sent to, which travels in its owner's image), I/O processor type and arbitrary external send ID, each prefixed by a `u16` length. Mail and admitted input do not hold their target address here: a resident target is a session in `target`, and an address that is not resident is in its waiting entry. Invocation reports also preserve their original report invokeid and reason. A traced envelope adds its [trace parent](#the-trace-parent) as the last field. |
| | **88** | **estimate**, on a 64-bit host |

### The trace parent

An envelope carries the trace parent its receiver's macrostep runs under
(core ADRs 0067, 0073). It is one more length-prefixed field in `text`,
present only when the trace ID is not zero:

| Part | Bytes | Meaning |
|---|---|---|
| length | 2 | 38, as for every text field |
| context | 18 | trace ID high and low (16), W3C trace-flags (1), local detail (1) |
| cause | 20 | the causing step's key: session (8), macrostep (8), microstep (4) |
| | **40** | |

- **An untraced envelope stays 88 bytes** and adds nothing to its text.
- **A traced envelope takes 40 bytes more of the name area.** A short
  name plus the trace parent exceeds one 60-byte chunk, so it usually
  takes a second 64-byte chunk ([BEHAVIORS.md](../BEHAVIORS.md#nameareabytes)).
- **What fills it:** an admitted
  [`h2c.event`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cevent) or
  [`h2c.invoke_report`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cinvoke_report)
  gives its trace fields, with the host cause `{session 0, macrostep
  parent_span_id}`. Mail and a delayed send give the sending step's
  context and its full step key.
- **Where it goes:** when the envelope is taken, the macrostep runs under
  it. A recorded session writes it in
  [`c2h.input`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput). A delayed
  send to an I/O processor writes it in
  [`c2h.send`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hsend) when due. A
  delayed send keeps it in session images and in
  [`c2h.state`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hstate), as today's
  `ta_delayed_send_info.trace`.
- **Recycling keeps it.** An envelope recycled as `error.communication`
  for its sender runs under the same trace parent.

The name is resolved to atoms when the envelope is **taken**, not stored as
an ID. Session IDs and local `send_id` already survive images (core ADRs
0066, 0048). The invocation-instance token is meaningful only in this system
lifetime; active local invocation participants cannot be unloaded.

## The pool

- **It is created with the system**, with room for at most
  `poolEnvelopes` envelopes and `nameAreaBytes` of text.
- **It can fill on demand, but only outside macrosteps.** This is the shape
  ADR 0093 uses for the event-block cache: allocate on first use, up to the
  bound, and keep. Growth happens only at points where nothing is running:
  when core admits h2c records, and after a macrostep end. Core keeps a reserve of
  free envelopes there (the most one macrostep may take before failing, see
  below). **A macrostep only takes envelopes that already exist.** A host
  that wants no allocation after start-up sets the pool to fill at creation.
- **Every list is intrusive.** It is a `u32` index in the envelopes
  themselves, so no list needs memory of its own.

### Memory, as estimates

These figures are **estimates** to check against an implementation, not
measurements:

| | Estimate |
|---|---|
| One envelope | 88 bytes, plus 4 for its slot in the deadline index |
| Default pool, 4,096 envelopes | about 368 KiB |
| One [waiting entry](#the-waiting-entry-one-copy-of-the-address) | about 40 bytes, plus the address's text in the name area: list head and tail 8, text reference 8, hash 4, `request_seq` 8, recycle cursor, flags and count about 12. Taken only while an address has envelopes or an outstanding load request. |
| The waiting-entry table | a fixed B3 table that holds `poolEnvelopes` keys at three-quarters load: 8 bytes per slot, 8,192 slots and 64 KiB at the default pool, allocated with the system |
| Default name area | 256 KiB (`nameAreaBytes`, 262,144): one 64-byte chunk per envelope ([BEHAVIORS.md](../BEHAVIORS.md#nameareabytes)) |
| A traced envelope | 40 more bytes of name area, usually a second 64-byte chunk; a traced system sets about 128 bytes of name area per envelope |
| Default ordinary internal quota, 256 entries of about 48 bytes | about 12 KiB (`internalQueueEvents`), plus the compiler-sized mandatory floor |
| Per actor | the head of its own pending delayed sends, and the head and tail of its deferred list: +12 bytes on today's 332 (ADR 0092: "an actor stays at 332 bytes") |
| Today, for comparison | napi's event blocks prefilled at about 811 KB per system before ADR 0093, each queued event (`ta_queued_event`) 96 bytes, and the kernel's mailboxes on top |

A quiet system that fills on demand pays only for the envelopes its peak
needed, as with ADR 0093 today.

## The lists an envelope can be on

| List | Holds | Order | One per |
|---|---|---|---|
| **Run queue** | runnable envelopes: admitted external events, mail, invocation reports, timers that became due | arrival | system |
| **Invocation token** | one envelope held by each live local invocation, converted to completion mail or a child stop when needed | not queued until converted | local invocation |
| **Outgoing list** | mail the running macrostep sent, until its macrostep end | send order | active slot |
| **Deferred list** | runnable envelopes for a session that is active, met while filling a slot | arrival | active session |
| **Waiting list** | envelopes for an address that is not resident; its head and tail are in the address's [waiting entry](#the-waiting-entry-one-copy-of-the-address) | arrival | waiting entry |
| **Owner list** | a session's pending delayed sends | send order | session |
| **Free list** | free envelopes | – | system |
| Deadline index | pending delayed sends, as a binary heap of envelope indexes ordered by (due, `send_id`), its positions in a paged array that grows and trims with the pool | – | system |

**There is one run queue in arrival order, not one queue per target.**
Arrival order keeps every sender-to-receiver pair in order, which is what
SCXML asks of one sender's events. A queue per target would need a
scheduler to choose between targets; one queue in arrival order has no
policy apart from `fairnessRatio`.

Two cases make envelopes wait for a particular target:

- the target isn't resident: they go on that address's waiting list
  ([below](#envelopes-for-an-address-that-isnt-resident));
- the target is active in a slot: they go on its deferred list
  ([below](#mail-for-an-active-session)).

**Why one queue:**

- **Queues per target need a scheduler to choose between them.** That
  scheduler is the policy the kernel held and this design deletes:
  fairness between targets, round robin, starvation. The slots take turns,
  but they choose nothing: each is filled from the one queue, in order.
- **Arrival order is explicit.** Each execution orders its runnable
  envelopes by arrival. Replay uses the same scheduling rule to seed a new
  execution, whose order may differ as resource conditions change
  ([recording-and-replay.md](recording-and-replay.md#session-replay-and-whole-system-replay)).
- **It costs one list head per system**, and a deferred list head per
  actor, used only while the actor is active.

### Mail for an active session

A session holds a slot from the take of an input until its
`c2h.input_completed` ([macrostep.md](macrostep.md#macrostep-slots)). Its
next input must wait for that, but other sessions' envelopes behind it
must not.

**The rule: a session's mail keeps its place in the run queue.** A fill
takes the first envelope in arrival order whose session is not active. An
envelope for an active session is skipped, not reordered. When the session's
input completes, its mail competes again at the place its arrival gives it:
not ahead of other sessions' earlier mail, and not behind their later mail.
The deferred list is how core keeps that rule without walking the skipped
envelopes on every fill:

- **When core fills a slot,** it walks the run queue from the head. An
  envelope whose target is active moves to the end of that session's
  **deferred list**. The walk takes the first envelope whose target is not
  active. Each envelope is moved at most once per activity of its target,
  so the walk costs at most the envelopes it moves, plus one.
- **Each runnable envelope carries an arrival number.** Core numbers an
  envelope when it joins the run queue. The number shares `heap_index`'s
  four bytes: an envelope in the deadline index is not runnable, and a
  runnable one is not in the deadline index. Numbers are compared modulo
  2^32, which is exact while fewer than 2^31 envelopes are runnable.
- **When the session's input completes,** core returns the first envelope
  of its deferred list to the run queue, at the position its arrival
  number gives it. The rest stay deferred: they cannot run before the first
  one, and its take makes the session active again. Every deferred
  envelope arrived before every envelope that no fill has walked past, so
  that position is among the returned envelopes at the head of the run
  queue. The next fill takes the first of them, so there is at most one
  per free slot, and the merge walks at most that many.
- **A returned envelope that leaves before it is taken,** because it is
  withdrawn ([below](#withdrawing-an-admitted-input)), makes core return
  the next envelope of the same deferred list in its place.
- **Order is kept per pair.** Every envelope on a deferred list arrived
  before any envelope for that session still in the run queue, and a
  session's envelopes return one at a time, in order.
- **A deferred envelope is still runnable.** It counts against the pool,
  can be withdrawn ([below](#withdrawing-an-admitted-input)), and is
  recycled like any other when its session is destroyed.

**Why arrival order, and not the session's backlog first.** A returned
envelope competes with other sessions' mail by arrival. If it went to the
head of the run queue instead, a session with a long backlog would run its
inputs back to back, ahead of mail that arrived before them. With arrival
order, a session with a backlog cannot take more than its share each time
it finishes an input, and with several slots a busy target holds one slot
while the others go on.

With one slot, nothing is ever deferred.

## Admission: from h2c into the pool

Core moves h2c events and invocation reports into the pool only while the pool is below
`admissionWatermark` (a fraction of `poolEnvelopes` and of `nameAreaBytes`).
The rest of the pool is kept free for what sessions send each other.

- **Below the watermark,** core takes the next h2c event, takes an
  envelope, copies the record's text into the name area, takes over its
  vref, and appends the envelope to the run queue (or to its target
  address's waiting list, whose
  [waiting entry](#the-waiting-entry-one-copy-of-the-address) holds the
  target address once). The h2c bytes are then released.
- **At or above it,** core sets `admitting` to false. The host can refuse new
  events at its edge. For an event already posted, core reserves a bounded
  `c2h.bounced`, consumes the event and releases its transferred references.
  The bounce preserves its source sequence and capacity reason so the host
  can refuse the originating request. No pool envelope is needed. If the
  bounce cannot fit, core returns for the host to drain c2h before consuming
  the event. A full pool therefore cannot trap controls behind an unread
  event indefinitely.
- **Records that are not events** ([`h2c.time`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2ctime),
  [`h2c.load`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload),
  [`h2c.load_failed`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload_failed),
  [`h2c.definition_register`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cdefinition_register), ...)
  are applied when read and take no envelope. They are still read in order;
  earlier events are admitted or explicitly bounced before the control runs.
  Record order is not an execution barrier. A host that needs an earlier
  event to finish before advancing time or applying another control waits
  for that event's `c2h.input_completed` first.

### Fairness

When both the run queue and h2c have work, core alternates: it runs up to
`fairnessRatio` envelopes from the run queue, then consumes one h2c record,
admitting or bouncing an event according to capacity. Internal work goes first, so work already started
finishes before new work comes in. The ratio keeps two sessions that mail
each other forever from shutting out external input.

The ratio is the only fairness policy. Between sessions, the run queue's
arrival order decides, including for mail that waited on a deferred list
([above](#mail-for-an-active-session)).

**Why not round robin across targets.** A queue per target, taken in turn,
would stop one busy target from delaying another's mail. It needs more
structure, and the scenarios and replay's scheduling rule rely on one run
queue in arrival order. It is internal scheduling, so it could be added
without a protocol change.

The ratio is part of the system's parameters. A new execution may choose a
different ratio, including when seeded by replay
([recording-and-replay.md](recording-and-replay.md)).

## Mail between local sessions

A `<send>` to a local session (`#_scxml_<session>`, a family address of this
system, `#_parent`, `#_<invokeid>`) doesn't go to c2h:

1. Core takes a free envelope. Tokens already held by local invocations are
   unavailable to ordinary sends ([Local invocation tokens](#local-invocation-tokens)).
2. It fills the envelope from the evaluated send, takes a reference to its
   data vref, and puts the text in the name area. A resident target is a
   session in `target`. A target that is not resident names its address's
   waiting entry, found or taken at the `<send>`
   ([below](#the-waiting-entry-one-copy-of-the-address)); the address is
   not copied into the envelope.
3. It keeps the envelope on the running macrostep's **outgoing list**, in
   send order. The envelope is on no queue yet, so nothing can take it.
4. At the macrostep's [`c2h.macrostep_ended`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hmacrostep_ended),
   core moves the outgoing list, in order, to the end of the run queue. The
   macrostep end's `mail` field counts them. An envelope whose target address isn't
   resident goes to its waiting entry's list instead, and core writes
   [`c2h.load_needed`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hload_needed)
   after the macrostep end ([below](#envelopes-for-an-address-that-isnt-resident)).

Post-macrostep routing and notifications are a bounded drain phase. Its cursor
uses the existing outgoing/wait lists and retains the outstanding `load_needed`
or latest `wake` notification until it can reserve and commit that record.
Core can return for a c2h drain and resume this phase; the completed macrostep
is not rolled back or executed again. The phase needs no overflow queue.
The current input completes only after its outgoing routing and mandatory
notifications have been flushed.

So local receivers run after the sender's macrostep ends. A macrostep
an operator abandons (ADR 0082) frees its outgoing list. No mail
crosses the host, so no JavaScript runs for it and no vref leaves core.

**Why mail waits for the macrostep end:**

- **A receiver never sees mail from an unfinished macrostep.** With slots,
  a receiver could run while its sender is paused in another slot. The
  outgoing list holds the mail until the sender's macrostep ends, so an
  abandoned macrostep can free mail nobody has seen.
  Host effects on c2h may execute before the macrostep end; they are independent
  of this local scheduling rule and cannot be rolled back by abandonment.
- **Cause comes before effect in the log.** A recorded sender's
  [`c2h.input`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput) is on c2h
  before the receiver can take the mail and write its own `c2h.input`. A
  stream read in order shows the sender's input observation first. This
  is an observation order, not a durability guarantee.
- **It costs little.** A receiver waits at most for the rest of its
  sender's macrostep, which is SCXML's own unit of work. The outgoing list
  only decides *when* the mail becomes visible.

Mail to an address in another system or process isn't local. It is a
[`c2h.send`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hsend) for the host's SCXML
I/O processor.

## Delayed sends: envelopes with a due time

**Core keeps the timers, as envelopes in the pool.**

- **A delayed `<send>`** takes an envelope in state `timer`, with its `due`.
  The envelope goes into the deadline index and onto its sender's owner list.
  This happens whatever the target: a local session, or an I/O processor.
  It retains evaluated data and content separately, plus the I/O processor
  type, target and all event metadata. Firing does not reevaluate expressions.
- **`<cancel>` is exact.** Core removes and frees every pending envelope on
  the sender's owner list whose `send_id` matches, including duplicate authored
  IDs. A timer core has
  already turned into input can't be cancelled. SCXML says the same, and
  core is the only place this can happen, so there is no race with the host.
- **[`h2c.time`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2ctime) moves the
  clock.** Every timer due by then leaves the index in (due, `send_id`).
  `send_id` counts per sender, so one sender's timers due at the same time
  leave in send order; timers of different senders with the same due time
  have no order between them, as mail between sessions has none:
  - a local target's timer becomes runnable at the end of the run queue;
  - an I/O processor's timer is written as a
    [`c2h.send`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hsend) and freed only
    after commit. If the record cannot fit, the due envelope retains all
    its references and core returns for the host to drain c2h and resume.
- **[`c2h.wake`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hwake)** is written
  after the head of the index changes. The bounded drain phase retains the
  latest head while c2h is full and emits the current wake deadline before
  completing the input or time operation. The host needs one timer.
- **Timers to an I/O processor take an envelope too**, until they are due.
  So a fanout of 10,000 per-notification timeouts takes 10,000 envelopes, and
  the 10,001st `<send delay>` fails as `error.execution.capacity.pool`.

What it replaces: ADR 0054's per-owner storage, the sparse state it grows,
and the global index ordering owners. The per-owner list stays, as `owner_next`.

**Why core keeps the timers, and not the host:**

- **`<cancel>` is exact.** Core fires a timer only when it reads
  `h2c.time`, with nothing queued in between. A cancelled timer never
  arrives. If the host kept timers, a timer that fired just before the
  cancel would still arrive, and a chart that reuses a sendid would act on
  it.
- **The capacity is shared.** A timer is an envelope like any other, so a
  fanout of 10,000 timeouts is limited by the pool, not by slots per actor.
- **The host needs only one timer.** It needs that one timer, set by
  [`c2h.wake`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hwake), plus its index of
  unloaded sessions ([unloading.md](unloading.md)).
- **A session's state is one artifact.** Its pending timers are inside its
  image and its [`c2h.state`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hstate).
  Replaying a session rebuilds them, so the host needs no reducer over
  suppressed sends.

### A due send to an I/O processor has no macrostep end

A timer for an I/O processor becomes due while core handles `h2c.time`, not
inside a macrostep. Core writes its `c2h.send` with `macrostep` 0, and no
[`c2h.macrostep_ended`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hmacrostep_ended) follows. The host
may act on it at once.

**Why there is no macrostep end:** this record is not part of a running macrostep.
A macrostep end would report the end of a macrostep that doesn't exist. Like every
other c2h effect, the host may execute it on receipt. Journaling its owner's
inputs does not establish whether the effect executed; replay seeds new
state and suppresses external effects while doing so.

## Envelopes for an address that isn't resident

An envelope whose target is an unloaded address, or a family address with no
resident session, can't run yet:

1. Core puts it on that address's waiting list: an admitted h2c record at
   admission, mail at its sender's macrostep end (never before, so a macrostep that
   doesn't commit asks for no load), a timer when it falls due. The list
   belongs to the address's
   [waiting entry](#the-waiting-entry-one-copy-of-the-address), which holds
   the address bytes once. The envelope holds the entry's index in
   `target`.
2. The first time an address gets a waiting list, core writes
   [`c2h.load_needed`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hload_needed).
  It writes it once until the host answers. Until that notification commits,
  a flag and cursor on the waiting entry retain the obligation;
  c2h exhaustion returns to the host for a drain, without dropping mail or
  attempting to undo its sender's completed macrostep.
3. **[`h2c.load`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload)** loads the
   session. Its waiting envelopes join the run queue in their order. Its
   delayed sends, kept in its image, rejoin the deadline index.
4. **[`h2c.load_failed`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload_failed)**
   recycles them (below).

Waiting envelopes count against the pool. A host that wants to keep mail for
an actor that is slow to load can refuse to post more at its edge. The
`pool.waiting` counter in the control block tells it when.

[unloading.md](unloading.md) covers how sessions leave and return.

### The waiting entry: one copy of the address

**Why.** Core ADR 0065 makes a named address a shared, counted handle,
because at four million named actors (one per websocket connection)
copying address strings for each message, and hashing and comparing them
for each directory operation, costs too much. The ring protocol keeps that
for resident targets. Mail to a resident session carries only the session
in `target`. A literal target resolves to a handle at registration. A
computed target costs one lookup: a hash and a compare, with no copy and no
insert. There is no directory vtable to call. Copying the address into
each waiting envelope would bring the per-message copy back, for exactly
the actors a large host keeps unloaded: a burst of N messages to one
unloaded actor would copy its address N times and use N name-area chunks.
So the address of a target that is not resident is stored once per
address, in a waiting entry, and every envelope for it names the entry.

**What it is.** A waiting entry exists for each address that has waiting
envelopes or an outstanding `c2h.load_needed`. Entries are items of a small
index pool (A5) of fixed-size items, beside the envelope pool. Its ceiling
is `poolEnvelopes`: no more addresses can wait than there are envelopes. It
fills on demand at safe points, with the envelope pool, and core keeps free
entries there as it keeps free envelopes. An entry holds:

| Field | Bytes | Meaning |
|---|---|---|
| `head`, `tail` | 8 | the waiting list (B1), threaded through the envelopes' `next` |
| `text` | 8 | the address's text in the name area: first chunk and length. Copied once, when the entry is taken. |
| `hash` | 4 | the address's hash, the entry's key in the waiting-entry table |
| `request_seq` | 8 | the `seq` of its `c2h.load_needed`, or 0 before that record commits |
| `cursor`, `flags`, `count` | about 12 | the recycling cursor of an `h2c.load_failed` in progress; whether `c2h.load_needed` is owed or written, and whether recycling is active; the number of envelopes that name the entry |
| | **about 40** | **estimate**, on a 64-bit host |

**Finding it.** The waiting-entry table is a B3 table in growing mode
that grows with the pool: the pool's refill, at the same safe point,
reserves room in it for one key per envelope the pool holds, up to
`poolEnvelopes`. An insert therefore never allocates, and a quiet system
pays for the table its peak needed, not for the bound: at the largest
pool, a table sized for the bound at creation would take about 256 MiB.
It maps the address's hash to the entry's index. A lookup
compares the candidate entry's stored address bytes. This is the table the
building blocks call "waiting lists by address"
([building-blocks.md](building-blocks.md#assembly-each-proposal-structure-from-blocks));
its value is the entry, which owns the list head and the address bytes.
[`h2c.load`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload),
[`h2c.spawn`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cspawn) and
[`h2c.load_failed`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload_failed)
find the entry by their `address` the same way, then check `request_seq`.

**An envelope for an address that is not resident** (local mail, an
admitted `h2c.event` naming a target address, a timer that fell due):

- **The entry exists:** the envelope names it and, when it is routed,
  joins its list. No copy, no allocation.
- **No entry exists:** core takes a free entry, copies the address into the
  name area once, inserts the entry into the table, and the envelope names
  it. Core writes `c2h.load_needed` once per address (step 2 above).
- **Inside a macrostep** the entry and its chunks come from what is already
  free, like every envelope and chunk. A `<send>` that finds no free entry
  fails as `error.execution.capacity.pool`, and one that finds no chunk for
  the address fails as `error.execution.capacity.names`, as for any send.
  The entry is taken when the `<send>` runs, so the failure is the chart's
  ([Running out](#running-out)). The mail joins the list at its sender's
  macrostep end. If a load made the address resident meanwhile, routing
  finds the session instead and the mail goes to the run queue.
- **At admission** the same capacity answers apply: an `h2c.event` that
  finds no entry or no chunk is bounced with reason `pool` or `names`.
- **A timer that falls due** does so while core applies `h2c.time`, a safe
  point, where core refills entries first. A due timer that still finds no
  entry or no chunk cannot be delivered, and is recycled as
  `error.communication` for its sender
  ([Recycling](#recycling-an-envelope-that-cant-be-delivered-becomes-its-own-error)),
  as when a load fails. It does not stay due: the chunks it would wait for
  may be held by other timers that cannot run.
- **A delayed send keeps its own target text** for as long as it is a
  timer, because it travels in its owner's image. When it falls due for an
  address that is not resident, it names the entry like any other
  envelope.

**The envelope's fields.** A waiting envelope holds its entry's index in
`target`. Its `text` holds its own event name, origin and the rest, but not
the target address.

**When the load succeeds** (`h2c.load`, or `h2c.spawn` answering the
request): the waiting envelopes move to the run queue in order, and each
`target` becomes the session. Core **retires** the entry: it removes it
from the table, so later mail finds the resident session, and frees it and
its address chunks. Outgoing mail of a slot that has not reached its
macrostep end can still name a retired entry; then the entry is freed when
the last such envelope is routed, which finds the address resident.

**When it fails** (`h2c.load_failed`): recycling proceeds as
[unloading.md](unloading.md#recycling-on-h2cload_failed) describes, and
the entry's cursor holds its place across c2h drains. Each recycled
envelope's `target` becomes its sender. The entry and its address chunks
are freed when recycling completes, with `c2h.load_result`.
[`c2h.destroyed`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hdestroyed) frees
the entry of a destroyed session's waiting envelopes the same way.

**Withdrawal** of the last waiting envelope leaves the entry in place, with
its outstanding load request
([below](#withdrawing-an-admitted-input)). It is freed when the host
answers. An entry whose envelopes all leave before any load request is
owed (the outgoing list of an abandoned macrostep) is freed with the last
of them.

**What reads the address text:**

| Reader | Reads it from |
|---|---|
| [`c2h.load_needed`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hload_needed) `address`, once per address | the entry's text |
| `h2c.load`, `h2c.spawn`, `h2c.load_failed` matching their `address` | the entry's hash and text |
| a `DEAD_LETTER` [`tel.trace`](../../../../tinyactors-core/src/generated/MESSAGES.md#teltrace) for an external event dropped while waiting | the entry's text, for the target it names |
| `error.communication` recycling | nothing: the error goes to the sender, by session |
| [`c2h.input`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput) of a recorded session | nothing: it is written when the envelope is taken, when its target is a resident session, and the record carries no target address |

**Bounds.** Entries are bounded by `poolEnvelopes`. Their address chunks
count against `nameAreaBytes` like any text. Nothing grows without bound,
and nothing is interned into the atom table, so the rule that only
authored names are interned holds ([names.md](names.md#the-split)). Per
message to an unloaded actor, the name area holds the event's own text,
not the address and the text. Per waiting address, it holds one copy of
the address.

## Recycling: an envelope that can't be delivered becomes its own error

When mail can't be delivered, SCXML says the sender gets
`error.communication`. Raising that error must not need room the pool may
not have. So **the envelope is reused**:

| The envelope was | It becomes |
|---|---|
| mail or a timer from session S (target gone, load failed, destroyed with mail waiting) | A data-free `error.communication` for S. `_event.sendid` is the send's ID; the original data/content are released. The envelope joins the run queue with target S. Detailed reasons may appear in telemetry. |
| an external event admitted from h2c | Freed after a reserved `c2h.input_refused` identifies its source sequence. `DEAD_LETTER` telemetry is additional; the host settles or retries using its separately retained original. |
| a sender that is itself gone | Freed. |

Every admitted local-mail envelope reserves enough name storage for its
possible data-free `error.communication`, even when the original event name
is shorter. Recycling uses that retained storage, because an error name can be longer
than its source name.

## Withdrawing an admitted input

A host sometimes no longer wants an input it has committed. An HTTP client
disconnects, or an `AbortSignal` fires, while the event still waits behind
a busy pool. h2c records are not edited after commit: commit is where a
`vref` moves to core, and an admitted event no longer lives in the ring.
The host posts [`h2c.withdraw`](../../../../tinyactors-core/src/generated/MESSAGES.md#h2cwithdraw)
instead, naming the input by its h2c seq.

- **It is a control.** It takes no envelope and is applied when read, so it
  works while core is not admitting, which is when it matters most. It is
  read in h2c order, like every other record.
- **A waiting input is withdrawn.** If the target is an envelope in state
  runnable (in the run queue or on a deferred list) or waiting, core reserves a `c2h.input_refused`, unlinks the
  envelope from its list, releases its `data` vref and its name text,
  frees it, and writes the refusal with reason `withdrawn`. No `c2h.input`
  is written, so a recorded session's journal never sees it.
- **A taken input is not.** Once core has taken the envelope, execution has
  begun, including a macrostep paused on its budget. Stopping it is an
  operator abandon ([debugging.md](debugging.md)), not a withdrawal.
- **Too late is not a separate answer.** If the input has been taken,
  bounced or refused, the withdrawal writes nothing. The input's own single
  terminal record (`c2h.bounced`, `c2h.input_refused` or
  `c2h.input_completed`) has come or will come, and the host settles from
  it. Every input still ends with exactly one terminal record, and a
  withdrawn one ends with `input_refused`.
- **Misuse is bounced.** A `target_seq` that is not lower than the
  withdrawal's own seq is `c2h.bounced` (`invalid`).

**The host's side.** A host that withdraws keeps its retry original until
the input's terminal record arrives, as for any input. The race is inherent:
fairness may take the input after its admission and before core reads the
withdrawal behind it. A host that must know before posting more waits for
the terminal record.

**An input waiting for a load.** Withdrawing the last envelope on an
address's waiting list leaves its
[waiting entry](#the-waiting-entry-one-copy-of-the-address) and the
outstanding `c2h.load_needed` in place. The host's answer is still valid;
the load then has nothing to deliver, and the answer frees the entry.

**Invocation reports.** An `h2c.invoke_report` can be withdrawn the same
way. A withdrawn `done` or an `ends` error leaves the invocation active, so
the chart learns of the service's end only from its own timeout. That is
the host's choice to make.

**Finding the envelope.** The envelope already carries `source_seq`. Core
scans the pool's array for a runnable or waiting envelope with that seq,
then unlinks it by walking the list it is on, which is singly linked. Both
are bounded by `poolEnvelopes`, run between macrosteps and allocate
nothing. The run queue's arrival order is unchanged for the envelopes that
remain. At the default 4,096 envelopes this is a few microseconds per
withdrawal.

**Open:** whether a storm of withdrawals (many clients disconnecting at
once) justifies a fixed `source_seq` index, allocated with the pool, so
that each withdrawal costs O(1) rather than a scan. It would cost about 8
bytes per envelope.

## The taken envelope stays put through input completion

When a macrostep starts, its envelope moves to state **taken**. It stays in
its slot while `_event` names it. If invocation startup generates internal
follow-up work, the envelope and its source correlation remain owned until
that work settles and `c2h.input_completed` is written, including for
unrecorded sessions. Otherwise completion follows the macrostep end directly.
This is ADR
0069's "taking an event does not move it", applied to the pool:

- inspection (ADR 0083) reports the event from the envelope;
- [`c2h.input`](../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput) for a recorded
  session is written from the envelope;
- if an operator abandons the macrostep (ADR 0082), core reports the input's
  terminal completion and frees the envelope and its outgoing list.

### Local invocation tokens

Before starting a local child, the post-macrostep startup phase acquires one
pool envelope and the bounded name storage needed for its completion or stop,
including `done.invoke.<invokeid>` and all completion metadata. Cancellation
and completion can reuse this storage without acquiring more name chunks.
Failure leaves the child unstarted and raises an invocation-start error in
the parent's mandatory internal storage. A successful start retains the token
for the invocation's lifetime; ordinary sends cannot consume it.

When the child completes normally, its token becomes
`done.invoke.<invokeid>` on the parent's **external** queue, with the child's
completion data and invocation-instance correlation. This remains true when
the child reaches final during bootstrap. Completion never becomes an internal
event of the parent and cannot fail because ordinary mail filled the pool.
The token is released after delivery completes, or discarded if the parent
or invocation instance is gone.

Completion data follows the existing `<donedata>` and parameter-evaluation
rules, including their failure handling. Done data is retained rather than
copied: the child terminates and cannot subsequently change it (ADR 0097).
It uses the retained token and pre-sized metadata, never a second envelope.

When the parent exits the invoking state, core retires that invocation
immediately after its exit handlers and converts the token to a child-stop
operation. The stop has priority over the child's ordinary inputs. The child's
exit handlers execute in a separate cancellation phase with its own mandatory
c2h reserve; descendants are stopped in the same way. This avoids running an
entire cancellation subtree inside the parent's macrostep. A canceled child
emits no `done.invoke`, and events from its retired instance are ignored.
The token is released after stop cleanup. Pending invocations that never
started need no cancellation.

Unloading is refused while a session is either the child of an active local
invocation or the parent of one. No image owns or duplicates a live token.
Normal completion or cancellation cleanup releases these restrictions only
after the live local relationship is retired.

### The internal queue is not in the pool

Internal events (`<raise>`, `done.state.*`, `error.*`) never leave the running
session, so they don't take envelopes. They use one ordered internal queue
with two storage quotas: `internalQueueEvents` ordinary entries for chart
raises, and a mandatory floor sized from the compiled definition for one
round of every phase. Mandatory events beyond the floor take free ordinary
entries. Both quotas preserve the same enqueue order; mandatory events do
not jump ahead of ordinary events.

- an entry is a fixed slot: the name (an authored ID, or a name-area chunk
  for a generated segment such as an invocation ID), the data vref, and
  `_event`'s other fields;
- a taken entry stays at the head while it is `_event`, and is freed when the
  next one is taken, as ADR 0069 does today;
- one top-level interpreter runs at a time, so there is **one live
  internal queue**, in the running workspace, plus one per nested
  interpreter (ADR 0060), not one lent to each actor (ADR 0064), and no
  cache of queues. A parked slot's pending entries are copied to the park
  area and back ([macrostep.md](macrostep.md#parking-one-running-workspace-and-a-park-area));
- mandatory entries cover errors from distinct executable blocks, condition
  evaluation and data initialization, as well as platform completion events;
- a mandatory event that fits neither the floor nor a free ordinary entry
  ends the input as a runaway, as the chain limit does
  ([macrostep.md](macrostep.md#sizing-mandatory-storage));
- storage is allocated or enlarged outside macrosteps at registration, spawn
  or load, before the definition or session becomes usable. A failed allocation
  changes nothing; execution never grows the queue.

A pool that is full therefore never stops a chart from raising its own
events, and a full internal queue never stops mail. Why the queue is kept
apart from the pool is in
[macrostep.md](macrostep.md#the-internal-queue-compiler-sized-mandatory-storage).

## Running out

There are two kinds of exhaustion, and the chart sees only one of them
([macrostep.md](macrostep.md#two-kinds-of-exhaustion)).

**A `<send>` or delayed send that finds no free envelope, or no name-area
room for its text, fails as `error.execution.capacity.pool` or `.names`.
So does a `<send>` to an address that is not resident and has no waiting
entry, when it finds no free entry or no room for the address
([above](#the-waiting-entry-one-copy-of-the-address)). A
`<raise>` that finds the internal queue full fails as
`error.execution.capacity.internal`.** It is an execution error and not a
communication error, deliberately:

- SCXML says that after an error in executable content, the rest of the
  block is not run. So a fanout that runs out fails **once**, at the first
  send that doesn't fit. There is no flood of 9,000 errors to raise.
- The chart knows exactly what happened: everything before the failing
  action ran, and nothing after it did. The event's last segment names the
  resource (`error.execution.capacity.pool`, `.names`, `.internal`,
  `.record`), and `_event.data` names the action
  ([macrostep.md](macrostep.md#two-kinds-of-exhaustion)).

**An ordinary raise cannot use the mandatory floor.** When its ordinary
quota is exhausted, the raise fails and its
`error.execution.capacity.internal` is a mandatory event: it uses the
floor. Failing one executable block does not suppress errors or execution
in other blocks. The floor covers one round of every phase; accumulation
beyond it uses free ordinary entries, and an overflow is a runaway
([macrostep.md](macrostep.md#sizing-mandatory-storage)).

Ordinary mail may fill every envelope not already held as an invocation token.
Error reporting uses internal storage; undeliverable mail reuses its own
envelope ([Recycling](#recycling-an-envelope-that-cant-be-delivered-becomes-its-own-error)).
A mandatory event that fits neither the floor nor a free ordinary entry is
the only internal-storage shortage that ends an input, as a runaway.

### The abandoned block, in a chart

Because the rest of the block is abandoned, **where** an action sits in its
block decides what a capacity failure costs:

```xml
<onentry>
  <send id="t" event="sending.timeout" delay="30s"/>   <!-- first: always scheduled -->
  <foreach array="batch" item="n">                    <!-- may fail partway -->
    <send type="x-notify" target="notify:" event="notification" namelist="n"/>
    <send targetexpr="tracker" event="progress.sent"><param name="id" expr="n.id"/></send>
  </foreach>
</onentry>
```

If the timer comes after the `<foreach>`, a fanout that runs out never
schedules its own timeout, and the chart waits forever.
[scenarios/S02-full-pool.md](../scenarios/S02-full-pool.md) walks through this
case, and [BEHAVIORS.md](../BEHAVIORS.md) lists who retries and how.
