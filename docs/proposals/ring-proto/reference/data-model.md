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
`error.execution`. `TA_OP_NOMEM` and the pause it caused are removed.
Why is in
[macrostep.md](macrostep.md#what-this-replaces-the-out-of-memory-stop-adrs-0047-0070).

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
the JavaScript heap. That concern applied only to the rejected option (b)
([below](#decided-the-data-model-stays-a-synchronous-vtable)), where core
would hold references to values that live elsewhere.

## Replay derives new state

Replay evaluates the supplied inputs under the chosen definition and the
new execution's resource conditions. It derives configuration and data
model values before publish. Those values may differ from an earlier run:

- a JavaScript `<script>` that calls `Date.now()` or `Math.random()` is
  not deterministic;
- the data model may forbid such calls, record them, or accept the
  divergence;
- capacity failures, budgets, ring draining and effect suppression may
  also change which actions run, even with a deterministic data model.

Reproducing the earlier state is not the replay contract.
[recording-and-replay.md](recording-and-replay.md) describes how a host
can detect a divergence with telemetry.

## Decided: the data model stays a synchronous vtable

An earlier iteration of the design kept a second option open, "option (b)":
every data model call would become a request record on c2h (`dm.assign`,
`dm.eval`, `dm.foreach`, ...), and the macrostep would suspend until the
answer arrived on h2c. **It is rejected.** The data model stays a vtable
that core calls synchronously, inside the macrostep.

**Why:**

- **In-process data models gain nothing from it.** napi, the test host, and
  the SQLite and PostgreSQL sketches (core runs inside the database
  connection) all call their data model synchronously. On a ring, each call
  would cost a write, a host turn and a read, about four per iteration of
  the [S01](../scenarios/S01-fanout.md) fanout, to do what a function call
  does.
- **It would make every action resumable in the middle.** ADR 0061
  continuations suspend *between* units of executable content, "never while
  an action runs". With requests on a ring, every `<assign>`, `<send>` and
  `<foreach>` would split into an issue stage and a completion stage. The
  continuation would have to hold the half-done action and the results it
  already evaluated, so that resuming never evaluates them twice. That
  means more state in the continuation, more code in every action, and the
  ADR 0047/0070 kind of reasoning about interrupted steps that this design
  removes ([macrostep.md](macrostep.md#what-this-replaces-the-out-of-memory-stop-adrs-0047-0070)).
- **It would undo run to completion.** A slow answer would block the whole
  system, unless core went back to interleaving sessions, with one
  continuation for each.
- **A data model in another process can still be served synchronously.**
  The binding's vtable may block on it. A system runs on one thread
  ([ring.md](ring.md#one-thread)), so blocking it is the binding's choice to
  make, and nothing in core changes.
