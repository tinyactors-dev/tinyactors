# ADR 0002: The kernel keeps the write-ahead log

**Status:** Accepted and implemented (2026-09-30); see the implementation notes. Takes over the input log from core ADR 0090 (sections 3 to 7) and core ADR 0091 (its byte form), as core ADR 0094 decides. Keeps the behaviour of napi ADRs 0032 to 0034 and 0036, with one change of vocabulary: a message has an identity.
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

### Step 4a: the kernel writes and replays the log (2026-09-30)

ROADMAP "Migrate to the host kernel", step 4, first half. Sections 1 to 7
are implemented. napi and its tests run on them; the test host is untouched.

**Where things are.**

- **`wal.h`/`wal.c`: the layout, version 2.**
  - `tah_wal_write` writes one record and restores the buffer on refusal.
  - `tah_wal_read` and `tah_wal_span` read records in place.
  - `tah_wal_read_value` decodes a value through the data model's codec.
  - The prefix is `wal`, not `log`: `check-recursion` compiles the kernel
    as one unit, and `tah_log_*` is taken by the OTLP encoder's log
    records.
- **`inputs.c`: recording at the call sites.** It answers the scheduler's
  notes and the lifecycle hooks, and is not core's input sink:
  - `tah_inputs_configure` no longer installs the sink or calls
    `ta_system_log_family`;
  - `tah_inputs_set_datamodel` names the codec, which napi sets to its
    built-in data model.
- **`replay.c`: the replay driver and checkpoints.** napi's
  `napi_replay.c` is now Node-API glue over them, and its JavaScript shapes
  are unchanged.

**Decisions.**

- **A record is written before core is called.** Each record is written
  into the turn's entry, and a `tah_input_mark` (entry, offset, the actor's
  previous record number) undoes it when core refuses.
  - The scheduler has three new notes: `DELIVERING` before
    `ta_actor_deliver`, `REPORTING` before `ta_actor_report_invoke`, and
    `UNDELIVERED` when either refused.
  - For `DELIVERING` and `REPORTING`, `unsupported` makes a dead letter
    (`refused`). Any other failure waits a turn, as out-of-memory does.
- **Record numbers.** `seq` counts a resident actor's records in its life,
  from 1 at `spawned`, and `inputs.actor_seq` tracks it.
  - A record written while the address has no resident actor (mail waiting
    for a loader, a `dropped` for it) has seq 0.
  - A replay's published actor continues from the log's last number
    (`tah_replay_finish`).
- **`accepted` is written when the message is queued**, invoker reports
  included (an empty name, and `report` set to the kind).
  - A message a replay owes keeps its identity when it is put back, and
    gets no second `accepted`.
  - A delivery of an envelope with no identity (queued before the log was
    turned on) writes `accepted` first.
- **`released`** is written at the first resume without the microstep limit
  after a release (`TAH_SCHED_RELEASE_NOTED`). A release that lifts
  nothing, such as after an abandon, writes nothing, as before.
- **`withdrawn` and `abandoned`** come from `tah_system_withdraw_send` and
  `tah_system_abandon`, which record, then call core. napi's
  `withdrawSend` and `abandon` call them. `ta_actor_abandon` lost its
  generated hook, and `ta_actor_{abandon,withdraw_send}` are plumbing in
  `bindings.rb`.
- **`destroyed`** comes from `tah_inputs_destroying` (napi's
  `inputForget`); `spawned` comes from the new `tah_hook_actor_spawned`,
  which destroys the actor again when the record cannot be written.
- **`fired`** is written by the router and the I/O adapter while due sends
  are dispatched.
  - It is written before the post, and a record that cannot be written
    answers RETRY (`TA_OP_NOMEM` from the I/O adapter).
  - Only a RETRY undoes it: an error (an unreachable target, data the
    receiver cannot log) consumed the send as well.
  - A send retried after a RETRY writes one `fired`, on the attempt that
    took it. Replay treats `fired` as time only in any case.
- **Replay** follows section 6:
  - `accepted` records are held as owed and deduplicated by identity;
  - `delivered` or `jump` delivers or reports the held message at the
    logged time;
  - `fired` and `evicted` carry time only;
  - napi publishes at the last record's time (`replayLastTime`).
- **Deviations in replay from version 1:**
  - mail accepted before a life's first record (it waited for a load) is
    held as owed, where version 1 skipped it: `delivered` now needs the
    content from `accepted`;
  - a `jump` whose target no longer resolves in the revision answers
    `invalid`.
- **Checkpoints moved in this step, not in 4b,** because napi's checkpoint
  tests must stay green.
  - `tah_checkpoint_write` writes the unit: `checkpoint`, one
    `checkpoint-send` per pending send, one `checkpoint-invoke` per active
    invocation, and an `accepted` per owed message. It writes from
    `ta_actor_export_state` through the sink, one record at a time; the
    largest record (the data root) is held in memory once.
  - Restoring reads it into `ta_actor_prepare_params.state`. Records with
    seq at or below the checkpoint's are skipped, and a checkpoint unit in
    the middle of a log is skipped.
- **Two core additions:** `ta_send_id_intern` and
  `ta_system_intern_invoke_id`, so replay can turn logged text back into
  IDs (commit c7d1502).
- **Removed:** the scheduler observer's `host_seq`. Delivery no longer
  passes a host number to core.

**Tests.**

- `wal_test`: every kind, the golden header bytes, reads in pieces of 1, 2,
  5 and 4096 bytes, the checkpoint unit's span, refusal of an unknown
  version or kind, restore after `unsupported`, and an out-of-memory sweep.
- `inputs_test`, at the call sites:
  - a life's records, golden;
  - the refusal of data with no durable form, which leaves no record and no
    gap in the record numbers;
  - an out-of-memory sweep over every allocation from spawn to finish, each
    leaving whole records, numbers without gaps, no delivery without its
    acceptance, and nothing leaked.
- napi's `input-log.js` and `replay.js` use the new kinds. `replay.js`
  writes its hand-made logs at version 2 and checks that a 10 MB value is
  logged once.

**Measurement: bytes per logged message.** A message `inc` with data
`{ by: 1 }` took 240 bytes at version 1 (`queued` and `event`, 120 each;
napi ADR 0034). It now takes **184 bytes**: `accepted` 144 and `delivered`
40, measured in napi and checked in `inputs_test`. The value is written
once, not twice.

**Left for step 4b (and step 5).**

- **Core still has its log**, compiled but unused by the kernel and napi:
  the input sink, host records, `ta_actor_replay`,
  `ta_actor_prepare_params.log`, `host_seq` on deliver, and checkpoint
  bytes. Its tests (the v1 layout, core ADR 0089's four timelines, the
  checkpoint sweeps) still run in core. Step 5 removes them, once their
  equivalents are in the kernel's tests.
- **Core ADR 0089's four timelines** as kernel tests (hosted, through
  `replay.c`) are not written yet. napi's `replay.js` covers them end to
  end.
- **The test host still drives core's log** (`ta_system_log_family`,
  `ta_actor_replay` in `scenario.rb`). Its persistence scenarios move to
  hosted mode in step 7, and must before step 5 lands.
- **`replay.c` has no native test of its own** beyond what napi
  exercises: prepare, run, redeliver, checkpoint round trip, and an
  out-of-memory sweep over replay are the next kernel tests to write.

### Step 4b: replay's coverage (2026-09-30)

Three attempts at native replay tests (`tests/replay_test.c`) stalled
while an API outage lasted, and left nothing usable. The coverage they
were for comes instead from the test host's hosted scenarios (kernel ADR
0001, step 7) and napi's end-to-end `test/replay.js`.

The hosted scenarios exercise `replay.c` through the kernel's public API
from a second binding:
- preparing from `spawned`;
- running the records and publishing at the last record's time;
- a pending timer that survives the replay;
- a large value streamed through the jq codec;
- a checkpoint as the whole log;
- a restore in another system after a crash.

They found one bug, a checkpoint's self-send (`3cb4e60`). An out-of-memory
sweep over replay stays with napi's and the kernel's existing
`inputs_test` sweeps, and is not repeated per binding.

### Step 4b: native replay tests after all (2026-09-30)

`tests/replay_test.c` now exists. It drives `replay.c` as a binding
does (open, push, prepare, run, finish, `ta_actor_publish()` with the
published hook, redeliver), and covers Step 4a's list of what was left:
- prepare, run and redeliver: the log pushed whole, cut in two at every
  byte and in small pieces; MORE, NEED and FINISHED; owed mail put back
  with its ID and data, and record numbers that continue the log's;
- the checkpoint round trip: a restore from a checkpoint alone, from a
  checkpoint followed by the records after it, and from a whole log with
  a checkpoint in the middle; `tah_checkpoint_write`'s refusals;
- two lives at one address (ENDED, then LIFE), CORRUPT and
  DEFINITION_MISMATCH;
- core ADR 0089 section 2's four timelines, as the kernel logs them (a
  timer's firing is a `fired` record carrying time only). All four behave
  as that ADR describes;
- an out-of-memory sweep over the whole sequence, a checkpoint written and
  restored included: each failed step answers NOMEM, leaves no actor
  behind and leaks nothing, and succeeds when done again. The turn that
  delivers the owed mail between the two runs no failure: the scheduler's
  out-of-memory handling is `inputs_test`'s and the scheduler's to sweep.

**This sweep replaces the line above that an out-of-memory sweep over
replay "is not repeated per binding".** It is the kernel's, so no binding
needs one. The sweep is exhaustive (AGENTS.md): the fast pass fails the
first eight allocations and every 16th after; `mise run test --full`
(`test:full`) sets `TAH_FULL=1`, and fails every one.

Writing the tests found two bugs, fixed in their own commits:
- `tah_wal_span()` answered a checkpoint unit whole when the bytes ended
  right after its CHECKPOINT record, so a checkpoint pushed in pieces
  could be read without its parts (CORRUPT) (`01c2a86`);
- `tah_replay_prepare()` left the prepared actor behind, unknown to the
  caller, when its bootstrap from `spawned` ran out of memory (`65f505e`).

### Step 4a, amended: checkpoints restore submitted sends (2026-09-30)

Step 4a wrote a send the router had refused into a checkpoint
(`checkpoint-send` with `TAH_WAL_SEND_SUBMITTED`), and core did not
restore it. Since its `fired` is written only by the attempt that takes
it, a checkpoint taken while it waited lost it after a crash. Core ADR
0095 restores it, and changes two lines of Step 4a:
- **"Replay treats `fired` as time only in any case" no longer holds.**
  `fired` still moves no clock itself, but the replay passes its send ID
  to `ta_actor_drop_submitted()`: a held submitted send with that ID was
  taken after the checkpoint, and is not sent again.
- **`evicted` drops every held submitted send:** the process that evicted
  the actor keeps them as orphans and delivers them.
- **`fired` always carries its send ID.** `tah_inputs_fired()` answers out
  of memory, so the router answers RETRY, when it cannot render the ID.

`tah_replay_read_checkpoint()` already handed the flag to core; nothing
else in the kernel changed. The tests are in `tests/replay_test.c`; core
ADR 0095 lists them.

### Step 4b: what the sweep found in the mailbox (2026-09-30)

The replay sweep above avoided two paths where the kernel lost mail when
an allocation failed. Both are fixed:
- **A message queued without its ID.** `tah_inputs_number()` put the
  message's ID in two maps after its `accepted` record was written, and
  when a map could not grow the message was queued anyway, unnumbered.
  `tah_checkpoint_owed()` skips unnumbered mail, so a checkpoint left it
  out, and a message a replay redelivered lost its original ID the same
  way. Now the ID is reserved first: the post is refused with out of
  memory and nothing is queued, the same answer a batch that cannot grow
  gives. A refused `accepted` takes the ID back. A redelivery that is
  refused makes `tah_replay_redeliver()` answer out of memory. A delivery of mail queued before the log was on numbers it the
  same way, and waits a turn when it cannot. The sweep now writes a
  checkpoint while the ping is still owed and restores it.
