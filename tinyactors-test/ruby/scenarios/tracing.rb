# frozen_string_literal: true

# Tracing (ADR 0067). A trace.recording instrument is the system's trace
# sink; these scenarios read what core wrote to it. Records are compared as
# lines (TinyactorsTest::TraceLine): the kind, the step key, where in the
# macrostep it was written, and what the kind carries.
module Tracing
  # Eventless transitions, a targetless transition with a <foreach>, an
  # internal transition inside a compound state, a failing <assign>, and a
  # guard over data. Its states, in document order:
  #   0 idle, 1 one, 2 two, 3 three, 4 x, 5 y
  BASIC = <<~SCXML
    <scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="jq" initial="idle" name="trace_basic">
      <datamodel>
        <data id="n" expr="0"/>
        <data id="i" expr="null"/>
      </datamodel>
      <state id="idle">
        <transition event="go" target="one"/>
        <transition event="count">
          <foreach array="[1, 2]" item="i"><assign location=".n" expr=".n + .i"/></foreach>
        </transition>
        <transition event="bad"><assign location=".n" expr="error(&quot;broken&quot;)"/></transition>
      </state>
      <state id="one"><transition target="two"/></state>
      <state id="two"><transition cond=".n == 3" target="three"/></state>
      <state id="three" initial="x">
        <transition event="inside" type="internal" target="y"/>
        <state id="x"/>
        <state id="y"/>
      </state>
    </scxml>
  SCXML

  # Parallel regions that move together, leave together, and come back
  # through a shallow history. States:
  #   0 p, 1 r1, 2 h1, 3 a1, 4 b1, 5 r2, 6 a2, 7 b2, 8 q
  PARALLEL = <<~SCXML
    <scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="jq" initial="p" name="trace_parallel">
      <parallel id="p">
        <state id="r1" initial="a1">
          <history id="h1" type="shallow"><transition target="a1"/></history>
          <state id="a1"><transition event="next" target="b1"/></state>
          <state id="b1"/>
        </state>
        <state id="r2" initial="a2">
          <state id="a2"><transition event="next" target="b2"/></state>
          <state id="b2"/>
        </state>
        <transition event="out" target="q"/>
      </parallel>
      <state id="q"><transition event="back" target="h1"/></state>
    </scxml>
  SCXML

  # On `start`, sends itself `tick` after 10 ms and `hop` to the session
  # `.next` names; passes `hop` on to `.next` when it names one.
  RELAY = <<~SCXML
    <scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="jq" initial="wait" name="trace_relay">
      <datamodel><data id="next" expr="null"/></datamodel>
      <state id="wait">
        <transition event="start">
          <send event="tick" delay="10ms"/>
          <send event="hop" targetexpr=".next"/>
        </transition>
        <transition event="hop" cond=".next != null"><send event="hop" targetexpr=".next"/></transition>
        <transition event="hop" cond=".next == null"/>
        <transition event="tick"/>
      </state>
    </scxml>
  SCXML

  # Every kind but the ones a residency cycle adds on purpose.
  SWEEP_KINDS = %w[spawned finished destroyed macrostep_begin macrostep_end condition microstep raised data_init
                   assign script foreach action_failed sent cancelled invoked invoke_cancelled paused resumed
                   stopped forwarded].freeze

  TRACE_ID = '4bf92f3577b34da6a3ce929d0e0e4736'
end

TinyactorsTest.scxml_chart :trace_basic, Tracing::BASIC, datamodel: 'jq'
TinyactorsTest.scxml_chart :trace_parallel, Tracing::PARALLEL, datamodel: 'jq'
TinyactorsTest.scxml_chart :trace_relay, Tracing::RELAY, datamodel: 'jq'

TinyactorsTest.scenarios 'tracing' do
  # Catches: a bootstrap that is not macrostep 1, data initialized without
  # a record, or the initial microstep's entry set missing. With every kind,
  # the definition's registration comes first (ADR 0080): it is registered
  # when the actor is spawned from it.
  scenario 'the bootstrap is macrostep 1, and its data and entry are recorded' do
    recording_trace
    spawn :trace_basic
    assert_equal ['registered trace_basic', 'spawned', 'begin 1 -', 'data 1.0 bootstrap n', 'data 1.0 bootstrap i',
                  'micro 1.1 - [] exit[] enter[idle]', 'end 1 [idle]'], trace_lines
  end

  # Catches: autoforwarding that leaves no record (ADR 0067 open point 3),
  # or one written before the forward committed.
  # Every external event is forwarded (SCXML 6.4), the child's own too.
  scenario 'each autoforwarded event is recorded once, naming the invocation and the event' do
    recording_trace(kinds: %i[forwarded])
    caller = spawn :caller
    run_system('the child starts')
    caller.deliver 'finish'
    forwarded = trace_lines(actor: caller).map { |line| line.sub(/ v\d+ /, ' v ') }
    assert_equal ['forwarded 2.0 finalize v progress', 'forwarded 3.0 finalize v finish',
                  'forwarded 4.0 finalize v done.invoke.worker'], forwarded
    assert_equal ['progress 21', 'done 42'], caller.data('.log'), 'the child still finished'
  end

  # Catches: an invoker's error report queued with no RAISED record, so its
  # macrostep had no visible cause (ADR 0072 closed the other such cases).
  scenario "an invoker's error report is recorded as the event it raised" do
    service = scripted_invoker('urn:tinyactors:test:service')
    client = spawn :service_client
    recording_trace(kinds: %i[raised])
    report_invoke(service.started.first, :error, reason: 'unavailable', ends: true)
    raised = trace_lines(actor: client)
    assert_equal 1, raised.size, "one RAISED record: #{raised.inspect}"
    assert raised.first.match?(/\Araised .*error\.communication\.invoke\.svc\.unavailable/), raised.first
  end

  # Catches: an invocation lost across an image queued with no RAISED record.
  scenario 'each invocation lost across an image is recorded as the event it raised' do
    scripted_invoker('urn:tinyactors:test:service')
    pair = spawn :service_pair, name: 'p1'
    recording_trace(kinds: %i[raised])
    cycle_residency(pair)
    run_system('restored')
    raised = trace_lines.grep(/unavailable/)
    assert_equal 2, raised.size, "two RAISED records: #{trace_lines.inspect}"
    assert raised[0].include?('invoke.first.unavailable'), raised[0]
    assert raised[1].include?('invoke.second.unavailable'), raised[1]
  end

  # Catches: a targetless transition that exits or enters, a <foreach>
  # whose iterations are not each recorded, and data changes without
  # their position.
  scenario 'a targetless transition runs its content, one record per iteration and assignment' do
    actor = spawn :trace_basic
    recording_trace
    actor.deliver 'count'
    assert_equal ['begin 2 count', 'micro 2.1 count [t1] exit[] enter[]',
                  'foreach 2.1 transition:idle i0', 'assign 2.1 transition:idle',
                  'foreach 2.1 transition:idle i1', 'assign 2.1 transition:idle', 'end 2 [idle]'], trace_lines
  end

  # Catches: a failing action without ACTION_FAILED, or its error.execution
  # without the RAISED record that follows it; and an internal event that
  # enables nothing written as a microstep.
  scenario 'a failing action is recorded, then the error it raised' do
    actor = spawn :trace_basic
    recording_trace
    actor.deliver 'bad'
    lines = trace_lines
    assert_equal 'begin 2 bad', lines[0]
    assert_equal 'micro 2.1 bad [t2] exit[] enter[]', lines[1]
    assert lines[2].start_with?('failed 2.1 transition:idle a'), "an ACTION_FAILED record: #{lines.inspect}"
    # Nothing handles the error, so it is taken and ignored (ADR 0072).
    assert_equal ['raised 2.1 transition:idle error.execution', 'ignored 2.1', 'end 2 [idle]'], lines[3..]
  end

  # Catches: eventless microsteps that share a key or skip one, a guard
  # written before its selection completed, and a microstep that names its
  # sets wrongly.
  scenario 'an eventless chain is one microstep each, and a guard precedes the microstep it enabled' do
    actor = spawn :trace_basic
    actor.deliver 'count'
    recording_trace
    actor.deliver 'go'
    assert_equal ['begin 3 go', 'micro 3.1 go [t0] exit[idle] enter[one]', 'micro 3.2 - [t3] exit[one] enter[two]',
                  'cond 3.2 select:two t4 true', 'micro 3.3 - [t4] exit[two] enter[three,x]',
                  'end 3 [three,x]'], trace_lines
  end

  # Catches: an internal transition that exits its source, and a macrostep
  # whose event enables nothing written as anything but BEGIN and END.
  scenario 'an internal transition exits only below its source, and an ignored event is BEGIN, IGNORED, then END' do
    actor = spawn :trace_basic
    actor.deliver 'count'
    actor.deliver 'go'
    recording_trace
    actor.deliver 'inside'
    actor.deliver 'unknown'
    assert_equal ['begin 4 inside', 'micro 4.1 inside [t5] exit[x] enter[y]', 'end 4 [three,y]',
                  'begin 5 unknown', 'ignored 5.0', 'end 5 [three,y]'], trace_lines
  end

  # Catches: parallel transitions out of selection order, exit sets out of
  # exit order, and a history state the exit recorded that goes unnamed.
  scenario 'parallel regions and a shallow history' do
    actor = spawn :trace_parallel
    recording_trace
    actor.deliver 'next'
    actor.deliver 'out'
    actor.deliver 'back'
    assert_equal ['begin 2 next', 'micro 2.1 next [t2,t3] exit[a2,a1] enter[b1,b2]', 'end 2 [p,r1,b1,r2,b2]',
                  'begin 3 out', 'micro 3.1 out [t0] exit[b2,r2,b1,r1,p] enter[q] history[h1]', 'end 3 [q]',
                  'begin 4 back', 'micro 4.1 back [t4] exit[q] enter[p,r1,b1,r2,a2]', 'end 4 [p,r1,b1,r2,a2]'],
                 trace_lines
  end

  # Catches: an unsampled macrostep that builds records anyway, and a
  # sink that asked for unsampled ones and does not get them.
  scenario 'an unsampled macrostep writes nothing unless the sink asks for unsampled ones' do
    actor = spawn :trace_basic
    recording_trace
    unsampled = TinyactorsTest.trace_parent(trace_id: Tracing::TRACE_ID, sampled: false, parent_span: 7)
    actor.deliver 'count', trace: unsampled
    assert_equal [], trace_lines, 'an unsampled macrostep is not built'
    sampled = TinyactorsTest.trace_parent(trace_id: Tracing::TRACE_ID, sampled: true, parent_span: 7)
    actor.deliver 'count', trace: sampled
    begin_record = trace_records.find { |record| record[:kind] == 'macrostep_begin' }
    assert_equal Tracing::TRACE_ID, TinyactorsTest.trace_id(begin_record[:context])
    assert_equal({ session: 0, macrostep: 7, microstep: 0 }, begin_record[:cause].slice(:session, :macrostep, :microstep))
  end

  scenario 'a sink that asks for unsampled macrosteps gets them' do
    actor = spawn :trace_basic
    recording_trace(unsampled: true)
    unsampled = TinyactorsTest.trace_parent(trace_id: Tracing::TRACE_ID, sampled: false)
    actor.deliver 'count', trace: unsampled
    assert_equal 'begin 2 count', trace_lines.first
  end

  # Catches: counters that restart after a session image, and a pending
  # send that loses the parent it was scheduled under.
  scenario 'counters and parents survive unload and load' do
    trace = TinyactorsTest.trace_parent(trace_id: Tracing::TRACE_ID, parent_span: 11)
    actor = spawn :trace_relay, name: 'solo'
    recording_trace
    actor.deliver 'start', trace: trace
    sent = trace_records.select { |record| record[:kind] == 'sent' }.map { |record| record[:key] }
    data = actor.data
    image = capture(actor)
    assert_equal 'ok', unload(actor, image)
    assert_equal 'ok', load(actor, image, binding: data)
    destroy_image(image)
    advance_time 10
    tick = trace_records.find { |record| record[:kind] == 'macrostep_begin' && record.dig(:event, :name_text) == 'tick' }
    assert tick, "the delayed tick ran a macrostep: #{trace_lines.inspect}"
    assert_equal 3, tick.dig(:key, :macrostep), 'the session went on counting after it was loaded'
    assert_equal Tracing::TRACE_ID, TinyactorsTest.trace_id(tick[:context]), 'the tick ran in the request\'s trace'
    assert_equal sent.last, tick[:cause], 'the tick names the step that scheduled it'
    kinds = trace_records.map { |record| record[:kind] }
    assert kinds.include?('unloaded') && kinds.include?('loaded'), 'residency changes are lifecycle records'
  end

  # One delivery that causes sends across three actors and a delayed send;
  # every macrostep it caused carries its trace ID and a cause chain back
  # to it. With `hold`, b and c are unloaded first: each hop's mail waits in
  # core, which asks this host to load its target, and the host loads it
  # later, newest request first: another pump, another order. The trace
  # parent travels in the waiting mail (core ADR 0099).
  causality = lambda do |hold|
    c = spawn :trace_relay, name: 'c'
    b_binding = { next: '#_actor_trace_relay/c' }
    b = spawn :trace_relay, name: 'b', binding: b_binding
    a = spawn :trace_relay, binding: { next: '#_actor_trace_relay/b' }
    images = {}
    if hold
      on_load_needed { |_request| :hold }
      { b => b_binding, c => nil }.each do |actor, binding|
        image = capture(actor)
        assert_equal 'ok', unload(actor, image)
        images[actor] = [image, binding]
      end
    end
    recording_trace
    request = TinyactorsTest.trace_parent(trace_id: Tracing::TRACE_ID, parent_span: 0xabc)
    a.deliver 'start', trace: request
    answered = []
    release = lambda do
      3.times do
        held = load_requests.reject { |waiting| answered.include?(waiting[:request]) }
        break if held.empty?

        held.reverse_each do |waiting|
          actor = waiting.dig(:destination, :name) == 'b' ? b : c
          image, binding = images.fetch(actor)
          assert_equal 'ok', load(actor, image, binding: binding)
          destroy_image(image)
          load_settled(waiting, actor)
          answered << waiting[:request]
        end
        run_system('load the held targets')
      end
    end
    release.call if hold
    advance_time 10
    release.call if hold
    records = trace_records
    begins = records.select { |record| record[:kind] == 'macrostep_begin' }
    sends = records.select { |record| record[:kind] == 'sent' }
    assert_equal 4, begins.size, "four macrosteps: start, two hops and the tick: #{trace_lines(records).inspect}"
    begins.each do |record|
      assert_equal Tracing::TRACE_ID, TinyactorsTest.trace_id(record[:context]), "#{record[:event][:name_text]} is in the request's trace"
    end
    by_session = ->(actor) { actor.inspect_actor[:session_id] }
    start = begins.find { |record| record.dig(:event, :name_text) == 'start' }
    assert_equal({ session: 0, macrostep: 0xabc, microstep: 0 }, start[:cause].slice(:session, :macrostep, :microstep))
    [[b, a], [c, b]].each do |receiver, sender|
      hop = begins.find { |record| record[:key][:session] == by_session.call(receiver) }
      from = sends.find { |record| record[:key][:session] == by_session.call(sender) && record.dig(:event, :name_text) == 'hop' }
      assert_equal from[:key], hop[:cause], "#{receiver}'s hop names #{sender}'s send as its cause"
    end
    tick = begins.find { |record| record.dig(:event, :name_text) == 'tick' }
    scheduled = sends.find { |record| record.dig(:event, :name_text) == 'tick' }
    assert_equal scheduled[:key], tick[:cause], 'the tick names the step that scheduled it'
  end

  scenario 'one delivery\'s trace reaches three actors and a delayed send' do
    instance_exec(false, &causality)
  end

  scenario 'one delivery\'s trace reaches three actors and a delayed send, through loads the host holds' do
    instance_exec(true, &causality)
  end

  # Every record is written once (ADR 0067 implementation notes): with each
  # allocation of every resume failing in turn, the records equal those of
  # the run that failed nothing, once PAUSED, RESUMED and STOPPED are left
  # out (TinyactorsTest::Trace#journals). The bodies and charts are the
  # out-of-memory sweep's.
  %w[jq ecmascript].each do |datamodel|
    OutOfMemory::BODIES.each do |kind, body|
      chart = :"oom_#{kind}_#{datamodel}"
      label = kind.to_s.tr('_', ' ')
      errors = OutOfMemory::CHART_ERRORS.include?(kind)

      scenario "#{label}: every record is written once when an allocation fails (#{datamodel})", exhaustive: true do
        result = assert_allocation_failures_invisible(counts: [1, 2], chart_errors: errors) do
          recording_trace(kinds: Tracing::SWEEP_KINDS)
          instance_exec(chart, nil, &body)
        end
        assert result[:runs] >= 3, "only #{result[:runs]} allocations were failed"
      end
    end
  end
end
