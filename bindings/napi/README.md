# Tinyactors for Node.js

Two layers, one native statechart engine:

- `require('./bindings/napi')`: nested JavaScript machines, object state, actor
  handles, scheduling, streaming inspection, and asynchronous I/O leases.
- `require('./bindings/napi/lowlevel')`: every public `ta_*` function and named
  constant from [tinyactors.h](../../src/tinyactors.h), with JavaScript values in
  place of pointers and output parameters.

CommonJS, ESM named exports, and TypeScript declarations are included. There is
no XML parser, string evaluator, transport implementation, or runtime npm dependency.

## Build from this repository

```sh
npm --prefix bindings/napi run check
node bindings/napi/examples/basic.js
node bindings/napi/examples/realtime-scenario.js
node --expose-gc bindings/napi/examples/entities.js 200000
```

Requires Node 18+, a C99 compiler, a C++17 compiler, and Node development headers.
The zero-dependency build script finds headers beside Node or in common system
locations; otherwise set `NODE_INCLUDE_DIR` to the directory containing
`node_api.h`. `CC` and `CXX` select compilers. The addon targets
[Node-API version 8](https://nodejs.org/api/n-api.html#node-api-version-matrix).

Alternatively, run `node-gyp rebuild` inside this directory. The supplied
[binding.gyp](binding.gyp) uses the usual [node-gyp prerequisites](https://github.com/nodejs/node-gyp/blob/main/README.md).
The custom build script supports macOS/Linux; Windows must use node-gyp.
Both build paths were tested on macOS arm64 with Node 26.5.1. Other platforms and
Node versions are targets, not verified results. This is a private source-checkout
package: it depends on `../../src`, and does not ship prebuilt binaries or a
standalone publishable npm tarball.

## Define and run actors

```js
const { defineMachine, createSystem } = require("./bindings/napi");

const entity = defineMachine({
  id: "entity",
  state: ({ input }) => ({ x: input.x, velocity: 2, sprite: "hero" }),
  initial: "moving",
  states: {
    moving: {
      on: {
        TICK: ({ state, event }) => {
          state.x += state.velocity * event.data.dt;
        },
        PAUSE: "paused",
      },
    },
    paused: { on: { RESUME: "moving" } },
  },
});

const simulation = createSystem({ limits: { actors: 250000 } });
const actor = simulation.spawn(entity, { name: "player", input: { x: 10 } });
actor.send("TICK", { dt: 0.5 });
simulation.run({ nowNs: 0n, workUnits: 10000 });
console.log(actor.snapshot()); // state.x === 11; active === ['moving']
simulation.close();
```

The C actor stores a rooted handle to the actual JS state object, not serialized
JSON. Return a **fresh object** from `state` for each actor. With no factory, the
default is a cloned input or `{}`. Functions use `{ state, event, self, now, in,
send, raise, cancel, system }`; `self` is an incarnation BigInt and `now` is
nanoseconds. TypeScript can specify state, input, and event types through
`defineMachine<State, Input, Event>(...)`; see [test/types.ts](test/types.ts).

Statechart features include nested states, `parallel`, `final`, shallow/deep
`history`, `initial`, `on`, `always`, `onDone`, guards, entry/exit blocks, data
declarations, invokes, and final `doneData`. Transition arrays are ordered
choices; multiple targets use `{ target: ['left.ready', 'right.ready'] }`.
Nested keys receive path-qualified IDs. Sibling targets resolve locally;
`.child` addresses a descendant and `#explicit.id` an absolute ID. `onDone`
matches that state's exact completion. Normal event descriptors keep SCXML
prefix/wildcard matching. Put transitions and entry/exit actions on child states,
not the implicit document root. `root` is the one-time root script.

`send`, `raise`, `cancel`, and `assign` build ordinary action descriptors:

```js
const { send, cancel, assign } = require("./bindings/napi");
// Example state members:
const waiting = {
  entry: send("TIMEOUT", { delayMs: 1000, id: "timeout" }),
  exit: cancel("timeout"),
  on: { UPDATE: { actions: assign(({ event }) => ({ latest: event.data })) } },
};
```

Actions, guards, factories, snapshot policies, and native callbacks are
synchronous. Never return a Promise or retain execution helpers for later use.
The helpers expire on return; captured object references cannot be revoked, so
do not mutate live state from outside actor execution. Exceptions become SCXML
execution errors and are available through `system.takeErrors()`; operation
failures in the convenience API throw `TinyActorsError` with a numeric `status`.
Use sends/invokes for asynchronous work.

## Identity, revisions, and scheduling

`actor.send(...)` always addresses that exact incarnation. `system.send(name,
event, data)` resolves the current name binding. Rebind with
`system.bind(name, replacement, expectedActor)`. A released actor handle becomes
stale; it never silently addresses a replacement. Within actions, `send` accepts
BigInts, local Actor handles, names, SCXML addresses, or raw target descriptors.
Strings containing `:` or starting with `#` are interpreted as URI addresses;
use a raw `TA_TARGET_NAME` descriptor for such a registered name.

`system.publish(machine, { key, expectedRevision })` changes what future
`spawnPublished(key)` calls instantiate. Existing actors keep their revision.
`actor.upgrade(newMachine, { expectedRevision, migrate })` explicitly upgrades a
stable actor while retaining its identity. Migration receives a snapshot of old
state plus old/new descriptions and the state mapping. Return the new object;
keep migration free of external side effects, since it can fail validation.
State IDs must remain compatible with the C upgrade rules.

Prepared code tables are definition-local, so replacing definitions does not
require a permanent global callback registry. `releaseDefinition(machine)` drops
the convenience layer's caller reference; actors and publications retain their
own pins. Explicitly release old revisions when they are no longer needed.

Scheduling is manual by default. `run()` returns progress, `runnable`, `ioReady`,
and `nextDeadlineNs`; call again while work remains. Time must be monotonic.
For Node-managed wakeups, use `system.start({ workUnits: 1000, actorQuantum: 100 })`
and `system.stop()`. This uses bounded turns and deadline timers, not idle polling.
`onIO(system)` is a synchronous notification to arrange host-side draining;
`onError(error)` handles scheduler failures. No background thread is created.

## Streaming inspection and multiple systems

```js
let page;
do {
  page = simulation.inspect(
    (record) => {
      if (record.kind === "actor") consume(record.actor, record.info.state);
    },
    { workUnits: 1000, cursor: page?.cursor },
  );
} while (!page.done);
```

`consume` is your callback; the library does not collect the actors into an array.
Return `'pause'` to retain a cursor or `'stop'` to abandon the traversal. Optional
actor/revision/stable filters and detail flags select data, active states, history,
and current events. Cursors detect system mutation and then report `TA_STALE`;
restart inspection after mutation. Do not call back into the inspected system
from its callback. Another system may be used independently, for example to post
renderer updates from simulation snapshots. Actor handles are system-local;
transfer application data, not handles. `Actor.snapshot()` is a convenience
snapshot of one actor, including active state names by default.

Messages and outgoing inspection views use `structuredClone` by default. The
state itself stays a native JS object. Socket instances, closures, and similar
resources should be represented by IDs in state and held by the host. A custom
`snapshot(value)` policy supports application-specific native values/classes; it
must synchronously return an isolated value, or an immutable share-safe value.
It applies to admission, inspection, migration snapshots, and lease payloads.
Returning the same mutable object defeats isolation.

## I/O processors, WebSockets, and invoked services

Register processors as an array, with optional aliases and a location callback:

```js
const system = createSystem({
  ioprocessors: [{ type: "ws", aliases: ["websocket"] }],
  services: [{ type: "subscription", event_processor: "ws" }],
});
```

A statechart `send('UPDATE', { processor: 'ws', to: 'ws:connection-id', data })`
produces an I/O lease. The host takes it and performs the real operation outside
the native callback. For example, given a host-owned connection map:

```js
await system.drainIO(async (request, lease) => {
  const socket = connections.get(request.target);
  await socket.write(JSON.stringify(request.body.content.native));
});
```

`drainIO` completes each lease after the handler resolves, or reports
`TA_COMMUNICATION_ERROR` on rejection. It drains at most 64 requests by default;
set `limit` or schedule another drain for additional work. The application must
define transport backpressure and what successful completion means. The sample
above handles SEND requests only; a host with invoked services must dispatch on
`request.kind` as well. For direct control use `ioNext()` and
`lease.complete(status)`, exactly once. A failed completion can deliver
`error.communication` to the actor.

External invokes generate START/CANCEL leases. Accept and complete START before
calling `lease.emit(...)`, `lease.done(data)`, or `lease.fail()`. Keep the invocation
reference for the service lifetime, check cancellation, stop the actual service,
and complete CANCEL. A taken START can race cancellation; see the tested sequence
in [test/statecharts.test.js](test/statecharts.test.js). Inline SCXML children use
`invoke: { inlineDefinition: system.prepare(child), id: 'child' }`; no external
driver is required. Inbound processor events use
`system.receive({ processor: 'ws', actor }, { type, data, origin, originType })`.

[examples/realtime.js](examples/realtime.js) starts a real multi-client browser
chat: one directory actor, one actor per room, and one reconnecting actor per
client session. Room membership, presence and bounded history are native JS
actor state. Install its isolated WebSocket dependency, then run it:

```sh
npm --prefix bindings/napi/examples/chat ci
node bindings/napi/examples/realtime.js
```

Open `http://127.0.0.1:8080` in multiple tabs. The [chat README](examples/chat/README.md)
explains reconnects, limits, streaming inspection, tests and demo-only security
scope. The binding itself still has no runtime npm dependency.
[examples/realtime-scenario.js](examples/realtime-scenario.js) preserves the
original deterministic transport stub without opening a network server.

## Trace hooks and lifecycle

Supply `trace: { start, error?, end }` as an adapter to your OTel tracer/exporter.
`start(input)` receives operation/kind constants, actor/revision/message IDs,
event, processor, destination, monotonic time, parent, and links. Return
`{ handle: yourSpan, context }`, where context has 16-byte `trace_id`, 8-byte
`span_id`, flags, and optional tracestate/baggage. `error(handle, diagnostic)`
reports span errors; `end({ handle, context, now_ns, status })` closes the same
rooted span. Preserve both parents and links: process spans link to incoming
message context; sends parent to the active processing span. No OTel SDK or
exporter is hardwired, and message payloads are not automatically exported.
Telemetry exceptions are recorded without changing actor transitions.

Use `actor.stop(); system.run(); actor.release()` to reclaim individual actors.
`system.close()` stops actors and runs exit actions, then releases resources.
It refuses outstanding I/O/invocations with `TA_BUSY`: stop transports, complete
leases/cancellation, and retry. `close({ force: true })` explicitly abandons
pending work without user callbacks. GC/environment cleanup is a safety fallback,
not a transport shutdown protocol, and never calls user JavaScript. Native
handles are confined to their Node environment; Workers use independent systems.

## The low-level mapping

```js
const n = require("./bindings/napi/lowlevel");
function ok(result) {
  if (result.status !== n.TA_OK)
    throw new Error("tinyactors status " + result.status);
  return result;
}
const { system } = ok(n.ta_system_create(null, {}));
const { definition } = ok(
  n.ta_definition_build(system, {
    name: "idle",
    data_model: "javascript",
    states: [{ id: "idle" }],
  }),
);
const { actor } = ok(n.ta_actor_spawn(system, { definition, seed: { x: 10 } }));
ok(
  n.ta_system_run(system, {
    now_ns: 0n,
    budget: { work_units: 100, actor_quantum: 10 },
  }),
);
console.log(ok(n.ta_actor_inspect(system, actor)).info.state.native);
n.disposeSystem(system); // Explicit silent host cleanup, not graceful shutdown.
```

Fields keep C snake_case names. Arrays replace pointer/count pairs and sentinels;
do not append sentinel objects in JS. Action union fields are flattened into the
action object. Numeric enum/status values remain Numbers; uint64 IDs, revisions,
times, and code tokens are BigInts (safe integer inputs are also accepted).
`TA_UNLIMITED` is `Infinity`, while `TA_TIME_NEVER` is a BigInt. Explicit zero
limits remain zero, not defaults. Invalid types/handles and lossy integers throw
before entering the runtime; ordinary C failures return a status.

Functions with status/output parameters return `{ status, outputName }`, with
`null` output on failure. Status-only functions return a Number. Void queries
return their output directly. Opaque arena/system/definition/context/memory/cursor
handles are branded objects, not pointers. The binding provides the chunk
allocator; `ta_system_create(null, config)` also creates the parent arena.
`ta_system_config_init()` returns defaults for detailed tuning.

Native payloads are `{ kind: TA_PAYLOAD_NATIVE, native: value }`; bytes use
`{ kind: TA_PAYLOAD_BYTES, bytes: buffer }`. Low-level native inspection returns
live values: treat them as borrowed/read-only, or snapshot them yourself.
`memoryRead`/`memoryWrite` copy arena bytes. `ta_context_bytes` returns a copy;
`contextWriteBytes` commits an exact-sized replacement. No escaped Buffer points
into a freed arena. Runtime-owned arenas cannot be closed by the caller.

Definition description output includes retained `inline_definition` handles,
plus diagnostic `inline_revision`/`inline_description`. Release those handles or
allow GC to release them when finished. They make description round-trips retain
child behavior. COPY deep-copies input descriptors; BORROW pins the bridge's
marshalled storage until the final definition pin is released. Its
`storage_release` notification runs on explicit release, but not GC cleanup.

The complete signatures and callback shapes are in [lowlevel.d.ts](lowlevel.d.ts).
[CONTRACT.md](CONTRACT.md) records boundary conventions; the C header specifies
the underlying semantics. All authored C/C++/JS functions take at most three
parameters; recurring options are grouped into objects.

## Limits and validation

`limits`, `actorLimits`, and `config.arenas` tune the C runtime. The convenience
layer accepts camelCase limit keys. Native marshalling is separately bounded by
`config.marshal_limits`: defaults are 1,000,000 nodes, depth 64, and 64 MiB per
call. The JS authoring walk also caps depth at 64 and states at 1,000,000.
Captured callback errors are capped at 1,024 until drained. Core byte limits do
not include VM heap, JS objects, host allocator overhead, or bridge metadata;
use Node's process/heap limits and `process.memoryUsage()` as well as `stats()`.

The 200,000-entity JS-object example passed on the tested machine: approximately
1.68 s setup, 2.31 s for posting/processing one update per actor, 0.56 s streaming
inspection, and 629 MB RSS at the idle sample. This is a capacity check, **not** a
60-FPS game-loop result. The lower-memory C byte-state benchmark is separate;
the Node layer adds object roots, wrappers, snapshots, and JS/native crossings.

Tests cover both layers against the compiled addon, object isolation, stale
handles/contexts, GC and Worker teardown, history/parallel states, inline/external
invocation, upgrades, timers, streaming cursors, processor errors, and message
span parents/links. Run the following for instrumented C and C++ checks, then
rebuild normally:

```sh
TINYACTORS_SANITIZE=undefined npm --prefix bindings/napi run check
```

Strict
TypeScript checks use [test/types.ts](test/types.ts). The core C suite remains
available through `make check` and `make ubsan`. Task context, delegated work,
review iterations, and measurements are recorded in [tasks.sqlite3](../../tasks/tasks.sqlite3).

Development checks (Acorn and TypeScript are temporary tools, not runtime dependencies):

```sh
npm exec --yes --package=acorn@8.15.0 -- node bindings/napi/scripts/check-arity.js
npm exec --yes --package=typescript@5.9.3 -- tsc --noEmit --strict --target es2022 --module nodenext --moduleResolution nodenext bindings/napi/test/types.ts
```
