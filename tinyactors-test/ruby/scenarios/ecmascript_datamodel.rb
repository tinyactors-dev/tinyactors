# frozen_string_literal: true

# The ECMAScript data model (datamodel.ecmascript) at the RPC and invoke
# boundaries. Values cross between sessions only through the codec, and the
# invoker refuses a child whose data model differs from its parent's: the
# parent sees error.execution and no child actor is spawned.

TinyactorsTest.chart :ecma_child_for_jq, datamodel: 'ecmascript' do
  state :running
end

TinyactorsTest.chart :jq_parent_of_ecma do
  data refused: false
  state :waiting do
    invoke chart: :ecma_child_for_jq
    transition on: 'error.execution', target: :refused do
      assign '.refused', 'true'
    end
    transition on: 'done.invoke', target: :started
  end
  final :refused
  final :started
end

TinyactorsTest.chart :jq_child_for_ecma do
  state :running
end

TinyactorsTest.chart :ecma_parent_of_jq, datamodel: 'ecmascript' do
  data refused: { expr: 'false' }
  state :waiting do
    invoke chart: :jq_child_for_ecma
    transition on: 'error.execution', target: :refused do
      assign 'refused', 'true'
    end
    transition on: 'done.invoke', target: :started
  end
  final :refused
  final :started
end

# No `datamodel`: the child inherits its ECMAScript parent's data model.
TinyactorsTest.chart :ecma_child, datamodel: nil do
  data :items, :label
  state :work do
    on_entry do
      send_event 'report', to: '#_parent',
                           data: '({isarr: items instanceof Array, sum: items[0] + items[1] + items[2], label: label})'
    end
    transition target: :finished
  end
  final :finished do
    done_data expr: '({finished: label})'
  end
end

TinyactorsTest.chart :ecma_parent, datamodel: 'ecmascript' do
  data :report, :done
  state :waiting do
    invoke chart: :ecma_child, params: { items: '[1, 2, 3]', label: "'hi'" }
    transition on: 'report' do
      assign 'report', '_event.data'
    end
    transition on: 'done.invoke', target: :finished do
      assign 'done', '_event.data'
    end
  end
  final :finished
end

TinyactorsTest.chart :ecma_receiver, datamodel: 'ecmascript' do
  data :isarr, :total
  state :idle do
    transition on: 'go' do
      assign 'isarr', '_event.data instanceof Array'
      assign 'total', '_event.data[0] + _event.data[1]'
    end
  end
end

TinyactorsTest.scenarios 'ecmascript data model' do
  scenario 'a jq parent cannot invoke an ECMAScript child' do
    parent = spawn :jq_parent_of_ecma
    assert parent.done?, 'the parent finished'
    assert_equal true, parent.data('.refused')
    assert_equal 1, rpc('system.actors', system: system_handle).size, 'no child actor was spawned'
  end

  scenario 'an ECMAScript parent cannot invoke a jq child' do
    parent = spawn :ecma_parent_of_jq
    assert parent.done?, 'the parent finished'
    assert_equal true, parent.data('.refused')
    assert_equal 1, rpc('system.actors', system: system_handle).size, 'no child actor was spawned'
  end

  scenario 'an ECMAScript parent passes params and receives #_parent data and donedata' do
    parent = spawn :ecma_parent
    assert parent.done?, 'the parent finished'
    assert_equal({ isarr: true, sum: 6, label: 'hi' }, parent.data('.report'))
    assert_equal({ finished: 'hi' }, parent.data('.done'))
  end

  scenario 'RPC event data arrives as values of the receiving session' do
    actor = spawn :ecma_receiver
    actor.deliver(:go, data: [1, 2])
    assert_equal true, actor.data('.isarr')
    assert_equal 3, actor.data('.total')
  end
end
