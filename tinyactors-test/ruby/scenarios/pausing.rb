# frozen_string_literal: true

# Run-to-completion is semantic, not a C stack boundary (ADR 0009): a
# macrostep may pause after any microstep and continue with the next
# ta_actor_resume. A paused actor is still busy with its event (invariant 2),
# and internal events finish before the next external event (invariant 6).
# Pausing must be invisible: a run that pauses after every microstep is
# observably identical to one that never pauses.
TinyactorsTest.scenarios 'pausing', chart: :relay do
  invariant('the log only grows by known entries') do |actor|
    actor.chart != :relay || (actor.data('.log') - ['go', 'n=5', 'counted', 'ping', 'extra']).empty?
  end

  # Catches: a pause that counts macrosteps instead of microsteps (the loop
  # would finish in one resume); a paused actor that accepts a new external
  # event (invariant 2), or whose refusal still changes it; an event that is
  # accepted while paused and then processed before the internal `counted`.
  #
  # The router holds the self-sent `ping`: delivered at once, it would sit in
  # the actor's single pending-event slot and make every later offer busy
  # for that reason alone, whether or not a paused macrostep refuses events.
  scenario 'a paused macrostep refuses new events and resumes one microstep at a time' do
    router = recording_router(mode: :hold)
    relay = spawn
    assert_equal 'ok', relay.post('go')
    counts = []
    refused = false
    loop do
      status = relay.resume(budget: 1)
      break if status == 'stable'

      assert_equal 'paused', status
      assert relay.inspect_actor[:macrostep_in_progress], 'a paused actor reports its macrostep in progress'
      counts << relay.data('.n')
      next if refused || relay.data('.n') != 2

      before = relay.observe
      assert_equal 'err_busy', relay.post('extra')
      assert_equal before, relay.observe, 'a refused event leaves the paused actor unchanged'
      refused = true
    end
    assert refused, 'the scenario offered an event while paused'
    assert_equal [0, 1, 2, 3, 4, 5], counts.uniq, 'each resume runs one microstep of the counting loop'

    assert_equal ['go', 'n=5', 'counted'], relay.data('.log')
    held = router.held
    assert_equal ['ping'], held.map { |delivery| delivery.dig(:event, :name_text) }
    assert_equal 'ok', router.release(held[0][:id])[:result]
    run_system('release the self-sent ping')
    relay.deliver 'extra'
    assert_equal ['go', 'n=5', 'counted', 'ping', 'extra'], relay.data('.log')
  end

  # Catches: inspection that loses (or misreports) the macrostep's external
  # event while the actor is paused, which a host needs to know what a
  # busy actor is working on.
  scenario 'every pause reports the external event of its macrostep' do
    relay = spawn
    relay.post('go')
    pauses = 0
    while relay.resume(budget: 1) == 'paused'
      pauses += 1
      current = relay.inspect_actor[:current_event]
      assert_equal 'go', current && event_text(current[:name]),
                   "pause #{pauses} (#{relay.active.join(', ')}, internal queue " \
                   "#{relay.observe[:internal_events].inspect})"
    end
    assert_equal 8, pauses
  end

  # Catches: an external self-send delivered into the sender's own
  # macrostep, ahead of the internal event raised after it (invariant 6).
  scenario 'an external self-send waits until the internal events are done' do
    router = recording_router
    relay = spawn
    relay.deliver 'go'
    assert_equal ['go', 'n=5', 'counted', 'ping'], relay.data('.log')
    assert_equal [%w[retry err_busy], %w[accepted ok]],
                 router.entries('submit').map { |entry| entry.values_at(:status, :result) }
  end

  # Catches: interpreter state that lives on the C stack or in scratch
  # memory between resumes (the continuation of ADR 0009): a pause would lose
  # or repeat a microstep, a raised event, a delayed send, or an invocation.
  scenario 'pausing after every microstep is invisible' do
    variants = { 'resume budget 1' => { resume_budget: 1 },
                 'resume budget 2' => { resume_budget: 2 },
                 'dispatch budget 1' => { dispatch_budget: 1 } }
    assert_equivalent_runs(variants: variants) do
      relay = spawn
      relay.deliver 'go'
      relay.deliver 'extra'
      light = spawn :traffic_light
      advance_time 30_000
      light.deliver 'fault'
      light.deliver 'repair'
      advance_time 30_000
      summing = spawn :foreach_sum
      assert summing.done?, 'foreach_sum reaches its top-level final state'
      caller = spawn :caller
      caller.deliver 'finish'
      assert_equal ['progress 21', 'done 42'], caller.data('.log')
    end
  end
end
