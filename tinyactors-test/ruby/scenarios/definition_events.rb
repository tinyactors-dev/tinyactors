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

  # The kinds a follower of the trace stream reads (ADR 0080).
  FOLLOWED = %i[spawned destroyed macrostep_begin macrostep_end invoked
                definition_registered definition_retired definition_freed].freeze

  # Both journals as one stream, in the order of the seq counter they
  # share: [source, kind, definition ID, actor, macrostep], where source is
  # :record (the trace sink) or :event (the definition sink), and a record's
  # kind drops its `definition_` prefix so the two sources compare.
  def self.stream(context)
    context.instance_exec do
      entries = @instruments['trace'].journal + @instruments['definitions'].journal
      entries.sort_by { |entry| Integer(entry[:seq]) }.filter_map do |entry|
        case entry[:op]
        when 'trace'
          [:record, entry[:kind].delete_prefix('definition_'), Integer(entry[:definition]),
           entry[:actor] && Integer(entry[:actor]), entry.dig(:key, :macrostep)]
        when 'definition'
          [:event, entry[:kind], Integer(entry[:definition]), nil, nil]
        end
      end
    end
  end

  # Every definition record, immediately followed by the sink's event for it.
  def self.paired?(stream)
    stream.each_with_index.all? do |(source, kind, definition), at|
      next true unless source == :record && %w[registered retired freed].include?(kind)

      stream[at + 1]&.first(3) == [:event, kind, definition]
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

  # Catches: definition records out of order with the actor records a
  # follower reads beside them (ADR 0080): a standby would spawn an actor of
  # a definition it has not seen, or drop a definition an actor still uses.
  # The caller registers its invoked child mid-run: that REGISTERED comes
  # inside the caller's bootstrap macrostep, before the child is spawned.
  scenario 'definition records sit in order with the actor records around them' do
    recording_definitions
    recording_trace(kinds: DefinitionEvents::FOLLOWED)
    parent = spawn :caller
    caller_id = definition(:caller)
    stream = DefinitionEvents.stream(self)
    assert DefinitionEvents.paired?(stream), "each record just before its event: #{stream.inspect}"

    at = ->(match) { stream.index { |entry| match.call(entry) } }
    registered_caller = at.call(->(e) { e.first(3) == [:record, 'registered', caller_id] })
    spawned_caller = at.call(->(e) { e[0] == :record && e[1] == 'spawned' && e[2] == caller_id })
    assert registered_caller && spawned_caller && registered_caller < spawned_caller,
           "the caller's definition before its SPAWNED: #{stream.inspect}"

    child = stream.find { |e| e[0] == :record && e[1] == 'registered' && e[2] != caller_id }
    assert child, "the invoked child's definition is registered: #{stream.inspect}"
    child_id = child[2]
    registered_child = stream.index(child)
    begin_bootstrap = at.call(->(e) { e[1] == 'macrostep_begin' && e[3] == parent.handle && e[4] == 1 })
    end_bootstrap = at.call(->(e) { e[1] == 'macrostep_end' && e[3] == parent.handle && e[4] == 1 })
    spawned_child = at.call(->(e) { e[0] == :record && e[1] == 'spawned' && e[2] == child_id })
    assert begin_bootstrap < registered_child && registered_child < end_bootstrap,
           "the child registered inside the caller's bootstrap macrostep: #{stream.inspect}"
    assert registered_child < spawned_child, "the child's definition before its SPAWNED: #{stream.inspect}"

    # Destroy: every DESTROYED, then FREED for each definition left.
    parent.post('finish')
    run_system('the child finishes, and the caller with it')
    mark = stream.size
    teardown!
    tail = DefinitionEvents.stream(self).drop(mark)
    records = tail.select { |e| e[0] == :record }
    last_destroyed = records.rindex { |e| e[1] == 'destroyed' }
    first_freed = records.index { |e| e[1] == 'freed' }
    assert first_freed, "destroy frees what is left: #{tail.inspect}"
    assert last_destroyed.nil? || last_destroyed < first_freed, "every DESTROYED before any FREED: #{tail.inspect}"
    assert_equal [caller_id, child_id].sort, records.select { |e| e[1] == 'freed' }.map { |e| e[2] }.sort
    assert DefinitionEvents.paired?(DefinitionEvents.stream(self)), 'each FREED record just before its event'
  end

  # Catches: records for kinds the sink did not ask for (ADR 0067 section
  # 2): a follower that wants actor records only would get definitions too.
  scenario 'a mask without the definition kinds gets no definition records' do
    recording_definitions
    recording_trace(kinds: %i[spawned destroyed])
    spawn :traffic_light
    kinds = @instruments['trace'].journal.select { |e| e[:op] == 'trace' }.map { |e| e[:kind] }
    assert_equal ['spawned'], kinds
    assert_equal ['registered'], definition_events.map(&:first), 'the definition sink still hears'
  end
end
