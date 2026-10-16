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
  `ta_datamodel_ecmascript_heap_bytes` behind a read of the engine's
  heap on a `datamodel: "quickjs"` host; the limit itself is the Host
  option at creation (`ta_datamodel_ecmascript_config.memory_limit`), so
  `ta_datamodel_ecmascript_set_memory_limit` is not exposed (below).
- **BasicHTTP** (*decided for the API review*, log
  `l-p0-inventory-basic-http`): `ta_http_encode_post` and
  `ta_http_parse_request` behind two functions of the module, the POST
  of a send and the event of an incoming request, for an application's
  HTTP I/O processor and the adapter's W3C BasicHTTP loopback.

### Used internally, with no item

- **The loop** (`src/loop.c`): `ta_runtime_now_ns`, the wall clock of a
  host given no clock (as Python's `loop.c`); `ta_host_snapshot` with an
  empty buffer, the quiescence check `snapshot()` makes before any
  datamodel state is built.
- **Effects** (`src/effects.c`): `ta_host_resume`, the answer in C to
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
- **The arena** (`src/host.c`, a mark per call): `ta_growable_arena_init`,
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
  at `chart_attach` the chart's code and names: `ta_image_get_info`,
  `ta_image_chart_name`, `ta_image_chart_source`,
  `ta_image_chart_datamodel` (also `src/host.c`, above),
  `ta_image_data_count`, `ta_image_data`, `ta_image_expression_count`,
  `ta_image_expression`, `ta_image_state_by_name` (`In()`'s argument),
  `ta_image_event_name` and `ta_image_string` (event names and ids shared
  per chart).
- **txml, for XML content** (`src/datamodel/dom.c`; tinyxml's API,
  `tinyxml/include/txml.h`, not core's): the parser, `txml_parser_init`
  (over the host's allocator), `txml_parse_feed`, `txml_parse_step`,
  `txml_parser_error`, `txml_parser_finish`, `txml_parser_destroy`,
  `txml_doc_destroy`; the tree, `txml_doc_root`, `txml_first_child`,
  `txml_next_sibling`, `txml_kind`, `txml_node_name_id`,
  `txml_doc_name_at` (`nodeName`, the qualified name), `txml_node_text`
  (text and CDATA); the attributes, `txml_first_attribute`,
  `txml_next_attribute`, `txml_attribute_read`. Nothing else of txml is
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
