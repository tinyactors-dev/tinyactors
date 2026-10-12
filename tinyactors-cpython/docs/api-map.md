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
| `tinyactors.encode(value) -> bytes` | `ta_cbor_encode` | The value table (`docs/values.md`) as tokens built by `src/values.c`, written in one call; the same conversion as requests and event data. The adapter writes the driver's frames with it. |
| `tinyactors.decode(data) -> object` | `ta_cbor_decode` | One call into tokens, then Python objects by the same table; what it cannot read is refused, never made into a value. The adapter reads the driver's frames with it. |
| `tinyactors.Address(text)` | `ta_cbor_encode`, `ta_cbor_decode` | An actor reference in a value: tag 32 on its text, which the CBOR functions write and read (`docs/values.md`). |
| `tinyactors.UNDEFINED` | `ta_cbor_encode`, `ta_cbor_decode` | CBOR's `undefined` in a value, distinct from `None` (`docs/values.md`). |
| `tinyactors.TinyactorsError` (`status`, `code`, `reason`, `path`) | `ta_host_request`, `ta_host_create`, `ta_cbor_decode` | What core refused: a request's or config's error reply unchanged, or the status of a call that failed (`docs/errors.md`). |
| `tinyactors.OK`, `ERROR_EXECUTION` … `ERROR_ABORTED` | `TA_OK`, `TA_ERROR_EXECUTION`, `TA_ERROR_COMMUNICATION`, `TA_ERROR_INVALID`, `TA_ERROR_NO_MEMORY`, `TA_ERROR_STALE`, `TA_ERROR_STATE`, `TA_ERROR_UNSUPPORTED`, `TA_ERROR_FATAL`, `TA_ERROR_ABORTED` | Core's statuses (`types.h`), which a `TinyactorsError`'s `status` holds. |
| `Host(config, *, clock=None, on_log=None, on_done=None, on_error=None, on_peer=None)` | `ta_host_create`, `ta_datamodel_ecmascript_new`, `ta_datamodel_ecmascript_binding` | The config (`docs/host-config.md`) as Python values, written with the value table and passed through unchecked; the environment holds the datamodels (quickjs; the `"python"` datamodel first, once it exists), the host's allocator (`PyMem_RawMalloc`) and the error buffer, whose reply a refused config raises unchanged. The handlers are called for effects, one call each (`effects.c`). |
| `Host.request(method, params=None) -> result` | `ta_host_request` | The map `{"method", "params", "id"}` from the caller's values, unchecked; the reply's `result` returned, its `error` raised as `TinyactorsError` (`code`, `reason`, `path`, message). Any method of `docs/requests.md`. |
| `Host.close()`, `with Host(...)` | `ta_host_free`, `ta_datamodel_ecmascript_free` | Frees the host now, or after the turn when called from a handler (bindings.md, section 5). |
| `Host.add_image(charts, files=None) -> segment` | `ta_compile_charts`, `ta_compile_image`, `ta_compile_diagnostics`, `ta_image_open`, `ta_host_add_image`, `TA_HOST_EFFECT_SEGMENT_FREED` | Compiles a dict of name to SCXML text (the first the default chart; `files` answers a `src`) and adds the image as a segment, kept until `TA_HOST_EFFECT_SEGMENT_FREED`. A refused compile raises with core's `diagnostics`. |
| `Host.boot(segment)` | `ta_host_boot` | Then the turn. |
| `Host.spawn(chart, params=None, name=None) -> session` | `ta_host_chart`, `ta_host_spawn`, `ta_host_segment_info_get`, `ta_image_chart_datamodel` | `params` become a payload of the chart's datamodel (its `from_wire`); then the turn. |
| `Host.send(session, event, data=None)` | `ta_host_send`, `ta_host_locate`, `ta_host_system`, `ta_actor_info_get` | `data` becomes a payload of the target's datamodel; then the turn. |
| `Host.send_many(events) -> int` | `ta_host_send_many` | Each a `(session, event[, data])` tuple; returns how many core took, in order; then the turn. |
| `Host.terminate(session)` | `ta_host_terminate` | Then the turn. |
| the turn, after each input and `increment` | `ta_host_advance_time`, `ta_host_run`, `ta_host_effects`, `ta_host_effects_ack`, `TA_RUN_IDLE`, `TA_RUN_YIELDED`, `TA_RUN_EFFECTS_FULL`, `TA_RUN_EFFECTS`, `TA_RUN_EXHAUSTED`, `TA_RUN_FAULT` | Both clocks, then runs until idle: again at once on `TA_RUN_YIELDED` (a signal stops it), `TA_RUN_EFFECTS_FULL` and `TA_RUN_EFFECTS`; `TA_RUN_EXHAUSTED` raises `MemoryError` and `TA_RUN_FAULT` a `TinyactorsError`; the effects acknowledged after each run whatever a handler raised, and the first handler's exception raised after the turn. |
| `Host.run()` | `ta_host_run`, `ta_host_advance_time` | The synchronous driver: turns on the real clock, sleeping until the run's `next_deadline`, until there is none. |
| `on_log(session, label, text)` | `TA_EFFECT_LOG`, `ta_host_effect_number`, `ta_host_effect_text` | A `<log>`: its label and text. |
| `on_done(session, data)` | `TA_EFFECT_DONE` | A spawned actor's end, with its `<donedata>` through its datamodel's `to_wire` and the value table (`None` without). |
| `on_error(session, code)` | `TA_EFFECT_ERROR` | An actor reclaimed after a fault, with its fault code. |
| `on_peer(peer, envelope)` | `TA_HOST_EFFECT_PEER`, `TA_HOST_EFFECT_PEER_ERROR`, `ta_host_envelope` | An envelope or error envelope for another system, as the bytes `ta_host_envelope` writes. |
| `SimulatedClock()`, `increment(ms)` | `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE` | A host created with it (`Host(..., clock=)`) arms no timer and its time is the clock's: `increment(ms)` moves it on (the monotonic time in the config's `ticksPerSecond`, the wall clock the simulated time) and runs each of its hosts until `TA_RUN_IDLE`; inputs run the turn at the current time. The adapter's `advance` steps use it. Without `ticksPerSecond` in the config it is refused (ROADMAP.md, request 2). |

## Core functions no item uses

TODO.
