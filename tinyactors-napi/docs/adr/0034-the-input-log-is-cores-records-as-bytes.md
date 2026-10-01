# ADR 0034: The input log is core's records, as bytes

**Status:** Superseded by [kernel ADR 0006](../../../tinyactors-host/docs/adr/0006-actors-record-their-events-through-the-application-and-are-rebuilt-by-replaying-them.md) (2026-10-01): records are `ActorEvent`s handed to a recorder, not bytes. Accepted and implemented (2026-09-30); see the implementation notes. Applies core ADR 0090 (the input log, its byte layout and its replay) and core ADR 0091 (checkpoints). Would refine napi ADRs 0032 and 0033: the records become bytes core writes, the replay driver becomes core's reducer, and checkpoints are added. Its code moved to the host kernel: see [kernel ADR 0001](../../../tinyactors-host/docs/adr/0001-the-host-kernel-holds-what-every-host-needs.md) section 6 and [ADR 0037](0037-napi-is-a-binding-over-the-host-kernel.md). Section 5's vocabulary superseded by [kernel ADR 0005](../../../tinyactors-host/docs/adr/0005-an-actors-log-is-an-inbox-with-acknowledgements.md) and [napi ADR 0040](0040-effects-are-not-held-and-send-resolves-once-its-message-is-done-and-committed.md) (2026-10-01): `readInputLog()` reads layout version 3 (`spawned`, `accepted`, `done`, `fired`, `destroyed`, checkpoints).
**Date:** 2026-09-29

## Context

Napi ADRs 0032 and 0033 record an actor's inputs in napi
(`src/inputs.c`, `src/napi_inputs.c`), hand them to JavaScript as objects
holding live values, and replay them with a driver in `lib/system.js`.
Core ADR 0090 moves all of this into core:
- which inputs are recorded, and when;
- a byte layout that carries no process-local identifiers;
- a data-model value codec that streams through byte sinks;
- a reducer that applies one record at a time.

Core ADR 0091 adds checkpoints. The goal is memory that stays bounded
on both paths, writing and loading, whatever the size of a value or the
length of a log.

## Decision

### 1. A batch is bytes

```ts
interface InputBatch {
  readonly entries: readonly { readonly address: ActorAddress; readonly bytes: Uint8Array }[];
  readonly wakes: readonly { readonly address: ActorAddress; readonly at: number | null }[];
}
```

- **Core writes records to napi's input sink,** which appends them to the
  turn's buffer for their address. Each entry holds one address's records
  from that turn, back to back.
- **The application appends `bytes` to that address's log as they are.**
  It does not parse them. Wakes are unchanged from ADR 0033.
- **Memory is bounded by one turn's records.** A batch's buffers are reset,
  not freed, after the commit (napi ADR 0018).
- **napi maps its calls onto core's:**
  - `setInputLog(sink, { families })` calls `ta_system_log_family()` for each
    family;
  - mailbox messages are written as host records (`queued`, `dropped`,
    `evicted`, core ADR 0090 section 6);
  - `src/inputs.c` stops recording core's inputs itself, and keeps only the
    batching, the wakes and the write-ahead hold.

### 2. JavaScript values are written durably in ADR 0031's form

napi's data model implements `value_write` and `value_read` (core ADR
0090 section 2):
- **`durable = true`** writes a value in the text that napi's built-in data
  model prints and reads back (ADR 0031): JSON, and `new Name(arguments)`
  for registered classes, `Date`, `Map`, `Set`, `RegExp` and typed
  arrays, as UTF-8.
  - It is written in chunks, by a printer that walks the value with an
    explicit stack, so a large value never exists whole as a string.
  - Reading parses the chunks as they arrive.
  - A value the built-in printer refuses answers `TA_OP_ERROR`: a
    function, a symbol, a cycle, or a class instance that doesn't describe
    itself. So an input carrying such a value to a logged actor is refused
    (`unsupported`), and `post()` or `send()` throws.
- **`durable = false`** (offload) keeps today's form: a handle to the value,
  valid in this process.

The same codec serves a data model registered with `registerDatamodel()`,
because its values are napi's values (ADR 0019).

### 3. A loader hands over bytes

```ts
{ readonly replay: { readonly spawn: Definition;
                     readonly inputs: Iterable<Uint8Array> | AsyncIterable<Uint8Array>;
                     readonly onReport?: (report: ReplayReport) => void } }
```

- **`inputs` is the address's log as byte chunks, cut anywhere:** a file's
  read stream, or a database cursor over stored batches. napi wraps it as a
  core byte source.
- **napi prepares from it** (`ta_actor_prepare_params.log`), then calls
  `ta_actor_replay()` until the source ends, resuming the actor within the
  pump's budgets whenever core answers `TA_REPLAY_RUN`.
- **It keeps the unmatched `queued` messages** that core hands back, and
  delivers them after publishing, then the mail that waited for the load
  (ADR 0033).
- **An asynchronous source** is read ahead by one chunk. Replay holds that
  chunk, one record, and the owed mail, whatever the length of the log.

`PreparedActor.replay(inputs)` takes the same byte chunks, for upgrades.
`prepare(definition, { incumbent | address, log })` prepares from a log's
first record: `spawned`, or a checkpoint.

### 4. Checkpoints

```ts
system.checkpoint(actor: ActorID, write: (chunk: Uint8Array) => void): void;
```

- **Core writes the checkpoint (core ADR 0091) in chunks** through `write`,
  synchronously. The chunks are views into a reused buffer, so the callback
  copies or writes them out before it returns.
- **It throws:** `busy` for an actor that is not idle, `invalid` for one
  that is unnamed or not logged, and `unsupported` for a value with no
  durable form.
- **A log that starts with a checkpoint** replays from it. Records the
  checkpoint covers may still follow, and are skipped.
- **Compaction is the application's choice.** Once a checkpoint is durable,
  it may replace the records before it. A checkpoint loads only under its
  own revision, so an upgrade after compaction migrates the state instead
  of replaying from `spawned` (core ADR 0091 section 4).

### 5. Reading a log for an audit trail

```ts
readInputLog(chunks: Iterable<Uint8Array> | AsyncIterable<Uint8Array>): AsyncIterable<InputRecord>;
```

It decodes core's layout into the `InputRecord` objects of ADR 0032,
one record at a time, with values decoded by the built-in data model's
reader. It is for showing an actor's history, such as granary's admin UI.
Replay never needs it.

## Consequences

- **The application stores bytes it never parses, and loads by streaming
  them.** Memory is bounded:
  - writing needs one turn's records plus the printer's walk of a value;
  - loading needs one chunk, one record and the owed mail;
  - a checkpoint needs its chunk buffer.
- **Behaviour changes:**
  - an input to a logged actor carrying a value with no durable form is
    refused, where napi ADR 0032 took it;
  - `InputBatch.entries` holds bytes, and `replay` and `inputs` take bytes;
  - `InputRecord` survives only as `readInputLog`'s output.
- **A log written by napi replays in the test host,** and the other way
  round, for charts on data models that write the same durable form.
- **napi's recording code shrinks** to batching, wakes and the write-ahead
  hold. The replay driver becomes a loop around `ta_actor_replay()`.
- **Costs:** measured when this is implemented, against napi ADR 0033's
  figures:
  - bytes per logged input;
  - ns per logged input (printing replaces the application's own encoding);
  - replay ns per event;
  - peak memory for a 100,000-record log and a 10 MB value.

## Implementation notes

- **Where:**
  - core: `ta_system_input_owner()` (`include/tinyactors.h`, `src/input_log.c`), which the sink asks whose record it writes; `src/actor.c` names a spawn's address while it writes SPAWNED, before the claim (commit `842fc45`).
  - napi: `src/inputs.{c,h}` (the sink that files records by address, the entries and their reused buffers, the mailbox's host records, the wakes, `tna_record_span`), `src/napi_inputs.{c,h}` (the batch of bytes, `writeEvicted`, `inputForget`), new `src/napi_replay.{c,h}` (the replay feed and driver, `checkpoint`), `src/scheduler.{c,h}` (an observer that can refuse a `queued`, host numbers passed on delivery, `TNA_SCHED_REPLAYING`; the replay-time map removed), `src/router.c` (a refused `queued` is `error.communication`), `lib/system.js`, new `lib/inputlog.js` (`readInputLog`), `index.js`, `index.d.ts`, `README.md` (commit `33694e9` and the docs commit).
- **Removed** (superseded, per AGENTS.md): napi's own recording of core's inputs (spawned, event, report, withdrawn, abandoned, released, finished, fired), the object records and their JavaScript conversion, `inputHost`, `redeliver`, `replaySend`, `requeue()`, the scheduler's replay times, `stoppedAt`/`spawned` options, and the old windowed replay driver.
- **The sink needed a core addition (not in the ADR):** core's sink carries no actor, so a sink that files records by address could not know whose record it was writing. `ta_system_input_owner()` answers the actor and its address handle from `begin` to `end`; during SPAWNED the address is not claimed yet, so it answers the address's text and napi interns it.
- **Replay is native and synchronous per window.** A feed (`replayFeed`) holds the bytes JavaScript pushed; napi hands core a record only once `tna_record_span` sees all of it (a checkpoint's whole unit for `prepare`), so an asynchronous source never leaves core half a record. After each `ta_actor_replay()` step RUN or RELEASE, napi resumes the unpublished actor itself until it is idle, in the same call (it crosses no border, so nothing waits for a turn), and the scheduler leaves it alone (`TNA_SCHED_REPLAYING`) until `publish`. `replayRun` applies up to 256 records, then JavaScript yields to the event loop. The feed keeps owed `queued` messages (core's TA_REPLAY_HOST) and releases those matched by an `event` or `dropped` host number; after the publish, the ones another incarnation queued are posted again with their host numbers, and no new `queued` record.
- **Host numbers** are 64-bit: the incarnation's hash in the high half, a counter in the low half, so logs from different processes never collide in matching. `readInputLog` shows them as `hostSeq` (text), apart from the record's own number `seq`.
- **Lives and leading host records (not in the ADR).** After `destroyed` (TA_REPLAY_ENDED), the replay takes the host records up to the next life's first record with the same replaying actor (`replayRun(..., between = true)`), so mail queued for the address between lives reaches the next life (or the fresh actor the load spawns). Host records before any life's first record (mail that waited for the loader of an address no actor had yet) have no actor to decode them and are skipped; that mail reaches the actor from memory, and after a crash it is lost. A log whose only records are such mail loads as a fresh actor.
- **`prepare({ log })`** takes the start of a log as one `Uint8Array` holding at least the first record (a whole checkpoint unit, when it starts with one); the rest stays in the prepared actor's feed and `replay(chunks)` continues from it. A replaying actor prepared with `at` joins a feed on its first `replay()`, which runs its bootstrap first.
- **`publish()`** of a replaying actor clears the replay flag, publishes at the actor's clock (`at` still moves it first), puts back the owed messages, and closes the feed.
- **`checkpoint(actor, write)`** gathers the numbered messages in the actor's mailbox as `owed`, and writes through a 64 KiB buffer: `write` gets a copy of each full buffer, synchronously, from inside the core call, so it must not call the system (`reentrant`).
- **`released`** is written by core only when a resume lifts the limit on a macrostep stopped past it: releasing a runaway after `abandon()` writes nothing, since nothing was lifted (`test/input-log.js`).
- **Entries group records by address within a turn**, as §1 says, so a batch's flattened order is per address, not the order core accepted inputs across actors.
- **Tests (Node and Bun):**
  - `test/input-log.js` (ported to bytes, decoded with `readInputLog`): each kind per address in core's order; spawned records name their address; `queued`/`event` pairs share a host number, host numbers unique; each actor's records numbered from 1; a batch is `{ entries, wakes }`; reports; abandon; a released runaway writes `released`; the write-ahead hold; a failed commit faults the batch's actors; `fired` under a dispatch budget and the wakes; `dropped` for a withdrawn and a buried message; `jump` by state names; mail waiting for a loader; no sink, no hold.
  - `test/replay.js` (ported): the end-to-end loop with a two-table store of opaque bytes; an evicted timeout on time; a timeout after a restart; a crash with mail in flight (exactly once); dropped messages and literal send IDs; a jump replayed; a finished history is `done`; `destroyed` frees the address and a new life replays; effects at most once across a crash; an upgrade by replay (`prepare({ incumbent, log })`, `replay([])`); a report and a runaway's operations; a 100,000-record log written by hand in core's layout and streamed from an async generator; a 10 MB value logged and replayed in 64 KiB pieces; checkpoints and compaction keeping state, pending sends and owed mail, refused under another revision and for an unnamed actor; a non-durable input refused (`unsupported`); refusals (`corrupt` bytes, a short `log`).
  - `test/types/usage.ts`, `test/version.js` (`readInputLog` exported); IRP 176/176 on Node and Bun.
- **Not done:** replaying a napi log in the test host. The layout is the same, but the test host's data models write their own durable form (JSON text for the JSON model), which reads napi's only for values that are plain JSON; no test runs it.
- **Costs** (Apple M4 Max; napi ADR 0033's figures in brackets):
  - **No input log (bench-compare 22f2192, 100,000 actors, medians of 2 alternating runs):** Node (machine 84c362297e3d) untraced 660 → 675 ns/event (+2.3%), core 281 and native 329 B/actor unchanged, RSS 756 → 759 B/actor; every traced variant within 5% ("nothing outside the 5% rule"). Bun (bbe6bf531954) untraced 564 → 554 ns/event (−1.6%), native 329 B/actor unchanged, RSS 1,010 → 1,021 B/actor. [ADR 0033: Node 650 → 657, Bun 545 → 547.]
  - **Bytes per logged input:** 240 bytes for a message with data `{ by: 1 }`, as its `queued` and `event` records together (192 without data): each record is 32 bytes of header plus 8 per field, its texts, and the value's chunks. [ADR 0033 handed objects; the application's encoding set the size.]
  - **ns per logged input** (1,000 actors × 50 messages, one flush per round): Node 9,081 ns against 767 unlogged (+8.3 µs); Bun 6,576 against 669 (+5.9 µs). Without data, Node 3,574 against 716 (+2.9 µs). So a value costs about 2.7 µs per durable write, and a message's value is written twice (`queued`, then `event`): napi's durable writer (ADR 0031's printer, `lib/durable.js`) is called from C through JavaScript once per value. That call is the first thing to make cheaper, for example a native writer for plain JSON values, or writing the value once, in `queued`, with the `event` record naming its host number. [ADR 0033 made objects in C and handed them to JavaScript per turn; its per-input time was not measured.]
  - **Replay, one actor, records streamed from an async generator:** 10,000 events Node 4.71 µs, Bun 2.75 µs per event; 100,000 events Node 2.47 µs, Bun 1.94 µs; 100,000 events with 2 KB values Node 9.62 µs, Bun 7.38 µs. [ADR 0032/0033's driver: 10,000 events Node 22.2, Bun 5.8; 100,000 Node 37.0, Bun 21.6 µs per event.] Records are applied natively, 256 between yields, instead of one pump turn per window.
  - **Peak memory, 100,000-record log with 2 KB values (over 200 MB if held):** the heap grew by 0.1 MB on Node and Bun: one record and one chunk are held.
  - **10 MB value:** 20.0 MB of records (`queued` and `event`). Replaying it in 64 KiB pieces grew Node's heap by 9.1 MB (the value the replayed actor holds) and RSS by 10.2 MB. On Bun the heap grew by 0.0 MB after collection but RSS by 339 MB, which the collector did not return to the system during the run: the feed holds the record (about 10 MB) while core reads it, and the durable reader builds the string from 16 KB pieces; Bun's retained RSS is not explained further here.

### Addendum (2026-09-30): the durable reader scans each piece once

- **What was wrong.** The reader (`lib/durable.js`) kept the text it had
  not consumed and rescanned it as pieces arrived; a string token was found
  by rescanning the accumulated text, then sliced whole and handed to
  `JSON.parse`. A 10 MB string was copied many times over while it was
  read. After replaying a 10 MB value, Bun's RSS had grown by 360 MB and
  was not returned; Node's peaked at +201 MB and fell back after a
  collection (this machine, measured before the change; the 339 MB above
  was the same effect on another run).
- **What it is now.** A scanner that consumes each piece once, in `71f6572`:
  - numbers and words keep only their own few characters (at most 1,024,
    more is refused);
  - a string gathers short runs and escapes as UTF-16 code units in an
    8,192-unit buffer, and long runs as slices of the piece; runs are
    kept in segments of about 16,384 characters and joined once, at the
    closing quote. A string that closes within its piece with nothing
    gathered, most keys, is one slice;
  - pieces may be strings or UTF-8 bytes cut anywhere: bytes go through one
    streaming decoder with `fatal: true`, so a character split across pieces
    is whole when scanned, and malformed UTF-8 is refused. `readInputLog`
    now hands the value streams' bytes straight to the reader;
  - a refusal names the character where the text goes wrong (a number or a
    word at its first character); the grammar (ADR 0031's forms, registered
    classes, `__proto__` as an own property) is unchanged.
- **Figures** (M4 Max; reading with `durableValue` in 16 KiB pieces; before
  → after):

  | | Node | Bun |
  |---|---|---|
  | long string (10 MB of `é`) | 66 → 416 MB/s | 171 → 1,254 MB/s |
  | long ASCII string (10 MB) | 37 → 1,101 MB/s | 176 → 8,115 MB/s |
  | escape-dense string (3.8 MB) | 62 → 144 MB/s | 198 → 356 MB/s |
  | structured (100,000 small objects, 6.4 MB) | 31 → 63 MB/s | 49 → 94 MB/s |
  | replaying a 10 MB value: RSS growth | +201 MB peak (+13 after GC) → +31 MB (+11 after GC) | +360 MB, kept → +34 MB, kept |

  The replay's growth is now about the value the replayed actor holds, plus
  the record the feed holds while core reads it. `test/replay.js` asserts
  RSS grows by less than 120 MB there.
- **Per logged input: unchanged.** Logging writes values; the reader is not
  on that path. 1,000 actors × 50 messages with data `{ by: 1 }`: Node +5.5
  µs (5,996 against 510 ns unlogged), Bun +3.8 µs (4,230 against 429),
  before and after, on this machine. (The +8.3 and +5.9 µs above were
  measured on another run.)
- **Tests** (`test/durable.js`, Node and Bun): every existing case read from
  pieces of 1, 2, 3, 7 and 4096 bytes; a text holding every token kind and
  every escape (`\"`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t`, `é`, a
  surrogate pair), cut at every position into two pieces and at a grid of
  positions into three, and as bytes cut inside 2- and 4-byte characters and
  one byte at a time; 500 random values through the printer, cut into
  random byte pieces of 1 to 9 bytes; refusals at the right character
  (`[1,2x]` at 4, `{"a" 1}` at 5, a raw control character, `\q`,
  `\u12G4`, `01`, `1.`, `nope`, `1 2`, an unclosed string, a truncated
  array, empty text, an unknown class, a 2,000-digit number, malformed
  UTF-8 in the middle and at the end).
- **Benchmark** (`bench-compare 51c907a`, machines a99e44a37e2e for Node and
  c989bd79a51f for Bun): nothing slower outside the 5% rule; the two rows
  flagged are RSS decreases (Node `decoded` −6.4%, Bun `full + values`
  −6.7%). The benchmark installs no input log, so it does not exercise the
  reader.

### Addendum (2026-09-30): a logged input measured at version 2; no native writer

- **What was measured.** The figures above came from a script that was
  not committed, and at layout version 1, which wrote a message's value
  twice (`queued`, then `event`). Version 2 (kernel ADR 0002) writes it
  once, in `accepted`; `delivered` names it by identity. The bench is now
  `test/bench/input_log.js`, which `mise run bench` and `mise run
  bench-compare` run beside the tracing bench: 1,000 named actors of one
  family, 50 `inc` messages to each per round and one flush, five rounds,
  with an input log whose sink counts the batches' bytes and drops them.
- **Figures** (`mise run bench`, commit `ecf3693` plus this bench; Apple M4
  Max, machines a99e44a37e2e for Node 22.23.2 and c989bd79a51f for Bun
  1.4.1; best round):

  | | unlogged, data `{ by: 1 }` | logged, data `{ by: 1 }` | logged, no data |
  |---|---|---|---|
  | Node ns/message | 504 | 3,712 (+3.2 µs) | 1,823 (+1.3 µs) |
  | Bun ns/message | 450 | 2,927 (+2.5 µs) | 1,450 (+1.0 µs) |
  | log bytes/message | - | 184 | 160 |
  | allocator calls/message | 0.01 | 2.09 | 0.09 |
  | native B/actor (both) | 657 | 1,020 | 1,020 |
  | RSS B/actor, Node / Bun | 3,310 / 7,651 | 7,324 / 14,959 | 7,455 / 17,646 |
  | heap B/actor, Node / Bun | 444 / 353 | 501 / 523 | 500 / 523 |

  So a logged message costs +3.2 µs on Node and +2.5 µs on Bun, against
  +5.5 and +3.8 µs in the addendum above (another day: only this run's
  figures compare with each other). The value is about 1.9 µs (Node) and
  1.5 µs (Bun) of it, and the two allocator calls per message: the one
  durable write through JavaScript per value. A `bench-compare HEAD` of
  the same tree (2 alternating runs) gave the same ns per message within
  1%. Native bytes per actor grow by 363 with a log installed; RSS per actor
  at 1,000 actors is noisy.
- **Decision (the owner's, 2026-09-30): option B is not built.** A native durable writer for plain
  JSON values (byte-exact with ADR 0031's printer) would save at most the
  value's share, under 2 µs per message, while adding a second printer
  that must stay byte-exact with `lib/durable.js`. At version 2 the cost
  is about 6x an unlogged message, and it is paid only by the families an
  application logs. The bench stays, so a change to this path is compared
  side by side.
