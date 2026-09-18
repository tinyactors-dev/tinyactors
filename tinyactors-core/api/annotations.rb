# frozen_string_literal: true

# Semantics of include/tinyactors.h that C types cannot express. `mise run api`
# merges this overlay into include/tinyactors.api.json. Conventions cover most
# of the API (see docs/internal/api-ir.md); list only the exceptions here.
#
# Paths are "type.field" for struct fields and "function.param" for function
# parameters.
module TinyactorsApiAnnotations
  ANNOTATIONS = {
    # Anonymous enums whose constants are the values of an integer typedef.
    enum_constants: {
      'ta_result' => 'TA_OK'
    },

    # Pointer fields that address exactly one optional element. By
    # convention a const pointer inside a ta_scxml_* specification is a
    # {0}-terminated list and a const pointer elsewhere is a single element.
    single: %w[
      ta_scxml_spec.identity
      ta_scxml_spec.initial
      ta_scxml_state_spec.initial
      ta_scxml_state_spec.done_data
    ],

    # Caller-supplied output buffers: the pointer is written by the callee,
    # sized by `capacity`, with the required size reported through `needed`.
    buffers: {
      'ta_event_name_format_params.buffer' => { capacity: 'capacity', needed: 'needed' },
      'ta_actor_active_states_params.states' => { capacity: 'capacity', needed: 'count' },
      'ta_actor_history_params.states' => { capacity: 'capacity', needed: 'count' },
      'ta_actor_delayed_sends_params.sends' => { capacity: 'capacity', needed: 'count' }
    },

    # Tagged-union kinds whose `as` member name differs from the enum
    # constant's suffix.
    union_members: {
      'ta_scxml_action_spec' => { 'TA_SCXML_ACTION_IF' => 'conditional' },
      'ta_origin' => { 'TA_ORIGIN_IO' => 'opaque' },
      'ta_invoke_report_params' => {
        'TA_INVOKE_REPORT_DONE' => 'done_data',
        'TA_INVOKE_REPORT_UNAVAILABLE' => 'fault'
      }
    },

    # Opaque types that are borrowed views, never owned handles.
    borrowed: %w[ta_dm_host ta_arena_block],

    # Output values that the caller must release, guarded by a flag field.
    owned_outputs: {
      'ta_actor_completion.data' => 'has_data',
      'ta_actor_inspection.datamodel_root' => 'has_datamodel_root'
    },

    # Handle lifetimes beyond the conventions (`T **out` creates a handle;
    # `ta_<type>_destroy(T *)` retires it).
    consumes: {
      # Registration takes ownership of the definition only on TA_OK.
      'ta_system_register_definition.definition' => 'ok'
    }
  }.freeze
end
