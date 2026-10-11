# tinyactors-napi: the Node addon, from the guide alone

**Status:** Proposed (2026-10-11). Part of [One host, two queues](README.md).

To test [“Writing a binding”](bindings.md), the Node addon was designed using nothing but the guide and the proposal: every effect, input and request of the host, and every scenario of today’s Node binding, every scenario its tests cover. Where the guide had no answer, or the answer needed something core does not have, that is a finding. The sketch below is the result after the fixes.

## Where the guide broke

Ordered by what they touch: core first, because those change the proposal; then the guide; then documentation and the API.

#### The three-parameter rule rewrites `host.h`. *(core)*

27 host functions take more than three parameters, the hot path included (`ta_host_spawn` 7, `ta_host_send_many` 4, `ta_host_deliver` 4, `ta_host_wake` 4, `ta_host_hibernate` 5, `ta_host_migrate` 6, `ta_host_restore` 6, `ta_host_snapshot_pack` 7), and so do 13 in `system.h` and most of the 27 datamodel vtable entries.

**Fix:** Group them in structs (`ta_spawn`, `ta_buffer`, …) as part of the Host step; the guide names the rule.

#### Subscribers cannot learn what changed. *(core)*

Today the bindings hook the datamodel to see which sessions changed. With the quickjs datamodel a binding implements nothing, so “tell me when this session changes” has no core function behind it.

**Fix:** A `subscribe`/`unsubscribe` request, and a `CHANGED` effect at the end of a macrostep for subscribed sessions only: no cost for the rest.

#### Sends to `#_<invokeid>` are resolved in the binding. *(core)*

Today `invoke.c` matches invoke ids by string to find the service a send is for. The guide says such sends “go to” the service without saying who resolves them.

**Fix:** The host puts the invocation’s token on every send to an invoked service; the binding keys services by `(segment, token)` only.

#### The host has no wall clock for traces. *(core)*

Owning the encoder, the host must stamp spans in wall time (`ta_otlp_mark` today, called by the binding), but `ta_host_advance_time` takes only monotonic ticks.

**Fix:** `ta_host_advance_time(host, ta_host_time)` with `{ monotonic, wall_ns }`.

#### Metrics are pulled, not pushed. *(core)*

Today the binding runs a 60-second interval of its own to export metrics: a second timer and a default in the binding. But the counters already live in core (per chart, per segment, host-wide) and change as the host runs; nothing needs a clock to keep them.

**Fix:** `ta_host_metrics(host, ta_buffer *)` writes the current counters as an OTLP metrics request whenever it is called. Core has no interval, no effect and no deadline for them. When to read (a scrape, a `stop()`, an application’s own cadence) is the caller’s: I/O, not semantics.

#### Creating a host mixes data and pointers. *(core)*

`ta_host_config` holds capacities, the system id and I/O processors (data) next to the allocator and datamodel bindings (pointers). Only the data can be CBOR.

**Fix:** `ta_host_create(ta_bytes config, const ta_host_environment *environment, ta_host **out)`: config as CBOR, the environment as a struct of pointers.

#### No catalogue of requests. *(docs)*

The guide says rarely-called calls are requests but nothing lists the operations, their fields, defaults and errors. An author cannot write `configureTraces` from it.

**Fix:** `tinyactors-core/docs/requests.md`: every op, its map, defaults, replies and error paths, generated alongside the vectors.

#### Reads during a run are unspecified. *(guide)*

An expression asking for another session’s state value or context: the guide allows sends during a run but says nothing of reads.

**Fix:** Requests, inspection included, are refused during a run (`TA_ERROR_STATE`); the API documents it.

#### Context after an actor ended has no core function. *(api)*

Today the bindings keep an ended actor’s last variables (“graves”) to put in `done` and `error`. Nothing in core provides them, so the mapping table rejects the item.

**Fix:** Drop it from the API: `done` carries `<donedata>`, which is what SCXML defines. Live context stays: the binding’s scope (native) or a `context` request (quickjs).

#### Threads are not mentioned. *(guide)*

Node worker threads, Java, Swift and Android all run code on several threads.

**Fix:** A host is single-threaded: every call from the thread that made it. A binding for a threaded runtime confines it (one host per worker, an executor, a serial queue).

#### Garbage collection is not mentioned. *(guide)*

The datamodel holds language references (scopes, payloads, functions); a finalizer that frees the host late keeps them alive, and callbacks can close over the owner.

**Fix:** Free deterministically on `stop()`, dropping every language reference the datamodel holds; a finalizer only as a fallback.

#### Exceptions in expressions are not mentioned. *(guide)*

A `c.expr` function throws. Today the bindings record the exception and report an `exception` effect of their own.

**Fix:** The native datamodel returns an error status, so core raises `error.execution` as SCXML requires; the binding passes the exception to the logger, and invents no effect.

#### Bindings must not decode host structs. *(core)*

The first sketch read effect records from host memory through published layouts, which is today’s `api.mjs` mistake again: the struct layout duplicated in every language that cannot include a header. Bindings have C; C should touch the structs.

**Fix:** Core provides two accessors, `ta_host_effect_number(host, index, field)` and `ta_host_effect_text(host, index, field)`, with a `TA_EFFECT_FIELD_*` enum: no layout anywhere but the header, and new fields need no binding change. Inputs that take structs (spawn, send_many) are built by the addon, which is C in every runtime: Node-API, the same C compiled into the wasm module, cffi. No published layouts, no `api.mjs`.

#### A binding is per language, not per runtime. *(guide)*

Designing the Node addon from the guide gives a loop, an effect dispatcher, a CBOR module and a datamodel that are pure JavaScript, the same ones the web binding needs.

**Fix:** One JavaScript package for Node, Bun and browsers over an addon per runtime (Node-API or wasm exports). No twin, by construction.

#### Process liveness is runtime behaviour. *(guide)*

Should a pending timer keep Node alive? The guide forbids policy in bindings.

**Fix:** Allowed and documented per runtime: liveness is I/O, not semantics. Node keeps the process alive while a deadline is pending.

## One package, two addons

The strongest finding changes the layout. Following the guide, everything above the FFI is plain JavaScript: the loop, the effect handlers, the CBOR module, the datamodel, the API. The Node addon and the web binding need the same code, so it is one package, and each runtime adds only an addon that exposes the same host calls.

    Your JavaScript: createHost · addImage · spawn · send · request · hibernate · migrate · stop
                                       |
    +----------------------------------v------------------------------------------------+
    | tinyactors-js: one package for Node, Bun and browsers                              |
    |   host.mjs (createHost, Host)  loop.mjs (turns, one timer)  effects.mjs (handlers) |
    |   cbor.mjs (tokens <-> values) datamodel.mjs (scopes, payloads)  compile.mjs       |
    +-------------------+-------------------------------------------+--------------------+
                        | the addon functions                       | the same list
    +-------------------v--------------------+   +------------------v---------------------+
    | tinyactors-napi: the Node-API addon    |   | tinyactors-browser: the wasm addon     |
    | src/addon.c · src/datamodel.c          |   | its own C, the same function list      |
    | (Node, Bun)                            |   | (see browser.md)                       |
    +-------------------+--------------------+   +------------------+---------------------+
                        | C calls                                   | wasm exports
    +-------------------v-------------------------------------------v---------------------+
    | core: ta_host, ta_system, ta_otlp, src/base/cbor, the quickjs datamodel             |
    +-------------------------------------------------------------------------------------+

*The addon is the only per-runtime code. A Python or Swift binding would be its own package over the C library, with the same structure.*

## Structs and signatures

### Node-API addon · `tinyactors-napi/src/addon.h`

```
/* tinyactors-napi/src/addon.h — the Node-API addon: ta_host as napi functions, nothing else */
typedef struct ta_napi_host {      /* one per ta_host: a JS Host */
    ta_host *host;
    napi_env env;                      /* the thread that made it: hosts are single-threaded */
    ta_napi_datamodel *datamodel;           /* the JavaScript datamodel, or NULL (quickjs) */
    ta_datamodel_ecmascript *quickjs;  /* the quickjs datamodel, or NULL */
    uint32_t freeing;                  /* stop() from an effect handler: free after the turn */
} ta_napi_host;

typedef struct ta_napi_datamodel {          /* src/datamodel.c: vtable entries call these */
    napi_ref object;                   /* datamodel.mjs's instance */
    napi_ref methods[TA_DATAMODEL_ENTRIES];
    ta_datamodel_binding binding;      /* { &ta_napi_vtable, this } */
} ta_napi_datamodel;

/* No view of host memory and no layout outside the headers: the addon reads
 * effects with ta_host_effect_number/_text and builds input structs itself. */
```

### The addon’s functions, as JavaScript sees them

```
// JavaScript signatures of tinyactors-napi/src/addon.c's exports (tinyactors-browser's wasm addon,
// its own C, exports the same list)
hostCreate(config: Uint8Array /* CBOR */, datamodel: object | null): Host      // ta_host_create
hostFree(h: Host): void                                                          // ta_host_free
advanceTime(h: Host, monotonicMs: number, wallNs: bigint): void                  // ta_host_advance_time
run(h: Host, budget: number, out: Float64Array /* status, deadline */): void     // ta_host_run
effects(h: Host): number                                    // ta_host_effects: how many
effectNumber(h: Host, i: number, field: number): number | bigint  // ta_host_effect_number
effectText(h: Host, i: number, field: number): string            // ta_host_effect_text
effectsAck(h: Host): void                                                        // ta_host_effects_ack

payload(h: Host, value: Uint8Array /* CBOR */): number   // from_wire (quickjs); native payloads live in JS
spawn(h: Host, segment: number, chart: number, name: string, payload: number): number
                                                     // the addon fills a ta_spawn: ta_host_spawn
send(h: Host, session: number, name: string, payload: number): number           // ta_host_send
sendMany(h: Host, sessions: Float64Array, names: string[], payloads: Uint32Array): number
                                                     // the addon fills ta_event_in[]: ta_host_send_many
terminate(h: Host, session: number): number                                      // ta_host_terminate
deliver(h: Host, peer: number, envelope: Uint8Array /* CBOR */): number          // ta_host_deliver

hibernate(h: Host, session: number, out: Uint8Array): number   // length, or -(needed)
wake(h: Host, session: number, blob: Uint8Array): number                         // ta_host_wake
request(h: Host, request: Uint8Array, reply: Uint8Array): number  // length, or -(needed)
traces(h: Host, out: Uint8Array): number                                         // ta_host_traces
metrics(h: Host, out: Uint8Array): number            // ta_host_metrics: the counters now, on demand
envelope(h: Host, effect: number, out: Uint8Array): number                       // ta_host_envelope

cborDecode(bytes: Uint8Array, tokens: Uint32Array /* 4 per token */): number     // ta_cbor_decode
cborEncode(tokens: Uint32Array, strings: Uint8Array, out: Uint8Array): number    // ta_cbor_encode
environmentIn(environment: number, state: string): boolean  // In(), for the JS datamodel
```

### The JavaScript package · `tinyactors-js/`

```
// tinyactors-js: the binding, shared by Node, Bun and browsers.
// The API is the host's: one Host per ta_host (which already runs many systems as
// segments); sessions are the host's ids, not objects. Each method: core behind it.

host.mjs       createHost(options) → Host                              ta_host_create
                 options: { systemID, capacities, datamodel: 'quickjs' | 'native',
                            ioProcessors: { [type]: { send(effect) } },
                            invokers: { [type]: { start(invocation), stop(invocation) } },
                            store: { get, put, delete },   (idle actors out of memory, not durability)
                            on: { done, error, log, changed, peer, traces } }
               Host.addImage(image) → segment                          ta_host_add_image
               Host.boot(segment)                                      ta_host_boot
               Host.spawn({ chart, name, input, traceparent }) → session
                                                                       ta_host_chart, ta_host_spawn
               Host.send(session, name, data, traceparent)             ta_host_send
               Host.sendMany(sessions, names, datas)                   ta_host_send_many
               Host.terminate(session)                                 ta_host_terminate
               Host.deliver(peer, envelope) → outcome                  ta_host_deliver
               Host.sweep(max) → sessions                              ta_host_sweep
               Host.hibernate(session) / wake(session, blob)           ta_host_hibernate / _wake
               Host.exportActor(session) → blob / adopt(blob) → session
                                                                       ta_host_export_actor / _adopt
               Host.migrate({ chart, session, wake, timeout }) → Promise
                                                                       ta_host_migrate, JOB_DONE
               Host.snapshot() → bytes · restoreHost(bytes, images, options)
                                                                       ta_host_snapshot_pack / _restore
                 (inspection, or the host out of memory for a while; same build only:
                  not a backup, a save file or crash recovery)
               Host.request(op, fields) → reply                       ta_host_request
                 ('state', 'context', 'stats', 'trace.configure', 'trace.watch',
                  'subscribe', … : docs/requests.md)
               Host.metrics() → OTLP bytes                             ta_host_metrics
               Host.stop()                                             ta_host_free
compile.mjs    compile(sources) → image · openImage(bytes) → image     ta_compile_*, ta_image_*
loop.mjs       wake() · turn() · arm(deadline)                         (one timer; section 3)
effects.mjs    handle(index)                 (one case per effect, fields by accessor; section 4)
cbor.mjs       decode(bytes) → value · encode(value) → bytes           (section 9)
datamodel.mjs  class Datamodel { attach · create · destroy · evalCondition · evalString · assign
               · script · log · foreach* · capture · retain · release · toWire · fromWire · … }
request.mjs    request(h, op, fields) → reply | throws TinyactorsError(path, message)

// Not here, because no core function is behind them: Actor objects and their
// snapshots, subscribe() callbacks per actor, chart builders, an "ActorRef".
// An application builds them on Host if it wants them.
```

## Five paths through the binding

### host.send(session, name, data)

```
Host.send(session, name, data)                      host.mjs
├─ payload = native ? datamodel.payloads.add(data)   datamodel.mjs (a JS handle)
│          : addon.payload(h, cbor.encode(data))     → from_wire (quickjs)
├─ addon.send(h, session, name, payload)             → ta_host_send
│    └─ inside a run (from an expression): appended to the mailbox
└─ loop.wake()                                      a turn soon, unless one is running
```

### a turn

```
loop.turn()                                         loop.mjs
├─ addon.advanceTime(h, performance.now(), wallNs())  → ta_host_advance_time
└─ repeat
   ├─ addon.run(h, budget, out)                      → ta_host_run
   │    └─ kernel → ta_napi_vtable.eval_* → datamodel.evalCondition(...) → c.expr
   │                                     └─ host.send(...) → addon.send (appended)
   ├─ n = addon.effects(h)                           → ta_host_effects
   ├─ for i < n: effects.handle(i)                  fields via addon.effectNumber / effectText
   │    ├─ SEND       → options.ioProcessors[type].send(…)  (sync: answered before the next run)
   │    ├─ INVOKE     → options.invokers[type].start({ segment, token, … })
   │    ├─ UNINVOKE   → options.invokers[type].stop(…)
   │    ├─ DONE/ERROR → options.on.done(session, output) / on.error(session, code)
   │    ├─ CHANGED    → options.on.changed(session)  (subscribed sessions only)
   │    ├─ LOG        → options.on.log(session, label, text)
   │    ├─ WAKE       → options.store.get(session).then(blob → addon.wake → loop.wake)
   │    ├─ PEER       → addon.envelope(h, i, buffer) → options.on.peer(peer, envelope)
   │    ├─ TRACES     → addon.traces(h, buffer) → options.on.traces(bytes)
   │    ├─ JOB_DONE   → migrations.settle(job, …)
   │    └─ FREED      → images.release(segment)
   ├─ addon.effectsAck(h)                            → ta_host_effects_ack
   └─ status: EFFECTS_FULL, EFFECTS → again · YIELDED → wake · IDLE → arm(deadline)
                                     EXHAUSTED, FAULT → report
```

### host.request("trace.configure", options)

```
Host.request('trace.configure', options)            host.mjs
└─ request(h, 'trace.configure', options)           request.mjs
   ├─ bytes = cbor.encode({ op, ...options })     → addon.cborEncode
   ├─ n = addon.request(h, bytes, reply)             → ta_host_request
   │    └─ n < 0: grow reply to -n, call again       (nothing was applied)
   ├─ value = cbor.decode(reply[0..n])            → addon.cborDecode
   └─ value.error ? throw TinyactorsError(value.error.path, value.error.message) : value
```

### host.hibernate(session), and a wake before the put settled

```
Host.hibernate(session)                             host.mjs
├─ n = addon.hibernate(h, session, buffer)           → ta_host_hibernate (grow on -n)
├─ held.set(session, blob)
└─ options.store.put(session, blob).then(() => {
     held.delete(session)
     if (woken.has(session)) options.store.delete(session)   (the store's copy is stale)
   })
… a WAKE effect arrives before the put settles:
effects.handle(WAKE)
├─ held.has(session) → addon.wake(h, session, held.get(session)); woken.add(session)
└─ else              → options.store.get(session).then(blob → addon.wake …)
```

### host.stop()

```
Host.stop()                                         host.mjs
├─ inside a turn → freeing = true; the turn frees after the ack
├─ every running invocation: options.invokers[type].stop(…) · timer cleared
├─ addon.traces → options.on.traces(final bytes)
└─ addon.hostFree(h)                                   → ta_host_free
     └─ datamodel.releaseAll()                        every scope, payload, function
```
