# tinyactors for browsers (and anything with WebAssembly)

The same API as the [Node.js binding](../node) — the same `index.d.ts` —
on its own implementation: the tinyactors libraries compiled to
WebAssembly, driven from JavaScript over their C API. The binding has no
C of its own.

## Installable distribution

Packages are not built at the moment: the pipeline that made them used
CMake and is to be rebuilt on the repository's release machinery
([distribution](../../docs/distribution.md)). In this repository, `mise run web:build` (in `tinyactors-core`) builds
`bindings/web/tinyactors.wasm`, `bindings/web/api.mjs` and `bindings/web/core.mjs`. The package contains the wasm, its JS and
the Node transports, with no files needed outside it. `import ... from 'tinyactors-web'` selects the Node or
browser backend; `tinyactors-web/browser` explicitly selects the browser.
For an unbundled server, import
`./node_modules/tinyactors-web/bindings/web/index.mjs` and serve the whole
package so its relative JS and wasm URLs resolve.
See [distribution](../../docs/distribution.md); publish the generated tarball,
not this development directory.

```html
<script type="module">
  import { scxml, createActor, createSystem, createLink } from './bindings/web/index.mjs';

  const c = scxml.chart();
  const toggle = c.scxml('toggle', { initial: 'off' },
    c.state('off', c.transition('TOGGLE', 'on')),
    c.state('on', c.transition('TOGGLE', 'off')));
  const system = createSystem();
  const actor = createActor(toggle, { system }).start();
  actor.subscribe((s) => console.log(s.value));
  actor.send({ type: 'TOGGLE' });

  // talk to actors on a server
  const link = createLink(system, { url: `wss://${location.host}/link`, name: 'server' });
  link.opened.then(() => console.log('link', link.status()));
</script>
```

- `tinyactors.wasm` (about 1.5 MB, 510 KB gzipped) is the release's three
  libraries — `tinyactors` (the core, the host, the OTLP codec),
  `tinyactors_runtime` (the compiler: charts compile in the page) and
  `tinyactors_datamodel_ecmascript` (QuickJS, most of the size) — linked
  into one module as the release links its wasm modules (`mise run
  web:build`): one memory, wasi-libc compiled in, importing nothing,
  exporting the public API and its function table.
- `native.mjs` is the wasm twin of the Node.js addon (`../node/src/*.c`):
  the same functions, topic by topic, with the same results, over the
  module's exports instead of Node-API. The run loop, links, the
  exporter's batching, trace policy, watches, snapshots and metrics are
  core's (`ta_host`, `ta_links`, `ta_otlp_batch`); it provides memory, the
  clock and the JavaScript datamodel. Structs are read and
  written at the wasm32 offsets of `api.mjs`, which web:build generates
  from the API descriptions; what it hands the libraries lives in the
  module's memory (`ta_wasm_alloc`): scratch released after each call, a
  system's blocks until it is destroyed, an image's until nothing holds
  it.
- Two `ecmascript` datamodels. `native` (the default) is the page's own
  JavaScript: variables and event data are JS values, `c.expr` functions
  are called directly, and the vtable entries the core calls are
  JavaScript functions placed in the module's function table (a small
  wasm module generated at load turns them into wasm functions), with
  integer handles for sessions and payloads. `quickjs` is the engine in
  the module; its clock is set from the page before each run.
- `core.mjs` is the API: a copy of the Node.js binding's, made by
  `mise run web:build`, so both bindings run the same file.
- `index.mjs` is for browsers (top-level `await`, `instantiateStreaming`).
  `node.mjs` is the same binding for Node.js: synchronous, and `serveLinks()`
  also accepts `{ server | port }`. Pick it with the `node` export condition.
- A pump runs at most 16 rounds of 10,000 steps, then the rest continues
  on a `MessageChannel` turn, so a busy system never blocks the page.
- SCXML documents (`fromSCXML`), I/O processors (synchronous ones too) and
  invokers, system manifests (`system.boot`), snapshots
  (`system.snapshot()`, `createSystem({ restore })`) and deterministic time
  (`createSystem({ clock })` + `flush()`) work as in the
  [Node binding](../node#how-it-works).
- Hibernation works here too: `createSystem({ hibernation: { store } })`, with
  a store backed by memory, IndexedDB or anything else (sync or async). Blobs
  are portable between the web and Node bindings.
- Actors move between systems with `system.export(actor)` and
  `system.adopt(bytes)` — between processes, and between this binding and
  the Node one.

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

A browser has no environment: the system exports only when the
application passes an endpoint (posted with `fetch`, so the collector must
allow the page's origin). In Node.js, `bindings/web/node.mjs` posts with
Node's HTTP client but reads no environment either; the Node.js addon's
entry is the one that reads `OTEL_*` variables.

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

## Call cost

`node tools/bench_js.mjs` runs the same workloads through both bindings
and both datamodels, each in its own process (deterministic clock,
synchronous flushes; best of three; per second, resident MB after).
Intel Xeon @ 2.80 GHz, 4 CPUs, Node 22, Linux, measured side by side with
the web binding before this one (the addon's C over a Node-API subset
emulated in JavaScript):

| workload | web native, before | web native | web quickjs, before | web quickjs | node native | node quickjs |
|---|---:|---:|---:|---:|---:|---:|
| send, 0 subscribed | 151 k (71) | 254 k (75) | 69 k (81) | 74 k (106) | 190 k (65) | 150 k (66) |
| send, 10 subscribed | 100 k (70) | 173 k (77) | 24 k (81) | 25 k (109) | 110 k (70) | 42 k (72) |
| send, 1000 subscribed | 68 k (124) | 162 k (130) | 23 k (89) | 28 k (136) | 80 k (109) | 39 k (81) |
| sendMany | 196 k (151) | 606 k (157) | 97 k (133) | 128 k (170) | 279 k (152) | 195 k (107) |
| spawn | 58 k (166) | 107 k (174) | 93 k (145) | 113 k (197) | 143 k (152) | 182 k (112) |
| hibernate | 83 k (583) | 122 k (566) | 29 k (574) | 33 k (592) | 120 k (189) | 51 k (185) |
| wake + event | 31 k (583) | 51 k (566) | 28 k (574) | 46 k (592) | 22 k (189) | 26 k (185) |

The JavaScript datamodel gains most: a datamodel call is one call from
wasm into a JavaScript function, with integer arguments, where the
emulated Node-API took a crossing for every value it touched. A
subscribed actor costs a snapshot after each run that changed it; with
quickjs that snapshot copies the variables out of the engine as JSON,
which is why subscribers cost it more.

## Build & test

```sh
mise run web:build    # zig cc → bindings/web/tinyactors.wasm
mise run web:test     # the Node binding's test suite against the wasm backend + API identity checks
```
