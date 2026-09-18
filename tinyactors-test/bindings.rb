# frozen_string_literal: true

# Binding policy for `mise run bindings`. Every public function and type in
# tinyactors-core/include/tinyactors.api.json is either generated, overridden
# by a hand-written codec/handler, or skipped here with a reason. The
# generator fails when something new is in none of these categories.
#
# Keys are File.fnmatch patterns.
module TinyactorsBindings
  SKIP_FUNCTIONS = {
    'ta_arena_*' => 'C memory utility; the protocol owns request memory',
    'ta_tree_*' => 'C construction helper; JSON specifications decode straight into the spec tree',
    'ta_value_retain' => 'static inline lifetime helper, not an ABI symbol; the value table owns references',
    'ta_value_release' => 'static inline lifetime helper, not an ABI symbol; the value table owns references',
    'ta_value_clone' => 'value lifetime is managed by the binding value table',
    'ta_event_clone' => 'C memory helper for native event ownership',
    'ta_event_release' => 'C memory helper for native event ownership',
    'ta_external_delivery_clone' => 'used inside asynchronous router adapters, not by hosts',
    'ta_external_delivery_release' => 'used inside asynchronous router adapters, not by hosts',
    'ta_dm_host_*' => 'callable only from inside data-model callbacks'
  }.freeze

  # Functions with a hand-written `tat_handler_<function>` in src/. None yet.
  OVERRIDE_FUNCTIONS = {}.freeze

  # Types with hand-written `tat_decode_<type>` / `tat_encode_<type>` in
  # src/binding.c.
  OVERRIDE_TYPES = {
    'ta_str' => 'JSON string copied into the request arena',
    'ta_bytes' => 'JSON string copied into the request arena',
    'ta_value' => 'jq-backed JSON values from the test data model',
    'ta_expression' => 'source text only; host expressions cannot cross the protocol'
  }.freeze

  SKIP_TYPES = {
    '*_vtable' => 'implemented by hand-written adapters (src/adapters.c)',
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

  def self.lookup(table, name)
    table.find { |pattern, _reason| File.fnmatch(pattern, name) }&.last
  end
end
