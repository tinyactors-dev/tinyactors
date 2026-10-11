# ADR 0008: Bindings do I/O; what is free of any language moves to core

**Status:** Accepted (2026-10-11).
**Date:** 2026-10-11

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the binding author and the maintainers.** Today the Node.js
  binding is a 5,092-line C file plus a 2,432-line `core.mjs`, and the web
  binding adds a 2,802-line JavaScript reimplementation of the C file
  (`native.mjs`) and a copy of `core.mjs`. The logic in the JavaScript
  files includes the scheduler, the snapshot container, the trace policy,
  watches, migration jobs, the link protocol, the exporter's batching and
  the run-duration histogram. None of it needs JavaScript, and the next
  binding would have to write all of it again.
- **Then: the application developer who runs actors in a browser.** The
  web binding becomes one wasm file and one small JavaScript file that
  gives the core memory, timers, the JavaScript datamodel and sockets.
- **The C embedder** gains the same pieces as C functions: the link
  protocol, the snapshot container, trace watches, the exporter's
  batching.
- **Out of scope:** new features; the PostgreSQL extension, which keeps
  its own wiring and could use `src/link` later; the chart compiler; the
  datamodels' semantics.
- **Who pays:**
  - nothing per event or per actor;
  - code size: about 2,000 lines of C in core. Node's `core.mjs` went
    from 2,432 to 1,651 lines, and the web binding's copy of it is
    removed; `native.mjs` is rewritten as the addon's twin (3,558 lines,
    against 2,802 before);
  - API surface: new host functions (state value, snapshot container,
    watches), a new `src/link` layer, and an exporter batcher in
    `src/otlp`;
  - work in every binding: less. Each binding only marshals values and
    performs I/O.

## Context

The rule in [the bindings README](../../tinyactors-core/bindings/README.md)
is that a binding exposes the core and nothing else. The JavaScript
bindings broke it gradually, and each rewrite kept the split it inherited.
That split was a C file of Node-API plumbing under a JavaScript layer that
made decisions. The inventory below lists each part of the JavaScript API,
the core functions behind it, and where its logic goes.

## Decisions

### 1. Layers

- **Language-free logic moves to core**, so Node.js and the web share it:
  - **`src/host`:**
    - an actor's state value as JSON (`ta_host_state_value`), from its
      configuration and the image's state tree;
    - the snapshot container: one blob holding the host snapshot, the
      datamodel's state and the images, with the system id, the datamodel
      name and the time;
    - trace watches with an expiry time, merged with the application's
      rules into one `ta_trace_policy`, so the host owns the classes;
  - **`src/trace`:** detail presets by name (`ta_trace_kinds_by_name`);
  - **`src/base`:** a JSON reader, a pull tokenizer over caller memory,
    for link frames;
  - **`src/link` (new):** the link protocol (`docs/protocol.md`):
    - hello, `ev`, `err`, ping and pong;
    - caps and the frame limit;
    - the name alias of a client link;
    - the reconnect backoff;
    - a server's table of systems and resumption windows;
    - store links as a profile that emits SQL commands.

    It takes received frames and the time, and gives frames to send and
    transport commands (open, close) through an outbox. It has no
    callbacks.
  - **`src/otlp`:**
    - the exporter's batching per signal (by time and by size), its
      cap, and the outcome counters;
    - a fixed-bound histogram for the run duration.
- **Each binding keeps:**
  - marshaling values;
  - the datamodels that need the language (the native JavaScript one);
  - the run loop around `ta_host_run`;
  - effect delivery;
  - I/O:
    - one timer;
    - the hibernation store's calls;
    - I/O processors and invokers;
    - sockets and byte streams;
    - posting OTLP over HTTP.
- **Links are removable.** They are one core directory (`src/link`), one
  addon file (`link.c`) and one JavaScript file. Nothing else names them.

### 2. The Node.js addon is C files per topic

`bindings/node/src/` is split by topic:

| File | Holds |
|---|---|
| `addon.c` | the export table and constants |
| `values.c` | neutral values, wire JSON, base64 |
| `images.c` | compile, open, image info and bytes |
| `pure.c` | addresses, frames, traceparents, span ids |
| `datamodel.c` | the native JavaScript datamodel |
| `quickjs.c` | the QuickJS datamodel, forwarded |
| `system.c` | create and restore, the run loop, effects, spawn, send, deliver |
| `persist.c` | hibernation, export, adopt, migrate, snapshots |
| `router.c` | peers, routes and exports |
| `link.c` | links over `src/link` |
| `trace.c` | the trace encoder, policy and watches |
| `metrics.c` | metrics, decoding, the run histogram |

The JavaScript on top (`core.mjs`) keeps:

- the chart builder;
- `Actor`, `ActorRef` and `toPromise`;
- the timer;
- the store, I/O processor and invoker calls;
- transports;
- the HTTP post.

### 3. Scheduling

- The JavaScript layer arms one timer. The addon calls
  `schedule(delay)`: `0` means more work now, `-1` means idle, and any
  other value is a deadline.
- **Effects:** the host is idle while effects are delivered, so calls made
  from effect handlers go straight into the host. The pump does not nest:
  it notes that there is more input and loops.
- **Calls during a run:** a call made inside a run (from a native
  expression function) is queued in the addon and goes in before the next
  round.
- The `_do` queue, `_run`, `_busy` and `_syncing` are deleted.

### 4. What the API drops

- The watch handle object: `watchTraces` returns the host's watch id, and
  `unwatchTraces(id)` ends a watch.
- The exporter's tuning options: batching and the cap become core
  constants.

### 5. The web binding is the wasm twin

- **The wasm file** holds core, system, host, link, otlp, the compiler and
  QuickJS.
- **The JavaScript file** provides:
  - memory;
  - the timer;
  - the JavaScript datamodel's vtable (through the function table);
  - effect delivery;
  - sockets and `fetch`.
- **The JavaScript file is `native.mjs`, written by hand as the addon's
  twin**: the same functions as `bindings/node/src/*.c`, topic by topic,
  over the module's exports instead of Node-API. Compiling the addon's C
  to wasm against an emulated Node-API (emnapi) was the alternative; it
  was not chosen.
- **The copy of `core.mjs` is deleted.** `mise run web:build` copies the
  Node.js binding's file into the package. The web binding passes the
  Node.js suites with `TA_BINDING=web`.

## Inventory

Each item of the JavaScript API, the core functions behind it, and where
its logic goes:

- *JS* stays in the JavaScript file;
- *addon* moves to the addon's C, and the web JavaScript file does the
  same marshaling;
- *core* moves to a core layer.

| API item | Core functions | Logic today | Goes to |
|---|---|---|---|
| `scxml.chart`, `fromSCXML`, `Chart`, `Expr` | `ta_compile_charts` (through `writeXML`) | `core.mjs` element table | JS (the API's chart builder) |
| expression compilation (`compileExpression`, `__scope`) | datamodel vtable `resolve` | `core.mjs` | JS (native datamodel only) |
| `loadImage`, `Image` | `ta_image_*` | `core.mjs` wrapper | JS over addon |
| `parseAddress`, `formatAddress` | `ta_address_*` | addon | addon |
| `encodeFrame`, `FrameDecoder` | `ta_frame_*` | addon and a small JS class | addon |
| `parseTraceparent`, `formatTraceparent`, `stepSpanID` | `ta_traceparent_*`, `ta_step_key_span_id` | addon | addon |
| `decodeTraces`, `metricsToOTLP`, `TRACE_KINDS` | `ta_otlp_read`, `ta_otlp_put_metrics` | addon, reshaped in JS | addon (the shape too) |
| `createSystem` options: systemID, ecmascript, capacities, seed | `ta_host_new` | JS checks | addon |
| options.restore and `snapshot()` | `ta_host_snapshot`, `ta_host_restore` | JS container (`pack`, `unpack`) | core (host container) |
| run loop: `_do`, `_run`, `flush`, `_at`, own clock | `ta_host_run`, `ta_host_advance_time` | `core.mjs` | addon, plus one JS timer |
| effect dispatch `_effects` | `ta_host_effects`, `ta_host_effects_ack` | `core.mjs` switch | addon delivers; JS only for I/O effects |
| `load`, `boot` | `ta_compile_*`, `ta_host_add_image`, `ta_host_boot` | JS source naming (`~n`), documents | addon |
| `_tree`, `_value` (state value) | `ta_actor_info_get` and image states | `core.mjs` | core (`ta_host_state_value`) |
| `get`, `actors` | `ta_host_actor_by_name`, `ta_host_actors` | JS | addon (JS keeps Actor objects) |
| `send`, `sendMany`, `deliver`, `ref` | `ta_host_send`, `ta_host_send_many`, `ta_host_deliver` | JS address split | addon |
| `Actor.start`, `stop`, `send` | `ta_host_spawn`, `ta_host_terminate`, `ta_host_send` | JS | JS calls the addon |
| `getSnapshot`, `subscribe` | `ta_actor_info_get`, datamodel context | JS | addon computes; JS keeps observers |
| `toPromise` | done and error effects | JS | JS |
| I/O processors (`_ioSend`, sync) | `TA_EFFECT_SEND`, `TA_RUN_EFFECTS` | JS | JS (I/O); the routing of `#_` targets to invocations moves to the addon |
| invokers (`_invoke`, `_uninvoke`) | `TA_EFFECT_INVOKE`, `UNINVOKE` | JS bookkeeping | addon tables; JS starts the service |
| `sweep`, `hibernate`, wake | `ta_host_sweep`, `ta_host_hibernate`, `ta_host_wake` | JS sets and maps | addon; JS calls the store |
| `export`, `adopt` | `ta_host_export_actor`, `ta_host_adopt` | JS | addon |
| `migrate` (jobs, promises) | `ta_host_migrate`, `ta_host_job_pending` | JS job map | addon (a promise resolved on `JOB_DONE`) |
| `stats` | `ta_host_stats_get`, `ta_host_chart_stats` | addon | addon |
| `metrics`, run duration | `ta_otlp_put_metrics`, `ta_otlp_add_stats` | JS histogram and reshaping | core histogram (otlp); addon shapes |
| `setTraceSink`, `configureTraces`, presets, rules | `ta_host_trace_set`, `ta_trace_policy` | JS `traceKinds`, `nativeRule`, classes | core (presets, classes in host) |
| `watchTraces`, `flushTraces` | `ta_host_trace_set` | JS watch list and timers | core (watches with expiry); `flushTraces` JS |
| Exporter (OTLP over HTTP) | `ta_otlp_*` | JS batching, cap, log rate | core batcher (otlp); JS posts |
| `createLink`, `serveLinks` | `ta_host_peer_*`, `ta_host_export`, `ta_host_envelope_json` | JS protocol | core (`src/link`); JS sockets |
| `asTransport`, `streamTransport` | `ta_frame_*` | JS | JS (byte streams are I/O) |
| `createStoreLink` | `ta_host_peer_*` | JS protocol over SQL | core (link profile); JS runs the queries |
| `stop` | `ta_host_free` | JS teardown | addon; JS clears its timer |

## Consequences

- A third binding writes marshaling and I/O, nothing else.
- The core grows a JSON reader. Until now it only wrote JSON.
- A behaviour change in scheduling, hibernation or links is a C change,
  tested by unit tests in `tests/unit/`, not by the binding suites alone.
