# An MCP server: connect an agent to a system

**Status:** Proposed (2026-09-29)
**Touches:** tinyactors-napi (MCP tools for the application's MCP server; DAP removed), tinyactors-test (an MCP server for the workbench; DAP and its transcripts removed), tinyactors-vscode (removed), root docs and AGENTS.md
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

Connect an agent to a running system, and it can do things there: list the
actors, read their configuration and data, send them events and read their
replies, load them, move a wedged one out of its state. The server is a
debugger for an agent, over the Model Context Protocol (MCP), revision
2026-07-28.

This replaces the Debug Adapter Protocol (DAP) adapters. Building them
found the core interfaces a debugger needs, and those stay (below). DAP
itself is less useful than MCP:

- it is shaped for a human in an editor: frames, scopes, variable
  references, a console that answers in prose;
- its pokes (`send`, `goto`, `cancel`) go through `evaluate` as text, and
  their results are English a program has to parse;
- a session is one actor (napi) or one workbench (the test host), and there
  is no discovery;
- its clients are editors. MCP's clients are agents (Claude, Copilot,
  Cursor, ChatGPT, Goose), plus generic tools such as MCP Inspector.

## What stays, what goes

**Stays**, because it is core API that any front end uses:

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
  napi ADR 0026 is superseded;
- the test host: `ruby/dap/`, `ruby/dap.rb`, `bin/dap`,
  `ruby/dap_selftest.rb`, `ruby/dap_client.rb`, `ruby/dap_transcripts.rb`,
  and `dap/` (the shared transcripts and their charts);
- whatever of the Scheduler only the debugger used: `hold`, `stop!`, the
  monitor. MCP does not stop or step actors in either host;
- `tinyactors-vscode`, whose purpose was testing the DAP adapters;
- `docs/debug-adapter.md`, and the AGENTS.md section on the VS Code
  extension. `docs/proposals/debug-adapter.md` stays as history.

The DAP permissions (`allow`) go with it. The MCP server has no
authorization layer (see [Access](#access)).

## Addresses

A system is the top level: actor names are unique within a system, not
across systems, and an application may run several (granary runs two). Every
URI names its system.

| URI | Names |
|---|---|
| `tinyactors://{system}` | a system |
| `tinyactors://{system}/actors/{family}/{name}` | a named actor, resident or not |
| `tinyactors://{system}/sessions/{session id}` | any session, anonymous and invoked ones included; `{session id}` is `<incarnation>.<n>.<family>` (core ADR 0088) |
| `tinyactors://{system}/definitions/{family}/{revision}` | one definition revision |
| `ui://tinyactors/chart` | the MCP App (below) |

- **`{system}` is a host binding's name.** Core knows a system by its
  incarnation, which changes when the host says so. napi takes the names
  where it adds the tools: `registerMCPTools(server, { systems: { orders,
  billing } })` (see [Hosts](#hosts)).
  The test host names the workbench's system after its directory.
- **Charts are unchanged.** Inside a chart an actor is still
  `#_actor_<family>/<name>` or `#_scxml_<session id>`. The server converts
  between the two forms through core's target grammar (core ADR 0044), in
  one place.

## Discovery

- `server/discover` declares `resources` (with subscriptions and list
  changes), `tools`, and the extensions `io.modelcontextprotocol/ui` and
  `io.modelcontextprotocol/tasks`.
- `resources/list` lists the systems and their definition revisions,
  paged. It does not list actors:
  - residency changes all the time, and each change would be a
    list-changed notification;
  - an unloaded named actor exists, but the directory that knows it
    belongs to the host (core ADR 0037) and cannot be walked.
- `resources/templates/list` lists the actor, session and definition
  templates above.
- The live population comes from the `list_actors` tool, paged over
  `ta_system_visit_actors`.

## Resources

`resources/read` answers JSON (`application/json`), except where noted.

| Resource | Contents | `ttlMs`, `cacheScope` |
|---|---|---|
| system | name, incarnation, logical time, parameters (core ADR 0092), families with resident counts, metrics counters (core ADR 0072), recent dead letters, definition links | short, private |
| actor or session | address, session ID, residency (`resident`, `unloaded`, `finished`, `unknown`), configuration, data (the data model's JSON), mailbox depth, internal queue, pending delayed sends by send ID, invocations (links to the children), paused position (core ADR 0083), definition link | `0`, private |
| definition revision | two contents: the spec as JSON, and the SCXML the codec encodes (`application/scxml+xml`), with source spans (core ADR 0085) | long, private |

- **Reading never loads an actor.** An unloaded actor answers `residency:
  "unloaded"`. Loading is the `load` tool.
- **An unknown system, actor or definition** answers `-32602`, which is
  what 2026-07-28 uses for a resource that is not found.

## Tools

Every tool takes `system`. `actor` is `{ family, name }` or a session ID.
Each result has `structuredContent` that matches the tool's `outputSchema`,
a short text summary, and a `resource_link` to what it touched. A failure is
a tool result with `isError: true` and core's result in
`structuredContent` (`not_found`, `unloaded`, `busy`, `unsupported`, ...),
not a protocol error.

| Tool | Arguments | Does | Annotations | Core or napi |
|---|---|---|---|---|
| `list_systems` | | the systems, with resident counts | readOnly | server configuration |
| `list_actors` | `system`, `family?`, `cursor?` | the resident actors, paged | readOnly | `ta_system_visit_actors`, `system.actors()` |
| `inspect` | `system`, `actor` | the actor's resource, as a tool result: a model cannot always read resources itself | readOnly | `ta_actor_inspect` |
| `describe` | `system`, `actor` or `definition` | the states, the events the current configuration accepts, the data model | readOnly | definition inspection (core ADRs 0042, 0071) |
| `evaluate` | `system`, `actor`, `expression` | evaluates an expression against a copy of the actor's data; what it writes is discarded | readOnly | the data model |
| `send` | `system`, `actor`, `event`, `data?`, `timeoutMs?` | delivers an event and returns the replies (below) | openWorld | `post` with an `origin` |
| `load` | `system`, `actor` | brings a named actor in, without sending it anything | idempotent | napi's loader (`setLoader`) |
| `goto` | `system`, `actor`, `states`, `from?` | an operator transition, queued behind the actor's mail | destructive | core ADR 0082 |
| `abandon` | `system`, `actor` | abandons a macrostep that never ends | destructive | core ADR 0082 |
| `cancel` | `system`, `actor`, `sendID` | withdraws one pending delayed send | destructive, idempotent | core ADR 0084 |
| `set` | `system`, `actor`, `location`, `value` | writes a data-model location; refused for an actor of a logged family | destructive | napi's write path; the test host captures and reloads (core ADR 0038) |
| `replace` | `system`, `actor`, `revision` | hot-replaces the actor with a registered revision | destructive | `prepare`, `publish` |
| `destroy` | `system`, `actor` | ends the actor | destructive | `ta_actor_destroy`, `system.destroy()` |

- **Logged actors stay replayable.** Every poke of an actor whose family
  is logged (napi ADR 0033, core ADR 0090) must be an input the log
  records, or a replay after a restart no longer arrives where the actor
  was:
  - `send` is an `event` record, `goto` a `jump`, `abandon` an
    `abandoned`, `cancel` a `withdrawn`, `destroy` a `destroyed`;
  - a data write has no record, so `set` answers `unsupported` for a
    logged actor and names `send` and `goto` instead;
  - `evaluate` never writes, for any actor: it runs against a copy of the
    data, so an agent can try an expression without changing anything.
    Writing is `set`'s job.
- **Nothing stops or steps an actor**, in either host. There are no
  breakpoints: what an actor did is in its trace (below), and `inspect`
  shows where it stands.
- **The tool list is static.** `toolsListChanged` is never sent.

## Replies to a send

A chart replies to whoever sent its event, with `_event.origin` and
`_event.origintype` (SCXML C.1; core ADR 0068 makes every origin
answerable). napi already has what an MCP caller needs, the same way an
application answers an HTTP request or a WebSocket connection:

- the server registers an I/O processor with `system.registerIO('mcp', …)`
  on each system it serves; it is listed in `_ioprocessors`;
- each `send` call opens a scope, `system.openScope('mcp',
  'mcp:call/<id>')`, with an `<id>` the server mints (MCP request IDs are
  the client's and can repeat across clients), and posts the event with
  `origin: { type: 'mcp', scope }`;
- the processor's `send()` collects every event sent to that scope while
  the call is open;
- when the call ends, the server closes the scope.

**The chart does not know it is talking to an agent.** It answers the
way it answers any sender:

```js
.on('quote.request', (t) => t.send('quote', (send) => send
  .via(({ event }) => event.originType)
  .to(({ event }) => event.origin)))
```

The same transition answers an HTTP handler, a WebSocket connection, or an
agent over MCP. A chart that never replies is fine too: the agent gets the
result of the send and reads the actor with `inspect`.

- **The type is `mcp` unless the application names another**
  (`registerMCPTools(server, { ioType })`), for an application that
  already registered an `mcp` processor of its own. Nothing else depends
  on the name: a chart that replies to its origin never writes it.
- **The reply goes back in the tool result.** 2026-07-28 has no
  connection, and the only way to the client is the response to a request
  in progress. So `send` keeps its call open and returns what reached its
  origin: a text block per reply for the model, and `structuredContent`
  with each reply's event name, data and sender.
- **How long a call stays open:** until nothing the send caused is
  runnable or due at the current time, followed through the records'
  causes (core ADR 0067), or until `timeoutMs`. A live system is never
  idle as a whole, so the send's own causes decide, not `flush()`. What
  counts as "the" reply is the application's protocol; the server returns
  every reply and does not guess.
- **A reply after the call ended** has no one to go to. The scope is
  closed, the processor throws, and the chart gets `error.communication`,
  as for any sender that went away (SCXML C.1). Nothing waits in an inbox.
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

A notification names the URI, and the client reads the resource again. It
is a live view, not a stream of records. Records go out as OTLP, as they do
now (napi ADR 0015). MCP's Logging is deprecated in 2026-07-28 in favour of
OpenTelemetry, and this server does not use it.

## Traces

A `tools/call` that carries `traceparent` and `tracestate` in `_meta`
(2026-07-28 documents the keys) passes them to `post`. The macrostep that
the call causes, and everything that macrostep causes, join the agent's
trace. The result names the trace ID in `structuredContent`.

## The MCP App (later)

`inspect` and `describe` declare `_meta.ui.resourceUri:
"ui://tinyactors/chart"`. The page draws the chart from the definition's
spec, marks the active configuration, and lists the pending delayed sends.
Its controls call `send`, `goto` and `cancel` through the host, and it
calls `inspect` again to refresh.

It is a later step, not the first: a developer's first client is usually a
coding agent in a terminal, which does not render Apps. The clients that do
are Claude (web and desktop), VS Code Copilot, Cursor, ChatGPT, Goose,
Postman and MCPJam. The tools and resources are complete without it.

## Access

The tools give full access to everything they serve. Authorization,
auditing and limits belong to the application's MCP server, or to an MCP
proxy in front of it, not to tinyactors. `evaluate` cannot change an actor,
but with the ECMAScript data model it runs code in the process, and every
destructive tool acts at once. The README says so where it shows how to add
the tools.

## Hosts

### napi: tools for the application's own MCP server

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
- **Options:** `systems` (names to systems), `ioType` (default `mcp`), and
  `prefix` for the tool names (default none), for an application whose own
  tools would clash.
- **The SDK is an optional peer dependency** of `@tinyactors/node`, loaded
  only by `@tinyactors/node/mcp`.

### The test host: the workbench's server

`bin/mcp` serves a workbench directory's system over stdio. A developer
starts it from an MCP client's configuration, the way `bin/dap` was started
from VS Code. The workbench's charts reload when their files change, as
they do now.

### Kept in step

- **Shared transcripts** keep the two in step, as the DAP transcripts did:
  a chart in both authoring forms, the requests a client sends, and the
  results and notifications it must get, with the same matchers
  (`$capture`, `$var`, `$expr`, ...). They live in `tinyactors-test/mcp/`,
  and both hosts play them in their test tasks.
- **Error codes:** MCP reserves `-32020` to `-32099`. The test host's own
  RPC codes (`-32001`, ...) are on its host protocol, not on the MCP
  server.

## Open questions

What is left is ours, not the application's:

1. **The SDK's support for 2026-07-28.** `registerMCPTools` assumes the
   official TypeScript SDK serves the stateless protocol,
   `subscriptions/listen` and `server/discover`. Check before an ADR, and
   decide what to do while it serves only 2025-11-25.
2. **Following a send's causes in napi.** The records carry causes, but
   napi has no call that says "nothing this send caused is runnable". It
   needs one for `send` to know when to answer.
3. **An evaluation against a copy.** Each data model needs a way to
   evaluate against a copy of an actor's data: a structured clone for
   napi's JavaScript values, a copy of the JSON for jq.
4. **A load without mail.** `load` needs a `LoadReason` of its own; napi's
   are `mail`, `timer` and `session`.

Whether core's stepping hooks (`pause_at_microstep_start`, action budgets)
become unused once the workbench no longer steps is checked in the change
that removes DAP. Scenarios exercise them, so they probably stay.

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
