# ADR 0091: A checkpoint is an actor's state at a record of its log

**Status:** Changed by [kernel ADR 0006](../../../tinyactors-host/docs/adr/0006-actors-record-their-events-through-the-application-and-are-rebuilt-by-replaying-them.md) (2026-10-01): a checkpoint is no longer a record of a log; its state export and import are the application's snapshots. Accepted and implemented (2026-09-29); see the implementation notes. Builds on ADR 0090 (the input log, its byte layout and its replay). Refines ADR 0089: a replay can start from a checkpoint instead of from `spawned`. Amended in place while Proposed, when it was implemented: a checkpoint is a unit of records rather than one record, it carries the messages the host still owes (section 5), and it carries no invocation values. Its byte form is superseded by ADR 0094 and removed from core (2026-09-30): core exports and restores structured state, and the host kernel encodes checkpoints (kernel ADR 0002). Section 1's rule that a send the router refused is not in a checkpoint is amended by ADR 0095 (2026-09-30): it is restored; that amendment is withdrawn by [kernel ADR 0003](../../../tinyactors-host/docs/adr/0003-a-full-mailbox-overflows-instead-of-refusing-mail-between-actors.md), and the rule holds again. Section 5 under [kernel ADR 0005](../../../tinyactors-host/docs/adr/0005-an-actors-log-is-an-inbox-with-acknowledgements.md) (2026-10-01): the messages the host still owes are the ones not marked `done`.
**Date:** 2026-09-29

## Context

With ADR 0090, a log replays in bounded memory, but not in bounded time.
A replay applies every record since `spawned`. napi measured about 22 µs
per event on Node and 6 µs on Bun for a single actor (napi ADR 0032's
notes), so a million-event history takes around 20 seconds to load, and
storage grows without limit.

A session image (ADR 0038) holds an actor's whole state, but only within
one process. It is in-memory, and its offload bytes are valid only in the
system that wrote them (ADR 0074). What a log needs is the same state
in a form that survives the process: written and read in pieces, like a
record (ADR 0090). A replay can then start from it.

## Decision

### 1. A checkpoint is a unit of records

A checkpoint is written as one unit of records in ADR 0090's layout, all
with the same `seq`, the number of the last log record it includes, and
the same `time`, the actor's clock:

| Record | Contents |
|---|---|
| `CHECKPOINT` | family, name and revision; the generated-ID counter (send and invoke IDs, ADR 0089 section 5); the session's step counters (ADR 0067); how many parts and owed messages follow; the active atomic states' IDs; the IDs of the states whose late-bound data is initialized (ADR 0032). Value: the data root |
| `CHECKPOINT_HISTORY` | one per history state that has recorded states: its ID and the recorded states' IDs |
| `CHECKPOINT_SEND` | one per pending delayed send: its send ID, event name, I/O type, destination as a target URI (or marked as the actor itself), due time, and the content IDs of a literal target or literal content. Values: its data, and its target value |
| `CHECKPOINT_INVOKE` | one per active invocation: its ID, the invoker's type, and its `<invoke>`'s position in the chart |
| `QUEUED` | one per message the host still owes (section 5) |

A unit of records, rather than one record with a list of everything, keeps
ADR 0090's rules: each record has fixed fields, a text part and at most a
couple of value streams, and a reader holds one record at a time.

- **State is written by ID,** the chart's text, never by number.
- **Every value goes through `value_write` with `durable = true`,** in
  pieces.
- **A checkpoint holds only what an idle actor has.** There is no internal
  queue, no continuation and no external event in it, because only an idle
  actor can be checkpointed (section 2).
- **A send the router already refused once is not in it.** Such a send has
  been sent (ADR 0054), its `fired` record is in the log, and it belongs to
  its receiver.
- **An invocation carries no values.** Its request's values were for the
  invoker, which a replaying actor never calls (ADR 0089 section 3), and a
  restored invocation is retired at publish or matched by the service's
  later events. Its ID, type and position are all that is needed.

### 2. Taking a checkpoint

```c
typedef struct { uint64_t host_seq; const ta_event *event; } ta_owed_message;
typedef struct {
    ta_actor_id actor;
    ta_byte_sink sink;
    const ta_value *root;              /* optional: null for the data model's inspect() view */
    const ta_owed_message *owed;       /* the messages the host holds for it (section 5) */
    uint32_t owed_count;
} ta_actor_checkpoint_params;
TA_API ta_result ta_actor_checkpoint(ta_system *system, TA_IN const ta_actor_checkpoint_params *params);
```

- **Only a named, idle, published actor of a logged family can be
  checkpointed.** Otherwise the answer is:
  - `TA_ERR_BUSY` while it is inside a macrostep, holds an event, or has
    queued internal events;
  - `TA_ERR_INVALID` if it is unnamed, unpublished or not logged;
  - `TA_ERR_DONE` if it has finished: its log already ends in `finished`.
- **The data root** is the one the host passes, as for offload (ADR 0074),
  or the data model's `inspect()` view when it passes none.
- **Core writes the checkpoint to the sink as one unit** (`begin` … `end`).
  `TA_ERR_NOMEM` means the sink refused it, and nothing changed.
- **A value with no durable form** answers `TA_ERR_UNSUPPORTED`. It cannot
  happen for a logged actor's inputs (ADR 0090 section 5), but it can
  for a value a script built.
- **Taking a checkpoint changes nothing in the actor.** It is not an input
  and writes nothing to the input sink.
- **Memory:** the fixed part and texts come from scratch. The configuration
  and history lists are written as they are walked, and values stream.
  Nothing is sized by the actor's data.

### 3. Starting a replay from a checkpoint

`ta_actor_prepare_params.log` may begin with a `checkpoint` record instead
of `spawned`. `prepare` then restores the checkpoint into a replaying
actor:
- its configuration and history, by state ID;
- its data root, generated-ID counter and clock;
- its pending sends, held as a replaying actor's are (ADR 0089 section 3);
- its active invocations, active in core without an invoker, so the
  services' later events still match them.

The restored actor is idle: no bootstrap runs. Replay then continues with
the records after the checkpoint, which are those whose `seq` is greater
than the checkpoint's. A record whose `seq` is not greater is skipped, so a
host may hand over a log that still contains records the checkpoint covers.
The same rule skips a checkpoint met again inside a full log replayed from
`spawned`: its records carry a `seq` the replay has already applied.

**A checkpoint is valid only for its revision.** Configuration and data
are written by the chart they came from:
- `prepare` with a checkpoint for another revision answers
  `TA_ERR_DEFINITION_MISMATCH`;
- for a state ID the chart no longer has, it answers `TA_ERR_NOT_FOUND`.

A host that wants to upgrade by reinterpreting history replays from the
`spawned` record instead, which needs the records before the checkpoint.

### 4. What a host keeps

The host chooses between two policies:
- **Keep everything.** Checkpoints make a load fast, and the full log
  keeps upgrade by reinterpretation available.
- **Compact.** Once a checkpoint is durable, drop the records it covers.
  Storage is then bounded by the actor's state plus the records since
  the checkpoint. An upgrade then starts from the checkpoint's state, and
  needs a migration of that state rather than a replay.

Core has no opinion. When to checkpoint (every N records, every T of
logical time, at eviction) is the host's, or its application's, policy.

### 5. The messages the host still owes

A host's mailbox is its own (ADR 0029), so a message it accepted but has
not delivered is known to the log only by its `queued` record, and matched
later by an `event` or `dropped` record with the same host number (ADR
0090 section 6). Compacting a log to its checkpoint would drop the
`queued` records of messages still owed, and the replay would never hand
them back.

So the host passes the messages it holds for the actor when it takes the
checkpoint, and core writes each as a `QUEUED` record at the end of the
checkpoint's unit. A replay that starts from the checkpoint hands them
back as `TA_REPLAY_HOST`, once, although their `seq` is the checkpoint's.
A replay that meets the checkpoint inside a full log skips them with the
rest of it: it already handed back the original `queued` records. Core
keeps nothing for this. The host holds the messages anyway, and core
holds no copy of what it did not deliver.

## Consequences

- **Loading is bounded by the state and the records since the last
  checkpoint**, not by the whole history, in time and in memory.
- **Storage can be bounded by compaction,** at the price of upgrading by
  migrating state instead of replaying from the start.
- **The durable layout of ADR 0090 now also describes an actor's whole
  state.** It is a portable counterpart of the session image. It does not
  replace the image: offload stays process-local and cheaper, because it
  may write handles.
- **Costs:**
  - a checkpoint's size is the actor's state: its configuration, its data
    as the data model encodes it, its pending sends and its invocations;
  - taking one is proportional to that size, and allocates nothing in core;
  - an actor that is never checkpointed pays nothing.

## Considered and rejected

- **Making session images durable.** An image carries interned
  identifiers, runtime references and continuation state. A checkpoint
  needs only an idle actor's state, written by ID, which is simpler and
  does not change the offload path.
- **Checkpoints that hold a paused macrostep.** A continuation's position
  is only meaningful within one compiled definition, and replay needs an
  idle actor between records anyway.
- **Core tracking the owed host numbers itself.** Core would still not hold
  the messages, so a checkpoint could name what is owed but not carry it.
  The host already holds them, so it passes them.

## Implementation notes

- **Where:**
  - `include/tinyactors.h`: the kinds `TA_RECORD_CHECKPOINT`, `_HISTORY`,
    `_SEND` and `_INVOKE` and their fields (in the input log's layout
    comment), `ta_owed_message`, `ta_actor_checkpoint_params` and
    `ta_actor_checkpoint()`, and the checkpoint cases of
    `ta_actor_prepare_params.log` and `ta_actor_replay()`.
  - `src/input_log.{c,h}`: the writer gained `ta_input_emit()` (a record
    written into an open unit), a second value per record, and trailing
    state lists asked for through a cursor (`target_at`), so a checkpoint
    walks its configuration and history lists without an array. The
    checkpoint writer (`ta_actor_checkpoint()`), the first-record reader
    (`ta_input_read_spawn()`, now for SPAWNED and CHECKPOINT), the restore
    (`ta_input_restore()`), and the replay's skip rule.
  - `src/invoke.{c,h}`: `ta_invoke_restore()`, an invocation made active as
    a replayed one.
  - `src/actor.{c,h}`: `ta_actor_prepare()` restores from a checkpoint; a
    replaying actor's `checkpoint_owed` count.
  - napi `bindings.rb`: the new function and types are skipped until napi
    ADR 0034 binds them.
  - test host: `src/instruments.c` (the kinds' names; a `compact` control
    on `input.recording` that drops the units a checkpoint covers),
    `ruby/tinyactors_test/scenario.rb` (`checkpoint`, `compact_inputs`),
    `ruby/scenarios/replay.rb`.
- **Where the code differs from the text:**
  - A checkpoint is a unit of records, not one record, and it carries the
    owed messages (sections 1 and 5, amended in place).
  - An invocation carries no values, and a send the router already
    refused is left out (section 1).
  - A finished actor is `TA_ERR_DONE`, and a checkpoint needs a published
    actor (section 2).
  - The restored actor's `seq` is the checkpoint's, which is how the skip
    rule of section 3 works: `ta_actor_replay()` skips every record at or
    below the last `seq` the actor applied, whatever its kind, and a
    CHECKPOINT record past that is `TA_ERR_INVALID` (prepare from it
    instead).
  - Owed messages, and parts, carry the checkpoint's `seq`; the restored
    actor counts the owed ones (`checkpoint_owed`) and hands exactly those
    back.
  - A send to the actor itself is marked, not written as a URI: its
    destination names the old session, and the restore points it at the
    new one, as a spawn's re-armed sends do (ADR 0074 section 8).
- **Tests:**
  - `tests/input_log_test.c` (fast):
    - a checkpoint alone, a checkpoint and its tail, a checkpoint and the
      whole log it covers, and a whole log with a checkpoint inside it,
      each read in 1- and 7-byte pieces, rebuilding the live actor's
      configuration, deep history, pending sends and binding, and writing
      nothing to the input sink;
    - owed messages: a compacted log hands back the owed one with its data;
      the checkpoint and the whole log hand it back once; a whole log with
      the checkpoint inside hands back the originals;
    - the timelines of ADR 0089 section 2 from a checkpoint taken mid-way
      and the records after it;
    - an invocation active at the checkpoint matches the service's later
      report, and a checkpoint alone retires it at publish;
    - refusals: busy, unnamed, unlogged, finished; a sink refusing at every
      byte (an out-of-memory sweep) and at `begin`; a root with no durable
      form; another revision; a checkpoint past what a replay applied;
    - taking a checkpoint allocates nothing (a counting allocator) and
      leaves the input log unchanged.
    None is exhaustive: the byte sweep is bounded by one checkpoint's
    size.
  - test host `ruby/scenarios/replay.rb`, group "input log": a log compacted
    to its checkpoint rebuilds an actor a week later, its data from the
    checkpoint; a checkpoint's busy and invalid refusals.
- **Costs:**
  - an actor that is never checkpointed: nothing. A replaying actor's held
    effects grow from 64 to 72 bytes (`checkpoint_owed`); only unpublished
    actors have them;
  - a record description on the stack, while one is written, grows by 136
    bytes to 752: `TA_RECORD_FIELDS_MAX` rose from 10 to 12 (56 bytes a
    field), plus `target_at`, `target_context` and `value2`;
  - a checkpoint's size: its CHECKPOINT record is 120 bytes of fixed part,
    8 more per listed state, its texts padded to 8, and the data root's
    stream; a HISTORY part 48 bytes plus 8 per recorded state; a SEND part
    96 bytes plus its texts and value streams; an INVOKE part 56 bytes
    plus its texts. Measured in the test: the `timers` actor waiting with
    two pending sends, 464 bytes; the `hist` actor with a recorded history
    and a pending send, 432 bytes mid-way and 584 at the end, against a
    720-byte log for the same history;
  - taking a checkpoint allocates nothing in core; restoring one reads
    each record once, allocating its fixed and text parts, which are freed
    before the next.
