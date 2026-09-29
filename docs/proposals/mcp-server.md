# An MCP server: connect an agent to a system

**Status:** Proposed (2026-09-29)
**Touches:** every host (one mapping of MCP onto core, shared transcripts); tinyactors-napi (MCP tools for the application's MCP server; DAP removed); tinyactors-test (an MCP server for the workbench; DAP removed); tinyactors-vscode (removed); root docs and AGENTS.md
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

Connect an agent to a running system, and it can do things there: list the
actors, read their configuration and data, send them events and read their
replies, load them, move a wedged one out of its state. It is a debugger
for an agent, over the Model Context Protocol (MCP), revision 2026-07-28.

This replaces the Debug Adapter Protocol (DAP) adapters. Building them
found the core interfaces a debugger needs, and those stay (see
[In this repository](#in-this-repository)). DAP itself is less useful than
MCP:

- it is shaped for a human in an editor: frames, scopes, variable
  references, a console that answers in prose;
- its pokes (`send`, `goto`, `cancel`) go through `evaluate` as text, and
  their results are English a program has to parse;
- a session is one actor or one workbench, and there is no discovery;
- its clients are editors. MCP's clients are agents (Claude, Copilot,
  Cursor, ChatGPT, Goose), plus generic tools such as MCP Inspector.

## How this proposal is split

- **[Part 1, the mapping](#part-1-the-mapping)** says what MCP means for
  tinyactors: the addresses, resources, tools, replies, streams and traces.
  It is the same in every host: napi, the test host, and hosts to come,
  such as a Python binding or the database hosts
  ([SQLite](sqlite-host.md), [PostgreSQL](postgres-host.md)). It is written
  in terms of core (its ADRs and C API) and MCP, and names no host's
  language, API or library.
- **[Part 2, host bindings](#part-2-host-bindings)** says what a host must
  provide to implement the mapping, and how napi and the test host provide
  it. Everything specific to Node, JavaScript or Ruby is there.
- **[In this repository](#in-this-repository)** lists what the change
  removes and keeps here.

A host may add to the mapping (tools of its own, fields under `host` in a
resource, options when the tools are set up). It must not change what the
mapping defines. The shared transcripts
([Conformance](#conformance)) check it.

# Part 1: the mapping

## Addresses

A system is the top level: actor names are unique within a system, not
across systems, and an application may run several (granary runs two).
Every URI names its system.

| URI | Names |
|---|---|
| `tinyactors://{system}` | a system |
| `tinyactors://{system}/actors/{family}/{name}` | a named actor, resident or not |
| `tinyactors://{system}/sessions/{session id}` | any session, anonymous and invoked ones included; `{session id}` is its text, `<incarnation>.<n>.<family>` (core ADR 0088) |
| `tinyactors://{system}/definitions/{family}/{revision}` | one definition revision |
| `ui://tinyactors/chart` | the MCP App |

- **`{system}` is a name the host binding gives each system it serves**,
  unique within one server. Core knows a system only by its incarnation,
  which changes when the host says so; the name is what an agent keeps.
- **Charts are unchanged.** Inside a chart an actor is still
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
  - an unloaded named actor exists, but the directory that knows it
    belongs to the host (core ADR 0037) and cannot be walked.
- `resources/templates/list` lists the actor, session and definition
  templates above.
- The live population comes from the `list_actors` tool, paged over core's
  walk of resident actors (`ta_system_visit_actors`, core ADR 0062).

## Resources

`resources/read` answers JSON (`application/json`), except where noted.
Every field comes from core. A host may add fields under a `host` key, and
nothing else.

| Resource | Contents | Core source | `ttlMs`, `cacheScope` |
|---|---|---|---|
| system | name, incarnation, logical time, parameters, families with resident counts, metrics counters, definition links | core ADRs 0088, 0092, 0072; definition inspection | short, private |
| actor or session | address, session ID, residency (`resident`, `unloaded`, `finished`, `unknown`), active configuration, data, internal queue, pending delayed sends by send ID, invocations (links to the children), paused position, definition link | `ta_actor_inspect` (core ADRs 0083, 0048), `ta_actor_find` and the directory (core ADR 0037) | `0`, private |
| definition revision | two contents: the spec as JSON, and the SCXML the codec encodes (`application/scxml+xml`), with source spans | core ADRs 0042, 0085 | long, private |

- **Data is the data model's JSON encoding**, the same form values cross
  between data models in. A value with no JSON form is shown as a
  placeholder naming its kind, never dropped silently.
- **Reading never loads an actor.** An unloaded actor answers `residency:
  "unloaded"`. Loading is the `load` tool.
- **An unknown system, actor or definition** answers `-32602`, which is
  what 2026-07-28 uses for a resource that is not found.

## Tools

Every tool takes `system`. `actor` is `{ family, name }` or a session ID.
Each result has `structuredContent` that matches the tool's `outputSchema`,
a short text summary, and a `resource_link` to what it touched. A failure
is a tool result with `isError: true` and core's result name in
`structuredContent` (`not_found`, `unloaded`, `busy`, `unsupported`, ...),
not a protocol error.

| Tool | Arguments | Does | Annotations | Core |
|---|---|---|---|---|
| `list_systems` | | the systems, with resident counts | readOnly | |
| `list_actors` | `system`, `family?`, `cursor?` | the resident actors, paged | readOnly | `ta_system_visit_actors` |
| `inspect` | `system`, `actor` | the actor's resource, as a tool result: a model cannot always read resources itself | readOnly | `ta_actor_inspect` |
| `describe` | `system`, `actor` or `definition` | the states, the events the current configuration accepts, the data model's name | readOnly | definition inspection (core ADRs 0042, 0071) |
| `evaluate` | `system`, `actor`, `expression` | evaluates an expression in the actor's data model, against a copy of its data; what it writes is discarded | readOnly | the data model |
| `send` | `system`, `actor`, `event`, `data?`, `timeoutMs?` | delivers an event with an origin, and returns the replies ([below](#replies)) | openWorld | delivery through the external-event router |
| `load` | `system`, `actor` | brings a named actor in without sending it anything, through the same path mail for that address would take | idempotent | `ta_actor_load` (core ADRs 0051, 0089) |
| `goto` | `system`, `actor`, `states`, `from?` | an operator transition, queued behind the actor's mail | destructive | core ADR 0082 |
| `abandon` | `system`, `actor` | abandons a macrostep that never ends | destructive | core ADR 0082 |
| `cancel` | `system`, `actor`, `sendID` | withdraws one pending delayed send | destructive, idempotent | `ta_actor_withdraw_send` (core ADR 0084) |
| `set` | `system`, `actor`, `location`, `value` | writes a data-model location; refused for a logged actor | destructive | the data model; capture and reload where there is no direct write (core ADR 0038) |
| `replace` | `system`, `actor`, `revision` | hot-replaces the actor with a registered revision | destructive | prepare and publish (core ADR 0089) |
| `destroy` | `system`, `actor` | ends the actor | destructive | `ta_actor_destroy` |

- **Logged actors stay replayable.** Every poke of an actor whose family
  is logged (core ADR 0090) is an input the log records, or a replay no
  longer arrives where the actor was:
  - `send` is an `event` record, `goto` a `jump`, `abandon` an
    `abandoned`, `cancel` a `withdrawn`, `destroy` a `destroyed`;
  - a data write has no record, so `set` answers `unsupported` for a
    logged actor and names `send` and `goto` instead;
  - `evaluate` never writes, for any actor, so an agent can try an
    expression without changing anything.
- **Nothing stops or steps an actor.** There are no breakpoints: what an
  actor did is in its trace, and `inspect` shows where it stands.
- **The tool list is static.** `toolsListChanged` is never sent. A host
  may add tools of its own, and may prefix these names when it is set up
  beside an application's tools; transcripts play without a prefix.

## Replies

A chart replies to whoever sent its event, with `_event.origin` and
`_event.origintype` (SCXML C.1). Core makes every origin answerable
through its I/O processor registry and origin scopes (core ADR 0068). An
agent's `send` uses them like any other sender:

- **Each served system has an I/O processor of type `mcp`**, listed in
  `_ioprocessors`.
- **Each `send` call opens an origin scope** for that processor, with the
  URI `mcp:call/<id>`, where `<id>` is minted by the server (MCP request
  IDs are the client's and can repeat across clients). The event is
  delivered with that origin.
- **The processor collects** every event sent to the scope while the call
  is open, and the scope is closed when the call ends.

**The chart does not know it is talking to an agent.** It answers the way
it answers any sender:

```xml
<transition event="quote.request">
  <send event="quote" typeexpr="_event.origintype" targetexpr="_event.origin"/>
</transition>
```

The same transition answers an HTTP request, a WebSocket connection, or an
agent. A chart that never replies is fine too: the agent gets the result
of the delivery, and reads the actor with `inspect`.

- **The reply goes back in the tool result.** 2026-07-28 has no
  connection, and the only way to the client is the response to a request
  in progress. So `send` keeps its call open and returns what reached its
  origin: a text block per reply for the model, and `structuredContent`
  with each reply's event name, data and sender.
- **How long a call stays open:** until nothing the send caused is
  runnable or due at the current time, or until `timeoutMs`. What the send
  caused is followed through the records' causes (core ADR 0067), not
  through the whole system, which a live host never sees idle. What
  counts as "the" reply is the application's protocol; the server returns
  every reply and does not guess.
- **A reply after the call ended** has no one to go to. The scope is
  closed, the send fails, and the chart gets `error.communication`, as for
  any sender that went away (SCXML C.1). Nothing waits in an inbox.
- **A wait longer than a request should be** is a task, where the client
  supports the Tasks extension: `send` answers a task handle, and
  `tasks/get` answers the replies once the wait ends.

## Streams

`subscriptions/listen` delivers:

| Filter | Notification | When |
|---|---|---|
| `resourceSubscriptions: [actor URI]` | `notifications/resources/updated` | after each macrostep of the actor, coalesced; on load, unload, finish and destroy |
| `resourceSubscriptions: [system URI]` | `notifications/resources/updated` | a definition registered or retired (core ADR 0079); resident counts changed, rate-limited |
| `resourcesListChanged` | `notifications/resources/list_changed` | a system or a definition revision came or went |

The events come from core's records (core ADRs 0067, 0080). A notification
names the URI, and the client reads the resource again: it is a live view,
not a stream of records. The records themselves go to the host's trace
sink, and on from there as the host exports them. MCP's Logging is
deprecated in 2026-07-28 in favour of OpenTelemetry, and the mapping does
not use it.

## Traces

A `tools/call` that carries `traceparent` and `tracestate` in `_meta`
(2026-07-28 documents the keys) delivers its event, jump or other input
under that trace parent (core ADR 0073). The macrostep it causes, and
everything that macrostep causes, join the agent's trace. The result names
the trace ID in `structuredContent`.

## The MCP App (later)

`inspect` and `describe` declare `_meta.ui.resourceUri:
"ui://tinyactors/chart"`. The page draws the chart from the definition's
spec, marks the active configuration, and lists the pending delayed sends.
It uses nothing but the tools: its controls call `send`, `goto` and
`cancel`, and it calls `inspect` again to refresh. So it is one page,
served unchanged by every host.

It is a later step, not the first: a developer's first client is usually
a coding agent in a terminal, which does not render Apps. The clients that
do are Claude (web and desktop), VS Code Copilot, Cursor, ChatGPT, Goose,
Postman and MCPJam. The tools and resources are complete without it.

## Access

The tools give full access to everything they serve. Authorization,
auditing and limits belong to the application's MCP server, or to an MCP
proxy in front of it, not to the mapping. `evaluate` cannot change an
actor, but a data model with a general-purpose language runs the agent's
code in the host's process, and every destructive tool acts at once. Each
host's documentation says so where it shows how to serve the tools.

## Conformance

Shared transcripts define the mapping, and every host plays them in its
test task, as the DAP transcripts did for two hosts:

- **A transcript** is a chart, the MCP requests a client sends, and the
  results and notifications it must get, with the same matchers as before
  (`$capture`, `$var`, `$prefix`, `$some`, ...).
- **Charts are SCXML documents,** which every host imports, not a host's
  own authoring form.
- **What differs is keyed by data model, not by host:** `$expr` has a form
  per data model (`jq`, `ecmascript`, `null`, ...), so a new host with a
  data model already covered plays the transcripts unchanged.
- **Setup is a host's own** (below), and ends in a system named
  `transcripts` that holds the transcript's actors.

# Part 2: host bindings

## What a host provides

| Needs | Why | napi | test host |
|---|---|---|---|
| names for the systems it serves | addresses | `registerMCPTools(server, { systems })` | the workbench's directory name |
| an MCP server and transport | reach a client | the application's own server (official TypeScript SDK) | its own, over stdio |
| the `mcp` I/O processor and origin scopes | replies | `system.registerIO`, `system.openScope`, `origin` on `post` | to add: an adapter in `src/adapters.c` |
| following a send's causes | when `send` answers | to add: nothing tells yet when a send's work is done | the Scheduler runs to quiescence; causes decide once delayed sends are due later |
| evaluating against a copy of the data | `evaluate` never writes | to add: a structured clone of the data | to add: a copy of the JSON (jq); a fresh context with the data decoded into it (ECMAScript) |
| a load no mail asked for | `load` | to add: a `LoadReason` of its own (today `mail`, `timer`, `session`) | the workbench's load path |
| a write of a data location | `set` | its write path | capture and reload (core ADR 0038) |
| a trace sink and an exporter | streams, traces | `setTraceSink`, OTLP (napi ADR 0015) | the trace records it reads now |

A new host fills the same rows. Two sketches, to check that nothing above
assumes a JavaScript or Ruby process:

- **A Python binding** adds the tools to an application's server built on
  the official Python SDK, as napi does for Node, and registers the `mcp`
  processor through its own binding of core's I/O registry.
- **A database host** ([SQLite](sqlite-host.md),
  [PostgreSQL](postgres-host.md)) cannot hold an MCP connection inside the
  database. Its server runs beside the database and calls the host's SQL
  functions. A `send` is one transaction; its replies are the events the
  transaction sent to the call's origin, returned once it commits. System
  names are the database's (a schema, or a table of systems).

## napi: tools for the application's own MCP server

An application that exposes an MCP server has one already, for its own
tools, with its own transport and authentication. napi adds its tools to
that server rather than running a second one:

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
- **Options:**
  - `systems`: names to systems;
  - `prefix` for the tool names (default none), for an application whose
    own tools would clash;
  - `ioType` (default `mcp`), only for an application that already
    registered an `mcp` processor of its own. A chart that replies to its
    origin never names the type.
- **The SDK is an optional peer dependency** of `@tinyactors/node`, loaded
  only by `@tinyactors/node/mcp`.
- **A reply chart in napi's builder** is the SCXML above:
  `t.send('quote', (send) => send.via(({ event }) => event.originType).to(({ event }) => event.origin))`.

## The test host: the workbench's server

`bin/mcp` serves a workbench directory's system over stdio. A developer
starts it from an MCP client's configuration, the way `bin/dap` was
started from VS Code. The workbench's charts reload when their files
change, as they do now. The server speaks MCP on its own; the test host's
host protocol and its error codes (`-32001`, ...) stay behind it, apart
from MCP's reserved range (`-32020` to `-32099`).

# In this repository

**Stays**, because it is core API that any host uses:

| Was needed by | Core feature |
|---|---|
| C1 | the paused position in `ta_actor_inspection` (core ADR 0083) |
| C2 | operator transitions and abandoning a macrostep (core ADR 0082) |
| C3 | withdrawing one delayed send (core ADR 0084) |
| C5 | source spans on decode and encode (core ADR 0085) |
| C6 | a resume that stops before a microstep's first unit (core ADR 0086) |

**Goes**, in the change that lands the MCP servers:

- napi: `createDebugSession`, `lib/debug.js`, `lib/debug/`, `DAPReader`,
  `encodeDAPMessage`, the `dap-server` task, `test/debug.js`,
  `test/dap-transcripts.js`, and their declarations in `index.d.ts`;
  napi ADR 0026 is superseded, and the DAP permissions (`allow`) go with
  it;
- the test host: `ruby/dap/`, `ruby/dap.rb`, `bin/dap`,
  `ruby/dap_selftest.rb`, `ruby/dap_client.rb`, `ruby/dap_transcripts.rb`,
  and `dap/`; the MCP transcripts take its place as `tinyactors-test/mcp/`;
- whatever of the Scheduler only the debugger used: `hold`, `stop!`, the
  monitor;
- `tinyactors-vscode`, whose purpose was testing the DAP adapters;
- `docs/debug-adapter.md`, and the AGENTS.md section on the VS Code
  extension. `docs/proposals/debug-adapter.md` stays as history.

Whether core's stepping hooks (`pause_at_microstep_start`, action budgets)
become unused once the workbench no longer steps is checked in that
change. Scenarios exercise them, so they probably stay.

# Open questions

**For the mapping:**

1. **Following a send's causes.** Every host needs to know when nothing a
   send caused is runnable or due. The records carry causes, so a host can
   follow them itself, but a core call that answers it would give every
   host the same answer. Decide which.
2. **The `mcp` processor's type.** Whether it stays the short name `mcp`
   or gets a URI long form, as SCXML's own processor types have.

**For the hosts:**

3. **napi: the SDK's support for 2026-07-28.** `registerMCPTools` assumes the
   official TypeScript SDK serves the stateless protocol,
   `subscriptions/listen` and `server/discover`. Check before an ADR, and
   decide what to do while it serves only 2025-11-25.
4. **The rows marked "to add"**, in napi and the test host, in [What a host provides](#what-a-host-provides).

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
- SCXML 1.0 (`tinyactors-core/docs/internal/scxml.md`): 6.2.4 (the target
  of a send), C.1 (the SCXML Event I/O Processor and its `#_` targets),
  5.10 (names beginning with `_` are reserved).
- What it replaces: [the debug adapter proposal](debug-adapter.md),
  [the status of every DAP message](../debug-adapter.md), napi ADR 0026.
