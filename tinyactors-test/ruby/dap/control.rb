# frozen_string_literal: true

module DAP
  # Where an actor stands between two units of a macrostep, from the last
  # PAUSED record it wrote (ADR 0061, ADR 0067). `key` is [macrostep,
  # microstep]. Core writes PAUSED with the phase, the state or transition
  # and the <foreach> iteration, but with no action: which unit runs next is
  # not in the record, so frames name the unit that ran last instead.
  #
  # Core ADR 0083 puts the paused position into ta_actor_inspection. Once it
  # is there, `Position.from_inspection` belongs beside `from_record`, so an
  # actor that was paused before the adapter saw it has a position too.
  Position = Data.define(:key, :at, :state, :transition, :iteration) do
    def self.from_record(record)
      where = record[:position] || {}
      new(key: [Integer(record.dig(:key, :macrostep)), Integer(record.dig(:key, :microstep))],
          at: where[:at].to_s, state: where[:state], transition: where[:transition],
          iteration: Integer(where[:iteration] || 0))
    end

    # The block a position is in: the phase and the state or transition.
    def block = [key, at, at == 'transition' ? transition : state]
  end

  # What the debugger knows about one actor's current macrostep, from its
  # trace records. Keyed by session ID, which survives a reload (ADR 0066).
  # `entry_stopped` says the actor was stopped before this macrostep began
  # (an `on:` breakpoint, or a step of an idle actor), so neither the next
  # round nor its MACROSTEP_BEGIN stops it there again.
  # `pending` holds breakpoints this microstep will reach (Breakpoints),
  # and `fault` the last fault a record carried, for the error event after it.
  # `taken` is the event the macrostep began with, `event` the one of the
  # step it is in, which is _event.
  Track = Struct.new(:session, :definition, :in_macrostep, :macrostep, :taken, :event, :microstep, :position,
                     :last_action, :summary, :entry_stopped, :pending, :fault, keyword_init: true)

  # The Scheduler's monitor while a client is attached: it decides the
  # budgets of every resume, reads the trace records each one wrote, and
  # stops the system at a breakpoint, an exception, the end of a step, or a
  # pause. Stopping is all-stop: nothing runs until the client continues or
  # steps.
  class Control
    # Why the system stopped, and in which thread.
    # `session` is the stopped actor's session, which a reload replaces.
    Stop = Struct.new(:thread, :session, :reason, :description, :breakpoints, :exception, keyword_init: true)

    # A step in progress. `start` is the position it started from and
    # `depth` how deeply nested the last unit that ran there was. `waiting`
    # is a step of an idle actor, which waits for its next event.
    Step = Struct.new(:kind, :granularity, :thread, :session, :handle, :start, :depth, :waiting,
                      keyword_init: true)

    FINE = { budget: 1, action_budget: 1 }.freeze

    attr_reader :stop, :step, :breakpoints

    # `output` is called as output.call(category, text) for the macrostep
    # summaries; `interrupt` answers whether a client message is waiting.
    def initialize(session:, labels:, threads:, breakpoints:, output:, interrupt:)
      @session = session
      @labels = labels
      @threads = threads
      @breakpoints = breakpoints
      @output = output
      @interrupt = interrupt
      @tracks = {}
      @cursor = nil
      @stop = nil
      @step = nil
      @pause_requested = false
      @macrosteps_ended = 0
      @hits = []
      @aside = false
    end

    def context = @session.context
    def scheduler = context.scheduler

    def stopped? = !@stop.nil?

    def pause_requested? = @pause_requested

    def track(session_id) = @tracks[session_id]

    # _event for `thread`: the event of the step it is in, or the one it is
    # about to take.
    def event_of(thread)
      track = @tracks[thread.session]
      track&.in_macrostep ? track.event : @session.inspect_actor(thread.handle)&.dig(:current_event)
    end

    # How many macrosteps ended since the last call; the adapter sends
    # `invalidated` when any did.
    def take_macrosteps_ended
      count = @macrosteps_ended
      @macrosteps_ended = 0
      count
    end

    # ------------------------------------------------------------------
    # Commands

    # Stops everything where it stands, as a `pause` or `stopOnEntry` does.
    # The thread is the one the scheduler was settling, when it is inside
    # a macrostep; otherwise the one the client named.
    def halt(reason, thread)
      if scheduler.pending? && !scheduler.stopped? && (current = scheduler.current)
        thread = @threads.by_handle(current) || thread
      end
      scheduler.stop!
      @step = nil
      @pause_requested = false
      @stop = Stop.new(thread: thread&.id, session: thread&.session, reason: reason, breakpoints: [])
    end

    # A pause asked for while work is pending stops after the next unit.
    def request_pause(thread)
      return halt('pause', thread) unless scheduler.pending?

      if scheduler.current
        halt('pause', thread)
      else
        @pause_requested = true
        nil
      end
    end

    def resume!
      @hits = []
      @stop = nil
      @step = nil
      @pause_requested = false
      scheduler.go!
    end

    # Starts a step of `thread`: stepIn, next or stepOut (`kind` :in, :over,
    # :out) at `granularity` ('instruction', 'statement' or 'line').
    def start_step(thread, kind, granularity)
      collect
      view = @session.inspect_actor(thread.handle)
      raise RequestError, "#{thread.label} is not resident" if view.nil?
      raise RequestError, "#{thread.label} is done: its session ended" if view[:residency] == 'done'

      track = (@tracks[thread.session] ||= new_track(thread.session, thread.definition))
      waiting = !view[:macrostep_in_progress]
      start = track.in_macrostep ? track.position : nil
      @stop = nil
      @pause_requested = false
      @step = Step.new(kind: kind, granularity: granularity || 'statement', thread: thread.id,
                       session: thread.session, handle: thread.handle, start: start,
                       depth: start_depth(track, start), waiting: waiting)
      scheduler.go!
      waiting
    end

    # Runs the block with the debugger out of the way: no budgets, no
    # breakpoints, no stops. A reload needs that: a replacement must finish
    # its bootstrap before it can be activated (Workbench::Session), even
    # while the client has everything stopped. What was stopped stays
    # stopped, and the adapter holds those actors out of the loop meanwhile.
    def aside
      stop = @stop
      @aside = true
      scheduler.go!
      yield
    ensure
      @aside = false
      @hits = []
      if stop
        @stop = stop
        scheduler.stop!
      end
    end

    # ------------------------------------------------------------------
    # The Scheduler's monitor

    def budgets(_id)
      armed? && !@aside ? FINE : nil
    end

    def admit(ids)
      collect
      return ids if @stop || @aside

      if @step && !@step.waiting
        return ids.include?(@step.handle) ? [@step.handle] : ids
      end

      ids.each do |id|
        thread = thread_of_handle(id)
        next if thread.nil?

        track = (@tracks[thread.session] ||= new_track(thread.session, thread.definition))
        next if track.in_macrostep || track.entry_stopped

        event = current_event(id)
        if @step&.waiting && @step.handle == id
          track.entry_stopped = true
          stop_at(thread, 'step', "took #{event_name(event) || 'its next event'}")
          return []
        end
        hits = @breakpoints.taking(thread, event)
        next if hits.empty?

        track.entry_stopped = true
        stop_at(thread, 'function breakpoint', "#{thread.label} takes #{event_name(event)}", breakpoints: hits)
        return []
      end
      ids
    end

    def resumed(id, status)
      records = collect
      return nil if @aside
      return :stop if @stop

      thread = thread_of_handle(id)
      return nil if thread.nil?

      decide(thread, records, status) ? :stop : nil
    end

    # A delayed send that could not be delivered raises its
    # error.communication outside any run (ADR 0072).
    def dispatched(_due)
      collect
      return nil if @aside
      return :stop if @stop

      decide(nil, [], nil) ? :stop : nil
    end

    def interrupt? = @interrupt.call

    # Reads the trace records written since the last read, keeps each
    # actor's track, and writes a summary of every macrostep that ended.
    # Answers the records.
    def collect
      sink = context.instruments['trace']
      return [] if sink.nil?

      records = sink.journal(since: @cursor).select { |entry| entry[:op] == 'trace' }
      @cursor = records.last[:seq] unless records.empty?
      records.each { |record| absorb(record) }
      records
    end

    private

    def armed? = !@step.nil? || @pause_requested || @breakpoints.any?

    # After a resume of `thread` (nil after a dispatch): stop for an
    # exception, a function or data breakpoint, the end of the step, or a
    # pause, in that order. Answers whether it stopped.
    def decide(thread, records, status)
      hits = @hits
      @hits = []
      if (hit = hits.find { |candidate| candidate[:kind] == :exception })
        stop_at(thread_of_session(hit[:session]) || thread, 'exception', hit[:description], exception: hit)
      elsif (hit = confirmed(hits, :function))
        stop_at(thread_of_session(hit[:session]) || thread, 'function breakpoint', hit[:description],
                breakpoints: hit[:ids])
      elsif (hit = confirmed(hits, :data))
        stop_at(thread_of_session(hit[:session]) || thread, 'data breakpoint', hit[:description],
                breakpoints: hit[:ids])
      elsif thread && @step && @step.session == thread.session &&
            step_done?(@step, thread, records.select { |record| record.dig(:key, :session) == thread.session },
                       status)
        stop_at(thread, 'step', nil)
      elsif thread && @pause_requested
        stop_at(thread, 'pause', nil)
      else
        return false
      end
      true
    end

    # The first hit of `kind` whose breakpoints fire: their condition holds
    # and their hit condition is met. Answers it with their IDs.
    def confirmed(hits, kind)
      hits.each do |hit|
        next unless hit[:kind] == kind

        thread = thread_of_session(hit[:session])
        ids = hit[:breakpoints].select { |breakpoint| @breakpoints.fire?(breakpoint, thread) }.map(&:id)
        return hit.merge(ids: ids) unless ids.empty?
      end
      nil
    end

    def stop_at(thread, reason, description, breakpoints: [], exception: nil)
      @step = nil
      @pause_requested = false
      scheduler.stop!
      @stop = Stop.new(thread: thread&.id, session: thread&.session, reason: reason, description: description,
                       breakpoints: breakpoints, exception: exception)
    end

    # Whether a step is over after a resume of its actor that wrote
    # `records` and answered `status`.
    def step_done?(step, thread, records, status)
      return true unless status == 'paused'

      track = @tracks[thread.session]
      position = track&.position
      return true if position.nil? || step.start.nil?

      if step.granularity == 'line'
        return false if step.kind == :out

        return position.key != step.start.key
      end
      return true if step.kind == :in || step.granularity == 'instruction'

      changed = position.block != step.start.block
      depth = last_depth(thread, records)
      if step.kind == :over
        changed || depth.nil? || depth <= step.depth
      else
        return false if step.start.at == 'none'

        changed || (!depth.nil? && depth < step.depth)
      end
    end

    # How deeply nested the last unit that ran was, among `records`; nil
    # when none of them names an action (a <log>, for one, writes none).
    def last_depth(thread, records)
      record = records.reverse.find { |candidate| !Labels.none?(candidate.dig(:position, :action)) }
      record && @labels.depth(record[:definition], record.dig(:position, :action))
    end

    def start_depth(track, start)
      last = track.last_action
      return 0 if start.nil? || last.nil? || last[:block] != start.block

      @labels.depth(track.definition, last[:action])
    end

    def absorb(record)
      session = record.dig(:key, :session)
      track = (@tracks[session] ||= new_track(session, record[:definition]))
      track.definition = record[:definition]
      case record[:kind]
      when 'macrostep_begin'
        track.in_macrostep = true
        track.macrostep = Integer(record.dig(:key, :macrostep))
        track.taken = record[:event]
        track.event = record[:event]
        track.microstep = nil
        track.position = Position.new(key: [track.macrostep, 0], at: 'none', state: nil, transition: nil,
                                      iteration: 0)
        track.last_action = nil
        track.summary = []
        track.pending = []
        track.fault = nil
      when 'microstep'
        track.microstep = record
        track.event = record[:event] if record[:event]
        track.position = Position.from_record(record)
        summarize(track, record)
      when 'paused'
        track.position = Position.from_record(record)
      when 'macrostep_end', 'finished', 'destroyed'
        finish(track, record)
      else
        summarize(track, record)
      end
      unless Labels.none?(record.dig(:position, :action))
        position = Position.from_record(record)
        track.last_action = { action: record.dig(:position, :action), block: position.block, kind: record[:kind],
                              iteration: position.iteration }
      end
      thread = @breakpoints.watching_data? ? @threads.by_session(session) : nil
      @breakpoints.observe(record, track, thread) { |hit| @hits << hit }
    end

    def finish(track, record)
      if track.in_macrostep && record[:kind] == 'macrostep_end'
        @macrosteps_ended += 1
        write_summary(track, record)
      end
      track.in_macrostep = false
      track.entry_stopped = false
      track.position = nil
      track.last_action = nil
      track.microstep = nil
    end

    def new_track(session, definition)
      Track.new(session: session, definition: definition, in_macrostep: false, summary: [], entry_stopped: false,
                pending: [])
    end

    # ------------------------------------------------------------------
    # Macrostep summaries (output, category console)

    def summarize(track, record)
      return unless track.in_macrostep

      definition = record[:definition]
      line = case record[:kind]
             when 'microstep'
               transitions = Array(record[:transitions]).map { |id| @labels.transition(definition, id) }
               transitions.empty? ? nil : "  #{transitions.join('; ')}"
             when 'condition'
               text = @labels.expression(definition, record[:expr])
               outcome = record[:has_fault] ? "fault #{record.dig(:fault, :message) || record.dig(:fault, :code)}" : record[:result]
               "  cond #{text} = #{outcome}"
             when 'assign' then "  assign #{@labels.expression(definition, record[:expr])} = #{JSON.generate(record[:value])}"
             when 'sent' then "  sent #{record.dig(:event, :name_text)}"
             when 'raised' then "  raised #{record.dig(:event, :name_text)}"
             when 'cancelled' then "  cancelled send #{record[:send]}"
             when 'action_failed' then "  #{@labels.action(definition, record[:action])} failed: #{fault_text(record)}"
             when 'script' then record[:has_fault] ? "  script failed: #{fault_text(record)}" : nil
             when 'ignored' then "  ignored #{record.dig(:event, :name_text)}"
             when 'invoked' then '  invoked'
             end
      track.summary << line if line
    end

    def write_summary(track, record)
      thread = thread_of_session(track.session)
      who = thread&.label || "session #{track.session}"
      event = track.taken ? "on #{track.taken[:name_text]}" : '(no event)'
      states = Array(record[:states]).map { |state| @labels.state(record[:definition], state) }
      text = "#{who}: macrostep #{track.macrostep} #{event} -> #{states.join(' ')}"
      @output.call('console', ([text] + track.summary).join("\n"))
      track.summary = []
    end

    def fault_text(record)
      fault = record[:fault] || {}
      fault[:message] || "code #{fault[:code]}"
    end

    # ------------------------------------------------------------------

    # A child spawned since the threads were last read is found by reading
    # them again.
    def thread_of_handle(handle)
      @threads.by_handle(handle) || (@threads.refresh || @threads.by_handle(handle))
    end

    def thread_of_session(session)
      @threads.by_session(session) || (@threads.refresh || @threads.by_session(session))
    end

    def current_event(handle)
      view = @session.inspect_actor(handle)
      view && view[:current_event]
    end

    def event_name(event)
      return nil if event.nil?

      event[:name_text] || context.event_text(event[:name])
    end
  end
end
