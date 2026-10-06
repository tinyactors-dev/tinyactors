# frozen_string_literal: true

# Run-to-completion is semantic, not a C stack boundary (ADR 0009): a
# macrostep may pause after any microstep and continue with the next
# ta_run. A paused actor is still busy with its event (invariant 2): core
# admits a new event for it and holds it back until the macrostep ends, and
# internal events finish before the next external event (invariant 6).
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

# Five internal events over two microsteps, so an actor paused after any
# action usually has some queued (ADR 0064).
TinyactorsTest.chart :raiser do
  data log: { expr: '[]' }

  state :idle do
    transition on: 'go', target: :working do
      assign '.log', '.log + ["go"]'
      raise_event 'r1'
      raise_event 'r2'
      raise_event 'r3'
    end
  end
  state :working do
    transition on: 'r1' do
      assign '.log', '.log + ["r1"]'
      raise_event 'r4'
      raise_event 'r5'
    end
    transition on: 'r2' do
      assign '.log', '.log + ["r2"]'
    end
    transition on: 'r3' do
      assign '.log', '.log + ["r3"]'
    end
    transition on: 'r4' do
      assign '.log', '.log + ["r4"]'
    end
    transition on: 'r5', target: :finished do
      assign '.log', '.log + ["r5"]'
    end
  end
  state :finished
end

TinyactorsTest.scenarios 'pausing', chart: :relay do
  invariant('the log only grows by known entries') do |actor|
    actor.chart != :relay || (actor.data('.log') - ['go', 'n=5', 'counted', 'ping', 'extra']).empty?
  end

  # Catches: a pause that counts macrosteps instead of microsteps (the loop
  # would finish in one resume); a paused actor whose macrostep takes a new
  # external event (invariant 2), or that changes when one is admitted; an
  # event admitted while paused and then processed before the internal
  # `counted`; a self-sent `ping` visible before its macrostep ends.
  #
  # The `ping` waits on the macrostep's outgoing list until the macrostep
  # ends (core ADR 0099), so it arrives after the event admitted while
  # paused, and never reaches the router.
  scenario 'a paused macrostep holds new events back and resumes one microstep at a time' do
    router = recording_router
    relay = spawn
    assert_equal 'ok', relay.post('go')
    counts = []
    held = false
    loop do
      status = relay.resume(budget: 1)
      break if status == 'stable'

      assert_equal 'paused', status
      assert relay.inspect_actor[:macrostep_in_progress], 'a paused actor reports its macrostep in progress'
      counts << relay.data('.n')
      next if held || relay.data('.n') != 2

      before = relay.observe
      assert_equal 'ok', relay.post('extra')
      assert_equal before, relay.observe, 'an admitted event leaves the paused actor unchanged'
      held = true
    end
    assert held, 'the scenario offered an event while paused'
    assert_equal [0, 1, 2, 3, 4, 5], counts.uniq, 'each resume runs one microstep of the counting loop'

    assert_equal ['go', 'n=5', 'counted'], relay.data('.log'), 'the macrostep ended without the held event'
    run_system('the held event, then the self-sent ping')
    assert_equal ['go', 'n=5', 'counted', 'extra', 'ping'], relay.data('.log')
    assert_equal [], router.entries('submit'), 'the self-send stayed in core'
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
    assert_equal [], router.entries('submit'),
                 'core queued the self-send itself, when the macrostep ended, and ran it after (core ADR 0099)'
  end

  # Catches: a send between actors lost silently, retried, or queued past
  # core's pool when the pool has no room for it (core ADR 0099). In a pool
  # of eight, `go`, an event waiting for a bystander and six of the
  # filler's sends, fallen due, leave no envelope free: the self-send `go`
  # makes fails in the sender as error.execution.capacity.pool, once, the
  # rest of its block (raising `counted`) does not run, and the ping never
  # arrives.
  scenario 'a send the pool has no room for fails in its sender, and never arrives' do
    system_params(pool_envelopes: 8)
    filler = TinyactorsTest::Chart.define(:filler) do
      state :filling do
        on_entry do
          (1..6).each { |index| send_event 'fill', delay: '1s', id: "f#{index}" }
        end
      end
    end
    spawn filler
    router = recording_router
    relay = spawn
    bystander = spawn
    assert_equal 'ok', relay.post('go')
    assert_equal 'ok', bystander.post('extra')
    set_time(1000)
    due = core('ta_system_dispatch_due', ta.system.dispatch_due(system: system_handle, budget: 100))
    assert_equal [6, false], due.values_at(:dispatched, :more_due), 'the filler\'s sends fill the pool'
    recording_trace
    run_system('go, whose ping finds no room, then extra')
    errors = trace_records.select { |record| record[:kind] == 'raised' }
                          .map { |record| record.dig(:event, :name_text) }.grep(/\Aerror/)
    assert_equal ['error.execution.capacity.pool'], errors, 'the send failed once, in its sender'
    assert_equal ['go', 'n=5'], relay.data('.log'), 'the ping never arrived, and `counted` was never raised'
    assert_equal ['extra'], bystander.data('.log')
    assert_equal [], router.entries('submit')
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
  # raised event, or that loses a snapshot across resumes (ADR 0061); and a
  # pause at a microstep's start that runs part of it twice or not at all
  # (ADR 0086).
  scenario 'pausing inside executable content is invisible', exhaustive: true do
    variants = (1..6).to_h { |budget| ["action budget #{budget}", { resume_action_budget: budget }] }
    variants['action budget 1, resume budget 1'] = { resume_action_budget: 1, resume_budget: 1 }
    variants['every microstep start'] = { resume_pause_at_microstep_start: true }
    variants['every microstep start, action budget 1'] = { resume_pause_at_microstep_start: true,
                                                           resume_action_budget: 1 }
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
  # takes an event into its macrostep, or does not say it is inside a
  # microstep.
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
      assert_equal 'ok', nested.post('back') if inside == 1
    end
    assert inside > 20, "only #{inside} pauses were inside a microstep"
    # Only whole configurations: after the go microstep, and after the inner one.
    assert_equal [[:a], %i[p b]], seen.uniq
    assert_equal %i[p c], nested.active
    assert_equal ['x1', 'x2 1', 'one', 'x2 2', 'two', 'x3', 't 1', 't 1.1', 't 1.2', 't 2', 't 2.1', 't 2.2',
                  't 3', 't 3.1', 't 3.2', 'p 1', 'p 2', 'b1', 'b2 1', 'b2 2', 'tc', 'c1'], nested.data('.log'),
                 '`back` was held: taken inside the macrostep, b would have ignored it'
    run_system('the held back')
    assert_equal [:a], nested.active, '`back` was taken in c, after the macrostep'
    assert_equal 'back', nested.data('.log').last
  end

  # Catches: a paused actor whose inspection does not say where it stands,
  # or says something other than the PAUSED record its run wrote; a
  # debugger that attaches to it then has no position (ADR 0083).
  scenario 'inspection reports where a paused actor stands, as its PAUSED record did' do
    recording_trace(kinds: %i[paused])
    nested = spawn :nested_content
    assert_equal 'ok', nested.post('go')
    places = []
    while nested.resume(budget: 1000, action_budget: 1) == 'paused'
      record = trace_records.reverse.find { |entry| entry[:kind] == 'paused' }
      inspection = nested.inspect_actor
      # A record carries no atom (core ADR 0098): its key's family is 0.
      assert_equal record[:key].except(:family), inspection[:step].except(:family)
      assert_equal record[:position], inspection[:position]
      places << inspection.dig(:position, :at)
    end
    assert_equal %w[exit transition enter], places.uniq, 'it paused in every phase that runs content'
    idle = nested.inspect_actor
    assert_equal 'none', idle.dig(:position, :at), 'an idle actor stands nowhere'
    assert_equal 0, Integer(idle.dig(:step, :macrostep))
  end

  # Catches: a stop at a microstep's start that has already run its first
  # unit or exited a state, a position there that names anything but that
  # first unit, and a step from there that runs more than it (ADR 0086).
  scenario "a resume can stop before a microstep's first unit" do
    recording_trace(kinds: %i[paused])
    nested = spawn :nested_content
    assert_equal 'ok', nested.post('go')
    assert_equal 'paused', nested.resume(budget: 1000, pause_at_microstep_start: true)
    inspection = nested.inspect_actor
    assert inspection[:microstep_in_progress], 'it stopped inside the go microstep'
    assert_equal [], nested.data('.log'), 'no unit ran'
    assert_equal [:a], nested.active
    record = trace_records.reverse.find { |entry| entry[:kind] == 'paused' }
    assert_equal record[:key].except(:family), inspection[:step].except(:family)
    assert_equal record[:position], inspection[:position]
    position = inspection[:position]
    assert_equal 'exit', position[:at]
    assert_equal :a, state_name(nested.definition, position[:state])
    action = core('ta_definition_action', ta.definition.action(system: system_handle, definition: nested.definition,
                                                                action: position[:action])).fetch(:out)
    assert_equal ['onexit', position[:state], 0, 0], action.values_at(:block, :owner, :block_index, :index),
                 "it stands at the first action of a's first <onexit>"

    assert_equal 'paused', nested.resume(budget: 1000, action_budget: 1, pause_at_microstep_start: true)
    assert_equal ['x1'], nested.data('.log'), 'a step runs the first unit alone'
    starts = []
    microstep = nested.inspect_actor.dig(:step, :microstep)
    while nested.resume(budget: 1000, action_budget: 1, pause_at_microstep_start: true) == 'paused'
      view = nested.inspect_actor
      next if view.dig(:step, :microstep) == microstep

      microstep = view.dig(:step, :microstep)
      starts << [view.dig(:position, :at), nested.data('.log').last, nested.active]
    end
    assert_equal [['transition', 'b2 2', %i[p b]]], starts, 'the inner microstep stopped at its start'
    assert_equal %i[p c], nested.active
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

  # Catches: an internal queue that a paused actor gives back while events
  # are still in it, or that capture, unload and load lose or reorder
  # (ADR 0064). The lent queue returns to the system only when it is empty.
  scenario 'queued internal events survive pauses and residency changes' do
    baseline = spawn :raiser, name: 'baseline'
    baseline.deliver 'go'
    expected = baseline.data('.log')
    assert_equal %w[go r1 r2 r3 r4 r5], expected

    raiser = spawn :raiser, name: 'cycled'
    assert_equal 'ok', raiser.post('go')
    cycled = 0
    while raiser.resume(budget: 1000, action_budget: 1) == 'paused'
      next if raiser.observe[:internal_events].to_a.empty?

      cycle_residency(raiser)
      cycled += 1
    end
    assert cycled >= 4, "only #{cycled} pauses had internal events queued"
    assert_equal expected, raiser.data('.log')
    assert_equal baseline.active, raiser.active
    assert raiser.observe[:internal_events].to_a.empty?, 'the queue is empty once the macrostep ends'
  end
end
