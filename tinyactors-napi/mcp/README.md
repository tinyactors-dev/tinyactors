# @tinyactors/mcp

A [Model Context Protocol](https://modelcontextprotocol.io) server over
running [`@tinyactors/node`](../README.md) hosts. An agent connected to it
can:

- inspect a running system;
- send events and ask questions;
- read the spans and `<log>` lines the hosts export;
- take snapshots and read every actor in them;
- compile a new version of a chart and rehearse its migration on a copy;
- deploy the version and migrate the running actors.

The package is opt-in and has no native code. It is JavaScript over the
public API of `@tinyactors/node` and over the MCP SDK
(`@modelcontextprotocol/sdk`).

```sh
npm install @tinyactors/node @tinyactors/mcp
```

## Serving a host

A handler is fixed when its host is made, so the server reads what the
handlers see through a **tap**. `tap().options(...)` wraps the host's
options and keeps a copy of the spans, the log lines, the migrations'
results and the running services. Every handler you give is still called.

```js
import { compileDir, Host, MemoryStore } from '@tinyactors/node';
import { createMCP, tap } from '@tinyactors/mcp';

const t = tap();
const host = new Host(compileDir('charts'), t.options({
  system: 'shop',
  config: { telemetry: { flushInterval: 1000 } },
  services: { payment: startPayment },
  ioProcessors: { warehouse },
  store: new MemoryStore(),
  resident: 1000,
}));

const mcp = createMCP(
  { shop: { host, tap: t, sources: { order: orderSCXML } } },
  { allowDeploy: true },
);
const served = await mcp.listen({ port: 7333, token: process.env.MCP_TOKEN });
console.log(served.url); // http://127.0.0.1:7333/mcp
```

`listen()` serves Streamable HTTP on `127.0.0.1` and makes one MCP server
per MCP session. `token` is a bearer token that every request must carry.
To use another transport, connect `mcp.server()` to it. Each call to
`server()` makes a new server, and all of them share the snapshots and
the drafts.

`sources` maps a chart's name to its SCXML. An image holds no source, so
`chart_source` can show only what you give here. `deploy` writes the new
text here.

## The tools

**Live tools** act on the host now:

| Tool | What it does |
|---|---|
| `systems` | lists the systems served, their charts and how many actors each holds |
| `host_stats` | shows the host's counters: actors, hibernated actors, pools, and turns, errors and faults per chart |
| `actor` | shows one actor: its state, mailbox, delayed sends and variables |
| `send`, `ask` | send an event; `ask` waits for the actor's reply to `_event.origin` |
| `trace_watch`, `trace_unwatch` | trace matching actors in more detail for a while |
| `traces`, `logs` | read the recent spans and `<log>` lines that the tap kept |
| `services` | lists the running services by invoking session |
| `end_service` | ends a service that will never answer, as `done.invoke` or `error.communication` (needs `allowDeploy`) |
| `chart_source`, `compile` | show a chart's SCXML; compile a draft without running it |
| `deploy` | adds a draft's image to the host and can migrate the chart's actors to it (needs `allowDeploy`) |
| `terminate` | ends an actor (needs `allowTerminate`) |

**Snapshot tools** read one snapshot of a host. A snapshot holds every
actor at one moment, and they read it with `readSnapshot`, core's own
readers of a snapshot of this build, which make no host and run
nothing:

| Tool | What it does |
|---|---|
| `snapshot` | takes a snapshot of a system and keeps it under an id |
| `snapshot_summary` | shows the versions (segments) and how many actors rest in each state |
| `snapshot_actors` | lists the actors by chart, state, status or name, paged |
| `snapshot_variables` | reads the actors' variables from a copy restored from the snapshot |
| `snapshot_chart` | shows a chart's states with the count of actors in each, and its transitions and data |
| `snapshot_traces` | reads the trace history that the snapshot kept: times on the host's clock, and no names or values given at run time in the spans a host had already exported |
| `rehearse` | migrates a copy of the snapshot to a draft and runs the copy on a simulated clock |

A **copy** is a host restored from a snapshot. It has the system, the
config and the datamodel of the host it copies. Its I/O processors, its
services and its transport are stand-ins, which record what they receive
and touch nothing outside the copy. The live host goes on as before. In
`rehearse`, each stand-in service is silent by default. It can also end
with `done`, fail with `error`, or send an event back:
`services: {payment: 'payment.ok'}`. A copy has no store, so a
hibernated actor stays hibernated in it, and its variables are in your
store.

## What an agent can and cannot do

- A snapshot is refused while the host is not quiescent: during a
  migration, or with a reply pending. `snapshot` tries again for about
  a second, and then reports the refusal.
- `deploy` without `migrate` only adds the version: actors spawned from
  then on run it, and the running actors finish on their own version.
  With `migrate`, the running actors move to the new version. An actor
  keeps a running service when the new version keeps the `<invoke>` in a
  kept state. The other services are stopped.
- The destructive tools are off unless the server allows them. Run the
  server on loopback, or behind your own authentication, and give it a
  token. Every tool the server offers acts as the host's application
  would.

## Development

The tests run against the addon built in the directory above
(`mise run build` there):

```sh
pnpm install
node --test test/
```
