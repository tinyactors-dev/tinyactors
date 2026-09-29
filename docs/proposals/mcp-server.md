# An MCP server: connect an agent to a system

**Status:** Proposed (2026-09-29)
**Touches:** tinyactors-napi (an MCP server; DAP removed), tinyactors-test (an MCP server for the workbench; DAP and its transcripts removed), tinyactors-vscode (removed), root docs and AGENTS.md
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
  where the server is created: `serveMCP({ systems: { orders, billing } })`.
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
| `evaluate` | `system`, `actor`, `expression` | evaluates an expression in the actor's data model | not readOnly | the data model |
| `send` | `system`, `actor`, `event`, `data?`, `until?`, `timeoutMs?` | delivers an event and returns the replies (below) | openWorld | `post` |
| `load` | `system`, `actor` | brings a named actor in, without sending it anything | idempotent | napi's loader (`setLoader`) |
| `goto` | `system`, `actor`, `states`, `from?` | an operator transition, queued behind the actor's mail | destructive | core ADR 0082 |
| `abandon` | `system`, `actor` | abandons a macrostep that never ends | destructive | core ADR 0082 |
| `cancel` | `system`, `actor`, `sendID` | withdraws one pending delayed send | destructive, idempotent | core ADR 0084 |
| `set` | `system`, `actor`, `location`, `value` | writes a data-model location | destructive | napi's write path; the test host captures and reloads (core ADR 0038) |
| `replace` | `system`, `actor`, `revision` | hot-replaces the actor with a registered revision | destructive | `prepare`, `publish` |
| `destroy` | `system`, `actor` | ends the actor | destructive | `ta_actor_destroy`, `system.destroy()` |

- **`load`** needs a load reason of its own: napi's `LoadReason` is `mail`,
  `timer` or `session` today, and a load that no mail asked for is none of
  them.
- **Nothing stops or steps an actor**, in either host. There are no
  breakpoints: what an actor did is in its trace (below), and `inspect`
  shows where it stands.
- **The tool list is static.** `toolsListChanged` is never sent.

## Replies to a send

A chart replies to whoever sent its event, using `_event.origin` and
`_event.origintype` (SCXML C.1; core ADR 0068 makes every origin
answerable). An event from MCP has an origin, and the chart can reply to it
like to any other.

- **The server registers an I/O processor** of type `mcp` in the system.
  It is listed in `_ioprocessors`.
- **An MCP `send` has an origin of its own**, `mcp:call/<id>`, where `<id>`
  is minted by the server for that call. MCP request IDs are the client's
  and can repeat across clients.
- **The reply goes back as the tool result.** 2026-07-28 has no connection
  and no channel to the client except the response to a request in
  progress. So `send` keeps its call open, collects every event sent to its
  origin, and returns them: a text block per reply for the model, and
  `structuredContent` with each reply's name, data and sender.
- **How long a call stays open:** by default until the system settles.
  Nothing runnable and nothing due at the current time is left of what the
  send caused, followed through the records' causes (core ADR 0067). A
  live napi system is never idle as a whole, so causes decide, not the
  whole system. `until` (a state of the target, or `reply`) and
  `timeoutMs` change it.
- **A reply after the call ended** finds nobody at the origin, and the
  sender gets `error.communication` (SCXML C.1, an inaccessible target).
  Nothing is kept in an inbox.
- **A long wait** is a task, when the client supports the Tasks extension:
  `send` answers a task handle, and `tasks/get` answers the replies once
  the wait ends.

A chart can also send to the `mcp` processor on its own, to an origin it
kept. That works while the call is open, and raises `error.communication`
after it.

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

## The MCP App

`inspect` and `describe` declare `_meta.ui.resourceUri:
"ui://tinyactors/chart"`. The page draws the chart from the definition's
spec, marks the active configuration, and lists the pending delayed sends.
Its controls call `send`, `goto` and `cancel` through the host, and it
calls `inspect` again to refresh. It runs in every client with MCP Apps:
Claude, VS Code Copilot, Cursor, ChatGPT, Goose, Postman, MCPJam.

## Access

The server gives full access to everything it serves. Authorization,
auditing and limits belong in an MCP proxy in front of it, or in the
middleware of the application that mounts it, not in tinyactors.

- napi's server is a request handler for Streamable HTTP, which the
  application mounts on a route of its own, and a stdio loop.
- The test host's server speaks stdio from the workbench.
- `evaluate` runs code in the process with the ECMAScript data model, and
  every destructive tool acts at once. The README says so where it shows
  how to mount the server.

## Hosts

| | Transport | Serves |
|---|---|---|
| napi | Streamable HTTP (a handler), stdio | the systems given to `serveMCP` |
| test host | stdio, from the Ruby workbench | the workbench's system |

- **Shared transcripts** keep the two servers in step, as the DAP
  transcripts did: a chart in both authoring forms, the requests a client
  sends, and the results and notifications it must get, with the same
  matchers (`$capture`, `$var`, `$expr`, ...). They live in
  `tinyactors-test/mcp/`, and both hosts play them in their test tasks.
- **Error codes:** MCP reserves `-32020` to `-32099`. The test host's own
  RPC codes (`-32001`, ...) are on its host protocol, not on the MCP
  server.

## Open questions

1. **The name of the `mcp` I/O processor type**, and whether its long form
   is a URI, as SCXML's own types are.
2. **What `until: reply` means** when a chart replies more than once.
3. **Whether `set` stays** once `evaluate` can assign, or both remain.
4. **How the MCP App gets updates**: whether a host passes resource
   updates into an App, or the page polls `inspect`.
5. **The test host's stepping hooks in core** (`pause_at_microstep_start`,
   action budgets): whether any become unused once the workbench no longer
   steps. Scenarios exercise them, so they probably stay.

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
