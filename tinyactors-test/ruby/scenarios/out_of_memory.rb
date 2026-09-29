# frozen_string_literal: true

# Running out of memory inside a macrostep (ADR 0047, ADR 0070). Every
# scenario here fails the allocations of every ta_actor_resume a run makes,
# one at a time, and checks that the resume answers err_nomem, that the
# stopped actor is still in its macrostep, refuses events and queued no
# error event, and that resuming it until it runs ends the run exactly as a
# run without the failure does: configuration, data, queued events, delayed
# sends, invocations, completion, and every instrument's journal (the
# events it sent, in order, and what it logged).
module OutOfMemory
  # Expressions in each data model's language.
  class Language
    def initialize(datamodel)
      @jq = datamodel == 'jq'
    end

    # The location of the top-level datum `name`.
    def at(name) = @jq ? ".#{name}" : name

    # The datum `name` with `item` appended.
    def append(name, item) = @jq ? ".#{name} + [#{item}]" : "#{name}.concat([#{item}])"

    def text(value) = @jq ? %("#{value}") : "'#{value}'"

    def event_name = @jq ? '$_event.name' : '_event.name'

    def to_text(expr) = @jq ? "(#{expr} | tostring)" : "String(#{expr})"

    def length(name) = @jq ? "(.#{name} | length)" : "#{name}.length"

    # An expression whose evaluation fails.
    def failing = @jq ? 'error("broken")' : 'undefinedFunction()'

    # The length of a text expression.
    def length_of(expr) = @jq ? "(#{expr} | length)" : "#{expr}.length"

    # A top-level <script> that sets `name` to 1.
    def set_script(name) = @jq ? ".#{name} = 1" : "#{name} = 1"
  end

  # Escapes an XML attribute value.
  def self.a(text)
    text.to_s.gsub('&', '&amp;').gsub('"', '&quot;').gsub('<', '&lt;').gsub('>', '&gt;')
  end

  # A trace without the directory's journal, where unloading and loading an
  # actor shows.
  WITHOUT_DIRECTORY = ->(state) { state.merge(journals: state[:journals].except('directory')) }

  # The places a sweep must have stopped a chart, by what its log ended with
  # when it stopped: the charts whose content grows storage in each phase.
  PHASE_LOGS = { phases: %w[exit move enter] }.freeze

  def self.check_stops(context, kind, stops)
    labels = PHASE_LOGS.fetch(kind, [])
    return if labels.empty?

    ends = stops.map { |stop| Array(stop.dig(:data, :log)).last }
    missing = labels.reject { |label| ends.include?(label) }
    context.assert missing.empty?, "no allocation failed in the #{missing.join(', ')} phase"
    context.assert stops.any? { |stop| stop[:microstep] }, 'no allocation failed inside a microstep'
  end

  # An event whose name is longer than a block of core's scratch, so that
  # rendering `_event.name` for the data model allocates (ADR 0068).
  LONG = "long.#{'x' * 5000}"

  def self.names(datamodel)
    l = Language.new(datamodel)
    document("oom_names_#{datamodel}", datamodel, <<~BODY)
      <state id="listening">
        <transition event="long" type="internal">#{log(l, l.to_text(l.length_of(l.event_name)))}</transition>
      </state>
    BODY
  end

  # A chart whose states sit in `main`, which records every error event in
  # `errors`, so a failure that turned into an error event shows in the data.
  def self.document(name, datamodel, body, data: {}, top: '', binding: nil)
    l = Language.new(datamodel)
    decls = { log: '[]', errors: '[]' }.merge(data)
    datums = decls.map { |id, expr| %(<data id="#{id}" expr="#{a(expr)}"/>) }.join("\n    ")
    <<~SCXML
      <scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="#{datamodel}" initial="main" name="#{name}"#{binding ? %( binding="#{binding}") : ''}>
        <datamodel>
          #{datums}
        </datamodel>
        #{top}
        <state id="main">
          <transition event="error" type="internal">
            <assign location="#{a(l.at('errors'))}" expr="#{a(l.append('errors', l.event_name))}"/>
          </transition>
          #{body}
        </state>
      </scxml>
    SCXML
  end

  # Appends `item` (an expression) to the log.
  def self.log(l, item) = %(<assign location="#{a(l.at('log'))}" expr="#{a(l.append('log', item))}"/>)

  # More internal events in one microstep than a queue's first capacity
  # holds (ADR 0064), taken one per microstep, then an eventless transition.
  def self.raises(datamodel)
    l = Language.new(datamodel)
    raises = (1..10).map { |n| %(<raise event="r.#{n}"/>) }.join
    document("oom_raises_#{datamodel}", datamodel, <<~BODY)
      <state id="idle">
        <transition event="go" target="busy">#{log(l, l.text('go'))}#{raises}</transition>
      </state>
      <state id="busy">
        <transition event="r" type="internal">#{log(l, l.event_name)}</transition>
        <transition cond="#{a("#{l.length('log')} == 11")}" target="settled">#{log(l, l.text('eventless'))}</transition>
      </state>
      <state id="settled">
        <transition event="again" target="idle"/>
      </state>
    BODY
  end

  # Delayed sends, more than an owner's first storage holds (ADR 0054):
  # literal and generated IDs, a delay expression, params, two cancelled.
  def self.delays(datamodel)
    l = Language.new(datamodel)
    sends = [
      %(<send event="t.1" delay="1s" idlocation="#{a(l.at('s1'))}"/>),
      %(<send event="t.2" delay="2s" id="two"/>),
      %(<send event="t.3" delay="3s" idlocation="#{a(l.at('s3'))}"/>),
      %(<send event="t.4" delay="4s"/>),
      %(<send event="t.5" delayexpr="#{a(l.text('5s'))}"/>),
      %(<send event="t.6" delay="6s"><param name="k" expr="6"/></send>),
      %(<send event="t.7" delay="7s" idlocation="#{a(l.at('s7'))}"/>),
      %(<cancel sendid="two"/>),
      %(<cancel sendidexpr="#{a(l.at('s3'))}"/>)
    ].join("\n")
    document("oom_delays_#{datamodel}", datamodel, <<~BODY, data: { s1: 'null', s3: 'null', s7: 'null' })
      <state id="idle">
        <transition event="go" target="waiting"/>
      </state>
      <state id="waiting">
        <onentry>#{sends}</onentry>
        <transition event="t.7" target="idle">#{log(l, l.text('last'))}</transition>
        <transition event="t" type="internal">#{log(l, l.event_name)}</transition>
      </state>
    BODY
  end

  # A <foreach> whose body assigns, raises, sends to itself through the
  # router (busy, so the send waits, ADR 0040), and sends with a delay.
  def self.loops(datamodel)
    l = Language.new(datamodel)
    body = [log(l, l.to_text(l.at('i'))), %(<raise event="each"/>), %(<send event="self"/>),
            %(<send event="later" delay="1s"/>)].join
    document("oom_loops_#{datamodel}", datamodel, <<~BODY, data: { i: 'null' })
      <state id="idle">
        <transition event="go" target="looped">
          <foreach array="[1, 2, 3, 4, 5, 6, 7, 8, 9]" item="i">#{body}</foreach>
        </transition>
      </state>
      <state id="looped">
        <transition event="each" type="internal">#{log(l, l.text('each'))}</transition>
        <transition event="self" type="internal">#{log(l, l.text('self'))}</transition>
        <transition event="later" type="internal">#{log(l, l.text('later'))}</transition>
      </state>
    BODY
  end

  # done.state for a compound state (with <donedata>), for a parallel
  # state whose regions both end, and a top-level <final> whose <donedata>
  # is the session's completion, after <onexit> runs as the session ends.
  def self.finals(datamodel)
    l = Language.new(datamodel)
    region = lambda do |id|
      <<~REGION
        <state id="#{id}" initial="#{id}a">
          <onexit>#{log(l, l.text("leave #{id}"))}<raise event="left.#{id}"/></onexit>
          <state id="#{id}a"><transition event="step" target="#{id}f"/></state>
          <final id="#{id}f"/>
        </state>
      REGION
    end
    body = <<~BODY
      <onexit>#{log(l, l.text('leave main'))}<log label="bye" expr="#{a(l.at('log'))}"/></onexit>
      <state id="idle">
        <transition event="go" target="compound"/>
      </state>
      <state id="compound" initial="c1">
        <state id="c1"><transition event="finish" target="cdone"/></state>
        <final id="cdone">
          <donedata><param name="p" expr="1"/><param name="q" expr="#{a(l.text('two'))}"/></donedata>
        </final>
        <transition event="done.state.compound" target="par">#{log(l, l.event_name)}</transition>
      </state>
      <parallel id="par">
        #{region.call('r1')}
        #{region.call('r2')}
        <transition event="done.state.r1" type="internal">#{log(l, l.event_name)}</transition>
        <transition event="done.state.par" target="end">#{log(l, l.event_name)}</transition>
      </parallel>
    BODY
    top = ''
    document("oom_finals_#{datamodel}", datamodel, body, top: top).sub(
      "  </state>\n</scxml>",
      "  </state>\n  <final id=\"end\"><donedata><param name=\"log\" expr=\"#{a(l.at('log'))}\"/></donedata></final>\n</scxml>"
    ).sub('<state id="par"', '<state id="par"')
  end

  # One microstep with content in every phase: exit handlers, transition
  # content and entry handlers, and history recorded and restored. Each
  # phase raises and sends enough to grow the queue and the delayed-send
  # storage once more, so a failed allocation lands in each of them.
  def self.phases(datamodel)
    l = Language.new(datamodel)
    content = lambda do |label, raises, sends|
      [log(l, l.text(label)),
       (1..raises).map { |n| %(<raise event="note.#{label}.#{n}"/>) }.join,
       (1..sends).map { |n| %(<send event="tick.#{label}.#{n}" delay="1s"/>) }.join,
       %(<log label="#{label}" expr="#{a(l.length('log'))}"/>),
       %(<foreach array="[1, 2]" item="i">#{log(l, l.to_text(l.at('i')))}<raise event="note.each"/></foreach>)].join
    end
    document("oom_phases_#{datamodel}", datamodel, <<~BODY, data: { i: 'null' })
      <state id="a" initial="a1">
        <onexit>#{content.call('exit', 9, 5)}</onexit>
        <state id="a1">
          <onexit>#{log(l, l.text('exit a1'))}</onexit>
          <transition event="deeper" target="a2"/>
        </state>
        <state id="a2"/>
        <history id="ah" type="deep"><transition target="a1"/></history>
        <transition event="go" target="b">#{content.call('move', 8, 4)}</transition>
      </state>
      <state id="b">
        <onentry>#{content.call('enter', 16, 8)}</onentry>
        <transition event="note" type="internal">#{log(l, l.event_name)}</transition>
        <transition event="tick" type="internal">#{log(l, l.event_name)}</transition>
        <transition event="back" target="ah"/>
      </state>
    BODY
  end

  # A parent and the child it invokes, with the SCXML invoker: the child
  # takes an autoforwarded event, sends three to its parent through the
  # router, and ends with <donedata>, which its end reports to the parent
  # as done.invoke.
  def self.family(datamodel)
    l = Language.new(datamodel)
    child = <<~CHILD
      <scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="#{datamodel}" initial="waiting" name="oom_child_#{datamodel}">
        <state id="waiting">
          <transition event="work" target="finished">
            <send target="#_parent" event="k.1"/><send target="#_parent" event="k.2"/><send target="#_parent" event="k.3"/>
          </transition>
        </state>
        <final id="finished"><donedata><param name="x" expr="42"/></donedata></final>
      </scxml>
    CHILD
    document("oom_family_#{datamodel}", datamodel, <<~BODY)
      <state id="home">
        <invoke id="kid" autoforward="true" type="http://www.w3.org/TR/scxml/"><content>#{child}</content></invoke>
        <transition event="k" type="internal">#{log(l, l.event_name)}</transition>
        <transition event="done.invoke.kid" target="over">#{log(l, l.event_name)}</transition>
      </state>
      <state id="over"/>
    BODY
  end

  SERVICE = 'urn:tinyactors:test:oom'

  # Five invocations (more than an actor's first record storage holds),
  # with generated IDs stored through idlocation, autoforwarding, and
  # <finalize>; leaving their state cancels them.
  def self.invokes(datamodel)
    l = Language.new(datamodel)
    invokes = (1..5).map do |n|
      <<~INVOKE
        <invoke type="#{SERVICE}" idlocation="#{a(l.at("i#{n}"))}" autoforward="#{n.odd?}">
          <finalize>#{log(l, l.text("finalize #{n}"))}<raise event="finalized"/></finalize>
        </invoke>
      INVOKE
    end.join
    data = (1..5).to_h { |n| [:"i#{n}", 'null'] }
    # Guards that fail after <finalize> ran for the event: their errors grow
    # the queue while the event can no longer be given back.
    failing = (1..9).map { %(<transition event="svc" cond="#{a(l.failing)}"/>) }.join
    document("oom_invokes_#{datamodel}", datamodel, <<~BODY, data: data)
      <state id="idle">
        <transition event="go" target="serving"/>
      </state>
      <state id="serving">
        #{invokes}
        #{failing}
        <transition event="svc" type="internal">#{log(l, l.event_name)}</transition>
        <transition event="finalized" type="internal">#{log(l, l.text('finalized'))}</transition>
        <transition event="poke" type="internal">#{log(l, l.text('poked'))}</transition>
        <transition event="stop" target="idle"/>
      </state>
    BODY
  end

  # Transition selection: guards that fail queue more errors than a queue's
  # first capacity holds, one per region of a parallel state.
  def self.guards(datamodel)
    l = Language.new(datamodel)
    regions = (1..10).map do |n|
      %(<state id="g#{n}"><transition event="go" cond="#{a(l.failing)}" target="g#{n}"/></state>)
    end.join
    document("oom_guards_#{datamodel}", datamodel, <<~BODY)
      <parallel id="all">
        #{regions}
        <transition event="go" target="after">#{log(l, l.text('selected'))}</transition>
      </parallel>
      <state id="after"/>
    BODY
  end

  # The bootstrap: data (two initializers fail), a top-level <script>, and
  # an initial transition whose content raises and sends.
  def self.boot(datamodel, binding: nil)
    l = Language.new(datamodel)
    data = (1..6).to_h { |n| [:"d#{n}", n.to_s] }.merge(bad1: l.failing, bad2: l.failing)
    top = %(<script>#{l.set_script('boot')}</script>)
    late = binding == 'late' ? %(<datamodel><data id="deep" expr="#{a(l.text('deep'))}"/><data id="worse" expr="#{a(l.failing)}"/></datamodel>) : ''
    document("oom_boot_#{binding || 'early'}_#{datamodel}", datamodel, <<~BODY, data: data.merge(boot: 'null'), top: top, binding: binding)
      <initial>
        <transition target="start">#{log(l, l.text('initial'))}<raise event="booted"/><send event="woke" delay="1s"/></transition>
      </initial>
      <state id="start">
        #{late}
        <onentry>#{log(l, l.text('start'))}</onentry>
        <transition event="booted" type="internal">#{log(l, l.event_name)}</transition>
        <transition event="woke" type="internal">#{log(l, l.event_name)}</transition>
      </state>
    BODY
  end

end

%w[jq ecmascript].each do |datamodel|
  %i[raises delays loops finals phases invokes guards family].each do |kind|
    TinyactorsTest.scxml_chart :"oom_#{kind}_#{datamodel}", OutOfMemory.public_send(kind, datamodel), datamodel: datamodel
  end
  TinyactorsTest.scxml_chart :"oom_names_#{datamodel}", OutOfMemory.names(datamodel), datamodel: datamodel
  TinyactorsTest.scxml_chart :"oom_boot_early_#{datamodel}", OutOfMemory.boot(datamodel), datamodel: datamodel
  TinyactorsTest.scxml_chart :"oom_boot_late_#{datamodel}", OutOfMemory.boot(datamodel, binding: 'late'),
                             datamodel: datamodel
end

module OutOfMemory
  # What each sweep runs against its chart: every event the chart takes,
  # twice where a cache or a grown array would hide the second time. Each
  # body takes the chart and a name: named actors can be moved out of
  # residency and back while stopped. The trace sweep (tracing.rb) runs the
  # same bodies.
  BODIES = {
    # Catches: a raise that is lost or queued twice when the queue could
    # not grow, and a microstep left half done.
    raises: lambda do |chart, name|
      actor = spawn chart, name: name
      actor.deliver 'go'
      actor.deliver 'again'
      actor.deliver 'go'
    end,
    # Catches: a delayed send stored twice or not at all, a generated send
    # ID that moves on a retry, and a cancel that misses its send.
    delays: lambda do |chart, name|
      actor = spawn chart, name: name
      actor.deliver 'go'
      advance_time 8000
      actor.deliver 'go'
      advance_time 8000
    end,
    # Catches: a <foreach> iteration run twice or skipped, and a send to
    # the busy actor itself that is lost or doubled when keeping it fails.
    loops: lambda do |chart, name|
      actor = spawn chart, name: name
      actor.deliver 'go'
      advance_time 2000
    end,
    # Catches: done.state events lost or doubled, <donedata> evaluated into
    # a completion twice, and a session that ends half way.
    finals: lambda do |chart, name|
      recording_log
      actor = spawn chart, name: name
      actor.deliver 'go'
      actor.deliver 'finish'
      actor.deliver 'step'
    end,
    # Catches: a failure in exit handlers, transition content or entry
    # handlers that repeats or skips content, or records history twice.
    phases: lambda do |chart, name|
      recording_log
      actor = spawn chart, name: name
      actor.deliver 'deeper'
      actor.deliver 'go'
      actor.deliver 'back'
      actor.deliver 'go'
      advance_time 1000
    end,
    # Catches: an invocation started twice or not at all, a generated
    # invoke ID that moves on a retry, <finalize> run twice, an event
    # autoforwarded twice, and cancellation that misses a service.
    invokes: lambda do |chart, name|
      service = scripted_invoker(OutOfMemory::SERVICE)
      actor = spawn chart, name: name
      actor.deliver 'go'
      service.started.each_with_index do |start, index|
        report_invoke(start, :event, event: "svc.#{index + 1}")
      end
      actor.deliver 'poke'
      actor.deliver 'stop'
      actor.deliver 'go'
    end,
    # Catches: errors of failing guards queued twice, or an event lost,
    # when queueing one of them runs out of memory.
    guards: lambda do |chart, name|
      actor = spawn chart, name: name
      actor.deliver 'go'
    end,
    # Catches: a bootstrap that ends in no state, initializes data twice
    # or skips it, or runs the top-level script twice.
    boot_early: lambda do |chart, name|
      spawn chart, name: name
      advance_time 1000
    end,
    boot_late: lambda do |chart, name|
      spawn chart, name: name
      advance_time 1000
    end,
    # Catches: events a child sends its parent lost, doubled or reordered,
    # and a done.invoke that the child's end reports twice or never.
    family: lambda do |chart, name|
      actor = spawn chart, name: name
      actor.deliver 'work'
    end
  }.freeze

  # Charts whose own content raises error events.
  CHART_ERRORS = %i[guards boot_early boot_late invokes].freeze
end

TinyactorsTest.scenarios 'running out of memory', exhaustive: true do
  # Every chart, in both data models, runs its body with each allocation of
  # each resume failing once.
  bodies = OutOfMemory::BODIES
  chart_errors = OutOfMemory::CHART_ERRORS
  # Charts whose content only core runs: every failed allocation is core's
  # own, so every one must stop the resume with err_nomem.
  self_contained = bodies.keys - %i[family]

  %w[jq ecmascript].each do |datamodel|
    bodies.each do |kind, body|
      chart = :"oom_#{kind}_#{datamodel}"
      label = kind.to_s.tr('_', ' ')
      errors = chart_errors.include?(kind)

      scenario "#{label}: each failed allocation is invisible (#{datamodel})" do
        result = assert_allocation_failures_invisible(chart_errors: errors) { instance_exec(chart, nil, &body) }
        warn "#{chart}: #{result.except(:stops).inspect}" if ENV['OOM_VERBOSE']
        assert result[:runs] >= 3, "only #{result[:runs]} allocations were failed"
        assert_equal [], result[:unreported], 'every failed allocation is reported' if self_contained.include?(kind)
        OutOfMemory.check_stops(self, kind, result[:stops])
      end

      # Catches: a retry that runs out of memory again (the same allocation
      # failing twice, then the one after it) and does not stop cleanly
      # the second time, or continues from the wrong place.
      scenario "#{label}: allocations failing three times in a row are invisible (#{datamodel})" do
        result = assert_allocation_failures_invisible(counts: [3], chart_errors: errors) do
          instance_exec(chart, nil, &body)
        end
        assert result[:stops].size > result[:runs], 'retries ran out of memory again'
      end

      # Catches: a failed allocation right after a stop at a microstep's
      # start (ADR 0086) that runs part of the microstep twice or not at
      # all, or loses the stop.
      if kind == :phases
        at_starts = "#{label}: each failed allocation is invisible when every microstep stops at its start"
        scenario "#{at_starts} (#{datamodel})" do
          result = assert_allocation_failures_invisible(chart_errors: errors, resume_pause_at_microstep_start: true,
                                                        resume_action_budget: 1) do
            instance_exec(chart, nil, &body)
          end
          assert result[:runs] >= 3, "only #{result[:runs]} allocations were failed"
          assert_equal [], result[:unreported], 'every failed allocation is reported'
        end
      end

      # Catches: a stopped actor whose image (ADR 0038) loses where it
      # stopped, so that the loaded actor redoes or skips a step. Loading
      # does not bring invocations back (ADR 0038), so the charts with
      # invocations take no part.
      next if %i[invokes family].include?(kind)

      scenario "#{label}: an actor stopped by a failed allocation survives unload and load (#{datamodel})" do
        result = assert_allocation_failures_invisible(on_stop: :cycle, chart_errors: errors,
                                                      normalize: OutOfMemory::WITHOUT_DIRECTORY) do
          instance_exec(chart, 'subject', &body)
        end
        assert result[:runs] >= 3, "only #{result[:runs]} allocations were failed"
      end
    end
  end

  %w[jq ecmascript].each do |datamodel|
    phases = :"oom_phases_#{datamodel}"

    # Catches: an image of an actor stopped inside a microstep that loses
    # where it stopped, so that the actor reconstructed from it (ADR 0038)
    # redoes or skips content, or ends in no state.
    scenario "an actor stopped inside a microstep continues from its image (#{datamodel})" do
      reference = spawn phases
      reference.deliver 'deeper'
      reference.deliver 'go'

      actor = spawn phases
      actor.deliver 'deeper'
      recording_trace(kinds: %i[stopped])
      assert_equal 'ok', actor.post('go')
      stopped = nil
      stopped_at = nil
      where = ->(inspection) { [inspection[:step].except(:session), inspection[:position]] }
      20.times do
        allocator.budget(allocator.stats[:live_bytes] + 32)
        response = ta.actor.resume(system: system_handle, actor: actor.handle, budget: 1024)
        allocator.budget(0)
        break unless response[:result] == 'err_nomem'

        inspection = actor.inspect_actor
        next unless inspection[:microstep_in_progress]

        # It stands where its STOPPED record said (ADR 0083).
        record = trace_records.last
        assert_equal 'stopped', record[:kind]
        assert_equal [record[:key], record[:position]], [inspection[:step], inspection[:position]]
        stopped = actor.data('.log')
        stopped_at = where.call(inspection)
        break
      end
      assert stopped, 'no failed allocation stopped the actor inside its microstep'

      image = capture(actor)
      copy_id = core('ta_actor_reconstruct',
                     ta.actor.reconstruct(system: system_handle, image: image,
                                          datamodel_binding: actor.data)).fetch(:out)
      destroy_image(image)
      # Without running the system, which would resume the copy.
      core('ta_actor_destroy', ta.actor.destroy(system: system_handle, actor: actor.handle))
      forget(actor)
      copy = TinyactorsTest::Actor.new(self, copy_id, phases, actor.definition)
      assert copy.inspect_actor[:microstep_in_progress], 'the reconstructed actor is stopped where the original was'
      # A reconstructed actor is a new session (ADR 0066); its steps and
      # position are the captured one's.
      assert_equal stopped_at, where.call(copy.inspect_actor)
      assert_equal stopped, copy.data('.log')
      status = 'paused'
      20.times do
        status = copy.resume(budget: 1024)
        break unless status == 'paused'
      end
      assert_equal 'stable', status
      run_system('dispatch the reconstructed actor\'s effects')
      assert_equal [reference.active, reference.data], [copy.active, copy.data],
                   'the reconstructed actor ended where an uninterrupted one does'
    end

    # Catches: a second actor that finds no continuation to stop into after
    # the first took the system's spare, or that changes before it has one.
    # Every run reserves its continuation before it does anything
    # (ADR 0070), so the second actor's first allocation is that
    # reservation, and failing it leaves the actor untouched.
    scenario "an actor that stops takes the spare continuation, and the next one reserves its own (#{datamodel})" do
      reference = spawn phases
      reference.deliver 'deeper'
      reference.deliver 'go'
      first = spawn phases
      second = spawn phases
      # Posting runs nothing, so both are still to take `go`.
      [first, second].each { |actor| actor.deliver 'deeper' }
      [first, second].each { |actor| assert_equal 'ok', actor.post('go') }

      20.times do
        allocator.budget(allocator.stats[:live_bytes] + 32)
        response = ta.actor.resume(system: system_handle, actor: first.handle, budget: 1024)
        allocator.budget(0)
        break if response[:result] == 'err_nomem' && first.inspect_actor[:microstep_in_progress]
      end
      assert first.inspect_actor[:microstep_in_progress], 'the first actor stopped inside its microstep'

      before = [second.active, second.data]
      allocator.arm_fault(after: 0, count: 1)
      response = ta.actor.resume(system: system_handle, actor: second.handle, budget: 1024)
      allocator.clear_faults
      assert_equal 'err_nomem', response[:result], 'the second actor could not reserve a continuation'
      view = second.inspect_actor
      assert view[:macrostep_in_progress], 'it still holds its event'
      assert !view[:microstep_in_progress], 'it had not begun a microstep'
      assert_equal 'go', event_text(view.dig(:current_event, :name))
      assert_equal before, [second.active, second.data], 'nothing of its macrostep ran'

      assert_equal 'stable', second.resume(budget: 1024)
      assert_equal 'stable', first.resume(budget: 1024)
      run_system('dispatch both actors\' effects')
      [first, second].each do |actor|
        assert_equal [reference.active, reference.data], [actor.active, actor.data], "#{actor} ended as a reference run"
      end
    end
  end

  # Catches: a data model that reads a system variable core could not
  # render for lack of memory as null. The jq data model changes nothing
  # until a program succeeds, so it answers TA_OP_NOMEM, and the program
  # runs again (ADR 0070).
  scenario 'a system variable core could not render stops the run (jq)' do
    result = assert_allocation_failures_invisible do
      actor = spawn :oom_names_jq
      assert_equal 'ok', actor.post(OutOfMemory::LONG)
      run_system('deliver an event with a long name')
    end
    assert_equal [], result[:unreported], 'every failed allocation is reported'
  end

  # ADR 0070's open point (the ECMAScript data model evaluates JavaScript,
  # so a failed rendering after the session changed could not be undone)
  # no longer arises from memory: since core ADR 0092, rendered text lives
  # in the render area allocated with the system, so rendering `_event.name`
  # takes no allocation that can fail, and every allocation failure that is
  # left stays invisible. A name longer than renderBytes fails the same way
  # whatever the memory (napi's test/datamodel.js).
  scenario 'a system variable core could not render stops the run (ecmascript)' do
    assert_allocation_failures_invisible do
      actor = spawn :oom_names_ecmascript
      assert_equal 'ok', actor.post(OutOfMemory::LONG)
      run_system('deliver an event with a long name')
    end
  end
end
