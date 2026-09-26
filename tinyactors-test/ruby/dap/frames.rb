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
  # There are no source locations yet: they need source spans (core ADR
  # 0086), so every frame has line 0.
  class Frames
    Frame = Struct.new(:id, :thread, :name, :hint, keyword_init: true) do
      def to_dap = { 'id' => id, 'name' => name, 'line' => 0, 'column' => 0, 'presentationHint' => hint }
    end

    def initialize(session, labels, threads, control)
      @session = session
      @labels = labels
      @threads = threads
      @control = control
      @frames = {}
      @next_id = 0
    end

    # Frame IDs are valid until the system runs again.
    def reset
      @frames = {}
    end

    def [](id) = @frames[Integer(id)]

    def for(thread)
      names(thread).map do |name, hint|
        frame = Frame.new(id: (@next_id += 1), thread: thread.id, name: name, hint: hint)
        @frames[frame.id] = frame
        frame
      end
    end

    private

    def names(thread)
      view = @session.inspect_actor(thread.handle)
      return [['gone', 'label']] if view.nil?

      definition = view[:definition_id]
      return [["done in #{@labels.state(definition, view[:final_state]) || 'its final state'}", 'normal']] if view[:residency] == 'done'
      return [["idle in #{Array(@threads.leaves(thread.handle, view)).join(' ')}", 'normal']] unless view[:macrostep_in_progress]

      position = Position.from_inspection(view)
      if position.key[0].zero?
        event = view[:current_event]
        name = event ? @session.context.event_text(event[:name]) : 'no event'
        return [["about to take #{name} (before transition selection)", 'normal']]
      end

      track = @control.track(thread.session)
      unit_frames(position, definition) + step_frames(position, track, definition)
    end

    def unit_frames(position, definition)
      return [['selecting transitions', 'normal']] if position.nowhere?

      frames = []
      action = position.action
      if action
        info = @labels.action_info(definition, action)
        foreach_next = info&.dig(:kind) == 'foreach'
        label = @labels.action(definition, action)
        label += " iteration #{position.iteration + 1}" if foreach_next
        frames << [label, 'normal']
        # The iteration is the innermost loop's: the one this action is in,
        # unless the action is a loop itself (ADR 0083, open point 2).
        innermost = !foreach_next
        @labels.enclosing(definition, action).each do |owner|
          label = @labels.action(definition, owner)
          if @labels.action_info(definition, owner)&.dig(:kind) == 'foreach'
            label += " iteration #{position.iteration + 1}" if innermost
            innermost = false
          end
          frames << [label, 'normal']
        end
      end
      frames << [block_name(position, definition), 'normal']
    end

    def block_name(position, definition)
      state = @labels.state(definition, position.state)
      case position.at
      when 'exit' then "onexit of #{state}"
      when 'transition' then "transition #{@labels.transition(definition, position.transition)}"
      when 'enter' then entering(position, definition, state)
      when 'select' then 'selecting transitions'
      when 'finalize' then "finalize in #{state}"
      when 'invoke' then "starting the invocations of #{state}"
      when 'bootstrap' then 'bootstrap'
      else position.at
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
        frames << ["microstep #{macrostep}.#{microstep}#{what}", 'normal']
      end
      event = track&.in_macrostep && track.macrostep == macrostep && track.taken ? " on #{track.taken[:name_text]}" : ''
      frames << ["macrostep #{macrostep}#{event}", 'normal']
    end
  end
end
