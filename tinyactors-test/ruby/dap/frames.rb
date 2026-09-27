# frozen_string_literal: true

module DAP
  # The stack of a thread, innermost first: the unit it runs next and the
  # <if> and <foreach> around it, the block (`onentry of busy`, a
  # transition's content), the microstep, and the macrostep with its event.
  # The top frame is where the actor stands, as a debugger's current line
  # is: what a step runs next (core ADR 0083). Between two microsteps that
  # is transition selection. An actor between macrosteps has one frame that
  # says where it rests.
  #
  # Where the actor stands comes from inspection, so an actor that paused
  # before the adapter saw it, or was reloaded from an image, has a
  # position too. The transitions of its microstep and the event of its
  # macrostep come from the trace records (Control), and are left out when
  # the adapter did not see them.
  #
  # Each frame is at the place its element was written (core ADR 0085): an
  # action's line, a transition's, or a state's, in the chart's Ruby file.
  # A chart compiled without spans (one an `invoke chart:` inlines) is shown
  # as the SCXML the codec encodes from it, a virtual source, at the place
  # the encode wrote each element (Catalog#location).
  #
  # Below the macrostep, after a `label` frame, comes the causal chain: the
  # step that sent the event the macrostep took, then the step that sent
  # that one's, across actors, back to the host (Control#chain).
  class Frames
    Frame = Struct.new(:id, :thread, :name, :hint, :where, keyword_init: true) do
      def to_dap
        body = { 'id' => id, 'name' => name, 'line' => 0, 'column' => 0, 'presentationHint' => hint }
        return body if where.nil?

        body['source'] = if where[:reference]
                           { 'name' => where[:name], 'sourceReference' => where[:reference],
                             'presentationHint' => 'deemphasize' }
                         else
                           { 'name' => File.basename(where[:source]), 'path' => where[:source] }
                         end
        body['line'] = where[:line]
        body['column'] = [where[:column], 1].max
        body['endLine'] = where[:end_line] if where[:end_line].positive?
        body['endColumn'] = where[:end_column] if where[:end_column].positive?
        body
      end
    end

    # `locator` answers location(definition, kind, id) for a definition
    # without spans (Catalog); `origins` are the I/O processors of the
    # debug console, by registry index (Expressions#io_origins); `console`
    # answers the mail waiting for an actor.
    def initialize(labels, threads, control, locator, origins, console)
      @labels = labels
      @console = console
      @origins = origins
      @threads = threads
      @control = control
      @locator = locator
      @frames = {}
      @next_id = 0
    end

    # Frame IDs are valid until the system runs again.
    def reset
      @frames = {}
    end

    def [](id) = @frames[Integer(id)]

    def for(thread)
      frames(thread).map do |name, where, hint|
        frame = Frame.new(id: (@next_id += 1), thread: thread.id, name: name, hint: hint || 'normal', where: where)
        @frames[frame.id] = frame
        frame
      end
    end

    private

    # [[name, where]] innermost first; `where` is a span or nil.
    def frames(thread)
      view = thread.view
      return [['gone', nil]] if view.nil?

      definition = thread.definition
      if view[:residency] == 'done'
        final = view[:final_state]
        return [["done in #{@labels.state(definition, final) || 'its final state'}", span(definition, :state, final)]]
      end
      return [["idle in #{leaves(thread, view).join(' ')}#{held_note(thread)}", leaf_span(thread, view)]] unless view[:macrostep_in_progress]

      position = Position.from_inspection(view)
      if position.key[0].zero?
        event = view[:current_event]
        name = event ? thread.context.event_text(event[:name]) : 'no event'
        return [["about to take #{name} (before transition selection)", leaf_span(thread, view)]]
      end

      unit_frames(position, definition) + step_frames(position, @control.track(thread), definition) +
        cause_frames(thread)
    end

    # The causal chain, after a label frame.
    def cause_frames(thread)
      hops = @control.chain(thread)
      return [] if hops.empty?

      [['caused by', nil, 'label']] + hops.map do |hop|
        if hop[:host]
          [host_name(hop[:event]), nil, 'subtle']
        elsif hop[:unseen]
          ["#{hop[:thread]&.title || 'an actor'}: step #{hop[:step]}, which the debugger did not see", nil, 'subtle']
        else
          ["#{hop[:thread]&.title || 'an actor'}: sent #{hop[:event]} in step #{hop[:step]}",
           span(hop[:definition], :action, hop[:action])]
        end
      end
    end

    def host_name(event)
      name = event&.dig(:name_text)
      origin = event&.dig(:origin)
      from_console = origin&.dig(:kind) == 'io' && @origins.key?(origin.dig(:io, :processor))
      from_console ? "the debug console sent #{name}" : "the host delivered #{name}"
    end

    def unit_frames(position, definition)
      return [['selecting transitions', nil]] if position.nowhere?

      frames = []
      action = position.action
      if action
        info = @labels.action_info(definition, action)
        foreach_next = info&.dig(:kind) == 'foreach'
        label = @labels.action(definition, action)
        label += " iteration #{position.iteration + 1}" if foreach_next
        frames << [label, span(definition, :action, action)]
        # The iteration is the innermost loop's: the one this action is in,
        # unless the action is a loop itself (ADR 0083, open point 2).
        innermost = !foreach_next
        @labels.enclosing(definition, action).each do |owner|
          label = @labels.action(definition, owner)
          if @labels.action_info(definition, owner)&.dig(:kind) == 'foreach'
            label += " iteration #{position.iteration + 1}" if innermost
            innermost = false
          end
          frames << [label, span(definition, :action, owner)]
        end
      end
      frames << block_frame(position, definition)
    end

    def block_frame(position, definition)
      state = @labels.state(definition, position.state)
      where = span(definition, :state, position.state)
      case position.at
      when 'exit' then ["onexit of #{state}", where]
      when 'transition'
        # An operator's jump with no content stands at its start with no
        # compiled transition to name (ADR 0086 section 3).
        return ['taking an operator\'s jump', nil] if Labels.none?(position.transition)

        ["transition #{@labels.transition(definition, position.transition)}",
         span(definition, :transition, position.transition)]
      when 'enter' then [entering(position, definition, state), where]
      when 'select' then ['selecting transitions', nil]
      when 'finalize' then ["finalize in #{state}", where]
      when 'invoke' then ["starting the invocations of #{state}", where]
      when 'bootstrap' then ['bootstrap', nil]
      else [position.at, nil]
      end
    end

    # The next action says which content of the state runs: its onentry,
    # or the content of its initial or history default.
    def entering(position, definition, state)
      outer = position.action && @labels.action_info(definition, @labels.enclosing(definition, position.action).last || position.action)
      outer&.dig(:block) == 'onentry' ? "onentry of #{state}" : "entering #{state}"
    end

    def step_frames(position, track, definition)
      macrostep, microstep = position.key
      frames = []
      micro = track&.microstep
      if microstep.positive?
        taken = micro && micro.dig(:key, :macrostep) == macrostep && micro.dig(:key, :microstep) == microstep
        what = taken ? ": #{@labels.microstep(definition, micro)}" : ''
        frames << ["microstep #{macrostep}.#{microstep}#{what}", taken ? microstep_span(definition, micro) : nil]
      end
      event = track&.in_macrostep && track.macrostep == macrostep && track.taken ? " on #{track.taken[:name_text]}" : ''
      frames << ["macrostep #{macrostep}#{event}", nil]
    end

    # A chart's microstep is where its first transition was written; an
    # operator's jump (ADR 0082) where its first target was.
    def microstep_span(definition, record)
      transition = Array(record[:transitions]).first
      return span(definition, :transition, transition) if transition

      span(definition, :state, Array(record[:states]).first)
    end

    def leaves(thread, view) = Array(@threads.leaves(thread.handle, view))

    # What holds an actor at rest: the system is stopped, and so many
    # console messages wait for it.
    def held_note(thread)
      return '' unless @control.stopped? || thread.context.scheduler.held?(thread.handle)

      waiting = @console.mail_for(thread).size
      " (held; #{waiting} message#{waiting == 1 ? '' : 's'} waiting)"
    end

    def leaf_span(thread, view)
      name = leaves(thread, view).first
      id = name && @labels.states(thread.definition).find { |_, info| info[:id] == name }&.first
      span(thread.definition, :state, id)
    end

    def span(definition, kind, id)
      return nil if definition.nil? || id.nil? || Labels.none?(id)

      @labels.span(definition, kind, id) || @locator.location(definition, kind, id)
    end
  end
end
