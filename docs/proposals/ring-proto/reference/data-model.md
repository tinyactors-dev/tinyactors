# The data model and value references

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

The data model does not move onto a ring. Core keeps calling
`ta_datamodel_vtable` synchronously, inside the macrostep, as it does
today. Only the data model's *results* cross the rings, and they cross as
value references.

## What stays as it is

| Today | In this design |
|---|---|
| Core calls `eval_condition`, `eval_value`, `read_location`, `assign`, `execute_script`, `declare`, `set_named`, `foreach_snapshot`/`foreach_bind`, `import_content`, `value_text`/`text_value` inside a microstep | Unchanged |
| napi runs each call in JavaScript (`tinyactors-napi/src/napi_datamodel.c`, `tna_dm_*` through `napi_call_function`) | Unchanged |
| A thrown exception becomes `TA_OP_ERROR` and then `error.execution` | Unchanged |
| A data model call may query `ta_dm_host_*` (`In()`, `_event`, time) | Unchanged |
| `value_write`/`value_read` encode a value into an image (core ADR 0074) | Unchanged, so images stay self-contained (see [Offload](#offload)) |

The data model's own memory is not core's memory. A JavaScript `assign`
may allocate as much as it likes: the rule that a macrostep never
allocates ([macrostep.md](macrostep.md)) is about core's allocator. A data
model that runs out of memory answers `TA_OP_ERROR`, and the chart sees
`error.execution`, as today.

## Value references

A **vref** is a `u64` slot number in the binding's value table. It is the
wire form of a `ta_value` whose payload the binding owns. In napi it is the
index of a value box (napi ADR 0011: a box holds one `napi_ref` and a
native reference count). 0 means "no value".

Core never sees the bytes of a JavaScript object. A send's data, an
event's `_event.data`, a log's value and done data cross the rings as
vrefs:

| Record | vref fields |
|---|---|
| [`h2c.event`](../protocol/generated/MESSAGES.md#h2cevent) | `data` |
| [`h2c.spawn`](../protocol/generated/MESSAGES.md#h2cspawn) | `data` (the data model binding) |
| [`h2c.invoke_report`](../protocol/generated/MESSAGES.md#h2cinvoke_report) | `data` |
| [`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput) | `data` |
| [`c2h.send`](../protocol/generated/MESSAGES.md#c2hsend) | `data`, `content` |
| [`c2h.invoke`](../protocol/generated/MESSAGES.md#c2hinvoke), [`c2h.invoke_forward`](../protocol/generated/MESSAGES.md#c2hinvoke_forward) | `data` |
| [`c2h.log`](../protocol/generated/MESSAGES.md#c2hlog) | `value` |
| [`c2h.finished`](../protocol/generated/MESSAGES.md#c2hfinished) | `donedata` |

### Who owns a vref

The rule follows the direction of the ring:

- **h2c: the host retains, core releases.** A vref in an h2c record is a
  reference the host retained for core. The envelope that holds the input
  owns it ([envelope.md](envelope.md)). Core releases it when the
  envelope is freed: after the macrostep that took it, or when the
  envelope is recycled or dropped.
- **c2h: core retains, the host releases.** A vref in a c2h record is a
  reference core retained for the host. The host releases it when it has
  used the record: after the I/O processor has the value, or after the
  recorder has encoded it.
- **tel never carries vrefs.** A lossy ring cannot hand over ownership,
  because a dropped record would leak its reference. Telemetry carries
  `value_text` instead ([telemetry.md](telemetry.md)).

Retain and release are the data model's `ta_value_vtable` operations, as
today. A retain in napi is a counter increment on an existing box, so it
does not allocate (core ADR 0050, ADR 0097). A data model whose values
are mutable may need `copy_out` when data leaves a session (ADR 0097).
That copy is the data model's allocation, not core's.

### Mail between local sessions

Mail between two sessions of the same system never crosses a ring
([envelope.md](envelope.md)). Its data stays a `ta_value` in the
envelope: the receiving session's `_event.data` is the same box,
retained. No encoding happens and no JavaScript runs to move it.

### The persistence boundary

A vref means nothing outside the process. Where a value must outlive the
process, the binding encodes it with its own codec:

- the recorder encodes `c2h.input.data` before it persists the record
  ([recording-and-replay.md](recording-and-replay.md));
- replay decodes it and posts a fresh vref in `h2c.event.data`.

Core plays no part in this. It never sees the encoded bytes.

## Offload

An image is self-contained. When core writes one for
[`c2h.unloaded`](../protocol/generated/MESSAGES.md#c2hunloaded) or
[`c2h.state`](../protocol/generated/MESSAGES.md#c2hstate), it calls
`value_write` for the data model root, and for the data of every delayed
send the session holds in the pool. The image holds bytes, not vrefs.
Loading calls `value_read`.

This is why unloading does not have "two halves", one in core and one in
the JavaScript heap. That concern applied only to option (b), below,
where core would hold references to values that live elsewhere.

## Determinism is the data model's business

Replay gives the data model the same calls, in the same order, with the
same inputs. Whether that yields the same state depends on the data model
alone:

- a JavaScript `<script>` that calls `Date.now()` or `Math.random()` is
  not deterministic;
- the data model may forbid such calls, record them, or accept the
  divergence.

Core makes no promise beyond "the same inputs, in the same order".
[recording-and-replay.md](recording-and-replay.md) describes how a host
can detect a divergence with telemetry.

## Option (b): data model requests on the ring

This option stays open, and nothing here builds it.

**What it is.** Each data model call becomes a request record on c2h
(`dm.assign`, `dm.eval`, `dm.foreach`, ...). The macrostep suspends until
the matching answer arrives on h2c. The data model could then live in
another process, or behind an asynchronous API.

**What it would need:**

1. **Suspension inside an action.** ADR 0061 continuations suspend
   *between* units of executable content, "never while an action runs".
   An `<assign>` is one unit, and its data model call is inside it. Each
   action would have to split into an issue stage and a completion stage,
   with the continuation holding the half-done action.
2. **ADR 0047 and ADR 0070 still hold.** A step that is interrupted must
   have had no effect that doing it again would repeat. A suspended
   `<send>` that has evaluated its `eventexpr` but not yet written its
   record must not evaluate it again on resume. The continuation would
   have to keep the evaluated parts. It is reserved before the run, so
   it would have to be sized for the largest action's intermediate
   results.
3. **One continuation per suspended session.** Under run to completion
   ([macrostep.md](macrostep.md)) one continuation per system is enough,
   because nothing else runs while a macrostep is paused. A data model
   that answers slowly would block the whole system, unless core goes
   back to interleaving, which the pool design gave up.
4. **A round trip per expression.** The fanout in
   [S01](../scenarios/S01-fanout.md) makes about four data model calls
   per iteration. On a ring, each is a write, a host wakeup and a read.

**When it would pay off.** Only when the data model cannot be called
synchronously:

- it is out of process, such as a database server for the
  [PostgreSQL host](../../postgres-host.md);
- it is behind an asynchronous API;
- a WebAssembly core talks to a data model on another thread.

The record format keeps the door open. A data model request would be one
more message kind (code range `0x04xx` is free). An in-process binding
could answer it inline, which is what the vtable already does.

**Open:** whether the [SQLite host](../../sqlite-host.md) needs this. It
does not if SQL runs on the same connection, synchronously, as that
sketch assumes.
