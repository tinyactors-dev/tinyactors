# Writing a binding

**Status:** Current (2026-10-13), after [ADR 0009](../../docs/adr/0009-one-host-two-queues.md) steps 2 to 6, [ADR 0010](../../docs/adr/0010-outputs-are-views-values-stream.md), [ADR 0011](../../docs/adr/0011-the-control-plane-is-posted-messages.md) and [ADR 0012](../../docs/adr/0012-lifetimes-follow-the-loop.md). Drafted in the proposal [One host, two queues](../../docs/proposals/one-host-two-queues/README.md).

How to use the core from C, for every embedder, is [embedding.md](embedding.md): read it first; this guide adds what a language binding does on top of it. Why the boundary is shaped as it is, with the contract of every call and every alternative considered and rejected, is in [boundary.md](boundary.md). Everything a binding author needs, in one place: the rules, the run-loop lessons of the past restructures, and the interface a binding drives. Write a binding from this guide and the headers, not from an older binding (both were removed for their rewrite, `6426816a` and `70434bee`).

## 1 · What a binding is

A binding lets one language drive `ta_host` in one runtime. It is that runtime’s extension, written in C against the runtime’s API (the CPython C API, Node-API): the extension defines the language’s types, runs the loop, converts values and implements the native datamodel, and the language holds nothing above it but loading it and, later, a convenience layer over its API (below, “Two layers”). There is no shared package per language; how a browser is served is decided later (`ROADMAP.md`). It has three parts and an API:

- **The queue loop:** feed inputs, run the host, read its effects, do the I/O they ask for, acknowledge them, arm one timer.
- **A datamodel:** the language’s own values and expressions behind the datamodel vtable, with the ECMAScript datamodel (quickjs-ng), which needs nothing from the binding, beside it (section 8).
- **A CBOR module:** one generic conversion between CBOR and the language’s values, streamed item by item with core’s writer and reader (`tinyactors/cbor.h`; ADR 0010). It carries the host’s config, posted messages, their replies and the host’s notifications, and the bytes that cross a process boundary: event data the application sends to another system, if it chooses the value table's CBOR.
- **The API:** what the language’s developers call. It is thin: each item is a host input, a typed method over a posted message, the handling of an effect, or a call of the compiler (`compile.h`).

**Two layers** (decided 2026-10-13). The **direct layer** is the extension: it mirrors the C API more or less one to one, in the language’s types and conventions (`ta_compile_charts` is a `compile_charts`, `ta_host_chart` a `chart`, `ta_host_spawn` a `spawn` that takes the chart reference `chart` returns), with the mapping table (section 2) behind every item. A **convenience layer**, written in the language over the direct layer only, may come later: it adds shapes, never behaviour (compiling a name→text mapping into an image, spawning by chart name, building a chart without writing XML, which `ta_xml_writer` in `compile.h` already serves). It calls no core function the direct layer does not expose, holds no C, and is not built before the direct layer has passed its milestone. Everything else this guide says about the API is about the direct layer.

**A resolver is any callable of the language,** `resolver(base, source) -> text` (one function: a function, a lambda, a bound method), given to the direct layer’s compile call, where it is the `ta_compile_options.resolve` callback: a static `src`, fetched at compile time and compiled into the same image; its refusal is reported as a diagnostic at the line and column it names ([boundary.md section 12](boundary.md#12-compiling-and-the-resolver)). The callback receives `request->scratch` and fills `ta_resolve_result`; copy language-owned names, text and messages into that arena when their references cannot be kept until compilation returns. **The resolver lives only at compile time:** the host has none. A `TA_EFFECT_REQUEST` (a fetch at run time) is answered in C with `TA_ERROR_INVALID` until run-time fetching is built (section 4); a runtime resolver on the host may answer it then, not now. A resolver that reads a directory belongs to the convenience layer; what it reads is the application’s.

## 2 · What a binding never does

- **It exposes the core, nothing else.** Before designing the API, inventory the public headers it drives and write the mapping table: each API item next to the core functions, effects, methods and flags behind it. An item with no core function behind it does not belong. A core function with no item is used internally (say where) or not exposed (say why).
- **It borrows shape, never semantics.** Another library (xstate for JavaScript) may suggest names and call shapes: `createActor`, `send`, `getSnapshot`, `subscribe`. What a call does is what the core does. Never emulate another library’s behaviour, rename the core’s events, or lower its concepts onto SCXML.
- **It invents nothing:** no event names, flags, fields, methods, defaults or mechanisms of its own. A real gap is a core proposal, not part of a binding.
- **It keeps no twin of C.** SCXML semantics, queues, routing, timers, the invoke lifecycle, addresses, trace policy, encodings: if C does it, bind the C.
- **It reads the core before claiming a gap.** `src/core` and `src/system` implement all of SCXML, including I/O processors, external invokes, sends to `#_<invokeid>` and error events. Cite the lines of `docs/SCXML.md` and the core before saying “the core cannot”.
- **It decides no policy.** Defaults, limits, validation and their error messages live in core and come back in replies. Retry and backoff for the binding’s own I/O (an HTTP post, a reconnect) are the exception: that is I/O.
- **It puts no serialization on the hot path.** The calls made per event take and give C structs; CBOR is for rarely called calls and for bytes that leave the process (the repository’s `AGENTS.md`, “No serialization on the hot path”).

## 3 · Creating a host, and the queue loop

```
config  = cbor_encode({ systemID: "app", system: {...}, telemetry: {...} })
error   = ta_buffer of your own
env     = { allocator, datamodels[], datamodel_count, trace_policy, error: &error }
ta_host_create(config, &env, &host)      # TA_ERROR_INVALID: error holds the reply
ta_host_post(host, {"method": "host.config", "id": 0}); drain(host)
rate    = reply.result.system.ticksPerSecond
                                         # once, in the binding's own C, before the host
                                         # is handed to the application (no batch is open):
                                         # the ticks per second of the monotonic clock
ta_host_add_image(host, image_bytes, &segment); ta_host_boot(host, segment)
                                         # the host copies the image's bytes (section 6)
```

- **The config is CBOR** ([host-config.md](host-config.md)): the system id, each segment’s capacities, the host’s I/O processors, the OTLP encoder and the host’s limits. Build it with the CBOR module from the language’s data. Core fills the defaults and validates it, and the method `host.config` ([requests.md](requests.md)) answers the effective config, every default filled in; a refused config answers in `environment->error` with the same `{"error": {"code", "message", "data": {"reason", "path"}}}` as a refused request. Raise it as the language’s exception, unchanged.
- **The environment** holds what cannot be data: the allocator, the datamodels (at most 8), the trace policy and the error buffer.

```
on wake (timer fired, input arrived, I/O completed):
    drain(host)                             # what calls since the last ack appended
    ta_host_advance_time(host, { monotonic, wall_ns })
    repeat:
        run = ta_host_run(host, budget)
        drain(host)                         # the run's effects, then its notifications

        TA_RUN_EFFECTS_FULL → the effect list was full: run again now
        TA_RUN_EFFECTS      → senders waited for this batch: they go on at its ack, run again now
        TA_RUN_YIELDED      → give the event loop a turn, wake again at once
        TA_RUN_EXHAUSTED    → the allocator refused (run.detail names the pool):
                              run again once it has memory
        TA_RUN_FAULT        → a segment could not run, or ta_host_run was called
                              during a run or with a batch open: report it
        TA_RUN_IDLE         → arm one timer at run.next_deadline (UINT64_MAX: none)
    whatever a handler raised: ack anyway, finish the turn, then raise the first error

drain(host):                                # section 5: one drain at a time
    while (n = ta_host_effects(host)) > 0:  # the open batch; never widened
        for i in 0 ..< n:
            handle(i)                       # calls into the host are fine; what they
                                            # append comes with the next take
        ta_host_effects_ack(host)           # everything handed out ends here
```

**The same loop in the binding's C.** Every binding writes embedding.md
section 2's loop as these two functions, named with its prefix
(`tinyactors-cpython/AGENTS.md`, "The loop", has Python's, with both
drivers). The comments carry the rules:

```c
static void drain(binding_host *host) {                     /* one batch at a time */
    const ta_host_effect *e; uint32_t n;
    while ((n = ta_host_effects(host->h, &e)) > 0) {
        for (uint32_t i = 0; i < n; i++) dispatch(host, i); /* handlers may call the host:
                                                               inputs apply now, effects come next take */
        ta_host_effects_ack(host->h);                       /* everything handed out for it ends */
        host->acks++;                                       /* the batch's thin views are stale from here
                                                               (section 6); core has no batch counter */
    }
}

/* one turn: returns what the driver does next */
static binding_next turn(binding_host *host) {
    drain(host);                                            /* what inputs and posts appended:
                                                               a pending reply needs no run */
    ta_host_advance_time(host->h, now(host));               /* ta_host_time: ticks at host.config's
                                                               rate, wall ns */
    for (;;) {
        ta_run run = ta_host_run(host->h, BUDGET);          /* bounded: BUDGET fuel per call */
        drain(host);
        switch (run.status) {
        case TA_RUN_EFFECTS:                                /* senders waited for the batch drain acked:
                                                               they go on now (host.h, ta_host_run) */
        case TA_RUN_EFFECTS_FULL:  continue;                /* run again now */
        case TA_RUN_YIELDED:       return NEXT_YIELD;       /* budget spent, actors still runnable */
        case TA_RUN_IDLE:          host->deadline = run.next_deadline; return NEXT_IDLE;
                                                            /* UINT64_MAX: no deadline */
        case TA_RUN_EXHAUSTED:     host->pool = run.detail;  return NEXT_EXHAUSTED; /* a TA_POOL_* */
        case TA_RUN_FAULT:         host->fault = (ta_status)run.detail; return NEXT_FAULT;
                                                            /* a negative status, for ta_status_name */
        default:                   host->fault = TA_ERROR_UNSUPPORTED; return NEXT_FAULT;
                                                            /* a status this build does not know */
        }
    }
}
```

`ta_run`'s `status`, `detail` and `next_deadline` and the six statuses are
`system.h:196-218`; `ta_host_advance_time` takes a `ta_host_time`
(`host.h:205-214`). What a driver does with each result:

| `turn` returns | A synchronous driver | An event-loop driver |
|---|---|---|
| `NEXT_YIELD` | check the runtime's signals (a pending interrupt), then turn again | give the event loop a turn (a task scheduled at once), then turn again |
| `NEXT_IDLE` | return (run until idle), or sleep to `deadline` and turn again (run forever); with a simulated clock, return | arm one timer at `deadline` (none at `UINT64_MAX`), replacing the last; an input from outside a turn schedules one turn |
| `NEXT_EXHAUSTED` | raise the language's out-of-memory error naming the pool; the host stays usable and the next turn goes on where the run stopped | the same, from the turn's callback |
| `NEXT_FAULT` | raise the binding's error with `ta_status_name(fault)`; not retried | the same |
| a handler raised | the turn finishes (the ack comes anyway), then the first error is raised | the same |

- **One timer,** re-armed after every loop; never one per actor or per deadline. `next_deadline` already holds every deadline the host has: timers and delayed sends, a migration’s timeout, the end of a trace watch, and the encoder’s flush interval.
- **Both clocks in one call.** `monotonic` (`ta_time`, in ticks at the rate `host.config` answers as `system.ticksPerSecond`) drives timers, deadlines and TTLs; `wall_ns` (nanoseconds since the Unix epoch) stamps the trace records written since the last call. Call it before every run.
- **Bound each turn.** The budget caps work per run; across many runs, yield to the runtime’s event loop (a macrotask in a browser, a callback in Node, a Looper message on Android) so a busy system never blocks it.
- **A caller’s own clock** (simulation, tests) replaces the timer: time moves only when the caller advances it and runs the loop. A binding offers it as a simulated clock in its public API (the shape of xstate’s `SimulatedClock` and `increment(ms)`, the meaning ours): a host created with one arms no timer; `increment(ms)` calls `ta_host_advance_time`, with the simulated time as the wall clock, and runs the loop until idle; inputs run the turn at the current time.
- **Not every program has an event loop.** Offer the same turn under two drivers where the language has both: a synchronous one (run until idle, sleeping to the next deadline or following the caller’s clock) and the language’s event loop (asyncio, a Looper, a dispatch queue).

## 4 · Effects: what to do with each

Read an effect’s fields with `ta_host_effect_number`, `ta_host_effect_text` and `ta_host_effect_bytes` (`WIRE`: a relayed value, a reply, a notification) (`TA_EFFECT_FIELD_*`, in `host.h`), by its index in the batch `ta_host_effects` returned; every field, `TRACEPARENT`’s text included, is valid until the ack. A take with no batch open hands out everything pending, with or without a run: the effects calls appended between runs in call order (replies, a deliver’s `PEER_ERROR`, `trace.end`’s `TRACES`), then a run’s effects in drain order, then the run’s notifications at its end.

**What reaches the application** (ADR 0012, decision 6), and through which channel:

| The application wants | Channel | Effect or call |
|---|---|---|
| an actor’s output | an I/O processor the application configured, one handler per type, given the send as a thin view (section 6); it may answer the send or fail it | `TA_EFFECT_SEND` to the application’s handler; `ta_host_send` for the answer |
| a service the chart invokes | a service the application registers by type | `TA_EFFECT_INVOKE`, `TA_EFFECT_UNINVOKE`, sends with `IO_PROCESSOR` `TA_NIL` |
| an actor’s state | a typed method, `actor.state` | `TA_HOST_EFFECT_REPLY` |
| to inspect | a typed method over post, with a response handler called by a later drain (section 10) | `TA_HOST_EFFECT_REPLY` |
| `<log>` lines | a log handler configured per host, given each line as a thin view; none configured: discarded. No global configuration (no `logging.basicConfig`, no console default) | `TA_EFFECT_LOG` |
| telemetry | the binding’s exporter, configured by the application | `TRACES`, `LOGS`, `ta_host_metrics` |

`DONE` and `ERROR` are not on that list: they are the binding’s own lifecycle.

| Effect                  | The binding                                                                                                                                                                                                                       | Answers with                                                                                                                         |
|-------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------|
| `TA_EFFECT_SEND`        | The actor’s output: hand it to the application’s handler for the I/O processor `io_processor - 1` of the config’s `ioProcessors` (they are numbered as `_ioprocessors`: 0 is the SCXML processor), as a thin view (section 6). Its sender waits until the batch is acknowledged, so an answer or a failure given in the handler comes before the sender goes on. With `io_processor` 0 it is a send to another system, for the application's transport handler; with `TA_NIL` a send to an invoked service: route it by `token`.          | The handler’s answer: `ta_host_send` to the sending session (`SESSION`) with the send’s `sendid` and `origintype` the processor’s type; its failure: `ta_host_send` with `TA_EVENT_IN_ERROR_COMMUNICATION` and the `sendid` (`system.h:155-157`), the event name not repeated.              |
| `TA_EFFECT_INVOKE`      | Start the service of type `event` with `target` (its src), `sendid` (its invokeid) and `payload` (its params), keyed by `(segment, token)`. Sends to it carry the same token: keep no table of invoke ids. Flagged `TA_EFFECT_FLAG_RESTORED`, it is a restored host announcing an invocation that was running at the snapshot (at its first run): the application's handler starts the service again, or a stand-in; without one, a communication error. | Its events with `TA_EVENT_IN_FROM_INVOKE` and the token; at its end `TA_EVENT_IN_INVOKE_DONE`; its failure `TA_EVENT_IN_FROM_INVOKE \| TA_EVENT_IN_ERROR_COMMUNICATION` with the token (core ends the invocation's side of the service: not announced again, later events refused). |
| `TA_EFFECT_UNINVOKE`    | Stop that service; forget the key.                                                                                                                                                                                                | Nothing.                                                                                                                             |
| `TA_EFFECT_REQUEST`     | A fetch at run time (2026-10-13: it stays; ADR 0012, decision 8): an `<invoke srcexpr>`, or a `src` the compiler did not match, whose name (`auxiliary`) core normalized and found no chart for in the requesting segment’s image (`route.c:439-497`). The fetching is built out later ([boundary.md section 12](boundary.md#12-compiling-and-the-resolver); `ROADMAP.md`, “Decided later”). Until then, answered in C, never by the language, and without a lookup of its own: core has already looked. Not the control plane, which is `ta_host_post` and its `REPLY` and `NOTIFICATION` (rows below; ADR 0011). | `ta_host_resume` with the token and `TA_ERROR_INVALID`; the parent gets `error.execution` (`route.c:544-550`). A resume names only a chart of the requesting segment’s image (`:544-553`). |
| `TA_EFFECT_DONE`, `TA_EFFECT_ERROR` | Internal to the binding’s C, not application callbacks: a root actor finished (`payload`, its done data, is released by core at the ack) or faulted (`flags`, the fault code; counted in `host.stats`’ `faults`). Release the rows the binding keeps for the session. | Nothing.                                                                                                                             |
| `TA_EFFECT_LOG`         | A `<log>`: label `auxiliary`, text `event`, session `session`. To the host’s log handler as a thin view (section 6); without one, nothing. | Nothing.                                                                                                                             |
| `TA_EFFECT_WAKE`        | Load the actor’s blob from the store. `MIGRATION` names the migration that asks (0: an event or a timer); a migration asks again when it finds the actor hibernated after a wake, so an application that keeps few actors in memory may queue a migration’s WAKEs and wake them a batch at a time. If the binding still holds that blob (its `put` has not settled yet), wake from the held copy and delete the store’s copy when the put settles.                                             | `ta_host_wake` with the blob.                                                                                                        |
| `TA_HOST_EFFECT_MIGRATED`, `TA_HOST_EFFECT_MIGRATION_DONE` | A session moved to a new chart version; a migration ended. Settle what the application started (its promise, future or callback) with `migrated`, the failed sessions and why (`ta_host_migration_failed`) and the pending sessions (`ta_host_migration_pending`). | Nothing.                                                                                                                             |
| `TA_HOST_EFFECT_SEGMENT_FREED` | A version retired: tell the application if it asked. The host freed its copy of the image; the binding keeps none.                                                                                                       | Nothing.                                                                                                                             |
| `TA_HOST_EFFECT_REPLY`  | The answer to a message the binding posted (`ta_host_post`): `WIRE` is `{"id", "result"}` or `{"id", "error"}`, the last effect of that post, in a take after the post. Settle the typed method’s pending result whose `id` it carries (section 10): its `result`, or its `error` as the language’s exception. | Nothing.                                                                                                                             |
| `TA_HOST_EFFECT_NOTIFICATION` | A notification of the host ([requests.md](requests.md), “Notifications”): `WIRE` is `{"method", "params"}`, `EVENT` the method. None is defined. Drop a method you do not know. | Nothing. |
| `TA_HOST_EFFECT_TRACES`, `TA_HOST_EFFECT_LOGS` | The encoder has a request ready. Call `ta_host_traces` (`ta_host_logs`): it gives a view of the request in the host’s memory until the ack; copy it into the language’s bytes if the post outlives the ack, then post it (`application/x-protobuf` to `/v1/traces`, `/v1/logs`) or give it to a sink. Retries and a cap on what waits are yours. At most one of each is outstanding: the next comes after a take. | Nothing.                                                                                                                             |

- **A host without telemetry** has a null encoder: it never gives `TRACES` or `LOGS`, and `ta_host_traces`, `ta_host_logs` and `ta_host_metrics` answer `TA_ERROR_STATE`.
- **Stopping ends the open spans.** A macrostep cut short has its span still open. `stop()` posts the `trace.end` message, drains (its `TRACES` comes before its reply, in the next take), takes traces with `ta_host_traces` until a take writes nothing, posts them, acks, and then calls `ta_host_free`. Ended spans carry `scxml.incomplete` and end at the last `wall_ns`; `ta_host_free` drops what was not taken.
- **Metrics are pulled.** `ta_host_metrics` writes the counters, as an OTLP metrics request, into the host’s memory whenever you call it, a view until the ack. Taken with no batch open, it opens one of its own: copy it and ack before the next take or run. There is no metrics timer or effect: read them when the application asks, on `stop()`, or on a cadence the application chooses.

## 5 · Inputs and reentrancy

- **Between runs, any call is fine,** including from an effect handler before the ack, except `ta_host_run` and snapshots: effects already taken stay where they are, outputs the call hands out live until the ack, and effects the call makes come with the next take.
- **One drain at a time.** A take returns the open batch and never widens it; the drain handles it whole and acks once; effects that handlers caused come with the next take. `ta_host_run` while a batch is open answers `TA_RUN_FAULT` with `TA_ERROR_STATE` and runs nothing. An empty take opens no batch. There is no nested drain: a language callback that runs during a drain (an I/O processor’s handler) and calls the binding again appends inputs, posts or reads outputs; it never takes or acks.
- **During a run** (a datamodel entry evaluating an expression), `ta_host_send`, `ta_host_send_many`, `ta_host_spawn` and `ta_host_terminate` take the paths of an internal `<send>`, `<invoke>` and cancel: no queue of their own, no copy, and nothing grows, so a full pool answers `TA_ERROR_NO_MEMORY` and the payload stays the caller’s. When they take effect:
  - an event for the actor whose expression sent it: at the end of the macrostep it is in, never inside it;
  - an event for another actor, or a spawned actor: at the back of its segment’s run queue, later in this run if the budget allows, else in the next;
  - a terminated actor exits at its next microstep boundary; the actor whose expression terminated itself does not run the rest of its macrostep.
- **Lookups answer during a run** (`ta_host_chart`, `ta_host_locate`, `ta_host_actor_by_name`, `ta_host_segment_count`, `ta_host_segment_info_get`, `ta_host_system`, `ta_host_resolve`, `ta_host_migration_failed`, `ta_host_migration_pending`, and the effect accessors, which answer empty: no batch is open in a run), and `ta_host_free` does nothing. **Everything else is refused:** calls that change the host’s structure (adding images, hibernating, waking, migrating, snapshots), every posted message, the telemetry calls (`TA_ERROR_STATE`); `ta_host_effects` returns 0, `ta_host_effects_ack` and `ta_host_advance_time` do nothing, and `ta_host_run` returns `TA_RUN_FAULT`. An expression cannot read another actor’s state. Say so in the API.
- **One thread.** Parallelism is shared-nothing: several hosts, one per thread, talking through their transports (section 9). Do not reach for shared memory in a binding (in the browser it would require cross-origin isolation); a multi-threaded host would be a core decision first. A host is single-threaded: every call from the thread that made it. A threaded runtime confines it (one host per worker, an executor, a serial queue), and the binding checks it: record the creating thread and refuse calls from others. A global lock (Python’s GIL) is not a substitute: free-threaded builds have none. Keep the binding’s state per interpreter or isolate, never in C globals.
- **Outside a run, while the host is inside a datamodel** (a posted message, hibernate, the telemetry takes, `release` in the ack, and free), the same rules apply as during a run: a datamodel’s code that calls back into the host can append inputs but cannot change what core is using (ADR 0010, decision 3).
- **Freeing the host from an effect handler** is safe only after the loop returns: mark it, finish the loop, then free.
- **The datamodel vtable is the only call out of core.**

## 6 · Memory: who owns what, for how long

| What                                                                                       | Owner                                        | Valid until                                                                                                                                                                                              |
|--------------------------------------------------------------------------------------------|----------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Every block of the host and its segments                                                   | the host, from `environment->allocator`      | `ta_host_free`, which returns all of them. A refused block leaves the host as it was: the call returns `TA_ERROR_NO_MEMORY`, a run returns `TA_RUN_EXHAUSTED` and the next run goes on where it stopped. Nothing grows during a run. |
| Effect records, their strings, bytes (replies and notifications), `TRACEPARENT`’s text and payload views | the host (borrowed)                | `ta_host_effects_ack` or `ta_host_free`, not the next run. Copy what you keep past the ack.                                       |
| A posted message                                                                           | the caller                                   | the call to `ta_host_post`; its reply is an effect.                                                                                                                                                      |
| The config bytes                                                                           | the caller                                   | the call to `ta_host_create`; nothing points into them afterwards.                                                                                                                                       |
| Views passed in (names, traceparents, origins, an address to resolve)                     | the caller                                   | the call; the host copies what it keeps.                                                                                                                                                                 |
| Payloads passed in                                                                         | moved to the host on success                 | the caller’s again on failure: release it. A payload from `from_wire` carries one reference.                                                                                                             |
| Image bytes passed in (`ta_host_add_image`, `ta_host_snapshot_input.images`)               | the caller                                   | the call: the host copies them into a block of its own, opens them there and frees it with the segment. Keep nothing; free or reuse the bytes once the call returns.                                    |
| An image a segment runs (`ta_host_segment_info.image`)                                     | the host (borrowed)                          | its segment is freed (then `NULL`) or the host goes.                                                                                                                                                     |
| A segment’s `ta_system` (`ta_host_system`)                                                 | the host (borrowed)                          | the next call that can grow or free it.                                                                                                                                                                  |
| A migration’s pending sessions                                                                   | your `ta_uint64_array`                       | yours.                                                                                                                                                                                                   |
| What the host writes for you: traces, logs, metrics, hibernation blobs | the host (a view)                | `ta_host_effects_ack` or `ta_host_free`, whatever calls come between. Copy it once into the language’s bytes, which the application keeps without another copy; never hand the application a view of the host’s memory ([boundary.md](boundary.md#7-host-outputs)). Copy what you keep past the ack. |
| Snapshots, and a refused config (`environment->error`)                                     | the caller’s `ta_buffer`                     | yours. On `TA_ERROR_NO_MEMORY`, `length` is what it needs and nothing changed: grow and call again.                                                                                                      |
| A datamodel’s output: `to_wire`, `serialize`, `payload_serialize`, `eval_string`, `log`    | the datamodel (a view)                       | the next call on the same datamodel; core copies what it keeps (ADR 0010, decision 1).                                                                                                                   |
| Hibernation blobs in flight                                                                | the binding, until the store’s `put` settles | the put settling. A wake can arrive first: wake from the held copy (section 4).                                                                                                                          |

- **One convention for bytes.** Bytes go in as `ta_bytes`. They come out as a `ta_bytes` view whose owner and lifetime the call states (the host’s outputs, a datamodel’s outputs), or, where the caller provides the memory (snapshots, the config error, the freestanding layers), through a `ta_buffer *`. No public function takes a pointer and a length (the lint rule `buffers-are-ta-buffer`).
- **Core never allocates after init below the host;** the host allocates only through its allocator, and only between runs; the CBOR functions allocate only through the allocator a caller gives a writer.
- **The binding allocates almost nothing.** Allocations and frees must pair, so keep them few and in one place. What a binding keeps:
  - **Its handle tables.** Language objects (a session's scope, a chart's
    compiled expressions, payload values, a typed method's response
    handler, references to the runtime) cannot be in core's memory (no
    pointers, moved by `memcpy`, hibernated) and must be visible to the
    garbage collector. A chart, session, payload, `<foreach>` or pending
    typed method is a row of its table, not an allocation of its own.
    The table is core's `ta_table` (`tinyactors/table.h`, ADR 0012 decision
    5): rows of the binding's size, 64-bit handles with a generation so
    a stale one is refused, grown by doubling through the environment's
    allocator, slots reused, freed once with the host. A row pointer is
    valid until remove, free or the next successful `ta_table_add`; keep
    handles, not pointers. Release row-owned references before removing or
    freeing their storage. The binding writes no table of its own. For a
    host snapshot, `ta_table_snapshot` writes a table's handle namespace
    (generations and free list, not the rows) and `ta_table_restore` puts
    it back into an empty table: the binding saves that and each row's
    values beside the snapshot, and refills the rows at their handles
    after `ta_host_restore`, before the first run. A session names its
    chart by `ta_datamodel_chart.segment` and `chart`, never by the
    image's hash alone: two segments may run one image, and the restore
    attaches the charts again under new contexts. Images
    need no table: pass the bytes (core's `ta_compile_image` view, a
    caller's buffer, at any alignment) to `ta_host_add_image`, which copies
    them; the compile result can be freed as soon as the call returns (the
    direct layer's compile object frees it at its close, section 1).
  - **One arena per host, a mark per call.** A `ta_growable_arena`
    (`tinyactors/arena.h`, ADR 0012 decision 5) made with the host from the
    environment's allocator and freed with it. Every API call takes a mark
    at its entry (`ta_growable_arena_position`) and releases to it at its
    exit (`ta_growable_arena_release`), on every path, exceptions included.
    It holds what that call converts: the CBOR of a config, a typed
    method's params and a payload for another datamodel (a `ta_cbor_writer`
    whose allocator is the arena's `ta_allocator` view), text where the
    runtime gives no view of it (Node-API), and input structs (the
    `ta_event_in` array of a `send_many`). Application code that runs during
    a call (a getter, `toJSON`, `__iter__`, a finalizer) may call the binding
    again: the nested call allocates above the outer call's memory and
    releases to its own mark, and nothing moves. Released chunks are kept;
    repeating an identical warmed allocation sequence reaches no backing
    allocator. Store the by-value allocator view for the lifetime of the
    consumers borrowing its address, and finish those consumers before
    releasing their mark. Nothing else is shared between calls.
  - **A datamodel's last output.** A native datamodel writes `to_wire`, `serialize` and `payload_serialize` with a writer it keeps between calls, and hands strings back to core (`eval_string`, `log`) from a language object it holds or, where the runtime gives no view of text (Node-API), from that same memory; each is overwritten by its next call.
  - **Views, never copies, of what the language holds:** a string or bytes passed into a call is a view for the call's duration (CPython's `PyUnicode_AsUTF8AndSize`, the buffer protocol, a typed array's data). Read what core supplies (system variables, a chart's states through `ta_image_*`) when it supplies it.
  - **Values cross by copy inside the process, and are shared when immutable.** A value the application passes in (an event's data, a spawn's params) becomes a payload row through the value table: a mutable container is copied, an immutable value shared. CBOR only where the other side reads it (another datamodel, an envelope).
  - So the allocator is called only by the tables, the arena while it grows, and the host's create and free; nothing per event, nothing per evaluation. A lint rule keeps allocation calls in those places.
- **Thin views of what core hands out.** Wherever a C value is exposed to the language, the binding hands it a thin view where it can, not a converted copy: an object holding where the value is (the host, the batch, an index) that converts a field only when the language reads it, and caches it. A handler pays nothing for a field it does not read. As the native datamodel's `_event.data` is its payload row's value itself (section 8), a view's `data` from the binding's own datamodel is that row's value, not a copy. The rules:
  - **A view of an effect is valid while its batch is open**, until `ta_host_effects_ack` (ADR 0012, decision 1). The binding counts its acks (`host->acks`, bumped right after the ack in `drain`, section 3); a view records the count when it is made, and reading a field when the count differs (or after the host was freed) raises the binding's error with `ta_status_name(TA_ERROR_STALE)`, the status core gives a stale handle (`types.h:108`). Core keeps no batch counter and needs none: only the binding acks. A value already read out of a view is the language's, and outlives the ack.
  - **What must outlive the ack is materialized**, at a point the binding names, before the ack: every field read and converted into a record of the language's objects. The case in every binding: a handler the event loop runs later (an `async def` processor, a promise continuation) gets a record made before it is scheduled.
  - **Converted once instead of viewed**, where a view buys nothing: a reply's `result` (section 10), decoded when its `REPLY` is dispatched, because its one reader, the response handler, reads it whole and often keeps it, and a lazy view of a nested CBOR map would need random access into bytes that end at the ack.
  - **Views of what the language reads only during a call into it** (the system variables at `create`, an event's strings at `bind_event`, valid only during that call, `datamodel.h:35-37`, `:62-63`) cannot be thin: the datamodel converts them then, once.

| Object exposed to the language | What it holds | Converts | Valid until | Then |
|---|---|---|---|---|
| a send to an I/O processor (a thin view) | the host, the ack count, the effect's index | each field on first read: `session` (`SESSION`), `event` (`EVENT`), `target` (`TARGET`), `sendid` (`SENDID`), `data` (`PAYLOAD`: the native datamodel's row value; another datamodel's payload decoded from its `to_wire`, on first read) | the ack | a read raises stale; its answer and fail raise stale |
| a send record (materialized) | the processor's type, `session`, `sendid`, `event`, `target`, `data`, as the language's objects | every field, before the ack, when the handler is scheduled to run later | the language's collector | its answer and fail call `ta_host_send` any time while the host lives |
| a log entry (a thin view) | the host, the ack count, the effect's index | `session` (`SESSION`), `label` (`AUXILIARY`), `text` (`EVENT`) on first read; no chart (a `LOG` effect carries none) | the ack | a read raises stale |
| a reply's result | the decoded `result` (or the error) | the whole `result`, once, at dispatch | the language's collector | — |
| `_event` (native datamodel) | name, type, sendid, origin, origintype, invokeid, raw; `data` | the strings at `bind_event`; `data` is the payload row's value, with a reference on the row | the next `bind_event` or `destroy` (kept across `detach_event`: SCXML keeps `_event` bound until another event) | the row's reference is dropped |
| system variables (native datamodel) | `_sessionid`, `_name`, `_ioprocessors`, immutable | at `create`, once | the session's end | — |

- **Statuses are named by core.** A bare status in an error message is
  `ta_status_name(status)` (`"TA_ERROR_STATE"`; `NULL` for one core does
  not define; ADR 0012 decision 5), never a table of the binding's.
- **Only C touches host structs.** Read effects with the accessors, and let the addon, which is C in every runtime (Node-API, the same C compiled into a wasm module, cffi), fill input structs. Never copy a struct layout into another language.
- **Garbage collection.** Free the host deterministically when the application stops it, and drop every language reference the binding holds (handlers, scopes, payloads, functions) then; offer the language’s scoped form (a context manager, `using`, `defer`). A finalizer is only a fallback: handlers often close over the host. Where the language has a cycle collector, let it see those references (the CPython slots `Py_tp_traverse` and `Py_tp_clear`).
- **Snapshots are not durability.** Hibernation and snapshots take actors out of memory for a while, and a snapshot lets a host be inspected. A snapshot is raw region memory, valid only for the same build of the library: never offer one as a backup, a save file or crash recovery, and never write it somewhere it outlives the build. Durability is the application’s (a database). The hibernation store is where idle actors wait while out of memory, not a record of them: a store that outlives the host (IndexedDB, a file) keys blobs per host and clears them when a host is created and when it stops, since no other host can wake them.

## 7 · Time

- One monotonic clock in ticks, at the rate `host.config` answers as `system.ticksPerSecond` (a config without the key gets the system’s default, 1000000000: nanoseconds). Read it once, after creating the host, and convert the language’s clock with it, both ways: a time for `ta_host_advance_time`, and `next_deadline` back to the clock of the timer you arm. Never assume a rate of your own; a caller who wants milliseconds sets `ticksPerSecond` 1000 in the config. Advance the clock with the wall clock before every run and every input that depends on it (spawn, deliver).
- A host restored from a snapshot (same build, while its actors were out of memory) resumes at the snapshot’s time: start the clock there.
- Span times are wall-clock nanoseconds, interpolated between the `wall_ns` of two calls to `ta_host_advance_time`; 0 means unknown.

## 8 · Choosing a datamodel

- **A datamodel is chosen by name.** Core matches each registered vtable’s `name` against the chart’s `datamodel` attribute. A binding may define a datamodel of its own language under its own name (`"python"`), with no core change: charts that ask for it evaluate that language. A host takes up to eight datamodels (`environment->datamodels`), and each chart uses the one it names. Core never translates a value between datamodels: a payload goes only to the datamodel it came from, and event data that arrives from another system is read by the datamodel of the chart it is delivered to.
- **ECMAScript** (`ta_datamodel_ecmascript_vtable()`, name `"ecmascript"`, on quickjs-ng): nothing to implement and no calls into the language; values cross as CBOR. Every binding registers it beside its native datamodel, which comes first (its developers write chart logic in their language): both pass the same suite (section 12).
- **Native:** expressions run in the language. Implement the datamodel vtable (`tinyactors/datamodel.h`; every per-instance entry takes a `ta_datamodel_call`, `create` and `deserialize` a `ta_datamodel_start`, `<foreach>` a `ta_datamodel_loop`, and blobs go through `ta_bytes`: in as views, out as views the datamodel owns until its next call): compile a chart’s expressions once at `chart_attach` (by kind: condition, value, location, script), keep one scope per session, and two tables of handles, for payloads (with reference counts) and for sessions, each a slot and a generation, never a pointer, so every language reference the binding holds can be found and a stale handle is refused. `to_wire` and `from_wire` convert a payload to and from a neutral value (section 9). `serialize`/`deserialize` and `payload_serialize`/`payload_deserialize` write the datamodel’s own blob format for hibernation and export. `variable` answers the `actor.context` method. An expression that throws returns `TA_ERROR_EXECUTION`, so core raises `error.execution` as SCXML requires; pass the exception to the logger and invent no effect for it. An exception that is the runtime’s rather than the chart’s (Python’s `KeyboardInterrupt`) is kept and raised again after the run.
- **The scope keeps SCXML’s rules.** Assigning to an undeclared location, or to a system variable (`_event`, `_sessionid`, `_name`, `_ioprocessors`, `_x`), is `error.execution`. Make the scope refuse it rather than check after the evaluation: Python’s session is an instance of a class with `__slots__` for the chart’s `<data>` ids and read-only properties for the system variables, so an undeclared name or a system variable cannot be assigned (`tinyactors-cpython/AGENTS.md`, “The "python" datamodel”); JavaScript compiles in strict mode inside `with (scope)`, as the ECMAScript datamodel does.
- **`_event` lives from `bind_event` to the next `bind_event` or `destroy`.** `bind_event`’s strings are valid only during the call (`datamodel.h:35-37`), so the datamodel converts them then; the data is a payload row, whose value `_event.data` is, with one reference the event holds. A datamodel that sets `TA_DATAMODEL_EVENT_DETACHABLE` gets `detach_event` at the stable point after the event’s macrostep (`maybe_detach`, `src/core/core.c:468-478`, `:1870`), and core frees its own copy of the event then; the datamodel keeps its `_event`. SCXML keeps `_event` bound to the last event until another is processed (SCXML 5.10): the `<onexit>` handlers of an actor terminated at rest read it, and `serialize` writes it. So an actor at rest holds its last event in the language (one Event and one reference on its data row), not in core.
- **`release` can run the language’s code** (a finalizer, a weakref callback), and core calls it outside a run as well: at the start of `ta_host_run`, in the ack, in `ta_host_free`, in posted messages and in deliver. Core holds the run's rules there (section 5), and the binding still defers dropping the last language reference until the outer call into core returns, so no language code runs inside core's bookkeeping.
- **Actors share no mutable value.** Inside a session a variable may hold any of the language’s objects. What crosses to another actor (captured `<send>` data, `<donedata>`, data sent from outside) is copied through the value table; immutable values may be shared. A mutable object outside the table fails the capture with `error.execution`; a session holding one is not hibernated (`serialize` answers `TA_ERROR_UNSUPPORTED`). Never serialize with a format that runs code when read (pickle, Java serialization).
- **Say what the language cannot bound.** A runtime that cannot be preempted mid-expression or limited in memory (CPython, unlike quickjs with `max_ops` and its allocator) makes its charts trusted code, and its values lie outside the host’s allocator. Charge fuel once per evaluation, and write both limits in the binding’s README.
- **Conformance:** translate the W3C suite’s expressions into the language (the charts unchanged) and run it on a host with the native datamodel as well as on one with ECMAScript.

## 9 · Values, the CBOR module, and other systems

- **Stream, in the extension’s C.** Read with `ta_cbor_reader`, a cursor over a view that validates as it reads and needs no memory, building the language’s values item by item; write with `ta_cbor_writer`, one call per item, into a room that grows through an allocator (section 6). A call per item costs nothing in C, and in a wasm build the conversion stays inside the module (ADR 0010). The token-list calls (`ta_cbor_decode`, `ta_cbor_member`) are for random access, as core’s request parsing uses them; a binding does not need them.
- **The value table** is [protocol.md, Values](protocol.md#values): integers, floats, text, bytes, arrays, maps with text keys, `false`, `true`, `null`, `undefined`, and tag 32 for an actor reference. Write down the language’s side of it. Where the language has no second empty value (Python’s only `None`), add a singleton for `undefined`.
- Integers are exact to 64 bits: use the language’s big integers where its numbers are doubles, and check a double before converting it (Node-API’s `napi_get_value_int64` saturates silently). Text is valid UTF-8: a lone surrogate is refused (`TA_ERROR_INVALID`), never replaced (Node-API replaces it with U+FFFD silently). Where a boolean is an integer (Python’s `bool`), test for it first. Byte strings are bytes, not base64. Tag 32 is an actor reference: where the language’s datamodel has a reference type, make it that object, which writes tag 32 again (Python’s `Address`; `values.txt`, “not ECMAScript” cases); where it has none, as in ECMAScript (Node’s datamodels both), read it as its address text and never write it ([protocol.md, Values](protocol.md#values); `values.txt`, “ECMAScript: an actor reference is its address”). `null` and `undefined` stay distinct. Writers use the shortest form of every integer, length and float; map keys stay in the order written.
- **Other systems** ([protocol.md, Other systems](protocol.md#other-systems)): every send to another system's address is a `TA_EFFECT_SEND` for the host's transport (`io_processor` 0, the SCXML processor; its `ORIGIN` field is the sender's address), which needs no config; the binding gives it to the application as a host handler (Python's `transport=`), the application's transport, and fails each such send when the application gave none. A send between systems is one message in two halves, with the same shape in every binding: the outgoing half is a value of the value table holding the fields of `ta_event_in` (`target`, `event`, `data`, `sendid`, `origin`, `traceparent`; Python's `send.message`), and the incoming half is the input that takes that value unchanged and ignores keys it does not know (Python's `host.send(message)`): `ta_host_resolve` the local target, make the data a payload of that session's datamodel, and `ta_host_send` with every field. A failure is never a key of a message: the binding offers an explicit input for it (Python's `host.fail(address, sendid)`, `TA_EVENT_IN_ERROR_COMMUNICATION`), which the application calls when it believes the other side. `target` and `origin` are the language's address value, whose system, session and name `ta_address_parse` reads, so no binding or application parses the grammar. A send the transport took needs no answer; one it cannot deliver is failed with `TA_EVENT_IN_ERROR_COMMUNICATION`. How the systems are connected (threads and a queue, a socket, a database) and how values are encoded on the wire are the application's; the CBOR module is one way.
- **The CBOR module may be exposed** as the language's own `encode` and `decode` (Python's `tinyactors.encode(value) -> bytes` and `tinyactors.decode(data) -> object`, Node's `encode` and `decode` exports): the same value table and the same streaming calls (`ta_cbor_writer`, `ta_cbor_reader`), not a second codec, each with its row in the mapping table. The binding's suite adapter frames the driver's calls with it and imports no other codec (section 12). The vectors then cross the binding's codec both ways, so the binding's own tests of its value table catch a value mapped wrong the same way in both directions.

## 10 · The control plane: typed methods over posted messages

- **Everything rarely called is a method posted with `ta_host_post`:** trace configuration, watches, stats, the effective config, inspection, state values, a session’s variables and decoded spans. The catalogue is [requests.md](requests.md): every method and notification, its params, defaults, replies and errors. An API item whose method is not there does not exist; what is not a method is listed there too.
- **The shape is JSON-RPC 2.0’s, without its `"jsonrpc"` member, in CBOR:** `{"method", "params", "id"}` in. The reply is an effect, `TA_HOST_EFFECT_REPLY`, the last one the post appends: its `WIRE` is `{"id", "result"}` or `{"id", "error": {"code", "message", "data": {"reason", "path"}}}`. A message without `"id"` is applied, with no reply and no error reported. The host’s notifications are `TA_HOST_EFFECT_NOTIFICATION` effects (section 4); none is defined.
- **No synchronous request, and response handlers** (ADR 0012, decision 3). The language API has one **typed method per method of requests.md that it has no direct call for** (`host_stats`, `actor_state(session)`, `actor_context(session)`, `trace_watch(rules, seconds)`, named by the language’s conventions), each a few lines over the post. `actor.send` is the binding’s `send` (`ta_host_resolve`, then `ta_host_send`), applied at the call: the method is for a client outside the process, so the binding has no typed method for it. It takes the method’s params, a **response handler** and an optional error handler, and returns nothing: it builds `params` with the CBOR module in the call’s arena (section 6), adds a row `{handler, on_error}` to the binding’s **pending table** (a `ta_table`), posts with **the row’s handle as the message id**, and returns. A refusal at the call (`ta_host_post` answering `TA_ERROR_NO_MEMORY` or `TA_ERROR_STATE`) removes the row and raises at once. The drain that handles the `REPLY` reads its `id` in C, finds the row with `ta_table_get` (O(1), the generation refusing a stale id), removes it, and calls the handler with the host and the decoded `result` (converted once, section 6), or the error handler with the `error` as the language’s exception with its `code`, `reason`, `path` and `message` unchanged. An error with no error handler is raised by the driver after the turn, as a handler’s exception is (section 3). Match on `code` and `reason`, never on `message`. A typed method called from a handler during a drain is answered in a later take of the same turn. There is no reply object, no future and no awaitable result: the handler is the one place a reply goes. Pending handlers are dropped, never called, at `ta_host_free`.
- **No raw `post` in the API** until a JSON-RPC transport exists (ADR 0012). The binding’s own C posts where it needs to (`host.config` at creation, section 3).
- `TA_ERROR_NO_MEMORY` from `ta_host_post` means the host’s allocator refused room for the reply, and nothing was applied and no reply appended; `TA_ERROR_STATE`, a post during a run or from a datamodel’s code.
- **A JSON-RPC transport** (a debugger, an admin socket), when one exists, is a pipe: frames to `ta_host_post`, the `WIRE` of each `REPLY` and `NOTIFICATION` to the socket, in the order the takes give them. A JSON-RPC batch is split into messages and its array assembled from the replies.

## 11 · Project and build

- A project per runtime at the repository root, named for it (`tinyactors-napi`, `tinyactors-cpython`), with its own `mise.toml` pinning the toolchain, so nobody else downloads it.
- **The conformance suite is shared, not code.** `tinyactors-test` holds the tests that are data (protocol vectors, self-checking scenarios, W3C) and runs them against one command per binding (`ROADMAP.md`, step 8). The binding’s own tests cover what the suite cannot see: memory failures, refusal during a run, threads, garbage collection, liveness.
- **Our names carry the binding’s prefix** (`ta_python_*`, `ta_napi_*`); the platform’s names appear only where it requires them.
- Liveness and timer accuracy are the runtime’s: whether a pending timer keeps the process alive, and how late a throttled timer fires (a hidden browser tab), are I/O behaviour, documented per runtime (Node: alive while a deadline is pending), not a policy. A late timer only makes delayed sends late; the next turn runs everything due, in order.
- Link the release’s libraries (the amalgamation, or the release sources compiled into a wasm module for browsers). The binding has no C of its own beyond its addon.
- **Wasm:** memory growth detaches every view, so the loader refreshes its views after any call that can allocate, and nothing else keeps one. Memory never shrinks: hibernation makes room for other actors, not a smaller footprint, so size for the peak. The page’s Content Security Policy can forbid `new Function`: offer the ECMAScript datamodel in a separate module for a strict policy, so pages that do not need it do not download it.
- Tasks as Ruby file tasks under `mise/tasks/`; its `test` task lists core’s sources in `#MISE sources` so a core change reruns it.
- Repository rules apply: acronyms in capitals or lowercase, at most three parameters for every C function the binding defines, static helpers included (a function whose signature a platform typedef fixes is exempt by that typedef), no serialization on the hot path, a `CHANGELOG.md` per released component, commits scoped by component.

## 12 · Testing

A binding is tested by the conformance suite, [tinyactors-test](../../tinyactors-test/README.md), run against its adapter: one command over the binding’s public API that speaks the [driver protocol](../../tinyactors-test/docs/driver.md) on its standard input and output (`mise run //tinyactors-test:run -- <command…>`). The adapter implements the methods; a method it does not offer answers -32601 and the cases that need it are skipped, not passed.

- **The protocol vectors** ([suites/protocol](../../tinyactors-test/suites/protocol/README.md)): `requests.txt`, `config.txt` and `values.txt`, byte-exact. The adapter takes each case’s bytes, sends them through the binding’s own calls into core (a typed method that takes the language’s values decodes them first and encodes the reply again, as its API does; the notifications its API delivered after a run are encoded again for `host.run`, [driver.md](../../tinyactors-test/docs/driver.md)), and the runner compares the answer with the expected bytes, where tag 39999 is a wildcard for one item of the named type. The `roundtrip` and `malformed` cases of `values.txt` go through the public path: the value is given to the echo setup's chart as the data of an event from another system (`actor.send` with `cbor` and `origin`), which sends `_event.data` back to `_event.origin`, out through the host's transport, and the data of that send is compared, so a binding's datamodel passes them with no method of its own; a datamodel in another language passes all but those named “ECMAScript: …”, an ECMAScript one all but those named “not ECMAScript: …”.
- **The scenarios** ([suites/scenarios](../../tinyactors-test/suites/scenarios/README.md)): core’s conformance scenarios as charts that check themselves, in the W3C IRP’s notation, and end in `pass` or `fail`. A `steps.txt` lists the host operations between runs (spawn, send, the clock, hibernation, snapshots, a new image, migration). The adapter runs them with the binding’s real loop and effect handlers in the environment the driver protocol describes (“The scenario environment”), and answers the first `Outcome` log. No effect is compared.
- **The W3C suite** runs through the adapter (`w3c.run`), with each datamodel the binding offers; every automatic test must pass except those on its language's skip list (`skip-<language>.txt`, each with a reason). Its environment has BasicHTTP as a synchronous I/O processor, which 12 tests use (201, 509, 510, 518, 519, 520, 522, 531, 532, 534, 567, 577): the binding's loop answers `TA_RUN_EFFECTS` (section 3).
- **The differential run**: the same scenarios and W3C suite run on the binding’s native datamodel and on quickjs, and both pass. No outcome or effect is compared beyond that.
- What a black box cannot show stays in the binding’s own tests: memory failures, refusal during a run, threads, garbage collection, liveness.
- Tests run in parallel: listen on port 0, write to temporary directories, depend on no other test.
- A test that compares timings runs alone.

## 13 · Done means

- The mapping table exists, and every API item has a core function or method behind it.
- The queue loop handles every run status and every effect in section 4, one drain at a time.
- Every method of requests.md but `actor.send` (the binding’s `send`) has its typed method, answered through its response handler at a later drain; no synchronous request and no raw post.
- What core hands the language is a thin view where section 6 says so, and stale after the ack.
- The suite passes against the binding's command: protocol vectors, scenarios and W3C, on every datamodel it offers.
- No defaults, limits, methods or messages of the binding’s own.
- Nothing C does is done again in the language.
