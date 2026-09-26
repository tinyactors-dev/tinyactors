# frozen_string_literal: true

# The definition lifecycle, told to the host (core ADR 0079). A host keeps
# state per definition (a resolver, cached names, a watch) and releases it
# when core frees the definition, so the events must be exact: a
# registration answered by identity is not a new definition, a cascade frees
# children before the parent that nested them, a retired definition is
# freed with its last actor, and destroying the system frees what is left.
module DefinitionEvents
  # Compiles and registers `chart` again, as a host defining it twice would.
  def self.register_again(context, chart)
    context.instance_exec do
      compiled = core('ta_definition_compile_spec',
                      ta.definition.compile_spec(system: system_handle, spec: TinyactorsTest.charts[chart].to_spec,
                                                 datamodel: datamodel_adapter(TinyactorsTest.charts[chart]),
                                                 allocator: allocator.adapter))
      core('ta_system_register_definition',
           ta.system.register_definition(system: system_handle, definition: compiled.fetch(:out))).fetch(:out)
    end
  end
end

TinyactorsTest.scenarios 'definition events' do
  # Catches: REGISTERED fired for a registration core answered with the
  # definition it already had (ADR 0077): a host counting registrations, or
  # keeping one resolver per REGISTERED, would count or keep one too many.
  scenario 'a registration answered by identity fires nothing' do
    recording_definitions
    first = definition(:traffic_light)
    assert_equal [['registered', 'traffic_light', first]], definition_events
    again = DefinitionEvents.register_again(self, :traffic_light)
    assert_equal first, again, 'the same identity answers with the same definition'
    assert_equal [['registered', 'traffic_light', first]], definition_events, 'no second REGISTERED'
    # Its second hold is given back; the definition stays.
    core('ta_system_release_definition', ta.system.release_definition(system: system_handle, definition: again))
    assert_equal 1, definition_events.size
  end

  # Catches: a cascade that tells a parent's free before its child's, or
  # skips the child: a host would release the parent's state while it still
  # thinks the child is alive, or never release the child's.
  scenario 'freeing a parent frees what it nested, children first' do
    recording_definitions
    parent = definition(:caller)
    child = definition(:callee)
    core('ta_definition_nest', ta.definition.nest(system: system_handle, parent: parent, invoke: 0, child: child))
    # Only the parent holds the child now.
    core('ta_system_release_definition', ta.system.release_definition(system: system_handle, definition: child))
    mark = definition_events.size
    core('ta_system_retire_definition', ta.system.retire_definition(system: system_handle, definition: parent))
    events = definition_events.drop(mark)
    assert_equal [['retired', 'caller', parent], ['freed', 'callee', child], ['freed', 'caller', parent]], events,
                 'the parent is retired, then its child and the parent are freed, leaves first'
  end

  # Catches: FREED fired at retirement while an actor still runs the
  # definition, or never fired when the last actor goes.
  scenario 'a retired definition is freed with its last actor' do
    recording_definitions
    light = spawn :traffic_light
    id = definition(:traffic_light)
    core('ta_system_retire_definition', ta.system.retire_definition(system: system_handle, definition: id))
    assert_equal [%w[registered traffic_light], %w[retired traffic_light]], definition_events.map { |e| e.first(2) },
                 'retired, not freed, while an actor runs it'
    light.destroy
    assert_equal ['freed', 'traffic_light', id], definition_events.last, 'freed when the last actor went'
  end

  # Catches: a system destroy that frees definitions without telling the
  # host, which would leak whatever the host kept for them.
  scenario 'destroying the system frees every definition still alive' do
    recording_definitions
    ids = %i[traffic_light relay canceller].map { |chart| definition(chart) }
    spawn :relay
    mark = definition_events.size
    teardown!
    freed = definition_events.drop(mark)
    assert_equal ids.sort, freed.map(&:last).sort, 'every definition is freed'
    assert freed.all? { |event| event.first == 'freed' }, "only FREED at destroy: #{freed.inspect}"
  end
end
