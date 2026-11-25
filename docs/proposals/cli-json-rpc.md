# `tinyactors run`: running charts from the command over JSON-RPC 2.0

**Status:** Sketch (2026-11-25). Complete enough to argue about, not to
decide: a fresh agent has not yet refuted its recommendations (see
"Checked so far" at the end), so the maintainer should treat its claims
as plausible until that step has run.
**Touches:** tinyactors-core: `src/cli` (a JSON layer, NDJSON framing),
`src/base/format` (the shortest-decimal writer, moved out of `cbor.c`),
`tools/tinyactors` (a `run` command, platform additions), `mise/core.rb`,
`tests/`, `docs/cli.md`; the root's `GLOSSARY.md`. No public header, no
library, no binding.
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps
win.

## 1. Summary

`tinyactors run [options] [INPUT...]` makes one host, adds the inputs'
image, and then serves **JSON-RPC 2.0 on standard input and output, one
message per line**. Its methods are the host's C calls (`host.spawn` is
`ta_host_spawn`, `host.run` is the loop around `ta_host_run`), and every
other method is forwarded to `ta_host_post` unchanged, so the control
plane (`actor.state`, `host.stats`, `trace.watch`, ...) needs no code of
its own. What actors do comes back as **notifications, one per effect,
written before the ack of their batch and before the response to the
request that drained them**. The clock is simulated: it moves only when
the client says so. Values are JSON, converted at the edge to and from
the CBOR value table, with RFC 8949's conversion rules and a base64 CBOR
twin where JSON cannot hold a value.

It is a workbench: an agent or a CI job loads charts, spawns actors, sends
events, answers the actors' sends, moves the clock and reads what
happened, without a language binding and with the same binary that
compiles and checks the charts.

## 2. Whom it serves, what already answers them, who pays, what is out

Following [docs/roles.md](../roles.md).

**First: the application developer, through an agent or a CI job.** The
questions are the role's own: why did this actor end up in this state,
which transition fired with what data, what does my chart send and what
happens when the answer is a failure or never comes, does the new version
still pass. They decide how to fix a chart and whether to merge it.

**Then: the maintainers.** A bug report becomes a transcript (the
client's lines and the server's) that runs as a test; the scenarios of
the conformance suite run through the same server (section 7).

| Question | What answers it today | Why that is not enough |
|---|---|---|
| does the chart compile, what does it hold, what changed | `tinyactors check`, `inspect`, `states`, `diff` (`tinyactors-core/docs/cli.md`) | nothing runs |
| what does the chart do with these events | a binding (`@tinyactors/node`, `tinyactors` on PyPI) and a test written in its language | needs Node.js or Python, a package, a test file per question; an agent has to write a program to ask one question |
| run a scenario from the outside | `ta-conform` (`tinyactors-core/tools/conform`) | not shipped (a test program, `mise/core.rb:258`); speaks length-prefixed CBOR with integer statuses and no notifications (`tinyactors-test/docs/driver.md:21-31`); its `host.run` handles only transport sends and notifications, and fails nothing else (driver.md, `host.run`) |
| the actor's state, stats, traces | the posted methods (`tinyactors-core/docs/requests.md`) | CBOR, from inside a process that owns a host |

Core already shaped its control plane for this server:
"requests, replies and notifications are one ordered stream, so that a
JSON-RPC transport is a pipe" (`tinyactors-core/docs/requests.md:9`), "a
transport that speaks JSON on its wire converts at its edge"
(`requests.md:18`), and ADR 0012 keeps a raw `post` out of language APIs
"until a JSON-RPC transport exists" (`docs/adr/0012-lifetimes-follow-the-loop.md:208`).

**Who pays:**

| Cost | What |
|---|---|
| per event, per actor, in core's libraries | nothing: no public header or library changes; `ta_format_double` moves inside `src/base` (it is `cbor.c`'s Dragon4 today, `src/base/cbor.c:466`) |
| the command's size | it links the host and quickjs: the release `tinyactors` is 171,048 B today, and `ta-conform`, which links the same host, compiler and quickjs layers, is 1,689,760 B (`build/release/bin`, measured 2026-11-25 on this checkout). Expect about 1.7 MB (an estimate from that pair, unverified until built) |
| code | a JSON reader and writer and NDJSON framing in `src/cli` (estimated 800 lines with the decimal reader), the server in `tools/tinyactors` (estimated 1,500), tests and transcripts. Estimates, unverified |
| maintainers | a third driver of the loop beside the bindings and `ta-conform`: a change to a host call's contract (`docs/boundary.md`) or to `docs/embedding.md` reaches it too |
| API surface | a new wire protocol, documented in `docs/cli.md`; 0.x, so it may change in place (root `AGENTS.md`, "Before 1.0") |
| every binding | nothing |
| the C embedder, the operator, the capacity planner | nothing; they are not served (below) |

**Out of scope, and whom that leaves unserved:**

- **Production.** One host, one client, standard input: an operator runs
  a binding or their own C loop. The operator and the capacity planner are
  not served.
- **Other transports** (TCP, HTTP, WebSocket): stdio only.
- **Several hosts per server.** One host per process, as one host per
  thread is the rule (`tinyactors-core/docs/embedding.md:250`). Two
  systems are two servers, and the client is the transport between them
  (section 4.6).
- **A real-time clock** in the first version (open question 1).
- **Datamodels other than quickjs.** `datamodel="python"` charts run in
  the Python binding; the native ECMAScript datamodel is Node's.
- **A store.** Hibernation blobs go to the client (section 4.5): "the
  application owns storage and policy" (root `AGENTS.md:15`).
- **A REPL for people.** The client is a program.
- **MCP.** An MCP server is a separate proposal
  ([mcp-server.md](mcp-server.md)), written against the dropped ring
  protocol; an MCP adapter could later sit over this server, but nothing
  here is shaped for it.

**Not a binding.** It is a C embedder of the host, like `ta-conform`,
with a wire protocol on top. It keeps the bindings' rule all the same:
**every method is a host call, a forwarded posted message, or the
handling of an effect**, and nothing core does is done again.

**Security.** The client is trusted: it started the process, and it can
name any file the process can read (`files`, `--config`) or write
(`path`). Charts are not code the server trusts more than quickjs does:
they run in quickjs with its limits (`max_ops`, `memory_limit`,
`tinyactors-core/docs/host-config.md`, "The datamodels' limits"), and
quickjs-libc's `std` and `os` modules are not built
(`mise/core.rb:76`), so a chart reads no file and opens no socket. A chart
that never idles is bounded by `host.run`'s run count.

## 3. Transport, startup and shutdown

### 3.1 Framing

**Newline-delimited JSON:** each message is one JSON text (RFC 8259) in
UTF-8 on one line, ended by `\n`; the server writes compact JSON, which
holds no raw newline because JSON escapes control characters in strings.
This is MCP's stdio transport ("Messages are delimited by newlines, and
MUST NOT contain embedded newlines",
<https://modelcontextprotocol.io/specification/2025-06-18/basic/transports>),
which agents' JSON-RPC clients already speak, and what `jq -c` and a
shell's `read` handle.

| Alternative | Not chosen because |
|---|---|
| LSP's `Content-Length` headers | its clients are editors; an agent's or a shell's client is line-based; a second framing is a second thing to test |
| the driver protocol's length-prefixed CBOR (`driver.md:21`) | not text: a CI job cannot write it with `echo` or read it with `jq` |

- **Lines.** A blank line is ignored; a `\r` before the `\n` is dropped.
  A line longer than `--max-message` (default 64 MiB, the server's own
  bound, so that a client sending no newline cannot grow it without
  end) is refused with -32600 and skipped to its newline.
- **Envelope.** JSON-RPC 2.0: a request has `"jsonrpc": "2.0"`, `method`,
  `params` (an object; absent: none) and `id` (integer, string or null);
  without `id` it is a notification, applied with no response. A message
  that is not one is answered with -32600 and `"id": null`, and one that
  is not JSON with -32700.
- **Batches** (an array of requests) are taken: each element runs in
  order, one at a time, as if sent alone; their notifications are written
  as they come, and the responses as one array after the last (none when
  every element was a notification), as JSON-RPC 2.0 says. Core refuses
  batches and leaves this to the transport (`requests.md:60`).
- **Pipelining** is allowed and changes nothing: the server reads a line,
  answers it, then reads the next. Responses come in request order.
- **Order on standard output:** every notification a request caused comes
  before its response. Standard error is free text for people.

### 3.2 Startup

```text
tinyactors run [--config FILE] [--system ID] [-W] [--boot]
               [--max-ops N] [--memory-limit BYTES] [--realm auto|isolated]
               [--max-message BYTES] [INPUT...]
```

| Option | Meaning |
|---|---|
| `--config FILE` | the host config (`tinyactors-core/docs/host-config.md`) as JSON, converted to CBOR (section 5); a file whose first byte starts a CBOR map is read as CBOR. Absent: `{}` |
| `--system ID` | the config's `systemID`; refused (usage) when the config holds another. One of the two must name it: core has no default for it, deliberately (host-config.md, "`systemID` has none") |
| `INPUT...` | an image, a manifest or charts, read as every command reads them (`docs/cli.md`, "Inputs"), added as segment 0 before the first message; with `-W`, a warning refuses it |
| `--boot` | `ta_host_boot` of segment 0 (a manifest's boot actors) |
| `--max-ops`, `--memory-limit`, `--realm` | `ta_datamodel_ecmascript_config` (`include/tinyactors/datamodel_ecmascript.h:61-71`) |

- **The environment** the server gives `ta_host_create`: the platform's
  allocator, the one quickjs datamodel, the default trace policy,
  `nonce` 0, an `error` buffer. With the config's `system.traceSeed`
  absent and a nonce of 0, two runs of one transcript write the same bytes
  (the bindings draw both at random, napi `AGENTS.md`, "The binding picks
  the trace seed"; a workbench wants transcripts that repeat).
- **The clock** starts at monotonic 0 and wall time 0 (section 4.3).
- **A refusal at startup** (an input that does not compile, a config core
  refuses) is told on standard error, `error`'s reply converted to JSON,
  and the command exits with status 1 before it reads a message. A CI job
  fails fast.
- The server reads `host.config` once (posted, drained at creation, as the
  bindings do) for `system.ticksPerSecond`.

### 3.3 Shutdown

At the end of standard input the server drains what is pending, posts
`trace.end` when the host has an encoder (its spans reach the client as
section 4.4 says), frees the host and then quickjs, and exits 0. There is
no `shutdown` method: closing standard input is the request, as for
`ta-conform` (`driver.md:24`).

| Exit status | When (`docs/cli.md`, "Exit status") |
|---|---|
| 0 | the input ended |
| 1 | an input or the config was refused at startup |
| 2 | the command line was not understood |
| 3 | standard input could not be read, standard output could not be written (the client went away), or memory ran out outside a run |

## 4. Methods and notifications

### 4.1 The rule

- **`host.<verb>` is the C call `ta_host_<verb>`**, its params the call's
  struct fields in camelCase (as `driver.md` names them: "takes their
  struct fields in camelCase"). The bindings name the same calls the same
  way (`host.spawn`, `host.send`, `host.fail`, `host.hibernate`,
  `host.wake`, `host.migrate`, `host.snapshot`; napi `AGENTS.md`).
- **Every other method is forwarded**: the message without its `jsonrpc`
  member, converted to CBOR, given to `ta_host_post`; its `REPLY`'s
  `WIRE` converted back is the response. A message without `id` is a
  posted notification, as JSON-RPC's, and gets no response in either
  protocol (`requests.md`, "The envelope"). A method core does not know is
  core's -32601. A method core adds later reaches the client with no
  change here.
- **Names cannot collide**: the server's `host.*` methods are calls core
  keeps out of the control plane by its own rule ("Not methods",
  `requests.md`: the hot path, images, bytes the host writes or takes,
  lookups). Core's posted `host.stats` and `host.config` have no C call of
  that name. Core's posted `actor.send` stays reachable, forwarded;
  `host.send` is `ta_host_send` with every field of `ta_event_in`.

### 4.2 The methods

Values (`data`, `params`) are JSON, or `cbor` (base64 of the neutral
value's bytes) for one JSON cannot hold (section 5). A target is a session
(an integer) or a local address (`ta_host_resolve`). Every result is a
JSON object.

| Method | Params | Result | Calls |
|---|---|---|---|
| `host.addImage` | one of `files` (paths, read as `INPUT`s are), `sources` (`[{name, text}]`, the first the default boot chart; a `src` names another by its name), `image` (base64 `.tai`); `warningsAsErrors` | `segment`, `charts` (`[{name, chart}]`), `diagnostics` (as `check -f json` writes them) | `ta_compile_charts` or `ta_compile_system`, `ta_compile_image`, `ta_host_add_image`, `ta_compile_free` |
| `host.boot` | `segment` | `{}` | `ta_host_boot` |
| `host.chart` | `name` | `segment`, `chart` | `ta_host_chart` |
| `host.spawn` | `chart` (a name: its current version) or `segment` and `chart` (an index); `name`, `params` or `paramsCBOR`, `traceparent` | `session`, `address` | `ta_host_chart`, `ta_datamodel_ecmascript_payload_from_cbor`, `ta_host_spawn`, `ta_address_format` |
| `host.send` | `target`, `event`; `data` or `cbor`; `sendid`, `origin`, `origintype`, `raw`, `traceparent`; for a service the application runs: `invocation` (the `token` of its `effect.invoke`) and `done` (bool) | `{}` | `ta_host_resolve`, `..._payload_from_cbor`, `ta_host_send` (`TA_EVENT_IN_FROM_INVOKE`, `TA_EVENT_IN_INVOKE_DONE`) |
| `host.sendMany` | `events`: `host.send`'s params each | `accepted`: the accepted prefix's length | `ta_host_send_many` |
| `host.fail` | `target`, `sendid`; or `target` and `invocation` | `{}` | `ta_host_send` with `TA_EVENT_IN_ERROR_COMMUNICATION` (with `TA_EVENT_IN_FROM_INVOKE` for an invocation) |
| `host.terminate` | `target` | `{}` | `ta_host_terminate` |
| `host.resolve`, `host.actorByName` | `address`; `name` | `session` (`null`: none) | `ta_host_resolve`; `ta_host_actor_by_name` |
| `host.advanceTime` | `by` or `to`: ticks (integer), or a duration as SCXML writes one (`"30s"`, `"150ms"`); `wallNs` | `time`, `wallNs` | `ta_host_advance_time` |
| `host.run` | `budget` (fuel per run, default 10,000), `runs` (at most, default 1,000), `hold` (bool) | `status`, `detail`, `runs`, `fuel`, `time`, `nextDeadline` (`null`: none) | the turn of section 4.4 |
| `host.effectsAck` | — | `{}` | `ta_host_effects_ack` of a held batch |
| `host.hibernate` | `session` | `blob` (base64) | `ta_host_hibernate`, copied before the ack |
| `host.wake` | `session`, `blob` | `{}` | `ta_host_wake` |
| `host.sweep` | `maxScan`, `limit` | `sessions` | `ta_host_sweep` |
| `host.migrate` | `chart`, `session`, `wake`, `timeout` (ticks or a duration) | `migration` | `ta_host_migrate` |
| `host.snapshot` | `path` (optional: written whole through the platform) | `snapshot` (base64), or `bytes` with `path` | `ta_datamodel_ecmascript_snapshot`, `ta_host_snapshot_pack` |
| `host.restore` | `snapshot` (base64) or `path` | `segments` | `ta_host_snapshot_unpack`, `ta_host_snapshot_image`, `ta_host_restore` (with the server's config), `ta_datamodel_ecmascript_restore`; the old host and datamodel freed after |
| `host.traces` | — | `spans`, as `trace.spans.decode` answers | `ta_host_traces`, copied, acked, then `trace.spans.decode` posted |
| `host.metrics` | — | `otlp` (base64) | `ta_host_metrics` |
| `host.post` | `message` (base64 CBOR) | `reply` (base64) or nothing for a notification | `ta_host_post`: the escape for bytes JSON cannot carry (a forwarded `trace.spans.decode`'s `traces`) |
| anything else | as core's | core's `result` or `error` | `ta_host_post` (`trace.*`, `host.stats`, `host.config`, `actor.list`, `actor.get`, `actor.context`, `actor.state`, `actor.send`) |

Defaults the server owns, and why: `budget` and `runs` bound a turn so a
chart that never idles cannot hang the client (`ta-conform` bounds its
loop the same way, at 1,000 runs, `driver.md`, `host.run`); `--max-message`
bounds a line. No other default: everything else is core's.

### 4.3 Time

- **Simulated.** Monotonic time starts at 0 and moves only with
  `host.advanceTime`; wall time starts at 0 and moves by the same
  duration (a `wallNs` param sets it), so spans have repeatable stamps.
  Nothing moves the clock behind the client's back, and nothing runs
  between requests.
- **Ticks, as core counts them.** Times in params and results are the
  host's ticks at `system.ticksPerSecond` (default 1e9, nanoseconds), as
  forwarded replies are (`actor.get`'s `due`, `requests.md`), so the two
  never disagree. A duration text (`"30s"`) is read by core's reader of
  SCXML delays (`ta_core_parse_duration`,
  `tinyactors-core/src/core/event_pool.c:58`) and converted with
  `ticksPerSecond`: the client writes time as the chart does.
- **Advancing and running are separate**, as in C: `host.advanceTime`
  then `host.run`, or both in one JSON-RPC batch. The W3C environment's
  "move to the next deadline when idle" is `{"to": nextDeadline}` from
  the last run's result.

### 4.4 The turn, and when the server acks

`host.run` is embedding.md's loop (section 2) with the effects handled by
the table below:

```
host.run:
    if a batch is held: ta_host_effects_ack            (the client answered it)
    drain                                              (what inputs appended)
    repeat at most `runs` times:
        run = ta_host_run(host, budget)
        drain, stopping with the batch open when `hold` and it holds a waiting send
        EFFECTS, EFFECTS_FULL -> again
        IDLE                  -> answer status "idle", nextDeadline
        YIELDED               -> again (status "yielded" when `runs` is spent)
        EXHAUSTED, FAULT      -> answer it, detail ta_status_name / the pool

drain:
    while (n = ta_host_effects(host, &effects)) > 0:
        for each effect, in order: handle it (table), writing its notification
        ta_host_effects_ack(host)                       (unless held)
        then the work that needs the ack: post trace.spans.decode for a TRACES taken
```

**The ack.** The server acks a batch after it has written every
notification of it, unless `host.run` was given `hold` and the batch has a
send whose sender waits (`TA_EFFECT_FLAG_WAITING`) and that only the
client can decide: a send to an I/O processor of the config, to the
transport, or to a service. Then `host.run` answers `"status": "held"`
with the batch open, and the sender stays parked until the client acks
(`host.run` again, or `host.effectsAck`). What the client sends before
that is in the sender's queue when it goes on: an answer, or
`host.fail`'s `error.communication`, which is how SCXML describes a
failed `<send>` (ADR 0017, decision 2,
`docs/adr/0017-one-way-out-and-sends-that-wait.md:78-90`). Without
`hold`, the batch is acked at once, the sender goes on, and an answer the
client gives later arrives as an ordinary event, as a binding's answer
given after its handler returned does (napi `AGENTS.md`, "Materialize
what outlives the ack").

**While a batch is held** (phase D, `docs/boundary.md`, "Phases"):

| Allowed | Refused |
|---|---|
| `host.send`, `host.sendMany`, `host.fail`, `host.terminate`, `host.spawn` (inputs apply now; their effects come after the ack), the lookups, `host.hibernate` (joins the batch), `host.run`, `host.effectsAck` | forwarded methods and `host.post`: core answers a post made while a batch is open only in the take after the ack (`requests.md:50`), so the response could not come before the next request. -32002, reason `state` |
| | `host.snapshot` (core refuses with a batch open), `host.addImage`, `host.restore` |

Every other method drains after its call (a forwarded post's batch holds
its reply and the effects the method appended, `trace.end`'s `TRACES`),
so every effect reaches the client before the response to the request
that caused it.

**Each effect, and what reaches the client.** Every notification carries
`batch` (the server's count of takes) and `index` (the effect's place in
it).

| Effect | The server | Notification `params` (besides `batch`, `index`) | The client may |
|---|---|---|---|
| `TA_EFFECT_SEND`, `IO_PROCESSOR` i + 1 | nothing more; `hold` stops here | `effect.send`: `session`, `type` (the config's `ioProcessors[i].type`), `target`, `event`, `sendid`, `data`/`cbor`, `traceparent`, `waiting` | answer: `host.send` to `session` with `sendid`, `origintype` `type`, `traceparent`; or `host.fail` |
| `TA_EFFECT_SEND`, `IO_PROCESSOR` 0 (the transport) | as above | `effect.send` with `message`: `{target, event, data, sendid, origin, traceparent}` (ADR 0017, decision 4) | pass `message` to another server's `host.send` (section 4.6); `host.fail`; nothing (success is silence, ADR 0014, decision 5) |
| `TA_EFFECT_SEND`, `IO_PROCESSOR` `TA_NIL` | as above | `effect.send` with `invocation` (the token) and `segment` | answer as the service: `host.send` with `invocation` |
| `TA_EFFECT_INVOKE` | nothing: the client is every service | `effect.invoke`: `session`, `segment`, `invocation` (token), `type`, `src`, `invokeid`, `data`, `restored` | `host.send` with `invocation` (and `done`); `host.fail` with `invocation` |
| `TA_EFFECT_UNINVOKE` | nothing | `effect.uninvoke`: `segment`, `invocation` | stop its stand-in |
| `TA_EFFECT_REQUEST` | `ta_host_resume` with `TA_ERROR_INVALID` before the ack, as both bindings do (the parent gets `error.execution`; `tinyactors-core/docs/embedding.md`, section 4) | `effect.request`: `session`, `src` | nothing |
| `TA_EFFECT_WAKE` | nothing (no store) | `effect.wake`: `session`, `migration` | `host.wake` with the blob `host.hibernate` gave it |
| `TA_EFFECT_LOG` | nothing | `effect.log`: `session`, `label`, `text` | nothing |
| `TA_EFFECT_DONE` | nothing | `effect.done`: `session`, `name`, `final`, `data`/`cbor` (the donedata) | nothing |
| `TA_EFFECT_ERROR` | nothing | `effect.error`: `session`, `status` (`ta_status_name` of `FLAGS`) | nothing |
| `TA_HOST_EFFECT_MIGRATED` | nothing | `effect.migrated`: `session`, `segment`, `chart`, `migration` | nothing |
| `TA_HOST_EFFECT_MIGRATION_DONE` | reads `ta_host_migration_failed` and `_pending` during dispatch | `effect.migrationDone`: `migration`, `migrated`, `failed` (`[{session, status}]`), `pending` | `host.wake` the pending |
| `TA_HOST_EFFECT_SEGMENT_FREED` | nothing | `effect.segmentFreed`: `segment` | nothing |
| `TA_HOST_EFFECT_TRACES` | `ta_host_traces`, the bytes copied into the request's arena; after the ack, `trace.spans.decode` posted and drained | `effect.traces`: `spans`, written after the other notifications of its batch | nothing |
| `TA_HOST_EFFECT_LOGS` | `ta_host_logs`, copied | `effect.logs`: `otlp` (base64; core has no posted decoder for logs) | nothing |
| `TA_HOST_EFFECT_REPLY` | the response to the forwarded request (or the server's own post) | — | — |
| `TA_HOST_EFFECT_NOTIFICATION` | none is defined (`requests.md`, "Notifications") | written as it is, its `method` and `params` converted | — |

A field the effect does not set is left out, never `null`, so a present
`null` is a value.

### 4.5 Hibernation, migration, snapshots

The blobs are the client's: `host.hibernate` answers the blob (copied
into the response before the batch it joins or opens is acked,
`docs/boundary.md`, `ta_host_hibernate`), an `effect.wake` says which
session needs its blob, and `host.wake` brings it back. A migration's
`effect.wake`s carry its id. `host.snapshot` packs the host with
quickjs's state (labelled `tinyactors`), refused by core unless the host
is quiescent; `host.restore` makes a new host from it with the server's
config and datamodel limits, frees the old ones, and keeps the clock at
the snapshot's time. Its first `host.run` announces every invocation that
was running as an `effect.invoke` with `restored` (`host.h:488-495`).

### 4.6 Two systems

A client that starts two servers (`--system warehouse`,
`--system storefront`) is the transport between them: it takes
`effect.send`'s `message` from one and gives it to the other's
`host.send` as it is. `host.send` takes `target` as an address and
resolves it, and passes `origin`, so a chart's reply to `_event.origin`
comes back out as the second server's `effect.send` for the transport.
This is the bindings' `send.message` and `host.send(message)`, decision 4
of ADR 0017.

## 5. Values and errors

### 5.1 JSON and the value table

Neutral values are CBOR (`tinyactors-core/docs/protocol.md`, "Values").
The server converts at the edge, with RFC 8949's own rules
(<https://www.rfc-editor.org/rfc/rfc8949.html>, sections 6.1 and 6.2),
and never invents a JSON form:

| CBOR | JSON written (6.1) | JSON read (6.2) |
|---|---|---|
| unsigned and negative integers | a number, exact over the whole range -2^64 … 2^64-1 | a number without fraction or exponent, within that range (6.2's default range is ±(2^53-1) and "other ranges are allowed"; the server takes 64 bits); outside it, a float |
| floats | the shortest decimal that reads back as the double (`-0.0` kept); NaN, ±Infinity as `null` | a number with a fraction or exponent, rounded to the nearest double, ties to even |
| text | a string | a string: UTF-8 checked, `\u` escapes joined in pairs, a lone surrogate refused |
| bytes | base64url text without padding | — (text is text) |
| arrays, maps with text keys | arrays, objects, keys in order | the same; a repeated key: the first, as core reads CBOR (`ta_cbor_member`, `include/tinyactors/cbor.h`) |
| `false`, `true`, `null` | themselves | themselves |
| `undefined` | `null` | — |
| tag 32 (an address) | its text, the tag dropped | — (text) |

**Where JSON loses something, the CBOR comes too.** When writing a value
changed it (bytes, `undefined`, a non-finite float, a tag), the field is
followed by its twin `cbor`, the neutral value's bytes in base64: `data`
and `cbor`, `params` and `paramsCBOR`. The client reads `data` and
looks at `cbor` only when it is there. On input, `cbor` instead of `data`
gives a value JSON cannot write (bytes, `undefined`, an address). This is
`driver.md`'s `actor.send` (`data` or `cbor`), not a new shape.

**No escape inside JSON.** A wrapper object such as `{"$bytes": "..."}`
is not used: a key starting with `$` is an ordinary key of the value table
(`protocol.md:108`), so any such convention would misread a value.

**Forwarded replies are core's, converted.** `actor.context` writes
`undefined` for a `<data>` with no value (`requests.md`, `actor.context`),
which JSON shows as `null`. A client that must tell them apart uses
`host.post` and reads the CBOR.

**Large integers.** Session ids are exact as JSON numbers. A JavaScript
client reading them with `JSON.parse` loses bits past 2^53: segment k
numbers its sessions from k·2^32 + 1 (`host.h:13-14`), so a host stays
below 2^53 until segment 2^21. A client that cares parses integers as
BigInts. (protocol.md requires session ids be kept lossless; the server
does, the client's parser is the client's.)

### 5.2 Errors

The error object is core's (`requests.md`, "The envelope"): `{"code",
"message", "data": {"reason", "path"}}`, with core's codes and reasons, so
a client handles forwarded and own errors alike. The server's own errors
add `data.status`, `ta_status_name` of the call's status, where a C call
refused.

| Condition | Code | Reason |
|---|---|---|
| not JSON | -32700 | `malformed` |
| not a request: not an object, `jsonrpc` not `"2.0"`, no `method`, `params` not an object, a bad `id`, a line past `--max-message` | -32600 | `type`, `missing`, `limit` |
| no such method | -32601 (core's, forwarded) | `unknown` |
| a param missing, of the wrong type, unknown, out of range | -32602 | `missing`, `type`, `unknown`, `range` |
| `TA_ERROR_STALE` | -32001 | `stale` |
| `TA_ERROR_STATE`, and a forwarded method while a batch is held | -32002 | `state` |
| `TA_ERROR_LIMIT` | -32003 | `limit` |
| `TA_ERROR_NO_MEMORY` | -32004 | `memory` |
| `TA_ERROR_INVALID` | -32005 | `invalid` |
| data `from_wire` cannot read (`TA_ERROR_INVALID` from `payload_from_cbor`) | -32006 | `malformed` |
| `TA_ERROR_UNSUPPORTED` (a snapshot of a value outside the table) | -32005 | `unsupported` (a new reason, the server's) |

A run's outcome is not an error: `host.run` answers `"status": "fault"`
or `"exhausted"` with its `detail`, and the host goes on, as
embedding.md's loop does.

## 6. A worked example

`order.scxml`: an order asks the payment processor to charge its total and
waits at most 30 seconds.

```xml
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" name="order"
       datamodel="ecmascript" initial="open">
  <datamodel><data id="total" expr="0"/><data id="charge" expr="null"/></datamodel>
  <state id="open">
    <transition event="checkout" target="paying"/>
  </state>
  <state id="paying">
    <onentry>
      <send type="payments" target="charges" event="charge" id="pay">
        <param name="amount" expr="total"/><param name="card" expr="_event.data.card"/>
      </send>
      <send event="payment.timeout" delay="30s" id="deadline"/>
    </onentry>
    <onexit><cancel sendid="deadline"/></onexit>
    <transition event="charge.ok" target="paid">
      <assign location="charge" expr="_event.data.charge"/>
    </transition>
    <transition event="charge.declined error.communication payment.timeout" target="open">
      <log label="order" expr="'payment failed: ' + _event.name"/>
    </transition>
  </state>
  <final id="paid">
    <onentry><log label="order" expr="'paid ' + charge"/></onentry>
    <donedata><param name="charge" expr="charge"/></donedata>
  </final>
</scxml>
```

`shop.json`: `{"system": {"ticksPerSecond": 1000}, "ioProcessors":
[{"type": "payments"}]}` (milliseconds, so `nextDeadline` reads easily).
`>` is a line the client writes, `<` one the server writes; `…` elides a
traceparent. The ids, sessions and times are what this design gives; the
fuel numbers are illustrative.

```text
$ tinyactors run --config shop.json --system shop order.scxml
> {"jsonrpc":"2.0","id":1,"method":"host.spawn","params":{"chart":"order","name":"order-17","params":{"total":4200}}}
< {"jsonrpc":"2.0","id":1,"result":{"session":1,"address":"tinyactors://shop/#_scxml_1"}}
> {"jsonrpc":"2.0","id":2,"method":"host.send","params":{"target":1,"event":"checkout","data":{"card":"tok_visa"}}}
< {"jsonrpc":"2.0","id":2,"result":{}}
> {"jsonrpc":"2.0","id":3,"method":"host.run","params":{"hold":true}}
< {"jsonrpc":"2.0","method":"effect.send","params":{"batch":1,"index":0,"session":1,"type":"payments","target":"charges","event":"charge","sendid":"pay","data":{"amount":4200,"card":"tok_visa"},"traceparent":"00-…-01","waiting":true}}
< {"jsonrpc":"2.0","id":3,"result":{"status":"held","runs":1,"fuel":41,"time":0,"nextDeadline":30000}}
```

The order is parked after its `<send>`. Its state cannot be read while
the batch is held:

```text
> {"jsonrpc":"2.0","id":4,"method":"actor.state","params":{"session":1}}
< {"jsonrpc":"2.0","id":4,"error":{"code":-32002,"message":"a batch is held: answer its sends, then host.run or host.effectsAck","data":{"reason":"state","path":""}}}
> {"jsonrpc":"2.0","id":5,"method":"host.send","params":{"target":1,"event":"charge.ok","sendid":"pay","origintype":"payments","traceparent":"00-…-01","data":{"charge":"ch_81"}}}
< {"jsonrpc":"2.0","id":5,"result":{}}
> {"jsonrpc":"2.0","id":6,"method":"host.run"}
< {"jsonrpc":"2.0","method":"effect.log","params":{"batch":2,"index":0,"session":1,"label":"order","text":"paid ch_81"}}
< {"jsonrpc":"2.0","method":"effect.done","params":{"batch":2,"index":1,"session":1,"name":"order-17","final":"paid","data":{"charge":"ch_81"}}}
< {"jsonrpc":"2.0","id":6,"result":{"status":"idle","runs":1,"fuel":37,"time":0,"nextDeadline":null}}
```

A second order whose processor never answers, without `hold`: the send is
reported and acked, and the deadline decides.

```text
> [{"jsonrpc":"2.0","id":7,"method":"host.spawn","params":{"chart":"order","params":{"total":990}}},
   {"jsonrpc":"2.0","id":8,"method":"host.send","params":{"target":2,"event":"checkout","data":{"card":"tok_expired"}}},
   {"jsonrpc":"2.0","id":9,"method":"host.run"}]
< {"jsonrpc":"2.0","method":"effect.send","params":{"batch":3,"index":0,"session":2,"type":"payments","target":"charges","event":"charge","sendid":"pay","data":{"amount":990,"card":"tok_expired"},"traceparent":"00-…-01","waiting":true}}
< [{"jsonrpc":"2.0","id":7,"result":{"session":2,"address":"tinyactors://shop/#_scxml_2"}},{"jsonrpc":"2.0","id":8,"result":{}},{"jsonrpc":"2.0","id":9,"result":{"status":"idle","runs":2,"fuel":44,"time":0,"nextDeadline":30000}}]
> {"jsonrpc":"2.0","id":10,"method":"host.advanceTime","params":{"by":"30s"}}
< {"jsonrpc":"2.0","id":10,"result":{"time":30000,"wallNs":30000000000}}
> {"jsonrpc":"2.0","id":11,"method":"host.run"}
< {"jsonrpc":"2.0","method":"effect.log","params":{"batch":4,"index":0,"session":2,"label":"order","text":"payment failed: payment.timeout"}}
< {"jsonrpc":"2.0","id":11,"result":{"status":"idle","runs":1,"fuel":23,"time":30000,"nextDeadline":null}}
> {"jsonrpc":"2.0","id":12,"method":"actor.state","params":{"session":2}}
< {"jsonrpc":"2.0","id":12,"result":{"value":"open","history":{}}}
```

(The batch above is shown on three lines; on the wire it is one.) The
spawn, send and run in one batch share one array response; the
notification comes first because the run that made it ran before the
array was written. Forwarded `actor.state` is core's reply, converted.

## 7. Implementation outline

### 7.1 Where the code goes

| Piece | Where | Why there |
|---|---|---|
| JSON ↔ CBOR, a JSON string writer | `src/cli/json.{h,c}` | freestanding and in no released library (`mise/core.rb:55-58`); ADR 0009 took JSON out of core's libraries (decision 5, `docs/adr/0009-one-host-two-queues.md:142`), and the `cli` layer already writes JSON (`src/cli/report.c:189`) |
| the shortest decimal of a double | `src/base/format.{h,c}`: `ta_format_double`, moved out of `cbor.c` (Dragon4, `src/base/cbor.c:466`) | one writer of floats: CBOR's diagnostic notation and JSON |
| NDJSON lines and the JSON-RPC envelope | `src/cli/rpc.{h,c}` | freestanding, unit-tested without a host |
| the command: options, startup, the message loop | `tools/tinyactors/run.c` | the commands' layer, compiled without the C library's headers |
| the methods and the drain | `tools/tinyactors/serve.c`, `tools/tinyactors/effects.c` | the same |
| reading standard input as it comes | `tools/tinyactors/platform.c` | the only file that uses the C library |

`tool_load` (`tools/tinyactors/input.c:138`) both makes the image and
tells what went wrong (`tool_complain`, the report); the server needs the
first half alone, for `host.addImage`'s diagnostics in a result: it splits
into `tool_image_make` (the image and its diagnostics) and `tool_load`
(that, then telling), every caller migrated in the same change.

`mise/core.rb`: the `cli` layer gains `json` and `rpc`; the `tool` layer
uses `host` and `datamodel_ecmascript` too; new unit tests
`test_cli_json`, `test_cli_rpc`.

### 7.2 Interfaces

`src/cli/json.h`:

```c
/* cli/json.h — JSON text (RFC 8259) to one CBOR item and back, at the edge
 * of a transport whose wire is JSON (docs/requests.md: "a transport that
 * speaks JSON on its wire converts at its edge"). RFC 8949, section 6.2
 * reading and 6.1 writing. Freestanding: no C library, no recursion (an
 * explicit stack of TA_JSON_MAX_DEPTH), no allocation but the writer's and
 * the output's allocators. */
typedef enum ta_json_limits {
    TA_JSON_MAX_DEPTH = 64 /* containers open at once: TA_CBOR_DEPTH_MAX */
} ta_json_limits;

/* Where a text was refused, and why (a static text). */
typedef struct ta_json_failure {
    size_t offset;
    ta_string_view reason;
} ta_json_failure;

typedef struct ta_json_reading {
    ta_string_view text;      /* one JSON text, UTF-8 */
    ta_cbor_writer *writer;   /* receives one item; containers TA_CBOR_COUNT_LATER */
    ta_json_failure failure;  /* set on TA_ERROR_INVALID and TA_ERROR_LIMIT */
} ta_json_reading;
/* TA_OK; TA_ERROR_INVALID: not JSON, not UTF-8, a lone surrogate;
 * TA_ERROR_LIMIT: nested deeper than TA_JSON_MAX_DEPTH;
 * TA_ERROR_NO_MEMORY: the writer's allocator refused. */
ta_status ta_json_read(ta_json_reading *reading);

/* What writing changed (6.1's substitutions). */
typedef enum ta_json_changes {
    TA_JSON_CHANGED_BYTES = 1u << 0,     /* bytes as base64url text */
    TA_JSON_CHANGED_UNDEFINED = 1u << 1, /* undefined, another simple value: null */
    TA_JSON_CHANGED_FLOAT = 1u << 2,     /* NaN, an infinity: null */
    TA_JSON_CHANGED_TAG = 1u << 3        /* a tag dropped: an address as its text */
} ta_json_changes;

typedef struct ta_json_writing {
    ta_bytes cbor;     /* one item, read with ta_cbor_reader */
    ta_output *out;    /* compact JSON appended */
    uint32_t changes;  /* ta_json_changes, set by the call */
} ta_json_writing;
/* TA_OK; TA_ERROR_INVALID and TA_ERROR_UNSUPPORTED as ta_cbor_read;
 * TA_ERROR_NO_MEMORY: the output could not grow. */
ta_status ta_json_write(ta_json_writing *writing);

/* `text` as a JSON string, quoted and escaped; report.c's JSON uses it. */
void ta_json_write_string(ta_output *out, ta_string_view text);
```

The decimal reader inside `json.c` rounds correctly (the nearest double,
ties to even) without the C library: a fast path when the digits and the
power of ten are exact doubles, else a comparison of big integers with
the fixed-size arithmetic `cbor.c`'s Dragon4 already has (44 words of 32
bits). No `strtod` (it is the C library), no quickjs `js_atod` (the `cli`
layer uses `base` only).

`src/cli/rpc.h`:

```c
/* cli/rpc.h — JSON-RPC 2.0, one message per line. */
typedef struct ta_rpc_lines {
    ta_output pending; /* bytes after the last newline, over the caller's allocator */
    size_t limit;      /* the longest line taken (--max-message) */
    size_t skipping;   /* past the limit: bytes dropped up to the next newline */
} ta_rpc_lines;
typedef struct ta_rpc_lines_config {
    const ta_allocator *allocator;
    size_t limit;
} ta_rpc_lines_config;
void ta_rpc_lines_init(ta_rpc_lines *lines, const ta_rpc_lines_config *config);
/* Bytes as they came: TA_OK, or TA_ERROR_NO_MEMORY (nothing taken). */
ta_status ta_rpc_lines_feed(ta_rpc_lines *lines, ta_bytes chunk);
/* The next whole line, without its "\r\n" or "\n": 1, 0 when none is
 * whole yet; a view valid until the next feed. `too_long`: a line past
 * the limit was dropped (answered -32600). */
typedef struct ta_rpc_line {
    ta_string_view text;
    uint32_t too_long;
} ta_rpc_line;
int ta_rpc_lines_next(ta_rpc_lines *lines, ta_rpc_line *out);
void ta_rpc_lines_free(ta_rpc_lines *lines);

/* A message read: its CBOR and tokens, and where its members are. */
typedef enum ta_rpc_kind { TA_RPC_REQUEST = 0, TA_RPC_NOTIFICATION = 1, TA_RPC_BATCH = 2 } ta_rpc_kind;
typedef struct ta_rpc_error {
    int32_t code;
    ta_string_view reason, path, message;
} ta_rpc_error;
typedef struct ta_rpc_message {
    ta_cbor_document document; /* tokens in the arena */
    uint32_t kind;             /* ta_rpc_kind */
    uint32_t method, params, id; /* token indices; TA_NIL: absent */
    ta_rpc_error error;        /* code 0: none */
} ta_rpc_message;
typedef struct ta_rpc_parsing {
    ta_string_view line;
    ta_growable_arena *arena; /* the message's CBOR and tokens */
} ta_rpc_parsing;
ta_status ta_rpc_parse(const ta_rpc_parsing *parsing, ta_rpc_message *out);
/* The message without "jsonrpc", as ta_host_post takes it. */
ta_status ta_rpc_without_version(const ta_rpc_message *message, ta_cbor_writer *writer);
```

`tools/tinyactors/tool.h` gains, for the platform:

```c
/* Bytes from standard input as they come, into room (up to its capacity;
 * room->length set): TA_OK; TA_OK with length 0 at the end of input;
 * TA_ERROR_INVALID: it could not be read. */
ta_status (*receive)(void *context, ta_buffer *room);
```

and for the server (the prefix `tool_` is the command's, as in the rest
of `tool.h`; `tool_run` is already the commands' function type, so the
command is `tool_serve`):

```c
typedef struct tool_clock {
    ta_host_time now;
    uint64_t ticks_per_second; /* host.config's, read once */
} tool_clock;

typedef struct tool_server {
    tool *invocation;
    ta_host *host;
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_ecmascript_config limits;
    ta_bytes config;            /* CBOR, in the invocation's arena: a restore reuses it */
    ta_growable_arena request;  /* a mark per message; capped between messages */
    ta_rpc_lines lines;
    tool_clock clock;
    uint64_t batches;           /* takes that handed out effects */
    int held;                   /* a batch is open for the client */
} tool_server;

/* One message being answered. */
typedef struct tool_call {
    tool_server *server;
    const ta_rpc_message *message;
    ta_cbor_writer *result;     /* the result map, in the request arena */
    ta_rpc_error error;
} tool_call;
typedef void (*tool_method_run)(tool_call *call);
typedef enum tool_method_flags {
    TOOL_METHOD_WHILE_HELD = 1u << 0 /* allowed while a batch is held */
} tool_method_flags;
typedef struct tool_method {
    ta_string_view name;
    tool_method_run run;
    uint32_t flags;             /* tool_method_flags */
    uint32_t param_count;
    const ta_string_view *params; /* the params it takes: any other is -32602 unknown */
} tool_method;

/* A payload of quickjs from a value param ("data" or "cbor"): TA_OK, or the
 * call's error set. The payload moves to the host on success. */
typedef struct tool_value_param {
    ta_string_view json_name, cbor_name;
} tool_value_param;
ta_status tool_value_read(tool_call *call, tool_value_param names, ta_payload *out);
/* A payload as "<name>" (and its CBOR twin when the conversion changed it),
 * a member of the map the writer has open. */
typedef struct tool_value_out {
    ta_payload payload;
    uint32_t datamodel;
    ta_string_view json_name, cbor_name;
} tool_value_out;
void tool_value_write(tool_server *server, const tool_value_out *value, ta_cbor_writer *writer);

/* Take, handle, ack until nothing is pending (or stop held). */
typedef enum tool_drain_flags { TOOL_DRAIN_HOLD = 1u << 0 } tool_drain_flags;
ta_status tool_drain(tool_server *server, uint32_t flags);

void tool_serve(tool *t); /* the run command */
```

Results and notifications are written as CBOR with `ta_cbor_writer` and
turned into one JSON line by `ta_json_write`, so the server has one
writer of values (CBOR) and one converter, and a core reply passes through
the same converter.

### 7.3 Memory

| What | Owner, from | Lives until |
|---|---|---|
| the command line, the startup inputs, the config's CBOR | the invocation's arena (`tool.h`, "one invocation is one arena") | exit |
| a compile result | the compiler, through the platform's allocator | `ta_compile_free`, right after `ta_host_add_image` copies the image |
| the host's blocks | the host, through the platform's allocator | `ta_host_free` (end of input, or a restore) |
| quickjs | its own allocator | `ta_datamodel_ecmascript_free`, after the host |
| bytes of a line not yet whole | `ta_rpc_lines`, over the platform's allocator | exit |
| a message: its CBOR and tokens, value CBOR for `from_cbor`, the result's CBOR, each JSON line written | the request arena, above the mark taken when the line is read | the release after the response is written |
| an effect's fields, payload views, `to_wire`'s output | the host until the ack; the datamodel's output until its next call | converted into a notification line and written before the ack; nothing kept |
| a held batch | the host | the client's `host.run` or `host.effectsAck` |
| the OTLP bytes of a `TRACES` | the host until the ack; copied into the request arena | the release after `effect.traces` is written |
| a hibernation blob, a snapshot | the host until the ack (a blob), the server's `ta_buffer` in the request arena (a pack) | the response written |

The request arena keeps its chunks between messages and frees them when it
keeps more than a cap (256 KiB, the figure napi's host arena uses, napi
`AGENTS.md`, "An arena keeps at most its cap between calls"), so one
large snapshot does not stay for the server's life. Every allocation that
can fail is checked: a refusal is -32004 for that message and the server
goes on.

### 7.4 What `platform.c` gains

`receive`: one `read(2)` on descriptor 0 into the room (a `fread` would
wait for a whole chunk, and a client waits for its response). `write`
already flushes every call (`platform_write_all`), so each line reaches
the client when written. No clock: the clock is simulated.

### 7.5 Tests

- **`tests/unit/test_cli_json.c`**: round trips (JSON → CBOR → JSON) of
  every JSON value kind; the vectors of `tinyactors-test/suites/protocol/values.txt`
  written as JSON with the changes each reports; refusals with their
  offsets (malformed, not UTF-8, a lone surrogate, too deep); integers at
  ±2^64 and past; the escaping of every control character; the writer's
  allocator refusing at every size until it succeeds (writing-c, section 5).
- **Numbers**: a corpus of decimal texts and the bits of the double each
  reads as, generated once with the C library's `strtod` (the provenance
  written in the corpus's README) and committed under `tests/json/`, so
  the test reads fixtures the repository owns (writing-c, section 6);
  plus every double of a seeded sample written with `ta_format_double`
  reading back as itself.
- **`tests/unit/test_cli_rpc.c`**: lines split across feeds, `\r\n`,
  blank lines, a line past the limit skipped to its newline; envelopes
  refused with the right code, reason and path; batches.
- **Transcripts**, `tests/run/<case>/`: the charts, `config.json`,
  `transcript.jsonl` (`>` and `<` lines, as in section 6) and a README
  saying what it shows. `test_tool` runs `tinyactors run` on each, feeds
  the `>` lines and requires the `<` lines byte for byte (the simulated
  clock, `traceSeed` 0 and nonce 0 make them repeat). Cases: the order of
  section 6; a held send failed; a service invoked, answered, done and
  cancelled; a send to another system and its `message`; hibernate, wake
  and a migration's wakes; a snapshot restored; every refusal of section
  5.2; a forwarded method while held; `trace.configure` then
  `effect.traces`; values with bytes, `undefined` and an address (the
  `cbor` twins).
- **The scenarios through the server**: a task in `tinyactors-core`
  (`mise run run:scenarios`, part of `mise run test`, with the suite in
  its `#MISE sources` as `conform` has) turns each
  `tinyactors-test/suites/scenarios/*/steps.txt` into requests (`image`
  → `host.addImage`, `spawn` → `host.spawn`, `send` → `host.send`,
  `send-many` → `host.sendMany`, `advance` → `host.advanceTime`,
  `hibernate` → `host.hibernate` keeping the blob, `snapshot` and
  `restore`, `migrate` → `host.migrate`), each followed by `host.run`
  answering `effect.wake` with the kept blob, and requires the first
  `effect.log` labelled `Outcome` to be `pass`, on the ECMAScript
  conversion the suite already makes. The driver protocol itself is not
  reused (section 2); its scenarios are.

### 7.6 Documentation

- `tinyactors-core/docs/cli.md`: `run` in the commands' table, and a
  section "Run" with section 3, 4 and 5 of this proposal as the protocol's
  reference (evergreen: no ADR numbers in it).
- `tinyactors-core/docs/embedding.md` section 1 and `docs/requests.md`'s
  introduction: the command is a JSON-RPC transport of the host.
- `tinyactors-core/docs/layout.md`: `src/cli/json`, `src/cli/rpc`.
- `GLOSSARY.md`: **workbench** (the `tinyactors run` server: one host
  driven by a client over JSON-RPC), **held batch** (a batch the
  workbench leaves open for its client to answer).
- `acronyms.txt` has JSON, RPC and UTF; NDJSON appears in prose only.

### 7.7 Order of work, a commit each

1. `refactor(core)`: `ta_format_double` out of `cbor.c` into
   `base/format`, its unit test.
2. `feat(core)`: `src/cli/json` with its tests and number fixtures;
   `report.c` writes JSON strings with `ta_json_write_string`.
3. `feat(core)`: `src/cli/rpc` with its tests.
4. `refactor(core)`: `tool_image_make` split out of `tool_load`.
5. `feat(core)`: `tinyactors run`: the platform's `receive`, startup,
   forwarding, `host.addImage`, `host.spawn`, `host.send`,
   `host.advanceTime`, `host.run` without `hold`, the notifications of
   `SEND`, `LOG`, `DONE`, `ERROR`, `REQUEST`; the first transcripts;
   `docs/cli.md`.
6. `feat(core)`: `hold`, `host.effectsAck`, `host.fail`, services
   (`invocation`, `done`), `host.sendMany`, `host.terminate`, the lookups.
7. `feat(core)`: hibernation, sweep, migration and their notifications.
8. `feat(core)`: snapshots and restore.
9. `feat(core)`: telemetry (`effect.traces`, `effect.logs`,
   `host.traces`, `host.metrics`), `host.post`, `trace.end` at the end of
   input.
10. `test(core)`: the scenarios through the server.

Each passes `mise run test` in `tinyactors-core` and the projects the
root's table names for a change to core (napi and cpython build core's
amalgamation; nothing here enters it, so they should pass unchanged).

## 8. Open questions

1. **A real clock.** `--clock real` would need the server to wake at
   `nextDeadline` with no request in flight (a `poll` on standard input
   with a timeout in `platform.c`) and to write notifications nobody
   asked for. *Recommendation:* not in the first version; the workbench
   is for runs that repeat. Revisit when a client asks for it.
2. **A blob store in the server.** *Recommendation:* no; blobs are the
   client's (section 4.5). An agent juggling base64 is the cost; a
   `--store memory` would be the first policy the server owns.
3. **The decimal reader.** Correct rounding in freestanding C (7.2), or
   the platform's `strtod` passed in as a function. *Recommendation:* the
   freestanding reader with fixtures: the `cli` layer stays testable
   without the C library, and one number reader serves every JSON the
   command reads.
4. **`hold` or server-to-client requests.** JSON-RPC lets the server send
   requests (an `effect.send` the client must answer, as LSP does).
   *Recommendation:* `hold`: the clients named (agents, shell scripts)
   answer requests poorly, and `hold` keeps every message the client's
   initiative.
5. **`ta-conform` over the same server layer.** The server's methods
   cover most of the driver's (`image.add`, `actor.spawn`,
   `clock.advance`, `host.run`). *Recommendation:* not now. Once the
   server exists, decide whether `ta-conform` becomes a CBOR-framed edge
   over `serve.c`, which would leave one C driver of the loop in tools.
6. **Several hosts per server.** *Recommendation:* no: one host per
   process, as one per thread is the host's rule; two systems are two
   servers (section 4.6).
7. **Bytes in forwarded params** (`trace.spans.decode`'s `traces`).
   *Recommendation:* `host.post` with base64 CBOR, and `host.traces` and
   `effect.traces` answering decoded spans; no `$`-key convention
   (section 5.1).
8. **Effects in results instead of notifications.** Collecting a run's
   effects into `host.run`'s result reads well in `jq`, but a forwarded
   post's result must be core's, so `trace.end`'s spans would need a
   second place. *Recommendation:* notifications only.
9. **Experimental.** *Recommendation:* mark the protocol experimental in
   `docs/cli.md` until a second client (an MCP adapter, a test harness in
   another repository) uses it; it is 0.x either way.

## Checked so far

Claims carry `path:line`, a primary source or a measurement; estimates
say so. Checked by the author against the repository:

- **confirmed:** core's posted methods are JSON-RPC-shaped CBOR meant for
  such a transport (`requests.md:9`, `:18`); a post while a batch is open
  is answered only after the ack (`requests.md:50`), hence the refusal
  while held; an answer or failure given before the ack is what a parked
  sender sees (ADR 0017:78-90); the `cli` layer is in no released library
  and already writes JSON (`mise/core.rb:55-58`, `report.c:189`); core
  has no JSON reader and no decimal reader (searched `src`, `include`,
  `tools`: none but quickjs's `js_atod`); `$` keys are ordinary values
  (`protocol.md:108`); MCP's stdio framing is newline-delimited (its
  specification, quoted); RFC 8949's sections 6.1 and 6.2 say what 5.1
  says.
- **plausible:** the command's size after linking quickjs (an estimate
  from `ta-conform`'s measured size; settled by building it); the line
  counts (settled by writing it); that converting a payload with
  quickjs's `payload_to_cbor` during dispatch is safe (`ta-conform`'s
  `host.run` does it, driver.md; settled by the transcripts under the
  sanitizer build).
- **pending:** the refutation of sections 3 to 7 by a fresh agent
  (`reasoned-analysis`, step 8). The agent that wrote this could not start
  one; the session that asked for it should, before the status moves to
  Proposed.
