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
