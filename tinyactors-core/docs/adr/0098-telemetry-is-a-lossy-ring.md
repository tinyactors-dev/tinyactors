# ADR 0098: Telemetry is a lossy ring

**Status:** Accepted and implemented (2026-10-05); see the implementation notes for where the code differs from the text. Refines ADR 0067 and ADR 0073.
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

## Implementation notes

### What was built (2026-10-05)

- **Core.** A system's control block (`ta_control_block` in
  `src/system.h`) holds the `tel` ring, a lossy `ta_ring`, whose counters
  are the ring's positions and its dropped count. `ta_trace_open` makes it
  at creation with a scratch area of `maxRecordBytes`; `telBytes` 0 makes
  neither. Three parameters are new: `telBytes` (16,384), `maxRecordBytes`
  (4,096) and `telemetryValueBytes` (1,024), with creation checking that
  `telBytes` is 0 or at least `maxRecordBytes` and `telemetryValueBytes` at
  most `maxRecordBytes` − 512. Call sites describe a record as a private
  `ta_trace_entry`; `src/trace.c` names it, renders its values, cuts it to
  fit, reports earlier drops, reserves it, stamps it and commits it
  (`src/trace.h` is the unit's contract). The public API gains
  `ta_system_set_trace_masks`, `ta_system_set_wall_clock` (`ta_wall_clock`
  and its vtable), `ta_system_tel_peek`, `ta_system_tel_release`,
  `ta_system_tel_status`, `ta_tel_record`, `ta_tel_kind`, `ta_tel_status`,
  `ta_trace_masks` (with `value_bytes`, below), `ta_trace_text` and
  `ta_trace_record_text`, the three parameters, and `trace_bytes` in
  `ta_datamodel_value_text_params`. `ta_trace_sink`,
  `ta_trace_sink_vtable`, `ta_null_trace_sink` and
  `ta_system_set_trace_sink` are gone. `ta_trace_record` stays, as a
  `tel.trace` record's IDs, numbers, flags and lists, which peek fills
  from the record's fixed part, and the record itself, whose texts
  `ta_trace_record_text` reads in place, only when a reader uses them.
  Each value's text is copied into the scratch
  area as soon as the data model renders it, so a data model may render
  every value into one buffer. `ta_system_destroy` sets the masks to zero
  first, so teardown builds no record, and calls no clock, for a ring
  about to be freed. Tests: `tests/tel_test.c` (nine claims; it includes
  the amalgamation, to write a record with three rendered values);
  `trace_test` and the other tests that installed a sink read tel
  instead, with no assertion weakened. `session_id_test` reads a record's
  session by its text, and `retirement_test` sees that teardown destroys
  every actor before it frees a definition through the definition sink,
  since teardown writes no record.
- **The host kernel.** The OTLP encoder takes `tel.trace` records: names,
  values and the fault come from the record, times from `wall_ns`, and the
  facts that name a definition's scope (family, revision, document name,
  source) are cached from the records that open a run. It no longer calls
  core for a record, nor reads the host clock for one. `tah_system_drain_telemetry`
  reads tel into it whenever core returns to the kernel, and before a
  `<log>` or definition log, so logs keep core's order. Decision 9 is the
  encoder's `tah_otlp_dropped`. The kernel installs the platform's clock
  and derives `telemetryValueBytes`, `maxRecordBytes` and `telBytes` while
  they are unset or at their defaults (napi passes every parameter, so an
  explicit value equal to the default follows the kernel too):
  `telemetryValueBytes` is `valueBytes`, `maxRecordBytes` at least that
  plus 512, and, when the binding says at creation that it may turn the
  encoder on (`traced`, as napi does), `telBytes` holds one resume's
  records, since the kernel reads tel only between resumes:
  `resumeBudget` microsteps of four records (the microstep, a guard, an
  action and the event it raises) and a macrostep's begin and end, each
  of `maxRecordBytes`, at most 2 GiB. With napi's defaults that is 258
  records of 4,096 bytes, 1,056,768 bytes, where 16 KiB held about 36 and
  a macrostep raising 20 events at detail `full` dropped records. The
  encoder is turned on after creation and core sizes its ring at
  creation, so every napi system gets that ring, traced or not; its pages
  are touched only as records are written. A system that is not `traced`
  (the test host's) keeps 16 KiB, or four of the largest records when
  `valueBytes` makes them larger.
  `tah_otlp_values` tells a binding whether the encoder puts values in
  attributes. `adapter_trace.{c,h}` and `tah_trace_masks` are gone.
- **The test host.** `trace.recording` is attached with an `attach`
  command, sets the masks, and reads tel after every method and before any
  other instrument journals, so the journals keep their order. It asks
  for every value, and a Trace relabels the session number inside
  `session_id` and `cause_session_id` as it does `key.session`, so the
  scenarios compare the texts. Scenario systems get a 1 MiB ring, since a
  scenario compares every record.
- **napi.** Only its call sites: `napi_trace.c` sets masks instead of
  installing a sink, with `value_bytes` 0 unless `values` is on, and
  drains before it reads the encoder; `napi_system.c` drains before a
  definition log and sizes the value buffer once, at creation;
  `napi_datamodel.c` answers `trace_bytes` with the JSON text the encoder
  used to ask for; `bindings.rb` names the new functions and types;
  `test/tracing.js` records the three new parameters' defaults in `v1`,
  and checks that a macrostep raising 20 events at detail `full` drops
  nothing.

### Where the code differs from the text

1. **The record is 384 fixed bytes, not 224** (decision 2). It mirrors
   today's record field for field: beside the names it keeps the IDs
   (actor, definition, the send and invoke handles, the state and action
   the kind names, the expression, ABANDONED's count, RUNAWAY's cause and
   five list counts), the event's type, sendid, invokeid and data, and the
   fault's data. Every user of the old record (the encoder, the test
   host's journal, core's tests) needs them to stay what they were. It
   carries no atom: the families of the key and the cause were
   process-local IDs, and the session texts name the families, so a
   decoded record's `key.family` and `cause.family` are 0. FOREACH's
   iteration is the position's. `maxRecordBytes`' minimum is therefore
   512, and `telemetryValueBytes` is at most `maxRecordBytes` − 512, not
   − 256.
2. **The lists are two spans** (decision 3): `list_ids`, every u32 ID back
   to back, first in the tail so a reader uses them in place, and
   `list_names`, each name as a u32 length and its bytes, in the same
   order. A record cut to fit keeps its lists whole from the first.
3. **Definition text is four fields** on SPAWNED, LOADED, MACROSTEP_BEGIN
   and the DEFINITION_* kinds: family, revision, document name and source.
   LOADED opens a run too; the family and revision name a span's scope.
   The session's ID as text (`session_id`, `cause_session_id`) is there as
   well as its address, since that is what OTLP names a session by.
4. **The host reads tel through functions**, not the control block
   directly: the record layouts are not in `tinyactors.h` yet, so
   `ta_system_tel_peek` checks a record with the codec and fills
   `ta_trace_record` with its IDs, numbers, flags and lists, and
   `ta_trace_record_text` reads a text, named by `ta_trace_text`, in place
   from the record's span; both point into the ring until
   `ta_system_tel_release`. No text is copied or decoded before a reader
   asks for it. The control block is private, and `ta_system_tel_status`
   reports its counters.
5. **The clock is installed by a call** that may come at any point, not
   only at creation.
6. **`value_text` says when it renders for a trace** (`trace_bytes`, 0 for
   a conversion): a data model may then render the value for people (napi
   gives JSON, as its encoder did, where a conversion gives `String()`)
   and may stop past `trace_bytes`, so core can tell the text was cut.
   **The masks carry `value_bytes`** (decisions 3 and 6): the text kept of
   a value and of an event's data, at most `telemetryValueBytes`, and 0 to
   render none and call no `value_text` for them, as napi's `values: false`
   asks. The reason a failure gives (a fault's data, the data of an error
   event core raises) is kept at `telemetryValueBytes` whatever it says,
   since a failure log writes it whatever `values` says.
7. **The fixed part is built in the slot**, but the call site fills a
   private entry on its stack first, and the texts core composes (labels,
   session text, the lists) are laid out in the scratch area and copied
   into the slot, because the record's size must be known before it is
   reserved. A record is measured once, as its texts are cut to fit, and
   the codec copies texts that lie end to end in one run. Texts that
   consecutive records repeat are kept instead of composed again: the
   last session's ID text, the last transition label and action label,
   and the last definition's document name and source, kept per system
   and forgotten when their definition is freed.
8. **Not in this change:** the per-actor watch bit (decision 6; the
   kernel's watch list stays), trace contexts on h2c and c2h records
   (decision 11), the `replaying` and `suppressed` flags (decision 12; no
   replay crosses a ring yet), and `tinyactors-otlp` (decision 8; the
   kernel's encoder reads tel meanwhile). A system being destroyed writes
   no record: the ring goes with it. The encoder still
   reads the host's clock for what is not a trace record: `<log>` records,
   host and definition logs, and spans ended because the sink goes.

### The OTLP output

The golden request of the kernel's `otlp_test` is unchanged byte for
byte, with core's clock at the value the encoder's clock had. The same
traces and logs from napi built before and after this change (a chart
that assigns, raises, cuts a value at `valueBytes` 64, fails an action
and moves between states, with `values: true`) decode to the same spans,
events, attributes and log records, of the same byte counts, once times
and IDs are set aside. What changed is the times: each span event now
carries its own record's `wall_ns`, where it carried the clock reading of
the last span boundary, and a span's start and end are its records'
`wall_ns`. A definition's scope is named only once the encoder has read a
record that opens a run; before, it asked core, so a sink whose masks
leave out every kind that opens a run names a scope by its ID alone.
Values are the data model's trace text, which for napi is the JSON it
was. napi's suites (Node and Bun), the test host's scenarios and the
kernel's tests pass unchanged apart from reading tel.

### Figures

- **Record sizes:** on a chart that writes every common kind (`tel_test`'s
  chart, names of a few characters), records take 408 to 488 bytes, 448 on
  average, so the default ring holds about 36 between drains.
- **Per system:** the ring (16,384 bytes by core's default; 1,056,768
  under the kernel with napi's defaults, see above) and the scratch area
  (4,096), and `ta_system` grows from 1,568 to 2,576 bytes: 255 of them
  the text of the last session a record named, 560 the last transition
  and action labels, 40 the last definition's facts; with `telBytes` 0,
  the ring and scratch area are not allocated.
- **Per actor:** nothing; `sizeof(ta_actor)` stays 232.
- **Cost per traced event** (`mise run bench-compare 20a6456 10000` in
  napi, 10,000 actors, three alternating runs, medians; Node on machine
  `a99e44a37e2e`, Bun on `c989bd79a51f`; each event is a macrostep that
  writes BEGIN, MICROSTEP, ASSIGN and END). The first run is the change
  as first written; the second, on another day, after the corrections in
  these notes (values rendered only when asked for, the writer zeroing
  only its padding, the reader decoding in place, napi's value buffer
  sized once, and the kernel's larger ring); the third, on another day,
  after one round of optimisation the profile below directed. Each is
  against the same old build, in ns per event:

  | Variant | Before | First | Change | Before | Corrected | Change | Before | Final | Change |
  |---|---|---|---|---|---|---|---|---|---|
  | untraced | 473 | 473 | 0% | 501 | 500 | 0% | 482 | 485 | +1% |
  | sampled out | 500 | 498 | 0% | 504 | 514 | +2% | 512 | 512 | 0% |
  | macrosteps (BEGIN, END) | 886 | 1,206 | +36% | 932 | 1,223 | +31% | 914 | 1,087 | +19% |
  | summary | 860 | 1,149 | +34% | 917 | 1,235 | +35% | 891 | 1,039 | +17% |
  | decisions | 1,317 | 1,744 | +32% | 1,398 | 1,822 | +30% | 1,359 | 1,541 | +13% |
  | every record | 1,522 | 2,474 | +63% | 1,599 | 2,245 | +40% | 1,567 | 1,851 | +18% |
  | full + values | 1,757 | 2,489 | +42% | 1,889 | 2,477 | +31% | 1,820 | 2,066 | +14% |

  On Bun the final build costs, against the old one: untraced 404 and
  403, sampled out 429 and 430, macrosteps 833 and 982 (+18%), summary
  823 and 972 (+18%), decisions 1,273 and 1,478 (+16%), every record
  1,450 and 1,775 (+22%), full + values 1,636 and 1,933 (+18%).

  **Where the time went.** A sampling profile (`sample`, Node, 10,000
  actors, 400 rounds of each variant against the old build) attributed
  each event's time by the innermost of core's write (`ta_trace_write`
  and the codec's writer), the host's read (`ta_system_tel_peek`, its
  decode and `ta_system_tel_release`), the clock, the kernel's encoder
  (`tah_otlp_*`, which reads the record's texts), and the rest (the
  interpreter, the scheduler and JavaScript), in ns per event:

  | Variant, build | Core write | Host read | Clock | Encoder | Rest |
  |---|---|---|---|---|---|
  | every record, old | 48 | — | 89 | 759 | 727 |
  | every record, corrected | 530 | 97 | 95 | 741 | 755 |
  | every record, final | 278 | 79 | 95 | 589 | 762 |
  | macrosteps, old | 25 | — | 31 | 281 | 604 |
  | macrosteps, corrected | 251 | 37 | 50 | 223 | 673 |
  | macrosteps, final | 133 | 38 | 56 | 199 | 599 |

  (The old build's encoder ran inside the sink call and asked core for
  names; its clock reads were the encoder's, at each span boundary.) In
  the corrected build a record cost core about 120 ns to write: of that,
  the codec's generic writer built an array of the 25 texts twice and
  looked the kind up three times, and `ta_text_put` stored a byte at a
  time; the session's ID text, transition and action labels and the
  definition's facts were composed again for each record; and the host
  decoded every text of every record into its struct, used or not. The round
  made the writer and size function straight-line code per kind, which
  copies texts lying end to end in one run; measured each record once;
  made `ta_text_put` copy a run at once; kept the last labels and facts
  between records; and let the host read texts in place
  (`ta_trace_record_text`), so peek fills only IDs, numbers, flags and
  lists. Core now writes a record in about 65 ns, and the host reads one
  in about 18.

  **What is left is the price of self-contained records.** Untraced and
  sampled-out systems pay nothing in time. A traced event costs 13% to
  22% more than before this change: about 65 ns per record in core and
  18 in the host's read, against an encoder that is now cheaper than
  before because it no longer calls core. Core composes and copies
  every name and the session's ID as text into each record (the session's ID text alone
  is about 25 ns of each MACROSTEP_BEGIN, since consecutive macrosteps
  are other actors'), reserves and commits it in the ring, and calls the
  clock once per record (about 25 ns on macOS, where the encoder before
  read it at each span boundary); and the host walks the ring. That is
  what a reader with no live system needs. Memory per actor moves by the
  system's ring and scratch area spread over 10,000 actors: core bytes
  per actor go from 294 to 400 untraced and 316 to 422 traced, since
  every napi system allocates the 1 MiB ring that holds one resume's
  records; RSS per actor moves by 0% to 9%, since a ring's pages are
  touched only as records are written. OTLP bytes per event are the same
  within 0.2%.
