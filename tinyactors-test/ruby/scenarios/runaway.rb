# frozen_string_literal: true

# Runaway macrosteps (ADR 0075, ADR 0103). A macrostep that runs its
# microstep limit stops between microsteps and is abandoned, as by
# ta_actor_abandon(): ta_run reports the input as "runaway", the session keeps
# the configuration after its last completed microstep, and the next input
# runs. A tenth of the way there it is recorded as long. The count is of
# microsteps, so a <foreach> that pauses across many resumes is one
# microstep and never reaches it.
module Runaway
  # An eventless self-transition that counts to `.to` and then leaves, so a
  # run past the limit can finish. States: 0 idle, 1 spin, 2 done.
  COUNTER = <<~SCXML
    <scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="jq" initial="idle" name="rw_counter">
      <datamodel>
        <data id="n" expr="0"/>
        <data id="to" expr="150"/>
      </datamodel>
      <state id="idle">
        <transition event="go" target="spin"><assign location=".n" expr="0"/></transition>
      </state>
      <state id="spin">
        <transition cond=".n &lt; .to" target="spin"><assign location=".n" expr=".n + 1"/></transition>
        <transition target="idle"/>
      </state>
    </scxml>
  SCXML

  # Two internal events that raise each other, forever.
  PING_PONG = <<~SCXML
    <scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="jq" initial="idle" name="rw_ping_pong">
      <state id="idle">
        <transition event="go"><raise event="ping"/></transition>
        <transition event="ping"><raise event="pong"/></transition>
        <transition event="pong"><raise event="ping"/></transition>
      </state>
    </scxml>
  SCXML

  # One transition whose <foreach> runs 3,000 iterations: one microstep.
  FOREACH = <<~SCXML
    <scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="jq" initial="idle" name="rw_foreach">
      <datamodel>
        <data id="n" expr="0"/>
        <data id="i" expr="null"/>
      </datamodel>
      <state id="idle">
        <transition event="go">
          <foreach array="[range(3000)]" item="i"><assign location=".n" expr=".n + 1"/></foreach>
        </transition>
      </state>
    </scxml>
  SCXML

  NONE = 0xFFFF_FFFF

  def self.limit(context, chart, limit)
    context.core('ta_definition_set_microstep_limit',
                 context.ta.definition.set_microstep_limit(system: context.system_handle,
                                                           definition: context.definition(chart), limit: limit))
  end

  def self.counters(context, chart)
    context.core('ta_definition_counters',
                 context.ta.definition.counters(system: context.system_handle, definition: context.definition(chart)))
           .fetch(:out)
  end

  # One ta_run; returns how it ended: "runaway", "paused", "done" or "stable".
  def self.resume(context, _actor, budget: 100_000, action_budget: 0)
    context.run_once(budget: budget, action_budget: action_budget)
  end
end

TinyactorsTest.scxml_chart :rw_counter, Runaway::COUNTER, datamodel: 'jq'
TinyactorsTest.scxml_chart :rw_ping_pong, Runaway::PING_PONG, datamodel: 'jq'
TinyactorsTest.scxml_chart :rw_foreach, Runaway::FOREACH, datamodel: 'jq'

TinyactorsTest.scenarios 'runaway macrosteps' do
  # Catches: a limit that is not checked, an abandon that changes more than
  # the macrostep (the session must keep the configuration after the last
  # completed microstep), a stop that counts twice, and a limit that does not
  # come back for the next macrostep.
  scenario 'an eventless loop stops at the limit, is abandoned, and a lifted limit lets it finish' do
    Runaway.limit(self, :rw_counter, 100)
    actor = spawn :rw_counter
    recording_trace(kinds: %i[long_macrostep runaway])
    assert_equal 'ok', actor.post('go')

    assert_equal 'runaway', Runaway.resume(self, actor)
    assert_equal ['long 2.10 [t1] [spin]', 'runaway 2.100 [t1] [spin]'], trace_lines
    counters = Runaway.counters(self, :rw_counter)
    assert_equal 1, counters[:runaways]
    assert_equal 1, counters[:macrosteps_long]
    assert_equal 99, actor.data('.n'), 'the go microstep, then 99 turns of the loop'
    assert !actor.inspect_actor[:macrostep_in_progress], 'the abandoned macrostep is over'
    assert_active actor, :spin

    # The next input runs: `go` enables nothing in spin, and the 51 turns of
    # the loop left are under the limit, so the macrostep finishes.
    assert_equal 'ok', actor.post('go')
    assert_equal 'stable', Runaway.resume(self, actor)
    assert_equal 150, actor.data('.n')
    assert_active actor, :idle
    assert_equal 1, Runaway.counters(self, :rw_counter)[:runaways]

    # The limit is there for the next macrostep, and counts again.
    assert_equal 'ok', actor.post('go')
    assert_equal 'runaway', Runaway.resume(self, actor)
    assert_equal 2, Runaway.counters(self, :rw_counter)[:runaways]

    # With the limit lifted the macrostep finishes.
    Runaway.limit(self, :rw_counter, Runaway::NONE)
    assert_equal 'ok', actor.post('go')
    assert_equal 'stable', Runaway.resume(self, actor)
    assert_equal 150, actor.data('.n'), 'the macrostep finished'
    assert_active actor, :idle
    assert_equal 2, Runaway.counters(self, :rw_counter)[:runaways]
  end

  # Catches: internal events that do not count as microsteps.
  scenario 'an internal-event ping-pong stops at the limit' do
    Runaway.limit(self, :rw_ping_pong, 50)
    actor = spawn :rw_ping_pong
    assert_equal 'ok', actor.post('go')
    assert_equal 'runaway', Runaway.resume(self, actor)
    assert_equal 1, Runaway.counters(self, :rw_ping_pong)[:runaways]
    assert_equal 'ok', actor.post('go'), 'an abandoned actor takes the next event'
  end

  # Catches: a limit on turns instead of microsteps, which a long <foreach>
  # pausing across many resumes would trip.
  scenario 'a long <foreach> that pauses across many resumes is one microstep', exhaustive: true do
    Runaway.limit(self, :rw_foreach, 5)
    actor = spawn :rw_foreach
    assert_equal 'ok', actor.post('go')
    resumes = 0
    loop do
      status = Runaway.resume(self, actor, action_budget: 50)
      resumes += 1
      break if status == 'stable'
    end
    assert resumes > 50, "the <foreach> paused across many resumes (#{resumes})"
    assert_equal 3000, actor.data('.n')
    assert_equal 0, Runaway.counters(self, :rw_foreach)[:runaways]
  end

  # Catches: an override that does not replace the system's limit, and
  # TA_MICROSTEP_LIMIT_NONE that still limits.
  scenario "a definition's limit overrides the system's, and none lifts it" do
    Runaway.limit(self, :rw_counter, Runaway::NONE)
    actor = spawn :rw_counter
    assert_equal 'ok', actor.post('go')
    assert_equal 'stable', Runaway.resume(self, actor)
    assert_equal 150, actor.data('.n')
    Runaway.limit(self, :rw_counter, 0)
    assert_equal 'ok', actor.post('go')
    assert_equal 'stable', Runaway.resume(self, actor), 'the test host sets no system limit'
  end

  # Catches: a retry after a failed allocation that counts a microstep twice
  # or resets the count, so the warning comes at another microstep or twice.
  scenario 'a failed allocation does not change where the warning comes', exhaustive: true do
    result = assert_allocation_failures_invisible do
      Runaway.limit(self, :rw_counter, 1000)
      actor = spawn :rw_counter
      actor.deliver 'go'
      counters = Runaway.counters(self, :rw_counter)
      assert_equal 1, counters[:macrosteps_long], 'warned once, at microstep 100'
      assert_equal 0, counters[:runaways]
    end
    assert result[:runs].positive?, 'the sweep failed allocations'
  end
end
