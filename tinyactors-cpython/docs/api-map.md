# The mapping table: the Python API and core

Each item of the `tinyactors` module's API next to what core has behind
it (tinyactors-core/docs/bindings.md, section 2: "An item with no core
function behind it does not belong"). Written before the item, in the
same commit, and kept in step with the module.

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

| API item | Core | Notes |
|---|---|---|
| `tinyactors.compile_charts(sources, resolver=None) -> Compiled` | `ta_compile_charts`, `ta_compile_diagnostics`, `TA_DIAGNOSTIC_ERROR` | the compiler's call; `sources` (name, text) pairs, the first the default boot chart; `resolver` the compile-time resolver, the only place a resolver is given; an error raises `CompileError` with every diagnostic and frees the result |
| `Compiled.image`, `Compiled.diagnostics` | `ta_compile_image`, `ta_compile_diagnostics` | `image` a read-only memoryview over the result, valid until `close`, then `TA_ERROR_STALE`; `diagnostics` converted once |
| `with compile_charts(...) as compiled`, `Compiled.close()` | `ta_compile_free` | the collector as a fallback; `close()` while a buffer of `image` is held raises `BufferError` |
| `CompileError`, `Diagnostic` | `ta_compile_diagnostics`, `TA_DIAGNOSTIC_ERROR` | a `TinyactorsError` with every diagnostic: `severity`, `code`, `file`, `line`, `column`, `message` |
| `resolver.resolve(base, source) -> str`, `tinyactors.ResolveError(message, line=None, column=None)` | `ta_compile_charts`, `ta_compile_diagnostics` | the resolver protocol, given to `compile_charts` only (static `src` at compile time); a `ResolveError`, a `SyntaxError` or the rewrite's refusal of a `.py` file becomes a diagnostic at its line and column, the message in the compilation's scratch arena; another exception is raised from `compile_charts` after the result is freed |
| `tinyactors.Host(config, io_processors=None, *, log=None, clock=None, peer=None, telemetry=None, wake=None, migrated=None, services=None)` | `ta_host_create`, `host.config` | the config as CBOR in the call's arena (`ta_growable_arena`); a refused config raises `TinyactorsError` with core's error; the extension posts `host.config` once for `ticksPerSecond` and the I/O processors' indices; the environment registers the `"python"` datamodel and quickjs |
| `Host.ticks_per_second`, `Host.closed` | `host.config`, `ta_host_free` | read once at creation; whether the host was freed |
| `with Host(...)`, `async with Host(...)`, `Host.close()` | `trace.end`, `ta_host_traces`, `ta_host_logs`, `ta_host_effects_ack`, `ta_host_free` | the stop of bindings.md section 4: with a telemetry handler, `trace.end` ends the open spans and the last take goes to it, its exception raised once the host is freed; pending response handlers dropped, never called; from a handler, after the loop returns |
| `Host.add_image(image) -> int` | `ta_host_add_image` | the host copies the bytes during the call; returns the segment |
| `Host.chart(name) -> ChartRef`, `ChartRef.segment`, `ChartRef.chart` | `ta_host_chart` | the current version of the chart: segment and chart index |
| `Host.spawn(ref, name=None, params=None, traceparent=None) -> int` | `ta_host_spawn` | an input, applied at the call; `ref` from `Host.chart`; `params` a payload of the chart's datamodel, moved on success, released on refusal; `traceparent` the `str`'s UTF-8, parsed by core |
| `Host.send(session, event, data=None, *, traceparent=None)` | `ta_host_send` | an input, applied at the call; `data` a payload of the target session's datamodel; `traceparent` the `str`'s UTF-8, parsed by core (`None`, or text that does not parse: a new trace) |
| `Host.send_many(items) -> int` | `ta_host_send_many` | inputs, applied at the call; each item `(session, event[, data[, traceparent]])`, the `ta_event_in` array in the call's arena; returns the length of the accepted prefix |
| `Host.terminate(session)` | `ta_host_terminate` | an input, applied at the call |
| `Host.peer_add(id, *, trusted=False) -> int` | `ta_host_peer_add`, `TA_PEER_TRUSTED` | another system through one connection; added so the envelope vectors of M1 run through the API (ROADMAP.md, "Decided in 8.4, for the API review") |
| `Host.deliver(peer, envelope) -> int` | `ta_host_envelope_read`, `ta_host_deliver`, `TA_ROUTE_DELIVERED`, `TA_ROUTE_REFUSED` | an envelope's bytes from a peer; data that is not one item is delivered too, and answered as malformed |
| `peer=handler(host, envelope)`, `envelope.peer`, `envelope.error`, `envelope.cbor` | `TA_HOST_EFFECT_PEER`, `TA_HOST_EFFECT_PEER_ERROR`, `ta_host_envelope` | the handling of an effect; `envelope` a thin view until the ack; `cbor` written once and copied into a `bytes` |
| `telemetry=handler(host, signal, data)` | `TA_HOST_EFFECT_TRACES`, `TA_HOST_EFFECT_LOGS`, `ta_host_traces`, `ta_host_logs` | the handling of an effect: the request taken when its effect comes (the next comes only after a take) and copied once into `bytes`, `signal` `"traces"` or `"logs"`; an `async def` handler runs as a task; without a handler the request is dropped |
| `Host.metrics() -> bytes` | `ta_host_metrics` | pulled when the application wants them, copied once; outside a drain its batch is acknowledged at once, from a handler it joins the open one; `TA_ERROR_STATE` without `telemetry` in the config |
| `Host.sweep(max_scan, limit) -> list[int]` | `ta_host_sweep` | the CLOCK sweep: at most `limit` sessions that did not run since the previous sweep and can be hibernated now, scanning at most `max_scan` slots; the sessions in the call's arena |
| `Host.hibernate(session) -> bytes` | `ta_host_hibernate` | the actor's blob copied once into `bytes` the application stores; outside a drain its batch is acknowledged at once, from a handler it joins the open one; `TA_ERROR_STATE` when the actor is not at a stable point, `TA_ERROR_UNSUPPORTED` when a `"python"` session holds a value outside the value table (it stays resident) |
| `Host.wake(session, blob)` | `ta_host_wake` | the blob `hibernate` gave, read during the call; the actor's waiting events run at the next turn; a blob core cannot read raises |
| `wake=handler(host, session, job)` | `TA_EFFECT_WAKE`, `TA_EFFECT_FIELD_JOB` | the handling of an effect: an event or a delayed send came for a hibernated session (one effect per hibernation; `job` `None`), or a migration job asks for it (`job` its id); the handler loads the blob and calls `Host.wake`, now or later; an `async def` handler runs as a task |
| `Host.migrate(handler, /, *, chart=None, session=None, wake=False, timeout=0) -> int` | `ta_host_migrate`, `TA_HOST_MIGRATE_WAKE`, `TA_HOST_EFFECT_JOB_DONE`, `ta_host_job_pending` | starts a job that moves actors of older versions (every chart, `chart`, or one `session`) to the current version in the next runs, hibernated ones asked for with `wake`, until `timeout` ticks of the host's clock; returns the job; at its `JOB_DONE` `handler(host, result)` gets `{"job", "migrated", "failed", "pending": [session, ...]}` (the pending sessions read at the dispatch); a handler of a job unfinished at the stop is dropped, never called |
| `migrated=handler(host, migration)` | `TA_HOST_EFFECT_MIGRATED` | the handling of an effect: a thin view (`session`, `segment`, `chart`: its name, `job`) per actor moved, until the ack; without a handler nothing is read |
| `Host.snapshot() -> bytes` | `ta_host_snapshot_pack`, `ta_datamodel_ecmascript_snapshot`, `ta_table_snapshot` | the whole host as one blob, copied once into `bytes`: every segment's region, the images, the system id, the host's time, and the datamodels' state (the `"python"` datamodel's tables' namespaces and rows' values, and quickjs's, as CBOR); refused from a handler and while a migration job runs (`TA_ERROR_STATE`), and for a value outside the value table (`TA_ERROR_UNSUPPORTED`) |
| `Host.restore(snapshot, config, io_processors=None, *, log=None, clock=None, peer=None, telemetry=None, wake=None, migrated=None) -> Host` | `ta_host_snapshot_unpack`, `ta_host_snapshot_image`, `ta_host_restore`, `ta_datamodel_ecmascript_restore`, `ta_table_restore` | a new host running the snapshot, its datamodels' handles valid again before the first run, the handlers as `Host(...)`'s; its clock goes on from the snapshot's time |
| `services={type: handler(host, invocation)}` | `TA_EFFECT_INVOKE` | the handling of an effect: the handler of the `<invoke>`'s type (`EVENT`) gets an `Invocation` record (`SESSION`, `SENDID` as `invokeid`, `EVENT` as `type`, `TARGET` as `src`, the payload as `data`, `TRACEPARENT`), made before the ack, and returns the service, kept by (`SEGMENT`, `TOKEN`); no handler, or one that raises: `ta_host_send` with `TA_EVENT_IN_ERROR_COMMUNICATION` to the invoker, the exception raised after the turn |
| `Invocation.send_back(event, data=None, *, traceparent=...)` | `ta_host_send`, `TA_EVENT_IN_FROM_INVOKE` | an event from the service to its invoker with the invocation's token; `TA_ERROR_STALE` once it is done or cancelled |
| `Invocation.done(data=None, *, traceparent=...)` | `ta_host_send`, `TA_EVENT_IN_INVOKE_DONE` | the service's end: `done.invoke.<invokeid>` with `data` for the invoker; the service is dropped |
| `service.receive(host, send)` | `TA_EFFECT_SEND` with `TA_EFFECT_FIELD_IO_PROCESSOR` `TA_NIL` | the handling of an effect: a send to `#_<invokeid>` (or autoforwarded) for the service of (`SEGMENT`, `TOKEN`), as a thin `Send` view whose `type` is `None` |
| `service.stop(host)` | `TA_EFFECT_UNINVOKE` | the handling of an effect: the invoker cancelled the invocation (it left the state, or ended); the service is dropped |
| `Host.run_until_idle()`, `Host.run()` | `ta_host_effects`, `ta_host_effects_ack`, `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE`, `TA_RUN_YIELDED`, `TA_RUN_EFFECTS`, `TA_RUN_EFFECTS_FULL`, `TA_RUN_EXHAUSTED`, `TA_RUN_FAULT` | the synchronous driver: turns until idle; `run` sleeps to `next_deadline` while there is one |
| `await Host.idle()` | `ta_host_effects`, `ta_host_effects_ack`, `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE`, `TA_RUN_YIELDED` | asyncio: `call_soon` on a yield, one `call_later` timer when idle; an input outside a turn schedules one |
| `SimulatedClock()`, `SimulatedClock.increment(ms)`, `SimulatedClock.now` | `ta_host_advance_time`, `ta_host_run` | the turn at the simulated time, for every host made with the clock; no timer |
| `io_processors={type: handler}`, `handler(host, send)` | `TA_EFFECT_SEND` | the handling of an effect; `send` a thin view until the ack, a record for an `async def` handler (refused with `TypeError` under `with`); a handler that raises before answering has its send failed (`TA_EVENT_IN_ERROR_COMMUNICATION`) |
| `send.session`, `send.event`, `send.target`, `send.sendid`, `send.data`, `send.type` | `ta_host_effect_number`, `ta_host_effect_text`, `ta_datamodel_ecmascript_payload_to_cbor` | fields read on first access; `data` the `"python"` row's value, or quickjs's payload decoded |
| `send.traceparent` | `ta_host_effect_text`, `TA_EFFECT_FIELD_TRACEPARENT` | the sending step's trace, formatted from the effect's binary context on first read; `None` when untraced (empty text) |
| `send.answer(event, data=None, *, traceparent=send.traceparent)` | `ta_host_send` | to `SESSION`, with the send's `sendid`, `origintype` the processor's type, and by default the send's trace; `traceparent=None` starts a new trace |
| `send.fail()` | `ta_host_send`, `TA_EVENT_IN_ERROR_COMMUNICATION` | with the send's `sendid` and `send.traceparent`; core raises `error.communication` in the sender |
| `log=handler(host, entry)`, `entry.session`, `entry.label`, `entry.text` | `TA_EFFECT_LOG`, `ta_host_effect_text`, `ta_host_effect_number` | the handling of an effect; `entry` a thin view until the ack |
| `Host.actor_state(handler, on_error=None, /, **params)` | `actor.state`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method: params passed through as keywords; the handler at the `REPLY`'s dispatch; the id is a pending row's handle (`ta_table`) |
| `Host.actor_context(handler, on_error=None, /, **params)` | `actor.context`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `Host.actor_list(handler, on_error=None, /, **params)` | `actor.list`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `Host.actor_get(handler, on_error=None, /, **params)` | `actor.get`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `Host.host_stats(handler, on_error=None, /, **params)` | `host.stats`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `Host.host_config(handler, on_error=None, /, **params)` | `host.config`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| `Host.trace_configure(handler, on_error=None, /, **params)` | `trace.configure`, `ta_host_post` | a typed method |
| `Host.trace_watch(...)`, `Host.trace_unwatch(...)` | `trace.watch`, `trace.unwatch`, `ta_host_post` | typed methods |
| `Host.trace_end(handler, on_error=None, /, **params)` | `trace.end`, `ta_host_post` | a typed method |
| `Host.trace_spans_decode(handler, on_error=None, /, **params)` | `trace.spans.decode`, `ta_host_post` | a typed method |
| `TinyactorsError` (`message`, `code`, `reason`, `path`, `status`) | `ta_host_post`, `TA_HOST_EFFECT_REPLY`, `ta_status_name` | a reply's error (`code`, `reason`, `path`, `message`), a refusal at a call, a fault, a stale view; a bare status named by `ta_status_name` |
| `tinyactors.encode(value)`, `tinyactors.decode(data)` | `ta_cbor_writer_init`, `ta_cbor_reader_init`, `ta_cbor_read`, `ta_cbor_writer_bytes`, `ta_cbor_writer_free` | streaming conversion of the neutral-value table; Python types, ownership and exceptions in docs/values.md |
| `tinyactors.Address(text)`, `Address.text`, `str(Address)`, reference equality and hashing | `TA_CBOR_TAG_URI`, `ta_cbor_write_tag` | immutable Python representation of tag 32 on text; no routing, URI validation or host handle |
| `tinyactors.UNDEFINED` | `TA_CBOR_UNDEFINED`, `ta_cbor_write_simple` | per-module singleton for undefined, preserved at the top level and inside containers; distinct from None |
| the `"python"` datamodel, `Event` (`_event`) | `TA_DATAMODEL_EVENT_DETACHABLE`, `ta_datamodel_environment_in`, `ta_image_chart_datamodel` | the vtable of `datamodel.h`: a class per chart at `chart_attach`, an instance per session, `_event` an immutable `Event` until the next `bind_event` or `destroy` (kept across `detach_event`) |

Used internally, with no item:

- `ta_host_resume`: the extension answers `TA_EFFECT_REQUEST` (a fetch
  at run time) in C with `TA_ERROR_INVALID` (the parent gets
  `error.execution`) until run-time fetching is built: core has already
  looked in the requesting segment's image. No Python code runs; how
  it is answered once fetching is built is decided then (ADR 0012,
  decision 8; root `ROADMAP.md`, "Decided
  later").
- `ta_host_locate`, `ta_host_system`, `ta_actor_info_get`,
  `ta_host_segment_info_get`: an input's data becomes a payload of the
  target session's datamodel (`host.c`, `ta_python_host_payload`).
