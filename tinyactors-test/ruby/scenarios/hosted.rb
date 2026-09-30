# frozen_string_literal: true

# Hosted mode (kernel ADR 0001 section 5): systems run by the host kernel
# (tinyactors-host) instead of by core calls a scenario makes one by one.
# The kernel queues, schedules and routes; the scenario runs turns and owns
# the clock.
TinyactorsTest.chart :hosted_ping do
  state :idle do
    transition on: 'go', target: :sent do
      send_event 'ping', to: '#_actor_hosted_pong/p'
    end
  end
  state :sent
end

TinyactorsTest.chart :hosted_pong do
  state :waiting do
    transition on: 'ping', target: :pinged
  end
  state :pinged
end

TinyactorsTest.chart :hosted_timer do
  state :idle do
    transition on: 'arm', target: :armed do
      send_event 'ring', delay: '5s'
    end
  end
  state :armed do
    transition on: 'ring', target: :rung
  end
  state :rung
end

TinyactorsTest.chart :hosted_box do
  data size: 0

  state :idle do
    transition on: 'put' do
      assign '.size', '$_event.data.blob | length'
    end
  end
end

TinyactorsTest.chart :hosted_seen do
  data seen: 0

  state :idle do
    transition on: 'e1', target: :waiting do
      assign '.seen', '.seen + 1'
      send_event 'timeout', delay: '5s'
    end
  end
  state :waiting do
    transition on: 'timeout', target: :timedout
  end
  state :timedout
end

TinyactorsTest.scenarios 'hosted' do
  # Catches: the test host not linking the kernel, or a hosted system that
  # leaks what the kernel allocated for it.
  scenario 'a hosted system opens and closes without leaking' do
    system = hosted_system
    assert_equal 0, close_hosted(system), 'the kernel freed everything'
  end

  # Catches: a hosted system that takes its memory from libc whatever the
  # scenario hands it, so a budget or an armed fault never reaches the
  # kernel; or a close that gives back less than the kernel took.
  scenario "a hosted system's memory comes from the scenario's allocator" do
    before = allocator.stats[:live_bytes]
    hosted = hosted_system
    assert allocator.stats[:live_bytes] > before, 'the kernel allocated through the counting allocator'
    assert_equal 0, close_hosted(hosted)
    assert_equal before, allocator.stats[:live_bytes], 'closing gave back everything'
  end

  # Catches: a system half-created under a budget, or one that keeps what
  # it had allocated when a later allocation fails.
  scenario 'a hosted system that cannot get its memory is not created' do
    before = allocator.stats[:live_bytes]
    AllocationFailure.tighten(self, headroom: 64)
    assert_equal 'err_nomem', hosted_system
    allocator.budget(0)
    assert_equal before, allocator.stats[:live_bytes], 'the refused create kept nothing'
    assert_equal 0, close_hosted(hosted_system), 'with room, the same create works'
  end

  # Catches: mail between actors that bypasses the kernel's mailboxes and
  # router, or a turn that does not run what was posted.
  scenario 'mail between two actors goes through the kernel' do
    hosted = hosted_system
    ping = hosted_spawn(hosted, hosted_define(hosted, :hosted_ping), family: 'hosted_ping', name: 'a')
    pong = hosted_spawn(hosted, hosted_define(hosted, :hosted_pong), family: 'hosted_pong', name: 'p')
    hosted_run(hosted)
    assert_equal 'ok', hosted_post(hosted, ping, 'go')
    assert hosted_run(hosted)[:quiet], 'the system is quiet after the mail was delivered'
    assert_equal [:sent], hosted_states(hosted, ping)
    assert_equal [:pinged], hosted_states(hosted, pong)
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a delayed send that the kernel's clock never makes due.
  scenario 'a delayed send fires when the scenario moves the clock' do
    hosted = hosted_system
    timer = hosted_spawn(hosted, hosted_define(hosted, :hosted_timer), family: 'hosted_timer', name: 't')
    hosted_run(hosted)
    hosted_post(hosted, timer, 'arm')
    hosted_run(hosted)
    assert_equal [:armed], hosted_states(hosted, timer)
    assert_equal 'ok', hosted_advance(hosted, 5000)
    hosted_run(hosted)
    assert_equal [:rung], hosted_states(hosted, timer)
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a log that misses an input, or a replay that loses the pending
  # timer: the rebuilt actor must be where the live one was, and its timeout
  # must still fire.
  scenario 'an evicted actor is rebuilt from its input log, with its timer' do
    hosted = hosted_system
    hosted_log(hosted, 'hosted_timer')
    definition = hosted_define(hosted, :hosted_timer)
    timer = hosted_spawn(hosted, definition, family: 'hosted_timer', name: 't')
    hosted_run(hosted)
    hosted_post(hosted, timer, 'arm')
    hosted_run(hosted)
    assert_equal 'ok', hosted_evict(hosted, timer)
    store = hosted_store(hosted)
    assert !store['hosted_timer/t'].empty?, 'the log holds records for the actor'
    rebuilt = hosted_restore(hosted, definition, 'hosted_timer', 't', store['hosted_timer/t'])
    assert_equal [:armed], hosted_states(hosted, rebuilt)
    hosted_advance(hosted, 5000)
    hosted_run(hosted)
    assert_equal [:rung], hosted_states(hosted, rebuilt)
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a value lost or cut on its way through the kernel's log and the
  # jq model's streaming codec.
  scenario 'a large value is logged and replayed through the kernel' do
    hosted = hosted_system
    hosted_log(hosted, 'hosted_box')
    definition = hosted_define(hosted, :hosted_box)
    box = hosted_spawn(hosted, definition, family: 'hosted_box', name: 'big')
    hosted_run(hosted)
    blob = 'v' * (1024 * 1024)
    assert_equal 'ok', hosted_post_data(hosted, box, 'put', { blob: blob })
    hosted_run(hosted)
    assert_equal blob.size, hosted_data(hosted, box, '.size')
    hosted_evict(hosted, box)
    log = hosted_store(hosted)['hosted_box/big']
    assert log.size / 2 > blob.size, 'the value is in the log'
    rebuilt = hosted_restore(hosted, definition, 'hosted_box', 'big', log)
    assert_equal blob.size, hosted_data(hosted, rebuilt, '.size')
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a checkpoint that does not hold the actor's state, or a log
  # compacted to it that loses its pending timer.
  scenario 'a log compacted to its checkpoint rebuilds the actor' do
    hosted = hosted_system
    hosted_log(hosted, 'hosted_seen')
    definition = hosted_define(hosted, :hosted_seen)
    live = hosted_spawn(hosted, definition, family: 'hosted_seen', name: 'k')
    hosted_run(hosted)
    hosted_advance(hosted, 1000)
    hosted_post(hosted, live, 'e1')
    hosted_run(hosted) # waiting; the timeout is due at 6000
    checkpoint = hosted_checkpoint(hosted, live)
    assert_equal 'ok', checkpoint[:result]
    hosted_evict(hosted, live)
    rebuilt = hosted_restore(hosted, definition, 'hosted_seen', 'k', checkpoint[:bytes]) # the checkpoint alone
    assert_equal [:waiting], hosted_states(hosted, rebuilt)
    assert_equal 1, hosted_data(hosted, rebuilt, '.seen'), 'the data came from the checkpoint'
    hosted_advance(hosted, 6000)
    hosted_run(hosted)
    assert_equal [:timedout], hosted_states(hosted, rebuilt)
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a checkpoint of an actor the log does not cover.
  scenario 'a checkpoint needs a logged, named actor' do
    hosted = hosted_system
    hosted_log(hosted, 'hosted_seen')
    unnamed = hosted_spawn(hosted, hosted_define(hosted, :hosted_seen))
    hosted_run(hosted)
    assert_equal 'err_invalid', hosted_checkpoint(hosted, unnamed)[:result]
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a log that cannot be replayed by another system, or a crash
  # that loses the actor's pending timer. The first system is dropped after
  # its batches were stored, as a crash would leave them.
  scenario 'after a crash, another system rebuilds the actor from the stored log' do
    first = hosted_system(incarnation: 1)
    hosted_log(first, 'hosted_seen')
    live = hosted_spawn(first, hosted_define(first, :hosted_seen), family: 'hosted_seen', name: 'c')
    hosted_run(first)
    hosted_advance(first, 1000)
    hosted_post(first, live, 'e1')
    hosted_run(first) # waiting; the timeout is due at 6000
    store = hosted_store(first)
    close_hosted(first)

    second = hosted_system(incarnation: 2)
    hosted_log(second, 'hosted_seen')
    definition = hosted_define(second, :hosted_seen)
    hosted_advance(second, 2000)
    rebuilt = hosted_restore(second, definition, 'hosted_seen', 'c', store['hosted_seen/c'])
    assert_equal [:waiting], hosted_states(second, rebuilt)
    assert_equal 1, hosted_data(second, rebuilt, '.seen')
    hosted_advance(second, 6000)
    hosted_run(second)
    assert_equal [:timedout], hosted_states(second, rebuilt)
    assert_equal 0, close_hosted(second)
  end
end
