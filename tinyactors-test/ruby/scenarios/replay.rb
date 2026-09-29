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
    assert_equal 'ok', set_actor_time(replay, 1000)
    replay.deliver 'e1'
    assert_equal 0, replay.delayed_sends, 'the timeout is held, not pending'
    assert_equal 'ok', set_actor_time(replay, 6000), 'time reached the timeout: it fired then'
    replay.deliver 'timeout'
    assert_active replay, :timedout
    assert_equal 1, logs.size, 'the replay wrote no log'
    assert_equal 'err_invalid', set_actor_time(replay, 1000), 'a replaying clock never goes back'

    assert_equal 'ok', publish(replay)
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
    assert_equal 'ok', set_actor_time(replay, 1000)
    replay.deliver 'e1'
    assert_equal 'ok', set_actor_time(replay, 3000), 'stopped at 3000; the timeout is due at 6000'
    assert_equal 'ok', publish(replay)
    advance_time 0
    assert_active replay, :timedout
  end
end

TinyactorsTest.scenarios 'input log' do
  # Catches: a log core wrote that does not rebuild the actor, or a replay
  # that sends, logs or fires what the live run already did.
  scenario "core logs a named actor's inputs, and replaying the log rebuilds it" do
    recording_log
    recording_inputs :replay_timer
    live = spawn :replay_timer, name: 'r'
    set_time 1000
    live.deliver 'e1'
    advance_time 5000 # the timeout fires at 6000 and is delivered
    assert_active live, :timedout
    assert_equal %w[spawned event fired event], input_kinds
    evict live # at 6000

    set_time 6000 + (7 * 24 * 3600 * 1000) # a week later
    replay = replay_log :replay_timer, address: live.address, piece: 7
    assert_active replay, :timedout
    assert_equal 1, logs.size, 'the replay wrote no log'
    assert_equal %w[spawned event fired event evicted], input_kinds, 'a replaying actor writes no record'
    assert_equal 'ok', publish(replay)
    assert_equal 1, replay.data('.seen')
    assert_equal 0, replay.delayed_sends, 'nothing fires twice'
  end

  # Catches: a checkpoint that does not hold the actor's state, or a log
  # compacted to it that loses what came after.
  scenario 'a log compacted to its checkpoint rebuilds the actor' do
    recording_log
    recording_inputs :replay_timer
    live = spawn :replay_timer, name: 'k'
    set_time 1000
    live.deliver 'e1' # waiting, the timeout due at 6000
    set_time 2000
    assert_equal 'ok', checkpoint(live)
    assert_equal %w[spawned event checkpoint], input_kinds
    compact_inputs 2 # the store lets go of what the checkpoint covers
    advance_time 4000 # the timeout fires at 6000 and is delivered
    assert_active live, :timedout
    evict live

    set_time 6000 + (7 * 24 * 3600 * 1000) # a week later
    replay = replay_log :replay_timer, address: live.address, piece: 7
    assert_active replay, :timedout
    assert_equal 1, logs.size, 'the replay wrote no log'
    assert_equal 'ok', publish(replay)
    assert_equal 1, replay.data('.seen'), 'the data came from the checkpoint'
    assert_equal 0, replay.delayed_sends, 'nothing fires twice'
  end

  # Catches: a checkpoint taken while the actor is not idle.
  scenario 'a checkpoint needs an idle, logged, named actor' do
    recording_inputs :replay_timer
    live = spawn :replay_timer, name: 'b'
    assert_equal 'ok', live.post('e1')
    assert_equal 'err_busy', checkpoint(live)
    run_system('take e1')
    assert_equal 'ok', checkpoint(live)
    anonymous = spawn :replay_timer
    assert_equal 'err_invalid', checkpoint(anonymous)
  end

  # Catches: an input taken although its record was refused.
  scenario 'an input whose record the sink refuses changes nothing' do
    inputs = recording_inputs :replay_timer
    live = spawn :replay_timer, name: 'q'
    inputs.arm_fault(:begin)
    assert_equal 'err_nomem', live.post('e1')
    assert_active live, :idle
    assert_equal %w[spawned], input_kinds
    live.deliver 'e1'
    assert_active live, :waiting
    assert_equal %w[spawned event], input_kinds
  end
end
