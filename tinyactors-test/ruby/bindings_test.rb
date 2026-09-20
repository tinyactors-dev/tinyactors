# frozen_string_literal: true

# Exercises the RPC bindings generated from tinyactors.api.json: discovery,
# codecs, 64-bit ID encoding, handle validation, and the unimplemented list.

require 'json'
require 'pathname'
require_relative 'tinyactors_test'

root = Pathname.new(__dir__).join('..').expand_path
load root.join('ruby/charts/traffic_light.rb').to_s

def check(condition, message)
  raise "bindings test failed: #{message}" unless condition
end

def rpc_error(code)
  yield
  raise 'bindings test failed: expected an RPC error'
rescue TinyactorsTest::RpcError => error
  check(error.code == code, "expected RPC error #{code}, got #{error.code}: #{error.data}")
  error
end

# Drops `kind` where the one other member already names it, e.g.
# {"kind": "expr", "expr": "0"} -> {"expr": "0"}, the chart DSL's shorthand.
def without_inferable_kinds(value)
  case value
  when Hash
    stripped = value.transform_values { |each| without_inferable_kinds(each) }
    others = stripped.keys - ['kind']
    stripped.delete('kind') if others.size == 1 && stripped['kind'] == others.first
    stripped
  when Array then value.map { |each| without_inferable_kinds(each) }
  else value
  end
end

host = TinyactorsTest::Driver.new(root.join('build/tinyactors-test'))
begin
  # Discovery lists generated methods with schemas and implementation status.
  discovery = host.api.instance_variable_get(:@discovery)
  methods = discovery.fetch(:methods)
  check(methods.include?('ta.actor.spawn'), 'generated function methods are discoverable')
  check(methods.include?('codec.roundtrip.ta_scxml_spec'), 'codec round-trip methods are discoverable')
  schema = discovery.dig(:schemas, :'ta.system.create')
  check(schema[:function] == 'ta_system_create', 'schema names the C function')
  check(schema.dig(:out, :out) == 'handle:system', 'schema describes handle outputs')
  unimplemented = discovery.fetch(:unimplemented)
  check(unimplemented.include?('ta.actor.prepare_replacement'), 'unimplemented core functions are listed')
  check(!unimplemented.include?('ta.system.create'), 'implemented core functions are not listed')
  check(unimplemented.none? { |name| name.start_with?('codec.') }, 'codecs are never unimplemented')
  check(discovery.fetch(:adapters).include?('datamodel.jq'), 'adapters are listed')

  # Calling an unimplemented function validates params, then reports -32001.
  system = host.ta.system.create(config: { allocator: { '$adapter' => 'allocator.libc' } }).dig(:out, :out)
  # Its params are plain IDs, not handles, so nothing rejects the call first.
  error = rpc_error(-32_001) do
    host.ta.actor.prepare_replacement(system: system, incumbent: 0, definition: 0)
  end
  check(error.data.include?('ta_actor_prepare_replacement'), 'unimplemented error names the function')
  error = rpc_error(-32_602) { host.ta.system.create(config: { allocator: { '$adapter' => 'allocator.none' } }) }
  check(error.data == 'params.config.allocator: no ta_allocator adapter named "allocator.none"',
        "adapter errors carry a path: #{error.data}")

  # Tagged unions: kind may be inferred from the single member key.
  check(host.codec.roundtrip.ta_scxml_value_spec(value: { expr: '.x' }) == { kind: 'expr', expr: '.x' },
        'tagged union round-trip infers kind')
  ref = host.codec.roundtrip.ta_actor_ref(value: { kind: 'named', family: { name: 'door' }, name: 'd1' })
  check(ref == { kind: 'named', family: { name: 'door' }, name: 'd1' }, "flattened struct member: #{ref}")
  origin = { kind: 'actor', actor: { kind: 'runtime', runtime: 7 } }
  check(host.codec.roundtrip.ta_origin(value: origin) == origin, 'colliding member stays nested')
  rpc_error(-32_602) { host.codec.roundtrip.ta_actor_ref(value: { name: 'd1' }) }

  # 64-bit IDs: numbers up to 2^53-1, decimal strings above.
  parent = host.codec.roundtrip.ta_actor_parent(value: { actor: '18446744073709551615', invoke_id: 9_007_199_254_740_991 })
  check(parent == { actor: '18446744073709551615', invoke_id: 9_007_199_254_740_991 }, "64-bit IDs: #{parent}")
  parent = host.codec.roundtrip.ta_actor_parent(value: { actor: '42', invoke_id: 0 })
  check(parent[:actor] == 42, 'small decimal strings come back as numbers')
  rpc_error(-32_602) { host.codec.roundtrip.ta_actor_parent(value: { actor: 9_007_199_254_740_992, invoke_id: 0 }) }

  # Terminated lists and a whole chart specification, including the SCXML
  # `datamodel` attribute that the chart DSL sets.
  spec = JSON.parse(JSON.generate(TinyactorsTest.charts.fetch(:traffic_light).to_spec))
  check(spec['datamodel'] == 'jq', "chart spec names its data model: #{spec['datamodel'].inspect}")
  round = JSON.parse(JSON.generate(host.codec.roundtrip.ta_scxml_spec(value: spec)))
  check(without_inferable_kinds(round) == spec, 'traffic light spec round-trips')

  # Definition inspection end to end: the adapter identifies itself, the
  # identity finds the definition, and the data-model root is its own call.
  live = host.ta.system.create(config: { allocator: { '$adapter' => 'allocator.libc' } }).dig(:out, :out)
  compiled = host.ta.definition.compile_spec(system: live, spec: spec, datamodel: { '$adapter' => 'datamodel.jq' },
                                             allocator: { '$adapter' => 'allocator.libc' })
  check(compiled[:result] == 'ok', "compile: #{compiled}")
  registered = host.ta.system.register_definition(system: live, definition: compiled.dig(:out, :out))
  id = registered.dig(:out, :out)
  facts = host.ta.definition.inspect(system: live, definition: id)
  check(facts[:result] == 'ok', "definition inspect: #{facts}")
  facts = facts[:out][:out]
  check(facts[:id] == id && facts[:datamodel] == { '$adapter': 'datamodel.jq' } && facts[:retired] == false &&
          facts[:binding] == 'early' && facts[:state_count].positive? && facts[:actor_count].zero? &&
          facts[:definition_bytes].positive? && facts.dig(:identity, :family, :name) == 'traffic_light',
        "definition inspection: #{facts}")
  found = host.ta.system.find_definition(system: live, identity: facts[:identity])
  check(found == { result: 'ok', out: { out: id } }, "find definition: #{found}")
  missing = host.ta.system.find_definition(system: live, identity: { family: { name: 'nope' }, revision: 'x' })
  check(missing[:result] == 'err_not_found', "find a missing definition: #{missing}")
  check(host.ta.definition.inspect(system: live, definition: id + 1)[:result] == 'err_not_found',
        'inspecting an unknown definition')
  actor = host.ta.actor.spawn(system: live, definition: id).dig(:out, :out)
  view = host.ta.actor.inspect(system: live, actor: actor).dig(:out, :out)
  check(!view.key?(:datamodel_root) && view[:allocated_bytes] >= facts[:actor_fixed_bytes],
        "actor inspection: #{view}")
  root = host.ta.actor.datamodel_root(system: live, actor: actor)
  check(root[:result] == 'ok' && root.dig(:out, :out).is_a?(Hash), "datamodel root: #{root}")

  # Caller buffers are bounded, because the client chooses `capacity`.
  error = rpc_error(-32_602) { host.ta.actor.active_states(system: live, actor: actor, capacity: 1 << 20) }
  check(error.data == 'params.capacity: capacity exceeds 65536', "buffer limit: #{error.data}")
  # An output is reported only after a result that makes it valid; `count`
  # is valid after every result.
  missing = host.ta.actor.get_completion(system: live, actor: 12_345)
  check(missing == { result: 'err_not_found', out: { out: nil } }, "invalid outputs are null: #{missing}")
  check(host.ta.actor.resume(system: live, actor: actor, budget: 100)[:result] == 'ok', 'the actor enters its states')
  short = host.ta.actor.active_states(system: live, actor: actor, capacity: 0)
  check(short[:result] == 'err_nomem' && short.dig(:out, :states).nil? && short.dig(:out, :count).positive?,
        "a size query reports the count: #{short}")
  check(host.ta.definition.inspect(system: live, definition: id).dig(:out, :out, :actor_count) == 1,
        'the spawned actor is counted')

  # Retirement and generational IDs (ADR 0043): the ID carries a generation
  # in its high 32 bits and also travels as a decimal string.
  check(id >= 2**32, "a definition ID carries its generation: #{id}")
  check(host.ta.definition.inspect(system: live, definition: id.to_s)[:result] == 'ok',
        'a definition ID as a decimal string')
  check(host.ta.definition.inspect(system: live, definition: (id + 2**60).to_s)[:result] == 'err_not_found',
        'an ID above 2^53 with a future generation is unknown')
  check(host.ta.system.retire_definition(system: live, definition: id)[:result] == 'ok',
        'retire a definition with a resident actor')
  retired = host.ta.definition.inspect(system: live, definition: id).dig(:out, :out)
  check(retired[:retired] == true && retired[:actor_count] == 1, "a retired definition stays inspectable: #{retired}")
  check(host.ta.actor.spawn(system: live, definition: id)[:result] == 'err_not_found',
        'a retired definition spawns no actors')
  check(host.ta.system.find_definition(system: live, identity: facts[:identity])[:result] == 'err_not_found',
        'find skips a retired definition')
  check(host.ta.actor.destroy(system: live, actor: actor)[:result] == 'ok', 'destroy the last actor')
  check(host.ta.definition.inspect(system: live, definition: id)[:result] == 'err_not_found',
        'the last actor frees a retired definition')
  check(host.ta.system.retire_definition(system: live, definition: id)[:result] == 'ok',
        'retiring a freed definition again is idempotent')
  host.ta.system.destroy(system: live)

  # The <scxml> <script> is a ta_expression member of the spec.
  scripted = TinyactorsTest::Chart.define(:scripted) do
    data count: 0
    script '.count = 1'
    state(:idle) { transition on: 'go', target: :idle }
  end
  spec = JSON.parse(JSON.generate(scripted.to_spec))
  check(spec['script'] == '.count = 1', "chart spec carries the top-level script: #{spec['script'].inspect}")
  round = JSON.parse(JSON.generate(host.codec.roundtrip.ta_scxml_spec(value: spec)))
  check(without_inferable_kinds(round) == spec, "scripted spec round-trips: #{round}")
  %w[script-twice script-in-state].each do |mistake|
    begin
      TinyactorsTest::Chart.define(:"bad_#{mistake}") do
        script '.a = 1'
        mistake == 'script-twice' ? script('.b = 1') : state(:s) { script '.c = 1' }
      end
      raise "bindings test failed: #{mistake} was accepted"
    rescue TinyactorsTest::ChartError => e
      check(e.message.include?('script'), e.message)
    end
  end
  error = rpc_error(-32_602) do
    host.codec.roundtrip.ta_scxml_state_spec(value: { kind: 'atomic', id: 's', states: [{ id: 'child' }] })
  end
  check(error.data == 'params.value.states[0]: element would terminate the list', error.data)

  # Handles: wrong type, unknown ID, and retired handles are rejected.
  definition = host.handle.fixture(type: 'definition')
  error = rpc_error(-32_602) { host.ta.system.destroy(system: definition) }
  check(error.data == %(params.system: expected a system handle, got "definition:#{definition[:$handle].split(':').last}"),
        "handle type error: #{error.data}")
  error = rpc_error(-32_602) { host.ta.system.destroy(system: { '$handle' => 'system:999' }) }
  check(error.data.include?('unknown handle'), error.data)
  system = host.handle.fixture(type: 'system')
  error = rpc_error(-32_602) { host.ta.system.destroy(system: system) }
  check(error.data.include?('cannot reach native code'), error.data)

  # The proxy validates argument names before sending.
  begin
    host.ta.actor.spawn(system: system, bogus: 1)
    raise 'bindings test failed: unknown argument accepted'
  rescue ArgumentError => error
    check(error.message.include?('bogus'), error.message)
  end
ensure
  host.close
end
puts 'tinyactors-test bindings test passed'
