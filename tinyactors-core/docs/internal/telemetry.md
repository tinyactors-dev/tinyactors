# Telemetry: the `tel` ring

Core reports what it decides, and the data it changes, as trace records
(ADR 0067) on the system's `tel` ring (ADR 0098). This document is the
part that crosses units and reaches the host. How one record is built,
cut and written is in [`src/trace.h`](../../src/trace.h); how the ring
itself works is in [`src/ring.h`](../../src/ring.h); the record's layout
is [`tel.trace`](../../src/generated/MESSAGES.md#teltrace) and
[`tel.dropped`](../../src/generated/MESSAGES.md#teldropped).

## The path of a record

1. **A call site decides** whether a kind is wanted, with one mask test
   before it builds anything: the interpreter for a macrostep's records,
   actors and session images for the lifecycle records, the system for the
   definition records. The masks are `ta_trace_masks` (ADR 0073), set with
   `ta_system_set_trace_masks` between calls.
2. **The call site describes the record** as a `ta_trace_entry`: IDs, the
   event, the value, the fault and the lists, all borrowed.
3. **`ta_trace_write` builds it in the ring.** It writes a `tel.dropped`
   first when records were dropped since the last report, renders the
   values through the actor's data model (`value_text` with
   `trace_bytes`), names every ID, cuts what does not fit
   `maxRecordBytes`, reserves the record, stamps it with the host's wall
   clock, fills it in place and commits it.
4. **The host reads it** between calls: `ta_system_tel_peek` answers the
   oldest record's IDs, numbers, flags and lists as a `ta_trace_record`,
   `ta_trace_record_text` reads each text in place in the ring, only when
   the host uses it, and `ta_system_tel_release` gives its room back.

Writing a record never allocates and never fails: a full ring drops the
record and counts it. Nothing a host must not lose is on `tel`.

## What a record carries

A record is self-contained. Beside each ID is its name: the session's ID
as `_sessionid` text, the session's named address, the names of the
state, transition and action at the position and of those the kind names,
the event's name, type, sendid, invokeid and data, the send and invoke
IDs, the `<data>` name, the fault's code, error event name, message and
data, and each list entry's ID and name. The kinds that open a run
(SPAWNED, LOADED, MACROSTEP_BEGIN and the DEFINITION_* kinds) also carry
the definition's family, revision, document name and source. A reader
needs no live system and never calls back into core.

- **Values are text**, rendered by the data model for a trace and cut at
  the masks' `value_bytes`, at most `telemetryValueBytes`; with 0 no value
  is rendered, but the reason a failure gives (a fault's data, an error
  event's data) still is. A record never holds a value reference, nor an
  atom: a session's family is in its ID text.
- **Times:** the header's `at` is the system's logical time; `wall_ns` is
  the clock the host installed with `ta_system_set_wall_clock`, or 0.
- **Sizes:** the fixed part is 384 bytes; with names, a record of a small
  chart takes 408 to 488 bytes (448 on average over every kind of one
  chart's run), so the default ring of 16,384 bytes holds about 36. A
  host that reads `tel` only between calls sizes `telBytes` for the most
  records one call writes (the kernel does, below).

## Parameters

| Parameter | Default | Bounds | What it bounds |
|---|---|---|---|
| `telBytes` | 16,384 | 0, or `maxRecordBytes` to 2 GiB | the ring; 0 makes none, and the masks must stay zero |
| `maxRecordBytes` | 4,096 | 512 to 429,496,729 | the largest record, header and tail |
| `telemetryValueBytes` | 1,024 | 0 to `maxRecordBytes` − 512 | the most text of each value the masks' `value_bytes` may keep; 0 renders none |

A system with a ring also has a scratch area of `maxRecordBytes`, where a
record's composed texts are laid out before it is reserved: 20,480 bytes
per system with the defaults, and none with `telBytes` 0. The tracer
keeps the text of the last session a record named, so a run of one
session's records formats its ID once.

## The hosts

- **The host kernel** (`tinyactors-host`) reads `tel` into its OTLP
  encoder whenever core returns to it (`tah_system_drain_telemetry`):
  after each core call, between the resumes of a turn, before a `<log>`
  record and before a definition's log. The encoder takes names and times
  from the records, keeps the definition facts that name a scope, and ends
  every open span when core reports dropped records. It installs the
  platform's clock, and while they are unset or at their defaults it sets
  `telemetryValueBytes` to its `valueBytes`, `maxRecordBytes` to at least
  that plus 512, and, for a binding that may turn its encoder on (napi),
  `telBytes` to one resume's records: `resumeBudget` microsteps of four
  records and a macrostep's two, each of `maxRecordBytes` (about 1 MiB
  with napi's defaults). Its encoder's
  `values` setting decides the masks' `value_bytes`: 0 when values are
  not put in attributes.
- **The test host** reads `tel` into its `trace.recording` instrument after
  each method that may run core, and before any other instrument but an
  allocator journals, so its journals keep core's order. It asks for every
  value, installs no clock, and relabels the session numbers in the
  session texts as it does in the keys, so runs compare equal.

## Invariants

- A record is written once, when the step that produced it commits (ADR
  0070); PAUSED, RESUMED and STOPPED are the only records that differ
  between a run that was interrupted and one that was not.
- The ring's sequence numbers have a gap exactly where a `tel.dropped`
  says there is one; the report comes before the next record.
- A kind that is off, or a record that is dropped, costs no clock call.
- A system being destroyed writes no record: `ta_system_destroy` sets the
  masks to zero first, since the ring goes with the system.
