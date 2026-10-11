# tinyactors-browser, from the guide alone

**Status:** Proposed (2026-10-11). Part of [One host, two queues](README.md).

The third binding designed from [“Writing a binding”](bindings.md), after the Node addon and CPython. The browser shares the JavaScript package with Node and Bun, so what this sketch designs is the runtime under it: a wasm addon in the `tinyactors-browser` project, the counterpart of `tinyactors-napi`. It tests the guide where Node could not: memory that grows and never shrinks, a download size, a Content Security Policy, two kinds of thread, throttled timers, and storage that outlives the page.

No core changes: the host interface held a third time. The browser binding is two small C files, a loader and a build, under the JavaScript package Node already uses. Today’s web binding reads structs at wasm32 offsets in 3,558 lines of JavaScript; that disappears because the addon is C. The findings are about the runtime: views detached by growth, memory that never shrinks, the page’s CSP choosing between two modules, threads, late timers, clocks, and a store that must not pretend to be durable. One finding corrects the [Node sketch](napi.md): each runtime’s addon is its own C, with one function list.

## What the browser asked of the guide

#### Each runtime’s addon is its own C. *(guide)*

The Node sketch said the wasm addon is “the same addon.c”. It cannot be: the Node addon is written against Node-API (`napi_value`, `napi_env`), which a browser does not have.

**Fix:** Two thin C files, one function list: `tinyactors-napi/src/addon.c` over Node-API and `tinyactors-browser/src/addon.c` over wasm numbers and linear-memory pointers. `tinyactors-js` sees the same functions from both and runs one test suite against each. The Node sketch is corrected.

#### Memory growth detaches every view. *(guide)*

When the module’s memory grows, the old `ArrayBuffer` is detached: every `Uint8Array` over it becomes empty. The host grows during `ta_host_create`, spawns, wakes, adopts and requests.

**Fix:** The loader owns the views and refreshes them when `memory.buffer` has changed, after every call that can allocate. Nothing else in `tinyactors-js` keeps a view of wasm memory.

#### Linear memory never shrinks. *(guide)*

Hibernation frees an actor’s memory inside the host’s allocator, but a wasm memory cannot give pages back.

**Fix:** Hibernation makes room for other actors; it does not lower the page’s footprint. The guide says to size for the peak in runtimes whose memory only grows (wasm), and the README gives the number.

#### The page’s Content Security Policy chooses the datamodel. *(guide)*

The JavaScript datamodel compiles chart expressions with `new Function`, which needs `'unsafe-eval'`. A page with a strict policy refuses it. Instantiating wasm needs only `'wasm-unsafe-eval'`.

**Fix:** Two modules: `tinyactors.wasm` for the JavaScript datamodel (smaller, expressions run in the page’s engine) and `tinyactors-quickjs.wasm` for a strict policy (expressions run in quickjs, with its `max_ops` and memory limits). `createHost({ datamodel: "quickjs" })` picks the second. The guide names the policy as a reason to choose.

#### The download is the module. *(build)*

Today’s module links all three libraries: 1.55 MB, 538 KB with gzip -9. In the object files the quickjs datamodel is about three times the size of core.

**Fix:** The module without quickjs is what most pages load. Both sizes go in the README, measured when the binding is generated, and the release checks them.

#### A browser has two kinds of thread. *(guide)*

A host can run on the page’s main thread or in a worker. On the main thread a long run blocks input and rendering.

**Fix:** The binding uses no DOM, so it runs in both. One host per thread, as the guide says. The run budget keeps a turn short; a yielded run continues in a new task (a `MessageChannel` message, not `setTimeout(0)`, which browsers clamp to 4 ms after five nested calls). A host in a worker exchanges envelopes with others through `postMessage`, transferring the buffer. No `SharedArrayBuffer`, so no cross-origin isolation headers.

#### Threads inside the module would be a core decision, not a binding option. *(guide)*

Leaving `SharedArrayBuffer` out could look like a corner: what if pages need parallel actors later? Two things decide it. Core’s model is shared-nothing: a host is single-threaded, and parallelism is several hosts exchanging envelopes, in every runtime (Node workers, free-threaded Python). And shared memory would buy little here: the JavaScript datamodel’s values live in one thread’s JavaScript heap, and quickjs is not thread-safe, so values between threads are converted whatever the memory. Meanwhile `SharedArrayBuffer` needs cross-origin isolation (COOP and COEP headers), which breaks third-party embeds, iframes and some CDNs: requiring it would be the real corner.

**Fix:** Parallelism is one host per worker, each with its own instance, exchanging CBOR envelopes by `postMessage` with the buffer transferred, not copied. Three things keep the door open: (1) every access to wasm memory stays in `wasm.mjs`, so a shared-memory build would change the loader only (growth no longer detaches, and `TextDecoder` needs a copy out of shared memory); (2) nothing in `tinyactors-js` assumes a thread or the DOM; (3) the binding’s tests measure an envelope round trip between two workers, so the cost of shared-nothing is known before anyone asks for more. A multi-threaded host (segments run in parallel on shared memory) would come first as a core ADR, with its own roles and costs; the binding would follow it, not lead it.

#### Hidden tabs throttle timers. *(guide)*

Browsers delay timers in background tabs (to once a second, and to once a minute after a few minutes in Chrome). The host’s one timer fires late.

**Fix:** Delayed sends fire late but in order: the next turn advances the clock to now and runs everything that is due. That is I/O, so the binding documents it and does nothing else. A frozen page (back/forward cache) resumes the same way.

#### Two clocks, both from `performance`. *(guide)*

`Date.now()` has milliseconds only and can jump; traces need wall nanoseconds.

**Fix:** Monotonic: `performance.now()`. Wall: `performance.timeOrigin + performance.now()`, in nanoseconds as a `BigInt`. The quickjs module’s `Date` reads the wall time the addon sets in `advanceTime` (today’s `ta_datamodel_ecmascript_wasm_set_time`), not a separate call from JavaScript.

#### A hibernation store that outlives the page. *(guide)*

IndexedDB keeps data across reloads. A blob left there by a previous page load belongs to a host that no longer exists: no host can wake it.

**Fix:** The store holds idle actors out of memory for this host only. The default store is a `Map`; an IndexedDB store keys blobs by a random id made per host and clears that id’s entries when the host is created and when it stops. Never write a snapshot there as a save file: snapshots are not durability, and neither is the store.

#### Session ids are 64-bit; wasm passes them as `BigInt`. *(api)*

A session id is `k·2³²+n`. An exported wasm function with an `i64` parameter takes a `BigInt`, which every caller would have to make.

**Fix:** The addon takes and returns session ids as `f64`, as the Node addon does: exact while the segment index stays below 2²¹. The JavaScript API keeps sessions as numbers in both runtimes.

#### Transports, posting and storage are handlers. *(api)*

There is no link protocol to ship. A browser has WebSocket, `BroadcastChannel`, `postMessage`, `fetch` and IndexedDB, all asynchronous and all I/O.

**Fix:** `on.peer(peer, bytes)` sends an envelope however the application likes (a WebSocket message is already framed); `host.deliver(peer, bytes)` takes one in. `on.traces(bytes)` posts with `fetch` and `keepalive`; the collector must allow the origin (CORS). The package ships none of them; the README shows each in a few lines.

## A wasm addon under the shared package

`tinyactors-js` is the binding for every JavaScript runtime; its `"browser"` export loads `wasm.mjs` where Node loads the Node-API addon. `tinyactors-browser`, a project at the repository root beside `tinyactors-napi`, holds the loader, the addon’s C and the build of both modules, and pins zig through the root `mise.toml`.

    Your page or worker: createHost from "tinyactors-js"; handlers use IndexedDB,
    WebSocket, postMessage, fetch
                                       |
    +----------------------------------v------------------------------------------------+
    | tinyactors-js: the same package as for Node and Bun (its "browser" export)         |
    |   host.mjs · loop.mjs · effects.mjs   cbor.mjs · request.mjs   datamodel.mjs ·     |
    |   compile.mjs                                                                      |
    +----------------------------------+-------------------------------------------------+
                                       | the addon functions
    +----------------------------------v-------------------------------------------------+
    | tinyactors-browser: the wasm addon                                                 |
    |   wasm.mjs: loaded once per thread,        src/addon.c · src/datamodel.c:          |
    |   views refreshed after growth,            the addon functions over wasm types;    |
    |   strings and bytes in and out             vtable entries call imports             |
    |   tinyactors.wasm: addon + core, for       tinyactors-quickjs.wasm: addon + core + |
    |   the JavaScript datamodel                 quickjs, for a strict CSP               |
    +------------------------------------------------------------------------------------+
    | core: ta_host, ta_system, ta_otlp, src/base/cbor; quickjs in the second module     |
    +------------------------------------------------------------------------------------+

*The JavaScript above the line is the same in Node, Bun and the browser; only the addon under it differs.*

## Structs and signatures

### The wasm addon · `tinyactors-browser/src/addon.h`

```
/* tinyactors-browser/src/addon.h — the wasm addon: the addon function list over wasm types */
typedef struct ta_browser_host {      /* one per ta_host: a JavaScript Host */
    ta_host *host;
    ta_browser_datamodel *datamodel;  /* the JavaScript datamodel (tinyactors.wasm), or NULL */
    ta_datamodel_ecmascript *quickjs; /* the quickjs datamodel (tinyactors-quickjs.wasm), or NULL */
    ta_buffer scratch;                /* strings and CBOR in and out, grown on TA_ERROR_NO_MEMORY */
    uint32_t freeing;                 /* stop() from a handler: free after the turn */
} ta_browser_host;

typedef struct ta_browser_datamodel { /* src/datamodel.c: each vtable entry calls an import */
    uint32_t id;                      /* which JavaScript datamodel the imports dispatch to */
    ta_datamodel_binding binding;     /* { &ta_browser_vtable, this } */
} ta_browser_datamodel;

/* The imports, one per vtable entry the JavaScript datamodel implements:
 *   __attribute__((import_module("tinyactors"), import_name("eval_condition")))
 *   ta_status ta_browser_eval_condition(uint32_t datamodel, double instance,
 *                                       uint32_t environment, uint32_t expression);
 * No JavaScript function is placed in the module's table, and no layout of a
 * struct exists outside the headers: the addon reads effects with
 * ta_host_effect_number/_text and builds input structs itself. Memory comes
 * from the allocator the module links (wasi-libc's malloc), passed to the host
 * as config.allocator. */
```

### The loader · `tinyactors-browser/wasm.mjs`

```
// what wasm.mjs gives tinyactors-js: the addon function list, as tinyactors-napi gives it.
// Each wraps one export of src/addon.c; "ptr, len" pairs are scratch memory in the module.
init(module?: URL | Response | WebAssembly.Module, kind = "native" | "quickjs")
                                   // compileStreaming once per page or worker; one instance
                                   // holds every host of that thread (up to 4 GiB, wasm32)
hostCreate(config: Uint8Array /* CBOR */, datamodel: object | null): Host      // ta_host_create
hostFree(h): void                                                                // ta_host_free
advanceTime(h, monotonicMs: number, wallNs: bigint): void        // ta_host_advance_time (+ quickjs Date)
run(h, budget: number): [status, deadline]                                       // ta_host_run
effects(h): number                                                               // ta_host_effects
effectNumber(h, i, field): number      effectText(h, i, field): string   // ta_host_effect_number/_text
effectsAck(h): void                                                              // ta_host_effects_ack

payload(h, value: Uint8Array): number            // from_wire (quickjs); JavaScript payloads stay in JS
spawn(h, segment, chart, name: string, payload): number        // the addon fills a ta_spawn
send(h, session: number, name: string, payload): number                          // ta_host_send
sendMany(h, sessions: Float64Array, names: string[], payloads: Uint32Array): number
terminate(h, session): number          deliver(h, peer, envelope: Uint8Array): number

hibernate(h, session): Uint8Array      wake(h, session, blob: Uint8Array): number
request(h, request: Uint8Array): Uint8Array      // grown and called again on TA_ERROR_NO_MEMORY
traces(h): Uint8Array      metrics(h): Uint8Array      envelope(h, effect): Uint8Array
cborDecode(bytes: Uint8Array): Uint32Array       cborEncode(tokens, strings): Uint8Array
environmentIn(environment: number, state: string): boolean      // In(), for the JavaScript datamodel

// Inside each: put strings with TextEncoder.encodeInto into scratch, call the export,
// refresh the views if memory.buffer changed, copy results out (TextDecoder, slice).
```

### In a page

```
// a page: the same API as in Node (see the [Node sketch](napi.md)); only the handlers differ
import { createHost, compile } from "tinyactors-js";       // the "browser" export: wasm.mjs

const host = await createHost({
  systemID: "tab-" + crypto.randomUUID(),
  datamodel: "native",                             // "quickjs" under a strict CSP
  ioProcessors: {                                  // JavaScript functions, as in Node
    "http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor": {
      send: (effect) => fetch(effect.target, { method: "POST", body: effect.data }),
    },
  },
  invokers: {
    "https://example.com/camera": { start: (invocation) => camera.open(invocation),
                                    stop:  (invocation) => camera.close(invocation) },
  },
  store: idbStore("tinyactors"),                   // idle actors out of memory, cleared per host
  on: {
    peer: (peer, bytes) => sockets.get(peer).send(bytes),        // a WebSocket message
    traces: (bytes) => fetch(collector, { method: "POST", body: bytes, keepalive: true,
                                          headers: { "content-type": "application/x-protobuf" } }),
    done: (session, output) => render(session, output),
  },
});
host.addImage(await compile({ "door.scxml": source }));
const door = host.spawn({ chart: "door" });
host.send(door, "OPEN", { by: "ops" });
socket.onmessage = (m) => host.deliver(peer, new Uint8Array(m.data));
```

### Roles and costs

| role                                 | what it means for them                                                                                                                                                                                                                                           |
|--------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| application developer (served first) | statecharts in a page or a worker with the same API as in Node, the page’s own JavaScript values in the JavaScript datamodel, and a choice of module by CSP and size                                                                                             |
| operator                             | traces posted with `fetch` to a collector that allows the origin; metrics read when the page wants (on `pagehide`, say). Late timers in hidden tabs are expected.                                                                                                |
| capacity planner                     | one linear memory per page or worker that only grows: hosts and their actors in it, JavaScript values outside it (JavaScript datamodel) or inside it (quickjs). Hibernation makes room, it does not shrink.                                                      |
| binding author                       | `src/addon.c` and `src/datamodel.c` (the function list again, over wasm types), `wasm.mjs`, and the build of two modules with the pinned zig. Everything else is `tinyactors-js`, shared with Node.                                                              |
| maintainers, C embedder              | no core change. Today’s 3,558-line `native.mjs`, which reads structs at wasm32 offsets, and its generated layouts (`api.mjs`) have no successor.                                                                                                                 |
| out of scope                         | a service worker as host (it is killed when idle), requiring cross-origin isolation (threads inside the module wait for a core decision on a multi-threaded host; see the findings), persisting actors in the browser, a WebSocket or HTTP client in the package |

## Five paths through the binding

### createHost(options)

```
createHost(options)                                  host.mjs
├─ await wasm.init(kind)                              once per page or worker
│    └─ WebAssembly.compileStreaming(fetch(new URL("tinyactors.wasm", import.meta.url)))
│       instantiate with imports { tinyactors: datamodel entries → datamodel.mjs }
├─ handlers = { ioProcessors, invokers, store, on }    JavaScript functions, kept in host.mjs
├─ config = cbor.encode({ systemID, capacities,
│                         ioProcessors: Object.keys(ioProcessors) })   only their type URIs:
│                                                     _ioprocessors and routing need them
└─ h = wasm.hostCreate(config, datamodel)             ta_host_create
     └─ memory grew: views refreshed                  (the loader, not the caller)
```

### a turn on the main thread

```
loop.turn()                                           loop.mjs
├─ wasm.advanceTime(h, performance.now(), wallNs())  monotonic, and wall in ns
├─ repeat
│  ├─ [status, deadline] = wasm.run(h, budget)        ta_host_run, budget sized for a frame
│  ├─ effects.handle(i) for i < wasm.effects(h)       fields by accessor
│  │    SEND to type T  → ioProcessors[T].send(effect)        your function
│  │    INVOKE of type T → invokers[T].start(invocation)  ·  UNINVOKE → .stop(invocation)
│  └─ wasm.effectsAck(h)                              always, then raise the first error
├─ YIELDED → channel.port2.postMessage(0)              a new task: input and rendering run first
└─ IDLE    → clearTimeout(timer); timer = setTimeout(turn, deadline - now)
             (a hidden tab fires it late: the next turn runs everything due, in order)
```

### an expression in the JavaScript datamodel

```
ta_host_run                                           core, inside tinyactors.wasm
└─ vtable eval_condition                               src/datamodel.c
   └─ ta_browser_eval_condition(id, instance, environment, expression)   a wasm import
      └─ datamodel.mjs evalCondition(...)              JavaScript
         ├─ chart.functions[expression](scope)          compiled once with new Function
         │    In("s") → wasm.environmentIn(environment, "s")
         └─ throws → TA_ERROR_EXECUTION                 core raises error.execution
```

### hibernate to IndexedDB, then a reload

```
host.hibernate(session)                               host.mjs
├─ blob = wasm.hibernate(h, session)                   ta_host_hibernate, copied out of memory
├─ held.set(session, blob)
└─ store.put(hostKey, session, blob).then(() => held.delete(session))
… the page reloads: this host is gone
createHost(...)                                        a new host, a new hostKey
└─ store.clear(previous hostKey)                       its blobs can never be woken: garbage
```

### a host in a worker, a page as its peer

```
worker:  on.peer(peer, bytes) → self.postMessage({ peer, bytes }, [bytes.buffer])
page:    worker.onmessage = ({ data }) → pageHost.deliver(workerPeer, data.bytes)
                                                       ta_host_deliver: the envelope, CBOR
page:    on.peer(workerPeer, bytes) → worker.postMessage({ peer, bytes }, [bytes.buffer])
         (transferred, not copied; the router keeps routes, dedup and error envelopes)
```
