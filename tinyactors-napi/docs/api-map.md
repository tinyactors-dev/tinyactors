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

A row marked *planned* is an item the plan (ROADMAP.md) calls for,
written ahead so its core is checked and the API review sees it; the
commit that adds the item removes the mark.

| API item | Core | Notes |
|---|---|---|
| `encode(value): Uint8Array` | `ta_cbor_writer_init`, `ta_cbor_writer_bytes`, `ta_cbor_writer_free` | The value table (`docs/values.md`) written item by item by `src/values.c` with core's streaming writer, into a room of the call's; the same conversion as requests and event data. The adapter writes the driver's frames with it. |
| `decode(bytes): unknown` | `ta_cbor_reader_init`, `ta_cbor_read` | Item by item with core's reader, straight into JavaScript values by the same table; what it cannot read is refused, never made into a value. The adapter reads the driver's frames with it. |
| `TinyactorsError` (`code`, `reason`, `path`, `status`) | `ta_host_request`, `ta_host_create`, `TA_ERROR_INVALID` | What core refused: a request's or a config's error reply, unchanged (`code`, `reason`, `path`, the message), or a call's status (`status`; the message names the call and the status, as the Python binding's). `docs/decisions.md`, "Errors". |
| `OK`, `ERROR_EXECUTION` … `ERROR_ABORTED` | `TA_OK`, `TA_ERROR_EXECUTION`, `TA_ERROR_COMMUNICATION`, `TA_ERROR_INVALID`, `TA_ERROR_NO_MEMORY`, `TA_ERROR_STALE`, `TA_ERROR_STATE`, `TA_ERROR_UNSUPPORTED`, `TA_ERROR_FATAL`, `TA_ERROR_ABORTED` | Core's statuses by name, to match a `TinyactorsError`'s `status` on; the Python binding's module constants. |
| `compile(charts, {resolve}): Uint8Array` | `ta_compile_charts`, `ta_compile_image`, `ta_compile_diagnostics`, `ta_compile_free` | The charts (`{name, text}`) into one image's bytes; `resolve({base, source})` answers a static `src` (`ta_compile_options.resolve`). Refused: `TinyactorsError` with core's `diagnostics`. |
| `new Host({config, datamodel, quickjs, clock, on})` | `ta_host_create`, `ta_datamodel_ecmascript_new`, `ta_datamodel_ecmascript_binding`, `ta_datamodel_environment_in`, `ta_datamodel_environment_charge` | The config as CBOR by the value table; a refusal throws `environment->error`'s reply. One datamodel per host: `'native'` (the default: `src/datamodel/`, core's vtable over the application's engine) or `'quickjs'` (configured by `quickjs`: its realm). `docs/decisions.md`, "The Host's API". |
| `host.addImage(image): bigint` | `ta_image_open`, `ta_host_add_image`, `TA_HOST_EFFECT_SEGMENT_FREED` | A copy of the bytes, kept until the segment is freed or the host closes; the segment. |
| `host.boot(segment)` | `ta_host_boot` | The actors a system image declares; then the turn. |
| `host.spawn(chart, {name, params, traceparent}): bigint` | `ta_host_chart`, `ta_host_spawn`, `ta_host_advance_time` | The current version of the chart named; `params` made a payload of the host's datamodel; the clocks advanced first (bindings.md, section 7); the session; then the turn. |
| `host.send(session, event, data)` | `ta_host_send` | `data` made a payload of the host's datamodel; then the turn. |
| `host.sendMany([{session, event, data}]): number` | `ta_host_send_many` | In batches of 64 from the stack; the length of the accepted prefix (the rest are released). Each item's session and event are checked first; a payload refused in a later batch throws after the earlier batches' turn. |
| `host.terminate(session)` | `ta_host_terminate` | Then the turn. |
| `host.request(method, params): unknown` | `ta_host_request` | `{"method", "params", "id"}` by the value table; the `result`, or the `error` thrown as `TinyactorsError`; a reply that does not fit is asked again in the length it needs. |
| `host.addPeer({id, trusted}): bigint` | `ta_host_peer_add`, `TA_PEER_TRUSTED` | The peer. |
| `host.deliver(peer, envelope): number` | `ta_host_envelope_read`, `ta_host_deliver`, `TA_ROUTE_DELIVERED`, `TA_ROUTE_REFUSED` | An envelope's bytes from another process; delivered also when its data is not one item (core refuses it and answers the sender); the `TA_ROUTE_*` outcome. |
| `host.close()` | `ta_host_free`, `ta_datamodel_ecmascript_free` | Frees the host, its datamodel and its images, and drops every reference it holds; from a handler, after the loop returns. |
| `on.log({session, label, text})` | `TA_EFFECT_LOG`, `ta_host_effect_text` | The `<log>`'s label (auxiliary) and text (event). |
| `on.done({session, name, data, final})` | `TA_EFFECT_DONE`, `ta_host_effect_number` | The actor's name (target), its donedata (the payload through its datamodel's `to_wire`), the `<final>` that ended it (auxiliary). |
| `on.error({session, code})` | `TA_EFFECT_ERROR` | The fault code (flags); the actor was reclaimed. |
| `on.peer({peer, envelope})`, `on.peerError({peer, envelope})` | `TA_HOST_EFFECT_PEER`, `TA_HOST_EFFECT_PEER_ERROR`, `ta_host_envelope` | The envelope or error envelope as bytes, for the transport to the peer's process. |
| the turn | `ta_host_advance_time`, `ta_host_run`, `ta_host_effects`, `ta_host_effects_ack`, `TA_RUN_IDLE`, `TA_RUN_YIELDED`, `TA_RUN_EFFECTS_FULL`, `TA_RUN_EFFECTS`, `TA_RUN_EXHAUSTED`, `TA_RUN_FAULT` | Not an item: what an input, the timer and `increment` run (`src/loop.c`): one `setTimeout` at `next_deadline`, `setImmediate` after a yielded run or an input. |
| `SimulatedClock`, `increment(ms)`, `now()` | `ta_host_advance_time`, `ta_host_run`, `TA_RUN_IDLE` | A host created with it arms no timer. `increment(ms)` advances the clock by `ms` (the wall clock is the simulated time) and runs the loop until `TA_RUN_IDLE`; inputs run the turn at the current time. The adapter's `advance` steps use it. |

## Core functions no item uses

Every function of core's API descriptions (`tinyactors-core/include/*.api.json`)
that no row above names, grouped by why (M1, 2026-10-12).

**Used inside the addon, not items of their own:**

- the loop and the effect records: `ta_host_effects`, `ta_host_effects_ack`,
  `ta_host_effect_number`, `ta_host_effect_text` (`src/loop.c`,
  `src/effects.c`);
- the value table and requests: `ta_cbor_write_map`, `ta_cbor_write_array`,
  `ta_cbor_write_end`, `ta_cbor_write_text`, `ta_cbor_write_bytes`,
  `ta_cbor_write_uint`, `ta_cbor_write_int`, `ta_cbor_write_negative`,
  `ta_cbor_write_float`, `ta_cbor_write_simple` (`src/values.c`,
  `src/request.c`), and `ta_host_request` with `host.config` for the
  host's tick rate (`src/request.c`);
- the native datamodel: `ta_datamodel_environment_in`,
  `ta_datamodel_environment_charge`, `ta_image_expression`,
  `ta_image_expression_count`, `ta_image_data`, `ta_image_data_count`,
  `ta_image_state_by_name`, `ta_image_string` (`src/datamodel/`);
- quickjs's `ta_datamodel_ecmascript_vtable` (through its binding),
  `ta_runtime_malloc` (the compiler's allocator).

**A later milestone's (ROADMAP.md):**

- M2, the W3C suite: `ta_http_encode_post`, `ta_http_parse_request`
  (BasicHTTP), `ta_host_resume` for `TA_RUN_EFFECTS`' sync sends.
- M3, every effect: `ta_host_resume` (`TA_EFFECT_REQUEST`),
  `ta_host_effect_bytes`, `ta_host_traces`, `ta_host_logs`,
  `ta_host_metrics`, `ta_host_actor_by_name`, `ta_host_locate`,
  `ta_host_chart` beyond spawn, `ta_host_segment_count`,
  `ta_host_segment_info_get`, `ta_traceparent_format`.
- M4, lifecycle and peers: `ta_host_hibernate`, `ta_host_wake`,
  `ta_host_sweep`, `ta_host_migrate`, `ta_host_job_pending`,
  `ta_host_export_actor`, `ta_host_adopt`, `ta_blob_rebase`,
  `ta_blob_migrate`, `ta_host_snapshot`, `ta_host_snapshot_pack`,
  `ta_host_snapshot_unpack`, `ta_host_snapshot_image`,
  `ta_host_snapshot_segments`, `ta_host_restore`,
  `ta_datamodel_ecmascript_snapshot`, `ta_datamodel_ecmascript_restore`,
  `ta_datamodel_ecmascript_last_error`, `ta_host_peer_up`,
  `ta_host_peer_remove`, `ta_host_peer_reach`, `ta_host_peer_status`,
  `ta_host_peer_round_trip_time`, `ta_host_peer_error`,
  `ta_host_peer_error_read`, `ta_host_route`, `ta_host_export`.
- Compile options: `ta_compile_system` (system manifests), and the
  `TA_COMPILE_WERROR` and `TA_COMPILE_STRIP_DEBUG` flags.

**Not exposed, and why:**

- `ta_system_*`, `ta_actor_*`, `ta_core_*`: the layers below the host;
  a binding drives `ta_host` (bindings.md, section 1), which calls them.
- `ta_host_system`: the borrowed `ta_system` of a segment, for the C
  layers' inspection; the API inspects through requests (bindings.md,
  section 10).
- `ta_host_route_begin`, `ta_host_route_step`, `ta_host_route_reason`,
  `ta_host_error_event`, `ta_host_error_route`, `ta_host_message_init`:
  the external storage driver, for a host whose actors live in a database
  (PostgreSQL, after M5: root ROADMAP.md, "Decided later"); a resident
  host routes with `ta_host_deliver`.
- `ta_otlp_*`, `ta_trace_*`, `ta_span_id_format`, `ta_trace_id_format`,
  `ta_step_key_span_id`, `ta_traceparent_parse`: the encoder and the
  trace records inside the host; a binding takes the encoder's requests
  with `ta_host_traces` and `ta_host_logs` (M3).
- `ta_address_*`: addresses are core's (bindings.md, section 2: "no
  twin of C"); an actor reference is its address text (`docs/values.md`).
- `ta_cbor_decode`, `ta_cbor_string`, `ta_cbor_member`, `ta_cbor_next`,
  `ta_cbor_diagnostic`, `ta_cbor_write_tag`: core's token index and
  diagnostic notation for C, and a tag the value table never writes (an
  actor reference is read as its address text); the value table streams
  with the writer and the reader (`src/values.c`).
- `ta_image_*` beyond the native datamodel's, `ta_datamodel_null`: core
  binds the null datamodel itself; an image is opened by `addImage`.
- `ta_datamodel_ecmascript_payload_eval`,
  `ta_datamodel_ecmascript_payload_to_cbor`,
  `ta_datamodel_ecmascript_payload_from_cbor`,
  `ta_datamodel_ecmascript_heap_bytes`,
  `ta_datamodel_ecmascript_set_memory_limit`,
  `ta_datamodel_ecmascript_js_runtime`,
  `ta_datamodel_ecmascript_wasm_set_time`: quickjs's C helpers; the API
  reaches its payloads through the vtable's `to_wire` and `from_wire`,
  evaluates no expression outside a chart (the driver's `value.encode` is
  C-level), and its limits (`maxOps`, `memoryLimit`) are not options yet.
- `ta_runtime_*` but `ta_runtime_malloc`, `ta_xml_writer_*`: the C
  runtime's files, clock and system helpers, and an XML writer for charts
  a binding builds; JavaScript has its own, and charts come as text.
- `ta_wasm_alloc`, `ta_wasm_free`: the wasm module's.
