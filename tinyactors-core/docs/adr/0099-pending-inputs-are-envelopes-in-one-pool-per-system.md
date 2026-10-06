# ADR 0099: Pending inputs are envelopes in one pool per system

**Status:** Accepted (2026-10-05); not yet implemented. Supersedes ADRs 0011, 0029, 0040, 0055, 0064, 0069 and 0093; extends ADR 0065.
**Date:** 2026-10-05

## Context

Core has no queue of pending inputs. ADRs 0011 and 0029 gave the external
queue to the host: core accepts an event only when the actor can start a
macrostep, and the host keeps everything else. ADR 0040 made core hand each
inter-session event to a host router, which queues it, loads its target and
later calls `ta_actor_deliver`. ADR 0055 made deliver the admission test, with
`TA_ERR_BUSY` meaning "keep it queued". ADRs 0064, 0069 and 0093 then built
storage for the one event an actor holds: an internal queue lent to a running
actor, grown into an event block (792 bytes), kept in a cache that fills on
demand up to `eventBlockCache` (default 1,024).

That one decision, the queue outside core, is the source of most of the
complexity the ring protocol removes
([README, "Why"](../../../docs/proposals/ring-proto/README.md#why)):

- **A send cannot be refused in the macrostep that made it.** Core hands mail
  to a router after evaluating it. Whether the receiver can ever take it is
  decided later, elsewhere, so the chart cannot see a full system as an error
  in the action that caused it.
- **Overflow has to be held somewhere.** A router that answers retry makes
  the sending continuation hold the delivery (ADR 0040); a busy actor makes
  the host hold the message (ADR 0055); an unloaded actor makes the loader
  hold it (napi's `maxWaiting` per family). Each holder has its own bound and
  its own dead letters.
- **Every binding needs the same router and scheduler.** They moved into a
  third layer, the host kernel (`scheduler.h`, `router.h`, `loads.h`), which
  every binding links and whose policy (`resumesPerTurn`, `messageBudget`,
  `maxQueuedMessages`) is a second set of parameters beside core's.

The ring protocol proposal puts the queue in core with a fixed bound. The
design was worked out in
[reference/envelope.md](../../../docs/proposals/ring-proto/reference/envelope.md),
[reference/unloading.md](../../../docs/proposals/ring-proto/reference/unloading.md#waiting-envelopes-count-against-the-pool),
[BEHAVIORS.md](../../../docs/proposals/ring-proto/BEHAVIORS.md#parameters)
and item 3 of
[reference/migration.md](../../../docs/proposals/ring-proto/reference/migration.md#adrs-to-write).
This ADR records it.

The building blocks it uses exist: the index pool (`src/index_pool.h`),
intrusive index lists (`src/list.h`), the indexed min-heap (`src/heap.h`),
chunk chains (`src/chunks.h`), quotas with reservations (`src/quota.h`) and
the hash table (`src/table.h`).

## Decision

1. **A pending input is an envelope.** An envelope is one input a session
   has not finished with: an external event admitted from h2c, mail from
   another local session, a delayed send not yet due, a local invocation's
   completion or stop token, and the event whose execution has not yet
   completed. Internal events (`<raise>`, `done.state.*`, `error.*`) are not
   envelopes; they keep interpreter storage of their own.

   An envelope has a fixed size, so the pool is an array of items. Its fields,
   at the level this ADR needs:
   - links: `next` (the one list that holds it), `owner_next` (its sender's
     pending delayed sends), and `heap_index` (its place in the deadline index
     while a timer, its arrival number while runnable);
   - `state` (free, outgoing, runnable, waiting, taken, timer, invocation
     token, child stop), and `type`, `source` and flags;
   - `target` (a session, or the index of a waiting entry, decision 8) and
     `from` (the sending session, 0 for external input);
   - `due`, `data` and `content` (held vrefs), `invoke_instance`, `send_id`,
     and `source_seq` (the h2c seq it was admitted from);
   - `text`: one chunk chain in the name area holding its variable parts
     (name, origin, origintype, invokeid, a delayed send's target, the I/O
     processor type, an external send ID, and the trace parent when traced),
     each prefixed by a `u16` length.

   A name is resolved to atoms when the envelope is taken, not stored as an
   ID.

2. **One pool per system.** Sending to a local session, delaying a send,
   starting a local invocation and admitting external input take an envelope
   from it. Raising an internal event does not.
   - **Its bound is `poolEnvelopes`** (default 4,096, from 8 to 2^24), fixed
     at creation as ADR 0092 fixes every parameter.
   - **It fills on demand, at safe points only:** when core admits h2c records
     and after a macrostep end. This is ADR 0093's shape: allocate on first
     use, up to the bound, and keep. Core keeps a reserve of free envelopes at
     each safe point. **A macrostep only takes envelopes that already exist.**
     A host that wants no allocation after start-up fills the pool at
     creation.
   - **Every list is intrusive:** a `u32` index in the envelopes, so no list
     owns memory.

3. **The name area holds the text.** It is a chunk area of 64-byte chunks
   (60 bytes of text), bounded by `nameAreaBytes` (default 262,144, one chunk
   per envelope at the default pool; at least `maxRecordBytes`, at most
   1 GiB). It fills on demand at the same safe points as the pool. A traced
   envelope's trace parent is 40 more bytes of it.

4. **One run queue, in arrival order.** Runnable envelopes (admitted events,
   mail, invocation reports, timers that fell due) are on one list per system,
   in the order they became runnable. There is no queue per target, so no
   scheduler chooses between targets. Arrival order keeps every
   sender-to-receiver pair in order, which is what SCXML asks of one sender's
   events. The other lists are:
   - a slot's **outgoing list**, in send order, holding the mail its
     macrostep sent until that macrostep ends;
   - a session's **deferred list** (decision 5);
   - an address's **waiting list** (decision 8);
   - a session's **owner list** of pending delayed sends;
   - the **free list**;
   - the **deadline index**, a binary heap of envelope indexes ordered by
     (due, `send_id`), in a paged array that grows and trims with the pool.

   How delayed sends use the owner list and the deadline index is a separate
   decision (item 4 of the migration plan); this ADR only places them in the
   pool.

5. **Mail for an active session keeps its place.** A session holds a slot
   from the take of an input until its `c2h.input_completed`. When core fills
   a slot it walks the run queue from the head; an envelope whose target is
   active moves to the end of that session's deferred list, and the walk takes
   the first envelope whose target is not active. When the session's input
   completes, core returns the **first** envelope of its deferred list to the
   run queue at the position its arrival number gives it, not at the head.
   The rest stay deferred until that one is taken. So a session with a backlog
   competes by arrival, and cannot run its backlog back to back ahead of
   other sessions' earlier mail. Each envelope is moved at most once per
   activity of its target. With one slot nothing is ever deferred. A deferred
   envelope is still runnable: it counts against the pool, can be withdrawn,
   and is recycled when its session is destroyed.

   This replaces `TA_ERR_BUSY` as an answer to the host: mail for a busy
   session waits in core.

6. **Admission and the watermark.** Core moves h2c events and invocation
   reports into the pool only while both the pool and the name area are below
   `admissionWatermark` (default 50%, from 1% to 99%). The rest is headroom
   for what sessions send each other.
   - **Below the watermark,** core takes an envelope, copies the record's
     text into the name area, takes over its vref, and appends the envelope
     to the run queue or to its target's waiting list.
   - **At or above it,** core clears `admitting` in the control block, so the
     host can refuse at its edge. An event already posted is **bounced:** core
     writes a `c2h.bounced` with its source seq and the capacity reason
     (`pool` or `names`), consumes the record and releases its references. A
     bounce takes no envelope. If c2h has no room for it, core returns for the
     host to drain c2h first. A full pool therefore never traps a control
     behind an unread event.
   - **Controls** (`h2c.time`, `h2c.load`, `h2c.load_failed`,
     `h2c.withdraw`, ...) take no envelope. They are applied when read, in h2c
     order.

7. **`fairnessRatio` is the only fairness policy.** While both the run queue
   and h2c have work, core runs up to `fairnessRatio` envelopes from the run
   queue (default 4, from 1 to 4,096), then consumes one h2c record. Work
   already started goes first; the ratio keeps two sessions that mail each
   other forever from shutting out external input. Between sessions, the run
   queue's arrival order decides, including for mail returning from a
   deferred list. The ratio shapes latency and fails nothing. A new execution,
   including one seeded by replay, may choose a different ratio.

   **Round robin across targets is rejected.** A queue per target taken in
   turn would stop one busy target from delaying another's mail. It needs a
   scheduler, which is the policy the kernel held and this design deletes,
   and more structure; the scenarios and replay's scheduling rule rely on one
   run queue in arrival order. It is internal scheduling, so it can be added
   later without a protocol change.

8. **One waiting entry per address that is not resident.** An envelope whose
   target is an unloaded address, or a family address with no resident
   session, waits on that address's waiting list. The list belongs to a
   **waiting entry**: an item of a small index pool beside the envelope pool,
   whose ceiling is `poolEnvelopes` (no more addresses can wait than there are
   envelopes). An entry holds the waiting list's head and tail, the address's
   text in the name area **once**, its hash, the `seq` of its
   `c2h.load_needed`, and the cursor, flags and count of recycling in
   progress. A table keyed by the address's hash finds it; its room is
   reserved with the pool at safe points, up to `poolEnvelopes` keys, so an
   insert never allocates.
   - A waiting envelope holds its entry's index in `target`, and its `text`
     holds its own event text but not the address. A burst of N messages to
     one unloaded actor copies its address once, not N times, and nothing is
     interned into the atom table.
   - The first envelope for an address takes the entry and makes core write
     `c2h.load_needed` once, until the host answers.
   - `h2c.load` (or `h2c.spawn` answering the request) moves the waiting
     envelopes to the run queue in order and retires the entry.
     `h2c.load_failed` recycles them (decision 10) and frees the entry when
     recycling completes.
   - Inside a macrostep, the entry and its chunks come from what is already
     free. A `<send>` that finds no free entry fails as
     `error.execution.capacity.pool`; one that finds no room for the address
     fails as `.names`. An `h2c.event` in the same case is bounced.

   This **extends ADR 0065.** ADR 0065 made a named address a shared,
   counted handle so that core does not copy address strings per message at
   four million named actors. Resident targets keep that: mail to a resident
   session carries only the session. The waiting entry carries the same
   property to addresses that are not resident, which are exactly the actors a
   large host keeps unloaded.

9. **No cap per address.** The pool is the only bound on envelopes that wait
   for a load. The host bounds how long they wait: it answers
   `h2c.load_failed` at once for an address it does not know, and times out a
   slow load itself. `c2h.load_needed` and `pool.waiting` in the control block
   tell it how many wait. This replaces napi's per-family `maxWaiting` and its
   `waiting-full` dead letters. A `maxWaitingPerAddress` parameter is
   rejected: it would refuse bursts to a popular actor that is slow to load
   even when the pool has room.

10. **An envelope that cannot be delivered becomes its own error.** SCXML
    gives the sender `error.communication`, and raising it must not need room
    the pool may not have, so the envelope is reused:
    - mail or a timer from session S (target gone, load failed, destroyed
      with mail waiting) becomes a data-free `error.communication` for S, with
      `_event.sendid` the send's ID, its data and content released, joining
      the run queue with target S under the same trace parent;
    - an external event admitted from h2c is freed after a reserved
      `c2h.input_refused` names its source seq;
    - an envelope whose sender is itself gone is freed.

    Every admitted local-mail envelope reserves enough name-area room for its
    possible `error.communication`, which can be longer than its own name.

11. **Withdrawal is a new record.** Committed h2c records never change.
    `h2c.withdraw` names an admitted input by its h2c seq. It is a control:
    it takes no envelope and works while core is not admitting.
    - If the envelope is runnable (in the run queue or a deferred list) or
      waiting, core unlinks it, releases its vref and text, frees it, and
      writes `c2h.input_refused` (`withdrawn`). No `c2h.input` is written.
    - If it has been taken, bounced or refused, the withdrawal writes nothing;
      the input's own terminal record stands. Stopping a taken input is an
      operator abandon (ADR 0082).
    - A `target_seq` not lower than the withdrawal's own seq is bounced
      (`invalid`).
    - Withdrawing the last envelope of a waiting list leaves its entry and
      the outstanding `c2h.load_needed` in place; the host's answer frees it.

    Core finds the envelope by scanning the pool for `source_seq`, bounded by
    `poolEnvelopes`, between macrosteps, allocating nothing.

12. **The taken envelope stays put until input completion.** When a
    macrostep starts, its envelope becomes `taken` and stays in its slot
    while `_event` names it, through invocation startup and internal
    follow-up, until `c2h.input_completed`. This is ADR 0069's "taking an
    event does not move it", applied to the pool: inspection (ADR 0083) and a
    recorded session's `c2h.input` read the event from the envelope, and an
    abandoned macrostep (ADR 0082) frees it and its outgoing list.

13. **The host gives memory back.** `ta_system_trim` is a native call the
    host makes between `ta_run` calls, by its own policy. It frees whole free
    pages above each structure's reserve in the pool, the name area, the park
    area and the deadline index. Core never trims on its own. Items never
    move, so a page goes back only when every item on it is free.

**What this ADR does not decide:** delayed sends as envelopes, exact
`<cancel>` and `maxPendingSends` as a quota inside the pool (item 4 of the
migration plan); the internal queue's storage and the rule that a macrostep
never allocates (item 2); macrostep slots, parking and the chain limit (item
10); the rings and their records (item 1); local invocation tokens beyond
their taking an envelope; and whether withdrawal storms justify an index by
`source_seq` (about 8 bytes per envelope), which the proposal leaves open.

## Consequences

- **A send that cannot be queued fails in the macrostep that made it,** as
  `error.execution.capacity.pool` or `.names`. SCXML abandons the rest of the
  block, so a fanout that runs out fails once, at the first send that does
  not fit.
- **No overflow is held anywhere.** Everything pending is in the pool, under
  one bound; admission refuses at the watermark, and undeliverable mail
  reuses its own envelope. The retry path of ADR 0040's router, the busy path
  of ADR 0055 and the loader's per-family waiting go.
- **No kernel scheduler.** The run queue, the deferred lists and
  `fairnessRatio` replace the kernel's mailboxes, router and load queue, and
  its `resumesPerTurn`, `dueBudget`, `messageBudget`, `maxQueuedMessages` and
  `maxRetainedBytes`. `ta_actor_deliver` and `TA_ERR_BUSY` stop being the
  host's admission path; `eventBlockCache` and the event blocks go.
- **Isolation depends on the host answering loads promptly.** Mail to one
  slow or unknown address can use the pool's headroom, and other charts'
  local sends then fail as `.pool`.
- **Order:** one sender's events to one receiver stay in order; events from
  different senders are ordered by arrival, which a new execution may derive
  differently as resource conditions change.

### Costs

These figures are **estimates** from the proposal, on a 64-bit host. Step
4.2 of the
[implementation plan](../../../docs/proposals/ring-proto/IMPLEMENTATION.md)
measures them and records the bytes per envelope, the pool's default and the
ring defaults in this ADR's implementation notes.

| | Before | After (estimate) |
|---|---|---|
| Per pending input | a `ta_queued_event` of 96 bytes in the host's queue, plus the kernel mailbox; a held external event in a 792-byte event block | one envelope: 88 bytes, plus 4 for its slot in the deadline index; and one 64-byte name-area chunk for short text |
| Pool, default | none in core; event blocks fill on demand up to 1,024 (about 811 KB at the bound) | 4,096 envelopes, about 368 KiB (376,832 bytes) at the bound, filled on demand |
| Name area, default | none | 256 KiB (262,144 bytes) at the bound, filled on demand |
| Per waiting address | napi's loader entry, and the address copied per queued message | one waiting entry, about 40 bytes, plus one copy of the address in the name area; the table finding entries is 8 bytes per slot (64 KiB at the default pool's bound) |
| Per traced envelope | a trace context in the held event | 40 more bytes of name area, usually a second chunk |
| Per actor | 332 bytes (ADR 0092) | +12 bytes: the head of its owner list of delayed sends, and the head and tail of its deferred list |
| Allocations in a macrostep | event-block and queue growth (ADR 0070's retry path) | none for envelopes, entries or text: they are taken only from what exists |

- **A quiet system pays for its peak, not the bound,** as with ADR 0093.
  At their bounds the pool and the name area are about 624 KiB per system,
  against napi's 811 KB prefilled block cache before ADR 0093.

## Implementation notes

Step 4.2 of the ring protocol plan implements mail that waits for a target
that is not resident (decisions 8 and 11) before the rings exist: the host
reads load requests with `ta_system_load_needed()` and answers them with
`ta_system_load_settled()` or `ta_system_load_failed()`, and posts by address
through `ta_actor_deliver_params.address`. Three points differ from the
reference documents:

- **A runtime ID no host could bring back fails at once.** A send to a
  runtime or session ID that no session holds, from a reference that names
  no family (ADR 0088; for example `#_scxml_foo`), does not wait: no host can
  load a target with no address and no family, so core raises
  `error.communication` in the sending macrostep, on its internal queue, as
  SCXML 6.2.4 and the W3C IRP tests 496 and 521 require. Waiting would make
  the error arrive after events the macrostep raised itself. Every other
  target that is not resident waits and asks, as decision 8 says, including
  a runtime ID with a family, which a family loader may answer.
- **Withdrawal scans the pool, as decision 11 says.** `envelope.md`
  describes a B3 table from correlation to envelope. It was built and
  removed: a B3 table grows by rehashing one contiguous array, so at 200,000
  admitted inputs it took one 4 MB block, against the 1 MB the system's
  largest block was at 50,000, and napi's scale test, which requires the
  largest block not to grow with the population, failed. ADR 0065 decision
  6 rejects such growth for tables that scale with the population. The scan
  is bounded by `poolEnvelopes`, runs between macrosteps, allocates nothing,
  and finds runnable inputs by arrival first, then waiting ones.
- **A waiting entry holds the address's handle, not a copy of its text.**
  Core interns every named address it routes to (ADR 0065), so the entry
  keeps the destination's reference, retained once per entry, and its table
  is keyed by the handle; a session ID is keyed by its runtime ID and
  family. This keeps one copy per address with no name-area chunks and no
  failure for want of them. The text copy the reference documents describe
  comes when h2c carries addresses as text (step 5.1), since then no handle
  exists.

