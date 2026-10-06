# frozen_string_literal: true

# Time is supplied, never read (invariant 14, ADR 0012): "Semantic time
# comes only from the supplied logical clock." Core calls no system clock,
# so nothing happens while the host is not looking, and a delayed send is
# due at an absolute logical time the host decides to reach.
#
# `advance_time` in this suite is `ta_system_set_time` plus a dispatch
# round, so these scenarios call the API directly where the point is what
# core does on its own.
TinyactorsTest.scenarios 'logical time', chart: :traffic_light do
  # Catches: a core that reads a wall clock - for the current time, for a
  # deadline, or to decide that a timer is due. Real time passes here and
  # nothing about the system may move.
  scenario 'real time passing changes nothing' do
    light = spawn
    before = { time: system_time, deadline: next_deadline, active: light.active,
               data: light.data, sends: light.sends }
    assert_equal 0, before[:time], 'the system starts at the initial_time it was given'
    assert_equal 30_000, before[:deadline], 'the light armed its first timer'

    sleep 0.25 # a quarter of a second of the host's time

    assert_equal before[:time], system_time, 'logical time did not move'
    assert_equal before[:deadline], next_deadline, 'the deadline did not move'
    due = dispatch_due(64)
    assert_equal [0, false], due.values_at(:dispatched, :more_due), 'nothing became due'
    assert_equal before[:active], light.active
    assert_equal before[:data], light.data
    assert_equal before[:sends], light.sends
  end

  # Catches: a clock that can be wound back, which would make a delayed
  # send due twice or never, and would let two captures of one actor
  # disagree about which is newer.
  scenario 'logical time never runs backwards' do
    spawn
    advance_time 10_000
    assert_equal 10_000, system_time

    assert_equal 'err_invalid', ta.system.set_time(system: system_handle, now: 9_999)[:result]
    assert_equal 'err_invalid', ta.system.set_time(system: system_handle, now: 0)[:result]
    assert_equal 10_000, system_time, 'a refused set_time left the clock alone'

    # Setting the time it already has is not moving backwards.
    assert_equal 'ok', ta.system.set_time(system: system_handle, now: 10_000)[:result]
    assert_equal 10_000, system_time
  end

  # Catches: a deadline that drifts with when the host looks at it, or a
  # timer that fires early. The light's timer is due at an absolute logical
  # time; stepping to one millisecond before it must change nothing, and
  # the step onto it must fire it exactly once.
  scenario 'a delayed send is due at its absolute logical time' do
    light = spawn
    assert_equal 30_000, next_deadline

    advance_time 29_999
    assert_active light, :red
    assert_equal 30_000, next_deadline, 'the deadline is absolute, not a remaining delay'

    advance_time 1
    assert_active light, :green
    assert_equal 55_000, next_deadline, 'green armed its own timer, 25s after it was entered'
  end

  # Catches: a jump that loses deadlines it passed, fires one of them
  # twice, or delivers them out of order. Three sends armed at once are
  # three absolute deadlines; a clock that jumps past all of them owes the
  # chart all three, oldest first.
  #
  # Note what is deliberately not asserted: a chart that arms its next
  # timer when the last one fires does not reach the same state as one
  # stepped through those deadlines, because the new delay counts from the
  # time the clock jumped to. That is the meaning of an absolute deadline,
  # not a bug.
  scenario 'a jump delivers every deadline it passes, once, in order' do
    chart = TinyactorsTest::Chart.define(:three_timers) do
      data log: { expr: '[]' }
      state :ready do
        on_entry do
          send_event 'first', delay: '10s', id: :a
          send_event 'second', delay: '20s', id: :b
          send_event 'third', delay: '30s', id: :c
        end
        transition on: 'first', type: :internal do assign '.log', '.log + ["first"]' end
        transition on: 'second', type: :internal do assign '.log', '.log + ["second"]' end
        transition on: 'third', type: :internal do assign '.log', '.log + ["third"]' end
      end
    end

    stepwise = spawn(chart)
    6.times { advance_time 10_000 }
    jumped = spawn(chart)
    advance_time 60_000

    assert_equal %w[first second third], stepwise.data('.log')
    assert_equal stepwise.data('.log'), jumped.data('.log'),
                 'the jump passed the same three deadlines the steps did'
    assert_equal [], jumped.sends, 'nothing is left armed'
    advance_time 60_000
    assert_equal %w[first second third], jumped.data('.log'), 'no deadline fires twice'
  end
end
