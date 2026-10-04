# An MCP server: connect an agent to a system

**Status:** Proposed (2026-10-04), **experimental**
**Experimental:** the MCP server is an experimental feature. Its tools, resources and transcripts may change or be removed between releases without a deprecation period, and a host enables it explicitly.
**Depends on:** [the ring protocol](ring-proto/README.md). Core talks to its host through three rings, and there is no host kernel.
**Touches:** a new shared C library, `tinyactors-mcp`, beside `tinyactors-otlp`; tinyactors-napi and tinyactors-test (bind it). Core gains nothing beyond the ring protocol. The DAP adapters and tinyactors-vscode were removed on 2026-09-30, ahead of it.
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

Connect an agent to a running system, and it can do things there: list the
actors, read their configuration and data, send them events and read their
replies, load them, and move a wedged one out of its state. It is a
debugger for an agent, over the Model Context Protocol (MCP), revision
2026-07-28.

It replaces the Debug Adapter Protocol (DAP) adapters, removed on
2026-09-30. Building them found the core interfaces a debugger needs, and
those stay ([In this repository](#in-this-repository)). MCP suits the job
better than DAP:

- DAP is shaped for a human in an editor: frames, scopes, variable
  references, a console that answers in prose;
- its pokes (`send`, `goto`, `cancel`) go through `evaluate` as text, and
  their results are English a program has to parse;
- a DAP session is one actor or one workbench, with no discovery;
- DAP's clients are editors. MCP's clients are agents (Claude, Copilot,
  Cursor, ChatGPT, Goose), plus generic tools such as MCP Inspector.

## How this proposal is split

- **[Part 1, the mapping](#part-1-the-mapping)** says what MCP means for
  tinyactors: addresses, resources, tools, replies, streams and traces. It
  is written in terms of core, its rings and MCP, and names no host's
  language, API or library. It is the same in every host.
- **[Part 2, the library and its bindings](#part-2-the-library-and-its-bindings)**
  says where the mapping is implemented (once, in `tinyactors-mcp`) and
  what is left for a host: feeding the library ring records and handing
  its MCP library the tools and resources.
- **[In this repository](#in-this-repository)** lists what the change adds
  and keeps here.

A host may add to the mapping: tools of its own, fields under `host` in a
resource, options when the tools are set up. It must not change what the
mapping defines. The shared transcripts ([Conformance](#conformance))
check it.

# Part 1: the mapping

## Addresses

A system is the top level. Actor names are unique within a system, not
across systems, and an application may run several (granary runs two).
Every URI names its system.

| URI | Names |
|---|---|
| `tinyactors://{system}` | a system |
| `tinyactors://{system}/actors/{family}/{name}` | a named actor, resident or not |
| `tinyactors://{system}/sessions/{session id}` | any session, anonymous and invoked ones included; `{session id}` is its text, `<incarnation>.<n>.<family>` (core ADR 0088) |
| `tinyactors://{system}/definitions/{family}/{revision}` | one definition revision |
| `ui://tinyactors/chart` | the MCP App |

- **`{system}` is a name the binding gives each system it serves**, unique
  within one server. Core knows a system only by its incarnation, which
  changes when the host says so. The name is what an agent keeps.
- **Charts are unaffected.** Inside a chart an actor is
  `#_actor_<family>/<name>` or `#_scxml_<session id>`. Converting between
  the two forms uses core's target grammar (core ADR 0044), in both
  directions.

## Discovery

- `server/discover` declares `resources` (with subscriptions and list
  changes) and `tools`, and the extensions `io.modelcontextprotocol/ui`
  and `io.modelcontextprotocol/tasks` where the server supports them.
- `resources/list` lists the systems and their definition revisions,
  paged. It does not list actors:
  - residency changes all the time, and each change would be a
    list-changed notification;
  - core knows only resident sessions. Unloaded ones are in the host's
    index ([unloading](ring-proto/reference/unloading.md#no-directory-vtable-not-resident-means-ask)),
    which the mapping cannot walk.
- `resources/templates/list` lists the actor, session and definition
  templates above.
- The live population comes from the `list_actors` tool, paged over core's
  walk of resident actors (`ta_system_visit_actors`, core ADR 0062).

## Observing and intervening

A `ta_system` has one owner at a time, on one thread
([ring.md](ring-proto/reference/ring.md#one-thread)). Normally the owner is
the host's pump, which posts to h2c, calls `ta_run` and drains c2h and
tel. The mapping works within that rule in two ways.

**Observing runs between `ta_run` calls.** A read-only tool or resource
read is a native call that the binding makes on the system's thread after
`ta_run` returns and before it is called again. Nothing executes while it
reads, and it changes nothing, so the pump goes on at once. It sees every
slot where it stands, including a macrostep paused at a budget or parked
for its turn (core ADR 0083). It never admits an input, runs a step or
loads an actor.

**Intervening attaches a debugger.** `goto` and `abandon` change an
actor's execution, which the ring protocol allows only while a debugger
owns the whole system ([debugging](ring-proto/reference/debugging.md)):

1. the binding suspends its pump after `ta_run` returns;
2. ownership passes to the library, which uses core's native debugging
   primitives: inspect, reach a microstep boundary, abandon, transition;
3. the library drains c2h and tel by the normal rules while it drives
   core, so effects and macrostep ends reach the host as usual;
4. the library returns ownership, and the pump resumes from where it
   stood. Nothing is rolled back.

**Every actor in the system stops while it is attached.** Pending inputs
stay pending, timers do not fire, and replies from the outside wait at the
host under its bounded admission policy. Other systems keep running. So an
intervention is short by design:

- **by default, an intervening tool attaches for its own call only:** it
  attaches, acts and detaches before it answers. An agent cannot leave a
  system frozen by forgetting a step.
- **`attach` holds the system across calls,** for a recovery that must see
  a configuration that does not change while the operator checks something
  outside (the recovery session in
  [debugging](ring-proto/reference/debugging.md#a-recovery-session)). It
  answers a hold handle (MCP 2026-07-28's stateful tools) with a lease,
  `holdMs`, capped by the binding. Tools given the handle act under it;
  `detach`, the lease running out or the client going away returns the
  system to the pump.

Tool annotations say which kind a tool is. A client can show an
intervening tool's cost before it runs: it stops the system, not one
actor.

## Resources

`resources/read` answers JSON (`application/json`), except where noted.
Every field comes from core. A host may add fields under a `host` key, and
nothing else. A read is an observation ([above](#observing-and-intervening)).

| Resource | Contents | Core source | `ttlMs`, `cacheScope` |
|---|---|---|---|
| system | name, incarnation, logical time, parameters, families with resident counts, ring and pool counters from the control block, definition links | core ADRs 0088, 0092; the control block ([ring.md](ring-proto/reference/ring.md)); definition inspection | short, private |
| actor or session | address, session ID, residency (`resident`, `finished`, or `not_resident`), whether it is recorded, active configuration, data, internal queue, pending delayed sends by send ID, invocations (links to the children), the position of a macrostep in progress, definition link | `ta_actor_inspect` (core ADRs 0083, 0048) | `0`, private |
| definition revision | two contents: the spec as JSON, and the SCXML the codec encodes (`application/scxml+xml`), with source spans | core ADRs 0042, 0085 | long, private |

- **Data is the data model's JSON encoding**, the same form values cross
  between data models in. A value with no JSON form is shown as a
  placeholder naming its kind, never dropped silently.
- **`not_resident` covers unloaded and unknown alike.** Only the host's
  index can tell them apart. A binding that keeps one may say which under
  `host`.
- **Reading never loads an actor.** Loading is the `load` tool.
- **An unknown system or definition**, or a session ID core does not know,
  answers `-32602`, which is what 2026-07-28 uses for a resource that is
  not found.

## Tools

Every tool takes `system`. `actor` is `{ family, name }` or a session ID.
Each result has `structuredContent` that matches the tool's `outputSchema`,
a short text summary, and a `resource_link` to what it touched. A failure
is a tool result with `isError: true` and a result name in
`structuredContent` (`not_found`, `not_resident`, `refused`, `bounced`,
`busy`, ...), not a protocol error.

| Tool | Arguments | Does | Kind, annotations | Core |
|---|---|---|---|---|
| `list_systems` | | the systems, with resident counts | observe, readOnly | |
| `list_actors` | `system`, `family?`, `cursor?` | the resident actors, paged | observe, readOnly | `ta_system_visit_actors` |
| `inspect` | `system`, `actor`, `hold?` | the actor's resource, as a tool result: a model cannot always read resources itself | observe, readOnly | `ta_actor_inspect` |
| `describe` | `system`, `actor` or `definition` | the states, the events the current configuration accepts, the data model's name | observe, readOnly | definition inspection (core ADRs 0042, 0071) |
| `evaluate` | `system`, `actor`, `expression` | evaluates an expression in the actor's data model against a copy of its data; what it writes is discarded | observe, readOnly | the data model |
| `send` | `system`, `actor`, `event`, `data?`, `waitMs?`, `timeoutMs?` | posts an event with an origin, and returns its disposition and the replies ([below](#replies)) | input, openWorld | [`h2c.event`](../../tinyactors-core/src/generated/MESSAGES.md#h2cevent) |
| `withdraw` | `system`, `call` | takes back a `send` whose event has not been taken yet | input, idempotent | [`h2c.withdraw`](../../tinyactors-core/src/generated/MESSAGES.md#h2cwithdraw) |
| `load` | `system`, `actor` | brings a named actor in without sending it anything | input, idempotent | the host's loader, [`h2c.load`](../../tinyactors-core/src/generated/MESSAGES.md#h2cload) |
| `destroy` | `system`, `actor` | ends the actor | input, destructive | [`h2c.destroy`](../../tinyactors-core/src/generated/MESSAGES.md#h2cdestroy) |
| `attach` | `system`, `holdMs?` | suspends the pump and holds the system; answers a hold handle | intervene, destructive | debugger attachment |
| `detach` | `hold` | returns the system to the pump | intervene, idempotent | |
| `goto` | `system`, `actor`, `states`, `from?`, `event?`, `hold?` | an operator transition through the normal engine | intervene, destructive | core ADR 0082 |
| `abandon` | `system`, `actor`, `hold?` | abandons the actor's macrostep in progress at its next microstep boundary | intervene, destructive | core ADR 0082 |

- **Input tools post h2c records and wait for their c2h answers**, like any
  host input. They go through the host's bounded submission table: when it
  or h2c is full, the tool answers `busy` at once. A tool never makes the
  host's drain wait for h2c ([the host loop](ring-proto/ARCHITECTURE.md#the-host-loop)).
- **`goto` needs a free slot for its actor.** If the actor holds a slot, or
  every slot is busy, the tool answers `busy` and names the macrostep in
  the way. The agent abandons it first, or waits for it to end. The tool
  never abandons anything it was not asked to.
- **Recorded sessions keep their journal.** A `send` to a recorded session
  appears as a [`c2h.input`](../../tinyactors-core/src/generated/MESSAGES.md#c2hinput)
  record, which the application journals like any other. An operator
  transition is not an input a replay can repeat: the binding records it
  in its audit trail, and the application should checkpoint the stable
  result ([`h2c.checkpoint`](../../tinyactors-core/src/generated/MESSAGES.md#h2ccheckpoint)),
  so that later replay starts from it
  ([recording and replay](ring-proto/reference/recording-and-replay.md)).
- **No tool edits data.** The debugging design has no data writes, effect
  substitution or rewind. A recovery changes data through a transition's
  handlers (`goto` with an `event`) or by sending the actor an event it
  handles. `evaluate` never writes, so an agent can try an expression
  without changing anything.
- **Nothing steps an actor or sets a breakpoint.** What an actor did is in
  its trace, and `inspect` shows where it stands.
- **The tool list is static.** `toolsListChanged` is never sent. A host may
  add tools of its own, and may prefix these names when it is set up beside
  an application's tools; transcripts play without a prefix.

## Replies

A chart replies to whoever sent its event, with `_event.origin` and
`_event.origintype` (SCXML C.1). Origins are text that core keeps without
interpreting; the I/O processor that receives a reply decides whether its
target is still live ([S12](ring-proto/scenarios/S12-http-request.md),
[S13](ring-proto/scenarios/S13-webhook-callback.md)). An agent's `send`
uses this like any other sender.

- **Each served system declares the MCP I/O processor** with
  [`h2c.type_declare`](../../tinyactors-core/src/generated/MESSAGES.md#h2ctype_declare)
  (registry `io`, type `mcp`). Core keeps only the name. The binding
  renders `_ioprocessors.mcp.location` for a session from its ID and
  address, as for every processor.
- **Each `send` call mints a reply URI**, `mcp:call/<id>`, where `<id>` is
  minted by the library (MCP request IDs are the client's and can repeat
  across clients). The event goes out as `h2c.event` with `origin` set to
  that URI and `origin_type` set to `mcp`.
- **The library collects replies.** Every
  [`c2h.send`](../../tinyactors-core/src/generated/MESSAGES.md#c2hsend) of type
  `mcp` reaches it through the binding. One whose target is an open call
  is added to that call's replies. Any other is dropped: an asynchronous
  reply with nobody to go to, which the chart's timeout covers.

**The chart does not know it is talking to an agent.** It answers the way
it answers any sender:

```xml
<transition event="quote.request">
  <send event="quote" typeexpr="_event.origintype" targetexpr="_event.origin"/>
</transition>
```

The same transition answers an HTTP request, a WebSocket connection or an
agent. A chart that never replies is fine too: the agent gets the
disposition of its event, and reads the actor with `inspect`.

**How long a call stays open.** 2026-07-28 has no connection: the only way
to the client is the response to a request in progress. So `send` keeps
its call open and answers once:

1. **the event's terminal record arrives:**
   - [`c2h.input_completed`](../../tinyactors-core/src/generated/MESSAGES.md#c2hinput_completed),
     with its status (`completed`, `runaway`, ...) and whether the event
     was `ignored`;
   - [`c2h.input_refused`](../../tinyactors-core/src/generated/MESSAGES.md#c2hinput_refused)
     or [`c2h.bounced`](../../tinyactors-core/src/generated/MESSAGES.md#c2hbounced),
     which end the call with `isError` and the reason;
2. **then `waitMs` passes** (default 0). Completion settles the event's own
   session, but a reply can come later through mail to other sessions.
   `waitMs` keeps collecting for that long;
3. **or `timeoutMs` passes first,** and the call answers with what it has.

The result has a text block per reply for the model, and
`structuredContent` with the disposition and each reply's event name,
data and sender. What counts as "the" reply is the application's
protocol. The library returns every reply and does not guess.

- **A reply after the call ended** is dropped, as above. Nothing waits in
  an inbox.
- **A wait longer than a request should be** is a task, where the client
  supports the Tasks extension: `send` answers a task handle, and
  `tasks/get` answers the result once the call ends.

## Streams

`subscriptions/listen` delivers:

| Filter | Notification | When |
|---|---|---|
| `resourceSubscriptions: [actor URI]` | `notifications/resources/updated` | after each macrostep of the actor ([`c2h.macrostep_ended`](../../tinyactors-core/src/generated/MESSAGES.md#c2hmacrostep_ended)), coalesced; on `c2h.loaded`, `c2h.unloaded`, `c2h.finished` and `c2h.destroyed` |
| `resourceSubscriptions: [system URI]` | `notifications/resources/updated` | a definition registered or retired ([`c2h.definition_result`](../../tinyactors-core/src/generated/MESSAGES.md#c2hdefinition_result)); resident counts changed (`c2h.spawned`, `c2h.destroyed`, `c2h.loaded`, `c2h.unloaded`), rate-limited |
| `resourcesListChanged` | `notifications/resources/list_changed` | a system or a definition revision came or went |

Notifications come from c2h records, which are never lost, so a
subscription never misses a change. A notification names the URI, and the
client reads the resource again: it is a live view, not a stream of
records.

The trace records themselves are on `tel` ([telemetry](ring-proto/reference/telemetry.md)).
The binding drains `tel` once and hands each record to `tinyactors-otlp`
for export. The mapping does not stream them: MCP's Logging is deprecated
in 2026-07-28 in favour of OpenTelemetry, and the agent's trace is
available through the application's exporter ([Traces](#traces)).

## Traces

A `tools/call` that carries `traceparent` and `tracestate` in `_meta`
(2026-07-28 documents the keys) delivers its event under that trace parent
(core ADR 0073). The macrostep it causes, and everything that macrostep
causes, join the agent's trace. The result names the trace ID in
`structuredContent`. The binding parses `traceparent` with
`ta_traceparent_parse` and fills the trace fields of the `send`'s
[`h2c.event`](../../tinyactors-core/src/generated/MESSAGES.md#h2cevent)
(`trace_id_high`, `trace_id_low`, `parent_span_id`, `trace_flags`), so
the `send` joins the caller's trace
([telemetry](ring-proto/reference/telemetry.md#trace-context-across-the-rings)).

## The MCP App (later)

`inspect` and `describe` declare `_meta.ui.resourceUri:
"ui://tinyactors/chart"`. The page draws the chart from the definition's
spec, marks the active configuration, and lists the pending delayed sends.
It uses nothing but the tools: its controls call `send` and `goto`, and it
calls `inspect` again to refresh. So it is one page, served unchanged by
every host. A control that intervenes says that it stops the system.

It is a later step: a developer's first client is usually a coding agent
in a terminal, which does not render Apps. The clients that do are Claude
(web and desktop), VS Code Copilot, Cursor, ChatGPT, Goose, Postman and
MCPJam. The tools and resources are complete without it.

## Access

The tools give full access to everything they serve. Authorization,
auditing and limits belong to the application's MCP server, or to an MCP
proxy in front of it, not to the mapping. Three things deserve a gate:

- an intervening tool stops every actor in the system while it runs, and
  `attach` stops them until its lease ends;
- every destructive tool acts at once;
- `evaluate` cannot change an actor, but a data model with a
  general-purpose language runs the agent's code in the host's process.

Each host's documentation says so where it shows how to serve the tools.

## Protocol revisions

The mapping targets MCP 2026-07-28, the current revision. MCP will have new
revisions over the life of the project, so the mapping keeps what depends
on one revision in one place:

- **Stable across revisions:** what the tools and resources are, their
  arguments and results, how a send's replies are collected, and which
  ring records make an update. This is the mapping.
- **Specific to 2026-07-28:** `subscriptions/listen` (it replaced
  `resources/subscribe`), `resultType`, `ttlMs` and `cacheScope`, `-32602`
  for a resource not found, the `_meta` keys for trace context, stateful
  tools and handles (`attach`), and the Tasks and Apps extensions.
- **Not the mapping's concern:** transport, JSON-RPC framing, version
  negotiation (`server/discover`, the protocol version in `_meta`) and
  capability exchange. An MCP library does these, in every language.

A new MCP revision changes the second list and the transcripts that cover
it, and names the revision it targets at the top of this section.

## Conformance

Shared transcripts define the mapping, and every host plays them in its
test task:

- **A transcript** is a chart, the MCP requests a client sends, and the
  results and notifications it must get, with matchers (`$capture`, `$var`,
  `$prefix`, `$some`, ...).
- **Charts are SCXML documents,** which every host imports, not a host's
  own authoring form.
- **What differs is keyed by data model, not by host:** `$expr` has a form
  per data model (`jq`, `ecmascript`, `null`, ...), so a new host with a
  data model already covered plays the transcripts unchanged.
- **Setup is a host's own,** and ends in a system named `transcripts` that
  holds the transcript's actors.

The library implements the mapping once, so the transcripts mostly test
`tinyactors-mcp`, through each host's binding of it. They also catch a
binding that drops or renames what the library offers, or feeds it records
out of order.

# Part 2: the library and its bindings

## The mapping lives in `tinyactors-mcp`

A host's language already has an MCP library (an official SDK or another)
that does the transport, the JSON-RPC framing, version negotiation and
tool registration. A host should not implement the mapping as well. It
hands its MCP library what one shared implementation offers.

That implementation is `tinyactors-mcp`, a small C library beside
`tinyactors-otlp`. Like it, it is free of any language's API and works
from ring records, so it serves every binding. It never drives a system on
its own: the binding owns the pump and calls into the library at defined
points.

| The mapping needs | It comes from |
|---|---|
| whether an event was taken, and how it ended | `c2h.input_completed`, `c2h.input_refused`, `c2h.bounced`, correlated by the h2c seq the library posted |
| the replies to a call | `c2h.send` records of type `mcp` |
| updates for subscriptions | `c2h.macrostep_ended`, `c2h.loaded`, `c2h.unloaded`, `c2h.finished`, `c2h.spawned`, `c2h.destroyed`, `c2h.definition_result` |
| delivering an event, a withdrawal, a destroy | the binding's h2c writer |
| loading an actor no mail asked for | the binding's loader, which posts `h2c.load` |
| resident actors, configuration, data, definitions | core's read-only inspection, between `ta_run` calls |
| inspecting, abandoning and transitions under ownership | core's native debugging primitives, while attached |

### What the library offers a host

In C, with JSON text in and out, so any language can pass it straight to
its MCP library:

- **the catalogue:** every tool (name, description, input and output
  schemas, annotations) and every resource template, as JSON;
- **`call`:** a tool name, its arguments and the request's `_meta` in; a
  result out. A `send` answers later, when its terminal record and replies
  are in;
- **`read` and `list`:** a resource by URI, and the resource list by
  cursor;
- **`listen`:** the URIs a client subscribed to in; a callback per update,
  with the URI to put in `notifications/resources/updated`;
- **`add_system` and `remove_system`:** the systems it serves, under the
  names the application gives them.

### What the library asks of a host

- **`feed`:** the binding hands it every c2h record it drains, after its
  own handlers, and before releasing the record's ring space. The library
  reads what it needs and returns at once. It never holds the record.
- **`post`:** a function that writes one h2c record, or answers that h2c
  or the submission table is full. The library answers `busy` then; it
  never waits.
- **`observe`:** a way to run a read-only call on the system's thread
  between `ta_run` calls. The binding runs it at its next yield.
- **`attach` and `detach`:** suspend the pump and pass ownership, and give
  it back.
- **`load`:** ask the binding's loader to bring an address in, as it does
  for `c2h.load_needed`.
- **`evaluate`:** for data models whose values live in the host's language
  (napi's JavaScript values), evaluation against a copy of an actor's
  data. The library does it for data models in C (jq, null).

### What a host still writes

- a few dozen lines that register the catalogue with its MCP library and
  forward calls, reads and subscriptions to the library;
- the hooks above, which are short because the pump already has each of
  them;
- its documentation of how an application serves the tools.

## Database hosts

The [SQLite](sqlite-host.md) and [PostgreSQL](postgres-host.md) sketches
drive core through the same rings. Their tables hold only what the host
has not yet posted to h2c, and session images there bound memory; they are
not persistence ([migration](ring-proto/reference/migration.md#open-questions)).
A database host binds `tinyactors-mcp` as any other host does. Its MCP
server runs beside the database: a `send` posts its event in one
transaction, and answers once the event's terminal record has been read
and the transaction has committed.

## napi

napi adds the library's tools to the application's own MCP server rather
than running a second one:

```js
const { McpServer } = require('@modelcontextprotocol/sdk/server/mcp.js');
const { registerMCPTools } = require('@tinyactors/node/mcp');

const server = new McpServer({ name: 'granary', version: '1.0.0' });
registerMCPTools(server, { systems: { orders, billing } });
server.registerTool('restock', /* the application's own tool */);
// Mount it on the application's HTTP server as the SDK shows.
```

- **The application chooses the transport.** A running application serves
  Streamable HTTP on a route of its own. stdio suits a process the client
  starts, which a long-running application is not.
- **Options:** `systems` (names to systems); `prefix` for the tool names
  (default none), for an application whose own tools would clash; and
  `maxHoldMs`, the cap on an `attach` lease.
- **The pump feeds the library.** `registerMCPTools` declares the `mcp`
  processor on each system, as `registerIO` does, and adds the library to
  the pump's c2h handlers.
- **The SDK is an optional peer dependency** of `@tinyactors/node`, loaded
  only by `@tinyactors/node/mcp`.
- **`evaluate`** on napi's JavaScript data model evaluates against a
  structured clone of the actor's data.
- **A reply in napi's builder** is the SCXML transition of
  [Replies](#replies): `t.send('quote', (send) => send.via(({ event }) =>
  event.originType).to(({ event }) => event.origin))`.

## The test host

The test host is the verification host. It drives core as a ring driver
(`hosted.post`, `hosted.turn`, `hosted.batches`). It links
`tinyactors-mcp` into its C host and binds it through its host protocol,
like any other C API, with the Ruby driver feeding it c2h records. So the
transcripts test the library, not a second implementation in Ruby.

`bin/mcp` serves a workbench directory's system, named after the
directory, over stdio. A developer starts it from an MCP client's
configuration. The workbench's charts reload when their files change.

# In this repository

**Adds:** the `tinyactors-mcp` package, with its own mise tasks, tests and
changelog; the shared transcripts as `tinyactors-test/mcp/`; napi's
`@tinyactors/node/mcp` entry point; the test host's `bin/mcp`.

**Core gains nothing for MCP** beyond the ring protocol: the MCP processor
is a declared type, replies are `c2h.send` records, and intervention uses
the debugging primitives.

**Stays**, because it is core API that any host uses:

| Used by | Core feature |
|---|---|
| `inspect` | the position of a macrostep in progress in `ta_actor_inspection` (core ADR 0083) |
| `goto`, `abandon` | operator transitions and abandoning a macrostep (core ADR 0082) |
| definition resources | source spans on decode and encode (core ADR 0085) |
| `abandon` | a resume that stops before a microstep's first unit (core ADR 0086), to reach a microstep boundary |

**Gone** (removed on 2026-09-30):

- napi: `createDebugSession`, `lib/debug.js`, `lib/debug/`, `DAPReader`,
  `encodeDAPMessage`, the `dap-server` task, `test/debug.js`,
  `test/dap-transcripts.js`, and their declarations in `index.d.ts`; the
  DAP permissions (`allow`) and audit hook went with them, and napi ADR
  0026's status says so;
- the test host: `ruby/dap/`, `ruby/dap.rb`, `bin/dap`,
  `ruby/dap_selftest.rb`, `ruby/dap_client.rb`, `ruby/dap_transcripts.rb`
  and `dap/`;
- `tinyactors-vscode`, whose purpose was testing the DAP adapters;
- `docs/debug-adapter.md`, and the AGENTS.md section on the VS Code
  extension. `docs/proposals/debug-adapter.md` stays as history.

# Open questions

1. **Following a send's causes.** `waitMs` is a fixed wait. Core could
   instead report when everything an input caused, through mail to other
   sessions, has settled. That is a new record or a field on
   `c2h.input_completed`, which the database hosts would use too.
2. **The URL of the MCP processor.** SCXML 6.2.5 names a processor by the
   URI of its description. The ring protocol's type registry holds names,
   and only core's own `scxml` has URI aliases. Whether a declared type may
   carry aliases decides whether `mcp` can also be selected by its URL.
3. **Cancelling a delayed send and replacing a definition.** Withdrawing
   one pending delayed send (core ADR 0084) and hot-replacing an actor's
   revision (core ADR 0089) have no ring record or debugging primitive.
   The mapping leaves them out until one exists.
4. **Operator transitions on recorded sessions.** How a replay represents
   a forced transition is open in the ring protocol. Until it is settled,
   a checkpoint after the transition is the only record replay can use.
5. **JSON in C.** The library speaks JSON text. It needs a JSON reader and
   writer; the test host uses jq's, napi none yet.
6. **The TypeScript SDK and 2026-07-28.** Whether the official SDK serves
   the stateless revision yet, and what napi does while it serves only
   2025-11-25.

## Sources

- MCP 2026-07-28: [key changes](https://modelcontextprotocol.io/specification/2026-07-28/changelog),
  [subscriptions](https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/subscriptions),
  [tools](https://modelcontextprotocol.io/specification/2026-07-28/server/tools) (stateful tools and handles),
  [resources](https://modelcontextprotocol.io/specification/2026-07-28/server/resources),
  [caching](https://modelcontextprotocol.io/specification/2026-07-28/server/utilities/caching),
  [multi round-trip requests](https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/mrtr).
- Extensions: [MCP Apps](https://modelcontextprotocol.io/extensions/apps/overview),
  [Tasks](https://modelcontextprotocol.io/extensions/tasks/overview),
  [client support](https://modelcontextprotocol.io/extensions/client-matrix).
- The ring protocol: [architecture](ring-proto/ARCHITECTURE.md),
  [debugging](ring-proto/reference/debugging.md),
  [telemetry](ring-proto/reference/telemetry.md),
  [the messages](../../tinyactors-core/src/generated/MESSAGES.md),
  [S12](ring-proto/scenarios/S12-http-request.md) and
  [S13](ring-proto/scenarios/S13-webhook-callback.md) for origins.
- SCXML 1.0 (`tinyactors-core/docs/internal/scxml.md`): 6.2.4 (the target
  of a send), 6.2.5 (I/O processor types), C.1 (the SCXML Event I/O
  Processor and its `#_` targets), 5.10 (names beginning with `_` are
  reserved).
- What it replaces: [the debug adapter proposal](debug-adapter.md), napi
  ADR 0026; the status of every DAP message was `docs/debug-adapter.md`,
  in the history before its removal.
