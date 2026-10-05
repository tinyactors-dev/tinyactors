# ADR 0098: Telemetry is a lossy ring

**Status:** Accepted (2026-10-04); implementation pending (ring-proto Phase 4.1). Refines ADR 0067: trace records go to the `tel` ring instead of a sink, and their values are text. The first of the core ADRs that adopt [the ring protocol proposal](../../../docs/proposals/ring-proto/README.md).
**Date:** 2026-10-04

## Context

ADR 0067 hands every trace record to a host sink, one callback per
record (`ta_trace_sink`, `ta_trace_sink_vtable.write`). The record is
borrowed for the call, and a sink that keeps a value retains it. The host
kernel's adapter (`tinyactors-host/src/adapter_trace.c`) copies each record
into a staging buffer (`TAH_PARAM_TRACE_STAGING_BYTES`), and the OTLP
encoder (`otlp.c`) later turns the staged records into spans and logs,
rendering each value as JSON up to `TAH_PARAM_VALUE_BYTES` (1,024 bytes).

The ring protocol proposal replaces core's callbacks to its host with three
rings in memory core provides ([ring.md](../../../docs/proposals/ring-proto/reference/ring.md)):
`h2c` for inputs, `c2h` for what must not be lost, and `tel` for telemetry,
which may be dropped. The rings are split by delivery guarantee, not by
purpose ([telemetry.md](../../../docs/proposals/ring-proto/reference/telemetry.md)):

- if telemetry shared `c2h`, a slow OTLP exporter would make a chart's
  sends fail;
- if recording shared `tel`, a burst of traces would lose inputs.

`tel` is the smallest real user of a ring. It needs no admission, no
reserve and no answer from the host, so it can replace the trace sink
before anything else changes (IMPLEMENTATION.md, step 4.1). The kernel
stays, and drains `tel` into its OTLP encoder.

A lossy ring cannot carry a value reference. A record that is dropped
could not give its reference back, so every drop would leak a value.

## Decision

### 1. Core allocates a control block and the `tel` ring when a system is created

`ta_system_create()` allocates, in one allocation from the system's
allocator (ADR 0023), a control block and the `tel` ring's bytes, laid out
with the layout builder (block A2). Nothing changes size afterwards.

```c
typedef enum { TA_RING_H2C = 0, TA_RING_C2H = 1, TA_RING_TEL = 2, TA_RING_COUNT } ta_ring_id;

typedef struct {
    uint64_t head;      /* bytes ever committed; written by the producer only */
    uint64_t tail;      /* bytes ever released; written by the consumer only */
    uint64_t next_seq;  /* the seq the next record will carry, from 1 */
    uint32_t capacity;  /* bytes, a multiple of 8; 0: the ring does not exist yet */
    uint32_t offset;    /* where its bytes start, from the start of the control block */
} ta_ring_state;

typedef struct {
    uint32_t magic;
    uint32_t version;
    ta_ring_state ring[TA_RING_COUNT];
    uint64_t tel_dropped;   /* tel records dropped since creation */
} ta_control_block;

TA_API ta_control_block *ta_system_control_block(ta_system *system);
```

The control block has an entry for each of the three rings from the start.
`h2c` and `c2h` have capacity 0 until the ADR that introduces them
(ring-proto Phase 5.1); the pool counters of the proposal's control block
arrive with ADR 0099. The layout is versioned by `version`, so adding
fields later is not a change a reader can miss.

The ring is the C1 block in its lossy mode: one producer (core) and one
consumer (the host) on the system's own thread (ADR 0017), two `u64`
counters, records aligned to 8 bytes, an 8-byte pad record (`kind` 0, no
seq) where a record would not fit before the end of the buffer. A reader
uses two calls, or reads the control block directly:

```c
typedef struct { const uint8_t *bytes; uint32_t size; } ta_ring_record;
/* The next committed record after skipping pads; false when there is none. */
TA_API bool ta_ring_peek(const ta_system *system, ta_ring_id ring, TA_OUT ta_ring_record *out);
/* Releases `size` bytes, the record peek returned, so the producer can reuse them. */
TA_API void ta_ring_release(ta_system *system, ta_ring_id ring, uint32_t size);
```

Records are decoded with the readers the record codec generates from the
protocol catalog (block C2), which validate that every span lies inside its
record.

### 2. When `tel` is full, the record is dropped and counted

- A record that does not fit is not written. `tel_dropped` and a pending
  drop count go up by one, and **the record still consumes its seq**, so a
  gap in seq is exactly where records were lost.
- As soon as the ring has room again, and before the next `tel.trace`,
  core writes [`tel.dropped`](../../protocol/generated/MESSAGES.md#teldropped)
  with the count and the seq the first dropped record would have had. If
  `tel.dropped` itself does not fit, the next record is dropped too and the
  count grows.
- Nothing waits for `tel`, and nothing fails because of it. Telemetry
  never slows a macrostep or changes what it does.

### 3. A `tel.trace` record carries every field its kind names

[`tel.trace`](../../protocol/generated/MESSAGES.md#teltrace)
is one `ta_trace_record`: its `trace_kind`, and the fields that kind names,
as today. The state arrays (`exited`, `entered`, `history`, `states`) and
the transitions are packed into the tail, each counted.

The catalog's sketch of `tel.trace` (120 fixed bytes) is the start, not the
whole record. Every field the kernel's OTLP encoder reads from a
`ta_trace_record` today stays: the actor and definition, the definition's
identity for definition records (ADR 0080), the event's name, type,
origin, send ID and invoke ID, the send and invoke IDs of SENT, INVOKED and
FORWARDED records, the fault's code and message, `now`, and the expression
ID. The ones the sketch lacks are added as fixed fields or tail spans when
the catalog moves into core (step 2.9). **Rule: nothing the encoder reads
today is lost.** Records are built straight into their reservation in the
ring, so an enabled kind costs no copy.

### 4. Values are text

A record's value (an ASSIGN's new value, a CONDITION's result, a RAISED
event's data), a fault's data, and the data of the event a record names
are rendered by the data model's `value_text` when the record is written,
and carried as text, cut at `telemetryValueBytes`. A cut value sets the
record's `truncated` flag. No `tel` record ever holds a value reference.

Rendering happens only for kinds the macrostep's mask enables and only for
sampled records, as building them does today. It measures first, then
fills, with the text writer (block D3), so a value is rendered into the
ring slot at its final length. The data model's memory for rendering is its
own, not core's.

### 5. The masks stay, as configuration

The sink's three masks (ADR 0073) become configuration of the system, set
by one call. They are not parameters: ADR 0092 rule 1 already says a sink's
configuration is not.

```c
typedef struct { uint32_t kinds; uint32_t watched_kinds; uint32_t unsampled_kinds; } ta_trace_masks;
TA_API ta_result ta_system_set_trace_masks(ta_system *system, TA_IN const ta_trace_masks *masks);
```

A system starts with all three at zero: it writes nothing to `tel`, which
is what `ta_null_trace_sink()` gave it. A kind that is off still costs one
branch. Sampling, trace contexts (ADR 0073) and counters (ADR 0072) do not
change; counters are read through the API, never through `tel`.

### 6. Three parameters

Under ADR 0092's rules, set once at creation and reported by
`ta_param_info_of()`:

| Parameter | Default | Minimum | Maximum | Bounds |
|---|---|---|---|---|
| `telBytes` | 65,536 | `maxRecordBytes` | 2 GiB | the `tel` ring |
| `maxRecordBytes` | 4,096 | 256 | `telBytes` (later also `c2hBytes` / 4) | the largest record on any ring, header and tail included |
| `telemetryValueBytes` | 1,024 | 0 | `maxRecordBytes` − 256 | the text kept of one value |

- **`telBytes` defaults to 64 KiB, not the 256 KiB of the proposal's
  table.** The ring is fixed at creation, and a system pays for it whether
  it traces or not. 256 KiB would more than triple the 97 KB a default napi
  system costs since ADR 0093, for systems that, by default, trace nothing.
  64 KiB holds about 380 records of 170 bytes between drains, which covers
  a sampled turn. **A busy server that traces** sets 256 KiB to 1 MiB, and
  watches `tel_dropped`.
- **`telBytes` cannot be 0.** A host that never traces keeps the masks at
  zero and may set `telBytes` to `maxRecordBytes` (4 KiB by default). One
  ring that always exists is one code path; 4 KiB is not worth a second.
- **`telemetryValueBytes` replaces the kernel's `TAH_PARAM_VALUE_BYTES`,**
  with the same default.

### 7. What goes

- `ta_trace_sink`, `ta_trace_sink_vtable`, `ta_null_trace_sink()` and
  `ta_system_set_trace_sink()`, and the rule that a sink must not call back
  into the running actor: nothing is called.
- The kernel's trace adapter becomes a reader of `tel`: after each call
  into core it drains `tel` into the OTLP encoder.
  `TAH_PARAM_TRACE_STAGING_BYTES` becomes a setting of the encoder, since
  `telBytes` bounds the ring. `TAH_PARAM_VALUE_BYTES` goes.
- The test host's instruments read `tel` after each RPC call that runs
  core (`instrument.journal`), and napi's `tna_trace_dispatch` drains it
  once per pump turn. Only their call sites change in this step.

A `tel.dropped` inside an open span is reported by the encoder as an
attribute on the enclosing span, plus a counter. That is the simplest of
the answers [telemetry.md](../../../docs/proposals/ring-proto/reference/telemetry.md)
lists, and the encoder can do better later without a change in core.

## Consequences

- Telemetry never slows the system and never fails a chart's action. A
  slow exporter loses records, and says how many.
- **Order.** Records arrive in the order core wrote them, with a gap and a
  `tel.dropped` where records were lost. A record is visible only once core
  returns to the host; nothing runs host code in the middle of a
  macrostep for telemetry any more.
- **Memory:** per system, `telBytes` (64 KiB by default) and a control
  block of about 128 bytes, at creation. The kernel's staging buffer is no
  longer needed between core and the encoder. Per actor and per event:
  nothing.
- **Cost per record:** the value rendering the encoder did moves into
  core's write. For kinds that are off, nothing changes. Span and trace IDs
  stay identical; `span_test` and the kernel's OTLP tests check them.
- **Tests:** the ring block has its own tests (a wrap at every offset, a
  ring of exactly `maxRecordBytes`, drop counting). A core test fills
  `tel` and checks the seq gap, the count, and that `tel.dropped` comes
  before the next `tel.trace`. The trace tests read `tel` instead of
  installing a sink.
- **Forward references.** Replay's `suppressed` and `replaying` flags
  ([telemetry.md](../../../docs/proposals/ring-proto/reference/telemetry.md#replay-the-suppressed-flag))
  arrive with recording on the rings (Phase 5.5). The watched bit per actor
  replaces the kernel's watch list when the kernel goes (Phase 6). Where the
  OTLP encoder lives after the kernel is deleted is still open.
