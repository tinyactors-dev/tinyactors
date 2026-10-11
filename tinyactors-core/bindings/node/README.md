# tinyactors for Node.js

SCXML statecharts and actors on the tinyactors runtime. The addon is written
against the **Node-API C API** (`node_api.h`, no C++ wrapper) and links the
tinyactors static libraries. The JavaScript API lives in `core.mjs`, a thin
layer over the addon (`src/*.c`, one C file per topic). The [web binding](../web) runs the
same `core.mjs` (and the same `index.d.ts`) over the addon's wasm twin, so the same
code runs in Node.js and in browsers. Node.js ≥ 22.12 (`require()` of the
ES-module core).

## Install

```sh
npm install @tinyactors/node     # or: pnpm add, bun add
```

The package carries no compiler step: each supported platform has a
prebuilt addon in its own package (`@tinyactors/node-darwin-arm64`,
`-darwin-x64`, `-linux-x64`, `-linux-arm64`), an optional dependency that
npm, pnpm and Bun install only where it matches. The Linux addon serves
glibc and musl alike. Development builds are under the `next` dist-tag
(`npm install @tinyactors/node@next`).

In this repository, `mise run node:build` (in `tinyactors-core`) builds the
addon into `bindings/node/build/tinyactors.node`, which `index.js` loads
before any installed platform package. Packaging and releasing are in
[distribution](../../docs/distribution.md).

```js
const { scxml, createSystem, createActor, toPromise } = require('@tinyactors/node');

const c = scxml.chart();
const toggle = c.scxml('toggle', { initial: 'inactive', datamodel: 'ecmascript' },
  c.datamodel({ count: 0 }),
  c.state('inactive', c.transition('TOGGLE', 'active', c.assign('count', 'count + 1'))),
  c.state('active',
    c.onentry(c.send('timeout', { delay: '1s', id: 'timeout' })),
    c.onexit(c.cancel('timeout')),
    c.transition('TOGGLE', 'inactive'),
    c.transition('timeout', 'inactive'),
    c.transition('STOP', 'done')),
  c.final('done', c.donedata(c.param('toggles', c.expr(({ count }) => count)))));

const system = createSystem();
const actor = createActor(toggle, { system }).start();
actor.subscribe((s) => console.log(s.value, s.context));
actor.send({ type: 'TOGGLE' });
actor.send({ type: 'STOP' });
toPromise(actor).then(console.log); // { toggles: 1 }
```

## How it works

The core is a complete SCXML implementation; this binding exposes it and
adds nothing of its own semantics.
[The proposal](../../../docs/proposals/thin-js-binding.md) maps every item of
the API to the core functions behind it.

- **Charts are SCXML.** `fromSCXML(xml)` takes a document as it is.
  `scxml.chart()` gives one helper per SCXML element, which writes the same
  document: identifying attributes positionally (`c.state(id)`,
  `c.transition(event, target)`, `c.assign(location, expr)`,
  `c.send(event)`), then an object of attributes under SCXML's names
  (`{ cond, delay, targetexpr, ... }`), then children. A string is
  ECMAScript text; another value is written as JSON (`c.datamodel({ count:
  0, items: [] })`). `c.expr(fn)` is JavaScript code in the chart: the
  document holds the function's source, and `fn` is called with the
  session's variables and the system variables (`_event`, `_sessionid`,
  `_name`, `_ioprocessors`, `In`). `chart.xml` is the document.
- **Two `ecmascript` datamodels.** `createSystem({ ecmascript: 'native' })`
  (the default) keeps variables and event data as JavaScript values and
  calls `c.expr` functions directly; other expressions are compiled once
  per chart with `new Function` (a browser CSP must allow `'unsafe-eval'`
  for them). `ecmascript: 'quickjs'` runs the C datamodel, QuickJS in the
  system's memory: values are copied in and out as JSON, and `c.expr`
  functions run as their source text, so they must not close over host
  variables. Both pass the W3C suite.
- **One native system per `System`.** It is one `ta_host`: its segments,
  its run loop, its router. Charts are compiled into an image and added as
  a segment (`system.load(charts)`, or on the first `createActor` of a
  chart). Every actor has an address, `tinyactors://<system id>/#_scxml_<session>`,
  and one started with `systemID` is also `.../@<systemID>`. A `<send>` to
  any address works across segments and, through links, across systems.
- **Effects come back once per run.** Events are processed on `send()`; at
  the end of each native run the binding gets the snapshots that changed,
  the I/O and invocation requests, the timers to arm and the trace bytes,
  in one call. Calls made from inside an expression or a subscriber are
  queued until the run returns. Long runs yield (`setImmediate`), so a
  runaway eventless loop cannot block the event loop. A system runs on the
  JavaScript thread; for more cores, run systems in worker threads and link
  them — [docs/concurrency.md](../../docs/concurrency.md) explains what
  runs when.

## Actors

`createActor(chart, { system, input, systemID, traceparent, logger })`;
`input` overrides `<data>` of the same ids. `actor.send(event)` takes
`{ type, ...data }` (the event's name is `type`, its data the object) or a
name. `actor.getSnapshot()` is `{ value, context, status, output, error }`:
`value` the active states (an atomic state's id, `{ compound: value }`, or
`{ parallel: { region: value } }`), `context` the session's variables,
`output` the `<donedata>`. `toPromise(actor)` settles with the output when
the actor is done. `system.get(systemID)` and `system.actors()` find
actors, invoked ones included; `system.send(address, event, { sendid,
origin, origintype, raw, traceparent })` takes every field of an incoming
SCXML event; `system.sendMany([[actor, event], ...])` sends many in one
native call.

Semantics are SCXML's: an exception in an expression raises
`error.execution` in the actor, which keeps running (`snapshot.error`);
external events wait while a macrostep runs.

## I/O processors and invokers

The outside world comes in where SCXML puts it: a `<send type>` goes to an
I/O processor, an `<invoke type>` to an invoker.

```js
const system = createSystem({
  ioProcessors: {
    'https://shop.dev/io/payments': {
      location: 'payments', // shown in _ioprocessors
      send({ target, event, sendid, data, origin }, { system, signal, traceparent }) {
        return charge(target, data); // a throw or a rejection: error.communication in the sender
      },
    },
  },
  invokers: {
    'https://shop.dev/invoke/stock': {
      start({ invokeid, src, params }, { send, done, fail, signal }) {
        reserve(params.sku, signal).then((n) => done({ n }), fail); // done.invoke.<id> / error.communication
      },
      receive(invokeid, { name, data }) {}, // <send target="#_<invokeid>">, autoforwarded events
    },
  },
});
```

An invocation's `signal` aborts when its state exits or the system stops.

A processor with `sync: true` (`TA_IO_PROCESSOR_SYNC`) is called inside the
run, right after the `<send>` and before the sender's next action. What its
`send()` sends to the system (`system.send`, `actor.send`, `deliver`) comes
first, and a throw is `error.communication` on the sender's internal queue,
as SCXML wants for the BasicHTTP processor. Other calls into the system
throw while it runs. A returned promise is still asynchronous: its
rejection arrives later, as for any processor.

## Hibernation

Idle actors can be unloaded and brought back on demand. The system never does
I/O itself: you hand it a store (a `Map` will do; `get`, `set` and `delete`
may return promises).

```js
const system = createSystem({ hibernation: { store: new Map() } });
setInterval(() => system.sweep(), 30_000); // unload actors idle since the previous sweep
system.hibernate(actor);   // or one actor now (throws, saying why, when it cannot be)
actor.hibernated;          // true; getSnapshot() is answered from cache
actor.send({ type: 'X' }); // the event waits, the blob is loaded, the actor carries on
```

Variables are stored as neutral values (JSON, plus bytes and actor
references; docs/protocol.md, "Values"). An actor whose variables hold a
function stays resident: the sweep skips it, `hibernate()` throws naming
the variable. Pending delayed events wake the actor when they fire.

### Moving an actor to another system

`system.export(actor)` takes an actor out of its system as bytes — states,
variables, delayed events with their data, the actors it invoked.
`system.adopt(bytes)` takes it in anywhere the same charts are loaded:
another system in this process, another process, Node or web, either
datamodel. It keeps its session (its address under the new system's id),
and its delayed events keep the time they had left.

```js
const bytes = here.export(actor);   // it stops here (no done event)
// ... another process ...
const actor = there.adopt(bytes);
```

## New versions and live migration

```js
system.load(counterV2); // its <scxml name> gets a new version: new actors run it
const { done } = system.migrate('counter'); // move running actors (a job in time slices)
const { migrated, failed, pending } = await done; // pending: left behind (timeout)
system.migrate(actor, { wake: false, timeoutMs: 5000 }); // one actor; options
// in counterV2: <transition event="error.platform.upgrade"> ... </transition>
```

`load(charts)` compiles the charts into a new segment; the newest version
of each name is the current one. Addresses stay valid. `migrate(target?)`
moves actors of older versions (one actor, a chart or its name, or all) at
a stable point: states are matched by id, variables are kept (ones the new
version does not declare are dropped, new ones initialized), timers keep
running, and the actor receives `error.platform.upgrade`. Migration is a
job advanced in time slices, so the event loop stays responsive with any
number of actors. Hibernated actors are loaded from the store first
(`wake`, on by default when the system has a store). `timeoutMs` ends the
job and reports who was left behind. An actor mid-macrostep or with running
invocations waits until it can move.

## System images

A `<system>` manifest names charts and the actors to start:

```js
const { server } = createSystem().boot(`
  <system>
    <chart name="pinger" src="ping.scxml"/>
    <actor name="server" chart="pinger"/>
  </system>`, { documents: { 'ping.scxml': pingXML } });
```

`loadImage(bytes)` opens an image the C `tac` compiler wrote, to read its
hash, charts and actors.

## Snapshots and deterministic time

`system.snapshot()` returns the whole system as bytes: every actor's
configuration, variables, queued events and pending timers, plus the images
it runs. `createSystem({ restore: bytes, charts })` brings it back — same
system id and datamodel, so addresses stay valid; find actors with
`system.get(systemID)`. `charts` gives charts with `c.expr` functions their
functions back (otherwise they are compiled from their source). Hibernated
actors stay in the hibernation store, so restore with the same store.

```js
const bytes = system.snapshot();           // e.g. on shutdown
// ... a new process ...
const system = createSystem({ restore: bytes, charts: [lobby], hibernation: { store } });
system.get('lobby').send({ type: 'PING' });
```

`createSystem({ clock })` makes the system run on your clock instead of wall
time: it arms no timers itself; advance the clock and call
`system.flush()`, which runs everything that is due until the system is
idle — for deterministic tests, simulations and replays.

```js
let now = 0;
const system = createSystem({ clock: () => now });
createActor(withDelayedSend, { system }).start();
now += 1000;
system.flush();  // the delayed event has been taken
```

## Links: actors across systems

`createLink(system, { url, name })` connects to another system
(`serveLinks` on the other side). `link.get(systemID)` is a reference to an
actor there; `link.status()` the connection (`connecting`, `open`,
`reconnecting`, `closed`, with rtt, attempts and queued); `link.opened`
resolves once it is up.

```js
// server
const system = createSystem({ systemID: 'chess' });
createActor(lobby, { system, systemID: 'lobby' }).start();
// browsers are untrusted peers: they may send only what is exported
serveLinks(system, { server: httpServer, path: '/link', exports: { lobby: 'JOIN LEAVE' } }); // or { port }, { tcp }, or { accept }

// client (Node or the web binding); name: the server's system id
const client = createSystem();
const link = createLink(client, { url: 'ws://localhost:8080/link', name: 'chess' });
createActor(player, { system: client, input: { lobby: link.get('lobby').address } });
// in the player: <send event="JOIN" targetexpr="lobby"/>
// in the lobby:  <send event="WELCOME" targetexpr="_event.origin"/>   — the reply crosses back
```

Every actor has an `address`. An actor sent in event data travels as a
reference (`{"$ref": address}`) and arrives as an `ActorRef` (with `send()`
and `address`); `system.ref(address)` creates one. `system.stop()` stops all
actors, closes the system's links and frees its native memory.

Routing is done by the native router (`ta_host`, docs/protocol.md), and the
link protocol (v1: `hello`, `ev`, `err`, `ping`/`pong`; reconnects,
resumption, exports) by core's `ta_links`, not in JS: every link
connection is one router *peer*, and the binding only moves frames. What that
gives you:

- **Policy.** A served peer is untrusted unless `trusted: true` or
  `trust(hello)` says so: it may send only to the `exports` (`['lobby']`,
  `{ lobby: 'JOIN LEAVE' }`, `'*'` for every name and session). Everything
  else, and every forged `origin`, is refused.
- **No silent drops.** A refused or undeliverable send — unknown name, ended
  session, no route, denied, queue full, expired — raises
  `error.communication` (with the `sendid`) in the sending actor, across any
  number of hops.
- **Queues and resumption.** While a link is down the router queues for it
  (`maxQueue`, default 1000; `queueTTLMs` to expire them; `maxQueue: 0` to fail
  fast). On the server, a peer that reconnects with the same system id within
  `resumeMs` (default 30 s, on the system's clock: with a `clock` option,
  simulated time) gets what was queued for it, in order.
- **Relaying.** With `announce: true` a system tells its peers which other
  systems it reaches, and forwards their envelopes (for trusted peers): a
  browser can reach a backend behind its server.
- **Values.** Event data travels as neutral values (JSON plus `{"$ref"}`
  and `{"$bytes"}`; docs/protocol.md, "Values"), whatever language is on
  the other side.
- **Status.** `link.status()` and `server.status(systemID)` report the route:
  state, queued, lag, rtt, sent/received/errors/duplicates.
- `system.deliver(envelope)` hands an envelope to the router from host code
  (any transport you write yourself starts there).

### Byte streams: TCP, stdio, serial

On byte streams the link protocol travels in length-prefixed frames (a 4-byte
big-endian length, then UTF-8 JSON; docs/protocol.md, "Framing"):

```js
// TCP
const served = serveLinks(server, { tcp: { port: 7000 }, exports: ['echo'] });   // served.port
const link = createLink(client, { tcp: { host: 'db-1', port: 7000 }, name: 'srv' });

// any duplex stream: `transport` and `accept` take byte streams as well as WebSocket-like objects
const child = spawn('node', ['worker.js'], { stdio: ['pipe', 'pipe', 'inherit'] });
serveLinks(system, { trusted: true, accept: (on) => on({ readable: child.stdout, writable: child.stdin }) });
// in worker.js (stdout carries the link: log with createSystem({ logger }) to stderr)
createLink(system, { name: 'parent', transport: () => ({ readable: process.stdin, writable: process.stdout }) });
```

A byte stream is a Node-style duplex (a `net.Socket`) or a `{ readable,
writable }` pair. `maxFrame` (default 1 MiB, both functions) is the largest
frame this side accepts and is announced in its hello. Frames above it are
skipped unread. An envelope that does not fit the other side's `maxFrame` is
not sent: its sender gets `error.communication`. `encodeFrame(text)` and
`new FrameDecoder({ maxFrame }).push(chunk)` (frames out; a refused one as
`{ reason: 'malformed', length }`) frame streams of your own; they are
`ta_frame_header` / `ta_frame_read` (`include/tinyactors/frame.h`), as
`parseAddress` and `formatAddress` are `address.h`.

### Store links: actors in PostgreSQL

`createStoreLink(system, { query, listen })` connects the system to a
database of actors (the PostgreSQL extension, bindings/postgres) through any
PostgreSQL client. The database becomes a peer: actors here and there send to
each other by address, replies included, and the database can reach the
browsers on your links (the link tells it which ones).

```js
const pool = new pg.Pool();
const client = await pool.connect();             // a connection for LISTEN
const store = createStoreLink(system, {
  query: (sql, params) => pool.query(sql, params).then((r) => r.rows),
  listen: async (channel, fn) => { client.on('notification', (n) => n.channel === channel && fn(n.payload));
                                   await client.query(`LISTEN ${channel}`); return () => client.query(`UNLISTEN ${channel}`); },
});
await store.ready;                               // store.id: the database's system id
system.send(`tinyactors://${store.id}/@ledger`, { type: 'add', amount: 5 });
```

Envelopes go in with `tinyactors.deliver` (in order, retried until taken;
the database drops repeats), come out of the outbox with `claim_for` on
NOTIFY (and every `pollMs`), and errors travel both ways as
`error.communication`. `store.flush()` waits until everything was handed
over; `store.status()` is the route's status.

## Traces

Every system carries a trace through every step, as W3C trace contexts:
an event a step sends, raises, delays or hands to another system over a
link continues the trace of the step that sent it. Give a `traceparent`
where an event comes from outside, and the step it starts is a child of
your span:

```js
createActor(chart, { system, traceparent: req.headers.traceparent });
actor.send({ type: 'charge' }, { traceparent });
system.ref(address).send({ type: 'charge' }, { traceparent });
system.sendMany([[actor, { type: 'charge' }, { traceparent }]]);
system.deliver({ target, name: 'charge', traceparent });
```

Core writes the records (macrosteps, microsteps, conditions, raised and
sent events, invocations, assignments, logs, failures, and an actor's
spawn, hibernation, wake and end) and C encodes them as OpenTelemetry's
OTLP protobuf after every run: the bytes reach JavaScript as one
`Uint8Array`, never as an object per span. Span names follow
`scxml.macrostep <event>`, `scxml.microstep <event>`, `scxml.spawned`,
`scxml.finished`; every span carries `tinyactors.actor.name`,
`tinyactors.chart.name` and `tinyactors.span.kind`.

**Export with no code.** With an endpoint, a system posts its traces,
logs and metrics to an OTLP/HTTP collector (protobuf; `/v1/traces`,
`/v1/logs` and `/v1/metrics` under the endpoint):

| Variable | Becomes |
|---|---|
| `OTEL_EXPORTER_OTLP_ENDPOINT` | `telemetry.endpoint` (`/v1/traces`, `/v1/logs` and `/v1/metrics` are added) |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | `telemetry.tracesEndpoint`, used as it is (logs are not sent unless the endpoint above is set too) |
| `OTEL_EXPORTER_OTLP_HEADERS` | `telemetry.headers` (`key=value,key=value`, values percent-decoded) |
| `OTEL_SERVICE_NAME` | the resource's `service.name` |
| `OTEL_RESOURCE_ATTRIBUTES` | `telemetry.resource` (`key=value,...`) |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | `telemetry.metricsEndpoint`, used as it is |
| `OTEL_METRIC_EXPORT_INTERVAL` | `telemetry.metricsInterval` (ms between metrics exports, default 60000) |
| `OTEL_METRICS_EXPORTER=none` | no metrics export (`telemetry.metricsEndpoint: ''`) |
| `OTEL_TRACES_SAMPLER_ARG` | `telemetry.sample` (0 to 1) |
| `OTEL_SDK_DISABLED=true` | no export |

They are read once, when the binding loads (`telemetry.js`), and options
an application passes to `createSystem` win over them.

```js
const system = createSystem({
  telemetry: {
    endpoint: 'http://localhost:4318',
    headers: { authorization: 'Bearer ...' },
    resource: { 'service.name': 'checkout' },
    sample: 0.1, // a tenth of the traces this system starts; a traceparent keeps its caller's decision
  },
});
await system.flushTraces(); // what was gathered, posted
```

Posts gather for a quarter of a second; a failing collector costs a log
line every ten seconds at most. `telemetry: false` turns export off.

**A sink** takes the bytes instead, for applications that route them
themselves (a queue, a file, a test):

```js
system.setTraceSink((traces, logs) => queue.push(traces, logs), {
  detail: 'full',        // 'summary' | 'decisions' | 'full' (default)
  kinds: undefined,      // or the record kinds wanted (TRACE_KINDS)
  values: false,         // event data and assigned values as attributes (they may hold personal data)
  valueLimit: 256,       // bytes of a value, cut there
  sample: 1,
  parent: 'cause',       // or 'link': each macrostep a root, linked to its cause
  resource: { 'service.name': 'checkout' },
});
system.configureTraces({ detail: 'summary', sample: 0.5 }); // while it runs
```

**Watching and rules** (docs/adr/0007): a watch traces matching steps in
full, whatever the ratio, and everything they cause, until `unwatchTraces(id)` or
`for` ms (15 minutes by default); rules decide where a trace starts and
where an actor takes an event, first match wins:

```js
const watch = system.watchTraces({ actors: [order], names: ['issue/*'], charts: ['checkout'], detail: 'full', values: true });
system.unwatchTraces(watch);
system.configureTraces({
  sample: 0.01,
  rules: [
    { chart: 'telemetry-sink', detail: 'off' },              // never traced
    { at: 'start', from: 'host', sample: true, perSecond: 5 }, // up to 5 host inputs a second
    { chart: 'checkout', sample: 0.1 },                       // consistent with the 1% default
  ],
});
```

A step a rule samples in an unsampled trace begins a new trace. An
unsampled trace keeps its ID: what it sends carries `traceparent ...-00`.

`createSystem({ traces: { sink, seed, ...options } })` installs it from the
start; `seed` makes the trace IDs a system starts repeatable (tests). With
neither a sink nor an endpoint, core writes no records at all; the trace
contexts still flow, so a sink installed later continues the traces
already running. `stop()` ends the spans still open, marked incomplete.

`decodeTraces(bytes)` reads OTLP protobuf back as plain spans (tests,
tools); `parseTraceparent`, `formatTraceparent` and `stepSpanID(session,
macrostep, microstep)` are the same C functions core uses, so a host
computes the span ID of any step without seeing its records.

## Metrics

`system.metrics()` returns what the system counted, as plain data
(docs/adr/0007 in the repository root): per chart (`tinyactors.chart.name`)
its actors (resident and hibernated), spawns, ends, macrosteps, fuel,
errors (`scxml.error.type`) and faults; the pools and their limits, memory,
the run queue, how late the earliest timer is, delivered events, lost trace
records, and the traces started and sampled; and `tinyactors.run.duration`,
how long each run blocked the loop. Sums are cumulative since
`startTime`. Core counts once per scheduling turn and where something
fails, never per event; latencies come from spans: a collector's
spanmetrics, or `telemetry.spanMetrics: true`, which adds
`tinyactors.macrostep.duration` per chart from the macrostep spans the
encoder writes, each bucket's exemplar one of those spans.

```js
const snapshot = system.metrics(); // { startTime, time, metrics: [{ name, unit, kind, points }] }
const bytes = metricsToOTLP(snapshot, { resource: { 'service.name': 'checkout' } }); // post to /v1/metrics
```

`metricsToOTLP` takes any snapshot of that shape, so an application
encodes its own metrics with the same C writer. With an endpoint, the
exporter posts `/v1/metrics` every minute (`telemetry.metricsInterval`, in
ms; 0 only when the system stops) and once more at `stop()`, adding what
`telemetry.metrics()` returns; `telemetry.metricsEndpoint` names the URL
(`''` sends none).

## Build & test

```sh
mise run node:test     # npm ci, build the addon, node --test, bun test, tsc
```
