# ADR 0020: Core reads snapshots: a restored host that never runs

**Status:** Proposed (2026-11-26), from the maintainer's request to move the
snapshot decoder into core ("move the CLI's snapshot decoder into core
proper and then have the CLI use that"; "move the decoder into core
next"). Revised the same day after a refutation. The refutation corrected:
- `advance_time` cannot refuse;
- a run must stop before the encoder drains;
- `sweep`, `traces` and `logs` change a host;
- a restore adds images;
- bytes the host writes are C calls, not methods;
- what the MCP server's tools need.

**Date:** 2026-11-26

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the operator, through a tool that reads a snapshot.** The
  tools are the MCP server (`@tinyactors/mcp`), `tinyactors inspect` and
  `tinyactors actors`. The operator asks what every actor was doing at
  one moment:
  - its state, its mailbox, its delayed sends and its variables;
  - the spans before that moment;
  - the versions running and their charts.

  Today the CLI answers with code of its own
  (`tools/tinyactors/snapshot.c`). The MCP server answers with a copy of
  the website Inspector's JavaScript decoder (`tinyactors-napi/mcp/lib/snapshot.mjs`),
  because a binding can only restore a snapshot into a host that runs: its
  queued events run at the first turn, so the moment is gone before the
  first question.
- **Then: the binding author,** who should not write a decoder.
- **Out of scope:** reading a snapshot of another build. The website
  Inspector does that in JavaScript, from the layout description the pack
  carries. Core's region is raw memory of one build, and a restore refuses
  another layout (`ta_host_restore`). The Inspector keeps its decoder.
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event | one test of a host flag in each input call (spawn, send, terminate, ...), beside its `running` check; nothing in a run |
  | per actor, memory | nothing |
  | per call | `ta_host_trace_history` lays out an OTLP encoder for the call, its staging sized to the journal's kept bytes, through the host's allocator, freed before it returns |
  | API | a flag on `ta_host_snapshot_input` (its `pad` becomes `flags`); one C call, `ta_host_trace_history`; one method, `chart.describe`; new keys in `actor.get`, `actor.list` and `host.stats` |
  | code | the CLI's do-nothing datamodels move into the host; an OTLP encoding of the journal's kept records |
  | bindings | a `frozen` option on `Host.restore`; `traceHistory()` (a host output, as `metrics()`); one typed method, `chartDescribe` |

## Context

A snapshot (`ta_host_snapshot_pack`) holds three things: every segment's
region, its images, and the datamodels' state. What it says about actors
is read in three ways today:

1. **The CLI** (`tools/tinyactors/snapshot.c`, `restore`):
   - unpacks the pack;
   - binds each datamodel its images name to a vtable with only its size
     and name set (`bind_name`);
   - restores a host that it never runs;
   - reads the host with the system's inspection readers:
     `ta_system_actors`, `ta_actor_info_get`, `ta_actor_config`, and the
     internal `ta_system_actor_mailbox`, `_timers` and `_name`.
2. **The website Inspector** (`website/examples/inspector/decode.mjs`)
   reads the bytes in JavaScript through the pack's layout description.
   That includes the trace journal's kept records, which it turns into
   spans the way core's OTLP encoder names them.
3. **A binding** restores with `Host.restore`, which runs. The MCP server
   gets variables from such a copy by posting every `actor.context`
   before its first await. It reads everything else with the copied
   Inspector decoder.

Nothing restores without running. Posts are answered without a run:
`ta_host_effects` hands out REPLYs whether or not a run came
(`host.h`, `src/host/request.c`). A restored host announces its
invocations only at its first run (`announcing`, `src/host/host.c`), and
a snapshot is taken only with no effect pending and no migration. So a
host that never runs holds the snapshot's moment.

The control plane already answers much of what an operator asks, on any
host (`docs/requests.md`):

| Method | Answers |
|---|---|
| `actor.list` | the sessions |
| `actor.get` | an actor's chart, parent, mailbox count, invocations and delayed sends |
| `actor.state` | an actor's configuration |
| `actor.context` | an actor's variables |
| `host.stats` | per segment, its pools; per chart, its counters |

What is missing:
- a host that holds still;
- the spans of the journal's kept records;
- a chart's structure;
- an actor's name, status, queued events and active states, read without
  three posts per actor.

## Decisions

1. **A frozen restore.** `ta_host_snapshot_input.flags` takes
   `TA_HOST_RESTORE_FROZEN`. A host restored with it changes nothing in
   its regions:
   - **`ta_host_run`** answers `TA_RUN_IDLE` with no deadline, before
     anything else it does. That comes before the encoder drains (which
     would acknowledge the journal's pending records), before errors are
     delivered and before invocations are announced. A binding's loop
     drains and stops, as on an idle host.
   - **`ta_host_advance_time`** does nothing (it stays `void`). A
     binding's loop calls it before every run as it does now.
   - **Refused with `TA_ERROR_STATE`**, every other call that would change
     the host:
     - spawn, send, send_many and terminate;
     - boot and add_image;
     - resume, hibernate, wake and migrate;
     - sweep (it clears the actors' touched flags);
     - `ta_host_traces` and `ta_host_logs` (a take acknowledges the
       journal's records).

     `ta_host_metrics` reads, and answers.
   - **Posts.** A post of a method that changes the host is answered with
     the error `state`. Those methods are `actor.send`,
     `trace.configure`, `trace.watch`, `trace.unwatch` and `trace.end`.
     The methods that read answer as on any host.
   - **Snapshots.** `ta_host_snapshot` of it gives the host snapshot it was
     restored from. `ta_host_snapshot_pack` packs the datamodels' state its
     caller gives.
2. **A frozen restore binds what it is not given.**
   - **What it binds.** For each datamodel its images name that the
     environment does not bind, the host adds a vtable that holds no
     state. These are the CLI's `bind_name`, moved into the host, and they
     come after the environment's bindings, in the order the images name
     them.
   - **Why that is safe.** A chart attaches to it. Nothing calls it,
     because nothing runs, and core null-checks every entry.
   - **What it answers.** `actor.context` of its actors is the error
     `state`, as for a datamodel that shows no variables.
   - **With the binding's own datamodel.** A binding passes its datamodels
     in the order the live host had them, and restores their state as it
     does now. Then `actor.context` answers the variables as they were.
   - **The limit.** The added bindings count toward the host's limit of
     eight.
3. **`actor.get` says what an operator asks first.** Its result gains
   these keys, on every host:
   - **`name`:** the name the actor was spawned with, `null` when it was
     given none.
   - **`status`:** one of `"idle"`, `"runnable"`, `"waitingWorkspace"`,
     `"waitingAck"`, `"done"` and `"hibernated"`, one for each `TA_ACTOR_*`
     state.
   - **`queued`:** the events in its mailbox, in the order it takes them,
     at most 32, each `{"event", "sendid"}`. It pairs with the count
     `mailbox`, as `timers` does with `timersPending`. The CLI's `actors`
     report renames its list to match (`queued`, with the count
     `mailbox`).
   - **`target`** in each delayed send of `timers`: its target, `null` for
     the actor's own queue.
4. **`actor.list` can say what each actor is doing.** With the param
   `"detail": true`, each entry of `sessions` is a map rather than a
   number: `{"session", "segment", "chart", "name", "status", "states"}`.
   `states` is the active atomic states' ids in document order, `[]` for
   a hibernated actor. The page is the same 1 to 512 entries. A tool then
   counts the actors per state with one post per page, not three per
   actor.
5. **`chart.describe`: a chart's structure, from its image.** It takes
   `segment` (uint) and `chart` (text, its name). Its result is
   `{"name", "datamodel", "states", "transitions", "data"}`:
   - **`states`:** `{"id", "kind", "parent"}` in document order, `kind`
     one of `"atomic"`, `"compound"`, `"parallel"`, `"final"` and
     `"history"`, `parent` an index or `null`.
   - **`transitions`:** `{"source", "events", "cond", "targets", "type"}`,
     where `source` and `targets` are state indices and `cond` is the
     expression's text or `null`.
   - **`data`:** the `<data>` ids.

   Errors: `missing`, `type`, `range` (no live segment), `unknown` (no
   chart of that name). It reads only the image, so it answers on any
   host. It is the chart of that version, which a tool cannot get from the
   current charts' sources.
6. **`host.stats` says what each segment runs and keeps.** Each entry of
   its `segments` gains:
   - **`live`** and **`charts`**: the names of the charts of its image.
   - **`trace`**: `{"records", "acknowledged", "pending", "bytes",
     "lost"}`, the journal's kept records, or `null` when tracing is off.
7. **`ta_host_trace_history(ta_host *h, ta_bytes *out)`: the spans the
   journal keeps.** It is a host output, a C call as `ta_host_traces` is
   (`docs/requests.md`, "What does not" belong there: bytes the host
   writes), on any host, live or frozen.
   - **What it writes.** An ExportTraceServiceRequest of every live
     segment's kept records, the acknowledged and the pending ones,
     oldest first. It reads them without taking or releasing any.
   - **The encoder.** It lays out an encoder of its own for the call,
     with `ta_otlp_config_normalize`'s defaults, so neither the host's
     telemetry nor its absence matters. Its staging is sized to the kept
     bytes, so the request is one.
   - **Names and attributes.** Spans are named and attributed as
     `ta_host_traces` names them.
   - **Times.** A kept record has no wall time, so a record's time is the
     system's clock converted with `ticksPerSecond` to nanoseconds, plus
     its fuel stamp's distance from the lowest of that tick.
   - **What an acknowledged record leaves out.** Its dynamic strings may
     have been freed, and its payloads released, so they are left out.
     Image strings stay.
   - **Spans at the edges.** A span whose beginning the ring dropped is
     left out. A span still open at the end ends there, marked
     `scxml.incomplete`.
   - **Its view.** It lives until the ack. With no batch open it opens one
     of its own, acknowledged at once by the binding, as `ta_host_metrics`
     does.
   - **Refusals.** `TA_ERROR_STATE` during a run or with tracing off.
   - **Decoding.** A binding decodes the bytes as any traces
     (`trace.spans.decode`, its `decodeTraces`).
8. **The CLI uses the host's.** `tinyactors inspect` of a snapshot and
   `tinyactors actors` restore frozen, and drop their own binding of
   datamodels. Their readers stay. `actors` renames its mailbox list as
   decision 3 says.
9. **The bindings expose it, nothing more.**
   - **`frozen`.** `Host.restore(snapshot, charts, {frozen: true})` in Node
     (`frozen=True` in Python) restores frozen. Its typed methods,
     `actor.state()` and `host.actors()` work, and its inputs throw the
     refusal.
   - **What the convenience layer skips on a frozen host.** It adds no
     image: `charts` must all be in the snapshot, or the call refuses them
     with `TA_ERROR_INVALID`. It trims nothing. It does not take traces or
     logs, even at close.
   - **`now()`** reads the snapshot's time, not the clock.
   - **The new calls.** `traceHistory()` (`trace_history()`) returns the
     bytes as `metrics()` does. `chartDescribe` (`chart_describe`) is the
     typed method of decision 5.
10. **The MCP server reads snapshots through it, and its copy of the
    Inspector's decoder goes:**

    | Tool | Reads |
    |---|---|
    | `snapshot_summary` | `host.stats` (segments, charts, trace counts) and `actor.list` with detail |
    | `snapshot_actors` | `actor.list` with detail, and `actor.get` |
    | `snapshot_variables` | `actor.context` of a frozen copy with the binding's datamodel |
    | `snapshot_chart` | `chart.describe`, and the counts from `actor.list` with detail |
    | `snapshot_traces` | `traceHistory()` decoded |
    | `rehearse` | its counts from `actor.list` with detail |

    The Inspector's own decoder stays, for snapshots of other builds.

## Consequences

- A tool reads a snapshot of its own build with the host's own readers.
  What a frozen host answers is what the live host would have answered at
  the moment of the snapshot.
- The keys added to `actor.get`, `actor.list` and `host.stats`, and the
  method `chart.describe`, are new protocol vectors
  (`tinyactors-test/suites/protocol/requests.txt`).
- What a frozen host cannot show:
  - a hibernated actor's configuration and variables, which are in its
    blob, as for a live host;
  - the dynamic strings and values of acknowledged trace records.
- A live host gains the same answers: an operator's `actor.get` shows the
  queued events, and `ta_host_trace_history` shows the recent spans
  without a frozen copy.
