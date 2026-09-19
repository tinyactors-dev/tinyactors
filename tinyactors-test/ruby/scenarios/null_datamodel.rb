# frozen_string_literal: true

# The SCXML Null data model (SCXML B.1), the minimum semantic profile
# (tinyactors-core/docs/internal/datamodel-null-profile.md): the whole
# expression language is the `In(state)` predicate in a condition, there are
# no data bindings, values, locations, scripts or iteration, and an actor of
# such a chart carries no data-model instance.
#
# The host implements it as the `datamodel.null` adapter
# (tinyactors-test/src/null_datamodel.c).

# The shape of W3C IRP test436: In() must see the sibling region of a
# parallel state, not just the region the transition is in.
TinyactorsTest.chart :null_parallel, datamodel: 'null' do
  parallel :p do
    state :ps0 do
      transition cond: "In('s1')", target: :wrong_sibling
      transition cond: "In('ps1')", target: :right
      transition target: :wrong_default
    end
    state :ps1
  end

  state :s1
  state :right
  state :wrong_sibling
  state :wrong_default
end

# One condition per event, each answered by which state the machine lands
# in: a condition exists only inside a transition, so this is how a
# scenario evaluates one.
TinyactorsTest.chart :null_predicates, datamodel: 'null' do
  parallel :p do
    state :ps0 do
      transition on: 'inactive', cond: "In('s1')", target: :yes
      transition on: 'inactive', target: :no
      transition on: 'unknown', cond: "In('no_such_state')", target: :yes
      transition on: 'unknown', target: :no
      transition on: 'active', cond: "In('ps1')", target: :yes
      transition on: 'active', target: :no
      transition on: 'spacing', cond: "In( \"ps1\" )", target: :yes
      transition on: 'spacing', target: :no
    end
    state :ps1
  end

  state :s1
  state :yes
  state :no
end

# Every operation the profile does not support, one per event.
TinyactorsTest.chart :null_unsupported, datamodel: 'null' do
  state :start do
    transition on: 'assign', type: :internal do
      assign '.x', '1'
    end
    transition on: 'script', type: :internal do
      script 'anything'
    end
    transition on: 'foreach', type: :internal do
      foreach '[1]', item: 'i' do
        log 'iteration'
      end
    end
    transition on: 'send_data', type: :internal do
      send_event 'out', data: '1'
    end
    transition on: 'send_content', type: :internal do
      send_event 'out', content: 'literal'
    end
    transition on: 'send_param', type: :internal do
      send_event 'out', params: { p: '1' }
    end
    transition on: 'bad_condition', cond: '1 + 1', target: :never_reached
    transition on: 'error.execution', target: :faulted
  end

  state :never_reached
  state :faulted
end

# A data declaration is itself unsupported, and fails when the actor starts.
TinyactorsTest.chart :null_with_data, datamodel: 'null' do
  data x: 0

  state :start do
    transition on: 'error.execution', target: :faulted
  end

  state :faulted
end

TinyactorsTest.scenarios 'null data model', chart: :null_parallel do
  # Catches: In() that only sees the region it is evaluated in, that answers
  # from the transition's source state, or that ignores parallel siblings.
  # This is the assertion of mandatory W3C test436.
  scenario 'In() sees the whole configuration, parallel siblings included' do
    machine = spawn
    assert_active machine, :right
    refute_active machine, :wrong_sibling, :wrong_default
  end

  # Catches: In() answering true for a state that is merely known to the
  # chart, or erroring on a state the chart does not have.
  scenario 'In() is false for an inactive state and for an unknown one' do
    answers = %w[inactive unknown active spacing].to_h do |question|
      machine = spawn :null_predicates
      machine.deliver question
      [question, machine.active.include?(:yes)]
    end
    assert_equal({ 'inactive' => false, 'unknown' => false, 'active' => true, 'spacing' => true }, answers)
  end

  # Catches: an operation that silently does nothing instead of faulting, or
  # one that faults as a communication error rather than an execution error.
  # The profile says the data model rejects them; SCXML says a failed
  # executable-content operation raises error.execution.
  scenario 'every operation that needs data is an execution error' do
    %w[assign script foreach send_data send_content send_param bad_condition].each do |operation|
      machine = spawn :null_unsupported
      machine.deliver operation
      assert_active machine, :faulted
      refute_active machine, :never_reached
    end
  end

  # Catches: a data declaration accepted silently, so a chart would look as
  # if it had data that no expression can ever read.
  scenario 'a data declaration faults when the actor starts' do
    machine = spawn :null_with_data
    assert_active machine, :faulted
  end

  # Catches: an inspection root invented for a model that has no data.
  scenario 'the null model offers no inspection root' do
    machine = spawn
    assert_equal 'err_unsupported',
                 ta.actor.datamodel_root(system: system_handle, actor: machine.handle)[:result]
  end

  # Catches: a data-model instance allocated for a profile that stores
  # nothing. The same chart under jq pays for its root object; under null it
  # pays only core's own per-actor storage.
  scenario 'an actor of a null-model chart carries no data-model instance' do
    spawn
    null_bytes = definition_facts(definition).fetch(:actor_fixed_bytes)

    spec = TinyactorsTest.charts[:null_parallel].to_spec.merge(datamodel: 'jq')
    compiled = core('ta_definition_compile_spec',
                    ta.definition.compile_spec(system: system_handle, spec: spec,
                                               datamodel: { '$adapter' => 'datamodel.jq' },
                                               allocator: allocator.adapter))
    jq_id = core('ta_system_register_definition',
                 ta.system.register_definition(system: system_handle, definition: compiled.fetch(:out))).fetch(:out)
    jq_bytes = definition_facts(jq_id).fetch(:actor_fixed_bytes)

    assert null_bytes < jq_bytes,
           "the same chart costs #{null_bytes} bytes per actor under null and #{jq_bytes} under jq"
  end

  # Catches: a compiler that ignores the <scxml datamodel> attribute, which
  # would run a chart against an expression language it was not written in.
  scenario 'a chart that requires the null model cannot compile with another adapter' do
    spec = TinyactorsTest.charts[:null_parallel].to_spec
    response = ta.definition.compile_spec(system: system_handle, spec: spec,
                                          datamodel: { '$adapter' => 'datamodel.jq' },
                                          allocator: allocator.adapter)
    assert_equal 'err_unsupported', response[:result]

    jq_spec = TinyactorsTest.charts[:relay].to_spec
    mismatched = ta.definition.compile_spec(system: system_handle, spec: jq_spec,
                                            datamodel: { '$adapter' => 'datamodel.null' },
                                            allocator: allocator.adapter)
    assert_equal 'err_unsupported', mismatched[:result]
  end
end
