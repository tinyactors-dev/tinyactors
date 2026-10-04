# Telemetry: the `tel` ring

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

Core's trace records (core ADR 0067) go to their own ring, `tel`. It is
**lossy**: when it is full, core drops the record and counts it.
Telemetry never slows the system, and nothing that must not be lost
travels on it.

## Why a separate ring

The rings are split by delivery guarantee, not by purpose:

| | `c2h` | `tel` |
|---|---|---|
| Loss | none | drops when full |
| When full | the action that writes fails as `error.execution.capacity`; a macrostep starts only with room for its reserve ([ring.md](ring.md)) | the record is dropped and counted |
| Carries vrefs | yes, ownership passes to the host | never |
| Used for replay | yes ([`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput)) | never |

If telemetry shared `c2h`, a slow OTLP exporter would make sends fail.
If recording shared `tel`, a burst of traces would lose inputs. Both
rings use one record format and one reader API ([ring.md](ring.md)), so a
host reads them with the same code.

## Records

- [`tel.trace`](../protocol/generated/MESSAGES.md#teltrace) is one
  `ta_trace_record`. Its `trace_kind` field is the `ta_trace_kind`, and
  only the fields that kind names are meaningful, as today. The records
  mirror today's trace kinds one for one: lifecycle records,
  MACROSTEP_BEGIN and MACROSTEP_END pairs, MICROSTEP, CONDITION, ASSIGN,
  SENT, RAISED, IGNORED, ACTION_FAILED, RUNAWAY and the rest. They are
  events, not spans. Spans are assembled after the ring, by the encoder
  ([below](#otlp-tinyactors-otlp)).
- [`tel.dropped`](../protocol/generated/MESSAGES.md#teldropped) says how
  many records were dropped since the last one, and the sequence number
  the first of them would have had. Core writes it as soon as the ring
  has room again, before the next `tel.trace`. The ring's sequence
  numbers therefore have a gap exactly where a `tel.dropped` says there
  is one.

### Records are self-contained

A `tel.trace` record carries everything a reader needs, in its tail:

- **names as text:** the session's address, the state, transition and
  action at the record's position, the event name, the send and invoke
  IDs. The kinds that open a run (SPAWNED, MACROSTEP_BEGIN, the
  DEFINITION_* kinds) also carry the definition's document name and
  source;
- **the value** as text ([below](#values-are-text));
- **the fault:** its code, its error event name and its message;
- **the lists:** a microstep's transitions, and its exited, entered and
  history states (or an operator transition's target states), each with
  its compiled ID and its name.

So a reader never calls back into core, and needs no live system. It can
read `tel` after the system is destroyed, or in another process.

Self-contained records are larger. A record must still fit
`maxRecordBytes` ([BEHAVIORS.md](../BEHAVIORS.md#maxrecordbytes)). A
microstep in a large chart can exit and enter more states than fit. Core
then cuts the lists, last list first, and sets the `truncated` flag. The
counts in the tail say how many entries are there, not how many there
were.

### Values are text

A trace record's value (an ASSIGN's new value, a CONDITION's result, a
RAISED event's data) is rendered by the data model's `value_text` and
carried as text, cut at `telemetryValueBytes`
([BEHAVIORS.md](../BEHAVIORS.md#parameters)). A cut value carries the `truncated`
flag.

The reason is ownership. A vref hands a reference to the reader
([data-model.md](data-model.md)). A record that is dropped could not
hand it back, so every drop would leak a value. Text costs a render, but
only for kinds that are enabled and sampled.

Today the kernel's OTLP encoder renders the value as JSON, up to
`TAH_PARAM_VALUE_BYTES` (1,024 bytes by default). `telemetryValueBytes`
replaces that parameter, and the rendering moves from the encoder into
core's write to `tel`.

## The host clock

A record read at drain time cannot say when it happened. Core has only
logical time: the header's `at` is the system clock that
[`h2c.time`](../protocol/generated/MESSAGES.md#h2ctime) moves. Today the
OTLP encoder fills the gap itself. It reads the host's wall clock at each
span boundary (`config.host.now()`, `tinyactors-host/src/otlp.c`), while
core calls the sink. After the ring that read happens too late.

So the host installs a **clock function** at system creation. It is
native integration, like the data model: a C function pointer that
returns wall-clock nanoseconds since the Unix epoch. Core calls it once
for every `tel` record it writes, and stamps the record's `wall_ns` with
the result. `wall_ns` is separate from the header's logical `at`.

- One clock call per record is cheaper than the sink call per record it
  replaces.
- napi installs a C function (`clock_gettime`). It never calls into
  JavaScript.
- The clock is **optional.** A host that installs none gets `wall_ns` 0.
  Its spans have no real durations: the encoder falls back to logical
  time. The test host can leave it out, or install a fake clock for
  stable output.
- Core calls the clock only for a record it writes. A kind that is off,
  or a record that is dropped, costs no clock call.

## Masks and sampling, as today

Nothing changes in what is traced:

- the `kinds`, `watched_kinds` and `unsampled_kinds` masks
  (`TA_TRACE_BIT(kind)`, today the sink's) decide which kinds core writes;
- trace contexts and sampling are core's (ADR 0073);
- metrics stay counters that core keeps (ADR 0072). They are read through
  the API, not through `tel`.

A kind that is off costs one branch, as it does today. Records are built
straight into the ring slot, so an enabled kind costs no copy.

**Watched sessions** (`tinyactors-host/src/watch.c`, napi ADR 0017
section 3) are sessions an operator asked to see in full. Today the
kernel's scheduler checks the watch list at each delivery. With the
scheduler in core, the watch becomes a per-actor bit, like the
`recorded` bit ([recording-and-replay.md](recording-and-replay.md)): set
by address selector, and read when the session takes an envelope.

## Replay: the `suppressed` flag

A session prepared to replay crosses no border. The sends and invocations
it would have made appear only on `tel`, as `tel.trace` records of kind
SENT or INVOKED with the `suppressed` flag. A `<log>` has no trace kind of
its own today (it goes to `ta_log_sink`), so a replay drops it without a
record. The
common flag `replaying` is set on every record about the session until
[`c2h.published`](../protocol/generated/MESSAGES.md#c2hpublished).

This lets an operator see what a replay did, and lets a host compare a
replay against the original run
([recording-and-replay.md](recording-and-replay.md), "Checking a replay
for divergence").

## OTLP: `tinyactors-otlp`

`tinyactors-host/src/otlp.c` and its trace adapter (about 2,300 lines)
turn trace records into OTLP protobuf spans and logs. They keep spans
that are still open between turns, and write the ones that ended into
memory the host provides (napi ADRs 0016 to 0018).

They move into **`tinyactors-otlp`**, a small C library of its own. It
is the reader of `tel` that encodes OTLP traces and logs. It does not
belong in core, which has no use for protobuf. It does not belong in each
binding, because encoding is free of any language's API (AGENTS.md).
`tinyactors-host` is deleted ([migration.md](migration.md)). napi and the
test host link `tinyactors-otlp`.

Most of it is a move. What changes:

- **Its input.** It takes `tel.trace` records instead of
  `ta_trace_record` callbacks. Span assembly stays where it is: the
  encoder keeps an open macrostep span and microstep span per actor, as
  today.
- **No callbacks into core.** Names come from the record's tail
  ([above](#records-are-self-contained)), not from
  `ta_definition_inspect`, `ta_definition_span` and their kin.
- **No clock reads.** Span start and end times are the records'
  `wall_ns` ([above](#the-host-clock)).
- **Its staging.** `TAH_PARAM_TRACE_STAGING_BYTES` becomes a setting of
  `tinyactors-otlp`, because `telBytes` now bounds the ring.

Its output API keeps its shape: a size call and a put call for traces
(`tah_otlp_traces_size`/`tah_otlp_put_traces`) and the same pair for
logs. The names may take the new library's prefix. napi's
`tna_trace_dispatch` (`tinyactors-napi/src/napi_trace.c`) drains `tel`
once per pump turn into the encoder, as it drains the staging buffer
today.

Why this split: the event records keep core's tracing as it is. The ring
moves span assembly and protobuf encoding off the macrostep's hot path.
It never slows the system, and the encoder cannot re-enter core.

Rejected:

- **core assembles complete spans** in a format of its own. That moves
  open-span state and its memory into core;
- **core writes OTLP protobuf** directly. Core has no use for protobuf;
- **keep the trace sink vtable.** It runs the encoder inside the
  macrostep, and lets a sink call back into core.

**A drop closes the spans it may have cut.** When `tinyactors-otlp` reads a
[`tel.dropped`](../protocol/generated/MESSAGES.md#teldropped), it ends every
span still open, at the wall-clock time of the last record it read, with
error status and a `tinyactors.telemetry.dropped` attribute that carries the
count. It also adds the count to a counter. A record that arrives later for
a span closed this way starts nothing: the encoder counts it and skips it.
No span is left open with an end it can never see, so no span claims a
duration it did not have. The price is that a drop under load marks spans
as errors that may have been fine. A viewer tells them from real failures
by the attribute. Marking open spans and leaving them open was the
alternative; it was rejected because a span whose end was dropped stays
open until the input completes and reports a wrong duration.

## What a host does with `tel`

| Host | Reads `tel` | Does |
|---|---|---|
| napi | once per pump turn | `tinyactors-otlp`, then the JavaScript trace hook with `{traces, logs}` bytes |
| test host | after each RPC call that runs core | `instrument.journal`, and assertions on records |
| SQLite host (sketch) | at commit | an insert into a `trace` table, or nothing |
| a host that does not trace | never | creates the system with `telBytes` 0 |

**A system may have no `tel` ring.** With `telBytes` 0, core allocates no
ring and the control block has no `tel` positions. The kind masks are
forced to zero, and creation refuses a non-zero mask with `TA_ERR_INVALID`,
naming the parameter. Nothing a host needs arrives only on `tel`: a runaway
is `c2h.input_completed` with status `runaway`, a dead letter is
`c2h.input_refused`, and an ignored event is the `ignored` flag on
`c2h.input_completed`. Writers already test the masks before they build a
record, so the hot path gains no branch. It saves the whole ring for test
suites that create thousands of systems and for systems on small devices.
The lossless extension below needs a ring; creation refuses `telBytes` 0
with lossless mode.

## Possible extension: lossless telemetry

Not in the first version. A creation parameter, such as
`telemetryMode: lossy | lossless`, could make `tel` lossless.

In lossless mode a full `tel` does not drop. It parks the slot at the
next unit boundary, exactly as a full `c2h` does
([macrostep.md](macrostep.md#a-full-c2h-pauses-the-slot)). This cannot
deadlock: the host frees `tel` by draining it, and draining needs nothing
from core.

Some records are written in bursts, inside a unit or between phases:
CONDITION for each guard, MICROSTEP, MACROSTEP_END, RUNAWAY. There is no
boundary to park at between them. So a unit reserves room for them before
it starts. The compiler can bound that reserve, as it bounds `c2h`'s
mandatory batch. The same rule applies as for `c2h`: the reserves
always leave room for one ordinary record
([ring.md](ring.md#the-c2h-reserve)).

Costs:

- it reverses "telemetry never slows the system" for that mode;
- lossless reaches only the host. To be complete end to end, the host
  must stop draining `tel` while its own exporter is full;
- masks and sampling still apply. Lossless means no drops of the records
  core chose to write, not every record.
