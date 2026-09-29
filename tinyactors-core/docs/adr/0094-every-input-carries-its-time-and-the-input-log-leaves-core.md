# ADR 0094: Every input carries its time, and the input log leaves core

**Status:** Proposed (2026-09-30). Would supersede:
- ADR 0090 sections 3 to 7: the record layout, the input sink, host records and the replay reducer move to the host kernel (kernel ADR 0002);
- ADR 0091's byte form: a checkpoint becomes structured state that core exports and restores;
- ADR 0089 section 2's `ta_actor_set_time()`: time becomes an argument of every input.

It keeps ADR 0090 sections 1 and 2 (byte sinks and sources, and the data model's streaming codec), which offload and the kernel use. It builds on kernel ADR 0001, which creates the host kernel.
**Date:** 2026-09-30

## Context

Replay-based persistence has been built in three rounds: core ADR 0089
(replaying actors), core ADR 0090 (core defines and writes the log) and
core ADR 0091 (checkpoints in the same bytes). Reviewing it from the
point of view of a second binding, and of the system as a whole, showed
two flaws in its shape.

**Core stands in the byte path for no reason.** When a logged input is
accepted, the bytes flow like this:

```
language value (host) → core accepts → core's record writer
  → the data model's value_write (host code) → core's framing → the input sink (host code)
```

Core sits between two pieces of host code. The host already holds the
value at the input edge, and core adds a round trip and a layer of
framing, then hands the bytes back. Replay runs the same hop in reverse.
The log went into core to avoid each embedding reimplementing it
(ADR 0090's context). A shared host kernel (kernel ADR 0001) answers that
without core in the byte path.

**The log's vocabulary follows who owns what, not the life of a message.**
A message is logged twice. The host writes `queued` through a side door
(host records), because the mailbox is the host's (ADR 0029). Core writes
`event` with the content again, because it does not know the host logged
it. Nothing gives a message an identity. The shape it should have is: a
message is recorded once when it is accepted, with an identity, and
afterwards only `delivered(id, time)` or `dropped(id)`. The host owns the
queue, so the host, meaning the kernel, is where acceptance and identity
belong.

**Designing the input API from scratch** gives the same answer. An actor's
life is a sequence of inputs, each at a logical time. If every input call
carries its time, then:
- a live call passes the host's clock, and a replayed call passes the
  logged time;
- replay is the same call made again with the logged arguments;
- no separate reducer, no `set_time`, and no replay-only mode on the clock
  are needed.

The `at` field ADR 0089 rejected as replay leaking into the API does not
leak when every input has it, live or replayed.

## Decision

### 1. Every input carries its time

The calls that start or feed a macrostep take a `ta_time time`: the
logical time of the input.
- `ta_actor_deliver_params.time`;
- `ta_invoke_report_params.time`;
- `ta_actor_prepare_params.at`, the bootstrap's time, as today;
- `ta_actor_publish_params.at`, a replaying actor's last clock move (ADR
  0089 section 2), which replaces `ta_actor_set_time()` before publishing.

The rules:

| Actor | `time` must be | Effect |
|---|---|---|
| published | the system's time (`ta_system_time()`) | as today. The field is checked, so a host passes the same clock it logs |
| replaying | no earlier than its previous input's time, no later than the system's | the actor's clock moves to `time`, and held delayed sends due by then are dropped (ADR 0089 section 2) |

`ta_actor_set_time()` is removed. The macrostep's time (`ta_dm_host_time()`,
delayed-send due times, trace records' `now`) is the input's time, as
before.

Operator actions that do not start a macrostep (`ta_actor_withdraw_send()`,
`ta_actor_abandon()`) take no time. They change the actor without reading
the clock.

### 2. The input log leaves core

Removed from core, with their tests moved to the kernel (kernel ADR 0002):
- `ta_system_set_input_sink()`, `ta_system_log_family()`,
  `ta_system_input_owner()`;
- `ta_system_write_host_record()` and host records;
- `ta_actor_replay()` and `ta_replay_step`;
- the record layout, its writer and its reader (`src/input_log.c`);
- `ta_actor_prepare_params.log`, and the `host_seq` of
  `ta_actor_deliver_params`;
- the byte form of `ta_actor_checkpoint()` (section 3).

Core keeps what makes inputs replayable. None of it is about bytes:
- time on every input (section 1);
- replaying actors that cross no border, prepare and publish (ADR 0089);
- send and invoke IDs numbered per session (ADR 0089 section 5);
- inputs accepted only between macrosteps (ADR 0089 section 4);
- `ignored` on resume, and `ta_dm_host_time()`;
- byte sinks and sources, and the streaming value codec (ADR 0090 sections 1
  and 2). Offload uses them (ADR 0074), and the kernel calls the data
  model's codec through the same interface.

### 3. A checkpoint is structured state

```c
/* An idle actor's state, borrowed while the actor is not changed. */
typedef struct {
    ta_definition_identity definition;      /* its revision */
    uint64_t generated_ids;                 /* the session's generated-ID counter */
    ta_time clock;
    TA_COUNTED_BY(state_count) const ta_str *states;           /* active atomic states, by ID */
    uint32_t state_count;
    TA_COUNTED_BY(history_count) const ta_history_view *history;
    uint32_t history_count;
    TA_COUNTED_BY(send_count) const ta_delayed_send_info *sends;
    uint32_t send_count;
    TA_COUNTED_BY(invoke_count) const ta_invoke_view *invokes;
    uint32_t invoke_count;
    ta_value root;                          /* the data model's root */
} ta_actor_state;

TA_API ta_result ta_actor_export_state(ta_system *system, ta_actor_id actor, TA_OUT ta_actor_state *out);
```

- **`ta_actor_export_state()`** fills the view for an idle, published
  actor. The answers are `TA_ERR_BUSY` inside a macrostep, and
  `TA_ERR_DONE` for a finished actor.
- **`ta_actor_prepare_params.state`** restores one into a replaying actor.
  States and history are resolved by ID, and a revision mismatch is
  `TA_ERR_DEFINITION_MISMATCH` (ADR 0091's rules).
- **The kernel encodes and decodes it,** along with anything of its own,
  such as the messages its queue still owes.

Core writes no checkpoint bytes and allocates nothing to export one. The
view borrows the actor's storage, as inspection does.

## Consequences

- **Core shrinks.** The input log's roughly 1,800 lines leave core, and so
  do its public functions and types. What remains is semantic.
- **The byte path is host → storage.** The kernel records an input at its
  own call site, with the value it already holds, and encodes it with the
  data model's codec.
- **Replay is the live API called again.** The kernel's replay driver
  decodes a record and makes the call the live system made, with the logged
  time. Core has no replay-specific entry point beyond the replaying actor.
- **Every host passes a time on every input.** For a live actor it is
  `ta_system_time()`, which the kernel reads once per turn.
- **Callers change:** the kernel (new), napi (napi ADR 0037), and the test
  host (kernel ADR 0001) migrate. Core's tests of the log move to the
  kernel, with the four timelines of ADR 0089 among them.
- **Costs:**
  - per input: one time argument, checked against the system's clock for a
    published actor;
  - per actor: unchanged;
  - core loses the input log's scratch buffer (256 bytes per system) and
    the 8-byte log sequence in a logged actor's sparse state.

## Considered and rejected

- **Identity at acceptance as a core concept** (the review's "option A").
  It would give core a call per accepted message and state per actor, for a
  queue core does not own. With the log in the kernel, identity lives where
  the queue lives.
- **Keeping the log in core, and fixing its vocabulary there.** That keeps
  core in the byte path, and host records as a side door, because the
  mailbox would still be someone else's.
- **Keeping `ta_actor_set_time()` beside a time on every input.** Two ways
  to set a clock, one of them only for replay.

## Implementation notes

None yet.
