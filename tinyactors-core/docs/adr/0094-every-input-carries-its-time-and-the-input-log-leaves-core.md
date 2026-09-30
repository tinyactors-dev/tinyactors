# ADR 0094: Every input carries its time, and the input log leaves core

**Status:** Accepted and implemented (2026-09-30); see the implementation notes. Supersedes:
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

### Sections 1 and 3 (roadmap step 3)

- **Where:** `include/tinyactors.h` (`ta_actor_deliver_params.time`,
  `ta_invoke_report_params.time`, `ta_actor_publish_params.at`,
  `ta_history_entry`, `ta_invoke_state`, `ta_actor_state`,
  `ta_actor_export_params`, `ta_actor_export_state()`,
  `ta_actor_prepare_params.state`), `src/actor.{c,h}` (the time check and
  the clock move, publish's `at`, export and restore), `src/invoke.c`
  (reports), `src/input_log.c` (the reducer passes each record's time).
  Commits: `9861f8a` (section 1) and the one that adds these notes
  (section 3).
- **One check, one move.** `ta_actor_input_time_valid()` is the rule of
  section 1's table; `ta_actor_move_clock()` drops held sends due by the
  time and sets the clock, for a replaying actor only. A delivery checks
  the time with its other `TA_ERR_INVALID` checks and moves the clock only
  after the last step that can fail (holding the event block), so a
  refused input changes nothing. An ERROR report moves it once the report
  is queued. Publish counts the held sends that `at` will leave before it
  reserves timer room, and moves the clock after the route is claimed, so
  a refused publish changes nothing either.
- **A prepared actor that is not replaying** (a replacement) takes inputs
  at the system's time, like a published one.
- **The reducer keeps a private clock move.** Records that are not inputs
  (`fired`, `evicted`, an `abandoned` macrostep) still move a replaying
  actor's clock: `ta_actor_advance_clock()`, private to core, which step 5
  removes with the reducer.
- **Callers:** the kernel's scheduler passes `ta_system_time()` read at each
  delivery and each queued report, one field read, so no per-turn copy was
  needed. napi: `publish(prepared, { at })` passes `at` to core, and
  `PreparedActor.setTime()` is removed; a message sent to a replaying actor
  is taken at the system's time, so `test/prepare.js` steps the manual clock
  to each input's time instead. The test host: `deliver`/`post` take
  `time:` (default the scenario's clock), `report_invoke` passes the
  clock, `publish` takes `at:`; `set_actor_time` is removed. Every
  `ta_actor_deliver()` and `ta_actor_report_invoke()` call in the C tests
  of core, the codec, the kernel and the test host passes a time.
- **Section 3 differs from the text in three ways:**
  - **State IDs are the definition's numbers, not text.** A state restores
    only under the revision it was exported from (the revision check), and
    a revision fixes the numbering (document order, ADR 0060), so a number
    is exact. The kernel may write text in its checkpoint encoding if it
    wants a readable log.
  - **`ta_actor_export_state()` takes a params struct** with three caller
    arrays, following the header's list convention (a zero capacity is a
    size query that answers `TA_ERR_NOMEM` with the totals): the active
    states and then the late-initialized ones, the recorded history as
    `(history, state)` pairs, and the active invocations with their
    `<invoke>`. Pending sends are borrowed from the actor, as inspection
    lends them; a submitted one is exported but not restored.
  - **The state also carries the late-initialized states and the step
    counters,** which ADR 0091's checkpoint held and a restore needs.
  napi's generator cannot bind a function with three caller arrays, so
  napi lists it as plumbing: the kernel calls it natively (step 4).
- **Restoring** reuses what the checkpoint restore did: the configuration
  and history bits set, the actor marked bootstrapped and idle, the
  counters taken back, each unsubmitted send held (runtime ID 0 as the
  destination meaning the actor itself, as spawn's pending sends do), each
  invocation made active with `ta_invoke_restore()`. The data is bound to
  `datamodel_binding`, or to the state's root. Everything the state names
  is checked before the actor is created.
- **Export allocates nothing** in core (a counting allocator shows it); the
  root is the data model's, from its `inspect`.
- **Tests:** `tests/replay_test.c`: the four timelines of ADR 0089 with a
  time on each input and publish's `at`; the live run replayed; a time
  refused for a live actor, backwards and in the future; publish's `at`
  refused for a live actor and before the clock. Export: a size query;
  export then prepare from the state reproduces the configuration, the
  history (a `back` through it lands in `a3`) and the pending sends, and
  runs no bootstrap; an invocation active in the state stays active with
  no invoker called and is retired as unavailable at publish; result codes
  (busy before bootstrap and while holding an event, done, unpublished,
  another revision, an unknown state, `at` with a state). The byte
  checkpoint (ADR 0091) keeps its own walk until step 5 removes it.

### Section 2: the log leaves core (2026-09-30)

Commit `e4b6974`. The host kernel had its own log (kernel ADR 0002, step
4a), and the test host's persistence scenarios ran on it (kernel ADR 0001,
step 7), so nothing depended on core's log any more.

- **Removed from core:**
  - `ta_system_set_input_sink`, `ta_system_log_family`,
    `ta_system_input_owner` (with `ta_input_owner`) and
    `ta_system_write_host_record` (with `ta_host_record_params` and
    `ta_host_record`);
  - `ta_actor_replay` (with `ta_replay_step` and `ta_actor_replay_params`),
    and the byte `ta_actor_checkpoint` (with `ta_owed_message`);
  - `ta_actor_prepare_params.log` and `ta_actor_deliver_params.host_seq`;
  - the reducer's private `ta_actor_advance_clock`;
  - the sparse state's `log_seq` and its three log flags, the definition's
    `input_logged`, and the system's staging buffer and logged families;
  - `src/input_log.{c,h}` (1,954 lines) and `tests/input_log_test.c`
    (1,881 lines).
- **Kept:** time on every input, replaying actors, prepare and publish, and
  export and restore of an idle actor's state (sections 1 and 3). The
  timelines of ADR 0089 section 2 stay covered in `tests/replay_test.c`,
  with a time on each input.
- **Callers:**
  - the test host drops its `input.recording` and `input.log` instruments,
    its core-log scenario helpers (`recording_inputs`, `input_kinds`,
    `compact_inputs`, `replay_log`, and `evict` and `checkpoint` through
    core), and the "input log" scenario group, whose cases run on the
    kernel in `ruby/scenarios/hosted.rb`;
  - napi's bindings policy and generated bindings drop the removed
    functions;
  - the kernel never called them.
- **Where this differs from the text:** the jq writer's "largest write"
  check went with the `input.recording` instrument. The jq data model's
  unit test (`tests/json_datamodel_test.c`) still shows it writes in
  pieces.
- **Costs:**

  | | before | after |
  |---|---|---|
  | `ta_system` | 1,808 bytes | 1,448 bytes |
  | `ta_actor_sparse_state` | 184 bytes | 176 bytes |
  | `ta_actor`, `ta_definition` | 232, 904 bytes | unchanged |

  4,337 lines leave core, 1,881 of them tests.
- **Tests:** `mise run test --all` passed: core, the kernel, the test host
  (437 of 437 scenarios, five fewer after the group moved) and napi, with
  IRP 176/176 on Node and Bun.
