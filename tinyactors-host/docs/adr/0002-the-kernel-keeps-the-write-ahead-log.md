# ADR 0002: The kernel keeps the write-ahead log

**Status:** Proposed (2026-09-30). Takes over the input log from core ADR 0090 (sections 3 to 7) and core ADR 0091 (its byte form), as core ADR 0094 decides. Keeps the behaviour of napi ADRs 0032 to 0034 and 0036, with one change of vocabulary: a message has an identity.
**Date:** 2026-09-30

## Context

The write-ahead log lets an application persist named actors by replay
(napi ADRs 0032 to 0034). Core ADR 0094 removes it from core. The host
sits at the input edge and holds every value, and it owns the queue, so
the log belongs with the queue: in the host kernel (kernel ADR 0001).

Moving it also fixes its vocabulary. Today a message is logged twice:
- `queued` is written by the host, through core's host records;
- `event` is written by core, with the content repeated.

That split follows who owned what. The kernel owns the queue and makes
every call into core, so it can give a message an identity when it is
accepted, log its content once, and record only what happens to it
afterwards.

## Decision

### 1. What the log is

> The input log is a write-ahead log of named actors' inputs. Each turn,
> the kernel hands the binding a batch, and nothing the turn caused leaves
> the process until the application has made that batch durable. The
> records are the log's own vocabulary: exactly what the kernel needs to
> rebuild an actor by replay. The application stores them per actor, in
> order, and hands them back when the kernel asks for an actor. It never
> interprets them, and chart authors never see them.

### 2. A message has an identity from acceptance

When a message for a logged actor is accepted into the kernel's queue
(a mailbox, or a wait for a loader), the kernel gives it an identity:
64 bits, with a hash of the incarnation in the high half and a counter in
the low half, as napi's host numbers are today. It records the content
once:

| Record | When | Carries |
|---|---|---|
| `accepted` | a message enters the queue for a logged actor: mail between actors, a `post()`, a delayed send that fired, an invoker report | id, event name, type, send ID, origin, origin type, invoke ID, **data** |
| `delivered` | the kernel delivers message `id` to core | id, time |
| `dropped` | message `id` will never be delivered: a dead letter, or a `send()` timeout withdrew it | id |

**Every input goes through the kernel's queue,** invoker reports included,
so there is no delivery without an `accepted` record.

The rest of the vocabulary is unchanged from napi ADRs 0032 and 0033:
- `spawned` (revision, binding) and `fired` (a logged actor's delayed send
  was dispatched, with its send ID);
- the operator's actions: `withdrawn`, `abandoned`, `released` and `jump`;
- `finished`, `destroyed` and `evicted`.

### 3. Recording happens at the kernel's call sites, before core

For each input, the kernel:
1. opens a unit in the turn's buffer for that address, and writes the
   record's fixed part and texts;
2. streams each value through the data model's codec with `durable = true`;
3. makes the core call, with the input's time (core ADR 0094 section 1);
4. keeps the unit if core accepted the input, and discards it otherwise.

A value with no durable form refuses the input before core sees it:
`post()` or `send()` fails with `unsupported`, as napi ADR 0034 decided.
A buffer that cannot grow refuses it with out-of-memory, and the binding
retries.

For timers, the kernel's router and I/O adapters write `fired` when core
dispatches a logged actor's delayed send. If that record cannot be written,
the adapter answers RETRY, so the send stays due.

A replaying actor is never recorded.

### 4. The layout

The layout is core ADR 0090 section 3's, owned by the kernel from now on:
- a little-endian fixed part, 8-byte aligned, read in place;
- a text part;
- values as chunked streams, ended by an empty chunk;
- every identifier as text.

Its version becomes 2 for the new kinds (`accepted`, `delivered`,
`dropped`), which replace `queued` and `event`. No stored log needs
reading across the change: nothing is published.

### 5. Batches, wakes and the write-ahead hold

These are unchanged from napi ADRs 0032, 0033 and 0036, and move with the
code:
- a batch is `{ entries: [{ address, bytes }], wakes }`;
- batches are committed in turn order, one at a time;
- a turn's outward effects are held until its commit resolves;
- a wake is the earlier of an actor's first delayed send and its oldest
  undelivered message.

The binding supplies the commit hook: a promise in napi, an explicit call
in the test host.

### 6. Replay

The kernel's replay driver reads a log from a byte source, cut anywhere,
one record at a time:
- **the first record** gives the actor. For `spawned`, the driver calls
  `ta_actor_prepare()` with its time and binding. For a checkpoint, it
  calls `ta_actor_prepare()` with the decoded state (section 7);
- **`accepted`:** the driver holds the message (its fields and decoded
  value) until its `delivered` or `dropped` record arrives. What it holds
  is bounded by the mail that was owed at any point, not by the length of
  the log;
- **`delivered`:** `ta_actor_deliver()` with the held message and the logged
  time, then the actor is resumed until idle, within the pump's budgets;
- **`fired`:** nothing to call. The next input's time moves the clock;
- **operator actions** call core's matching function;
- **`destroyed`** ends a life, so the driver drops the prepared actor and
  starts again from the next `spawned`;
- **a final state** ends the replay (`done`);
- **at the end,** `ta_actor_publish()` with `at` set to the last record's
  time. Then messages still held are put back in the actor's mailbox, in
  identity order, and then the mail that waited for the load.

Memory stays bounded: one record at a time, plus the owed messages.

### 7. Checkpoints

A checkpoint is a group of records: core's exported state
(`ta_actor_export_state()`, core ADR 0094 section 3), encoded by the kernel
with values through the data model's codec, followed by an `accepted`
record for each message the kernel's queue still owes.

Restoring decodes the state into `ta_actor_prepare_params.state`, then
continues with the records after it, skipping any that the checkpoint
covers. The revision rule and the compaction trade-off are unchanged
(core ADR 0091 section 4).

## Consequences

- **The log is defined once, in the kernel.** Both of its consumers (napi,
  the test host in hosted mode) and every future binding get the same log,
  and core never handles its bytes.
- **Each message's data is written once,** in `accepted`. napi measured two
  value writes per logged message before, about half the value-writing
  cost of a logged input.
- **An audit reader pairs `accepted` with `delivered` by identity.** napi's
  `readInputLog` changes its record kinds accordingly.
- **Core's tests of the log** (the byte layout, the four timelines of core
  ADR 0089, checkpoints, the out-of-memory sweeps) move to the kernel's
  tests, and the test host's hosted scenarios.
- **Costs:**
  - per logged message: one value written instead of two;
  - per `delivered`: a fixed record of about 48 bytes;
  - replay holds the owed messages, as napi does today;
  - with no log installed, nothing is recorded and nothing is held.

## Implementation notes

None yet.
