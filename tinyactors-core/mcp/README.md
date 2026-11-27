# The MCP server's catalog

`tools.json` is the one list of the tools every language's MCP server
offers: the main interface an agent has to a running system. Core's code
never reads it. It lives beside core because every binding builds the core
of its release, so each binding's package ships the catalog of that core
(Node: `@tinyactors/node/mcp-tools.json`), and a server and the requests it
forwards come from one release. A change here reruns every binding's tests.

The servers are Node's (`tinyactors-napi/mcp/`, `@tinyactors/mcp`) and
Python's (`tinyactors-cpython/mcp/`, `tinyactors_mcp`); this page is what
each implements, and what another one (Go, Java) would. The MCP suite
(`tinyactors-test/docs/mcp.md`) holds every one to the same results: a
new server comes with a fixture program for it.

## A server is three parts

1. **The catalog.** List the tools as they are written: `name`,
   `description`, `inputSchema`, `annotations`. On a call, check the
   arguments against the schema and fill in its defaults. The schemas use
   a small subset of JSON Schema: an object of `properties`, each with a
   `type` (`string`, `number`, `integer`, `boolean`, `array`, `object`) or
   none (any value), or an `enum`, with `default`, `description`, `items`,
   `additionalProperties`, and the object's `required`. An argument the
   schema does not name is refused.
2. **The gate.** A tool with `gate` runs only when the server was started
   with that option set (`allowDeploy`, `allowTerminate`); otherwise it is
   refused, naming the option.
3. **What runs it** (`run`), one of:
   - `{"snapshot": "<method>"}`: a method of core's snapshot reader
     ([docs/snapshot-requests.md](../docs/snapshot-requests.md)), on the
     snapshot the `snapshot` argument names (one the `snapshot` tool took
     and the server keeps), with the other arguments as its params. The
     result is the reply's `result`; an error reply is the tool's error.
   - `{"host": "<method>"}` or `{"host": {"<key>": "<method>", ...}}`:
     core's requests ([docs/requests.md](../docs/requests.md)) through the
     binding's typed methods. The server keeps a table from a method's name
     to its typed method (no binding gains a raw post). The params are the
     arguments without `system`, which names the host, and with `actor`
     resolved to `session`. A map runs each method and gives each result
     under its key, a refusal as `{"refused": message}`.
   - `{"binding": "<operation>"}`: the language's own code, below.

## Arguments and results

- **`system`** names a host the server serves; **`actor`** is a session id
  or the name an actor was spawned with (`host.actor`, by session or by
  name); **`snapshot`** an id the `snapshot` tool gave; **`draft`** an id
  the `compile` tool gave.
- **Units:** a param forwarded to core has core's unit (`trace.watch`'s
  `seconds`); the binding operations take milliseconds (`timeoutMs`,
  `advanceMs`).
- **Results** are JSON in the tool's `structuredContent` (an array or a
  scalar under `items`) and as indented text in its `content`. An integer
  past 2^53 is its decimal text; bytes are `<n bytes>`.
- **Errors** are a result with `isError`, its text the error's name and
  message, its `structuredContent` `{"error"}` (and core's `reason` and
  `path` when core refused).

## The binding operations

What the catalog marks `{"binding": ...}`, and what each must do:

| Operation | What it does |
|---|---|
| `systems` | each system: its name (`system`), its charts' names (`charts`, `host.stats`), its `actors`, `hibernated` actors and `segments` |
| `send` | the event to the actor (the binding's send); returns once it is in the mailbox |
| `ask` | the binding's `ask`: the event, and the actor's reply to `_event.origin`, refused after `timeoutMs` |
| `traces`, `logs` | what the tap kept: the spans the host exported (decoded) and the `<log>` lines, newest last, filtered by session (and a span name prefix), the last `limit` |
| `services` | the running invocations the tap saw, by invoking session: `invokeid`, `type`, `src` |
| `end_service` | the tap's invocation ended as its service would: `done` or `error` (`error.communication` sent back) |
| `chart_source` | the SCXML the application gave for the chart, which `deploy` replaces |
| `compile` | the compiler on the text; a draft id and its diagnostics, or the diagnostics as the error |
| `deploy` | the draft's image added to the host; with `migrate`, the chart's actors migrated (`wake`: the hibernated ones too) and the migration's result awaited for `timeoutMs`, failures grouped by reason with five sessions of each |
| `terminate` | the actor ended |
| `snapshot` | the host's snapshot, opened with the binding's snapshot reader and kept under a new id (the oldest dropped past `snapshotKeep`, 8); a snapshot refused because the host is not quiescent is tried again for about a second |
| `snapshot_variables` | the sessions `snapshot.actors` lists for the chart and state (or the one session), resident only, at most `limit`, their variables read with `actor.context` from a copy (below), posted before the copy's first turn |
| `rehearse` | a copy (below) with the draft added and the chart migrated, run for `advanceMs` on its simulated clock; the migration's result, the chart's resting configurations before (the snapshot's) and after (the copy's own snapshot, `snapshot.summary`), what the stand-ins received, the copy's `<log>` lines and errors |

## The tap

A handler is fixed when its host is made, so the server reads what the
handlers see through a **tap**: it wraps the options the application
gives its host, calls every handler the application gave, and keeps, with
limits:

- the spans the telemetry handler exports, decoded;
- the `<log>` lines;
- the migrations' results, and who waits for the next;
- the running invocations, by invoking session;
- the options themselves, for copies.

## Copies and stand-ins

A **copy** is a host restored from a snapshot, with the system, config,
datamodel (and its options) of the host it copies, on a simulated clock.
Its I/O processors, services and transport are **stand-ins**, which record
what they receive and touch nothing outside the copy:

- an I/O processor: records `<type> <event>`;
- the transport: records `transport <event>`;
- a service: records `<type>` (`<type> (restored)` for a restored
  invocation) and answers as the call says, by type: `silent` (never; the
  default), `done`, `error` (`error.communication` sent back), or the name
  of an event it sends back.

A copy has no store: a hibernated actor stays hibernated in it. It is
closed before the tool returns.
