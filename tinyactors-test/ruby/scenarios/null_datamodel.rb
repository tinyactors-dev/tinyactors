# frozen_string_literal: true

# The SCXML Null data model (SCXML B.1), the minimum semantic profile
# (tinyactors-core/docs/internal/datamodel-null-profile.md): the whole
# expression language is the `In(state)` predicate in a condition, there are
# no data bindings, values, locations, scripts or iteration, and an actor of
# such a chart carries no data-model instance.
#
# Core ships it as ta_null_datamodel() (ADR 0057); the host exposes it as
# the `datamodel.null` adapter.

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

# Every construct that needs a data-model expression, one chart each. The
# profile has only In(state), so each is refused when the chart is defined
# (ADR 0057), not on first use. The charts are built with Chart.define and
# not registered, because a registered chart must compile (the export
# round-trip check compiles every one).
NULL_REFUSED = {
  assign: proc { transition on: 'go', type: :internal do assign '.x', '1' end },
  script: proc { transition on: 'go', type: :internal do script 'anything' end },
  foreach: proc { transition on: 'go', type: :internal do foreach '[1]', item: 'i' do log 'iteration' end end },
  send_data: proc { transition on: 'go', type: :internal do send_event 'out', data: '1' end },
  send_param: proc { transition on: 'go', type: :internal do send_event 'out', params: { p: '1' } end },
  condition: proc { transition on: 'go', cond: '1 + 1', target: :start }
}.to_h do |operation, transitions|
  [operation, TinyactorsTest::Chart.define(:"null_refuses_#{operation}", datamodel: 'null') do
    state :start, &transitions
  end]
end.freeze

# What is left at run time: operations that carry no expression but need
# data. Literal content has to be imported into the data model.
TinyactorsTest.chart :null_literal_content, datamodel: 'null' do
  state :start do
    transition on: 'send_content', type: :internal do
      send_event 'out', content: 'literal'
    end
    transition on: 'error.execution', target: :faulted
  end
  state :faulted
end

# A data declaration without an initializer has no expression either, and
# declaring it fails when the actor starts.
TinyactorsTest.chart :null_with_data, datamodel: 'null' do
  data :x

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

  # Catches: a chart the null model cannot run that is accepted anyway and
  # fails only when the operation first executes (ADR 0057). Every
  # expression other than In(state) is refused when the chart is defined.
  scenario 'every expression other than In() is refused when the chart is defined' do
    NULL_REFUSED.each do |operation, chart|
      spec = chart.to_spec
      response = ta.definition.compile_spec(system: system_handle, spec: spec,
                                            datamodel: { '$adapter' => 'datamodel.null' },
                                            allocator: allocator.adapter)
      assert_equal 'err_invalid', response[:result], "#{operation} compiled under the null data model"
    end
  end
  
  # Catches: a refusal that does not say which expression or why. Through
  # the codec, the compile error is located in the document (ADR 0058).
  scenario 'a refused expression is named, with its place in the document' do
    source = "<scxml xmlns=\"http://www.w3.org/2005/07/scxml\" name=\"c\" datamodel=\"null\">\n" \
             "  <state id=\"a\">\n    <transition event=\"go\" cond=\"1 + 1\" target=\"a\"/>\n  </state>\n</scxml>"
    response = ta.definition.import(system: system_handle, codec: { '$adapter' => 'codec.scxml' }, input: source,
                                    datamodel: { '$adapter' => 'datamodel.null' },
                                    allocator: { '$adapter' => 'allocator.libc' })
    assert_equal 'err_invalid', response[:result]
    error = response.dig(:out, :error)
    compile = error.dig(:as, :compile) || error[:compile]
    assert_equal %w[expression_rejected 1\ +\ 1], [compile[:code], compile[:subject]]
    assert_equal '3:34: states[0].transitions[0].condition: the null data model has only the In(state) predicate "1 + 1"',
                 TinyactorsTest.describe_import_error(ta, error)
  end
  
  # Catches: an operation with no expression that silently does nothing
  # instead of faulting, or one that faults as a communication error rather
  # than an execution error. SCXML says a failed executable-content
  # operation raises error.execution.
  scenario 'literal content is an execution error' do
    machine = spawn :null_literal_content
    machine.deliver 'send_content'
    assert_active machine, :faulted
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
