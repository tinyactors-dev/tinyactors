# frozen_string_literal: true

# Definitions are immutable and shared (ADR 0003, invariant 13): "Actor
# execution never mutates the compiled definition." Every actor of a chart
# reads the same tables, so anything an actor wrote into them would change
# what its siblings run - and would change what `ta_definition_export`
# rebuilds from those tables (ADR 0042).
#
# The observable definition is its export, its identity, and its
# measurements. The run below exercises what an interpreter touches most:
# transitions, history, timers, data initializers, failing expressions, and
# an invocation with done data.
module DefinitionIdentity
  CHARTS = %i[traffic_light relay foreach_sum caller callee].freeze

  # Everything the definition is asked about, as one comparable value.
  def self.state(context)
    CHARTS.to_h do |chart|
      facts = context.definition_facts(chart)
      [chart, { export: context.export(chart),
                identity: facts.fetch(:identity),
                document_name: facts[:document_name],
                state_count: facts.fetch(:state_count),
                binding: facts.fetch(:binding),
                actor_fixed_bytes: facts.fetch(:actor_fixed_bytes),
                definition_bytes: facts.fetch(:definition_bytes) }]
    end
  end

  # Drives actors through the interpreter's write paths.
  def self.exercise(context)
    context.instance_exec do
      lights = Array.new(3) { spawn :traffic_light }
      advance_time 30_000
      lights[0].deliver 'fault'
      lights[0].deliver 'repair'
      advance_time 25_000
      lights[1].deliver 'fault'
      advance_time 60_000

      relay = spawn :relay
      relay.deliver 'go'
      relay.deliver 'extra'

      summing = spawn :foreach_sum
      assert summing.done?, 'foreach_sum reached its final state'

      caller = spawn :caller
      caller.deliver 'finish'
      assert_equal ['progress 21', 'done 42'], caller.data('.log')

      lights[2].destroy
    end
  end
end

TinyactorsTest.scenarios 'definition identity' do
  # Catches: an interpreter that writes back into the definition it runs - a
  # compiled transition patched at runtime, a history default rewritten when
  # history is recorded, a data initializer replaced by the value it
  # produced, a state marked active in definition-level storage. Any of
  # those changes the export, the revision, or the byte counts.
  scenario 'running actors leaves every definition exactly as it was compiled' do
    before = DefinitionIdentity.state(self)
    DefinitionIdentity.exercise(self)
    assert_equal before, DefinitionIdentity.state(self),
                 'every definition exports, measures, and identifies the same after a run'
  end

  # Catches: a revision that depends on anything but the chart - a counter,
  # a timestamp, an address, or state left over from an earlier compile.
  # Recompiling the same spec must land on the same revision, whether or not
  # actors have run, which is what lets a session image name its definition
  # (ADR 0038) and what `ta_system_find_definition` compares.
  scenario 'recompiling a chart after a run yields the same revision' do
    original = definition_facts(:traffic_light).fetch(:identity)
    DefinitionIdentity.exercise(self)

    spec = TinyactorsTest.charts[:traffic_light].to_spec
    compiled = core('ta_definition_compile_spec',
                    ta.definition.compile_spec(system: system_handle, spec: spec,
                                               datamodel: { '$adapter' => 'datamodel.jq' },
                                               allocator: allocator.adapter))
    again = core('ta_system_register_definition',
                 ta.system.register_definition(system: system_handle, definition: compiled.fetch(:out))).fetch(:out)
    assert_equal original, definition_facts(again).fetch(:identity)
    # The identity finds a registered definition; the most recent wins.
    found = ta.system.find_definition(system: system_handle, identity: original)
    assert_equal ['ok', again], [found[:result], found.dig(:out, :out)]
  end

  # Catches: measurements that drift with use - a per-actor cost that grows
  # as actors come and go, or definition storage that an actor extends.
  scenario 'the definition measurements do not move as actors come and go' do
    facts = definition_facts(:traffic_light)
    fixed = facts.fetch(:actor_fixed_bytes)
    bytes = facts.fetch(:definition_bytes)

    5.times do
      actors = Array.new(4) { spawn :traffic_light }
      advance_time 30_000
      actors.each(&:destroy)
    end

    after = definition_facts(:traffic_light)
    assert_equal [fixed, bytes], after.values_at(:actor_fixed_bytes, :definition_bytes)
    assert_equal 0, after.fetch(:actor_count), 'no actor of the definition is left'
  end
end
