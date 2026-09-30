# ADR 0090: Core defines the input log, its byte layout and its replay

**Status:** Accepted and implemented (2026-09-29); see the implementation notes. ADR 0091 adds checkpoints: record kinds CHECKPOINT, CHECKPOINT_HISTORY, CHECKPOINT_SEND (with two value streams) and CHECKPOINT_INVOKE, and a replay that skips records at or below the last `seq` it applied. Amends: Sections 3 to 7 are superseded by ADR 0094 and removed from core (2026-09-30): the log, its layout and its replay now belong to the host kernel (kernel ADR 0002).
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

### Sections 1 and 2

- **Where:**
  - `include/tinyactors.h`: `ta_byte_sink` (begin, write, end), `ta_byte_source` (read), `ta_byte_read_params`, and `value_write`/`value_read` with their params in place of `value_encode`/`value_decode`.
  - `src/session_image.c`: offload writes each value through an arena-backed sink (`ta_arena_sink`), which gathers the pieces into one run of the offload's arena and doubles it as the value grows, and reads it through a memory source.
  - `src/null_datamodel.c`: it has no values, so both operations refuse with a fault.
  - napi: `src/napi_datamodel.{c,h}`, `lib/durable.js`, `src/napi_loads.c` (`setDurableCodec`, and the internal test methods `durableBytes`/`durableValue`), `lib/system.js`.
  - test host: `src/json_datamodel.c`, `src/ecmascript_datamodel.c`.
- **Who brackets a unit.** `value_write` only calls `write`. The code that owns the unit (the offload for one value here, a record in section 5) calls `begin` and `end(keep)` around it. The header says so.
- **A source holds exactly one value.** `value_read` reads until the source reports that it ended (`*got == 0`). The chunk framing of section 3 is what gives a record's value stream its end; offload hands a memory source over the value's bytes.
- **`ta_byte_read_params.buffer`** is annotated `TA_BUFFER(capacity, got)` with `got` `TA_ALWAYS`: the API extractor refuses a writable list.
- **napi's durable form** is ADR 0031's text:
  - `lib/durable.js`'s `durableChunks(value)` walks the value with an explicit stack of frames, one per open container, and yields pieces of about 16,384 characters. A long string is escaped in slices of that size, which keeps a lone surrogate at a slice boundary valid: JSON escapes it, and parsing rejoins the pair.
  - A Map's entries, a Set's items and a typed array's elements are walked lazily, without copying them into an array first.
  - The writer uses `,` and `:` without spaces. The built-in reader of ADR 0031 accepts either.
  - Natively, each piece crosses as a string and is converted to UTF-8 in a buffer the size of that piece, then written.
  - The reader, `durableReader(classes)`, is an incremental parser with an explicit stack. It keeps only the text it has not consumed. While a string is open, it resumes scanning where the last piece stopped, so a long string is scanned once. A number that ends a piece waits for the next one, because `1.` may become `1.5`.
  - Natively, the source is read in pieces of 16,384 bytes, and a UTF-8 character split across pieces is carried to the next piece.
  - Refusals are the built-in printer's (a function, a symbol, a BigInt, NaN, -0, undefined, a cycle, a class instance without `[serialize]()`, holes, accessors), with the JavaScript error as the fault's data. A class the reading system hasn't registered is refused on the way back.
- **napi's offload form** (`durable = false`) is unchanged: a tag, then v8's serialization or a handle. It is written through the sink in pieces, and gathered whole before it is read, because `v8.deserialize` and a handle both need all of it.
- **Test host:**
  - the JSON data model writes its JSON text for both forms, and reads it with jq's incremental parser;
  - the ECMAScript data model writes QuickJS's structured serialization for offload, and JSON text for `durable = true`. A value JSON cannot say has no durable form.

  jq's dumper, and QuickJS in both directions, build or read the whole text. The test host accepts that; its data models are not where memory bounds matter.
- **Memory:** in core, the arena sink is the offload's own memory, and nothing else is allocated. In napi, a durable write holds one piece (about 16,384 characters, and its UTF-8 copy). A durable read holds one 16 KB piece, plus the parser's unconsumed text. While a long string is being read, that is its escaped text until the closing quote. The value itself is, of course, in memory once it is built.
- **Tests:**
  - core `tests/offload_test.c`:
    - a blob data model that writes in 1-, 7- and 4096-byte pieces and reads in 1-, 3-, 7- and 4096-byte pieces, round-tripping a 10,000-byte root through offload and load;
    - a refused `value_write`, which gives `TA_ERR_UNSUPPORTED`, leaves the actor resident, and releases the fault's data;
    - an out-of-memory sweep over offload while a 3,000-byte value is written one byte at a time.
  - napi `test/durable.js` (Node and Bun):
    - every kind of value the built-in data model prints, read back from pieces of 1, 2, 3, 7, 4096 and whole;
    - a 10 MB string, which crosses in pieces no larger than 64 KB;
    - 50,000 levels of nesting;
    - the refusals, an unregistered class, and truncated or malformed text;
    - the offload tag.
  - test host `tests/json_datamodel_test.c` and `tests/ecmascript_datamodel_test.c`: both forms, read from 1- and 7-byte pieces and whole, and bytes that are no value refused.
- **Costs:**
  - the offload format is unchanged byte for byte;
  - an offload now copies each value's bytes from the sink's run into the offload buffer, as it copied the arena bytes before;
  - no per-actor or per-event cost.

### Sections 3 to 7

- **Where:**
  - `include/tinyactors.h`: the layout and the kinds' fields (a comment of the "input log" section), `TA_RECORD_VERSION`, `ta_record_kind`, `ta_system_set_input_sink()`, `ta_system_log_family()`, `ta_host_record_params` and `ta_system_write_host_record()`, `ta_replay_step`, `ta_host_record`, `ta_actor_replay_params` and `ta_actor_replay()`; `ta_actor_prepare_params.log` and `ta_actor_deliver_params.host_seq`.
  - `src/input_log.{c,h}` (new): which actors are logged, the staged writer (`ta_input_open()`, `ta_input_close()`), the record builders, the framing sink around `value_write`, the reader, the value-stream source around `value_read`, and the reducer.
  - Where records are written: `src/actor.c` (SPAWNED around the address claim in `ta_actor_spawn()`, EVENT and JUMP in `ta_actor_deliver()`, WITHDRAWN, ABANDONED, DESTROYED; `ta_actor_prepare()` reads a log), `src/invoke.c` (REPORT), `src/interpreter.c` (RELEASED and FINISHED in `ta_interpreter_resume()`), `src/system.c` (FIRED in `ta_system_dispatch_due()`; the logged bit at registration).
  - `src/types.{c,h}`: `ta_text_writer.skip`, so a rendered text (an event name, a send ID, an origin URI) is written through a 128-byte window without a buffer of its own.
  - napi: `bindings.rb` skips the input-log functions and types until napi ADR 0034 binds them by hand, and its generator (`mise/lib/napi/{plan,emit}.rb`) learned an `:absent` field: an optional pointer to a skipped type, left null, which is what `ta_actor_prepare_params.log` is to JavaScript today. napi installs no input sink yet, so its own recording (napi ADR 0032, 0033) is untouched.
  - test host: `src/instruments.c` (`input.recording`, a sink that keeps the bytes and journals each record's kind; `input.log`, a source over them), `src/adapters.c`, `ruby/tinyactors_test/scenario.rb` (`recording_inputs`, `input_kinds`, `evict`, `replay_log`), `ruby/scenarios/replay.rb` (group "input log").
- **Layout details the text left open:**
  - a text reference's offset is from the start of the text part; `UINT32_MAX` is absent (an empty text is present with length 0);
  - every kind field is 8 bytes: a u64, or a reference;
  - JUMP's targets are its last fields, one reference each, after a u64 count; REPORT carries the report kind (always ERROR, see below) before `ends`;
  - a record has at most one value (`value_count` 0 or 1).
- **Which inputs are records:**
  - an EVENT or DONE report goes through `ta_actor_deliver()`, so it is an EVENT record with its invoke ID; only an ERROR report is a REPORT;
  - FIRED is written the first time a delayed send is offered: a send the router answered RETRY for has left its owner (ADR 0054), and is not written again;
  - RELEASED is written once per macrostep, on the first resume with `no_microstep_limit` of an actor stopped past its limit;
  - FINISHED is written when the resume that reached the final state ends; if the sink refuses it, that resume answers `TA_ERR_NOMEM` (status `TA_RUN_ERROR`) and the next writes it and answers `TA_RUN_DONE`;
  - DESTROYED is not written for a finished actor, one the host logged EVICTED, or one the directory no longer routes its address to (an incumbent a replacement took over);
  - SPAWNED is written by `ta_actor_spawn()` only. A replacement published without a replay (`ta_actor_prepare()` without `at` or `log`) is not reflected in the log; a logged family upgrades by replaying into a replacement. SPAWNED does not carry `pending_sends` a spawn re-arms either (ADR 0074 section 8): a host that persists by replay does not re-arm.
- **Record numbers** live in the actor's sparse state (`log_seq`), which a logged actor now keeps: `ta_compact_sparse()` frees it no longer once the actor has written a record. A host record written for an address with no actor given has seq 0, and does not advance a number. A replay sets the actor's number to each record's, so a published copy continues it.
- **A replaying actor's inputs are refused as runaway-stopped only for inputs:** an actor stopped past its limit takes ABANDONED and RELEASED, and host records; anything else answers `TA_ERR_BUSY`, after the record has been read.
- **RELEASE is a sixth step** (not in the ADR's list): "resume with `no_microstep_limit` until idle" cannot be said with RUN.
- **Replay's origins:** an event from an invocation arrives with its invoke ID and no origin, and core fills the invoke origin (ADR 0068); if the invocation is not active in the replay, the event arrives without its invoke ID. An SCXML origin is parsed back as a target (an actor reference); another processor's origin becomes a MESSAGE origin of the processor registered for its type, or none when this system has no such processor.
- **A literal send ID** a replay meets is interned, so a system that never saw it replays it.
- **Tests:**
  - `tests/input_log_test.c` (new, fast): every kind byte for byte against a second writer of the layout in the test (SPAWNED with a binding, EVENT with data, a send ID, an actor origin and a host number, WITHDRAWN, FIRED, JUMP, QUEUED with data, DROPPED with seq 0, EVICTED, DESTROYED, REPORT with data, RELEASED, ABANDONED, FINISHED once); a live actor's log replayed through 1-, 7- and 4096-byte sources reproducing configuration, deep history, pending sends and the spawn's binding, and a published copy logging on; the timelines of ADR 0089 section 2 from core's own log; a log of two lives ending the first at DESTROYED; a report, a release and an abandon, a jump, a withdrawal and host records (QUEUED with data handed back, the delivered and dropped host numbers); refusals (a sink refusing `begin`; a sink refusing at every byte of a record, a sweep over every byte of the record in which the actor stays idle and the log unchanged; a value with no durable form; a timer whose FIRED is refused stays due; a refused DESTROYED leaves the actor); readers (an unknown version and kind are `TA_ERR_CORRUPT`, a newer writer's extra field is skipped, a truncated SPAWNED is `TA_ERR_CORRUPT`, a log not starting with SPAWNED and `log` with `at` are `TA_ERR_INVALID`, a replay into a published actor is `TA_ERR_INVALID`); and a counting allocator showing that delivering to a logged actor allocates exactly as much as with no sink. None is exhaustive: the byte sweep is bounded by one record's size.
  - test host `ruby/scenarios/replay.rb`, group "input log": a named actor logged by core, evicted, and rebuilt a week later from its log read 7 bytes at a time (nothing logged or fired twice, the records' kinds as expected, a replaying actor writing none); a record the sink refuses refusing its input.
- **Costs:**
  - an unlogged actor: 0 bytes in `ta_actor` (232 before and after); an actor that has a sparse state for another reason pays 8 bytes more for `log_seq` (176 to 184 bytes; its three flags fill padding);
  - a logged actor keeps a sparse state (184 bytes) from its first record on;
  - a definition: 0 bytes (`input_logged` fills padding; 896 before and after);
  - a system: 352 bytes (1,328 to 1,680), of which 256 are the scratch buffer (`TA_INPUT_SCRATCH_BYTES`), plus the logged families' atoms;
  - a record: its fixed part is 32 bytes plus 8 per field; texts are padded to 8; a value costs, for each piece the data model writes, a 4-byte length and padding to 8, plus an 8-byte terminator. As written by the test: SPAWNED for `timers`/`a`/`v1` without a binding 72 bytes; EVENT `e1` with no data 96 bytes; FIRED `t1` 48; DROPPED 40; EVICTED, DESTROYED, ABANDONED and RELEASED 32;
  - writing a record allocates nothing (tested); reading one allocates its fixed part and text part once, and frees them before the call returns;
  - a delivery to an unlogged actor costs one flag test more.

### Addendum (2026-09-30): the test host's data models

- **The jq model now streams both directions.** Its writer walks the `jv`
  value with an explicit stack and writes through one 4 KiB buffer, where it
  used to build the whole text with `jv_dump_string()` and cut it; its reader
  already streamed (`jv_parser_set_buf`). The bytes are `jv_dump_string(v,
  0)`'s: strings are escaped by jq's rules, objects keep jq's order, and each
  number is printed by jq itself, so a literal keeps its text. The one
  difference is depth: jq prints `<skipped: too deep>` past 10,000 levels,
  where the writer writes the value whole (jq's parser takes at most 10,000
  levels, so such a value comes only from a jq program).
  `tinyactors-test/src/json_datamodel.c`; `tests/json_datamodel_test.c`
  compares the writer's bytes with jq's for escapes, control characters,
  DEL, non-ASCII text, number literals and nested containers, writes a 5 MB
  string with no write larger than 4 KiB, and 9,999 levels of nesting.
- **The QuickJS model holds whole encodings by design.** It writes with
  `JS_JSONStringify` and reads by gathering the pieces for `JS_ParseJSON`.
  The test host needs one fully streaming reference model, which the jq
  model is, and QuickJS has no incremental parser; writing one would prove
  again what napi's data model and the jq model already prove. The same note
  is in `ecmascript_datamodel.c`.
- **A scenario shows core's side end to end** (`ruby/scenarios/replay.rb`,
  "a large value is logged and replayed in pieces, and core holds no copy of
  it"): a 4 MB value is delivered to a logged actor on the jq model, logged,
  evicted and replayed from 4 KiB pieces. The sink never receives a write
  larger than 8 KiB, and the counting allocator behind the system peaks far
  below the value's size, so neither the framing nor the replay holds a
  copy. jq allocates its values from its own heap, which the counting
  allocator does not see; the writer's streaming is shown by the unit test
  above. The `input.recording` instrument gained a `stats` command
  reporting its bytes and largest write.
