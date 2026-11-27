# Snapshot requests: reading a pack with no host

A snapshot reader (`ta_host_snapshot_reader_open`, `include/tinyactors/host.h`)
answers requests about a pack (`ta_host_snapshot_pack`'s bytes): every
actor's state, mailbox and delayed sends at the moment it was taken, its
charts, and the spans its journals kept. Nothing runs and nothing changes.
A binding wraps the three calls in one object; a tool, such as an MCP
server, forwards its questions as these requests instead of deriving the
answers itself.

## The calls

1. `ta_host_snapshot_reader_size(pack)`: the bytes a reader needs (0: not
   an 8-aligned pack of this build).
2. `ta_host_snapshot_reader_open(&input, &reader)`: the reader made in
   `input.memory`, a block of the caller's memory of that size, 8-aligned.
   It copies the pack and reads each live segment's region into the block
   (`ta_system_read`), which holds pointers into itself: keep it where it
   is, and free it when done. The reader keeps no other memory.
3. `ta_host_snapshot_reader_request(reader, &request, &reply)`: one message,
   answered at the call into the caller's `ta_buffer`.
   `TA_ERROR_NO_MEMORY` with `reply.length` set: the reply needs that many
   bytes, and nothing was written; ask again with that room.
   `request.allocator` lends the call's working memory (rows to sort, the
   trace history, counts), all of it given back before the call returns.

## The envelope

The control plane's ([requests.md, The envelope](requests.md#the-envelope)):
`{"id", "method", "params"}` in CBOR; `{"id", "result"}` or `{"id",
"error": {"code", "message", "data": {"reason", "path"}}}` out, with the
same codes and reasons. A message without `id` is a notification: nothing
is written. A message is decoded into at most `TA_HOST_REQUEST_TOKENS`
items. Keys come in the order shown here.

Errors every method shares: `unknown` (a param the method does not take),
`type`, `missing`, `range`, `memory` (the request's allocator refused).

## A segment at a time: core's readers

Each takes `segment` (uint, required: a segment's index; `range` when the
snapshot has no live segment of that index). Those that read an actor take
`handle` too (uint, required: from `snapshot.handles`; `stale` when no actor
has it).

| Method | Params | Result |
|---|---|---|
| `snapshot.header` | none | `{"systemID", "datamodel", "time", "segments", "state"}`: the pack's system id and datamodels' label, the host's time in ticks, the number of segments (live and retired), the datamodels' state as packed (bytes) |
| `snapshot.stats` | `segment` | `{"now", "ticksPerSecond", "nextDeadline", "actors", "hibernated", "runnable", "events", "timers", "requests", "regionBytes", "wallNanoseconds"}` (`ta_system_stats`) |
| `snapshot.handles` | `segment` | the actors' handles, in the system's order |
| `snapshot.actor` | `segment`, `handle` | `{"session", "name", "chart", "chartName", "status", "parent", "mailbox", "invocations", "services"}`: `name` and `parent` (a session) `null` when it has none; `chart` the chart's index in the image; `status` one of `free`, `idle`, `runnable`, `waitingWorkspace`, `waitingAck`, `done`, `hibernated` |
| `snapshot.configuration` | `segment`, `handle` | its active states' indexes in its chart |
| `snapshot.mailbox` | `segment`, `handle` | `[{"event", "sendid"}]`, in order (`sendid` `null` when none) |
| `snapshot.timers` | `segment`, `handle` | `[{"due", "event", "sendid"}]`, earliest first, `due` in the segment's ticks |
| `snapshot.charts` | `segment` | each chart of the image: `{"name", "datamodel", "states": [{"id", "kind", "parent"}], "transitions": [{"source", "type", "events", "cond", "targets"}], "data": [id]}`, states and targets by index, `kind` one of `atomic`, `compound`, `parallel`, `final`, `shallowHistory`, `deepHistory`, `type` one of `external`, `internal`, `initial` |
| `snapshot.traceKept` | `segment` | `{"records", "acknowledgedRecords", "bytes", "acknowledgedBytes", "lost"}`, or `null` when the segment does not trace (`ta_system_trace_kept`) |
| `snapshot.traces` | `segment` | the kept records' spans as the bytes of an ExportTraceServiceRequest (`ta_host_trace_history`), or `null` when the segment does not trace |

## The whole snapshot: what a tool asks

| Method | Params | Result |
|---|---|---|
| `snapshot.summary` | none | `{"systemID", "time", "segments": [{"segment", "live", "charts", "actors"}], "states", "traces"}`: a retired segment has only `segment` and `live`; `states` is `{chart: {rest: actors}}`, where `rest` is an actor's leaf states joined by `+`, or `(hibernated)`; `traces` is `{"records", "acknowledgedRecords", "bytes", "lost"}` summed over the segments that trace, or `null` |
| `snapshot.actors` | `chart`, `state`, `status`, `name` (text, each optional), `offset` (uint, 0), `limit` (uint 0–512, 25) | `{"total", "offset", "actors"}`: the actors of every live segment whose chart is named `chart`, one of whose active states is `state`, whose status is `status` and whose name starts with `name`, ordered by session; `actors` the `limit` rows from `offset`, each `{"session", "name", "chart", "segment", "status", "states", "mailbox", "queued", "delayed"}`: `states` its leaf states' ids, `queued` its queued events' names, `delayed` `[{"event", "sendid", "dueInMs"}]`, milliseconds from the segment's time |
| `snapshot.chart` | `chart` (text, required) | the chart in the newest live segment that holds it (`stale` when none): `{"segment", "datamodel", "data", "states": [{"id", "kind", "parent", "actors"}], "transitions": [{"from", "events", "cond", "to"}]}`, by state ids, `actors` the actors of that segment in each state, the root and initial transitions left out |
| `snapshot.spans` | `session` (uint, optional), `name` (text, optional: a prefix), `limit` (uint 0–512, 40) | `{"total", "retained", "spans"}`: the spans of the trace history of every segment that traces, a session's when `session` is given (its `scxml.session_id` attribute) and those whose name starts with `name`, the last `limit` of them, oldest first, each as `trace.spans.decode` writes it ([requests.md](requests.md#tracespansdecode)); `retained` as `snapshot.summary`'s `traces`. `state` when no segment traces |

The spans' times and names are `ta_host_trace_history`'s
([embedding.md, Reading a snapshot](embedding.md#12--reading-a-snapshot)):
wall time, and of a record already exported, no name or value given at
run time.

## Not here

- **The variables:** they are the datamodel's state. Restore the snapshot
  and ask `actor.context`.
- **Another build's pack:** a region is raw memory of one build.
