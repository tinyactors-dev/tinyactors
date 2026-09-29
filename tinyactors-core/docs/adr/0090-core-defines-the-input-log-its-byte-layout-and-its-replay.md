# ADR 0090: Core defines the input log, its byte layout and its replay

**Status:** Proposed (2026-09-29). Would amend:
- ADR 0037: core defines the format of a log and how it replays, and still stores nothing and requires nothing;
- ADR 0074: the data model's value codec streams through a byte sink and a byte source, and says whether its bytes are durable;
- ADR 0089: a host replays by handing core records, instead of calling the input functions itself.

ADR 0091 (checkpoints) and napi ADR 0034 build on it.
**Date:** 2026-09-29

## Context

Core ADR 0089 made replay possible. Napi ADRs 0032 and 0033 built the
log on top of it in the host: about 900 lines of C in `src/inputs.c` and
`src/napi_inputs.c`, plus a replay driver in `lib/system.js`. Most of that
code is about core, not about JavaScript:
- **What counts as an input.** Core accepts a delivery, applies an
  operator's action, dispatches a timer, finishes a session. Each of these
  is core's decision, and napi instruments its own call sites to notice
  them.
- **The replay reducer.** Moving the clock, delivering with the origin
  parsed back, matching a report to its invocation, performing an
  operation, preparing from the `spawned` record: these are sequences of
  core calls.

Every other embedding would repeat them: the test host already drives
replay by hand (`tinyactors-test/ruby/scenarios/replay.rb`), and so would
Python, WASM and the host kernel. A log written by one embedding could not
be replayed by another.

The records are also JavaScript objects holding live values, which the
application encodes. That costs memory in two places:
- a turn's records are materialised as objects before they are written;
- a value's encoding is built whole, both by the application and by the
  data model's codec. ADR 0074's `value_encode` writes one value into an
  arena in full.

What matters most is **memory bounds**. Writing a record must not
allocate in proportion to its values. Replaying a log must not hold more
than one record. A large value must pass through in pieces.

The trace encoder already shows the shape of a byte format core can own
(napi ADR 0018): a fixed layout, written straight into memory the host
provides. This ADR applies that to the log, with a layout that can be read
in place in the style of Cap'n Proto, without depending on it.

## Decision

### 1. Byte sinks and byte sources

```c
typedef struct ta_byte_sink_vtable {
    /* Starts a unit (a record, or a value); returns TA_OP_NOMEM to refuse it. */
    ta_op_status (*begin)(void *self);
    ta_op_status (*write)(void *self, ta_bytes bytes);
    /* keep=false discards everything written since begin. */
    void (*end)(void *self, bool keep);
} ta_byte_sink_vtable TA_VTABLE;
typedef struct { void *self; const ta_byte_sink_vtable *v; } ta_byte_sink TA_ADAPTER;

typedef struct { uint8_t *buffer; uint32_t capacity; TA_OUT uint32_t *got; } ta_byte_read_params;
typedef struct ta_byte_source_vtable {
    /* Fills up to capacity; *got == 0 means the source has ended. */
    ta_op_status (*read)(void *self, TA_IN const ta_byte_read_params *params);
} ta_byte_source_vtable TA_VTABLE;
typedef struct { void *self; const ta_byte_source_vtable *v; } ta_byte_source TA_ADAPTER;
```

A sink receives bytes in pieces, and decides where they go: a file, a
socket, a buffer per batch. `begin` and `end` bracket a unit, so a unit
that fails halfway is discarded whole (ADR 0070). A source is read in
pieces of the reader's size. Neither side needs a unit's total size in
advance.

### 2. The data model's value codec streams, and says whether it lasts

ADR 0074's `value_encode` and `value_decode` are replaced in place:

```c
typedef struct { void *shared; TA_IN const ta_value *value; ta_byte_sink sink; bool durable;
                 TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_value_write_params;
typedef struct { void *shared; ta_byte_source source; bool durable;
                 TA_OUT TA_RELEASE(ta_value_release) ta_value *out;
                 TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_value_read_params;
```

- **`value_write` writes a value to the sink in pieces, and `value_read`
  reads it back from the source in pieces.** Neither must buffer the whole
  encoding. A data model whose values are trees writes them as it walks
  them, without recursion (ADR 0060).
- **`durable` asks for bytes that outlive the process:**
  - `true`: no handles and no process-local identifiers, so the bytes can
    be read by another process running the same data model. A value with
    no durable form answers `TA_OP_ERROR`;
  - `false`: what ADR 0074's offload asks for. The bytes stay in this
    process, and a data model may write a handle.
- **Offload migrates onto the new pair** with an arena-backed sink and a
  memory source, so there is still one codec per data model.
- **The null data model** has no values to write.

### 3. The record layout

A log is a sequence of records, one actor's inputs in order. A record is
little-endian and 8-byte aligned, in three parts:

```
fixed part   (fixed_bytes, a multiple of 8, read in place)
  u16 version        layout version, 1
  u16 kind           record kind (section 4)
  u32 fixed_bytes    size of the fixed part, header included
  u64 seq            the actor's record number, from 1
  u64 time           logical time, in milliseconds
  u32 text_bytes     size of the text part
  u16 value_count    number of value streams that follow the text part
  u16 flags
  ... kind fields    fixed-width integers, and text references (u32 offset, u32 length)
text part    (text_bytes, padded to 8)
  UTF-8 text the kind fields refer to: event names, send IDs, invoke IDs,
  origins and their types, state IDs, family and name
value streams (value_count of them, in the order the kind lists)
  each is chunks: u32 length, the bytes, padding to 8; a chunk of length 0 ends it
```

- **The fixed part and the texts can be read in place**, and a reader
  learns the text part's size before reading it. A value is read by
  handing its stream to the data model as a source. A reader never
  needs a record's total size, and a writer never needs a value's size.
- **The layout can grow without breaking readers.** A newer writer may
  append kind fields: `fixed_bytes` tells an older reader how much to
  skip. A record kind that a reader does not know is an error, never
  skipped silently. `version` changes only for an incompatible layout.
- **Every identifier is text.** Nothing in a record depends on the process
  that wrote it: no atoms, no runtime IDs, no handles.

### 4. Record kinds

| Kind | Written when | Fields | Values |
|---|---|---|---|
| `spawned` | a logged actor is spawned or prepared | family, name, revision | binding |
| `event` | `ta_actor_deliver()` accepts an event | name, type, send ID, origin, origin type, invoke ID; `host_seq` for mail the host logged as queued | data |
| `jump` | a delivery carries an operator transition | source state ID, target state IDs | |
| `report` | `ta_actor_report_invoke()` accepts a report | invoke ID, kind, reason, ends | data |
| `fired` | a delayed send of a logged actor is dispatched | send ID | |
| `withdrawn` | `ta_actor_withdraw_send()` removes a send | send ID | |
| `abandoned` | `ta_actor_abandon()` | | |
| `released` | a resume lifts the microstep limit | | |
| `finished` | the session reaches a top-level final state | final state ID | |
| `destroyed` | a logged actor is destroyed, and its address is not taken over | | |
| `queued`, `dropped`, `evicted` | written by the host (section 6) | host_seq; for `queued` the event's fields | `queued`: data |

State IDs are the chart's `id` text, so a `jump` still means something
under a new revision. Where no state has that ID, the replay answers
`TA_ERR_NOT_FOUND`, and the host decides.

### 5. The input sink

```c
TA_API void ta_system_set_input_sink(ta_system *system, ta_byte_sink sink);
TA_API ta_result ta_system_log_family(ta_system *system, ta_str family, bool logged);
```

Core writes a record to the sink for every input accepted by a named
actor of a logged family, at the moment it accepts it:
- **Writing is part of accepting.** Core calls `begin`, writes the fixed
  part and the texts from a fixed-size scratch buffer, streams each value
  through the data model with `durable = true`, then calls `end(keep)`.
- **If any step fails, the input is refused and nothing changes:**
  - a sink that answers `TA_OP_NOMEM` gives `TA_ERR_NOMEM`, which the host
    retries;
  - a value with no durable form gives `TA_ERR_UNSUPPORTED`.

  So a logged actor never takes an input that its log does not hold.
- **Records core writes outside a host call** (`fired` during
  `ta_system_dispatch_due()`, and `finished`) follow the same rule. A
  timer whose record cannot be written stays due, as one the router
  answered RETRY for.
- **Core allocates nothing to write a record.** Its memory is the scratch
  buffer, sized by the largest fixed part, plus whatever the data model
  uses to walk a value.
- **Batching, durability and the write-ahead hold stay with the host.** The
  sink decides where bytes go and when they are durable. The host holds
  its effects until its sink has made a turn's records durable (napi ADR
  0032 section 6).
- **Wake times are not records.** A host asks for an actor's first
  deadline, the head of its sorted list, O(1) (ADR 0054), when it needs a
  wake time.

### 6. Host records

A host's mailbox is its own (ADR 0029), so the host writes the records
that make it durable, through core, into the same log:

```c
TA_API ta_result ta_system_write_host_record(ta_system *system, TA_IN const ta_host_record_params *params);
```

Its parameters are the address, the kind (`queued`, `dropped` or
`evicted`), `host_seq`, and for `queued` the event's name, send ID,
origin and data. Core writes the record in the layout of section 3, with
the actor's next `seq`. When a host message is delivered, the host passes
its `host_seq` in `ta_actor_deliver_params`, and the `event` record
carries it.

### 7. The replay reducer

A host replays a log by handing core one record at a time:

```c
typedef enum {
    TA_REPLAY_NEXT = 0,      /* applied; read the next record */
    TA_REPLAY_RUN,           /* applied; resume the actor until idle, then read the next */
    TA_REPLAY_HOST,          /* a host record: see out.host */
    TA_REPLAY_FINISHED,      /* the actor reached a final state; stop */
    TA_REPLAY_ENDED          /* a `destroyed` record: this life ended; a new one starts at the next record */
} ta_replay_step TA_SYMBOLS("TA_REPLAY_");

typedef struct {
    ta_actor_id actor;
    ta_byte_source source;           /* positioned at a record */
    TA_OUT ta_replay_step *step;
    TA_OUT ta_host_record *host;     /* for TA_REPLAY_HOST: kind, host_seq, and a queued message's event */
} ta_actor_replay_params;
TA_API ta_result ta_actor_replay(ta_system *system, TA_IN const ta_actor_replay_params *params);
```

- **Preparing from a log:** `ta_actor_prepare_params` gains
  `TA_OPTIONAL ta_byte_source *log`. With it, `prepare` reads the
  `spawned` record, starts the clock at its time, and binds its binding.
  `binding` and `at` must then be absent.
- **Each call reads exactly one record.** It moves the clock to the
  record's time, then applies the record:
  - an `event` is delivered: its name is interned, and its origin is
    parsed as a target;
  - a `report` is matched to the replaying actor's invocation by its ID
    text;
  - a `jump` resolves its state IDs;
  - `withdrawn`, `abandoned` and `released` are performed;
  - `fired` and `evicted` only move the clock;
  - `destroyed` answers `TA_REPLAY_ENDED`. The address was freed, so the host
    destroys the replaying actor and prepares again from the next record,
    which is a new `spawned`. If there is no next record, the load is a fresh
    actor. A log keeps every life of an address, and a replay still reads it
    once, in order, as a stream (napi `0f8bd34`);
  - a host record is handed back to the host.
- **`TA_REPLAY_RUN` asks the host to resume the actor until it is idle**
  before the next record, within the host's own budgets.
- **Memory:** a replay holds one record's fixed part and texts at a time,
  and streams each value through the data model. A log of any length
  replays in bounded memory.
- **After the last record,** `ta_actor_publish()` publishes at the actor's
  clock, which is the last record's time (ADR 0089 section 6).
- **Owed mail stays the host's.** A `queued` record whose `host_seq` has
  no later `event` or `dropped` is mail still owed. The host keeps the
  unmatched ones, and delivers them after publishing. Keeping them is
  bounded by the mail that was owed, not by the length of the log.
- **A replaying actor writes nothing to the input sink**, as napi ADR 0033
  already requires.

## Consequences

- **One definition of what an actor's log is and how it replays,** shared
  by every embedding. The application stores opaque bytes per address.
  Because the layout carries no process-local identifiers, a log written
  by napi can be replayed by the test host, or by another process.
- **Memory is bounded on both sides.** Writing needs a fixed scratch buffer
  plus the data model's own traversal. Replaying needs one record, plus the
  owed mail. Values of any size pass through in chunks.
- **ADR 0074's codec changes in place:** napi's and the test host's data
  models implement `value_write` and `value_read`, and offload uses them.
  napi needs a durable encoding for JavaScript values; napi ADR 0034
  chooses it.
- **A value that cannot be encoded durably refuses the input** to a logged
  actor, where napi ADR 0032 took it and left the problem to the
  application's encoder.
- **Costs:**
  - an unlogged actor pays nothing: the family check is one bit on its
    definition;
  - a logged input costs one record written to the sink: the fixed part
    and texts from scratch, plus the value's own encoding;
  - no allocation in core per record;
  - the offload format changes with the codec. It is process-local, so no
    stored offload survives the change anyway.

## Considered and rejected

- **Records as structs, as the trace sink hands them over (ADR 0067).** Each
  host would still encode them, and logs would differ between hosts.
- **Cap'n Proto or protobuf themselves.** A dependency, and a general
  schema compiler, for a dozen fixed layouts. Core writes these directly,
  as napi writes OTLP.
- **Knowing a record's size first, as the trace encoder does.** It needs a
  value encoded twice, or held whole. Chunked value streams need neither.
- **Keeping ADR 0074's arena codec, and adding a second, streaming one.**
  AGENTS.md: one way to do each thing. Offload runs well on the streaming
  pair with an arena-backed sink.

## Implementation notes

None yet.
