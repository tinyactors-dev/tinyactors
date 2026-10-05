# frozen_string_literal: true

# What `_event` names (SCXML 5.10.1): the Processor binds it when it
# removes an event from a queue, and nothing unbinds it, so after its first
# event a session's `_event` is the most recent event it took, between its
# macrosteps too.
#
# Chart code of a session normally runs only inside that session's own
# macrosteps. One exception reaches `_event` between them: cancelling an
# invoked child runs the child's `<onexit>` handlers inside the parent's
# macrostep (SCXML 6.4, exitInterpreter), while the child is idle, or has
# been handed an event it has not taken yet.
module EventBinding
  CHILD = {
    'jq' => {
      name: '$_event.name', x: '$_event.data.x', inner: '&quot;inner&quot;'
    },
    'ecmascript' => {
      name: '_event.name', x: '_event.data.x', inner: "'inner'"
    }
  }.freeze

  # A parent that invokes a child and cancels it on `stop`. The child
  # ignores `note`, turns `echo` into the internal event `echoed` with x =
  # "inner", and logs `_event` when it is exited.
  def self.parent(datamodel)
    e = CHILD.fetch(datamodel)
    <<~SCXML
      <scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="#{datamodel}" initial="running" name="event_binding_#{datamodel}">
        <state id="running">
          <invoke id="worker" type="http://www.w3.org/TR/scxml/">
            <content>
              <scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="#{datamodel}" initial="listening" name="event_binding_child_#{datamodel}">
                <state id="listening">
                  <onexit>
                    <log label="name" expr="#{e[:name]}"/>
                    <log label="x" expr="#{e[:x]}"/>
                  </onexit>
                  <transition event="note" type="internal"/>
                  <transition event="echo" type="internal">
                    <send target="#_internal" event="echoed"><param name="x" expr="#{e[:inner]}"/></send>
                  </transition>
                </state>
              </scxml>
            </content>
          </invoke>
          <transition event="stop" target="stopped"/>
        </state>
        <state id="stopped"/>
      </scxml>
    SCXML
  end

  # Cancels the child from inside the parent's macrostep, without running
  # anything else in the system, and returns what the child's <onexit> saw.
  def self.cancel(context, parent)
    context.instance_exec do
      assert_equal 'ok', parent.post('stop')
      assert_equal 'stable', parent.resume(budget: 64)
      assert_equal [], invoked_actors, 'the child was cancelled'
      logs.map { |record| [record[:label], record[:value]] }
    end
  end
end

# Records `_event.name` in every transition: an external event, the
# internal event its transition raises, and an eventless transition after
# that, whose `_event` is still the internal event. `go` raises eight
# events, which fill a queue of the first capacity (ADR 0064), so the
# microstep of `inner` may have to grow it first.
TinyactorsTest.chart :event_trail do
  data seen: { expr: '[]' }

  state :waiting do
    transition on: 'go', target: :raised do
      assign '.seen', '.seen + [$_event.name]'
      raise_event 'inner'
      7.times { raise_event 'pad' }
    end
  end
  state :raised do
    transition on: 'inner', target: :settling do
      assign '.seen', '.seen + [$_event.name]'
    end
  end
  state :settling do
    transition target: :waiting do
      assign '.seen', '.seen + ["after \\($_event.name)"]'
    end
  end
end

%w[jq ecmascript].each do |datamodel|
  TinyactorsTest.scxml_chart :"event_binding_#{datamodel}", EventBinding.parent(datamodel), datamodel: datamodel
end

TinyactorsTest.scenarios '_event binding' do
  %w[jq ecmascript].each do |datamodel|
    chart = :"event_binding_#{datamodel}"

    # Catches: a child that forgets its last event when its macrostep
    # ends, so that its <onexit> sees no `_event` (error.execution) when a
    # parent cancels it between macrosteps.
    scenario "a cancelled child's <onexit> sees the external event it took last (#{datamodel})" do
      recording_log
      parent = spawn chart
      child = invoked_actors.fetch(0)
      child.deliver 'note', data: { x: 'first' }

      assert_equal [%w[name note], %w[x first]], EventBinding.cancel(self, parent)
    end

    # Catches: a last event that is kept only when it was external. An
    # internal event taken from the queue is `_event` as much as an
    # external one is.
    scenario "a cancelled child's <onexit> sees the internal event it took last (#{datamodel})" do
      recording_log
      parent = spawn chart
      child = invoked_actors.fetch(0)
      child.deliver 'echo', data: { x: 'outer' }

      assert_equal [%w[name echoed], %w[x inner]], EventBinding.cancel(self, parent)
    end

    # Catches: a delivered event that becomes `_event` before the child's
    # macrostep takes it. The child was handed `second` but never ran, so
    # the last event it removed from a queue is still `first`.
    scenario "a cancelled child's <onexit> does not see an event it has not taken yet (#{datamodel})" do
      recording_log
      parent = spawn chart
      child = invoked_actors.fetch(0)
      child.deliver 'note', data: { x: 'first' }
      assert_equal 'ok', child.post('note', data: { x: 'second' })

      assert_equal [%w[name note], %w[x first]], EventBinding.cancel(self, parent)
    end
  end

  # ADR 0047: a macrostep that runs out of memory is suspended with
  # nothing visible, and resuming it continues where it was. The only
  # allocations core may make after it took an event and before the
  # microstep is visible are the reservation before each microstep (ADR
  # 0064): here, a queue for the external event's microstep and a larger
  # one for the internal event's. Paused actors hold every cached queue, so
  # the reservations allocate, and each fails once in turn.
  #
  # Catches: a retry that loses the event it had taken (the transition
  # never runs), takes it twice, or binds `_event` to the wrong event, so
  # that `.seen` differs from an uninterrupted run.
  scenario 'a macrostep that runs out of memory before a microstep binds the same _event when resumed' do
    reference = spawn :event_trail
    reference.deliver 'go'
    expected = ['go', 'inner', 'after inner']
    assert_equal expected, reference.data('.seen')

    # Spawning runs the system, which would resume the holders, so every
    # actor is spawned before they pause. A run that ends stable returns
    # its queue to the cache, so one more holder takes it back each time.
    probe = spawn :event_trail
    actors = Array.new(4) { spawn :event_trail }
    holders = Array.new(16) { spawn :event_trail }
    hold = lambda do
      holder = holders.shift
      assert_equal 'ok', holder.post('go')
      assert_equal 'paused', holder.resume(budget: 1), 'a paused actor keeps its queue'
    end
    8.times { hold.call }

    assert_equal 'ok', probe.post('go')
    before = allocator.stats[:allocations]
    assert_equal 'stable', probe.resume(budget: 64)
    allocations = allocator.stats[:allocations] - before
    assert allocations.positive?, "the macrostep allocates (#{allocations})"
    assert allocations <= actors.size, "one actor per allocation (#{allocations})"
    hold.call

    allocations.times do |failing|
      actor = actors.fetch(failing)
      assert_equal 'ok', actor.post('go')
      allocator.arm_fault(after: failing, count: 1)
      result = actor.run_once(budget: 64)[:result]
      allocator.clear_faults
      assert_equal 'err_nomem', result, "allocation #{failing + 1} of #{allocations} fails the resume"
      assert_equal 'stable', actor.resume(budget: 64)
      hold.call
      assert_equal expected, actor.data('.seen'), "after allocation #{failing + 1} of #{allocations} failed"
    end
  end
end
