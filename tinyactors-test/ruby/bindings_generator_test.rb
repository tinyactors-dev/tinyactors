# frozen_string_literal: true

# Tests the bindings generator's refusals: it must fail, naming the item,
# rather than bind something it cannot map. Each case generates from the real
# API description with one policy entry changed.

require 'json'
require 'pathname'
require 'tmpdir'

root = Pathname.new(__dir__).join('..').expand_path
require root.join('bindings').to_s
require root.join('mise/lib/rpc').to_s

DESCRIPTION = root.parent.join('tinyactors-core/dist/include/tinyactors.api.json')

def check(condition, message)
  raise "bindings generator test failed: #{message}" unless condition
end

# A copy of the policy with some constants replaced.
def policy(**changes)
  copy = Module.new
  TinyactorsBindings.constants.each do |name|
    copy.const_set(name, changes.fetch(name) { TinyactorsBindings.const_get(name) })
  end
  copy
end

def refuses(label, expected, description: DESCRIPTION, **changes)
  Rpc.generate(description, policy(**changes))
  raise "bindings generator test failed: #{label} was accepted"
rescue Rpc::Error => e
  check(e.message.include?(expected), "#{label}: expected #{expected.inspect} in:\n#{e.message}")
end

# The real policy generates.
output = Rpc.generate(DESCRIPTION, TinyactorsBindings)
check(output.source.include?('TAT_VALUE_RECEIVERS[]'), 'value receivers are generated from the value models')
check(output.source.include?('{"ta.actor.spawn", "definition", TAT_VALUE_MODEL_DEFINITION}'), 'spawn values follow the definition')
check(output.coverage['value_models'].key?('ta_actor_load'), 'the session-image fallback is reported')

skips = TinyactorsBindings::SKIP_FUNCTIONS
refuses('a stale skip entry', 'SKIP_FUNCTIONS ta_nothing_*', SKIP_FUNCTIONS: skips.merge('ta_nothing_*' => 'gone'))
refuses('a flattened member that collides', 'ta_scxml_action_spec.as.invoke: members kind collide',
        FLATTEN_UNION_MEMBERS: TinyactorsBindings::FLATTEN_UNION_MEMBERS.merge('ta_scxml_action_spec.invoke' => 'no'))
refuses('flattening a non-struct member', 'ta_actor_ref.as.runtime: only plain struct members can be flattened',
        FLATTEN_UNION_MEMBERS: TinyactorsBindings::FLATTEN_UNION_MEMBERS.merge('ta_actor_ref.runtime' => 'no'))
refuses('a skipped type a bound type needs', 'ta_actor_address is skipped in bindings.rb',
        SKIP_TYPES: TinyactorsBindings::SKIP_TYPES.merge('ta_actor_address' => 'test'))
refuses('an in-place pointer parameter', 'ta_arena_reset(arena): inout pointers cannot cross the protocol',
        SKIP_FUNCTIONS: skips.reject { |pattern, _| pattern == 'ta_arena_*' })
refuses('a returned pointer', 'ta_dm_host_event: returned pointers cannot cross the protocol',
        SKIP_FUNCTIONS: skips.reject { |pattern, _| pattern == 'ta_dm_host_*' })
refuses('an unresolvable value model', 'ta_actor_load: the host cannot find the data model of a session_image',
        VALUE_MODEL_FALLBACKS: {})
refuses('an unbound vtable', 'ta_io_vtable: adapter operations cannot cross the protocol',
        SKIP_TYPES: TinyactorsBindings::SKIP_TYPES.reject { |pattern, _| pattern == '*_vtable' })

Dir.mktmpdir do |directory|
  old = File.join(directory, 'old.json')
  File.write(old, JSON.generate(JSON.parse(File.read(DESCRIPTION)).merge('format' => 'tinyactors-api/1')))
  refuses('an older description format', 'this generator reads tinyactors-api/2', description: old)
end

puts 'tinyactors-test bindings generator test passed'
