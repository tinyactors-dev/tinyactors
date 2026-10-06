# frozen_string_literal: true

# Mail between resident sessions (core ADR 0099): a <send> to a session
# resident in the system takes an envelope from core's pool and waits on the
# sending macrostep's outgoing list until that macrostep ends, then joins the
# run queue. It never reaches the host's router. A send that finds no free
# envelope fails in the chart as error.execution.capacity.pool, and SCXML
# abandons the rest of its block, so a fanout fails once (S02).

# Sends `note` with each item of `.items` to its `peer`, in one <foreach>,
# then marks `.after`, which a failed iteration skips. Counts the capacity
# errors it gets.
TinyactorsTest.chart :mail_fanout do
  data items: { expr: '[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]' }
  data peer: nil
  data after: false
  data capacity: []

  state :ready do
    transition on: 'go', type: :internal do
      foreach '.items', item: 'n' do
        send_event 'note', to_expr: '.peer', data: '{n: .n}'
      end
      assign '.after', 'true'
    end
    transition on: 'error.execution.capacity', type: :internal do
      assign '.capacity', '.capacity + [$_event.name]'
    end
  end
end

# Keeps every `note` it receives, in order.
TinyactorsTest.chart :mail_sink do
  data notes: { expr: '[]' }

  state :listening do
    transition on: 'note', type: :internal do
      assign '.notes', '.notes + [$_event.data.n]'
    end
  end
end

TinyactorsTest.scenarios 'mail between resident sessions' do
  # Catches: a fanout that floods the chart with one error per send that
  # did not fit, that runs the rest of its block after the failure, that
  # loses the sends before it, or that takes an envelope core does not
  # have. In a pool of eight, the fanout's own `go` holds one envelope:
  # seven notes fit, the eighth iteration fails once, and the rest of the
  # block does not run.
  scenario 'a fanout fails once, at the first send without an envelope' do
    system_params(pool_envelopes: 8)
    router = recording_router
    sink = spawn :mail_sink
    sender = spawn :mail_fanout, binding: { peer: sink.session_uri }
    sender.deliver 'go'
    assert_equal ['error.execution.capacity.pool'], sender.data('.capacity'), 'one capacity error'
    assert_equal false, sender.data('.after'), 'the rest of the block was abandoned'
    assert_equal [1, 2, 3, 4, 5, 6, 7], sink.data('.notes'), 'the sends before the failure arrived, in order'
    assert_equal [], router.entries('submit'), 'mail to a resident session stays in core'
  end

  # Catches: mail visible to its receiver while its sender's macrostep has
  # not ended. Paused after one action, the sender has sent its first note:
  # the receiver has nothing to take until the macrostep ends.
  scenario 'mail waits for its sender\'s macrostep to end' do
    sink = spawn :mail_sink
    sender = spawn :mail_fanout, binding: { peer: sink.session_uri }
    assert_equal 'ok', sender.post('go')
    assert_equal 'paused', sender.resume(budget: 1000, action_budget: 3)
    assert_equal 'stable', sink.resume(budget: 1000), 'the receiver finds nothing to take'
    assert_equal [], sink.data('.notes')
    nil while sender.resume(budget: 1000, action_budget: 3) == 'paused'
    run_system('the notes, once the sender\'s macrostep ended')
    assert_equal (1..10).to_a, sink.data('.notes')
    assert_equal true, sender.data('.after')
  end
end
