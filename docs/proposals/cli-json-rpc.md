# `tinyactors run`: running charts from the command over JSON-RPC 2.0

**Status:** Proposed (2026-11-25). A fresh agent refuted the first
draft, and the maintainer then decided four points (2026-11-25): the
config is optional, `ioProcessors` is a map in the protocol, the protocol
has a JSON Schema, and the command, a separate distributable, may grow and
keep its own JSON. Both are folded in; section 9 lists each finding and
decision and what it changed.
**Touches:** tinyactors-core: `src/cli` (the command's own layer: a JSON
reader and writer, NDJSON framing), `src/base/format` (the shortest-decimal
writer and the quoted-string escaper, moved there from `cbor.c` unchanged),
`tools/tinyactors` (a `run` command, its schema, platform additions),
`mise/core.rb`, `tests/`, `docs/cli.md`, `docs/requests.md` (one error
code), `docs/distribution.md` (the command's size); the root's
`GLOSSARY.md`. No public header, no released library, no binding.
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps
win.

## 1. Summary

`tinyactors run [options] [INPUT...]` makes one host, adds the inputs'
image, and then serves **JSON-RPC 2.0 on standard input and output, one
message per line**. Its own methods are the host's C calls, under the
area `run.` (`run.spawn` is `ta_host_spawn`, `run.turn` is the loop
around `ta_host_run`); every other method is forwarded to `ta_host_post`,
so the control plane (`actor.state`, `host.stats`, `trace.watch`, ...)
needs no code of its own. What actors do comes back as **notifications,
one per effect, written before the ack of their batch and before the
response to the request that drained them**. The clock is simulated: it
moves only when the client says so. Values are JSON, converted at the
edge to and from the CBOR value table with RFC 8949's rules, with a
base64url CBOR twin where JSON cannot hold a value. A JSON Schema (2020-12)
describes every message, so a coding agent can generate a client from it.
No config is needed: `tinyactors run order.scxml` works.

It is a workbench: an agent or a CI job loads charts, spawns actors, sends
events, answers the actors' sends, moves the clock and reads what
happened, without a language binding and with the same release that
compiles and checks the charts.

## 2. Whom it serves, what already answers them, who pays, what is out

Following [docs/roles.md](../roles.md).

**First: the application developer, through an agent or a CI job.** The
role's own questions: why did this actor end up in this state, which
transition fired with what data, what does my chart send and what happens
when the answer is a failure or never comes, does the new version still
pass. They decide how to fix a chart and whether to merge it.

**Then: the maintainers.** A bug report becomes a transcript (the
client's lines and the server's) that runs as a test.

| Question | What answers it today | Why that is not enough |
|---|---|---|
| does the chart compile, what does it hold, what changed | `tinyactors check`, `inspect`, `states`, `diff`, `lint`, `path` (`tinyactors-core/docs/cli.md`) | nothing runs |
| what does the chart do with these events | a binding (`@tinyactors/node`, `tinyactors` on PyPI) and a test in its language | needs Node.js or Python, a package, a test file per question; an agent writes a program to ask one question |
| run a scenario from the outside | `ta-conform` (`tinyactors-core/tools/conform`) | not shipped (a test program, `mise/core.rb:263`); speaks length-prefixed CBOR with integer statuses and no notifications (`tinyactors-test/docs/driver.md:21-31`); its `host.run` handles only transport sends and notifications (driver.md, `host.run`) |
| the actor's state, stats, traces | the posted methods (`tinyactors-core/docs/requests.md`) | CBOR, from inside a process that owns a host |

Core shaped its control plane for a transport like this one: "requests,
replies and notifications are one ordered stream, so that a JSON-RPC
transport is a pipe" (`requests.md:9`), and "a transport that speaks JSON
on its wire converts at its edge" (`requests.md:18`), while "core never
reads or writes JSON" (`requests.md:15-16`). ADR 0012 keeps a raw `post`
out of language APIs "until a JSON-RPC transport exists"
(`docs/adr/0012-lifetimes-follow-the-loop.md:208`).

**JSON is the command's own** (the maintainer, 2026-11-25: the command
is a client of core and a separate distributable, which may be bigger and
have its own JSON implementation). The JSON reader and writer live in
`src/cli`, the layer only the command uses (`mise/core.rb:55-58`, "not in
a released library"; its only other users are its own unit tests,
`mise/core.rb:204-205`). This does not reverse ADR 0009, decision 5, which
took JSON out of "core, the bindings and the examples"
(`docs/adr/0009-one-host-two-queues.md:142-143`): core's libraries, their
amalgamation and the bindings stay JSON-free, and the command, already
writing JSON for its reports (`src/cli/report.c:189`), reads it too.

**Who pays:**

| Cost | What |
|---|---|
| per event, per actor, in core's libraries | nothing: the shortest-decimal writer and the quoted-string escaper move from `cbor.c` into the existing `base/format.{h,c}` unchanged, so `ta_cbor_diagnostic` and the command share them; no new code enters `libtinyactors` |
| the command's download | **accepted** (the maintainer, 2026-11-25): the release `tinyactors` is about 200 KB (`tinyactors-core/docs/distribution.md:26`; 171,048 B in this checkout's `build/release/bin`). Linking the host and quickjs makes it about 1.7 MB: `ta-conform`, which links the same host, compiler and quickjs layers, is 1,689,760 B (measured 2026-11-25). An estimate until built; `distribution.md` is updated with the measured size. Every user of `check`, `graph` or `diff` downloads quickjs with it |
| code | a JSON reader and writer and NDJSON framing in `src/cli` (estimated 800 lines), the server and its schema in `tools/tinyactors` (estimated 1,500 lines of C and 1,000 of schema), tests and transcripts. Unverified |
| maintainers | a third driver of the loop beside the bindings and `ta-conform`: a change to a host call's contract (`docs/boundary.md`) or to `docs/embedding.md` reaches it too |
| API surface | a wire protocol, documented in `docs/cli.md` and defined by its JSON Schema (section 4.7); 0.x, so it may change in place (root `AGENTS.md`, "Before 1.0"); one error code in `requests.md`'s table (section 5.2) |
| every binding | nothing |
| the C embedder, the operator, the capacity planner | nothing; not served (below) |

**Out of scope, and whom that leaves unserved:**

- **Production.** One host, one client, standard input: an operator runs
  a binding or their own C loop. Operators and capacity planners are not
  served.
- **Other transports** (TCP, HTTP, WebSocket): stdio only.
- **Several hosts per server.** One host per process, as one host per
  thread is the rule (`tinyactors-core/docs/embedding.md:250`). Two
  systems are two servers, and the client is the transport between them
  (section 4.6).
- **A real-time clock** in the first version (open question 3).
- **Datamodels other than quickjs.** `datamodel="python"` charts run in
  the Python binding; the native ECMAScript datamodel is Node's.
- **A store.** Hibernation blobs go to the client (section 4.5): "the
  application owns storage and policy" (root `AGENTS.md:15`).
- **A REPL for people.** The client is a program.
- **MCP.** A separate proposal ([mcp-server.md](mcp-server.md)), written
  against the dropped ring protocol; an MCP adapter could later sit over
  this server, but nothing here is shaped for it.

**Not a binding.** It is a C embedder of the host, like `ta-conform`,
with a wire protocol on top. It keeps the bindings' rule all the same:
**every method is a host call, a forwarded posted message, or the
handling of an effect**, and nothing core does is done again.

**Security.** The client is trusted: it started the process, and it can
name any file the process can read (`files`, `--config`) or write
(`path`). Charts run in quickjs with its limits (`max_ops`,
`memory_limit`; `tinyactors-core/docs/host-config.md`, "The datamodels'
limits"), and quickjs-libc's `std` and `os` modules are not built
(`mise/core.rb:80`), so a chart reads no file and opens no socket. A chart
that never idles is bounded by `run.turn`'s run count.

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

- **Lines.** A blank line is ignored; a `\r` before the `\n` is dropped; a
  last line without `\n` at the end of input is a message. A line longer
  than `--max-message` (default 64 MiB, the server's own bound, so a
  client that sends no newline cannot grow it without end) is answered
  with -32600 and skipped to its newline. After a line longer than 1 MiB
  the server gives the line buffer's block back, so one large message
  does not stay for the server's life.
- **Envelope.** JSON-RPC 2.0: a request has `"jsonrpc": "2.0"`, `method`,
  `params` (an object; absent: none) and `id` (integer, string or null);
  without `id` it is a notification, applied with no response. A message
  that is not one is answered with -32600 and `"id": null`, one that is
  not JSON with -32700.
- **Repeated keys** are refused in every JSON object the server reads
  (-32700, `data.path` naming the key): the readers behind it disagree on
  which one wins (core's `ta_cbor_member` finds the first, `cbor.h`;
  quickjs's `wire_read` defines the properties in order, so the last
  stays, `src/datamodel_ecmascript/datamodel_ecmascript.c:3123-3138`).
- **Batches** (an array of requests) are taken: each element runs in
  order, one at a time, as if sent alone; their notifications are written
  as they come, and the responses as one array after the last (none when
  every element was a notification). An empty array is one -32600
  response, as JSON-RPC 2.0 says. Core refuses batches and leaves them to
  the transport (`requests.md:60`).
- **Pipelining** is allowed and changes nothing: the server reads a line,
  answers it, then reads the next. Responses come in request order.
- **Order on standard output:** every notification a request caused comes
  before its response. Standard error is free text for people.

### 3.2 Startup

```text
tinyactors run [--config FILE] [--system ID] [--io-processor TYPE[=LOCATION]]...
               [-W] [--boot] [--wall-start NS] [--max-ops N] [--memory-limit BYTES]
               [--realm auto|isolated] [--max-message BYTES] [--schema] [INPUT...]
```

**Nothing is required** (the maintainer, 2026-11-25): with no option the
server runs with every default, and `tinyactors run order.scxml` works.

| Option | Meaning | Default |
|---|---|---|
| `--config FILE` | the host config (`tinyactors-core/docs/host-config.md`) as JSON, in the protocol's shape: core's keys, but `ioProcessors` a map (below) | none: every key takes core's default, but `systemID` |
| `--system ID` | the config's `systemID`; refused (usage) when the config holds another | `tinyactors-run` (below) |
| `--io-processor TYPE[=LOCATION]` | one I/O processor, repeatable; added after the config's, a type both name refused (usage) | none |
| `INPUT...` | an image, a manifest or charts, read as every command reads them (`docs/cli.md`, "Inputs"), added as segment 0 before the first message; with `-W`, a warning refuses it | none: a client adds images with `run.addImage` |
| `--boot` | `ta_host_boot` of segment 0 (a manifest's boot actors) | off |
| `--wall-start NS` | the wall clock's start, nanoseconds since the Unix epoch (section 4.3) | 0 |
| `--max-ops`, `--memory-limit`, `--realm` | `ta_datamodel_ecmascript_config` (`include/tinyactors/datamodel_ecmascript.h:61-71`) | quickjs's, 0 in each field |
| `--max-message BYTES` | the longest line taken (3.1) | 64 MiB |
| `--schema` | write the protocol's JSON Schema (section 4.7) to standard output and exit 0 | — |

**The default system id is `tinyactors-run`.** Core requires one and has
no default, so that two systems never share one by accident
(`host-config.md:57`, "`systemID` has none"); the server supplies one
because a lone workbench's id appears only in its own addresses. Two
systems sharing an id matters only where an address or a blob crosses
between them: a client connecting two servers as each other's transport
(section 4.6), or a blob given to another server's `run.wake` (refused
only by identity, the system id with nonce 0). A client that connects
servers names each with `--system`, as section 4.6 says; one that does
not would see a send to the other server's address delivered locally (a
local address, `protocol.md`, "Other systems"). A fixed name is chosen
over one derived from the first chart's name because a derived id would
differ between runs that load other charts, and could still collide; a
chart's name can also be no valid label (`protocol.md`, "Addresses").

**`ioProcessors` is a map in the protocol.** SCXML's `_ioprocessors` maps
a processor's type to its location; core's config takes a list only
because a send effect names its processor by position (entry i is
`IO_PROCESSOR` i + 1, `host-config.md:59`), and the datamodel builds the
`_ioprocessors` map from that list. A client never sees those numbers
(`effect.send` carries the `type`), so the protocol takes
`{"<type>": {"location": "<location>"}}` (`location` optional, as in
core's entries) and the server writes core's list in the object's order,
then `--io-processor`'s in the order given; a JSON object cannot repeat a
type, because repeated keys are refused (section 3.1). A forwarded
`host.config` answers core's list, as core writes it.

- **The environment** the server gives `ta_host_create`: the platform's
  allocator, the one quickjs datamodel, the default trace policy,
  `nonce` 0, an `error` buffer. With the config's `system.traceSeed`
  absent (core's default is 0, host-config.md) and a nonce of 0, the
  host's own bytes repeat between runs of one transcript (the bindings
  draw both at random, napi `AGENTS.md`, "The binding picks the trace
  seed"). What quickjs does not repeat is section 4.3's.
- **The clock** starts at monotonic 0 and wall time `--wall-start`.
- **A refusal at startup** (an input that does not compile, a config core
  refuses) is told on standard error, `error`'s reply converted to JSON,
  and the command exits with status 1 before it reads a message.
- The server posts `host.config` once and drains it at creation, as the
  bindings do, for `system.ticksPerSecond`.
- `platform.c` ignores `SIGPIPE`, so a client that went away makes a
  write fail (exit 3) instead of killing the process.

### 3.3 Shutdown

At the end of standard input the server **acks a held batch** (its
parked senders go on, as if the client had taken their sends without an
answer: success is silence, ADR 0014, decision 5), drains what is
pending, posts `trace.end` when the host has an encoder and drains its
spans to the client (section 4.4), frees the host and then quickjs, and
exits 0. There is no `shutdown` method: closing standard input is the
request, as for `ta-conform` (`driver.md:25`).

| Exit status | When (`docs/cli.md`, "Exit status") |
|---|---|
| 0 | the input ended |
| 1 | an input or the config was refused at startup |
| 2 | the command line was not understood |
| 3 | standard input could not be read, standard output could not be written (the client went away), or memory ran out at startup or for the line buffer. Memory a single message cannot get is that message's -32004, and the server goes on |

## 4. Methods and notifications

### 4.1 The rule

- **The server's own methods are `run.<verb>`, each a C call
  `ta_host_<verb>`**, its params the call's struct fields in camelCase
  (as `driver.md` names them: "takes their struct fields in camelCase").
  The verbs are the bindings' (`spawn`, `send`, `fail`, `hibernate`,
  `wake`, `migrate`, `snapshot`; napi `AGENTS.md`).
- **The area `run.` is the server's.** Core files new whole-host methods
  under `host.` (`requests.md`, "Adding a method", step 1), so a server
  method in `host.` could be shadowed by a core method added later. Core
  has no area `run`, and a method core adds in any other area reaches
  the client with no change here. When core ever adds an area `run`,
  `requests.md` says the command owns it (a line in "Adding a method").
- **Every other method is forwarded**: the message without its `jsonrpc`
  member, converted to CBOR, given to `ta_host_post`, with the id
  replaced (below); its `REPLY`'s `WIRE` converted back, with the
  client's id, is the response. A message without `id` is a posted
  notification, as JSON-RPC's, and gets no response in either protocol
  (`requests.md`, "The envelope"). A method core does not know is core's
  -32601. Core's posted `actor.send` stays reachable, forwarded;
  `run.send` is `ta_host_send` with every field of `ta_event_in`.
- **The server's posts have their own ids.** Every post (the client's
  forwarded ones, and the server's own: `host.config` at startup,
  `trace.spans.decode` for a `TRACES`, `trace.end` at shutdown) carries
  as its id the handle of a row in a pending `ta_table`
  (`include/tinyactors/table.h`), which says whose post it is: a
  forwarded request (answered with the client's id, held by the call
  until its response is written) or one of the server's. A `REPLY` is
  matched by that handle, so the client's ids and the server's never
  collide, as both bindings match their typed methods' replies.

### 4.2 The methods

Values (`data`, `params`) are JSON, or `cbor` for a value JSON cannot hold
(section 5). Every byte string in the protocol is base64url without
padding (RFC 4648, section 5), the alphabet RFC 8949 section 6.1 uses for
bytes: values' `cbor` twins, blobs, snapshots, images, OTLP. A target is a
session (an integer) or a local address (`ta_host_resolve`). Every result
is a JSON object.

| Method | Params | Result | Calls |
|---|---|---|---|
| `run.addImage` | one of `files` (paths, read as `INPUT`s are), `sources` (`[{name, text}]`, the first the default boot chart; a `src` names another by its name), `image` (a `.tai`); `warningsAsErrors` | `segment`, `charts` (`[{name, chart}]`), `diagnostics` (as `check -f json` writes them) | `ta_compile_charts` or `ta_compile_system`, `ta_compile_image`, `ta_host_add_image`, `ta_compile_free` |
| `run.boot` | `segment` | `{}` | `ta_host_boot` |
| `run.chart` | `name` | `segment`, `chart` | `ta_host_chart` |
| `run.spawn` | `chart` (a name: its current version) or `segment` and `chart` (an index); `name`, `params` or `paramsCBOR`, `traceparent` | `session`, `address` | `ta_host_chart`, `from_wire` of the chart's datamodel (section 5.1), `ta_host_spawn`, `ta_address_format` |
| `run.send` | `target`, `event`; `data` or `cbor`; `sendid`, `origin`, `origintype`, `raw`, `traceparent`; for a service the client runs: `invocation` (the `token` of its `effect.invoke`) and `done` (bool) | `{}` | `ta_host_resolve`, `ta_host_locate` and the target chart's datamodel, `from_wire`, `ta_host_send` (`TA_EVENT_IN_FROM_INVOKE`, `TA_EVENT_IN_INVOKE_DONE`) |
| `run.sendMany` | `events`: `run.send`'s params each | `accepted`: the accepted prefix's length | `ta_host_send_many` |
| `run.fail` | `target`, `sendid`; or `target` and `invocation` | `{}` | `ta_host_send` with `TA_EVENT_IN_ERROR_COMMUNICATION` (with `TA_EVENT_IN_FROM_INVOKE` for an invocation) |
| `run.terminate` | `target` | `{}` | `ta_host_terminate` |
| `run.resolve`, `run.actorByName` | `address`; `name` | `session` (`null`: none) | `ta_host_resolve`; `ta_host_actor_by_name` |
| `run.advanceTime` | `by` or `to`: ticks (integer), or a duration as SCXML writes one (`"30s"`, `"150ms"`); `wallNs` | `time`, `wallNs` | `ta_host_advance_time` |
| `run.turn` | `budget` (fuel per run, default 10,000), `runs` (at most, default 1,000), `hold` (bool) | `status`, `detail`, `runs`, `fuel`, `time`, `nextDeadline` (`null`: none) | the turn of section 4.4 |
| `run.ack` | — | `{}` | `ta_host_effects_ack` of a held batch; -32002 when none is held |
| `run.hibernate` | `session` | `blob` | `ta_host_hibernate`, copied, then the ack of the batch it opened (section 4.4) |
| `run.wake` | `session`, `blob` | `{}` | `ta_host_wake` |
| `run.sweep` | `maxScan`, `limit` | `sessions` | `ta_host_sweep` |
| `run.migrate` | `chart`, `session`, `wake`, `timeout` (ticks or a duration) | `migration` | `ta_host_migrate` |
| `run.snapshot` | `path` (optional: written whole through the platform) | `snapshot`, or `bytes` with `path` | `ta_datamodel_ecmascript_snapshot`, `ta_host_snapshot_pack` |
| `run.restore` | `snapshot` or `path` | `segments` | `ta_host_snapshot_unpack`, `ta_host_snapshot_image`, `ta_host_restore` (with the server's config), `ta_datamodel_ecmascript_restore`; the old host and datamodel freed after |
| `run.traces` | — | `spans`, as `trace.spans.decode` answers | `ta_host_traces`, `trace.spans.decode` posted with the view, the ack, the drain that answers it |
| `run.metrics` | — | `otlp` | `ta_host_metrics`, copied, then the ack |
| `run.post` | `message` (CBOR) | `reply` (CBOR), or nothing for a notification | `ta_host_post`, the message as given: the escape for bytes JSON cannot carry (a forwarded `trace.spans.decode`'s `traces`) and for a reply's exact values |
| `run.schema` | — | `schema`: the protocol's JSON Schema (section 4.7) | none |
| anything else | as core's | core's `result` or `error` | `ta_host_post` (`trace.*`, `host.stats`, `host.config`, `actor.list`, `actor.get`, `actor.context`, `actor.state`, `actor.send`) |

Defaults the server owns, and why: `budget` and `runs` bound a turn so a
chart that never idles cannot hang the client (`ta-conform` bounds its
loop the same way, at 1,000 runs, `driver.md`, `host.run`); `--max-message`
bounds a line. No other default: everything else is core's.

### 4.3 Time

- **Simulated.** Monotonic time starts at 0 and moves only with
  `run.advanceTime`; wall time starts at `--wall-start` and moves by the
  same duration (a `wallNs` param sets it). Nothing moves the clock behind
  the client's back, and nothing runs between requests.
- **Ticks for the clock, as core counts them.** `time`, `nextDeadline`,
  `by`, `to` and `timeout` are the host's ticks at `system.ticksPerSecond`
  (default 1e9, nanoseconds), as `actor.get`'s `due` is (`requests.md`).
  Core's other units stay core's in forwarded methods: `trace.watch` takes
  `seconds` on the host's clock (`requests.md:142`), and spans' `start`
  and `end` are nanoseconds since the Unix epoch from the wall clock
  (`requests.md:409`), so with the default `--wall-start` of 0 spans are
  dated in 1970. That keeps transcripts repeatable; a client that wants
  real dates passes `--wall-start`.
- **A duration text** (`"30s"`) is read by core's reader of SCXML delays,
  `ta_core_parse_duration` (`src/core/core_internal.h:384`), and
  converted with `ticksPerSecond`, so the client writes time as the chart
  does. It is internal to core: the command includes that header, as
  `tool.h` already includes `image/records.h`, and links core through the
  host it now uses (section 7.1).
- **Advancing and running are separate**, as in C: `run.advanceTime`
  then `run.turn`, or both in one JSON-RPC batch. The W3C environment's
  "move to the next deadline when idle" is `{"to": nextDeadline}` from
  the last turn's result.
- **What does not repeat.** quickjs seeds `Math.random` and its hash from
  `gettimeofday` (`third_party/quickjs-ng/quickjs.c:2879-2883`), and
  `Date` reads the real clock, not the simulated one. A chart that reads
  either gives other bytes on each run; the transcripts the repository
  keeps use neither (open question 4).

### 4.4 The turn, and when the server acks

The server keeps two flags. **`open`**: a batch is open, set by a take
that returned effects and by an output call (`ta_host_hibernate`,
`ta_host_metrics`, `ta_host_traces`, `ta_host_logs`) made with no batch
open, which opens a batch of its own where `ta_host_effects` returns 0
(`host.h:78-89`; `docs/boundary.md`, "Phases", D), and cleared by the ack.
**`held`**: the open batch is left open for the client. Both bindings
track an open batch so, never by the call depth (napi `AGENTS.md`, "An
output taken with no batch open opens a batch of its own").

`run.turn` is embedding.md's loop (section 2) with the effects handled by
the table below:

```
run.turn:
    if held: ta_host_effects_ack, open = held = 0     (the client answered it)
    drain
    repeat at most `runs` times:
        run = ta_host_run(host, budget)
        drain; stop with status "held" when it held a batch
        EFFECTS, EFFECTS_FULL -> again
        IDLE                  -> answer status "idle", nextDeadline
        YIELDED               -> again (status "yielded" when `runs` is spent)
        EXHAUSTED, FAULT      -> answer it, detail the pool or ta_status_name

drain (hold: whether the turn was given `hold`):
    loop:
        n = ta_host_effects(host, &effects)
        if n == 0 and not open: return
        if n > 0: open = 1, batches += 1
        for each effect, in order: handle it (the table below), writing its notification
        if hold and the batch has a waiting send only the client decides: held = 1; return
        ta_host_effects_ack(host); open = 0
```

**The ack.** The server acks a batch after it has written every
notification of it, unless `run.turn` was given `hold` and the batch has
a send whose sender waits (`TA_EFFECT_FLAG_WAITING`) and that only the
client can decide: a send to an I/O processor of the config, to the
transport, or to a service. Then `run.turn` answers `"status": "held"`
with the batch open, and the sender stays parked until the client acks
(`run.turn` again, or `run.ack`). What the client sends before that is in
the sender's queue when it goes on: an answer, or `run.fail`'s
`error.communication`, which is how SCXML describes a failed `<send>`
(ADR 0017, decision 2, `docs/adr/0017-one-way-out-and-sends-that-wait.md:78-90`).
Without `hold`, the batch is acked at once, the sender goes on, and an
answer the client gives later arrives as an ordinary event, as a
binding's answer given after its handler returned does (napi `AGENTS.md`,
"Materialize what outlives the ack").

**Outputs taken outside a turn.** `run.hibernate`, `run.metrics` and
`run.traces` with no batch open open one (`open`), copy what they answer
(or post the decode, below), and ack it before the response; called while
a batch is held, `run.hibernate` joins it and acks nothing, and the other
two are refused (they would decide the held batch's fate or need its
ack).

**While a batch is held** (phase D, `docs/boundary.md:41-53`):

| Method | While held |
|---|---|
| `run.send`, `run.sendMany`, `run.fail`, `run.terminate`, `run.spawn`, `run.wake` | allowed: inputs apply now, their effects come after the ack (`boundary.md`, "Host calls") |
| `run.chart`, `run.resolve`, `run.actorByName`, `run.schema` | allowed: lookups |
| `run.hibernate` | allowed: the blob joins the held batch and lives until its ack; it is copied into the response at once |
| `run.sweep` | allowed (`ta_host_sweep` in I and D) |
| `run.migrate` | allowed (`ta_host_migrate` in I and D); its effects come in later runs |
| `run.boot` | allowed (I and D) |
| `run.advanceTime` | allowed (I and D); the clock moves, nothing runs |
| `run.turn`, `run.ack` | allowed: they ack the held batch |
| `run.traces`, `run.metrics` | refused, -32002 `state`: they would ack the client's batch or wait for its ack |
| forwarded methods, `run.post` | refused, -32002 `state`: core answers a post made while a batch is open only in the take after the ack (`requests.md:50`), so the response could not come before the next request |
| `run.snapshot`, `run.addImage`, `run.restore` | refused, -32002 `state`: core refuses a snapshot with a batch open (`host.h:460`), and the server keeps images and restores to quiescent hosts only |

Every method that is not refused drains after its call when no batch is
held (a forwarded post's batch holds its reply and the effects the method
appended, `trace.end`'s `TRACES`), so every effect reaches the client
before the response to the request that caused it.

**Each effect, and what reaches the client.** Every notification carries
`batch` (the server's count of batches) and `index` (the effect's place in
it). A field the effect does not set is left out, never `null`.

| Effect | The server | Notification `params` (besides `batch`, `index`) | The client may |
|---|---|---|---|
| `TA_EFFECT_SEND`, `IO_PROCESSOR` i + 1 | nothing more; `hold` stops here | `effect.send`: `session`, `type` (the processor's type: the server keeps the types in the order it wrote core's list, section 3.2, and reads entry i), `target`, `event`, `sendid`, `data`/`cbor`, `traceparent`, `waiting` | answer: `run.send` to `session` with `sendid`, `origintype` `type`, `traceparent`; or `run.fail` |
| `TA_EFFECT_SEND`, `IO_PROCESSOR` 0 (the transport) | as above | `effect.send` with `message`: `{target, event, data, sendid, origin, traceparent}` (ADR 0017, decision 4) | pass `message` to another server's `run.send` (section 4.6); `run.fail`; nothing (success is silence, ADR 0014, decision 5) |
| `TA_EFFECT_SEND`, `IO_PROCESSOR` `TA_NIL` | as above | `effect.send` with `invocation` (the token) and `segment` | answer as the service: `run.send` with `invocation` |
| `TA_EFFECT_INVOKE` | nothing: the client is every service | `effect.invoke`: `session`, `segment`, `invocation` (token), `type`, `src`, `invokeid`, `data`, `restored` | `run.send` with `invocation` (and `done`); `run.fail` with `invocation` |
| `TA_EFFECT_UNINVOKE` | nothing | `effect.uninvoke`: `segment`, `invocation` | stop its stand-in |
| `TA_EFFECT_REQUEST` | `ta_host_resume` with `TA_ERROR_INVALID` before the ack, as both bindings do (the parent gets `error.execution`; embedding.md, section 4) | `effect.request`: `session`, `src` | nothing |
| `TA_EFFECT_WAKE` | nothing (no store) | `effect.wake`: `session`, `migration` | `run.wake` with the blob `run.hibernate` gave it |
| `TA_EFFECT_LOG` | nothing | `effect.log`: `session`, `label`, `text` | nothing |
| `TA_EFFECT_DONE` | nothing | `effect.done`: `session`, `name`, `final`, `data`/`cbor` (the donedata) | nothing |
| `TA_EFFECT_ERROR` | nothing | `effect.error`: `session`, `status` (`ta_status_name` of `FLAGS`) | nothing |
| `TA_HOST_EFFECT_MIGRATED` | nothing | `effect.migrated`: `session`, `segment`, `chart`, `migration` | nothing |
| `TA_HOST_EFFECT_MIGRATION_DONE` | reads `ta_host_migration_failed` and `_pending` during dispatch | `effect.migrationDone`: `migration`, `migrated`, `failed` (`[{session, status}]`), `pending` | `run.wake` the pending |
| `TA_HOST_EFFECT_SEGMENT_FREED` | nothing | `effect.segmentFreed`: `segment` | nothing |
| `TA_HOST_EFFECT_TRACES` | `ta_host_traces`, then, during dispatch, `trace.spans.decode` posted with the view as its `traces` (a post is allowed in phase D, `boundary.md:48`; the post reads its message during the call) under a pending row of the server's | `effect.traces`: `spans`, written when the decode's `REPLY` is dispatched: in the batch after this one's ack, so after a held batch only once the client acks it | nothing |
| `TA_HOST_EFFECT_LOGS` | `ta_host_logs`, written into the notification from the view | `effect.logs`: `otlp` (core has no posted decoder for logs) | nothing |
| `TA_HOST_EFFECT_REPLY` | the pending row its id names: a forwarded request's response, or a post of the server's | — | — |
| `TA_HOST_EFFECT_NOTIFICATION` | none is defined (`requests.md`, "Notifications") | written as it is, its `method` and `params` converted | — |

Every view an effect's notification reads (fields, payloads, the OTLP
bytes) is read and written out before the ack, so nothing the host hands
out is kept past it, held batch or not.

### 4.5 Hibernation, migration, snapshots

The blobs are the client's: `run.hibernate` answers the blob (copied into
the response before the batch it joins or opens is acked,
`docs/boundary.md`, `ta_host_hibernate`), an `effect.wake` says which
session needs its blob, and `run.wake` brings it back. A migration's
`effect.wake`s carry its id. `run.snapshot` packs the host with quickjs's
state, refused by core unless the host is quiescent; `run.restore` makes
a new host from it with the server's config and datamodel limits, frees
the old ones, and keeps the clock at the snapshot's time. Its first
`run.turn` announces every invocation that was running as an
`effect.invoke` with `restored` (`host.h:488-495`).

### 4.6 Two systems

A client that starts two servers (`--system warehouse`,
`--system storefront`) is the transport between them: it takes
`effect.send`'s `message` from one and gives it to the other's `run.send`
as it is. `run.send` takes `target` as an address and resolves it, and
passes `origin`, so a chart's reply to `_event.origin` comes back out as
the second server's `effect.send` for the transport. This is the
bindings' `send.message` and `host.send(message)`, decision 4 of ADR 0017.

### 4.7 The protocol as a JSON Schema

So that a coding agent can generate a client with confidence (the
maintainer, 2026-11-25), the protocol is defined once, machine-readable,
in **JSON Schema 2020-12** (<https://json-schema.org/draft/2020-12>):

- **What it holds.** One document with `$defs` for: each method's params
  and result (`run.*` and every forwarded method of `requests.md`); each
  notification's params (`effect.*`); the request, response and
  notification envelopes; the error object (`code`, `message`, `data` with
  `reason`, `path`, `status`); the run statuses; and the value encoding:
  `value` (any JSON), `cbor` (a string of the base64url alphabet without
  padding, `^[A-Za-z0-9_-]*$`), and for each value field its twin, with
  `data` and `cbor` exclusive on input (`not` both `required`). At the top,
  two `oneOf`s: `clientMessage` (a request per method, its `method` a
  `const` and its `params` the method's `$def`, a notification, or a batch
  of them) and `serverMessage` (a response, an `effect.*` notification, or
  a batch of responses), so a whole line validates and the method names
  are in the schema itself. Params and results set
  `"additionalProperties": false`, as the server refuses a param it does
  not take; numbers that are session ids, ticks and counts are
  `"type": "integer"` with `"minimum": 0` and, where core bounds them, a
  `maximum`, with a `description` saying they reach 2^64-1.
- **Where it lives.** `tinyactors-core/tools/tinyactors/run.schema.json`,
  beside the command that answers to it, and embedded in the command at
  build time (a generated C file holding it as a `static const char`
  array, as the build already generates sources from data). `docs/cli.md`
  links it, and each release's archive carries it beside `bin/tinyactors`.
- **How the command gives it.** `tinyactors run --schema` writes it and
  exits, so a generator needs no running server; and the method
  `run.schema` answers it as a JSON value, for a client already connected.
  Both write the same embedded bytes.
- **How it stays true** (tests, section 7.5): every line of every
  transcript is validated against `clientMessage` or `serverMessage`; a
  test fails when a `run.*` method is in the server's method table and not
  in the schema or the other way round (it reads both: the table through
  `run.schema`'s list and a call per method, which must not answer -32601);
  and one fails when a method listed in `requests.md` (which says "if one
  is not listed here, it does not exist") has no `$def`, or the schema
  has a forwarded method core answers -32601 to.
- **Why not OpenRPC** (<https://spec.open-rpc.org/>, 1.4.x): it describes
  JSON-RPC methods with JSON Schema, which suits the requests and their
  results, but its Schema Objects are JSON Schema draft 07, and it
  describes only the methods a client calls: the `effect.*` notifications
  the server sends, half of what a client must handle, have no place in it
  (the specification covers methods without results, not
  server-to-client messages). A 2020-12 schema describes both directions
  and validates whole lines. An OpenRPC document of the `run.*` and
  forwarded methods could be generated from the schema later for tools
  that read OpenRPC (open question 9).

## 5. Values and errors

### 5.1 JSON and the value table

Neutral values are CBOR (`tinyactors-core/docs/protocol.md`, "Values").
The server converts at the edge, with RFC 8949's own rules
(<https://www.rfc-editor.org/rfc/rfc8949.html>, sections 6.1 and 6.2),
and never invents a JSON form:

| CBOR | JSON written (6.1) | JSON read (6.2) |
|---|---|---|
| unsigned and negative integers | a number, exact over the whole range -2^64 … 2^64-1 | a number whose value is an integer in that range, written with or without a fraction or exponent (`7`, `7.0`, `7e0`): the value table's floats are "numbers that are not integers" (`protocol.md:98`). 6.2's default range is ±(2^53-1) and "other ranges are allowed"; the server takes 64 bits. Outside the range, a float |
| floats | the shortest decimal that reads back as the double (`-0.0` kept); NaN, ±Infinity as `null` | a number that is not an integer, or `-0.0`, rounded to the nearest double, ties to even |
| text | a string | a string: UTF-8 checked, `\u` escapes joined in pairs, a lone surrogate refused |
| bytes | base64url text without padding | — (text is text) |
| arrays, maps with text keys | arrays, objects, keys in order | the same; a repeated key refused (section 3.1) |
| `false`, `true`, `null` | themselves | themselves |
| `undefined` | `null` | — |
| tag 32 (an address) | its text, the tag dropped | — (text) |

**Where JSON loses something, the CBOR comes too.** When writing a value
changed it (bytes, `undefined`, a non-finite float, a tag), the field is
followed by its twin `cbor`, the neutral value's bytes: `data` and `cbor`,
`params` and `paramsCBOR`. The client reads `data` and looks at `cbor`
only when it is there. On input, `cbor` instead of `data` gives a value
JSON cannot write (bytes, `undefined`, an address). This is `driver.md`'s
`actor.send` (`data` or `cbor`), not a new shape.

**No escape inside JSON.** A wrapper object such as `{"$bytes": "..."}`
is not used: a key starting with `$` is an ordinary key of the value table
(`protocol.md:108`), so any such convention would misread a value.

**Whose payload.** An input's payload is read by the datamodel of the
target's chart: `ta_host_send` stamps the event with that chart's
datamodel (`src/system/schedule.c:429`), and `ta_event_in` carries no
datamodel of its own. So the server finds the target's chart
(`ta_host_locate`, the segment's image, `ta_image_chart_datamodel`) and
makes the payload with that datamodel's `from_wire`: quickjs for
`"ecmascript"`. A chart whose datamodel reads no values (the null
datamodel, no `from_wire`) refuses `data` with -32602 `range`. The
conversion core does since 2026-11-25 (`ta_core_payload_convert`,
`src/core/core.c:525`, commit `0adae4ef`) does not change this: it
converts a payload that carries its maker's datamodel index (an effect's,
an event's between charts); an input's payload carries none, so the
caller must make it in the right datamodel.

**Forwarded replies are core's, converted.** `actor.context` writes
`undefined` for a `<data>` with no value (`requests.md`, `actor.context`),
which JSON shows as `null`. A client that must tell them apart uses
`run.post` and reads the CBOR.

**Large integers.** Session ids are exact as JSON numbers. A JavaScript
client reading them with `JSON.parse` loses bits past 2^53: segment k
numbers its sessions from k·2^32 + 1 (`host.h:13-14`), so a host stays
below 2^53 until segment 2^21. A client that cares parses integers as
BigInts; protocol.md asks for lossless session ids, and the server keeps
them so.

### 5.2 Errors

The error object is core's (`requests.md`, "The envelope"): `{"code",
"message", "data": {"reason", "path"}}`, with core's codes and reasons, so
a client handles forwarded and own errors alike. The server's own errors
add `data.status`, `ta_status_name` of the call's status, where a C call
refused.

| Condition | Code | Reason |
|---|---|---|
| not JSON, or a repeated key | -32700 | `malformed` |
| not a request: not an object, `jsonrpc` not `"2.0"`, no `method`, `params` not an object, a bad `id`, an empty batch, a line past `--max-message` | -32600 | `type`, `missing`, `limit` |
| no such method | -32601 (core's, forwarded) | `unknown` |
| a param missing, of the wrong type, unknown, out of range; `data` for a chart that reads no values | -32602 | `missing`, `type`, `unknown`, `range` |
| `TA_ERROR_STALE` | -32001 | `stale` |
| `TA_ERROR_STATE`; a method refused while a batch is held; `run.ack` with none held | -32002 | `state` |
| `TA_ERROR_LIMIT` | -32003 | `limit` |
| `TA_ERROR_NO_MEMORY` | -32004 | `memory` |
| `TA_ERROR_INVALID` | -32005 | `invalid` |
| data `from_wire` cannot read | -32006 | `malformed` |
| `TA_ERROR_UNSUPPORTED` (a snapshot or hibernation of a value outside the table) | -32007 | `unsupported`: a new code, as `requests.md`'s "Adding a method" step 4 asks of a new reason, added to its table as the command's so core never gives -32007 another meaning |
| `TA_ERROR_EXECUTION`, `TA_ERROR_COMMUNICATION`, `TA_ERROR_FATAL`, `TA_ERROR_ABORTED` (`include/tinyactors/types.h:121-130`) | -32603 | `internal`: JSON-RPC's internal error; no call the server makes documents these, so one is a bug to report (unverified for every call until each call's refusals are listed in the implementation) |

A run's outcome is not an error: `run.turn` answers `"status": "fault"`
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
{"payments": {}}}` (milliseconds, so `nextDeadline` reads easily; without
it, `tinyactors run --io-processor payments order.scxml` runs the same
charts with nanosecond ticks). The system id is the default,
`tinyactors-run`.
`>` is a line the client writes, `<` one the server writes; `…` elides a
traceparent. The ids, sessions and times are what this design gives; the
fuel numbers are illustrative.

```text
$ tinyactors run --config shop.json order.scxml
> {"jsonrpc":"2.0","id":1,"method":"run.spawn","params":{"chart":"order","name":"order-17","params":{"total":4200}}}
< {"jsonrpc":"2.0","id":1,"result":{"session":1,"address":"tinyactors://tinyactors-run/#_scxml_1"}}
> {"jsonrpc":"2.0","id":2,"method":"run.send","params":{"target":1,"event":"checkout","data":{"card":"tok_visa"}}}
< {"jsonrpc":"2.0","id":2,"result":{}}
> {"jsonrpc":"2.0","id":3,"method":"run.turn","params":{"hold":true}}
< {"jsonrpc":"2.0","method":"effect.send","params":{"batch":1,"index":0,"session":1,"type":"payments","target":"charges","event":"charge","sendid":"pay","data":{"amount":4200,"card":"tok_visa"},"traceparent":"00-…-01","waiting":true}}
< {"jsonrpc":"2.0","id":3,"result":{"status":"held","runs":1,"fuel":41,"time":0,"nextDeadline":30000}}
```

The order is parked after its `<send>`. Its state cannot be read while
the batch is held:

```text
> {"jsonrpc":"2.0","id":4,"method":"actor.state","params":{"session":1}}
< {"jsonrpc":"2.0","id":4,"error":{"code":-32002,"message":"a batch is held: answer its sends, then run.turn or run.ack","data":{"reason":"state","path":""}}}
> {"jsonrpc":"2.0","id":5,"method":"run.send","params":{"target":1,"event":"charge.ok","sendid":"pay","origintype":"payments","traceparent":"00-…-01","data":{"charge":"ch_81"}}}
< {"jsonrpc":"2.0","id":5,"result":{}}
> {"jsonrpc":"2.0","id":6,"method":"run.turn"}
< {"jsonrpc":"2.0","method":"effect.log","params":{"batch":2,"index":0,"session":1,"label":"order","text":"paid ch_81"}}
< {"jsonrpc":"2.0","method":"effect.done","params":{"batch":2,"index":1,"session":1,"name":"order-17","final":"paid","data":{"charge":"ch_81"}}}
< {"jsonrpc":"2.0","id":6,"result":{"status":"idle","runs":1,"fuel":37,"time":0,"nextDeadline":null}}
```

A second order whose processor never answers, without `hold`: the send is
reported and acked, and the deadline decides.

```text
> [{"jsonrpc":"2.0","id":7,"method":"run.spawn","params":{"chart":"order","params":{"total":990}}},
   {"jsonrpc":"2.0","id":8,"method":"run.send","params":{"target":2,"event":"checkout","data":{"card":"tok_expired"}}},
   {"jsonrpc":"2.0","id":9,"method":"run.turn"}]
< {"jsonrpc":"2.0","method":"effect.send","params":{"batch":3,"index":0,"session":2,"type":"payments","target":"charges","event":"charge","sendid":"pay","data":{"amount":990,"card":"tok_expired"},"traceparent":"00-…-01","waiting":true}}
< [{"jsonrpc":"2.0","id":7,"result":{"session":2,"address":"tinyactors://tinyactors-run/#_scxml_2"}},{"jsonrpc":"2.0","id":8,"result":{}},{"jsonrpc":"2.0","id":9,"result":{"status":"idle","runs":2,"fuel":44,"time":0,"nextDeadline":30000}}]
> {"jsonrpc":"2.0","id":10,"method":"run.advanceTime","params":{"by":"30s"}}
< {"jsonrpc":"2.0","id":10,"result":{"time":30000,"wallNs":30000000000}}
> {"jsonrpc":"2.0","id":11,"method":"run.turn"}
< {"jsonrpc":"2.0","method":"effect.log","params":{"batch":4,"index":0,"session":2,"label":"order","text":"payment failed: payment.timeout"}}
< {"jsonrpc":"2.0","id":11,"result":{"status":"idle","runs":1,"fuel":23,"time":30000,"nextDeadline":null}}
> {"jsonrpc":"2.0","id":12,"method":"actor.state","params":{"session":2}}
< {"jsonrpc":"2.0","id":12,"result":{"value":"open","history":{}}}
```

(The batch above is shown on three lines; on the wire it is one.) The
spawn, send and turn in one batch share one array response; the
notification comes first because the run that made it ran before the
array was written. Forwarded `actor.state` is core's reply, converted,
with the client's id.

## 7. Implementation outline

### 7.1 Where the code goes

| Piece | Where | Why there |
|---|---|---|
| JSON text to CBOR, CBOR to JSON text | `src/cli/json.{h,c}` | the command's own layer: freestanding, used by the command and its tests alone, in no released library (`mise/core.rb:55-58`, `:204-205`) |
| the shortest decimal of a double, a string quoted as JSON escapes it | into the existing `src/base/format.{h,c}`: `ta_format_double` (Dragon4, `src/base/cbor.c:466`) and `ta_format_quoted` (`cbor.c:441`), moved out of `cbor.c` unchanged | `ta_cbor_diagnostic` keeps calling them; the command's JSON and `report.c`'s JSON strings (`report.c:189`) call the same ones, so floats and escapes are written once |
| NDJSON lines and the JSON-RPC envelope | `src/cli/rpc.{h,c}` | freestanding, unit-tested without a host |
| the command: options, startup, the message loop | `tools/tinyactors/run.c` | the commands' layer, compiled without the C library's headers |
| the methods, the pending posts and the drain | `tools/tinyactors/serve.c`, `tools/tinyactors/effects.c` | the same |
| the schema | `tools/tinyactors/run.schema.json`, embedded by the build | section 4.7 |
| reading standard input as it comes; `SIGPIPE` ignored; `strtod` if open question 2 takes it | `tools/tinyactors/platform.c` | the only file that uses the C library |

**One CBOR walker or two.** The command's CBOR-to-JSON writer walks a CBOR
item as `ta_cbor_diagnostic` does (`include/tinyactors/cbor.h:80-89`:
for maps, arrays, text, integers, `true`, `false` and `null` its notation
"reads as its JSON"), and writing-c, section 2 asks for one writer of an
output kind. A JSON mode of that walker would put JSON into
`libtinyactors`, which ADR 0009 keeps JSON-free; so the command has its
own walk, about a hundred lines over `ta_cbor_reader`, and shares
everything else with the diagnostic notation: the float and string
writers above. The two outputs are different kinds (one is JSON, one is
not), and what they share is shared.

`tool_load` (`tools/tinyactors/input.c:138`) both makes the image and
tells what went wrong (`tool_complain`, the report), and reads files into
the invocation's arena, which lives until exit (`tool.h:11-14`), so each
`run.addImage` would grow it for the server's life. It splits into
`tool_image_make`, which takes the arena it reads and compiles into (the
request arena for `run.addImage`, the invocation's at startup) and gives
the image and its diagnostics, and `tool_load` (that, then telling); every
caller is migrated in the same change.

`mise/core.rb`: the `cli` layer gains `json` and `rpc`; the `tool` layer
(`mise/core.rb:73`) uses `host` and `datamodel_ecmascript` too, which
brings `system` and `core`, so `ta_core_parse_duration` links; a build
step embeds the schema; new unit tests `test_cli_json`, `test_cli_rpc`.

### 7.2 Interfaces

`src/cli/json.h`:

```c
/* cli/json.h — JSON text (RFC 8259) to one CBOR item and back, at the
 * command's JSON-RPC wire: RFC 8949, section 6.2 reading (integers to 64
 * bits, integral numbers as integers: docs/protocol.md has a float be a
 * number that is not an integer) and 6.1 writing. The command's own: in no
 * released library. Freestanding: no C library, no recursion (explicit
 * stacks of TA_JSON_MAX_DEPTH), no allocation but the writer's allocator,
 * the output's and the arena given for keys. */
typedef enum ta_json_limits {
    TA_JSON_MAX_DEPTH = 64 /* containers open at once: TA_CBOR_DEPTH_MAX */
} ta_json_limits;

/* Where a text was refused, and why (a static text). */
typedef struct ta_json_failure {
    size_t offset;
    ta_string_view reason;
} ta_json_failure;

/* A decimal that is not an integer, as the nearest double (ties to even):
 * the reader's own, or the platform's (open question 2). */
typedef struct ta_json_decimal {
    void *context;
    ta_status (*read)(void *context, ta_string_view text, double *out);
} ta_json_decimal;

typedef struct ta_json_reading {
    ta_string_view text;          /* one JSON text, UTF-8 */
    ta_cbor_writer *writer;       /* receives one item; containers TA_CBOR_COUNT_LATER */
    ta_growable_arena *arena;     /* the keys of the objects open, to refuse a repeated one */
    const ta_json_decimal *decimal;
    ta_json_failure failure;      /* set on TA_ERROR_INVALID and TA_ERROR_LIMIT */
} ta_json_reading;
/* TA_OK; TA_ERROR_INVALID: not JSON, not UTF-8, a lone surrogate, a key
 * repeated in an object; TA_ERROR_LIMIT: nested deeper than
 * TA_JSON_MAX_DEPTH; TA_ERROR_NO_MEMORY: the writer or the arena refused. */
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
```

`src/base/format.h` gains, moved from `cbor.c` with their callers there:

```c
/* The shortest decimal that reads back as `value` (Steele and White's
 * Dragon4), with a point or an exponent; NaN, Infinity, -Infinity. */
void ta_format_double(ta_output *out, double value);
/* `text` quoted and escaped as JSON escapes it. */
void ta_format_quoted(ta_output *out, ta_string_view text);
```

**The decimal reader** stays open (question 2). Correct rounding needs
more than `cbor.c`'s Dragon4 arithmetic: its 44-word big integer is sized
for its own output, 2^1076 × 10 (`src/base/cbor.c:470-473`), and an exact
comparison of an 800-digit mantissa scaled by 10^-1100 needs more than
3,600 bits. Either the platform's `strtod` (the command has the C library
in `platform.c`, which gives `ta_json_decimal`), or a port of the
algorithm Go's `strconv` uses (an Eisel–Lemire fast path, then the
"simple decimal conversion" over at most 800 significant digits with a
truncated flag acting as a sticky digit; `atof.go`, `eisel_lemire.go`,
`decimal.go` in Go's `src/strconv`, unverified until ported).

`src/cli/rpc.h`:

```c
/* cli/rpc.h — JSON-RPC 2.0, one message per line. */
typedef struct ta_rpc_lines {
    ta_output pending;  /* bytes after the last newline, over the caller's allocator */
    size_t limit;       /* the longest line taken (--max-message) */
    size_t skipping;    /* past the limit: bytes dropped up to the next newline */
} ta_rpc_lines;
typedef struct ta_rpc_lines_config {
    const ta_allocator *allocator;
    size_t limit;
} ta_rpc_lines_config;
void ta_rpc_lines_init(ta_rpc_lines *lines, const ta_rpc_lines_config *config);
/* Bytes as they came: TA_OK, or TA_ERROR_NO_MEMORY (nothing taken). A feed
 * after the end of input (an empty chunk) makes the unended last line whole. */
ta_status ta_rpc_lines_feed(ta_rpc_lines *lines, ta_bytes chunk);
/* The next whole line, without its "\r\n" or "\n": 1, 0 when none is
 * whole yet; a view valid until the next feed. `too_long`: a line past
 * the limit was dropped (answered -32600). After a line past 1 MiB the
 * pending block is given back and starts small again. */
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
    ta_cbor_document document;   /* tokens in the arena */
    uint32_t kind;               /* ta_rpc_kind */
    uint32_t method, params, id; /* token indices; TA_NIL: absent */
    ta_rpc_error error;          /* code 0: none */
} ta_rpc_message;
typedef struct ta_rpc_parsing {
    ta_string_view line;
    ta_growable_arena *arena; /* the message's CBOR, tokens and the reader's keys */
} ta_rpc_parsing;
ta_status ta_rpc_parse(const ta_rpc_parsing *parsing, ta_rpc_message *out);
/* The message for ta_host_post: without "jsonrpc", its id replaced. */
typedef struct ta_rpc_forward {
    const ta_rpc_message *message;
    uint64_t id; /* the pending row's handle */
} ta_rpc_forward;
ta_status ta_rpc_forward_write(const ta_rpc_forward *forward, ta_cbor_writer *writer);
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

/* What a pending post is, by the row its id names. */
typedef enum tool_post_kind {
    TOOL_POST_FORWARDED = 0, /* a client's request: answered with its id */
    TOOL_POST_CONFIG = 1,    /* host.config at creation */
    TOOL_POST_SPANS = 2,     /* trace.spans.decode of a TRACES: effect.traces */
    TOOL_POST_SPANS_RESULT = 3, /* the same for run.traces: its result */
    TOOL_POST_END = 4        /* trace.end at the end of input */
} tool_post_kind;
typedef struct tool_post {
    uint32_t kind;           /* tool_post_kind */
    uint32_t pad;
    struct tool_call *call;  /* TOOL_POST_FORWARDED, TOOL_POST_SPANS_RESULT: answered within it */
} tool_post;

typedef struct tool_server {
    tool *invocation;
    ta_host *host;
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_ecmascript_config limits;
    ta_bytes config;            /* CBOR, in the invocation's arena: a restore reuses it */
    ta_growable_arena request;  /* a mark per message; capped between messages */
    ta_table posts;             /* tool_post rows: a post's id is its handle */
    ta_rpc_lines lines;
    tool_clock clock;
    uint64_t batches;           /* batches handed out */
    int open;                   /* a batch is open: a take or an output opened it */
    int held;                   /* the open batch is left for the client */
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
    uint32_t flags;               /* tool_method_flags */
    uint32_t param_count;
    const ta_string_view *params; /* the params it takes: any other is -32602 unknown */
} tool_method;

/* A payload, made by the target chart's datamodel, from a value param
 * ("data" or "cbor"): TA_OK, or the call's error set. It moves to the host
 * on success. */
typedef struct tool_value_param {
    ta_string_view json_name, cbor_name;
    uint64_t target; /* the session whose chart's datamodel reads it */
} tool_value_param;
ta_status tool_value_read(tool_call *call, const tool_value_param *param, ta_payload *out);
/* A payload as "<name>" (and its CBOR twin when the conversion changed it),
 * a member of the map the writer has open. */
typedef struct tool_value_out {
    ta_payload payload;
    uint32_t datamodel;
    ta_string_view json_name, cbor_name;
} tool_value_out;
void tool_value_write(tool_server *server, const tool_value_out *value, ta_cbor_writer *writer);

/* Take, handle, ack until nothing is pending and no batch is open, or stop
 * held; 1 when it held a batch. */
typedef enum tool_drain_flags { TOOL_DRAIN_HOLD = 1u << 0 } tool_drain_flags;
int tool_drain(tool_server *server, uint32_t flags);

void tool_serve(tool *t); /* the run command */
```

Results and notifications are written as CBOR with `ta_cbor_writer` and
turned into one JSON line by `ta_json_write`, so the server has one
writer of values (CBOR) and one converter, and a core reply passes through
the same converter.

### 7.3 Memory

| What | Owner, from | Lives until |
|---|---|---|
| the command line, the startup inputs, the config's CBOR | the invocation's arena (`tool.h:11-14`) | exit |
| a compile result | the compiler, through the platform's allocator | `ta_compile_free`, right after `ta_host_add_image` copies the image |
| a `run.addImage`'s files and image | the request arena (`tool_image_make` given it) | the message's release |
| the host's blocks | the host, through the platform's allocator | `ta_host_free` (end of input, or a restore) |
| quickjs | its own allocator | `ta_datamodel_ecmascript_free`, after the host |
| the pending posts' rows | `posts`, a `ta_table` over the platform's allocator | each row: its `REPLY`'s dispatch; the table: exit |
| bytes of a line not yet whole | `ta_rpc_lines`, over the platform's allocator; given back after a line past 1 MiB | exit |
| a message: its CBOR and tokens, the reader's keys, value CBOR for `from_wire`, the result's CBOR, each JSON line written, a forwarded post's message | the request arena, above the mark taken when the line is read | the release after the response is written |
| an effect's fields, payload views, `to_wire`'s output, a `TRACES`' or `LOGS`' OTLP view | the host until the ack; the datamodel's output until its next call | read into a notification line (or into the decode post's message, read during `ta_host_post`) and written before the ack; nothing kept |
| a held batch | the host | the client's `run.turn` or `run.ack`, or the end of input |
| a hibernation blob, metrics | the host until the ack | copied into the response before the server acks the batch the call opened |
| a snapshot | the server's `ta_buffer` in the request arena | the response written |

The request arena keeps its chunks between messages and frees them when it
keeps more than a cap (256 KiB, the figure napi's host arena uses, napi
`AGENTS.md`, "An arena keeps at most its cap between calls"). Every
allocation that can fail is checked: a refusal is -32004 for that message
and the server goes on.

### 7.4 What `platform.c` gains

`receive`: one `read(2)` on descriptor 0 into the room (a `fread` would
wait for a whole chunk, and a client waits for its response). `SIGPIPE`
ignored at `main`. `write` already flushes every call
(`platform_write_all`), so each line reaches the client when written. No
clock: the clock is simulated.

### 7.5 Tests

- **`tests/unit/test_cli_json.c`**: round trips (JSON → CBOR → JSON) of
  every JSON value kind; the vectors of `tinyactors-test/suites/protocol/values.txt`
  written as JSON with the changes each reports; refusals with their
  offsets (malformed, not UTF-8, a lone surrogate, a repeated key, too
  deep); integers at ±2^64 and past, `7.0` and `1e3` as integers, `-0.0`
  as a float; the escaping of every control character; the writer's and
  the arena's allocators refusing at every size until they succeed
  (writing-c, section 5). `test_cbor`'s diagnostic cases stay as they are
  and pass through the moved `ta_format_double` and `ta_format_quoted`.
- **Numbers**: a corpus of decimal texts and the bits of the double each
  reads as, generated once with the C library's `strtod` (the provenance
  written in the corpus's README) and committed under `tests/json/`, with
  the hard cases (800 digits and more, halfway cases, subnormals, the
  largest and smallest), so the test reads fixtures the repository owns
  (writing-c, section 6); plus every double of a seeded sample written
  with `ta_format_double` reading back as itself.
- **`tests/unit/test_cli_rpc.c`**: lines split across feeds, `\r\n`,
  blank lines, an unended last line, a line past the limit skipped to its
  newline and the block given back; envelopes refused with the right code,
  reason and path; batches, the empty batch; the forwarded message's id
  replaced and `jsonrpc` gone.
- **Transcripts**, `tests/run/<case>/`: the charts, `config.json`,
  `transcript.jsonl` (`>` and `<` lines, as in section 6) and a README
  saying what it shows. `test_tool` runs `tinyactors run` on each, feeds
  the `>` lines and requires the `<` lines byte for byte; no chart of the
  corpus reads `Date` or `Math.random` (section 4.3). Cases: the order of
  section 6; a held send failed; a service invoked, answered, done and
  cancelled; a send to another system and its `message`; `run.hibernate`
  with no batch open and while held, then the next `run.turn` (the output
  batch acked); `run.metrics` the same; a migration's wakes; a snapshot
  restored; `trace.configure` then `effect.traces`, with and without
  `hold`, and `run.traces`; every refusal of section 5.2; every row of the
  held table; the end of input with a batch held; values with bytes,
  `undefined` and an address (the `cbor` twins); `data` for a null-datamodel
  chart refused.
- **The schema** (section 4.7), a task `mise run run:schema` in
  `tinyactors-core`, part of its `test`: every transcript line validated
  against `clientMessage` or `serverMessage` with a 2020-12 validator (a
  Ruby gem pinned with the root's tools; which one supports 2020-12 fully
  is unverified); the server's method table against the schema's, both
  ways; `requests.md`'s methods against the schema's forwarded ones, both
  ways; `tinyactors run --schema` byte for byte the file.
- **The scenarios**: how the conformance suite reaches the server is open
  question 1 (an adapter driven by `tinyactors-test`'s runner,
  recommended).

### 7.6 Documentation

- `tinyactors-core/docs/cli.md`: `run` in the commands' table, and a
  section "Run" with sections 3, 4 and 5 of this proposal as the
  protocol's reference (evergreen: no ADR numbers in it).
- `tinyactors-core/docs/requests.md`: -32007 `unsupported` in its code
  table as the command's, the area `run` the command's in "Adding a
  method", and in its introduction that the command is a JSON-RPC
  transport of the host.
- `tinyactors-core/docs/embedding.md` section 1: the command runs charts.
- `tinyactors-core/docs/layout.md`: `src/cli/json`, `src/cli/rpc`,
  `tools/tinyactors/run.schema.json`.
- `tinyactors-core/docs/distribution.md`: "about 200 KB" replaced with the
  command's measured size, and that it runs charts with quickjs inside;
  the schema in the archive.
- `GLOSSARY.md`: **workbench** (the `tinyactors run` server: one host
  driven by a client over JSON-RPC), **held batch** (a batch the workbench
  leaves open for its client to answer).
- `acronyms.txt` has JSON, RPC and UTF; NDJSON appears in prose only.

### 7.7 Order of work, a commit each

1. `refactor(core)`: `ta_format_double` and `ta_format_quoted` out of
   `cbor.c` into `base/format`; `report.c`'s strings through
   `ta_format_quoted`; their unit tests.
2. `feat(core)`: `src/cli/json` with its tests and number fixtures, the
   decimal reader as open question 2 decides.
3. `feat(core)`: `src/cli/rpc` with its tests.
4. `refactor(core)`: `tool_image_make` split out of `tool_load`, taking
   its arena.
5. `feat(core)`: `tinyactors run`: the platform's `receive` and `SIGPIPE`,
   startup, the pending posts and forwarding, `run.addImage`, `run.spawn`,
   `run.send`, `run.advanceTime`, `run.turn` without `hold`, the
   notifications of `SEND`, `LOG`, `DONE`, `ERROR`, `REQUEST`; the first
   transcripts; the schema of these methods, `--schema`, `run.schema` and
   `run:schema`; `docs/cli.md`, `requests.md`, `distribution.md`.
6. `feat(core)`: `hold`, `run.ack`, `run.fail`, services (`invocation`,
   `done`), `run.sendMany`, `run.terminate`, the lookups, the end of input
   with a batch held.
7. `feat(core)`: hibernation (the output batch and its ack), sweep,
   migration and their notifications.
8. `feat(core)`: snapshots and restore.
9. `feat(core)`: telemetry (`effect.traces`, `effect.logs`, `run.traces`,
   `run.metrics`), `run.post`, `trace.end` at the end of input.
10. The scenarios through the server, as open question 1 decides.

Steps 6 to 9 each extend the schema with their methods and
notifications, so `run:schema` holds at every commit.

Each passes `mise run test` in `tinyactors-core` and the projects the
root's table names for a change to core (napi and cpython build core's
amalgamation; step 1 changes `cbor.c`, which is in it, so their tests
matter there; nothing else here enters it).

## 8. Open questions

Settled by the maintainer (2026-11-25), and no longer open: JSON in the
command's own layer (section 2), the command's size accepted (section 2),
an optional config with a default system id and `ioProcessors` as a map
(section 3.2), a JSON Schema of the protocol (section 4.7).

1. **The scenarios through the server** (the refutation's finding 6). The
   first draft had a task in core parse `steps.txt` and require `pass`,
   which reimplements the suite's runner (its parsing, its conversion of
   `.txml`) and gets the canaries wrong (`suites/scenarios/outcome-compile`,
   `outcome-fail`, `outcome-timeout` expect other outcomes).
   *Recommendation:* an adapter that speaks the driver protocol to
   `tinyactors-test`'s runner and `run.*` to the server, as
   `tinyactors-napi/conform.mjs` does over Node's API, run as a `binding`
   kind (it cannot offer `value.encode`), in Ruby with the root's tasks,
   linted as the adapters are. The driver protocol stays the runner's wire,
   and the server's stays JSON.
2. **The decimal reader** (finding 7). The platform's `strtod`, given to
   the reader as `ta_json_decimal` by `platform.c`, or a port of Go's
   `strconv` algorithm in `src/cli` (7.2). *Recommendation:* `strtod` from
   the platform: the command is a separate distributable with the C
   library in its platform file, the reader then stays a few lines, and
   the number fixtures (7.5) check the result on each release target,
   where the C library differs (musl on Linux, libSystem on macOS,
   `distribution.md:24-26`); that musl's and Apple's `strtod` round
   correctly is unverified until the fixtures pass on both. The port
   remains the answer if one of them fails.
3. **A real clock.** `--clock real` would need the server to wake at
   `nextDeadline` with no request in flight (a `poll` on standard input
   with a timeout in `platform.c`) and to write notifications nobody asked
   for. *Recommendation:* not in the first version; the workbench is for
   runs that repeat.
4. **Determinism** (finding 8). quickjs seeds `Math.random` and its hash
   from `gettimeofday` (`quickjs.c:2879-2883`) and `Date` reads the real
   clock, so transcripts repeat only for charts that use neither. Options:
   (a) say so, and keep the repository's transcripts free of them; (b) a
   request to core: a seed and a clock for quickjs in
   `ta_datamodel_ecmascript_config`, so `Date` reads the host's wall clock
   and `Math.random` repeats. *Recommendation:* (a) now; (b) when a
   transcript needs `Date` (an order stamped with the time is likely), as a
   request to core with that scenario, since it changes a public config
   struct (and whether quickjs offers the hooks is unverified).
5. **A blob store in the server.** *Recommendation:* no; blobs are the
   client's (section 4.5).
6. **`hold` or server-to-client requests.** JSON-RPC lets the server send
   requests (an `effect.send` the client must answer, as LSP does).
   *Recommendation:* `hold`: the clients named (agents, shell scripts)
   answer requests poorly, and `hold` keeps every message the client's
   initiative.
7. **`ta-conform` over the same server layer.** *Recommendation:* not now.
   With the adapter of question 1 in place, decide whether `ta-conform`
   becomes that adapter's C twin, leaving one C driver of the loop in
   tools.
8. **Effects in results instead of notifications.** *Recommendation:*
   notifications only: a forwarded post's result must be core's, so
   effects drained with it would need a second place.
9. **The schema's format.** A JSON Schema 2020-12 document, or an OpenRPC
   document (section 4.7). *Recommendation:* 2020-12, because OpenRPC's
   draft-07 schemas cannot describe the server's notifications; generate
   an OpenRPC document of the client-to-server methods from it only when a
   tool asks for one.
10. **Experimental.** *Recommendation:* mark the protocol experimental in
    `docs/cli.md` until a second client uses it; it is 0.x either way.

## 9. The refutation, and what it changed

A fresh agent checked the first draft against the repository
(2026-11-25), and the maintainer then decided four points. The findings
and decisions, and what became of each:

| # | Finding | Now |
|---|---|---|
| 1 | a held batch broke the `TRACES` lifetime: the copy lived in a request arena released before the ack; `host.traces` acked the client's held batch | the decode is posted during dispatch from the view, under a server-owned pending row; `run.traces` and `run.metrics` refused while held (4.4) |
| 2 | a batch opened by an output alone was never acked | the `open` flag beside `held`; the output methods ack what they opened (4.4) |
| 3 | ADR 0009, decision 5 cited as support while JSON came back into core | the maintainer: JSON is the command's own, in `src/cli`, which no released library holds; why that keeps decision 5 (section 2) |
| 4 | the command's size against `distribution.md:26` | the maintainer: accepted; in "who pays", and `distribution.md` updated (7.6) |
| 5 | repeated keys: quickjs keeps the last, core finds the first | refused everywhere (3.1) |
| 6 | the scenarios task reimplemented the runner and misread the canaries | open question 1: an adapter driven by the runner |
| 7 | the 44-word big integer cannot round 800-digit decimals | open question 2: the platform's `strtod` recommended, Go's `strconv` algorithm the alternative (7.2) |
| 8 | quickjs's `Math.random`, hash seed and `Date` do not repeat | stated (4.3); transcripts avoid them; open question 4 |
| 9 | end of input with a held batch, an unended last line, `[]`, an ack with none held, `SIGPIPE`, exit 3 for memory | each specified (3.1, 3.3, 4.2, 5.2) |
| 10 | the held table lacked rows | a row per method (4.4) |
| 11 | `unsupported` reused -32005; four statuses unmapped | -32007 of its own; -32603 for the four (5.2) |
| 12 | integral JSON numbers, two base64 alphabets, payloads of the wrong datamodel | integral numbers are integers; base64url everywhere; the payload made by the target chart's datamodel, and why `ta_core_payload_convert` does not cover inputs (5.1) |
| 13 | `tool_load` grew the invocation's arena per `addImage`; the line buffer never shrank | `tool_image_make` takes the request arena; the line block given back after 1 MiB (7.1, 7.2) |
| 14 | `ta_core_parse_duration` is internal and outside the tool layer; `ta_json_write` was a second walker | the dependency stated (4.3, 7.1); the float and string writers shared through `base/format`, the walk itself the command's, and why (7.1) |
| — | the maintainer: the config is optional | every key defaults; the system id `tinyactors-run`, and when it can collide (3.2) |
| — | the maintainer: `ioProcessors` as SCXML's map | the protocol's config takes a map, written as core's list in a stated order (3.2) |
| — | the maintainer: a machine-readable protocol | a JSON Schema 2020-12, `--schema`, `run.schema`, tests that keep it true; OpenRPC weighed (4.7) |
| — | name collisions with core's future `host.*` methods | the server's methods moved to the area `run.` (4.1) |
| — | citations (`core.rb`, `driver.md`, `requests.md:15-16`, `ta-conform`'s `to_wire`) | corrected |

It confirmed, and the design keeps: forwarding and core's -32601;
replies as `REPLY` effects; posts refused while held, which keeps
responses in order; `hold` cannot deadlock (the client always may ack);
an answer or failure before the ack is what a parked sender sees (ADR
0017, decision 2); inputs, lookups and `ta_host_resume` in phase D
(`boundary.md:47-81`); snapshots refused with a batch open
(`host.h:460`); `REQUEST` answered with `TA_ERROR_INVALID`; restored
invocations announced; `MIGRATION_DONE`'s reads in dispatch; the
transport's success as silence (ADR 0014); the message between two
servers (ADR 0017, decision 4); the worked example; the C interfaces (at
most three parameters, enums, names, explicit stacks); flushing; nonce 0
and `traceSeed` 0.

**Still unverified:** the line counts and the size of the command (settled
by building it); that the platforms' `strtod` rounds correctly (settled by
the number fixtures on each target); a Ruby validator for 2020-12; that converting a payload with the datamodel's
`to_wire` during dispatch is safe (`ta-conform` does it,
`tools/conform/datamodel.c:77-78`; settled by the transcripts under the
sanitizer build); whether quickjs can take a seed and a clock (open
question 4).
