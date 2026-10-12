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
| `Host(config, *, clock=None, on_log=None, on_done=None, on_error=None, on_peer=None)` | `ta_host_create`, `ta_datamodel_ecmascript_new`, `ta_datamodel_ecmascript_binding`, `host.config` | The config (`docs/host-config.md`) as Python values, written with the value table and passed through unchecked; the environment holds the datamodels (the `"python"` datamodel first, then quickjs), the host's allocator (`PyMem_RawMalloc`) and the error buffer, whose reply a refused config raises unchanged. Then `host.config`, once, for the tick rate the clocks convert with. The handlers are called for effects, one call each (`effects.c`). |
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
| `Host.add_peer(id, *, trusted=False) -> peer` | `ta_host_peer_add`, `TA_PEER_TRUSTED` | Another system, as one transport connection; then the turn (a peer's queue may flush). |
| `Host.deliver(peer, envelope) -> route` | `ta_host_envelope_read`, `ta_host_deliver` | Envelope bytes from another process (peer 0: this host's own code), read and delivered, also when its data is not one well-formed item, so core answers the sender; bytes that are not an envelope raise its status. Returns core's route; then the turn. |
| `tinyactors.ROUTE_DELIVERED`, `ROUTE_RELAYED`, `ROUTE_DUPLICATE`, `ROUTE_REFUSED` | `TA_ROUTE_DELIVERED`, `TA_ROUTE_RELAYED`, `TA_ROUTE_DUPLICATE`, `TA_ROUTE_REFUSED` | What `deliver` returns (`host.h`). |
| `on_log(session, label, text)` | `TA_EFFECT_LOG`, `ta_host_effect_number`, `ta_host_effect_text` | A `<log>`: its label and text. |
| `on_done(session, data)` | `TA_EFFECT_DONE` | A spawned actor's end, with its `<donedata>` through its datamodel's `to_wire` and the value table (`None` without). |
| `on_error(session, code)` | `TA_EFFECT_ERROR` | An actor reclaimed after a fault, with its fault code. |
| `on_peer(peer, envelope)` | `TA_HOST_EFFECT_PEER`, `TA_HOST_EFFECT_PEER_ERROR`, `ta_host_envelope` | An envelope or error envelope for another system, as the bytes `ta_host_envelope` writes. |
| the `"python"` datamodel (`datamodel="python"` in a chart) | `ta_image_expression`, `ta_image_expression_count`, `ta_image_data`, `ta_image_data_count`, `ta_image_string`, `ta_image_state_by_name`, `ta_datamodel_environment_in`, `ta_datamodel_environment_charge` | Not an item one calls: the vtable core calls (`src/datamodel/`, `docs/datamodel.md`). A chart's expressions compiled at attach, one globals dict per session, `In()` through the environment, fuel charged once per evaluation. |
| `tinyactors.Event`, the type of `_event` | `TA_EVENT_TYPE_PLATFORM`, `TA_EVENT_TYPE_INTERNAL`, `TA_EVENT_TYPE_EXTERNAL` | The event view `bind_event` gets, as a struct sequence read by attribute: `name`, `type` (`"platform"`, `"internal"`, `"external"`), `sendid`, `origin`, `origintype`, `invokeid`, `data`, `raw`; `None` where the view has nothing. |
| `SimulatedClock()`, `increment(ms)` | `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE` | A host created with it (`Host(..., clock=)`) arms no timer and its time is the clock's: `increment(ms)` moves it on (the monotonic time in the host's ticks, from `host.config`, the wall clock the simulated time) and runs each of its hosts until `TA_RUN_IDLE`; inputs run the turn at the current time. The adapter's `advance` steps use it. |

## Core functions no item uses

**Used inside an item, with no item of their own:**

- `ta_cbor_string` (`src/values.c`): the text and bytes of a decoded token.
- `ta_compile_free` (`src/host.c`): the compile result, once its image is copied.
- `ta_image_chart_source` (`src/datamodel/expressions.c`): the file name a chart's Python code is compiled under, so a traceback names the chart.

**Not exposed in M1, for a later milestone of the plan** (ROADMAP.md,
"Later milestones"; the root ROADMAP.md, step 8):

- I/O processors, invocations and resolution (M2's sync sends, M3's
  effects): `ta_host_resume`, `ta_http_encode_post`, `ta_http_parse_request`.
- Telemetry (M3, every effect): `ta_host_traces`, `ta_host_logs`, `ta_host_metrics`.
- Hibernation, migration, export and snapshots (M4):
  `ta_host_hibernate`, `ta_host_wake`, `ta_host_sweep`, `ta_host_migrate`,
  `ta_host_job_pending`, `ta_host_export_actor`, `ta_host_adopt`,
  `ta_host_snapshot`, `ta_host_snapshot_pack`, `ta_host_snapshot_unpack`,
  `ta_host_snapshot_image`, `ta_host_snapshot_segments`, `ta_host_restore`,
  `ta_blob_rebase`, `ta_blob_migrate`, `ta_datamodel_ecmascript_snapshot`,
  `ta_datamodel_ecmascript_restore`, `ta_datamodel_ecmascript_last_error`.
- Peers beyond adding one and delivering (M4): `ta_host_peer_up`,
  `ta_host_peer_remove`, `ta_host_peer_reach`, `ta_host_peer_status`,
  `ta_host_peer_round_trip_time`, `ta_host_peer_error`,
  `ta_host_peer_error_read`, `ta_host_route`, `ta_host_export`,
  `ta_host_effect_bytes` (a relayed envelope's `wire`: `ta_host_envelope`
  already writes it).
- System manifests (`<system>`): `ta_compile_system`; `boot` takes the
  segment of any image.
- Lookups the requests answer (`actor.list`, `actor.get`):
  `ta_host_actor_by_name`, `ta_host_segment_count`.
- quickjs's own settings (its memory, its runtime):
  `ta_datamodel_ecmascript_heap_bytes`, `ta_datamodel_ecmascript_set_memory_limit`,
  `ta_datamodel_ecmascript_js_runtime`; and its payload helpers, which the
  vtable's `to_wire` and `from_wire` already are:
  `ta_datamodel_ecmascript_payload_to_cbor`,
  `ta_datamodel_ecmascript_payload_from_cbor`,
  `ta_datamodel_ecmascript_payload_eval` (the C-level `value.encode` of the
  suite, which a binding does not offer), `ta_datamodel_ecmascript_vtable`
  (`ta_datamodel_ecmascript_binding` gives it with its instance).

**Not exposed, below what a binding drives** (bindings.md, sections 1
and 2: the binding drives `ta_host`):

- The system and the core under the host: `ta_system_*` (`ta_system_init`,
  `ta_system_run`, ...), `ta_core_*` (`ta_core_step`, ...), `ta_actor_*`
  but `ta_actor_info_get` (`ta_actor_hibernate`, `ta_actor_export`, ...),
  `ta_datamodel_null`, `ta_datamodel_environment_chart` and
  `ta_datamodel_environment_image` (the chart context holds both).
- The runtime's conveniences, which the binding replaces with its own
  allocator, resolver and clock: `ta_runtime_*` (`ta_runtime_malloc`,
  `ta_runtime_resolve`, `ta_runtime_now_ns`, ...).
- The host's encoder and trace records, reached through the config and
  the requests: `ta_otlp_*`, `ta_trace_*`, `ta_traceparent_format`,
  `ta_traceparent_parse`, `ta_span_id_format`, `ta_step_key_span_id`.
- Addresses: text the binding passes through; core parses them
  (`ta_address_parse`, `ta_address_format`, ...).
- The CBOR writer and readers: `src/values.c` builds token lists itself
  and encodes them with `ta_cbor_encode` (`ta_cbor_write_*`,
  `ta_cbor_finish`, `ta_cbor_member`, `ta_cbor_next`); and the diagnostic
  notation, for tools (`ta_cbor_diagnostic`).
- Charts built by calls instead of text: `ta_xml_writer_*`.
- Image inspection, which the requests answer: `ta_image_get_info`,
  `ta_image_bytes`, `ta_image_boot_actor`, `ta_image_chart_by_name`,
  `ta_image_chart_name`, `ta_image_event_name`, `ta_image_state_count`,
  `ta_image_state_name`, `ta_image_state_info`,
  `ta_image_ahead_of_time_section`.
- The external storage driver (a database's routing, not an in-process
  host's): `ta_host_route_begin`, `ta_host_route_step`,
  `ta_host_route_reason`, `ta_host_error_event`, `ta_host_error_route`,
  `ta_host_message_init`.
- Wasm only: `ta_wasm_alloc`, `ta_wasm_free`,
  `ta_datamodel_ecmascript_wasm_set_time`.
