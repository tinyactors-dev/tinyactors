/* Every public header must be self-contained and C89-ABI friendly; this TU
 * includes each one first in its own block (compiled as C and as C++). */
#include <tinyactors/types.h>
#include <tinyactors/cbor.h>
#include <tinyactors/stats.h>
#include <tinyactors/allocator.h>
#include <tinyactors/image.h>
#include <tinyactors/datamodel.h>
#include <tinyactors/system.h>
#include <tinyactors/core.h>
#include <tinyactors/compile.h>
#include <tinyactors/runtime.h>
#include <tinyactors.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/trace.h>
#include <tinyactors/otlp.h>

/* ABI layout guards: fields crossing FFI must keep their offsets. */
#define ABI_CHECK(c, m) typedef char abi_check_##m[(c) ? 1 : -1]
ABI_CHECK(sizeof(ta_payload) == 8, payload_is_u64);
ABI_CHECK(sizeof(ta_image) == 256, image_view_fixed);
ABI_CHECK(offsetof(ta_event_in, size) == 0, event_in_size_first);
ABI_CHECK(offsetof(ta_event_in, session) == 16, event_in_session_offset);
ABI_CHECK(offsetof(ta_effect, size) == 0, effect_size_first);
ABI_CHECK(offsetof(ta_system_config, size) == 0, config_size_first);
ABI_CHECK(offsetof(ta_datamodel_ecmascript_config, size) == 0, ecmascript_config_size_first);
/* extensible structs grow at the end: fields read only when `size` covers them */
ABI_CHECK(offsetof(ta_datamodel_ecmascript_config, realm) >
              offsetof(ta_datamodel_ecmascript_config, stack_limit),
          ecmascript_config_realm_last);
ABI_CHECK(offsetof(ta_datamodel_ecmascript_config, abort_flag) >
              offsetof(ta_datamodel_ecmascript_config, realm),
          ecmascript_config_abort_last);
ABI_CHECK(offsetof(ta_compile_options, size) == 0, compile_options_size_first);
ABI_CHECK(offsetof(ta_compile_options, abort_flag) ==
              offsetof(ta_compile_options, resolve_context) + sizeof(void *),
          compile_options_abort_last);
ABI_CHECK(TA_ERROR_ABORTED == -9, status_aborted);
ABI_CHECK(sizeof(ta_cbor_token) == 16, cbor_token_fixed);
ABI_CHECK(offsetof(ta_cbor_token, value) == 8, cbor_token_value_offset);
ABI_CHECK(offsetof(ta_buffer, length) == 2 * sizeof(size_t) && sizeof(ta_buffer) == 3 * sizeof(void *),
          buffer_layout);
ABI_CHECK(sizeof(ta_memory) == 2 * sizeof(void *), memory_layout);
ABI_CHECK(offsetof(ta_spawn, name) == 8 && offsetof(ta_resume, result) == 16, call_structs_layout);
ABI_CHECK(offsetof(ta_datamodel_vtable, migrate) > offsetof(ta_datamodel_vtable, deserialize),
          vtable_optional_hooks_last);
ABI_CHECK(offsetof(ta_datamodel_vtable, to_wire) > offsetof(ta_datamodel_vtable, migrate),
          vtable_wire_after_migrate);
ABI_CHECK(offsetof(ta_datamodel_vtable, from_wire) > offsetof(ta_datamodel_vtable, to_wire),
          vtable_from_wire_after_to_wire);
ABI_CHECK(offsetof(ta_datamodel_vtable, payload_serialize) >
              offsetof(ta_datamodel_vtable, from_wire),
          vtable_payload_bytes_after_wire);
ABI_CHECK(offsetof(ta_datamodel_vtable, payload_deserialize) >
              offsetof(ta_datamodel_vtable, payload_serialize),
          vtable_payload_deserialize_last);
ABI_CHECK(TA_IO_PROCESSOR_SCXML_SHARED == 4, io_processor_shared_flag);
ABI_CHECK(offsetof(ta_state_info, size) == 0, state_info_size_first);
ABI_CHECK(offsetof(ta_actor_info, size) == 0, actor_info_size_first);
ABI_CHECK(sizeof(ta_actor_info) == 56, actor_info_size);
ABI_CHECK(offsetof(ta_host_environment, size) == 0, host_environment_size_first);
ABI_CHECK(offsetof(ta_host_effect, size) == 0, host_effect_size_first);
ABI_CHECK(offsetof(ta_host_segment_info, size) == 0, host_segment_info_size_first);
ABI_CHECK(offsetof(ta_host_stats, size) == 0, host_stats_size_first);
ABI_CHECK(offsetof(ta_host_snapshot_contents, size) == 0, host_snapshot_contents_size_first);
ABI_CHECK(offsetof(ta_system_stats, size) == 0, system_stats_size_first);
/* the stats grow at the end: older readers set a smaller size */
ABI_CHECK(offsetof(ta_system_stats, traces_started) > offsetof(ta_system_stats, fuel_clock),
          system_stats_traces_last);
ABI_CHECK(offsetof(ta_chart_stats, size) == 0, chart_stats_size_first);
ABI_CHECK(offsetof(ta_chart_stats, actors) == 8 + sizeof(ta_string_view),
          chart_stats_actors_offset);
ABI_CHECK(offsetof(ta_route_request, size) == 0, route_request_size_first);
ABI_CHECK(offsetof(ta_route_request, session) == 32, route_request_session_offset);
ABI_CHECK(offsetof(ta_host_message, size) == 0, host_message_size_first);
ABI_CHECK(offsetof(ta_host_message, revision) == 8, host_message_revision_offset);
ABI_CHECK(offsetof(ta_host_message, envelope) == 16, host_message_envelope_offset);
ABI_CHECK(offsetof(ta_envelope, size) == 0, envelope_size_first);
ABI_CHECK(offsetof(ta_peer_config, size) == 0, peer_config_size_first);
ABI_CHECK(offsetof(ta_peer_status, size) == 0, peer_status_size_first);
/* trace values: plain integers, the same layout on every target */
ABI_CHECK(sizeof(ta_trace_id) == 16, trace_id_size);
ABI_CHECK(sizeof(ta_span_id) == 8, span_id_size);
ABI_CHECK(sizeof(ta_step_key) == 24, step_key_size);
ABI_CHECK(offsetof(ta_step_key, microstep) == 16, step_key_microstep_offset);
ABI_CHECK(sizeof(ta_trace_context) == 32, trace_context_size);
ABI_CHECK(offsetof(ta_trace_context, parent) == 16, trace_context_parent_offset);
ABI_CHECK(sizeof(ta_trace_generator) == 32, trace_generator_size);
ABI_CHECK(TA_TRACEPARENT_LENGTH == 55, traceparent_length);
/* the trace record schema: readers in every language rely on these */
ABI_CHECK(sizeof(ta_trace_record) == 88, trace_record_header_size);
ABI_CHECK(offsetof(ta_trace_record, key) == 16, trace_record_key_offset);
ABI_CHECK(offsetof(ta_trace_record, trace) == 40, trace_record_trace_offset);
ABI_CHECK(offsetof(ta_trace_record, fuel) == 72, trace_record_fuel_offset);
ABI_CHECK(sizeof(ta_trace_macrostep_begin) == 72, trace_macrostep_begin_size);
ABI_CHECK(sizeof(ta_trace_microstep) == 24, trace_microstep_size);
ABI_CHECK(sizeof(ta_trace_sent) == 56, trace_sent_size);
ABI_CHECK(sizeof(ta_trace_invoked) == 24, trace_invoked_size);
ABI_CHECK(sizeof(ta_trace_view) == 8 + 2 * sizeof(void *), trace_view_size);
ABI_CHECK(offsetof(ta_otlp_config, size) == 0, otlp_config_size_first);
ABI_CHECK(offsetof(ta_otlp_input, size) == 0, otlp_input_size_first);
ABI_CHECK(offsetof(ta_otlp_stats, size) == 0, otlp_stats_size_first);
ABI_CHECK(offsetof(ta_otlp_stats_input, size) == 0, otlp_stats_input_size_first);
ABI_CHECK(offsetof(ta_trace_policy, size) == 0, trace_policy_size_first);
ABI_CHECK(TA_TRACE_DETAIL(0x0100u | TA_TRACE_FLAG_SAMPLED) == TA_TRACE_DETAIL_SAMPLED,
          trace_detail_above_w3c_flags);
ABI_CHECK(sizeof(ta_otlp_exemplar) == 40, otlp_exemplar_size);
ABI_CHECK(sizeof(ta_otlp_cursor) == 48, otlp_cursor_size);

int main(void) {
    return 0;
}
