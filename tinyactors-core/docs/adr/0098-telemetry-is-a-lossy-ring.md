# ADR 0098: Telemetry is a lossy ring

**Status:** Accepted (2026-10-05); not yet implemented. Refines ADR 0067 and ADR 0073.
**Date:** 2026-10-05

## Context

ADR 0067 gives a system one trace sink, a `{self, vtable}` adapter with a
mask of kinds. Core calls `write` once per record, inside the macrostep,
with a `ta_trace_record` whose fields are borrowed for the call. ADR 0073
moves trace contexts into core: it parses and writes `traceparent`,
generates trace IDs, samples, and replaces the sink's `unsampled` flag
with the masks `kinds`, `watched_kinds` and `unsampled_kinds`.

The sink works, but its cost and its coupling sit in the wrong place:

- **The encoder runs inside the macrostep.** The host kernel's OTLP
  encoder (`tinyactors-host/src/otlp.c`, 2,334 lines) is the sink napi
  installs. Every record runs span assembly and protobuf encoding before
  core continues. napi ADR 0017 measured the result and accepted it as
  the baseline: a traced macrostep span costs about 530 ns (Node) and
  575 ns (Bun) over an untraced one, and every record about 1,470 ns
  against 780.
- **A sink calls back into core.** The record holds IDs, so the encoder
  asks core for names (`ta_definition_inspect`, `ta_definition_span`
  and their kin) while core is in the middle of a run. A record cannot be
  read after the system is gone, or in another process.
- **The time is read by the encoder.** The encoder reads the host's clock
  (`config.host.now()`) at each span boundary. That is right only because
  the sink runs at the moment the record is made.
- **Values are borrowed.** The sink retains the values it keeps, and the
  kernel renders them as text, cut at `TAH_PARAM_VALUE_BYTES` (1,024
  bytes).

Core is moving to talk to its host through three rings, `h2c`, `c2h` and
`tel`, which split by delivery guarantee: `h2c` and `c2h` are lossless,
`tel` is lossy. A lossy ring needs no host call per record, and a reader
drains it when it chooses. `src/ring.h` already has the lossy mode: a full
ring drops the record, counts it, and consumes its sequence number, and
`ta_ring_take_drops` answers how many were dropped and the first lost
sequence number.

ADR 0067 rejected "records serialized into a host-lent buffer": it saved
one indirect call per record, and core would copy borrowed arrays and
values into a format of its own. That reason no longer holds. The
encoder's own cost, not the call, is what slows a traced macrostep, and a
ring takes it off the macrostep.

The design was worked out in
[telemetry.md](../../../docs/proposals/ring-proto/reference/telemetry.md),
[ring.md](../../../docs/proposals/ring-proto/reference/ring.md),
[envelope.md](../../../docs/proposals/ring-proto/reference/envelope.md#the-trace-parent)
and [BEHAVIORS.md](../../../docs/proposals/ring-proto/BEHAVIORS.md#telbytes).
The record layouts are in
[MESSAGES.md](../../src/generated/MESSAGES.md#teltrace).

Options considered and rejected:

- **Complete spans from core.** Core would keep open-span state per actor,
  and its memory, across turns. Core's records are events; turning them
  into spans is the reader's work.
- **OTLP protobuf from core.** Core has no use for protobuf, and ADR 0067
  already keeps OTEL export out of core. Encoding is free of any
  language's API, so it belongs in a C library the hosts share, not in
  core and not in each binding.
- **Keeping the trace sink vtable.** It runs the encoder inside the
  macrostep, and it lets a sink call back into core.

## Decision

1. **Core writes trace records to `tel`, a lossy ring.** When `tel` is
   full, core drops the record and counts it. Telemetry never slows the
   system, and nothing a host must not lose travels on `tel`. The trace
   sink vtable, `ta_null_trace_sink` and `ta_system_set_trace_sink` are
   removed. Core builds each record straight into the ring slot, so an
   enabled kind costs no copy of the fixed part.

2. **`tel.trace` mirrors today's trace kinds one for one.** It is one
   `ta_trace_record` (`0x0301`, 224 fixed bytes). `trace_kind` is the
   `ta_trace_kind`, and only the fields that kind names are meaningful:
   the lifecycle kinds, MACROSTEP_BEGIN and MACROSTEP_END, MICROSTEP,
   CONDITION, ASSIGN, SENT, RAISED, IGNORED, ACTION_FAILED, RUNAWAY and
   the rest. Records are events, not spans.

3. **A record is self-contained.** Its tail carries:
   - **names as text:** the session's address, the state, transition and
     action at the record's position, the event name, and the send and
     invoke IDs;
   - **definition text** (document name and source), only on the kinds
     that open a run: SPAWNED, MACROSTEP_BEGIN and the DEFINITION_* kinds;
   - **the value as text,** rendered by the data model's `value_text` and
     cut at `telemetryValueBytes` (default 1,024; minimum 0; maximum
     `maxRecordBytes` − 256). Values are never vrefs on `tel`: a dropped
     record cannot hand a reference back, so every drop would leak one;
   - **the fault:** its code, its error event name and its message;
   - **the lists:** a microstep's transitions, and its exited, entered and
     history states (or an operator transition's target states), each
     with its compiled ID and its name.

   A record must fit `maxRecordBytes`. When the lists do not fit, core
   cuts them, last list first. The counts in the tail say how many entries
   are there, not how many there were. A cut value or a cut list sets the
   `truncated` flag. A reader never calls back into core and needs no live
   system.

4. **`tel.dropped` reports drops.** It carries the number of records
   dropped since the last report and the sequence number the first of
   them would have had (`0x0302`, 48 fixed bytes). Core writes it as soon
   as `tel` has room again, before the next `tel.trace`. It reserves the
   report first and only then calls `ta_ring_take_drops`, so a refused
   report is counted with the rest. The ring's sequence numbers have a gap
   exactly where a `tel.dropped` says there is one.

5. **Core computes trace and span IDs (ADR 0073).** `tel.trace` carries
   `trace_id_high`, `trace_id_low`, `trace_flags`, `trace_detail` and the
   cause's step key. It has no span ID field: a span ID is
   `ta_step_key_span_id` of the step key, the fixed 64-bit mix of ADR
   0067 section 6, the same in every reader.

6. **Core decides what is traced, with today's controls.** The masks
   `kinds`, `watched_kinds` and `unsampled_kinds` (`TA_TRACE_BIT(kind)`)
   and ADR 0073's sampling stay. A kind that is
   off costs one branch, as in ADR 0067 section 2. A watched session is a
   per-actor bit, set by address selector and read when the session takes
   an envelope; it replaces the watch list the kernel's scheduler checks
   at each delivery. Metrics stay counters read through the API (ADR
   0072), not records on `tel`.

7. **The host installs a clock natively.** At system creation the host
   may install a clock function: a C function pointer that answers
   wall-clock nanoseconds since the Unix epoch. Core calls it once for
   each `tel` record it writes and stamps the record's `wall_ns`. A kind
   that is off, or a record that is dropped, costs no clock call.
   - **Why core stamps the time:** a record read at drain time cannot say
     when it happened, and core itself has only logical time. `wall_ns` is
     separate from the header's logical `at`, which `h2c.time` moves.
   - **The clock is optional.** Without one, `wall_ns` is 0, and the
     encoder falls back to logical time. The test host can leave it out or
     install a fake clock for stable output.
   - **It is native, like the data model.** napi installs `clock_gettime`
     and never calls into JavaScript for it.

8. **`tinyactors-otlp` encodes.** It is a small C library that reads
   `tel.trace` and encodes OTLP traces and logs; napi and the test host
   link it. It keeps the spans that are still open between turns (an open
   macrostep span and microstep span per actor), takes names from the
   record's tail and times from `wall_ns`, reads no clock and never calls
   into core. Its staging bound is its own setting; `telBytes` bounds the
   ring, not the encoder. It is mostly a move of `tinyactors-host/src/otlp.c`
   and its trace adapter.

9. **A drop ends the open spans.** When `tinyactors-otlp` reads a
   `tel.dropped`, it ends every open span at the `wall_ns` of the last
   record it read, with error status and a
   `tinyactors.telemetry.dropped` attribute carrying the count, and adds
   the count to a counter. A later record for a span ended this way starts
   nothing: the library counts it and skips it. So no span claims a
   duration it did not have. A span left open until its input completes
   would report a wrong duration, because its end may be among the dropped
   records.

10. **`telBytes` 0 creates no `tel` ring.** `telBytes` defaults to 16,384;
    otherwise it is 0, or at least `maxRecordBytes`, so the largest record
    fits. With 0, core allocates no ring, the control block has no `tel`
    positions, and creation refuses a non-zero kind mask with
    `TA_ERR_INVALID`, naming the parameter. Writers test the masks before
    they build a record, so the hot path has no branch for a missing ring.
    This is safe because nothing a host needs arrives only on `tel`: a
    runaway is `c2h.input_completed` with status `runaway`, a dead letter
    is `c2h.input_refused`, and an ignored event is the `ignored` flag on
    `c2h.input_completed`.

11. **Trace context travels on records.** A trace parent is a W3C trace
    context and the key of the step that caused it (ADRs 0067, 0073). No
    host calls into core to read or write one.
    - **In:** `h2c.event`, `h2c.spawn` and `h2c.invoke_report` carry
      `trace_id_high`, `trace_id_low`, `parent_span_id`, `trace_flags` and
      `trace_detail`. A host fills them with `ta_traceparent_parse`. An
      all-zero trace ID means no context. The macrostep that takes the
      input runs under it, with the host cause
      `{session 0, macrostep parent_span_id}`.
    - **Out:** `c2h.send`, `c2h.invoke`, `c2h.invoke_forward` and
      `c2h.log` carry the step's trace ID and flags and its `span_id`
      (`ta_step_key_span_id` of the step). An I/O processor writes
      `traceparent` version `00` from them. The local `detail` byte stays
      in the process.
    - **Recorded:** `c2h.input` carries the input's trace parent in
      `h2c.event`'s form, so a replay runs under the same trace ID and
      parent span.
    - **Local mail** never leaves core. The envelope carries the sender's
      context and step key in its name-area text, from the send to the
      receiver's take, and a delayed send keeps it the same way, in images
      and in `c2h.state`. A traced envelope takes 40 bytes of name area
      (a 2-byte length, an 18-byte context, a 20-byte step key); an
      untraced one takes none.
    - **Loads:** a load writes its LOADED record under the trace parent of
      the first envelope waiting for the address.

12. **A replaying session shows its effects only on `tel`.** The sends
    and invocations it would have made are `tel.trace` records of kind
    SENT or INVOKED with the `suppressed` flag. Every record about the
    session carries the `replaying` flag until `c2h.published`.

**Possible extension, not decided: lossless telemetry.** A creation
parameter could make `tel` lossless: a full `tel` would park the slot at
the next unit boundary, as a full `c2h` does, with a compiler-sized
reserve for records written in bursts. It would reverse "telemetry never
slows the system" for that mode, and would require `telBytes` above 0.
This ADR does not adopt it.

**What this ADR does not decide:** the ring's record format, header and
reader API, and the other two rings; the envelope pool and the name area
that hold the 40 bytes; the removal of `tinyactors-host`; the API of
`tinyactors-otlp` and its staging bound; the shape of napi's JavaScript
trace hook; the test host's journal; how `<log>` reaches a host
(`c2h.log`).

## Consequences

- **Traced macrosteps lose the encoder.** Today a traced macrostep pays
  the sink call and the encoder's work for every record (napi ADR 0017:
  about 1,470 ns per record on Node, against 780). After, it pays for
  building the record in the slot, rendering a value, copying names into
  the tail, and one clock call. Span assembly and encoding move to the
  drain, once per pump turn in napi. They still run on the system's
  thread, so a turn's total work does not vanish; it leaves the
  macrostep, and the encoder can no longer re-enter core. These are
  estimates. The implementing change measures traced and untraced
  throughput with `bench-compare` and records them.
- **Per system:** a `tel` ring of `telBytes`, 16,384 bytes by default, and
  its positions in the control block, where today the system holds only
  the sink adapter. With `telBytes` 0 it holds neither. The kernel's
  encoder staging (`traceStagingBytes`, 1 MiB by default, reserved while
  tracing) moves to `tinyactors-otlp` as its own setting.
- **Per record:** 224 bytes of fixed part plus the tail, aligned to 8,
  where today the record is a struct borrowed for one call. So 16 KiB
  holds at most 73 records with empty tails, fewer with names, values and
  lists. A host that traces every kind in a busy turn raises `telBytes`
  or sees `tel.dropped`.
- **Per record written, one clock call** when a clock is installed. None
  for a kind that is off or a record that is dropped.
- **Per value:** the render moves from the kernel's encoder into core's
  write, cut at `telemetryValueBytes` instead of `TAH_PARAM_VALUE_BYTES`
  (both 1,024 by default). It happens only for enabled, sampled kinds.
- **Per traced envelope:** 40 bytes of name area, which with a short name
  usually takes a second 64-byte chunk. An untraced envelope adds
  nothing. Today a pending delayed send carries its parent in 48 bytes
  (ADR 0067 section 8).
- **Per actor:** a watch bit. The implementing change reports whether it
  changes the actor's size.
- **Untraced cost** stays one mask test per call site. With `telBytes` 0
  the ring's memory goes too, which helps test suites that create
  thousands of systems and small devices.
- **Telemetry may be incomplete.** A drop under load marks as errors spans
  that may have been fine; a viewer tells them from real failures by the
  `tinyactors.telemetry.dropped` attribute.
- **Records outlive the system.** A reader can read `tel` after the system
  is destroyed, or in another process, because records name everything
  they refer to. The price is the text in every record, and the
  definition text on the kinds that open a run.
- **ADR 0067's sink rules go with the sink.** A sink may no longer
  inspect other actors during a record, since there is no call. ADR
  0067's null sink (section 2) is replaced by `telBytes` 0 or zero masks.
