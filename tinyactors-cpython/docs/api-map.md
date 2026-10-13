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

A row marked *planned* is an item the plan (ROADMAP.md) calls for,
written ahead so its core is checked and the API review sees it; the
commit that adds the item removes the mark.

| API item | Core | Notes |
|---|---|---|
| *planned* `tinyactors.Host(config, io_processors, log, clock)` | `ta_host_create`, `host.config`, `ta_host_add_image` | the config as CBOR in the call's arena (`ta_growable_arena` [8.2]); the extension posts `host.config` once for `ticksPerSecond`; the environment registers the `"python"` datamodel and quickjs |
| *planned* `with Host(...)`, `async with Host(...)`, `Host.close()` | `trace.end`, `ta_host_traces`, `ta_host_effects_ack`, `ta_host_free` | the stop of bindings.md section 4; pending response handlers dropped, never called |
| *planned* `Host.add_image(charts)` | `ta_compile_charts`, `ta_compile_image`, `ta_host_add_image`, `ta_compile_free` | the host copies the bytes; the compile result is freed at once |
| *planned* `Host.spawn(chart, name=None, params=None) -> int` | `ta_host_chart`, `ta_host_spawn` | an input, applied at the call; `params` a payload row moved on success |
| *planned* `Host.send(session, event, data=None)` | `ta_host_send` | an input, applied at the call; `data` a payload of the target session's datamodel |
| *planned* `Host.terminate(session)` | `ta_host_terminate` | an input, applied at the call |
| *planned* `Host.run_until_idle()`, `Host.run()` | `ta_host_effects`, `ta_host_effects_ack`, `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE`, `TA_RUN_YIELDED`, `TA_RUN_EFFECTS`, `TA_RUN_EFFECTS_FULL`, `TA_RUN_EXHAUSTED`, `TA_RUN_FAULT` | the synchronous driver: turns until idle; `run` sleeps to `next_deadline` |
| *planned* `await Host.idle()` | `ta_host_effects`, `ta_host_effects_ack`, `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE`, `TA_RUN_YIELDED` | asyncio: `call_soon` on a yield, one `call_at` timer when idle |
| *planned* `SimulatedClock.increment(ms)` | `ta_host_advance_time`, `ta_host_run` | the turn at the simulated time; no timer |
| *planned* `io_processors={type: handler}`, `handler(host, send)` | `TA_EFFECT_SEND` | the handling of an effect; `send` a thin view until the ack, a record for an `async def` handler |
| *planned* `send.session`, `send.event`, `send.target`, `send.sendid`, `send.data` | `ta_host_effect_number`, `ta_host_effect_text`, `ta_datamodel_ecmascript_payload_to_cbor` | fields read on first access; `data` the `"python"` row's value, or quickjs's payload decoded |
| *planned* `send.answer(event, data=None)` | `ta_host_send` | to `SESSION`, with the send's `sendid` and `origintype` the processor's type |
| *planned* `send.fail()` | `ta_host_send`, `TA_EVENT_IN_ERROR_COMMUNICATION` | with the send's `sendid`; core raises `error.communication` in the sender |
| *planned* `log=handler(host, entry)`, `entry.chart`, `entry.session`, `entry.label`, `entry.text` | `TA_EFFECT_LOG`, `ta_host_effect_text`, `ta_host_effect_number` | the handling of an effect; `entry` a thin view until the ack |
| *planned* `Host.actor_state(session, handler, on_error=None)` | `actor.state`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method; the handler at the `REPLY`'s dispatch; the id is a pending row's handle (`ta_table` [8.2]) |
| *planned* `Host.actor_context(session, handler, on_error=None)` | `actor.context`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| *planned* `Host.actor_list(handler, on_error=None, ...)` | `actor.list`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method; params as requests.md |
| *planned* `Host.actor_get(session, handler, on_error=None)` | `actor.get`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| *planned* `Host.host_stats(handler, on_error=None)` | `host.stats`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| *planned* `Host.trace_configure(rules, handler, on_error=None)` | `trace.configure`, `ta_host_post` | a typed method |
| *planned* `Host.trace_watch(rules, seconds, handler, on_error=None)`, `Host.trace_unwatch(...)` | `trace.watch`, `trace.unwatch`, `ta_host_post` | typed methods |
| *planned* `Host.trace_spans_decode(traces, handler, on_error=None)` | `trace.spans.decode`, `ta_host_post` | a typed method |
| *planned* `TinyactorsError` | `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a reply's error (`code`, `reason`, `path`, `message`), a refusal at a call, a fault, a stale view; the status named by `ta_status_name` [8.2] |
| *planned* `tinyactors.encode(value)`, `tinyactors.decode(data)` | `ta_cbor_writer_init`, `ta_cbor_reader_init` | the value table, streaming |
| *planned* the `"python"` datamodel | `TA_DATAMODEL_EVENT_DETACHABLE`, `ta_datamodel_environment_in`, `ta_image_chart_datamodel` | the vtable of `datamodel.h`: a class per chart at `chart_attach`, an instance per session, `_event` until `detach_event` |
