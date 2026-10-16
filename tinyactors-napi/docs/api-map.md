# The mapping table: the JavaScript API and core

Each item of `@tinyactors/node`'s API next to what core has behind it
(tinyactors-core/docs/bindings.md, section 2: "An item with no core
function behind it does not belong"). Written before the item, in the
same commit, and kept in step with `index.d.ts`.

**The rule:** every row's Core cell names, in backticks, at least one of

- a function of core's API descriptions
  (`tinyactors-core/include/*.api.json`): `ta_host_send`;
- a constant of them: an effect (`TA_EFFECT_LOG`, `TA_HOST_EFFECT_PEER`),
  a run status (`TA_RUN_IDLE`), a flag;
- a request method of `tinyactors-core/docs/requests.md`: `actor.context`;

and nothing else in backticks. `mise run lint` checks it
(`mise/lib/api_map.rb` at the repository root): a name core does not have
fails. Say in Notes how the item uses them: the turn, a request, the
handling of an effect.

A core function no item uses is listed below the table: used internally
(say where) or not exposed (say why).

A row marked *planned* is an item the plan (`docs/plan.xml`, section
api) calls for, written ahead so its core is checked and the API review
sees it; the commit that adds the item removes the mark. `index.d.ts`
declares every row, planned or not; `index.mjs` exports the items
built so far. Names the plan left open were picked when the
rows were written (log entries `l-p0-map-*`, for the maintainer).

| API item | Core | Notes |
|---|---|---|
| `compileCharts(sources, resolver?) → Compiled` | `ta_compile_charts`, `ta_compile_diagnostics`, `TA_DIAGNOSTIC_ERROR` | the compiler's call; `sources` an array of `{name, text}`, the first the default boot chart; `resolver` the compile-time resolver, the only place a resolver is given; an error throws `CompileError` with every diagnostic, the result freed first |
| `compileSystem(manifest, resolver?) → Compiled` | `ta_compile_system`, `ta_compile_diagnostics`, `TA_DIAGNOSTIC_ERROR` | a `<system>` manifest (`{name, text}`): its charts, read through `resolver`, and the boot actors `host.boot` spawns; the same `Compiled` and `CompileError` as `compileCharts` |
| `compiled.image`, `compiled.diagnostics` | `ta_compile_image`, `ta_compile_diagnostics` | `image` a `Buffer` copied once from the result on first read (`napi_create_buffer_copy`) and cached, the application's from then on; a first read after `close()` throws `TinyactorsError` with `TA_ERROR_STALE`; `diagnostics` the warnings and notes, converted once |
| `compiled.close()`, `compiled[Symbol.dispose]()` | `ta_compile_free` | frees the result and removes its wrap; a second call does nothing; the finalizer as a fallback; a copy of `image` already read lives on with the collector (no export count: nothing holds the result's memory) |
| `CompileError`, `Diagnostic` | `ta_compile_diagnostics`, `TA_DIAGNOSTIC_ERROR`, `TA_DIAGNOSTIC_WARNING`, `TA_DIAGNOSTIC_NOTE` | a `TinyactorsError` (status `TA_ERROR_INVALID`) whose `diagnostics` are every diagnostic: `severity` (`"error"`, `"warning"`, `"note"`), `code`, `file`, `line`, `column`, `message` |
| `resolver(base, source) → string`, `new ResolveError(message, {line, column}?)` | `ta_compile_charts`, `ta_compile_diagnostics` | any callable, as `ta_compile_options.resolve`, given to the compile calls only (a static `src` at compile time); a `ResolveError` becomes a diagnostic of the resolved file at its line and column, the message's UTF-8 in the compilation's scratch arena; a `SyntaxError` is not converted (no JavaScript is compiled at resolve time); any other error is thrown from the compile call after the result is freed |
| `new Host(config, options?)`: `ioProcessors`, `log`, `clock`, `telemetry`, `wake`, `migrated`, `services`, `migrationDone`, `transport`, `datamodel`, `quickjs` | `ta_host_create`, `host.config`, `ta_datamodel_ecmascript_new`, `ta_datamodel_ecmascript_binding` | the config (a plain object) as CBOR in the host's arena; a refused config throws `TinyactorsError` with core's error; `host.config` posted once at creation for `system.ticksPerSecond` and the I/O processors' indices; `datamodel` `"native"` (the default, the maintainer's exception) or `"quickjs"`, the one ECMAScript datamodel in the environment's list; `quickjs` the sandbox's `maxOps`, `memoryLimit`, `stackLimit` and `realm`, refused on a native host; an unknown option, a handler that is not a function, or an `ioProcessors` type the config lacks throws `TypeError`; the handlers kept on the Host's own object under a symbol, never by a strong reference |
| `options.quickjs`: `{maxOps, memoryLimit, stackLimit, realm}` | `ta_datamodel_ecmascript_new`, `TA_DATAMODEL_ECMASCRIPT_REALM_AUTO`, `TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED` | *planned*; the sandbox's limits, passed through as `ta_datamodel_ecmascript_config`'s `max_ops`, `memory_limit` (bytes), `stack_limit` (bytes) and `realm` (`"auto"` or `"isolated"`), each absent field core's default (0); given with `datamodel: "native"` it throws `TypeError` at creation |
| `host.quickjsHeapBytes` | `ta_datamodel_ecmascript_heap_bytes` | *planned*; the quickjs engine's heap in bytes, read on each access (O(1)), for an operator watching a sandboxed host against `memoryLimit`; `undefined` on a native host |
| `host.ticksPerSecond`, `host.closed` | `host.config`, `ta_host_free` | read once at creation; whether the host was freed (as Python's) |
| `host.close()`, `host[Symbol.dispose]()`, `host[Symbol.asyncDispose]()` | `trace.end`, `ta_host_traces`, `ta_host_logs`, `ta_host_effects_ack`, `ta_host_free` | frees the host (`ta_host_free`), its tables and its datamodel, and drops the handlers; a second call does nothing; the stop of bindings.md section 4: the timers cleared, a waiting `idle()` rejected with `TA_ERROR_STATE`, and with a telemetry handler and an encoder `trace.end` ends the open spans and the last take (bounded) goes to it (dropped until the telemetry handler is called, *planned*); pending response handlers dropped, never called; from a handler, deferred to the end of the turn; `asyncDispose` also awaits the promises the last take's handlers returned |
| `host.addImage(image) → number` | `ta_host_add_image` | any `Uint8Array` or `ArrayBuffer` (`compiled.image`, a `.tai` file read), copied by the host during the call; returns the segment |
| `host.chart(name) → ChartRef`, `ref.segment`, `ref.chart` | `ta_host_chart` | the current version of chart `name`: a frozen object of its segment and chart index |
| `host.boot(segment)` | `ta_host_boot` | the segment's boot actors (`<system><actor>`s, or the first chart compiled), each under its name |
| `host.spawn(ref, {name, params, traceparent}?) → bigint` | `ta_host_spawn` | an input, applied at the call, the clocks advanced first; `ref` from `host.chart` (any object of `segment` and `chart`); `params` a payload of the chart's datamodel, moved on success, released on refusal; `traceparent` W3C text, its UTF-8 for the call, parsed by core; the session always a `bigint` |
| `host.send(target, event, data?, {sendid, traceparent, origin}?)`, `host.send(message)` | `ta_host_resolve`, `ta_host_send` | an input, applied at the call; `target` a session (`bigint`), or a local address (a string or an `Address`) core resolves first; `data` a payload of the target session's datamodel; `traceparent` parsed by core (none, or text that does not parse: a new trace); `origin` another system's sender (a string or an `Address`); `send(message)` takes the fields of an object `send.message` gave (`target`, `event`, `data`, `sendid`, `origin`, `traceparent`), ignoring other keys; options other than `sendid`, `traceparent` and `origin` throw `TypeError`; data a chart whose datamodel takes none throws `TypeError`; what `actor.send` does for a client outside the process, so it has no typed method |
| `host.fail(target, sendid?)` | `ta_host_resolve`, `ta_host_send`, `TA_EVENT_IN_ERROR_COMMUNICATION` | an input: a send the transport could not deliver, reported to its sender (a session, or its local address as a string or an `Address`) with the sendid; core raises `error.communication` in the sender |
| `host.sendMany(items) → number` | `ta_host_send_many` | inputs, applied at the call; each item an object of `host.send(message)`'s fields, the `ta_event_in` array in the call's arena; returns the length of the accepted prefix; handed to core in slices of 256, so the arena holds a slice, not the array; an item that cannot be converted throws, the slices before it sent |
| `host.terminate(session)` | `ta_host_terminate` | an input, applied at the call |
| `host.actorByName(name) → bigint or undefined` | `ta_host_actor_by_name` | the live actor spawned with `name`, newest segment first; `undefined` for core's 0 |
| the turn, with no driver; `await host.idle()` | `ta_host_effects`, `ta_host_effects_ack`, `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE`, `TA_RUN_YIELDED`, `TA_RUN_EFFECTS`, `TA_RUN_EFFECTS_FULL`, `TA_RUN_EXHAUSTED`, `TA_RUN_FAULT` | the event loop drives every host: an input outside a turn schedules one turn (`setImmediate`, one pending), a turn at its bound (64 runs of 10000 fuel, a drain of 64 batches) yields the same way, an idle turn arms one ref'd `setTimeout` at the deadline (at most 2^31-1 ms; the Host its argument), replacing the last; both clocks before every run, and after it with an encoder; `idle()` a promise settled at the next idle turn, the same to every caller until then, rejected with the turn's first error (an exhausted pool as `TinyactorsError` with `TA_ERROR_NO_MEMORY` naming the pool, a fault as `TinyactorsError` with its status) or with `TA_ERROR_STATE` by `close()`; with none waiting the error is thrown from the turn's callback, after the next turn is scheduled or the timer armed; refused on a closed host |
| `host.nextDeadline` | `ta_host_run`, `TA_RUN_IDLE` | the last idle run's next deadline as milliseconds from now on the host's clock; `undefined` for none (`UINT64_MAX`) |
| `new SimulatedClock()`, `clock.increment(ms)`, `clock.now` | `ta_host_advance_time`, `ta_host_run` | a clock in milliseconds (nanoseconds inside, exact) given as `options.clock`, its time the host's monotonic and wall clocks: `increment` (a number, not negative; `RangeError` otherwise) moves it and schedules a turn on every open host made with it, turning nothing itself; it holds the hosts weakly (`WeakRef`); such a host arms no timer; `options.clock` that is not one throws `TypeError` |
| `options.ioProcessors`: `{[type]: (host, send) => ...}` | `TA_EFFECT_SEND`, `TA_EFFECT_FIELD_IO_PROCESSOR`, `ta_host_send`, `TA_EVENT_IN_ERROR_COMMUNICATION` | the handling of an effect (`IO_PROCESSOR` `i + 1` for the config's `ioProcessors[i]`); `send` a thin view until the ack (a read after it throws `TA_ERROR_STALE`); without a handler the send is failed; a handler returning a thenable has its view filled with every field before the ack, and its `answer()` and `fail()` work while the host lives; one that throws, or whose promise rejects before an answer, has its send failed |
| `options.transport`: `(host, send) => ...` | `TA_EFFECT_SEND`, `TA_EFFECT_FIELD_IO_PROCESSOR`, `TA_EVENT_IN_ERROR_COMMUNICATION` | the handling of an effect: a send to another system's address (`IO_PROCESSOR` 0, the SCXML processor), as for an I/O processor, `send.type` `undefined`; without a handler each such send is failed |
| `send.session`, `send.event`, `send.target`, `send.sendid`, `send.data`, `send.type`, `send.origin` | `ta_host_effect_number`, `ta_host_effect_text`, `TA_EFFECT_FIELD_SESSION`, `TA_EFFECT_FIELD_EVENT`, `TA_EFFECT_FIELD_TARGET`, `TA_EFFECT_FIELD_SENDID`, `TA_EFFECT_FIELD_PAYLOAD`, `TA_EFFECT_FIELD_ORIGIN`, `ta_datamodel_ecmascript_payload_to_cbor` | fields read on first access and cached; `data` the native row's value itself, or quickjs's payload decoded; `origin`, and the transport's `target`, an `Address`; `type` the processor's type from the config (`undefined` for the transport); `target` and `sendid` `undefined` when empty; a read after the ack throws `TA_ERROR_STALE` |
| `send.traceparent` | `ta_host_effect_text`, `TA_EFFECT_FIELD_TRACEPARENT` | the sending step's trace, formatted from the effect's binary context on first read; `undefined` when untraced (empty text) |
| `send.answer(event, data?, {traceparent, origin, raw}?)` | `ta_host_send` | to `SESSION`, with the send's `sendid`, `origintype` the processor's type, and the send's trace unless `traceparent` is given (`null`: a new trace); `origin` (a string or an `Address`) and `raw` are `ta_event_in`'s, and core gives the receiver an `origintype` only with an `origin`; once only: a second answer or a fail after it throws `TA_ERROR_STATE` |
| `send.fail()` | `ta_host_send`, `TA_EVENT_IN_ERROR_COMMUNICATION` | with the send's `sendid` and `send.traceparent`; core raises `error.communication` in the sender; recorded, so a later rejection fails nothing again |
| `send.message` | `ta_host_effect_text`, `TA_EFFECT_FIELD_ORIGIN`, `TA_EFFECT_FIELD_TRACEPARENT` | the send's outgoing half as an object, the value `host.send(message)` takes on the other side: `target` (an `Address` for the transport), `event`, `data`, `sendid`, `origin` (an `Address`), `traceparent` |
| `options.log`: `(host, entry) => void`, `entry.session`, `entry.label`, `entry.text` | `TA_EFFECT_LOG`, `ta_host_effect_text`, `ta_host_effect_number`, `TA_EFFECT_FIELD_AUXILIARY` | the handling of an effect; `entry` a thin view until the ack (`label` from `AUXILIARY`, `text` from `EVENT`); without a handler the line is discarded, no console default |
| `host.actorState(params, onResult, onError?)` | `actor.state`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method: `params` an object passed to core as given (`undefined` for none; anything else core refuses in the reply), the clocks advanced and a turn scheduled; `onResult(host, result)` at the `REPLY`'s dispatch in a later drain, the result converted once; `onError(host, error)` (or `null`/absent: the error rejects the turn's `idle()`) with a `TinyactorsError` of `code`, `reason`, `path` and `message`, no `status`; never a param; returns nothing; the message id is a pending row's handle (`ta_table`), read from the reply in C, a stale one dropped; a refusal at the call (`TA_ERROR_NO_MEMORY`, `TA_ERROR_STATE`) throws and leaves nothing pending; pending handlers are dropped, never called, at `close()` |
| `host.actorContext(params, onResult, onError?)` | `actor.context`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `host.actorList(params, onResult, onError?)` | `actor.list`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `host.actorGet(params, onResult, onError?)` | `actor.get`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `host.hostStats(params, onResult, onError?)` | `host.stats`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `host.hostConfig(params, onResult, onError?)` | `host.config`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `host.traceConfigure(params, onResult, onError?)` | `trace.configure`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `host.traceWatch(params, onResult, onError?)` | `trace.watch`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method; the host's clock advanced before the post (a watch ends at a time of it) |
| `host.traceUnwatch(params, onResult, onError?)` | `trace.unwatch`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `host.traceEnd(params, onResult, onError?)` | `trace.end`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `host.traceSpansDecode(params, onResult, onError?)` | `trace.spans.decode`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `options.telemetry`: `(host, signal, data) => ...` | `TA_HOST_EFFECT_TRACES`, `TA_HOST_EFFECT_LOGS`, `ta_host_traces`, `ta_host_logs` | *planned*; the handling of an effect: the request taken when its effect comes and copied once into a `Buffer`, `signal` `"traces"` or `"logs"`; without a handler the request is taken and dropped |
| `host.metrics() → Buffer` | `ta_host_metrics` | *planned*; pulled when the application wants them, copied once; outside a drain its batch is acknowledged at once, from a handler it joins the open one; `TA_ERROR_STATE` without telemetry in the config |
| `host.sweep(maxScan, limit) → bigint[]` | `ta_host_sweep` | *planned*; the CLOCK sweep: at most `limit` sessions that did not run since the previous sweep and can be hibernated now, scanning at most `maxScan` slots |
| `host.hibernate(session) → Buffer` | `ta_host_hibernate`, `ta_datamodel_ecmascript_last_error` | *planned*; the actor's blob copied once; outside a drain its batch is acknowledged at once, from a handler it joins the open one; `TA_ERROR_STATE` when the actor is not at a stable point, `TA_ERROR_UNSUPPORTED` for a value the datamodel cannot store (on quickjs the message names the variable) |
| `host.wake(session, blob)` | `ta_host_wake` | *planned*; the blob `hibernate` gave, read during the call; the actor's waiting events run at the next turn |
| `options.wake`: `(host, session, migration) => ...` | `TA_EFFECT_WAKE`, `TA_EFFECT_FIELD_MIGRATION` | *planned*; the handling of an effect: an event or a delayed send came for a hibernated session (`migration` `undefined`), or a migration asks for it (`migration` its id); the handler loads the blob and calls `host.wake`, now or later; without one the effect is dropped |
| `host.migrate({chart, session, wake, timeout}?) → number` | `ta_host_migrate`, `TA_HOST_MIGRATE_WAKE` | *planned*; starts a migration of every chart, of `chart` (a name) or of one `session` to the current version in later runs, hibernated ones asked for with `wake`, until `timeout` milliseconds of the host's clock (ticks, rounded up); returns core's id; schedules a turn |
| `options.migrationDone`: `(host, result) => void` | `TA_HOST_EFFECT_MIGRATION_DONE`, `ta_host_migration_pending` | *planned*; the handling of an effect: `result` is `{migration, migrated, failed, pending}`, the pending sessions (`bigint`s) read at the dispatch; a handler of the host, so a restored host's migrations reach it; without one nothing is read |
| `options.migrated`: `(host, migrated) => void` | `TA_HOST_EFFECT_MIGRATED` | *planned*; the handling of an effect: a thin `Migrated` view (`session`, `segment`, `chart`: its name, `migration`) per actor moved, until the ack; without a handler nothing is read |
| `host.snapshot() → Buffer` | `ta_host_snapshot_pack`, `ta_datamodel_ecmascript_snapshot`, `ta_table_snapshot` | *planned*; the whole host as one blob labelled `tinyactors-napi`, copied once: every segment, the images, the system id, the host's time and the datamodel's state (the native tables' namespaces and rows, or quickjs's); refused from a handler, with an effect or reply pending, or during a migration (`TA_ERROR_STATE`), and for a value outside the value table (`TA_ERROR_UNSUPPORTED`) |
| `Host.restore(snapshot, config, options?) → Host` | `ta_host_snapshot_unpack`, `ta_host_snapshot_image`, `ta_host_restore`, `ta_datamodel_ecmascript_restore`, `ta_table_restore` | *planned*; a new host running the snapshot, its datamodel's handles valid again before the first run, `options` as `new Host`'s; a pack another program labelled is refused; its clock goes on from the snapshot's time |
| `options.services`: `{[type]: (host, invocation) => service}` | `TA_EFFECT_INVOKE` | *planned*; the handling of an effect: the handler of the `<invoke>`'s type (`EVENT`) gets an `Invocation` record (`session`, `invokeid` from `SENDID`, `type`, `src` from `TARGET`, `data`, `traceparent`), made before the ack, and returns the service, kept by (`SEGMENT`, `TOKEN`) unless it called `done`; no handler, or one that throws: `ta_host_send` with `TA_EVENT_IN_ERROR_COMMUNICATION` to the invoker and the error reported after the turn |
| `invocation.sendBack(event, data?, {traceparent}?)` | `ta_host_send`, `TA_EVENT_IN_FROM_INVOKE` | *planned*; an event from the service to its invoker with the invocation's token; `TA_ERROR_STALE` once it ended |
| `invocation.done(data?, {traceparent}?)` | `ta_host_send`, `TA_EVENT_IN_INVOKE_DONE` | *planned*; the service's end: `done.invoke.<invokeid>` with `data` for the invoker; the service is dropped |
| `invocation.restored` | `TA_EFFECT_FIELD_FLAGS`, `TA_EFFECT_FLAG_RESTORED` | *planned*; the INVOKE is a restored host's announcement of an invocation that was running at the snapshot; no handler for its type: `TA_EVENT_IN_ERROR_COMMUNICATION` to the invoker, nothing thrown |
| `service.receive(host, send)` | `TA_EFFECT_SEND`, `TA_EFFECT_FIELD_IO_PROCESSOR`, `TA_NIL` | *planned*; the handling of an effect: a send with `IO_PROCESSOR` `TA_NIL` for the service of (`SEGMENT`, `TOKEN`), as a thin `Send` view whose `type` is `undefined`; to a service that failed or crashed, or one whose `receive` throws: `ta_host_send` with `TA_EVENT_IN_ERROR_COMMUNICATION` and the send's sendid |
| `service.stop(host)` | `TA_EFFECT_UNINVOKE` | *planned*; the handling of an effect: the invoker cancelled the invocation; the service is dropped |
| `TinyactorsError` (`message`, `code`, `reason`, `path`, `status`) | `ta_host_post`, `TA_HOST_EFFECT_REPLY`, `ta_status_name` | an `Error` subclass, `new TinyactorsError(message, {code, reason, path, status}?)`, each field read-only and `undefined` when not given; a reply's error (`code`, `reason`, `path`, `message` as core wrote them), a refusal at a call, a fault, a stale view; `status` a bare status named by `ta_status_name` (`"TA_ERROR_STALE"`) |
| `encode(value) → Uint8Array`, `decode(bytes) → value` | `ta_cbor_writer_init`, `ta_cbor_reader_init`, `ta_cbor_read`, `ta_cbor_writer_bytes`, `ta_cbor_writer_free` | the one conversion the API uses, streaming, the writer and the call's text in an arena of the environment's instance, a mark per call; `undefined` and `null` distinct, integers beyond 2^53 `bigint`; tag 32 read as its address string and never written; what it cannot read or write throws a `TinyactorsError` (`TA_ERROR_UNSUPPORTED` for a value outside the table, `TA_ERROR_INVALID` for one it cannot hold or malformed CBOR); `decode` takes a `Uint8Array` or an `ArrayBuffer`, anything else a `TypeError` (docs/values.md) |
| `new Address(text)`, `address.text`, `address.toString()`, `address.system`, `address.session`, `address.name` | `ta_address_parse`, `ta_address_name_decode` | immutable; the parts read by core's parser on each access, `session` a `bigint`, `name` decoded; `undefined` for a part the text lacks, or when the text is no address; `toString()` the text |
| `Address.fromSession(system, session)`, `Address.fromName(system, name)` | `ta_address_format`, `ta_address_format_name` | an `Address` made from its parts by core's formatter (the name percent-encoded); a system id or name core refuses throws `TinyactorsError` with `TA_ERROR_INVALID` |
| `encodeHTTPPost({url, event, fields, body}) → Buffer` | `ta_http_encode_post` | *planned*; SCXML's BasicHTTP (C.2.1): the POST request of a send, `fields` an array of `{name, value}` (or `body` when there are none), written into the call's arena and copied once; a URL that is not `http://host[:port]/path` throws `TA_ERROR_INVALID` |
| `parseHTTPRequest(message) → {method, target, body, event, fields}` | `ta_http_parse_request` | *planned*; SCXML's BasicHTTP (C.2.2): an incoming request's event name (`_scxmleventname`, else `HTTP.<METHOD>`) and its decoded form fields, as strings, the scratch in the call's arena; a malformed request throws `TA_ERROR_INVALID` |
| the native `"ecmascript"` datamodel, `_event` | `TA_DATAMODEL_EVENT_DETACHABLE`, `ta_datamodel_environment_in`, `ta_image_chart_datamodel` | the vtable of `datamodel.h` in the application's engine: `node:vm` contexts (one shared per host, one per session for a chart whose scripts declare top-level names, as the engine says at `chart_attach`), a scope per session made by its chart's constructor over a frozen prototype of the system variables, expressions compiled one at a time at their first evaluation (sloppy `with`, strict inside: an undeclared assignment, a write to a system variable or to `_event` is `error.execution`), values crossing by copy into the receiving context, `_event` frozen and kept until the next `bind_event` or `destroy` (across `detach_event`), XML content a DOM through txml; hibernation, migration and snapshots *planned* (`TA_ERROR_UNSUPPORTED` until then) |

## Every public function of core, in one of three lists

The inventory of core's public headers (`tinyactors-core/include/tinyactors/*.h`,
every `TA_API` function, as `include/*.api.json` lists them), taken
2026-10-16: each function is behind an API item, used internally, or not
exposed, with the reason. The API items are those of `docs/plan.xml`
(section api), whose rows the table above gets marked *planned*. A
function decided here for the API review is marked so; its decision is in
`docs/log.xml` (kind decision, for the maintainer).

### Behind an API item

- **Compiling** (`compileCharts`, `compileSystem`, `Compiled`, the
  resolver): `ta_compile_charts`, `ta_compile_system`,
  `ta_compile_image`, `ta_compile_diagnostics`, `ta_compile_free`.
- **The `Host`, its images and inputs**: `ta_host_create`,
  `ta_host_free` (`close`, the dispose symbols, the finalizer),
  `ta_host_add_image`, `ta_host_chart`, `ta_host_boot`,
  `ta_host_actor_by_name`, `ta_host_spawn`, `ta_host_send`,
  `ta_host_resolve` (a local address as `send`'s target),
  `ta_host_send_many`, `ta_host_terminate`.
- **The turn** (`idle()`, `nextDeadline`, `SimulatedClock`):
  `ta_host_effects`, `ta_host_effects_ack`, `ta_host_advance_time`,
  `ta_host_run`.
- **Effects as views** (the I/O processors' and transport's `send`, the
  log `entry`, `Migrated`, `Invocation`): `ta_host_effect_number`,
  `ta_host_effect_text`.
- **Typed methods** (`actorState` and the others) and their replies:
  `ta_host_post`; the reply's result read from `ta_host_effect_bytes`.
- **Telemetry and outputs**: `ta_host_traces`, `ta_host_logs`,
  `ta_host_metrics` (`metrics()`).
- **Hibernation**: `ta_host_sweep`, `ta_host_hibernate`, `ta_host_wake`.
- **Migration**: `ta_host_migrate`, `ta_host_migration_pending`
  (`migrationDone`'s pending sessions).
- **Snapshots** (`snapshot()`, `Host.restore`): `ta_host_snapshot_pack`,
  `ta_host_snapshot_unpack`, `ta_host_snapshot_image`, `ta_host_restore`,
  `ta_datamodel_ecmascript_snapshot` and `ta_datamodel_ecmascript_restore`
  (quickjs's state in the pack).
- **`Address`** (`.system`, `.session`, `.name`): `ta_address_parse`,
  `ta_address_name_decode`. *Decided for the API review*: an `Address`
  made from its parts, `Address.fromSession(system, session)` and
  `Address.fromName(system, name)`, over `ta_address_format` and
  `ta_address_format_name` (log `l-p0-inventory-address-format`).
- **`encode` and `decode`** (`src/values.c`, the one conversion the API
  uses, streaming): `ta_cbor_reader_init`, `ta_cbor_read`,
  `ta_cbor_writer_init`, `ta_cbor_write_map`, `ta_cbor_write_array`,
  `ta_cbor_write_end`, `ta_cbor_write_text`, `ta_cbor_write_bytes`,
  `ta_cbor_write_uint`, `ta_cbor_write_int`, `ta_cbor_write_negative`,
  `ta_cbor_write_float`, `ta_cbor_write_simple`, `ta_cbor_write_tag`,
  `ta_cbor_writer_bytes`, `ta_cbor_writer_free`.
- **`TinyactorsError`**: `ta_status_name` (a status named by core).
- **quickjs's memory, for the operator of a sandboxed host** (*decided
  for the API review*, log `l-p0-inventory-quickjs-limits`):
  `ta_datamodel_ecmascript_heap_bytes` behind `host.quickjsHeapBytes`, a
  read of the engine's heap on a `datamodel: "quickjs"` host; the limit
  itself is the Host option at creation (`options.quickjs.memoryLimit`,
  `ta_datamodel_ecmascript_config.memory_limit`), so
  `ta_datamodel_ecmascript_set_memory_limit` is not exposed (below).
- **BasicHTTP** (*decided for the API review*, log
  `l-p0-inventory-basic-http`): `ta_http_encode_post` and
  `ta_http_parse_request` behind two functions of the module,
  `encodeHTTPPost` (the POST of a send) and `parseHTTPRequest` (the event
  of an incoming request), for an application's
  HTTP I/O processor and the adapter's W3C BasicHTTP loopback.

### Used internally, with no item

- **The loop** (`src/loop.c`): `ta_runtime_now_ns`, the wall clock of a
  host given no clock (as Python's `loop.c`); `ta_host_snapshot` with an
  empty buffer, the quiescence check `snapshot()` makes before any
  datamodel state is built.
- **Effects** (the drain's dispatch, `src/loop.c`, until `src/effects.c`
  is built): `ta_host_resume`, the answer in C to
  `TA_EFFECT_REQUEST` with `TA_ERROR_INVALID` until run-time fetching is
  built (no JavaScript runs).
- **Inputs** (`src/host.c`): `ta_host_locate`, `ta_host_system`,
  `ta_actor_info_get` and `ta_host_segment_info_get`, to find the target
  session's datamodel, of which an input's data becomes a payload
  (`ta_image_chart_datamodel`, below).
- **The environment** (`src/host.c`): `ta_datamodel_ecmascript_new`,
  `ta_datamodel_ecmascript_free`, `ta_datamodel_ecmascript_vtable` and
  `ta_datamodel_ecmascript_binding` (quickjs in the environment when the
  Host option names it); `ta_datamodel_ecmascript_payload_to_cbor` and
  `ta_datamodel_ecmascript_payload_from_cbor` (a value to or from a
  quickjs payload); `ta_datamodel_ecmascript_last_error`, the message of
  the `TinyactorsError` that `hibernate()`, `snapshot()` or a migration's
  failure gives on quickjs with `TA_ERROR_UNSUPPORTED` (which variable
  could not be stored; log `l-p0-inventory-quickjs-limits`).
- **The arena** (`src/host.c` per host, `src/module.c` per environment for
  the module's own calls, a mark per call): `ta_growable_arena_init`,
  `ta_growable_arena_alloc`, `ta_growable_arena_position`,
  `ta_growable_arena_release`, `ta_growable_arena_allocator` (the CBOR
  writer's allocator), `ta_growable_arena_free`.
- **The tables** (`src/host.c`, `src/methods.c`, `src/datamodel/rows.c`:
  charts, sessions, payloads, loops, pending replies): `ta_table_init`,
  `ta_table_add`, `ta_table_get`, `ta_table_at`, `ta_table_handle`,
  `ta_table_remove`, `ta_table_free`; `ta_table_snapshot` and
  `ta_table_restore` for the native datamodel's part of a snapshot.
- **The native datamodel** (`src/datamodel/`): `ta_datamodel_environment_in`
  (`In()`), `ta_datamodel_environment_charge` (fuel per evaluation),
  `ta_datamodel_environment_image` and `ta_datamodel_environment_chart`;
  the chart's code and names, read from its image: `ta_image_data_count`,
  `ta_image_data`, `ta_image_expression_count`, `ta_image_expression`
  (each expression's source, compiled at its first evaluation),
  `ta_image_state_by_name` (`In()`'s argument) and `ta_image_string`
  (`<param>` names); `ta_image_chart_datamodel` in `src/host.c`, above.
  Event names and invoke ids are shared per chart by core's ids
  (`ta_event_view`), not read from the image.
- **txml, for XML content** (`src/datamodel/dom.c`; tinyxml's API,
  `tinyxml/include/txml.h`, not core's): the parser, `txml_parser_init`
  (over the host's allocator), `txml_parse_feed`, `txml_parse_step`,
  `txml_parser_doc`, `txml_parser_destroy`; the tree, `txml_doc_root`,
  `txml_first_child`, `txml_next_sibling`, `txml_kind`, `txml_flags`
  (CDATA), `txml_node_name_id`, `txml_node_prefix` and `txml_doc_name_at`
  (`nodeName`, the qualified name), `txml_node_text` (text, CDATA,
  comments, processing instructions); the attributes,
  `txml_first_attribute`, `txml_next_attribute`, `txml_attribute_read`. Nothing else of txml is
  called: `getElementsByTagName` walks the JavaScript objects already
  built, and the DOM is read-only, so no writer.

### Not exposed

- **The system layer, below the host** (`system.h`): the host drives it,
  and a binding drives the host (bindings.md, section 1).
  `ta_system_layout`, `ta_system_init`, `ta_system_relocate`,
  `ta_system_deinit`, `ta_system_boot`, `ta_system_spawn`,
  `ta_system_terminate`, `ta_system_actor_by_name`,
  `ta_system_actor_by_session`, `ta_system_actors`, `ta_system_send`,
  `ta_system_send_many`, `ta_system_resume`, `ta_system_run`,
  `ta_system_advance_time`, `ta_system_next_deadline`,
  `ta_system_effects`, `ta_system_effects_ack`, `ta_actor_watch`,
  `ta_actor_config`, `ta_system_stats_get`, `ta_system_chart_stats_get`
  (the host's `host.stats` and `actor.*` methods report them),
  `ta_system_trace`, `ta_system_trace_ack`, `ta_system_trace_clock`,
  `ta_trace_policy_default`, `ta_system_trace_set` (the host's
  `trace.*` methods), `ta_actor_hibernate`, `ta_actor_wake`,
  `ta_system_cold_actors`, `ta_actor_export`, `ta_blob_migrate`,
  `ta_system_adopt`, `ta_system_set_next_serial`, `ta_system_snapshot`,
  `ta_system_restore` (the host's hibernation, migration and snapshots).
  `ta_actor_info_get` is used internally (above).
- **The core, below the system** (`core.h`): `ta_core_instance_size`,
  `ta_core_workspace_size`, `ta_core_step`, `ta_core_deliver`.
- **The system's tracing** (`trace.h`): a traceparent crosses as W3C
  text, which core parses and formats itself (`ta_traceparent_parse` and
  `ta_traceparent_format` are called by core, not by the addon), and
  spans leave as OTLP from `ta_host_traces`. `ta_step_key_span_id`,
  `ta_trace_context_child`, `ta_traceparent_parse`,
  `ta_traceparent_format`, `ta_trace_id_format`, `ta_span_id_format`,
  `ta_trace_generator_seed`, `ta_trace_generator_next`,
  `ta_trace_ratio_threshold`, `ta_trace_sampled`, `ta_trace_kind_name`,
  `ta_trace_kind_by_name`, `ta_trace_detail_by_name` (`trace.configure`
  takes the names as params), `ta_trace_body_size`, `ta_trace_next`,
  `ta_trace_body`, `ta_trace_list`, `ta_trace_dynamic_string`.
- **The OTLP encoder's internals** (`otlp.h`): the host runs the encoder
  and hands out its requests (`ta_host_traces`, `ta_host_logs`,
  `ta_host_metrics`); decoding spans is the `trace.spans.decode` method.
  `ta_otlp_layout`, `ta_otlp_init`, `ta_otlp_relocate`,
  `ta_otlp_set_resource`, `ta_otlp_set_flags`, `ta_otlp_mark`,
  `ta_otlp_read`, `ta_otlp_end_all`, `ta_otlp_put_traces`,
  `ta_otlp_put_logs`, `ta_otlp_add_log`, `ta_otlp_add_metric`,
  `ta_otlp_add_stats`, `ta_otlp_add_span_metrics`, `ta_otlp_put_metrics`,
  `ta_otlp_stats_get`, `ta_otlp_next_span`, `ta_otlp_next_attribute`,
  `ta_otlp_next_value`, `ta_otlp_next_event`, `ta_otlp_next_link`,
  `ta_otlp_next_metric`, `ta_otlp_next_point`, `ta_otlp_next_exemplar`.
- **The hosted runtime** (`runtime.h`), core's own tools' driver of one
  system: files, a resolver that does not confine reads to a root
  (a directory resolver is the convenience layer's, in JavaScript), and a
  system runner the host replaces. `ta_runtime_malloc`,
  `ta_runtime_load_file`, `ta_runtime_free_file`, `ta_runtime_files_new`,
  `ta_runtime_files_free`, `ta_runtime_resolve`,
  `ta_runtime_compile_file`, `ta_runtime_system_new`, `ta_runtime_run`,
  `ta_runtime_system_grow`, `ta_runtime_system_free`.
  `ta_runtime_now_ns` is used internally (above).
- **The image reader beyond the datamodel's needs** (`image.h`): the
  host opens and keeps the images (`ta_host_add_image` copies them,
  `ta_host_chart` names a chart, `ta_host_boot` spawns the boot actors).
  `ta_image_open`, `ta_image_bytes`, `ta_image_chart_by_name`,
  `ta_image_state_count`, `ta_image_state_name`, `ta_image_state_info`,
  `ta_image_boot_actor`, `ta_image_ahead_of_time_section`.
- **Segments**, as Python decided (tinyactors-cpython/ROADMAP.md, "The
  API review"): `ta_host_segment_count` (`host.stats` has `segments`) and
  `ta_host_snapshot_segments` (`Host.restore` reads the images from the
  pack) (log `l-p0-inventory-segments`).
- **The XML writer** (`compile.h`): `ta_xml_writer_new`,
  `ta_xml_writer_open`, `ta_xml_writer_attribute`, `ta_xml_writer_text`,
  `ta_xml_writer_close`, `ta_xml_writer_finish`, `ta_xml_writer_free`.
  The chart builder that would use it is the convenience layer's, after
  M1 and when the maintainer picks it; the direct layer gains an item
  over it in the same change (log `l-p0-inventory-xml-writer`).
- **quickjs, beyond the environment** (`datamodel_ecmascript.h`):
  `ta_datamodel_ecmascript_set_memory_limit` (the limit is the Host
  option at creation; no need shown for changing it while a host runs),
  `ta_datamodel_ecmascript_payload_eval` (an ECMAScript expression made a
  quickjs payload, for inspection: the API's values are JavaScript
  values already), `ta_datamodel_ecmascript_js_runtime` (QuickJS's C API,
  which JavaScript cannot use), `ta_datamodel_ecmascript_wasm_set_time`
  (the wasm build only).
- **The null datamodel** (`datamodel.h`): `ta_datamodel_null`, which core
  builds in; the environment lists only the datamodels the binding adds.
- **The address parts no item needs** (`address.h`):
  `ta_address_parse_local` (core resolves a local address itself,
  `ta_host_resolve`) and `ta_address_system_valid` (core checks the
  system id in the config and in `ta_address_format`).
- **CBOR beyond the streaming codec** (`cbor.h`): `ta_cbor_decode`,
  `ta_cbor_next`, `ta_cbor_member` and `ta_cbor_string`, the tree reader
  (`values.c` streams with `ta_cbor_reader`, and nothing needs a decoded
  tree), and `ta_cbor_diagnostic` (diagnostic notation for logs and
  tests; `decode` gives values).
- **wasm** (`wasm.h`): `ta_wasm_alloc`, `ta_wasm_free`, the wasm build's
  allocator.
