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
| *planned* `tinyactors.compile_charts(sources, resolver=None) -> Compiled` | `ta_compile_charts`, `ta_compile_diagnostics`, `TA_DIAGNOSTIC_ERROR` | the compiler's call; `resolver` the compile-time resolver, the only place a resolver is given; an error raises `CompileError` with every diagnostic and frees the result |
| *planned* `Compiled.image`, `Compiled.diagnostics` | `ta_compile_image`, `ta_compile_diagnostics` | `image` a thin buffer view, valid until `close`; `diagnostics` converted once |
| *planned* `with compile_charts(...) as compiled`, `Compiled.close()` | `ta_compile_free` | the collector as a fallback; `close()` while a `memoryview` of `image` is held raises `BufferError` |
| *planned* `CompileError` | `ta_compile_diagnostics`, `TA_DIAGNOSTIC_ERROR` | a `TinyactorsError` with every diagnostic: `file`, `line`, `column`, `message` |
| *planned* `resolver.resolve(base, source) -> str`, `tinyactors.ResolveError(message, line=None, column=None)` | `ta_compile_charts`, `ta_compile_diagnostics` | the resolver protocol, given to `compile_charts` only (static `src` at compile time); a `ResolveError`, a `SyntaxError` or the rewrite's refusal becomes a diagnostic at its line and column, the message in the compilation's scratch arena. `DirectoryResolver` is the convenience layer's, in Python |
| *planned* `tinyactors.Host(config, io_processors, log, clock)` | `ta_host_create`, `host.config` | the config as CBOR in the call's arena (`ta_growable_arena` [8.2]); the extension posts `host.config` once for `ticksPerSecond`; the environment registers the `"python"` datamodel and quickjs |
| *planned* `with Host(...)`, `async with Host(...)`, `Host.close()` | `trace.end`, `ta_host_traces`, `ta_host_effects_ack`, `ta_host_free` | the stop of bindings.md section 4; pending response handlers dropped, never called |
| *planned* `Host.add_image(image) -> int` | `ta_host_add_image` | the host copies the bytes during the call; returns the segment |
| *planned* `Host.chart(name)` | `ta_host_chart` | the current version of the chart: segment and chart index |
| *planned* `Host.spawn(ref, name=None, params=None, traceparent=None) -> int` | `ta_host_spawn` | an input, applied at the call; `ref` from `Host.chart`; `params` a payload row moved on success; `traceparent` the `str`'s UTF-8, parsed by core |
| *planned* `Host.send(session, event, data=None, *, traceparent=None)` | `ta_host_send` | an input, applied at the call; `data` a payload of the target session's datamodel; `traceparent` the `str`'s UTF-8, parsed by core (`None`, or text that does not parse: a new trace) |
| *planned* `Host.send_many(items) -> int` | `ta_host_send_many` | inputs, applied at the call; each item the arguments of `send`, `traceparent` included; returns the length of the accepted prefix |
| *planned* `Host.terminate(session)` | `ta_host_terminate` | an input, applied at the call |
| *planned* `Host.run_until_idle()`, `Host.run()` | `ta_host_effects`, `ta_host_effects_ack`, `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE`, `TA_RUN_YIELDED`, `TA_RUN_EFFECTS`, `TA_RUN_EFFECTS_FULL`, `TA_RUN_EXHAUSTED`, `TA_RUN_FAULT` | the synchronous driver: turns until idle; `run` sleeps to `next_deadline` |
| *planned* `await Host.idle()` | `ta_host_effects`, `ta_host_effects_ack`, `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE`, `TA_RUN_YIELDED` | asyncio: `call_soon` on a yield, one `call_at` timer when idle |
| *planned* `SimulatedClock.increment(ms)` | `ta_host_advance_time`, `ta_host_run` | the turn at the simulated time; no timer |
| *planned* `io_processors={type: handler}`, `handler(host, send)` | `TA_EFFECT_SEND` | the handling of an effect; `send` a thin view until the ack, a record for an `async def` handler (refused with `TypeError` under `with`); a handler that raises before answering has its send failed (`TA_EVENT_IN_ERROR_COMMUNICATION`) |
| *planned* `send.session`, `send.event`, `send.target`, `send.sendid`, `send.data` | `ta_host_effect_number`, `ta_host_effect_text`, `ta_datamodel_ecmascript_payload_to_cbor` | fields read on first access; `data` the `"python"` row's value, or quickjs's payload decoded |
| *planned* `send.traceparent` | `ta_host_effect_text`, `TA_EFFECT_FIELD_TRACEPARENT` | the sending step's trace, formatted from the effect's binary context on first read; `None` when untraced (empty text) |
| *planned* `send.answer(event, data=None, *, traceparent=send.traceparent)` | `ta_host_send` | to `SESSION`, with the send's `sendid`, `origintype` the processor's type, and by default the send's trace; `traceparent=None` starts a new trace |
| *planned* `send.fail()` | `ta_host_send`, `TA_EVENT_IN_ERROR_COMMUNICATION` | with the send's `sendid` and `send.traceparent`; core raises `error.communication` in the sender |
| *planned* `log=handler(host, entry)`, `entry.session`, `entry.label`, `entry.text` | `TA_EFFECT_LOG`, `ta_host_effect_text`, `ta_host_effect_number` | the handling of an effect; `entry` a thin view until the ack |
| *planned* `Host.actor_state(session, handler, on_error=None)` | `actor.state`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method; the handler at the `REPLY`'s dispatch; the id is a pending row's handle (`ta_table` [8.2]) |
| *planned* `Host.actor_context(session, handler, on_error=None)` | `actor.context`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| *planned* `Host.actor_list(handler, on_error=None, ...)` | `actor.list`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method; params as requests.md |
| *planned* `Host.actor_get(session, handler, on_error=None)` | `actor.get`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| *planned* `Host.host_stats(handler, on_error=None)` | `host.stats`, `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a typed method |
| *planned* `Host.trace_configure(rules, handler, on_error=None)` | `trace.configure`, `ta_host_post` | a typed method |
| *planned* `Host.trace_watch(rules, seconds, handler, on_error=None)`, `Host.trace_unwatch(...)` | `trace.watch`, `trace.unwatch`, `ta_host_post` | typed methods |
| *planned* `Host.trace_spans_decode(traces, handler, on_error=None)` | `trace.spans.decode`, `ta_host_post` | a typed method |
| *planned* `TinyactorsError` | `ta_host_post`, `TA_HOST_EFFECT_REPLY` | a reply's error (`code`, `reason`, `path`, `message`), a refusal at a call, a fault, a stale view; the status named by `ta_status_name` [8.2] |
| `tinyactors.encode(value)`, `tinyactors.decode(data)` | `ta_cbor_writer_init`, `ta_cbor_reader_init`, `ta_cbor_read`, `ta_cbor_writer_bytes`, `ta_cbor_writer_free` | streaming conversion of the neutral-value table; Python types, ownership and exceptions in docs/values.md |
| `tinyactors.Address(text)`, `Address.text`, `str(Address)`, reference equality and hashing | `TA_CBOR_TAG_URI`, `ta_cbor_write_tag` | immutable Python representation of tag 32 on text; no routing, URI validation or host handle |
| `tinyactors.UNDEFINED` | `TA_CBOR_UNDEFINED`, `ta_cbor_write_simple` | per-module singleton for undefined, preserved at the top level and inside containers; distinct from None |
| *planned* the `"python"` datamodel | `TA_DATAMODEL_EVENT_DETACHABLE`, `ta_datamodel_environment_in`, `ta_image_chart_datamodel` | the vtable of `datamodel.h`: a class per chart at `chart_attach`, an instance per session, `_event` until `detach_event` |

Used internally, with no item:

- `ta_host_resume`: the extension answers `TA_EFFECT_REQUEST` (a fetch
  at run time) in C with `TA_ERROR_INVALID` (the parent gets
  `error.execution`) until run-time fetching is built: core has already
  looked in the requesting segment's image. No Python code runs; how
  it is answered once fetching is built is decided then (ADR 0012,
  decision 8; root `ROADMAP.md`, "Decided
  later").
