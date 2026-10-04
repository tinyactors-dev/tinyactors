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
`error.execution`. There is no `TA_OP_NOMEM` and no pause for it, unlike
today's core. Why is in
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

- **h2c: commit transfers one retained reference to core.** Before commit,
  the host owns the provisional retain and releases it if posting fails.
  After commit, core releases the transferred reference exactly once,
  including when it bounces the request, discards a stale invocation reply,
  or frees, recycles or drops its envelope. A bounce returns status, not
  the reference. The host may keep a separate original reference in its
  bounded outstanding-request table if it wants to retry.
- **c2h: commit transfers one retained reference to the host.** Core
  releases provisional retains and copies if constructing a record fails.
  After commit the host releases the transferred reference when it has
  used the record: after the I/O processor has taken its own reference, or
  after the recorder has encoded it. Advancing the ring tail only releases
  record bytes; it does not release values.
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
([envelope.md](envelope.md)). ADR 0097 still applies: a send to an external
queue, including the sender's own queue, first calls `copy_out`. The
envelope holds that result as a `ta_value`; the receiver retains the box.
No encoding is required, but a mutable data model may execute its copier.
Invocation parameters and each autoforwarded event are copied too.

### Recorded input snapshots

Before taking a recorded input, and before installing `_event` or running
`<finalize>`, autoforwarding or chart code, core calls `copy_out` for its
data. The observation owns the result; the live event keeps the original
input. The snapshot must remain unchanged by chart execution. Immutable
data models can retain; mutable data models must isolate the snapshot.
Core allocates nothing for this operation; the adapter may allocate.

If copying fails, core takes no input and runs no chart code. An external
submission receives `c2h.input_refused`; local mail or a local delayed send
is recycled into a data-free `error.communication` for its sender, or
discarded if the sender no longer exists. The data-free error needs no
recording copy, so a failed copier cannot cause an endless exchange of
failed error snapshots. No partial `c2h.input` is committed. The host's
refusal report is bounded and contains no failed snapshot. See
[recording-and-replay.md](recording-and-replay.md#recording-and-effects-are-independent).

### The persistence boundary

A vref means nothing outside the process. Where a value must outlive the
process, the binding encodes it with its own codec:

- the recorder encodes the stable snapshot in `c2h.input.data` before it persists the record
  ([recording-and-replay.md](recording-and-replay.md));
- replay decodes it and posts a fresh vref in `h2c.event.data`.

Core plays no part in this. It never sees the encoded bytes.

The codec must preserve the snapshot isolation needed by the observation.
An adapter that shares mutable objects it cannot copy cannot promise a
stable recorded value for those objects; it must refuse that snapshot
rather than silently journal later mutations.

## Offload

An image is self-contained. When core writes one for
[`c2h.unloaded`](../protocol/generated/MESSAGES.md#c2hunloaded) or
[`c2h.state`](../protocol/generated/MESSAGES.md#c2hstate), it calls
`value_write` for the data model root, and for the data of every delayed
send the session holds in the pool. The image holds bytes, not vrefs.
Loading calls `value_read`.

So unloading does not have "two halves", one in core and one in the
JavaScript heap. It would have them only if core held references to values
that live elsewhere, as in the request-record data model
([below](#why-the-data-model-is-not-on-a-ring)).

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


## Why the data model is not on a ring

The alternative is a data model on the rings: every data model call
becomes a request record on c2h (`dm.assign`, `dm.eval`, `dm.foreach`,
...), and the macrostep suspends until the answer arrives on h2c. The data
model is instead a vtable that core calls synchronously, inside the
macrostep, for these reasons:

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
  does without ([macrostep.md](macrostep.md#what-this-replaces-the-out-of-memory-stop-adrs-0047-0070)).
- **Slots would not help.** Macrostep slots interleave sessions only
  between units of executable content
  ([macrostep.md](macrostep.md#macrostep-slots)). A slow answer arrives in
  the middle of an action, so it would still hold the thread unless every
  action could stop in the middle, which the point above rules out.
- **A data model in another process can still be served synchronously.**
  The binding's vtable may block on it. A system runs on one thread
  ([ring.md](ring.md#one-thread)), so blocking it is the binding's choice to
  make, and nothing in core changes.
