# frozen_string_literal: true

# Binding policy for `mise run bindings`: the decisions about the JSON-RPC
# host that tinyactors-core/include/tinyactors.api.json cannot make. Every
# public function and type in the description is bound, implemented by hand,
# or skipped here with a reason; the generator fails when an item fits none
# of these, when a skipped type is still needed by a bound one, and when an
# entry below matches nothing.
#
# Keys are File.fnmatch patterns; {a,b} lists alternatives.
module TinyactorsBindings
  SKIP_FUNCTIONS = {
    'ta_arena_*' => 'C memory utility; the protocol owns request memory',
    'ta_tree_*' => 'C construction helper; JSON specifications decode straight into the spec tree',
    'ta_value_retain' => 'static inline lifetime helper; the value table owns references',
    'ta_value_release' => 'static inline lifetime helper; the value table owns references',
    'ta_value_clone' => 'value lifetime is managed by the binding value table',
    'ta_event_clone' => 'C memory helper for native event ownership',
    'ta_event_release' => 'C memory helper for native event ownership',
    'ta_external_delivery_clone' => 'used inside asynchronous router adapters, not by hosts',
    'ta_external_delivery_release' => 'used inside asynchronous router adapters, not by hosts',
    'ta_dm_host_*' => 'callable only from inside data-model callbacks'
  }.freeze

  # Functions with a hand-written `tat_handler_<function>` in src/. None yet.
  OVERRIDE_FUNCTIONS = {}.freeze

  # Types with a hand-written `tat_decode_<type>` / `tat_encode_<type>` in
  # src/binding.c. (String, bytes, and host value types have runtime codecs
  # by their semantic in the API description.)
  OVERRIDE_TYPES = {
    'ta_expression' => 'source text only; host expressions cannot cross the protocol'
  }.freeze

  SKIP_TYPES = {
    '*_vtable' => 'adapter operations; adapters are C factories in src/adapters.c',
    'ta_arena' => 'C memory utility',
    'ta_tree' => 'C construction helper',
    'ta_tree_list' => 'C construction helper',
    'ta_tree_list_layout' => 'C construction helper',
    'ta_allocator_free_params' => 'allocator callback argument',
    'ta_expression_host' => 'host expressions cannot cross the protocol',
    'ta_datamodel_*_params' => 'data-model callback argument; adapters are C',
    'ta_invoker_start_params' => 'invoker callback argument; adapters are C',
    'ta_invoker_forward_params' => 'invoker callback argument; adapters are C'
  }.freeze

  # Struct members of tagged unions whose fields sit in the parent object
  # instead of under the member's name: {"kind": "named", "family": ..., "name": ...}.
  # Every other member is keyed by its name. Flattening fails when a member's
  # keys collide with the parent's.
  #
  # ta_scxml_action_spec.invoke stays keyed: its own `kind` would collide.
  FLATTEN_UNION_MEMBERS = {
    'ta_actor_ref.named' => 'an address reads as the actor reference itself',
    'ta_scxml_action_spec.{raise,send,cancel,assign,script,log,conditional,foreach,extension}' =>
      'an action is one object, as in SCXML markup'
  }.freeze

  # A parameter with this name that points at a struct is flattened: its input
  # fields are members of the params object next to the other parameters.
  FLATTEN_PARAMETER = 'params'

  # Value-model kinds the host cannot resolve to a data model; their ta_value
  # inputs decode with the jq data model.
  VALUE_MODEL_FALLBACKS = {
    'session_image' => 'no core query maps an image to its data model; ta_actor_load and ta_actor_reconstruct are unimplemented'
  }.freeze

  # The largest caller buffer a request may ask for, in elements. It bounds
  # the per-request arena: `capacity` comes from the client.
  MAX_BUFFER_ELEMENTS = 65_536
end
