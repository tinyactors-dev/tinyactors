# frozen_string_literal: true

# Run-to-completion is semantic, not a C stack boundary (ADR 0009): a
# macrostep may pause after any microstep and continue with the next
# ta_actor_resume. A paused actor is still busy with its event (invariant 2),
# and internal events finish before the next external event (invariant 6).
# Pausing must be invisible: a run that pauses after every microstep is
# observably identical to one that never pauses. With an action budget
# (ADR 0061) the same holds for pauses inside executable content.

# Nested <if> and <foreach> in exit handlers, transition content and entry
# handlers, outside the bootstrap, where an action budget can stop them
# (ADR 0061).
TinyactorsTest.chart :nested_content do
  data log: { expr: '[]' }

  state :a do
    on_exit do
      assign '.log', '.log + ["x1"]'
      conditional do
        branch('1 == 1') do
          foreach '[1, 2]', item: 'i' do
            assign '.log', '.log + ["x2 \(.i)"]'
            conditional do
              branch('.i == 2') { assign '.log', '.log + ["two"]' }
              otherwise { assign '.log', '.log + ["one"]' }
            end
          end
        end
      end
    end
    on_exit { assign '.log', '.log + ["x3"]' }
    transition on: 'go', target: :p do
      foreach '[1, 2, 3]', item: 'j' do
        assign '.log', '.log + ["t \(.j)"]'
        foreach '[1, 2]', item: 'k' do
          assign '.log', '.log + ["t \(.j).\(.k)"]'
        end
      end
      raise_event 'inner'
    end
  end

  state :p, initial: :b do
    on_entry do
      foreach '[1, 2]', item: 'm' do
        assign '.log', '.log + ["p \(.m)"]'
      end
    end
    state :b do
      on_entry { assign '.log', '.log + ["b1"]' }
      on_entry do
        foreach '[1, 2]', item: 'n' do
          assign '.log', '.log + ["b2 \(.n)"]'
        end
      end
      transition on: 'inner', target: :c do
        assign '.log', '.log + ["tc"]'
      end
    end
    state :c do
      on_entry { assign '.log', '.log + ["c1"]' }
      transition on: 'back', target: :a do
        assign '.log', '.log + ["back"]'
      end
    end
  end
end

# An invoked child whose `work` microstep is long enough to pause inside,
# and a parent that cancels it by leaving the invoking state (ADR 0061
# section 6).
TinyactorsTest.chart :slow_child do
  state :idle do
    on_exit { log 'idle exit' }
    transition on: 'work', target: :busy
  end
  state :busy, initial: :inner do
    on_exit { log 'busy exit' }
    state :inner do
      on_entry do
        foreach '[1, 2, 3]', item: 'i' do
          log 'step'
        end
      end
      on_exit { log 'inner exit' }
    end
  end
end

TinyactorsTest.chart :child_canceller do
  state :running do
    invoke chart: :slow_child, id: :worker
    transition on: 'stop', target: :stopped
  end
  state :stopped
end

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

  # Catches: a pause inside executable content that repeats or skips an
  # action, a <foreach> iteration, a handler, a state's activation or a
  # raised event, or that loses a snapshot across resumes (ADR 0061).
  scenario 'pausing inside executable content is invisible' do
    variants = (1..6).to_h { |budget| ["action budget #{budget}", { resume_action_budget: budget }] }
    variants['action budget 1, resume budget 1'] = { resume_action_budget: 1, resume_budget: 1 }
    assert_equivalent_runs(variants: variants) do
      nested = spawn :nested_content
      nested.deliver 'go'
      nested.deliver 'back'
      nested.deliver 'go'
      relay = spawn
      relay.deliver 'go'
      light = spawn :traffic_light
      advance_time 30_000
      light.deliver 'fault'
      light.deliver 'repair'
      caller = spawn :caller
      caller.deliver 'finish'
      assert_equal ['progress 21', 'done 42'], caller.data('.log')
    end
  end

  # Catches: an actor paused inside a microstep that reports a half-changed
  # configuration instead of the one after its last completed microstep,
  # accepts an event, or does not say it is inside a microstep.
  scenario 'an actor paused inside a microstep is busy and reports its last configuration' do
    nested = spawn :nested_content
    assert_equal [:a], nested.active
    assert_equal 'ok', nested.post('go')
    inside = 0
    seen = []
    while nested.resume(budget: 1000, action_budget: 1) == 'paused'
      inspection = nested.inspect_actor
      assert inspection[:macrostep_in_progress], 'a paused actor is in a macrostep'
      next unless inspection[:microstep_in_progress]

      inside += 1
      seen << nested.active
      assert_equal 'err_busy', nested.post('back')
    end
    assert inside > 20, "only #{inside} pauses were inside a microstep"
    # Only whole configurations: after the go microstep, and after the inner one.
    assert_equal [[:a], %i[p b]], seen.uniq
    assert_equal %i[p c], nested.active
    assert_equal ['x1', 'x2 1', 'one', 'x2 2', 'two', 'x3', 't 1', 't 1.1', 't 1.2', 't 2', 't 2.1', 't 2.2',
                  't 3', 't 3.1', 't 3.2', 'p 1', 'p 2', 'b1', 'b2 1', 'b2 2', 'tc', 'c1'], nested.data('.log')
  end
  # Catches: a cancelled child paused inside a microstep that exits the
  # configuration from before that microstep (running `idle exit` a second
  # time and never exiting the states it had entered), or that leaks its
  # paused frames.
  scenario 'cancelling a child paused inside a microstep exits its live states' do
    recording_log
    parent = spawn :child_canceller
    child = invoked_actors.fetch(0)
    assert_equal 'ok', child.post('work')
    assert_equal 'paused', child.resume(budget: 1000, action_budget: 2)
    assert child.inspect_actor[:microstep_in_progress], 'the child paused inside its entry handler'
    assert_logs ['idle exit', nil]

    assert_equal 'ok', parent.post('stop')
    assert_equal 'stable', parent.resume(budget: 1000)
    assert_logs ['idle exit', nil], ['inner exit', nil], ['busy exit', nil]
    assert_equal %i[stopped], parent.active
    run_system('reclaim the cancelled child')
    assert_equal [], invoked_actors
  end
end
