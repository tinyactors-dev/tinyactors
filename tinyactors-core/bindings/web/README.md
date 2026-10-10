# tinyactors for browsers (and anything with WebAssembly)

The same xstate-style API as the [Node.js binding](../node) — the same
`index.d.ts` (one shared core, `bindings/shared/core.mjs`) — over the tinyactors
runtime compiled to WebAssembly.

## Installable distribution

Packages are not built at the moment: the pipeline that made them used
CMake and is to be rebuilt on the repository's release machinery
([distribution](../../docs/distribution.md)). In this repository, `mise run web:build` (in `tinyactors-core`) builds
`bindings/web/tinyactors.wasm`. The package contains the wasm, shared JS and
the Node transports, with no files needed outside it. `import ... from 'tinyactors-web'` selects the Node or
browser backend; `tinyactors-web/browser` explicitly selects the browser.
For an unbundled server, import
`./node_modules/tinyactors-web/bindings/web/index.mjs` and serve the whole
package so its relative JS and wasm URLs resolve.
See [distribution](../../docs/distribution.md); publish the generated tarball,
not this development directory.

```html
<script type="module">
  import { createMachine, createActor, createSystem, createLink } from './bindings/web/index.mjs';

  const toggle = createMachine({ id: 'toggle', initial: 'off', states: { off: { on: { TOGGLE: 'on' } }, on: { on: { TOGGLE: 'off' } } } });
  const actor = createActor(toggle).start();
  actor.subscribe((s) => console.log(s.value));
  actor.send({ type: 'TOGGLE' });

  // talk to actors on a server
  const link = createLink(createSystem(), { url: `wss://${location.host}/link`, name: 'server' });
  link.subscribe((s) => console.log('link', s.value, s.context.rtt));
</script>
```

- `tinyactors.wasm` (~125 KB, ~52 KB gzipped) contains the runtime, the
  compiler (machines compile to SCXML in the page) and `src/glue.c`, the
  procedural interface. It needs no WASI imports: its only imports are the
  datamodel callbacks.
- It is built for wasm32-wasi, with wasi-libc compiled in. The host and the
  system need no C library (`libtinyactors`, docs/adr/0005); what still
  does is the compiler (`malloc`, `snprintf`/`vsnprintf`, `qsort`, string
  functions), `ta_runtime_malloc` (the host's allocator here), and the
  binding itself (`bindings/shared/binding.c` and `src/glue.c`: `malloc`,
  `calloc`, `realloc`, `snprintf`, string functions).
- The datamodel is plain JavaScript (`native.mjs`). Contexts, events and
  payloads are ordinary JS values behind integer handles, so nothing is
  serialized between JS and wasm on the hot path.
- `index.mjs` is for browsers (top-level `await`, `instantiateStreaming`).
  `node.mjs` is the same binding for Node.js: synchronous, and `serveLinks()`
  also accepts `{ server | port }`. Pick it with the `node` export condition.
- Slices of work are time-boxed (~4 ms) and continue on a `MessageChannel`
  turn, so a busy system never blocks the page.
- SCXML documents (`fromSCXML`), system images (`compileSystem`,
  `loadImage`, `system.boot`), snapshots (`system.snapshot()`,
  `createSystem({ restore })`) and deterministic time (`createSystem({ clock })`
  + `flush()`) work as in the [Node binding](../node#scxml-documents);
  snapshots and images are portable between the two.
- Hibernation works here too: `createSystem({ hibernation: { store } })`, with
  a store backed by memory, IndexedDB or anything else (sync or async). Blobs
  are portable between the web and Node bindings.

- Actors move between systems with `system.export(actor)` and
  `system.adopt(bytes)` — between processes, and between this binding and
  the Node one: delayed events carry their data as values.

## Traces

Every system carries a trace through every step, as W3C trace contexts:
an event a step sends, raises, delays or hands to another system over a
link continues the trace of the step that sent it. Give a `traceparent`
where an event comes from outside, and the step it starts is a child of
your span:

```js
createActor(machine, { system, traceparent: req.headers.traceparent });
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
fails, never per event; latencies come from spans (a collector's
spanmetrics).

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

## Call cost (wasm vs the Node addon)

Every datamodel call and result crosses between JS and wasm through the
N-API-subset adapter (`native.mjs`). `node tools/bench_js.mjs` runs the same
workloads through both bindings, each in its own process (deterministic
clock, synchronous flushes; best of three; ops/s). Orb, 4 vCPUs, Node 26:

| workload | node, before | web, before | node | web | web / node |
|---|---:|---:|---:|---:|---:|
| machine: `actor.send()` per event | 221 k | 58 k | 440 k | 303 k | 0.69 |
| machine: `sendMany`, 1000 per call | 830 k | 214 k | 908 k | 855 k | 0.94 |
| SCXML document: `actor.send()` (cond, assign, script) | 123 k | 33 k | 165 k | 162 k | 0.98 |
| spawn (`createActor().start()`) | 105 k | 44 k | 138 k | 126 k | 0.91 |
| hibernate (SCXML actors, one by one) | 142 k | 65 k | 147 k | 149 k | 1.01 |
| wake + event (`sendMany` to hibernated actors) | 53 k | 18 k | 55 k | 56 k | 1.02 |

"Before" is the adapter as first written (web at 0.26–0.46 of Node). What
made the difference, hottest first:

- views of wasm memory (`DataView`, `Uint8Array`) were made per access —
  every handle read and out-parameter write; now made again only after the
  memory grew;
- property names (C string literals) were decoded with `TextDecoder` on
  every `napi_*_named_property`; now once per address, internalized;
- `native.run` built 11 result arrays and set 14 properties per run, mostly
  empty (on wasm, ~40 crossings for every event sent); it now lists only
  what happened (this also sped up the Node addon);
- strings: short ASCII without `TextDecoder`, UTF-8 lengths without encoding;
  no rest/spread arrays per call, no byte-mirror syncs when nothing is
  mirrored, no frame copies per callback.

What remains is mostly the crossings themselves (`wasm-to-js`) and keyed
property stores into the objects C builds.

## Build & test

```sh
mise run web:build    # zig cc → bindings/web/tinyactors.wasm
mise run web:test     # the Node binding's test suite against the wasm backend + API identity checks
```
