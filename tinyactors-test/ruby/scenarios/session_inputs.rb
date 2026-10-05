# frozen_string_literal: true

# What core ADR 0089 changes for live sessions: a resume says whether its
# event was ignored (section 8), and generated invoke IDs are numbered by
# the session, with its generated send IDs (section 5), so a replay of the
# session numbers them as the original run did.

module SessionInputs
  SERVICE = 'urn:tinyactors:test:inputs-service'
end

TinyactorsTest.chart :inputs_door do
  state :closed do
    transition on: 'open', target: :opened
  end
  state :opened do
    transition on: 'close', target: :closed
  end
end

# A generated send ID, then a generated invoke ID, stored through
# idlocation, in that order.
TinyactorsTest.chart :inputs_numbering do
  data send_id: nil
  data invoke_id: nil

  state :waiting do
    on_entry do
      send_event 'later', delay: '3600s', id_location: '.send_id'
    end
    invoke type: SessionInputs::SERVICE, id_location: '.invoke_id'
  end
end

TinyactorsTest.scenarios 'session inputs' do
  # Catches: a resume that cannot tell a host whether its message was used,
  # which a replay needs to report history the current chart ignores.
  scenario 'a completed input says whether its external event was ignored' do
    door = spawn :inputs_door
    outcome = lambda do |event|
      assert_equal 'ok', door.post(event)
      response = core('ta_run', ta.run(system: system_handle, budget: 100, notice_capacity: 8))
      completed = response[:notices].select { |notice| notice[:kind] == 'input_completed' }
      assert_equal 1, completed.size
      assert_equal 'stable', completed.first[:status]
      completed.first[:ignored]
    end
    assert_equal true, outcome.call('close'), 'closed takes no close'
    assert_equal false, outcome.call('open')
    assert_active door, :opened
    assert_equal true, outcome.call('open'), 'opened takes no open'
    assert_equal false, outcome.call('close')
  end

  # Catches: `ignored` reported for a call that ended no macrostep, or
  # carried over from an earlier macrostep.
  scenario 'a run that takes no input reports nothing' do
    door = spawn :inputs_door
    response = core('ta_run', ta.run(system: system_handle, budget: 100, notice_capacity: 8))
    assert_equal 'idle', response[:status]
    assert_equal [], response[:notices].to_a, 'nothing was runnable, so nothing is reported'
  end

  # Catches: invoke IDs drawn from a counter shared by the whole system, so
  # a session's IDs depend on what every other session invoked.
  scenario 'generated invoke IDs are numbered per session, after its send IDs' do
    scripted_invoker(SessionInputs::SERVICE)
    first = spawn :inputs_numbering
    second = spawn :inputs_numbering
    assert_equal first.data('.invoke_id'), second.data('.invoke_id'),
                 'each session numbers its own invocations'
    assert_equal 'waiting.2', first.data('.invoke_id'),
                 'the send took 1 from the same counter, the invocation 2'
    assert_equal first.data('.send_id'), second.data('.send_id')
  end
end
