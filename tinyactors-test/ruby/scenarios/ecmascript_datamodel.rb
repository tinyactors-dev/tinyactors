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

# An image's binding is a value of the data model its definition uses. The
# test host decoded the binding of ta.actor.load and ta.actor.reconstruct with
# jq, so an ECMAScript actor refused its binding (err_unsupported) and could
# never be paged back in. It now asks core which definition the image needs
# (ta_session_image_definition) and decodes with that definition's model.
TinyactorsTest.chart :ecma_counter, datamodel: 'ecmascript' do
  data v: 0

  state :counting do
    transition on: 'add', type: :internal do
      assign 'v', 'v + _event.data.m'
    end
  end
end

TinyactorsTest.scenarios 'ecmascript data model: residency' do
  scenario 'an ECMAScript actor loads with its binding and continues from it' do
    actor = spawn :ecma_counter, name: 'counter'
    actor.deliver 'add', data: { m: 5 }
    image = capture(actor)
    binding = actor.data
    assert_equal({ v: 5 }, binding)
    assert_equal 'ok', unload(actor, image)
    assert_equal 'ok', load(actor, image, binding: binding)
    assert_equal({ v: 5 }, actor.data, 'the binding restores the data')
    actor.deliver 'add', data: { m: 2 }
    assert_equal({ v: 7 }, actor.data, 'the restored actor continues from it')
    destroy_image(image)
  end

  scenario 'an ECMAScript actor reconstructs with its binding' do
    actor = spawn :ecma_counter
    actor.deliver 'add', data: { m: 4 }
    image = capture(actor)
    binding = actor.data
    copy = core('ta_actor_reconstruct',
                ta.actor.reconstruct(system: system_handle, image: image, datamodel_binding: binding)).fetch(:out)
    root = ta.actor.datamodel_root(system: system_handle, actor: copy)
    assert_equal 'ok', root[:result]
    assert_equal({ v: 4 }, root.dig(:out, :out), 'the reconstructed session has the binding')
    core('ta_actor_destroy', ta.actor.destroy(system: system_handle, actor: copy))
    destroy_image(image)
  end
end
