# frozen_string_literal: true

# Operator transitions and abandoning a macrostep (core ADR 0082).
#
# An operator moves a live actor to states it names, in a chart nobody
# prepared for it: core builds one transition from the actor's
# configuration and takes it through the normal engine, as the first
# microstep of the macrostep of the event it came with. Exits, history,
# invocations, entries, defaults and done events all happen as for a
# chart's transition, and `_event` is the operator's event.
#
# A macrostep that never becomes stable answers busy to everything, a jump
# included. ta_actor_abandon ends it between two microsteps and leaves the
# actor idle with the configuration after its last completed microstep.

# Nothing in this chart was written for an operator.
#
#   idle --go--> work (w1 --next--> w2; wh: deep history, default w1)
#                par  (r1: r1a | r1b, r2: r2a | r2b)
#   idle --spin-> loop (l1 <-> l2, eventless, forever; l1 raises tick)
#                end  (final, with done data)
TinyactorsTest.chart :operator do
  data log: { expr: '[]' }

  state :idle do
    transition on: 'go', target: :work
    transition on: 'spin', target: :loop
  end
  state :work do
    on_entry { assign '.log', '.log + ["enter work \($_event.name)"]' }
    on_exit { assign '.log', '.log + ["exit work \($_event.name)"]' }
    state :w1 do
      on_entry do
        foreach '[1, 2]', item: 'i' do
          assign '.log', '.log + ["w1 \(.i) \($_event.name)"]'
        end
      end
      transition on: 'next', target: :w2
    end
    state :w2 do
      on_entry { assign '.log', '.log + ["enter w2 \($_event.name)"]' }
    end
    history :wh, default: :w1, type: :deep
  end
  parallel :par do
    state :r1 do
      state :r1a
      state :r1b
    end
    state :r2 do
      state :r2a do
        on_exit { assign '.log', '.log + ["exit r2a"]' }
      end
      state :r2b
    end
  end
  state :loop do
    state :l1 do
      on_entry { raise_event 'tick' }
      transition target: :l2
    end
    state :l2 do
      transition target: :l1
    end
  end
  final :end do
    done_data expr: '.log | length'
  end
end

module Operator
  # Moving a stopped actor out of residency and back writes to the
  # directory, which a run that failed nothing did not.
  WITHOUT_DIRECTORY = ->(state) { state.merge(journals: state[:journals].except('directory')) }

  # The internal events an actor holds, waiting to be taken.
  def self.waiting(actor) = Array(actor.inspect_actor[:internal_events]).size

  # An actor spinning in `loop`, paused between two microsteps.
  def self.spinning(context, name: nil)
    actor = context.spawn(:operator, name: name)
    context.assert_equal 'ok', actor.post('spin')
    context.assert_equal 'paused', actor.resume(budget: 25)
    actor
  end
end

TinyactorsTest.scenarios 'operator transitions', chart: :operator do
  # Catches: a jump that writes the configuration instead of taking a
  # transition (no exit or entry handlers, no default child), a jump seen by
  # the chart's transitions, `_event` not being the operator's event, and a
  # MICROSTEP a sink cannot tell from a chart's.
  scenario 'a jump runs the engine, and its handlers see the operator event' do
    actor = spawn
    actor.deliver 'go'
    assert_equal %i[work w1], actor.active
    recording_trace
    actor.jump :w2, data: { who: 'oncall' }
    assert_equal %i[work w2], actor.active
    assert_equal ['enter work go', 'w1 1 go', 'w1 2 go', 'exit work operator.goto', 'enter work operator.goto',
                  'enter w2 operator.goto'], actor.data('.log')
    assert_equal ['begin 3 operator.goto',
                  'micro 3.1 operator.goto jump root->[w2] exit[w1,work] enter[work,w2] history[wh]',
                  'assign 3.1 exit:work', 'assign 3.1 enter:work', 'assign 3.1 enter:w2', 'end 3 [work,w2]'],
                 trace_lines
    micro = trace_records.find { |record| record[:kind] == 'microstep' }
    assert_equal [], Array(micro[:transitions]), 'an operator transition has no compiled ID'
  end

  # Catches: a jump source ignored (every jump from the root), so a region
  # of a parallel state cannot be moved alone; and a target in one region
  # that leaves the other region without its default.
  scenario 'a region of a parallel state as the source moves that region only' do
    actor = spawn
    actor.jump :r1b, :r2a
    assert_equal %i[par r1 r1b r2 r2a], actor.active
    recording_trace
    actor.jump :r1a, from: :r1
    assert_equal %i[par r1 r1a r2 r2a], actor.active
    assert_equal ['begin 3 operator.goto', 'micro 3.1 operator.goto jump r1->[r1a] exit[r1b] enter[r1a]',
                  'end 3 [par,r1,r1a,r2,r2a]'], trace_lines
    assert_equal [], actor.data('.log'), 'r2a was not exited'
    actor.jump :r2b
    assert_equal %i[par r1 r1a r2 r2b], actor.active
    assert_equal ['exit r2a'], actor.data('.log')
  end

  # Catches: a history target entered as its default instead of what it
  # recorded, or history not recorded when a jump exits its parent.
  scenario 'a jump through a history state puts the region back where it was' do
    actor = spawn
    actor.deliver 'go'
    actor.deliver 'next'
    actor.jump :idle
    assert_equal [:w2], actor.history(:wh), 'leaving work by a jump recorded its history'
    actor.jump :wh
    assert_equal %i[work w2], actor.active
    assert_equal 'enter w2 operator.goto', actor.data('.log').last
  end

  # Catches: a jump to a top-level final that does not end the session, or
  # ends it without its done data.
  scenario 'a jump to a top-level final finishes the actor with its done data' do
    actor = spawn
    actor.deliver 'go'
    actor.jump :end
    assert actor.done?, 'the actor finished'
    assert_equal 4, actor.completion
    assert_equal 'err_done', actor.post_jump(:idle), 'a finished actor is done for a jump too'
    assert_equal 'err_invalid', actor.post_jump, 'a malformed jump is invalid, finished or not'
    assert_equal 'err_done', actor.abandon
  end

  # Catches: a malformed jump that is accepted, or refused after it changed
  # the actor or kept its event.
  scenario 'deliver refuses a malformed jump and keeps nothing' do
    actor = spawn
    before = actor.observe
    refusals = {
      'no targets' => [[], nil],
      'an unknown target' => [[9999], nil],
      'an unknown source' => [[:w1], 9999],
      'a source that is not active' => [[:w2], :w1],
      'a history state as the source' => [[:w2], :wh],
      'two children of one state' => [%i[w1 w2], nil],
      'a state and its ancestor' => [%i[work w1], nil],
      'the same state twice' => [%i[w1 w1], nil],
      "a history state and a state of its parent's region" => [%i[wh w2], nil],
      'two top-level states' => [%i[idle end], nil],
      'two states of one region' => [%i[r1a r1b], nil]
    }
    refusals.each do |what, (targets, source)|
      assert_equal 'err_invalid', actor.post_jump(*targets, from: source), what
    end
    assert_equal before, actor.observe, 'a refused jump leaves the actor as it was'
    assert_equal 'ok', actor.post('go'), 'the actor holds no event'
  end

  # Catches: a jump accepted by an actor that is busy, which would run it
  # from a configuration it was not checked against.
  scenario 'a jump waits for the actor like any event' do
    actor = spawn
    assert_equal 'ok', actor.post('go')
    assert_equal 'err_busy', actor.post_jump(:w2)
    assert_equal 'paused', actor.resume(budget: 1000, action_budget: 1)
    assert actor.inspect_actor[:microstep_in_progress], 'paused inside the entry of w1'
    assert_equal 'err_busy', actor.post_jump(:w2)
    run_system('finish go')
    actor.jump :w2
    assert_equal %i[work w2], actor.active
  end

  # Catches: a pause inside the operator's microstep that loses or repeats
  # a unit, or that resumes as a chart transition (the continuation stores
  # transitions by compiled ID, and this one has none).
  scenario 'pausing inside a jump is invisible' do
    variants = (1..3).to_h { |budget| ["action budget #{budget}", { resume_action_budget: budget }] }
    variants['resume budget 1'] = { resume_budget: 1 }
    variants['every microstep start'] = { resume_pause_at_microstep_start: true }
    assert_equivalent_runs(variants: variants) do
      actor = spawn
      actor.jump :w1
      actor.jump :r1b, :r2b
      actor.jump :wh
      actor.jump :end
    end
  end

  # Catches: a jump that cannot stop before its first unit, or that has
  # exited a state or run a handler when it stops there; and an image that
  # loses that stop (ADR 0087).
  scenario 'a jump can stop before its first unit' do
    actor = spawn(name: 'subject')
    assert_equal 'ok', actor.post_jump(:w2)
    assert_equal 'paused', actor.resume(budget: 1000, pause_at_microstep_start: true)
    view = actor.inspect_actor
    assert view[:microstep_in_progress], 'it stopped inside the jump'
    assert_equal [:idle], actor.active
    assert_equal [], actor.data('.log')
    assert_equal 'enter', view.dig(:position, :at)
    assert_equal :work, state_name(actor.definition, view.dig(:position, :state))
    assert_equal 'err_busy', actor.abandon
    cycle_residency(actor)
    assert_equal view.slice(:step, :position), actor.inspect_actor.slice(:step, :position)
    assert_equal 'paused', actor.resume(budget: 1000, action_budget: 1, pause_at_microstep_start: true)
    assert_equal ['enter work operator.goto'], actor.data('.log')
    run_system('finish the jump')
    assert_equal %i[work w2], actor.active
    assert_equal ['enter work operator.goto', 'enter w2 operator.goto'], actor.data('.log')
  end

  # Catches: an image that loses an operator transition waiting with its
  # event, or the position inside the operator's microstep.
  scenario 'a jump survives unload and load, before it is taken and inside it' do
    actor = spawn(name: 'subject')
    assert_equal 'ok', actor.post_jump(:w1)
    cycle_residency(actor)
    assert_equal 'paused', actor.resume(budget: 1000, action_budget: 2)
    assert actor.inspect_actor[:microstep_in_progress], 'paused inside the jump'
    assert_equal [:idle], actor.active, 'a paused jump reports the configuration before it'
    cycle_residency(actor)
    run_system('finish the jump')
    assert_equal %i[work w1], actor.active
    assert_equal ['enter work operator.goto', 'w1 1 operator.goto', 'w1 2 operator.goto'], actor.data('.log')
  end

  # Catches: a failed allocation during a jump that repeats or loses part of
  # it.
  scenario 'each failed allocation of a jump is invisible' do
    result = assert_allocation_failures_invisible do
      actor = spawn
      actor.jump :w1
      actor.jump :r1b, :r2a
      actor.jump :wh
    end
    assert result[:runs].positive?, 'some allocation was failed'
  end

  # Catches: an actor stopped inside a jump whose image loses where it
  # stopped, so that the loaded actor redoes or skips part of the jump.
  scenario 'a jump stopped by a failed allocation survives unload and load' do
    result = assert_allocation_failures_invisible(on_stop: :cycle, normalize: Operator::WITHOUT_DIRECTORY) do
      actor = spawn(name: 'subject')
      actor.jump :w1
      actor.jump :r1b, :r2a
    end
    assert result[:runs].positive?, 'some allocation was failed'
  end
end

TinyactorsTest.scenarios 'abandoning a macrostep', chart: :operator do
  # Catches: a wedged actor that cannot be recovered, an abandon that
  # leaves the actor busy or with an illegal configuration, internal events
  # that survive it, and a macrostep span a sink cannot close.
  scenario 'a macrostep that never ends is abandoned, and a jump then works' do
    recording_trace
    actor = Operator.spinning(self)
    assert_equal 'err_busy', actor.post_jump(:idle), 'a wedged actor refuses a jump'
    waiting = Operator.waiting(actor)
    assert waiting >= 10, "the loop queued ticks (#{waiting})"
    before = trace_records.size
    assert_equal 'ok', actor.abandon
    lines = trace_lines(trace_records.drop(before))
    assert_equal 2, lines.size, lines.inspect
    assert_equal "abandoned 2.25 spin dropped #{waiting}", lines[0]
    assert lines[1].match?(/\Aend 2 \[loop,l[12]\]\z/), "MACROSTEP_END with the kept configuration: #{lines[1]}"
    inspection = actor.inspect_actor
    assert !inspection[:macrostep_in_progress], 'the actor is idle'
    assert_equal 'idle', inspection[:residency]
    assert_equal 0, Operator.waiting(actor)
    assert_equal 'err_invalid', actor.abandon, 'nothing is left to abandon'
    actor.jump :idle
    assert_equal [:idle], actor.active
    actor.deliver 'go'
    assert_equal %i[work w1], actor.active
  end

  # Catches: an abandon inside a microstep, which would keep a configuration
  # that does not match the invocations and history its entries recorded.
  scenario 'abandoning waits for the microstep in progress to finish' do
    actor = spawn
    assert_equal 'ok', actor.post('spin')
    assert_equal 'paused', actor.resume(budget: 1000, action_budget: 1)
    assert actor.inspect_actor[:microstep_in_progress], "paused inside l1's entry"
    assert_equal 'err_busy', actor.abandon
    assert_equal 'paused', actor.resume(budget: 1, action_budget: 0)
    assert !actor.inspect_actor[:microstep_in_progress], 'between microsteps'
    assert_equal 'ok', actor.abandon
    assert_equal %i[loop l1], actor.active
  end

  # Catches: an abandon that answers ok for an actor with nothing to
  # abandon, and one that forgets a delivered event (or its operator
  # transition) that was not taken yet.
  scenario 'abandoning answers for every actor state' do
    recording_trace
    actor = spawn
    assert_equal 'err_invalid', actor.abandon, 'an idle actor has no macrostep'
    assert_equal 'ok', actor.post('go')
    before = trace_records.size
    assert_equal 'ok', actor.abandon, 'an event not yet taken is dropped'
    assert_equal ['abandoned 0.0 go dropped 0'], trace_lines(trace_records.drop(before)),
                 'no macrostep began, so none ends'
    assert_equal 'ok', actor.post_jump(:w2)
    assert_equal 'ok', actor.abandon, 'a jump not yet taken is dropped with its event'
    run_system('after abandoning')
    assert_equal [:idle], actor.active
    assert_equal [], actor.data('.log')
    actor.destroy
    assert_equal 'err_not_found', actor.abandon
  end
end
