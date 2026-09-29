# ADR 0091: A checkpoint is an actor's state at a record of its log

**Status:** Proposed (2026-09-29). Builds on ADR 0090 (the input log, its byte layout and its replay). Would refine ADR 0089: a replay can start from a checkpoint instead of from `spawned`.
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

### 1. A checkpoint record

A `checkpoint` is a record kind in ADR 0090's layout. Its `seq` is the
number of the last log record it includes, and its `time` is the actor's
clock.

| Part | Contents |
|---|---|
| Fixed fields | the definition's revision; the generated-ID counter (send and invoke IDs, ADR 0089 section 5); the session's step counters (ADR 0067); flags (finished) |
| Texts | family and name; the active atomic states' IDs; each history state's recorded IDs; for each pending delayed send, its send ID, event name, I/O type, target, due time and whether it was submitted; for each active invocation, its ID text and its `<invoke>`'s position in the chart |
| Values | the data model's root; each pending send's data and target value; each active invocation's values as the invoke request carried them |

- **State is written by ID,** the chart's text, never by number.
- **Every value goes through `value_write` with `durable = true`,** in
  pieces.
- **A checkpoint holds only what an idle actor has.** There is no internal
  queue, no continuation and no external event in it, because only an idle
  actor can be checkpointed (section 2).

### 2. Taking a checkpoint

```c
typedef struct { ta_actor_id actor; ta_byte_sink sink; } ta_actor_checkpoint_params;
TA_API ta_result ta_actor_checkpoint(ta_system *system, TA_IN const ta_actor_checkpoint_params *params);
```

- **Only a named, idle actor of a logged family can be checkpointed.**
  Otherwise the answer is:
  - `TA_ERR_BUSY` while it is inside a macrostep or holds an event;
  - `TA_ERR_INVALID` if it is unnamed or not logged.
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

Replay then continues with the records after the checkpoint, which are
those whose `seq` is greater than the checkpoint's. A record whose `seq`
is not greater is skipped, so a host may hand over a log that still
contains records the checkpoint covers.

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

## Implementation notes

None yet.
