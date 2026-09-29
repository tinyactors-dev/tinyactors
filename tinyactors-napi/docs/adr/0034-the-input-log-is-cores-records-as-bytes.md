# ADR 0034: The input log is core's records, as bytes

**Status:** Proposed (2026-09-29). Applies core ADR 0090 (the input log, its byte layout and its replay) and core ADR 0091 (checkpoints). Would refine napi ADRs 0032 and 0033: the records become bytes core writes, the replay driver becomes core's reducer, and checkpoints are added.
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

None yet.
