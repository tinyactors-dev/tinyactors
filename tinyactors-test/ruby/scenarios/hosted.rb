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

TinyactorsTest.scenarios 'hosted' do
  # Catches: the test host not linking the kernel, or a hosted system that
  # leaks what the kernel allocated for it.
  scenario 'a hosted system opens and closes without leaking' do
    system = hosted_system
    assert_equal 0, close_hosted(system), 'the kernel freed everything'
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
end
