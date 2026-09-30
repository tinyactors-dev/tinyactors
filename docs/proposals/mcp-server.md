# An MCP server: connect an agent to a system

**Status:** Proposed (2026-09-29)
**Touches:** tinyactors-core (every I/O processor registers a URL and a short name); the host kernel (the mapping, implemented once); tinyactors-napi and tinyactors-test (bind it). The DAP adapters and tinyactors-vscode were removed on 2026-09-30, ahead of it.
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

Connect an agent to a running system, and it can do things there: list the
actors, read their configuration and data, send them events and read their
replies, load them, move a wedged one out of its state. It is a debugger
for an agent, over the Model Context Protocol (MCP), revision 2026-07-28.

This replaces the Debug Adapter Protocol (DAP) adapters, which were
removed on 2026-09-30 ahead of it. Building them
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
- **[Part 2, host bindings](#part-2-host-bindings)** says where the mapping
  is implemented (once, in the [host kernel](host-kernel.md)), and what is
  left for a host: handing its MCP library the kernel's tools and
  resources. Everything specific to Node, JavaScript or Ruby is there.
- **[In this repository](#in-this-repository)** lists what the change
  adds, removes and keeps here.

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

- **Each served system has the MCP I/O processor**, registered in core's
  I/O processor registry (core ADR 0068) with a URL and a short name, as
  every processor is ([Changes in core](#in-this-repository)):
  - the URL is the permanent URL of this mapping's documentation, as
    SCXML 6.2.5 asks ("the URI of the description of the relevant Event
    I/O Processor"), fixed when the documentation has a home;
  - the short name is `mcp`, as `scxml` is the built-in processor's.

  Both select it in `<send type>`. `_ioprocessors` and
  `_event.origintype` carry the URL. A reply never needs the short name:
  it uses `_event.origintype`.
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

## Protocol revisions

The mapping targets MCP 2026-07-28, the current revision. MCP will have
new revisions over the life of the project, so the mapping keeps what
depends on one revision in one place:

- **Stable across revisions:** what the tools and resources are, their
  arguments and results, how a send's replies are collected, and which
  core records make an update. This is the mapping.
- **Specific to 2026-07-28:** `subscriptions/listen` (it replaced
  `resources/subscribe`), `resultType`, `ttlMs` and `cacheScope`,
  `-32602` for a resource not found, the `_meta` keys for trace context,
  and the Tasks and Apps extensions.
- **Not the mapping's concern:** transport, JSON-RPC framing, version
  negotiation (`server/discover`, the protocol version in `_meta`) and
  capability exchange. An MCP library does these, in every language.

A new MCP revision changes the second list and the transcripts that cover
it, and names the revision it targets at the top of this section.

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
- **Setup is a host's own,** and ends in a system named `transcripts`
  that holds the transcript's actors.

Since the kernel implements the mapping once, the transcripts mostly test
the kernel, through each host's binding of it. They also catch a binding
that drops or renames what the kernel offers.

# Part 2: host bindings

## The mapping lives in the host kernel

A host's language already has an MCP library (an official SDK or another)
that does the transport, the JSON-RPC framing, version negotiation and
tool registration. A host should not implement the mapping as well. It
should hand the library what one shared implementation offers.

That implementation is a module of the [host kernel](host-kernel.md), the
C library on top of core that the kernel proposal extracts from napi. It
fits there because the mapping needs what the kernel already holds:

| The mapping needs | The kernel has |
|---|---|
| following what a send caused, to know when to answer | the scheduler, its queues and turn state, and core's records |
| loading an actor no mail asked for | the directory and the loader hooks |
| delivering with an origin and a trace parent | the router and effects |
| the MCP I/O processor and its origin scopes | the I/O adapters (`adapters/io.c`) |
| resident actors, definitions, metrics | enumeration, and core's inspection |

### What the kernel's MCP module offers a host

In C, with JSON text in and out, so any language can pass it straight to
its MCP library:

- **the catalogue:** every tool (name, description, input and output
  schemas, annotations) and every resource template, as JSON;
- **`call`:** a tool name, its arguments and the request's `_meta` in; a
  result out. A `send` answers later, when its replies are in, through
  the same completion the host's pump already drives;
- **`read` and `list`:** a resource by URI, and the resource list by
  cursor;
- **`listen`:** the URIs a client subscribed to in; a callback per
  update, with the URI to put in `notifications/resources/updated`;
- **`add_system` and `remove_system`:** the systems the module serves,
  under the names the application gives them.

### What a host still writes

- a few dozen lines that register the catalogue with its MCP library and
  forward calls, reads and subscriptions to the module;
- the evaluation against a copy of an actor's data, for data models whose
  values live in the host's language (napi's JavaScript values); the
  kernel does it for data models in C (jq, null);
- its documentation of how an application serves the tools.

### Hosts without the kernel

The kernel holds actors in memory. The database hosts keep mailboxes,
the directory and timers in tables (the kernel proposal leaves them out),
so the MCP module depends on the kernel only through a small interface:

- the scheduling questions: is anything a send caused still runnable or
  due, and wake me when that changes;
- loading an actor by address.

A database host answers them from its tables, and uses the rest of the
module unchanged. Its MCP server runs beside the database and calls the
host's SQL functions; a `send` is one transaction, and its replies are
returned once it commits.

### When

The kernel proposal waits for a second language host before it
extracts anything. The MCP module need not wait: it is written in napi's
Node-free C (`tinyactors-napi/src/`, beside `scheduler.c` and
`router.c`), under the kernel's rules, and moves with them when the
kernel is extracted.

## napi

napi adds the module's tools to the application's own MCP server rather
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
- **Options:** `systems` (names to systems), and `prefix` for the tool
  names (default none), for an application whose own tools would clash.
- **The SDK is an optional peer dependency** of `@tinyactors/node`, loaded
  only by `@tinyactors/node/mcp`.
- **`evaluate`** on napi's JavaScript data model evaluates against a
  structured clone of the actor's data.
- **A reply in napi's builder** is the SCXML transition of
  [Replies](#replies): `t.send('quote', (send) => send.via(({ event }) =>
  event.originType).to(({ event }) => event.origin))`.

## The test host

The test host is the verification host: it links core and drives it from
Ruby, and its Scheduler is in Ruby, not in the kernel. It links the
kernel's MCP module into the C host and binds it through its host
protocol, like any other C API, with the Ruby Scheduler behind the
module's scheduling questions. So the transcripts test the module, not a
second implementation in Ruby.

`bin/mcp` serves a workbench directory's system, named after the
directory, over stdio. A developer starts it from an MCP client's
configuration. The workbench's charts reload when their files change, as
they do now.

# In this repository

**Changes in core:** core already keeps the I/O processor registry
(`ta_system_register_io`, core ADR 0068), but a registration names its
type with one atom, and the only short form is the built-in processor's
`scxml`, hard-coded in `system.c`. SCXML 6.2.5 asks for a URL per
processor and allows short forms as an authoring convenience, so every
registration takes both:

- `ta_io_registration_params` takes the type's URL and a short name,
  and `<send type>` (literal or `typeexpr`) selects a processor by either;
- the built-in processor becomes an ordinary entry at index 0, with its
  URL and the short name `scxml`, and the special case goes;
- `_ioprocessors` and `origintype` carry the URL; a data model may also
  offer the short name as a key, as ADR 0068 leaves to it;
- every caller moves in the same change: napi's `registerIO` takes a URL
  and a short name, and so do the test host's adapters.

This is a change to the registry, not to MCP: an application's own
processors (an `http`, a `ws`) get short names the same way. It needs a
core ADR of its own.

**Stays**, because it is core API that any host uses:

| Was needed by | Core feature |
|---|---|
| C1 | the paused position in `ta_actor_inspection` (core ADR 0083) |
| C2 | operator transitions and abandoning a macrostep (core ADR 0082) |
| C3 | withdrawing one delayed send (core ADR 0084) |
| C5 | source spans on decode and encode (core ADR 0085) |
| C6 | a resume that stops before a microstep's first unit (core ADR 0086) |

**Gone** (removed on 2026-09-30, ahead of the MCP servers):

- napi: `createDebugSession`, `lib/debug.js`, `lib/debug/`, `DAPReader`,
  `encodeDAPMessage`, the `dap-server` task, `test/debug.js`,
  `test/dap-transcripts.js`, and their declarations in `index.d.ts`;
  the DAP permissions (`allow`) and audit hook went with them, and napi
  ADR 0026's status says so;
- the test host: `ruby/dap/`, `ruby/dap.rb`, `bin/dap`,
  `ruby/dap_selftest.rb`, `ruby/dap_client.rb`, `ruby/dap_transcripts.rb`,
  and `dap/`; the MCP transcripts will take its place as
  `tinyactors-test/mcp/`;
- whatever of the Scheduler only the debugger used: `hold`, `stop!`, the
  monitor;
- `tinyactors-vscode`, whose purpose was testing the DAP adapters;
- `docs/debug-adapter.md`, and the AGENTS.md section on the VS Code
  extension. `docs/proposals/debug-adapter.md` stays as history.

Core's stepping hooks (`pause_at_microstep_start`, action budgets) stay:
the pausing and operator scenarios use them.

# Open questions

1. **The URL of the MCP processor.** The rule is settled (the permanent
   URL of this mapping's documentation, short name `mcp`); the URL waits
   for the documentation to have a home.
2. **Following a send's causes.** Whether the kernel follows the records'
   causes itself, or core gains a call that answers "is anything this
   input caused still runnable or due", which the database hosts would
   use too.
3. **JSON in C.** The module speaks JSON text. The kernel needs a JSON
   reader and writer; the test host uses jq's, napi none yet.
4. **The TypeScript SDK and 2026-07-28.** Whether the official SDK serves
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
- SCXML 1.0 (`tinyactors-core/docs/internal/scxml.md`): 6.2.4 (the target
  of a send), C.1 (the SCXML Event I/O Processor and its `#_` targets),
  5.10 (names beginning with `_` are reserved).
- What it replaces: [the debug adapter proposal](debug-adapter.md), napi
  ADR 0026; the status of every DAP message was `docs/debug-adapter.md`,
  in the history before its removal.
