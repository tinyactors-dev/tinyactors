# frozen_string_literal: true

# Runaway macrosteps (ADR 0075). A macrostep that runs its microstep limit
# stops between microsteps with err_runaway, and nothing about the actor
# changes: it stays mid-macrostep and a later resume continues it. A tenth
# of the way there it is recorded as long. The count is of microsteps, so a
# <foreach> that pauses across many resumes is one microstep and never
# reaches it.
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

  # One ta_actor_resume; returns [result, status].
  def self.resume(context, actor, budget: 100_000, action_budget: 0, lifted: false)
    response = context.ta.actor.resume(system: context.system_handle, actor: actor.handle, budget: budget,
                                       action_budget: action_budget, no_microstep_limit: lifted)
    [response[:result], response.dig(:out, :status)]
  end
end

TinyactorsTest.scxml_chart :rw_counter, Runaway::COUNTER, datamodel: 'jq'
TinyactorsTest.scxml_chart :rw_ping_pong, Runaway::PING_PONG, datamodel: 'jq'
TinyactorsTest.scxml_chart :rw_foreach, Runaway::FOREACH, datamodel: 'jq'

TinyactorsTest.scenarios 'runaway macrosteps' do
  # Catches: a limit that is not checked, a stop that changes the actor, a
  # second resume that counts again, and a lifted limit that does not let
  # the macrostep finish.
  scenario 'an eventless loop stops at the limit, unchanged, and a lifted limit lets it finish' do
    Runaway.limit(self, :rw_counter, 100)
    actor = spawn :rw_counter
    recording_trace(kinds: %i[long_macrostep runaway])
    assert_equal 'ok', actor.post('go')

    assert_equal 'err_runaway', Runaway.resume(self, actor).first
    assert_equal ['long 2.10 [t1] [spin]', 'runaway 2.100 [t1] [spin]'], trace_lines
    counters = Runaway.counters(self, :rw_counter)
    assert_equal 1, counters[:runaways]
    assert_equal 1, counters[:macrosteps_long]
    assert_equal 99, actor.data('.n'), 'the go microstep, then 99 turns of the loop'
    assert actor.inspect_actor[:macrostep_in_progress], 'the actor is still mid-macrostep'

    assert_equal 'err_runaway', Runaway.resume(self, actor).first, 'the limit holds until lifted'
    assert_equal 1, Runaway.counters(self, :rw_counter)[:runaways], 'a second stop does not count again'
    assert_equal 99, actor.data('.n')

    assert_equal %w[ok stable], Runaway.resume(self, actor, lifted: true)
    assert_equal 150, actor.data('.n'), 'the macrostep finished'
    assert_active actor, :idle

    # The next macrostep has the limit again.
    assert_equal 'ok', actor.post('go')
    assert_equal 'err_runaway', Runaway.resume(self, actor).first
    assert_equal 2, Runaway.counters(self, :rw_counter)[:runaways]
  end

  # Catches: internal events that do not count as microsteps.
  scenario 'an internal-event ping-pong stops at the limit' do
    Runaway.limit(self, :rw_ping_pong, 50)
    actor = spawn :rw_ping_pong
    assert_equal 'ok', actor.post('go')
    assert_equal 'err_runaway', Runaway.resume(self, actor).first
    assert_equal 1, Runaway.counters(self, :rw_ping_pong)[:runaways]
    assert_equal 'err_busy', actor.post('go'), 'a stopped actor takes no event'
  end

  # Catches: a limit on turns instead of microsteps, which a long <foreach>
  # pausing across many resumes would trip.
  scenario 'a long <foreach> that pauses across many resumes is one microstep' do
    Runaway.limit(self, :rw_foreach, 5)
    actor = spawn :rw_foreach
    assert_equal 'ok', actor.post('go')
    resumes = 0
    loop do
      result, status = Runaway.resume(self, actor, action_budget: 50)
      assert_equal 'ok', result
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
    assert_equal %w[ok stable], Runaway.resume(self, actor)
    assert_equal 150, actor.data('.n')
    Runaway.limit(self, :rw_counter, 0)
    assert_equal 'ok', actor.post('go')
    assert_equal %w[ok stable], Runaway.resume(self, actor), 'the test host sets no system limit'
  end

  # Catches: a retry after a failed allocation that counts a microstep twice
  # or resets the count, so the warning comes at another microstep or twice.
  scenario 'a failed allocation does not change where the warning comes' do
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
