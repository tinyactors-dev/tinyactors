# The rings: shared memory between core and its host

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

A ring is how core and its host hand each other records without a call per
record. There are three: [`h2c`](#the-three-rings) from the host to core,
[`c2h`](#the-three-rings) and [`tel`](#the-three-rings) from core to the
host. This file describes how one ring works, then why there are exactly
three. The records themselves are defined in
[`protocol/catalog.rb`](../protocol/catalog.rb) and listed in
[MESSAGES.md](../protocol/generated/MESSAGES.md).

## What a ring is

- **One producer and one consumer, on one thread.** Each ring has exactly
  one writer and one reader, and both run on the system's own thread
  ([One thread](#one-thread)). A ring needs no lock, no compare-and-swap and
  no atomics. Its state is two counters.
- **Bytes, not slots.** A ring is a byte buffer of `h2cBytes`, `c2hBytes`
  or `telBytes`. Records have different sizes, so a ring holds many small
  records or a few large ones.
- **Core allocates it when the system is created**, from the system's
  allocator (core ADR 0023), like every other buffer (core ADR 0092).
  Nothing changes size after creation.
- **It is not a call.** Writing a record does not run the reader. The reader
  looks when it is next its turn: core when the host calls `ta_run`, the host
  when `ta_run` returns.

This is the same idea as the trace sink (`ta_trace_sink`, core ADR 0067),
with one change: the sink was a callback per record, and a ring is a buffer
the reader drains when it chooses.

## The control block

Core creates one control block per system, next to the rings. The host
reads it; only the noted side writes each field.

| Field | Written by | Meaning |
|---|---|---|
| `magic`, `version` | core, at creation | identify the layout |
| `ring[r].capacity` | core, at creation | bytes in ring `r`, a multiple of 8 |
| `ring[r].head` | the producer | bytes ever committed (a `u64` that only grows) |
| `ring[r].tail` | the consumer | bytes ever released |
| `ring[r].next_seq` | the producer | the seq the next record will carry |
| `tel.dropped` | core | telemetry records dropped since creation |
| `pool.envelopes`, `pool.free`, `pool.waiting`, `pool.high_water` | core | the envelope pool ([envelope.md](envelope.md)) |
| `names.bytes`, `names.used` | core | the name area ([names.md](names.md)) |
| `admitting` | core | whether core is moving h2c events into the pool now (pool below `admissionWatermark`); definition requests use no envelope |

The usage counters exist so a host can make decisions at its edge without
asking core. For example, it can answer HTTP 503 while `admitting` is
false, before h2c is even full.

The control block is a few hundred bytes per system.

## Records

Every record has the 32-byte common header from
[`protocol/dsl.rb`](../protocol/dsl.rb):

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `kind` | u16 | the message code, such as `0x0208` for [`c2h.macrostep_ended`](../protocol/generated/MESSAGES.md#c2hmacrostep_ended) |
| 2 | `flags` | u16 | the message's flags (bits 0..7) and the common flags `replaying`, `recorded` (bits 8..15) |
| 4 | `size` | u32 | the whole record, header and tail, a multiple of 8 |
| 8 | `seq` | u64 | the record's sequence number in this ring |
| 16 | `session` | u64 | the session it is about, or 0 |
| 24 | `at` | u64 | the system clock when it was written; on h2c, the input's time |

After the header come the message's fixed fields, each at its natural
alignment, then the **tail**. A `text` or `bytes` field is a span: a `u32`
offset from the start of the record and a `u32` length, pointing into the
tail. A record's size is rounded up to 8 bytes, so every record starts
8-byte aligned.

- **The largest record is `maxRecordBytes`.** A producer never writes a
  bigger one. On c2h, an action whose record would be bigger fails as
  `error.execution.capacity` ([macrostep.md](macrostep.md)). On h2c, core
  bounces a bigger record with
  [`c2h.bounced`](../protocol/generated/MESSAGES.md#c2hbounced)
  (`invalid`), and the host should never write one. On tel, a too-long
  `value_text` is cut and flagged `truncated`.
- **A reader ignores a kind it does not know**, by skipping `size` bytes.
  So a newer core can add a message to a ring without breaking an older
  reader's framing. Whether the reader may ignore it is a protocol version
  question, and `version` in the control block answers it.

## Writing: reserve, fill, commit

The producer works on a private write position, then publishes it:

1. **Reserve.** Round the record's size up to 8. If
   `capacity - (head - tail)` is smaller than that, the ring is full (see
   [When a ring is full](#when-a-ring-is-full)).
2. **Wrap if needed.** If the record does not fit between the write
   position and the end of the buffer, write a **pad** there (below), and
   reserve from the start of the buffer instead. Both the pad and the
   record must fit.
3. **Fill** the header, the fixed fields and the tail in place. Nothing is
   visible to the reader yet.
4. **Commit.** Store the new `head`. The reader can now see the record.

A producer may commit after every record, or after several. Core commits
c2h after every record, so a host whose `ta_run` returned on a budget pause
can execute a long macrostep's effects and release their ring space before
its macrostep end. The macrostep end reports completion; it does not hold effects
([macrostep.md](macrostep.md#the-macrostep-end)).

### A record never splits across the end: the pad

A record is always contiguous in memory. When the space left before the end
of the buffer is too small, the producer writes a pad covering it and
continues at offset 0.

**A pad is an 8-byte prefix:** `kind = 0x0000`, `flags = 0`, `size` = the
bytes to the end of the buffer. It carries no seq. The reader reads the
first 8 bytes of every record (`kind` and `size`) before anything else. If
`kind` is 0, it skips `size` bytes and reads on.

Why a pad and not the other choices:

| Choice | Why not |
|---|---|
| A skip marker, a separate flag meaning "the rest is unused" | It is a pad under another name. It needs the same 8 bytes, and the reader needs a second rule. |
| A full pad record with a 32-byte header | It does not fit when fewer than 32 bytes are left. Every record is aligned to 8, so 8 bytes always fit. |
| Splitting a record across the end | Every reader and writer would have to copy, and no field could be read in place. |
| Mapping the buffer twice in virtual memory so the end runs into the start | Not portable. Core runs on hosts with no virtual memory: wasm, an MCU. |

The cost is at most `maxRecordBytes - 8` bytes left unused per wrap.

## Reading: look, use, release

1. **Look.** Read `head`. Everything between the read position and `head`
   is committed records.
2. **Use.** Read the record in place. A host that keeps a record past
   release copies it. That includes the bytes its spans point to.
3. **Release.** Store the new `tail`. The producer may now overwrite those
   bytes.

A reader may release a batch at once. Releasing is what makes room, so a
host that holds records unreleased keeps the ring full.

## Sequence numbers

- **Each ring numbers its records from 1.** Pads are not numbered.
- **On a lossless ring (h2c, c2h), seq has no gaps.** A reader that sees
  seq jump has a bug or a corrupted ring. Core treats an h2c gap as
  `invalid` and stops reading h2c.
- **On tel, a dropped record still takes its number.** The gap is how a
  reader knows records were lost, and
  [`tel.dropped`](../protocol/generated/MESSAGES.md#teldropped) says how
  many and from which seq.
- **Other records cite h2c seq numbers.** For example `source_seq` in
  [`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput) and
  [`c2h.bounced`](../protocol/generated/MESSAGES.md#c2hbounced), and
  `cause_seq` in
  [`c2h.spawned`](../protocol/generated/MESSAGES.md#c2hspawned). So the
  host writes h2c seq numbers and core checks them.

## When a ring is full

What happens depends on the ring, and that difference is the reason there
are three:

| Ring | Producer | When full | Who retries |
|---|---|---|---|
| `h2c` | host | The host cannot post. It refuses at its edge (HTTP 503, or it stops reading a socket), or keeps the input in its own memory. Core never waits on h2c. | The external client, or the host by its own policy |
| `c2h` | core | An action whose record does not fit fails as `error.execution.capacity.c2h`, and the rest of its block is abandoned. A run never waits. | The sending chart |
| `tel` | core | The record is dropped and counted. The next [`tel.dropped`](../protocol/generated/MESSAGES.md#teldropped) reports it. | Nobody: telemetry is allowed to be incomplete |

### The c2h reserve

Some c2h records are not written by an action, so no chart could be told
when they don't fit:
[`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput) for a recorded
session, and the [`c2h.macrostep_ended`](../protocol/generated/MESSAGES.md#c2hmacrostep_ended)
that ends every macrostep. So:

> **Core starts a macrostep only when c2h has room for its `c2h.input`, its
> `c2h.macrostep_ended`, and one record of `maxRecordBytes`.**

The last part is for the action that finds c2h full: its failure becomes
`error.execution.capacity.c2h` in the internal queue, and the macrostep still
reaches its macrostep end. Every action that writes to c2h first checks that the
room left after its own record still holds the macrostep end. So an action can
fail, but the macrostep end can't.

When the reserve isn't there, core takes no envelope and `ta_run` returns.
The host drains c2h and calls `ta_run` again. Records outside a macrostep
work the same way: [`c2h.bounced`](../protocol/generated/MESSAGES.md#c2hbounced),
[`c2h.load_needed`](../protocol/generated/MESSAGES.md#c2hload_needed),
[`c2h.wake`](../protocol/generated/MESSAGES.md#c2hwake). If c2h has no room
for one, core doesn't consume the h2c record or event that would cause it.

Definition requests follow this rule too: core reserves their fixed
[`c2h.definition_result`](../protocol/generated/MESSAGES.md#c2hdefinition_result)
before consuming the request, compiling or changing definition state.
They run between macrosteps without an envelope; compile diagnostics use
a bounded host-owned buffer ([definitions.md](definitions.md)).

## One thread

An attached debugger temporarily replaces the production driver on this
same thread. It owns the system and its rings exclusively until detachment;
debugger commands use the host's debugger connection, not an operator ring
([debugging.md](debugging.md)).

**A `ta_system` is not thread-safe, and neither are its rings.** Core and
the host take turns on the system's own thread. The host writes h2c, calls
`ta_run(budget)`, and reads c2h and tel when it returns. Core calls the data
model through its vtable during the run, on the same thread
([data-model.md](data-model.md)).

A host that does work on other threads (a C server with an I/O thread pool,
a Node worker) puts **its own queue or channel in front of the system**.
Other threads hand inputs to the system's thread through it, and that
thread alone touches the rings. Separate systems may run on separate
threads, since they share nothing.

**Why one owner thread:**

- **No atomics, no memory ordering, no cache-line padding.** The counters
  are plain loads and stores. The same code runs unchanged on an MCU, in
  wasm without threads, and in Node.
- **The data model is single-threaded anyway.** JavaScript must be called
  from the thread that owns it, so napi could never have run core and its
  data model apart.
- **The hand-off a threaded host needs already exists in its runtime**
  (a channel, a libuv async handle, an MPSC queue). That is where policy
  about blocking, batching and backpressure between threads belongs, and
  it is the host's policy.
- **Reading across processes is not offered.** vrefs and blobs name the
  binding's tables in core's process, so a record means little outside it.
  A tool that wants to watch a system goes through the host, or through
  the test host's protocol.

## Values and buffers across a ring

A record carries text and bytes in its tail. Anything bigger or owned by
the binding crosses as a reference:

| Reference | What it names | Who owns it on each ring |
|---|---|---|
| `vref` | a slot in the binding's value table: a JS object, a `jv`, an SQL row ([data-model.md](data-model.md)) | On **c2h**, core retained it for the host, and the host releases it when done with the record. On **h2c**, the host retained it for core, and core releases it when the envelope holding it is freed ([envelope.md](envelope.md)). **Never on tel:** a lossy ring would leak the references it drops, so tel carries `value_text`, cut at `telemetryValueBytes`. |
| `blob` | a host-owned byte buffer, read or written through core's byte source and sink (core ADR 0090) | The host. Core writes images into a sink the host provides ([`c2h.unloaded`](../protocol/generated/MESSAGES.md#c2hunloaded), [`c2h.state`](../protocol/generated/MESSAGES.md#c2hstate)) and reads them from a source ([`h2c.load`](../protocol/generated/MESSAGES.md#h2cload)). The ring carries only the handle. |

So a ring's bytes are always small and fixed. A 10 MB image or a large JS
object never passes through it.

## The three rings

| Ring | Direction | Guarantee | Carries |
|---|---|---|---|
| `h2c` | host → core | lossless, gap-free; full means the host refuses at its edge | every input: [`h2c.event`](../protocol/generated/MESSAGES.md#h2cevent), [`h2c.time`](../protocol/generated/MESSAGES.md#h2ctime), [`h2c.spawn`](../protocol/generated/MESSAGES.md#h2cspawn), [`h2c.load`](../protocol/generated/MESSAGES.md#h2cload), [`h2c.invoke_report`](../protocol/generated/MESSAGES.md#h2cinvoke_report), ... |
| `c2h` | core → host | lossless, gap-free, ordered with macrostep ends; full means the action fails | effects, macrostep ends, bounces, load requests, images, and the inputs of recorded sessions |
| `tel` | core → host | lossy; full means drop and count | trace records for observability ([telemetry.md](telemetry.md)) |

**There are exactly three because there are three delivery guarantees, not
three purposes.** Splitting by purpose instead was considered and rejected:

- **Recording on a ring of its own** would add another lossless core-to-host
  stream without a new delivery guarantee. c2h already carries input
  observations, effects and completion reports in order. That order lets
  the host relate them; it imposes no persistence-before-effects rule
  ([recording-and-replay.md](recording-and-replay.md)).
- **Recording on tel**, "replay is driven by tracing", fails because tel
  drops. A replay log that can lose an input isn't one.
- **Telemetry on c2h** would let a slow OTLP exporter fail a chart's sends.
- **Mail between local sessions on a ring** was rejected. It deadlocks when
  the ring the running session writes to is the ring only that run can
  drain. Mail lives in the envelope pool instead ([envelope.md](envelope.md)).

A fourth ring would need a fourth guarantee. None has come up.
