# frozen_string_literal: true

# A named actor rebuilt by replaying its inputs before it is published (core
# ADR 0089): a replaying actor runs on its own clock, crosses no border, and
# publishing puts it on the route with what is still pending.

TinyactorsTest.chart :replay_timer do
  data seen: 0

  state :idle do
    transition on: 'e1', target: :waiting do
      assign '.seen', '.seen + 1'
      log 'armed'
      send_event 'timeout', delay: '5s'
    end
  end
  state :waiting do
    transition on: 'timeout', target: :timedout
  end
  state :timedout
end

TinyactorsTest.scenarios 'replay' do
  # Catches: a replay that sends, logs, or fires a timeout the live run
  # already took, or that loses what the live run reached.
  scenario 'a replay rebuilds an evicted actor from its inputs, and nothing crosses the border' do
    recording_log
    live = spawn :replay_timer, name: 'r'
    set_time 1000
    live.deliver 'e1'
    advance_time 5000 # the timeout fires at 6000 and is delivered
    assert_active live, :timedout
    assert_equal 1, logs.size
    live.destroy # evicted at 6000

    set_time 6000 + (7 * 24 * 3600 * 1000) # a week later
    replay = prepare :replay_timer, address: live.address, at: 0
    replay.deliver 'e1', time: 1000 # each input carries its logged time (core ADR 0094)
    assert_equal 0, replay.delayed_sends, 'the timeout is held, not pending'
    replay.deliver 'timeout', time: 6000 # time reached the timeout: it fired then
    assert_active replay, :timedout
    assert_equal 1, logs.size, 'the replay wrote no log'
    assert_equal 'err_invalid', replay.post('e1', time: 1000), 'a replaying clock never goes back'

    assert_equal 'ok', publish(replay, at: 6000)
    assert_equal ['ok', replay.handle], find(live.address), 'the address names the rebuilt actor'
    assert_equal 1, replay.data('.seen')
    assert_equal 0, replay.delayed_sends, 'nothing fires twice'
  end

  # Catches: a send still pending when the actor stopped that a replay drops
  # instead of sending once, late.
  scenario 'a send still pending when the actor stopped fires once, late, after publish' do
    memory_directory
    set_time 10_000
    replay = prepare :replay_timer, address: { family: { name: 'replay_timer' }, name: 'p' }, at: 0
    replay.deliver 'e1', time: 1000
    assert_equal 'ok', publish(replay, at: 3000), 'stopped at 3000; the timeout is due at 6000'
    advance_time 0
    assert_active replay, :timedout
  end
end
