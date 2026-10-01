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
  only the fields that kind names are meaningful, as today. The state
  arrays (`exited`, `entered`, `history`, `states`) are packed into the
  `states` bytes in the tail.
- [`tel.dropped`](../protocol/generated/MESSAGES.md#teldropped) says how
  many records were dropped since the last one, and the sequence number
  the first of them would have had. Core writes it as soon as the ring
  has room again, before the next `tel.trace`. The ring's sequence
  numbers therefore have a gap exactly where a `tel.dropped` says there
  is one.

### Values are text

A trace record's value (an ASSIGN's new value, a CONDITION's result, a
RAISED event's data) is rendered by the data model's `value_text` and
carried as text, cut at `telemetryValueBytes`
([parameters.md](parameters.md)). A cut value carries the `truncated`
flag.

The reason is ownership. A vref hands a reference to the reader
([data-model.md](data-model.md)). A record that is dropped could not
hand it back, so every drop would leak a value. Text costs a render, but
only for kinds that are enabled and sampled.

Today the kernel's OTLP encoder renders the value as JSON, up to
`TAH_PARAM_VALUE_BYTES` (1,024 bytes by default). `telemetryValueBytes`
replaces that parameter, and the rendering moves from the encoder into
core's write to `tel`.

## Masks and sampling, as today

Nothing changes in what is traced:

- the sink's `kinds`, `watched_kinds` and `unsampled_kinds` masks
  (`TA_TRACE_BIT(kind)`) decide which kinds core writes;
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

## OTLP stays a library

`tinyactors-host/src/otlp.c` (about 2,300 lines) turns trace records
into OTLP protobuf spans and logs. It keeps spans that are still open
between turns, and writes the ones that ended into memory the host
provides (napi ADRs 0016 to 0018).

In this design it becomes **a reader of `tel`**:

- it takes `tel.trace` records instead of `ta_trace_record` callbacks;
- a `tel.dropped` record closes or marks the spans it may have cut. How
  exactly is **open**. The simplest answer is an attribute on the
  enclosing span, plus a counter;
- it moves out of `tinyactors-host`, which is deleted
  ([migration.md](migration.md)). It does not belong in core, which has
  no use for protobuf. Where it should live is **open**: a small C
  library of its own (`tinyactors-otlp`), or a module in each binding.
  Encoding is free of any language's API, so AGENTS.md argues for a
  shared C library.

`tah_otlp_traces_size`/`tah_otlp_put_traces` and
`tah_otlp_logs_size`/`tah_otlp_put_logs` keep their shape. Only their
input changes. napi's `tna_trace_dispatch` (`tinyactors-napi/src/napi_trace.c`)
drains `tel` once per pump turn into the encoder, as it drains the
staging buffer today. `TAH_PARAM_TRACE_STAGING_BYTES` becomes the
encoder's own setting, because `telBytes` now bounds the ring.

## What a host does with `tel`

| Host | Reads `tel` | Does |
|---|---|---|
| napi | once per pump turn | OTLP encoder, then the JavaScript trace hook with `{traces, logs}` bytes |
| test host | after each RPC call that runs core | `instrument.journal`, and assertions on records |
| SQLite host (sketch) | at commit | an insert into a `trace` table, or nothing |
| a host that does not trace | never | set the masks to zero; `telBytes` can be small |

**Open:** whether `telBytes` may be 0, meaning the system has no `tel`
ring at all. That would save the memory for hosts that never trace.
