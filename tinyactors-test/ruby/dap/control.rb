# frozen_string_literal: true

module DAP
  # Where a stopped actor stands (core ADR 0083): `key` is [macrostep,
  # microstep] of its step, and the rest names the unit it runs next, which
  # is also the first unit a single step runs. `at` is 'none' between two
  # microsteps and outside a macrostep. A PAUSED record carries the same.
  Position = Data.define(:key, :at, :state, :transition, :action, :iteration) do
    def self.from_inspection(view)
      where = view[:position] || {}
      step = view[:step] || {}
      new(key: [Integer(step[:macrostep] || 0), Integer(step[:microstep] || 0)], at: where[:at].to_s,
          state: where[:state], transition: where[:transition],
          action: Labels.none?(where[:action]) ? nil : Integer(where[:action]),
          iteration: Integer(where[:iteration] || 0))
    end

    # The block the next unit is in: the phase and the state or transition.
    def block = [key, at, at == 'transition' ? transition : state]

    def nowhere? = at == 'none' || at.empty?
  end

  # What the debugger learns about one actor's current macrostep from its
  # trace records, for what inspection does not say: the event it took,
  # the transitions of its microstep, and the summary written when it ends.
  # `key` is [context, session ID]: a session survives a reload (ADR 0066),
  # and a fork's system numbers its sessions on its own.
  # `entry_stopped` says the actor was stopped before this macrostep began
  # (an `on:` breakpoint, or a step of an idle actor), so neither the next
  # round nor its MACROSTEP_BEGIN stops it there again.
  # `pending` holds breakpoints this microstep will reach (Breakpoints),
  # and `fault` the last fault a record carried, for the error event after it.
  # `taken` is the event the macrostep began with, `event` the one of the
  # step it is in, which is _event. `fresh` says its microstep has begun
  # and no unit of it has written a record yet (Breakpoints#first_unit).
  Track = Struct.new(:key, :definition, :in_macrostep, :macrostep, :taken, :event, :microstep,
                     :summary, :entry_stopped, :pending, :fault, :fresh, keyword_init: true)

  # The monitor of every place's Scheduler while a client is attached: it
  # decides the budgets of every resume, reads the trace records each one
  # wrote, and stops the system at a breakpoint, an exception, the end of a
  # step, or a pause. Stopping is all-stop, across the workbench and every
  # fork: nothing runs until the client continues or steps.
  class Control
    # Why the system stopped, and in which thread. `key` is the stopped
    # actor's track key, which a reload or a fork's restore replaces.
    Stop = Struct.new(:thread, :key, :reason, :description, :breakpoints, :exception, keyword_init: true)

    # A step in progress. `start` is the position it started from and
    # `depth` how deeply nested its next unit was. `waiting` is a step of
    # an idle actor, which waits for its next event.
    Step = Struct.new(:kind, :granularity, :thread, :key, :place, :handle, :start, :depth, :waiting,
                      keyword_init: true)

    FINE = { budget: 1, action_budget: 1 }.freeze

    # What one place's Scheduler calls: the Control, told which place.
    class Monitor
      def initialize(control, place)
        @control = control
        @place = place
      end

      def budgets(id) = @control.budgets(@place, id)
      def admit(ids) = @control.admit(@place, ids)
      def resumed(id, status) = @control.resumed(@place, id, status)
      def dispatched(due) = @control.dispatched(@place, due)
      def interrupt? = @control.interrupt?
    end

    attr_reader :stop, :step, :breakpoints

    # `places` answers the workbench session and the forks. `output` is
    # called as output.call(category, text) for the macrostep summaries;
    # `interrupt` answers whether a client message is waiting.
    def initialize(labels:, threads:, breakpoints:, places:, output:, interrupt:)
      @labels = labels
      @threads = threads
      @breakpoints = breakpoints
      @places = places
      @output = output
      @interrupt = interrupt
      @tracks = {}
      @cursors = {}
      @stop = nil
      @step = nil
      @pause_requested = false
      @macrosteps_ended = 0
      @hits = []
      @aside = false
    end

    # Makes this the monitor of a place's scheduler; a place that comes
    # while everything is stopped stops too.
    def adopt(place)
      scheduler = place.context.scheduler
      scheduler.monitor = Monitor.new(self, place)
      scheduler.stop! if stopped?
    end

    # A fork's system is gone: what was kept about its actors goes too.
    def forget(context)
      @tracks.delete_if { |key, _| key.first.equal?(context) }
      @cursors.delete(context)
    end

    def schedulers = @places.call.map { |place| place.context.scheduler }

    def pending? = schedulers.any?(&:pending?)

    def stopped? = !@stop.nil?

    def pause_requested? = @pause_requested

    def track(thread) = @tracks[thread.track_key]

    # _event for `thread`: the event of the step it is in, or the one it is
    # about to take.
    def event_of(thread)
      track = track(thread)
      track&.in_macrostep ? track.event : thread.view&.dig(:current_event)
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
    # The thread is the one a scheduler was settling, when one is inside a
    # macrostep; otherwise the one the client named.
    def halt(reason, thread)
      @places.call.each do |place|
        scheduler = place.context.scheduler
        next unless scheduler.pending? && !scheduler.stopped? && (current = scheduler.current)

        thread = @threads.by_handle(place, current) || thread
        break
      end
      stop_all
      @step = nil
      @pause_requested = false
      @stop = Stop.new(thread: thread&.id, key: thread&.track_key, reason: reason, breakpoints: [])
    end

    # A pause asked for while work is pending stops after the next unit.
    def request_pause(thread)
      return halt('pause', thread) unless pending?
      return halt('pause', thread) if schedulers.any?(&:current)

      @pause_requested = true
      nil
    end

    def resume!
      @hits = []
      @stop = nil
      @step = nil
      @pause_requested = false
      go_all
    end

    # Starts a step of `thread`: stepIn, next or stepOut (`kind` :in, :over,
    # :out) at `granularity` ('instruction', 'statement' or 'line').
    def start_step(thread, kind, granularity)
      collect
      view = thread.view
      raise RequestError, "#{thread.label} is not resident" if view.nil?
      raise RequestError, "#{thread.label} is done: its session ended" if view[:residency] == 'done'

      waiting = !view[:macrostep_in_progress]
      start = Position.from_inspection(view)
      @stop = nil
      @pause_requested = false
      @step = Step.new(kind: kind, granularity: granularity || 'statement', thread: thread.id, key: thread.track_key,
                       place: thread.place, handle: thread.handle, start: start,
                       depth: depth(thread.definition, start), waiting: waiting)
      go_all
      waiting
    end

    # Runs `thread`'s next macrostep, which an operator's jump was just
    # delivered for, and stops with reason `goto` when it ends. Only that
    # actor runs meanwhile.
    def start_goto(thread)
      @stop = nil
      @pause_requested = false
      @step = Step.new(kind: :goto, granularity: 'macrostep', thread: thread.id, key: thread.track_key,
                       place: thread.place, handle: thread.handle, start: nil, depth: 0, waiting: false)
      go_all
      thread.context.scheduler.wake
    end

    # Stops everything again, as `stop` says, after a fork was restored
    # from an image: its actor is a new session.
    def restopped(thread, reason, description = nil)
      @step = nil
      @pause_requested = false
      stop_all
      @stop = Stop.new(thread: thread.id, key: thread.track_key, reason: reason, description: description,
                       breakpoints: [])
    end

    # Runs the block with the debugger out of the way: no budgets, no
    # breakpoints, no stops. A reload needs that: a replacement must finish
    # its bootstrap before it can be activated (Workbench::Session), even
    # while the client has everything stopped. What was stopped stays
    # stopped, and the adapter holds those actors out of the loop meanwhile.
    def aside
      stop = @stop
      @aside = true
      go_all
      yield
    ensure
      @aside = false
      @hits = []
      if stop
        @stop = stop
        stop_all
      end
    end

    # ------------------------------------------------------------------
    # The Schedulers' monitor, through Monitor

    def budgets(_place, _id)
      armed? && !@aside ? FINE : nil
    end

    def admit(place, ids)
      collect
      return ids if @aside

      ids.each { |id| starting(place, id) } if place.respond_to?(:macrostep_starting)
      return ids if @stop

      if @step && !@step.waiting && @step.place.equal?(place)
        return ids.include?(@step.handle) ? [@step.handle] : ids
      end

      ids.each do |id|
        thread = thread_of_handle(place, id)
        next if thread.nil?

        track = track_for(thread)
        next if track.in_macrostep || track.entry_stopped

        event = thread.view&.dig(:current_event)
        if @step&.waiting && @step.key == thread.track_key
          track.entry_stopped = true
          stop_at(thread, 'step', "took #{event_name(thread, event) || 'its next event'}")
          return []
        end
        hits = @breakpoints.taking(thread, event)
        next if hits.empty?

        track.entry_stopped = true
        stop_at(thread, 'function breakpoint', "#{thread.label} takes #{event_name(thread, event)}",
                breakpoints: hits)
        return []
      end
      ids
    end

    def resumed(place, id, status)
      collect
      return nil if @aside
      return :stop if @stop

      thread = thread_of_handle(place, id)
      return nil if thread.nil?

      view = thread.view
      position = view && Position.from_inspection(view)
      track = track(thread)
      @breakpoints.reach(track, position) { |hit| @hits << hit } if track && position
      decide(thread, position, status) ? :stop : nil
    end

    # A delayed send that could not be delivered raises its
    # error.communication outside any run (ADR 0072).
    def dispatched(_place, _due)
      collect
      return nil if @aside
      return :stop if @stop

      decide(nil, nil, nil) ? :stop : nil
    end

    def interrupt? = @interrupt.call

    # Reads the trace records every place wrote since the last read, keeps
    # each actor's track, and writes a summary of every macrostep that
    # ended. A record's `definition` becomes a Definition of its place.
    def collect
      @places.call.each do |place|
        context = place.context
        sink = context.instruments['trace']
        next if sink.nil?

        records = sink.journal(since: @cursors[context]).select { |entry| entry[:op] == 'trace' }
        next if records.empty?

        @cursors[context] = records.last[:seq]
        records.each { |record| absorb(record.merge(definition: Definition.new(context, record[:definition]))) }
      end
    end

    private

    def armed? = !@step.nil? || @pause_requested || @breakpoints.any?

    def stop_all = schedulers.each(&:stop!)

    def go_all = schedulers.each(&:go!)

    # A fork captures an image when one of its actors is about to begin a
    # macrostep (it holds an event and has taken no step of it, ADR 0083),
    # for restartFrame.
    def starting(place, id)
      step = place.inspect_actor(id)&.dig(:step)
      place.macrostep_starting(id) if step && Integer(step[:macrostep]).zero?
    end

    # After a resume of `thread` (nil after a dispatch): stop for an
    # exception, a breakpoint, the end of the step, or a pause, in that
    # order. Answers whether it stopped.
    def decide(thread, position, status)
      hits = @hits
      @hits = []
      if (hit = hits.find { |candidate| candidate[:kind] == :exception })
        stop_at(thread_of_key(hit[:key]) || thread, 'exception', hit[:description], exception: hit)
      elsif (hit = confirmed(hits, :line))
        stop_at(thread_of_key(hit[:key]) || thread, 'breakpoint', hit[:description], breakpoints: hit[:ids])
      elsif (hit = confirmed(hits, :function))
        stop_at(thread_of_key(hit[:key]) || thread, 'function breakpoint', hit[:description],
                breakpoints: hit[:ids])
      elsif (hit = confirmed(hits, :data))
        stop_at(thread_of_key(hit[:key]) || thread, 'data breakpoint', hit[:description], breakpoints: hit[:ids])
      elsif thread && @step && @step.key == thread.track_key && step_done?(@step, thread, position, status)
        stop_at(thread, @step.kind == :goto ? 'goto' : 'step', nil)
      elsif thread && @pause_requested
        stop_at(thread, 'pause', nil)
      else
        return false
      end
      true
    end

    # The first hit of `kind` whose breakpoints fire: their condition holds
    # and their hit condition is met. A log point fires by writing its
    # message and does not stop. Answers the hit with the IDs that stop.
    def confirmed(hits, kind)
      hits.each do |hit|
        next unless hit[:kind] == kind

        thread = thread_of_key(hit[:key])
        firing = hit[:breakpoints].select { |breakpoint| @breakpoints.fire?(breakpoint, thread) }
        logs, stops = firing.partition { |breakpoint| @breakpoints.log_point?(breakpoint) }
        logs.each { |breakpoint| @output.call('console', @breakpoints.log_message(breakpoint, thread)) }
        return hit.merge(ids: stops.map(&:id)) unless stops.empty?
      end
      nil
    end

    def stop_at(thread, reason, description, breakpoints: [], exception: nil)
      @step = nil
      @pause_requested = false
      stop_all
      @stop = Stop.new(thread: thread&.id, key: thread&.track_key, reason: reason, description: description,
                       breakpoints: breakpoints, exception: exception)
    end

    # Whether a step is over after a resume of its actor that answered
    # `status` and left it at `position`.
    #
    # `stepIn` stops before the next unit. At `statement` granularity it
    # passes over a stop that names the unit it started at: a <foreach> that
    # has not run and the same loop before its first iteration have one
    # position (ADR 0083, open point 1), and a debugger that stops twice at
    # one place looks stuck. `next` goes on until the next unit is at the
    # depth it started at or shallower, or in another block; `stepOut` until
    # it is shallower, or in another block, and between microsteps until the
    # macrostep ends. At `line` granularity a step is a microstep.
    def step_done?(step, thread, position, status)
      return true unless status == 'paused'
      return false if step.kind == :goto
      return true if position.nil? || step.start.nil?

      if step.granularity == 'line'
        return false if step.kind == :out

        return position.key != step.start.key
      end
      same = position == step.start
      return !same || step.granularity == 'instruction' if step.kind == :in
      return true if step.granularity == 'instruction' && step.kind == :over

      changed = position.block != step.start.block
      depth = depth(thread.definition, position)
      if step.kind == :over
        changed || (!same && depth <= step.depth)
      else
        return false if step.start.nowhere?

        changed || depth < step.depth
      end
    end

    # How deeply nested the next unit is in <if> and <foreach> bodies.
    def depth(definition, position)
      position.action.nil? ? 0 : @labels.depth(definition, position.action)
    end

    def absorb(record)
      key = [record[:definition].context, record.dig(:key, :session)]
      track = (@tracks[key] ||= new_track(key, record[:definition]))
      track.definition = record[:definition]
      case record[:kind]
      when 'macrostep_begin'
        track.in_macrostep = true
        track.macrostep = Integer(record.dig(:key, :macrostep))
        track.taken = record[:event]
        track.event = record[:event]
        track.microstep = nil
        track.summary = []
        track.pending = []
        track.fault = nil
      when 'microstep'
        track.microstep = record
        track.event = record[:event] if record[:event]
        summarize(track, record)
      when 'macrostep_end', 'finished', 'destroyed'
        finish(track, record)
      when 'abandoned'
        abandoned(track, record)
      else
        summarize(track, record)
      end
      thread = @breakpoints.needs_thread? ? @threads.by_key(key) : nil
      @breakpoints.observe(record, track, thread) { |hit| @hits << hit }
    end

    def finish(track, record)
      if track.in_macrostep && record[:kind] == 'macrostep_end'
        @macrosteps_ended += 1
        write_summary(track, record)
      end
      track.in_macrostep = false
      track.entry_stopped = false
      track.microstep = nil
    end

    # An abandoned macrostep (ADR 0082) ends with MACROSTEP_END when it had
    # begun; one that never began writes ABANDONED alone.
    def abandoned(track, record)
      dropped = Integer(record[:dropped] || 0)
      line = "  abandoned#{dropped.positive? ? ", #{dropped} internal event(s) dropped" : ''}"
      if track.in_macrostep
        track.summary << line
      else
        who = thread_of_key(track.key)&.label || "session #{track.key.last}"
        event = record.dig(:event, :name_text)
        @output.call('console', "#{who}: abandoned #{event || 'its waiting event'} before it began")
        track.entry_stopped = false
      end
    end

    def track_for(thread) = (@tracks[thread.track_key] ||= new_track(thread.track_key, thread.definition))

    def new_track(key, definition)
      Track.new(key: key, definition: definition, in_macrostep: false, summary: [], entry_stopped: false,
                pending: [], fresh: false)
    end

    # ------------------------------------------------------------------
    # Macrostep summaries (output, category console)

    def summarize(track, record)
      return unless track.in_macrostep

      definition = record[:definition]
      line = case record[:kind]
             when 'microstep' then "  #{@labels.microstep(definition, record)}"
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
      who = thread_of_key(track.key)&.label || "session #{track.key.last}"
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
    def thread_of_handle(place, handle)
      @threads.by_handle(place, handle) || (@threads.refresh || @threads.by_handle(place, handle))
    end

    def thread_of_key(key)
      return nil if key.nil?

      @threads.by_key(key) || (@threads.refresh || @threads.by_key(key))
    end

    def event_name(thread, event)
      return nil if event.nil?

      event[:name_text] || thread.context.event_text(event[:name])
    end
  end
end
