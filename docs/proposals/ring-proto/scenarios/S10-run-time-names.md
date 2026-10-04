# S10: Run-time names

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

The host maps `POST /campaigns/42/events/{name}` to an event, and a
hostile client posts a million random names. Today each one is interned
for good. Under the proposal each name is bytes in the name area for as
long as its envelope lives. Matching looks segments up and never inserts
them, so memory does not grow ([names](../reference/names.md)).

**Parameters:** `poolEnvelopes` 64, `admissionWatermark` 75% (48 envelopes),
`fairnessRatio` 8, **`nameAreaBytes` 4096**, `h2cBytes` 64 KiB,
`c2hBytes` 64 KiB, `telBytes` 256 KiB, `maxRecordBytes` 1024.

## Today

- `ta_event` carries a `ta_event_name_id`. So a name from outside is
  interned before delivery (`ta_event_name_intern`). So is every
  `eventexpr` result (`interpreter.c`, `ta_event_name_intern_dotted`),
  inside a macrostep.
- The atom table (`src/atom.h`: a 24-byte `ta_atom_entry` plus the text in
  an arena) and the event-name trie (`src/event_name.h`: a 16-byte
  `ta_event_name_node` per segment) only grow. Neither has a remove
  operation.
- A million random two-segment names cost on the order of 50 MB, counting
  hash buckets and vector doubling. That memory is kept until the system
  is destroyed. Each interning is also an allocation that can fail in the
  middle of a run.

## Under the proposal

| Step | Ring | Record | What happens |
|---|---|---|---|
| S10.1 | h2c | [`h2c.event`][h2c.event] `0x0102` `{campaign/42, name: "zq81f.x9"}` | The name is 8 bytes in the record's tail. |
| S10.2 | – | – | Admission. The 8 bytes are copied into the name area for the envelope. Nothing is looked up yet. **pool 1/64, names 8/4096 bytes.** |
| S10.3 | – | – | s1 takes it. Core looks up each segment in the atom table without inserting it ([names](../reference/names.md)). Neither `zq81f` nor `x9` is an atom, because no chart's descriptor mentions them, so both resolve to `UNKNOWN`. The matcher runs over the resolved segments `[UNKNOWN, UNKNOWN]` and checks every descriptor of the active states: `notify.ack`, `send.requested`, `progress.sync`, `error.communication`, … `UNKNOWN` matches only `*`, and the campaign has no `*` transition. |
| S10.4 | tel | [`tel.trace`][tel.trace] `0x0301` `{IGNORED, name: "zq81f.x9"}` | The event enables nothing. Telemetry carries the name as text. |
| S10.5 | c2h | [`c2h.macrostep_ended`][c2h.macrostep_ended] `0x0208` `{s1.m…, effects: 0}` | The envelope is freed, and so are its 8 bytes. **pool 0/64, names 0/4096.** |
| S10.6 | – | – | After a million such names, memory is the same as before the first one. |

## When the names come faster than they are freed

| Step | Ring | Record | What happens |
|---|---|---|---|
| S10.7 | h2c | many × [`h2c.event`][h2c.event] with 200-byte names | The host gates events when the name area passes its watermark. An already submitted event that cannot be admitted is consumed and bounced with `names`, allowing later controls to progress ([envelope](../reference/envelope.md#admission-from-h2c-into-the-pool)). The host settles the bounded outstanding request and answers HTTP 503 ([S09.5](S09-edge-backpressure.md#filling-up)). |
| S10.8 | c2h | [`c2h.bounced`][c2h.bounced] `0x020A` `{reason: invalid, detail: "name has 80 segments; eventNameDepth is 64"}` | A name deeper than `eventNameDepth` is handed back unrun. A name longer than fits in one record cannot be posted at all: `maxRecordBytes` bounds it at the host. |

## Names a chart computes

A chart may build a name, for example
`<send eventexpr="'order.' + id + '.paid'" targetexpr="shop"/>`.

- The text goes into the name area with the mail's envelope.
- If the name area is full, the send fails as
  `error.execution.capacity.names`, in the sender's macrostep, like a full
  pool ([S02.5](S02-full-pool.md#the-brutal-chart)).
- The receiver's descriptor `order.*.paid` is three authored segments.
  `order` and `paid` were interned when its definition was compiled.
- The event resolves to `[order, UNKNOWN, paid]`, and the two-pointer
  wildcard matcher matches it. `order.124` does not match.
  `order.123.paid` never became an atom.

## Who drops what, who retries how

| What was dropped | Dropped by | Told how | Retried by |
|---|---|---|---|
| A random name nobody listens for | the chart: it enables nothing (S10.4) | [`tel.trace`][tel.trace] `IGNORED` | nobody |
| Clicks while the name area is past its watermark | the host, at its edge (S10.7) | HTTP 503 | the client |
| A name deeper than `eventNameDepth` | core (S10.8) | [`c2h.bounced`][c2h.bounced] | nobody: it is invalid |
| A computed name with no room | core, in the sender's macrostep | `error.execution.capacity.names` | the sending chart |

[h2c.time]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2ctime
[h2c.event]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cevent
[h2c.spawn]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cspawn
[h2c.publish]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cpublish
[h2c.load]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload
[h2c.load_failed]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload_failed
[h2c.unload]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cunload
[h2c.checkpoint]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2ccheckpoint
[h2c.invoke_report]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cinvoke_report
[h2c.destroy]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cdestroy
[c2h.spawned]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hspawned
[c2h.input]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput
[c2h.send]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hsend
[c2h.invoke]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinvoke
[c2h.invoke_cancel]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinvoke_cancel
[c2h.invoke_forward]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinvoke_forward
[c2h.log]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hlog
[c2h.macrostep_ended]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hmacrostep_ended
[c2h.wake]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hwake
[c2h.bounced]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hbounced
[c2h.load_needed]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hload_needed
[c2h.loaded]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hloaded
[c2h.unloaded]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hunloaded
[c2h.state]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hstate
[c2h.published]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hpublished
[c2h.finished]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hfinished
[c2h.destroyed]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hdestroyed
[tel.trace]: ../../../../tinyactors-core/src/generated/MESSAGES.md#teltrace
[tel.dropped]: ../../../../tinyactors-core/src/generated/MESSAGES.md#teldropped
