# frozen_string_literal: true

module DAP
  # The stack of a thread, innermost first: the unit that ran last and the
  # <if> and <foreach> around it, the block (`onentry of busy`, a
  # transition's content), the microstep, and the macrostep with its event.
  # An actor between macrosteps has one frame that says where it rests.
  #
  # There are no source locations yet: they need source spans (core ADR
  # 0086), so every frame has line 0. `Frame#position` is what a source map
  # will turn into a line.
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

      track = @control.track(thread.session)
      unless track&.in_macrostep
        event = view[:current_event]
        name = event ? @session.context.event_text(event[:name]) : 'no event'
        return [["about to take #{name} (before transition selection)", 'normal']]
      end

      block_frames(track, definition) + step_frames(track, definition)
    end

    def block_frames(track, definition)
      position = track.position
      return [] if position.nil? || position.at == 'none'

      frames = []
      last = track.last_action
      if last && last[:block] == position.block
        frames << if last[:kind] == 'foreach'
                    ["#{@labels.action(definition, last[:action])} iteration #{last[:iteration] + 1}", 'normal']
                  else
                    ["after #{@labels.action(definition, last[:action])}", 'normal']
                  end
        innermost = true
        @labels.enclosing(definition, last[:action]).each do |owner|
          info = @labels.action_info(definition, owner)
          label = @labels.action(definition, owner)
          label += " iteration #{position.iteration + 1}" if info&.dig(:kind) == 'foreach' && innermost
          innermost = false if info&.dig(:kind) == 'foreach'
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
      when 'enter' then "entering #{state}"
      when 'select' then 'selecting transitions'
      when 'finalize' then "finalize in #{state}"
      when 'invoke' then "starting the invocations of #{state}"
      when 'bootstrap' then 'bootstrap'
      else position.at
      end
    end

    def step_frames(track, definition)
      frames = []
      micro = track.microstep
      if micro && micro.dig(:key, :macrostep) == track.macrostep
        transitions = Array(micro[:transitions]).map { |id| @labels.transition(definition, id) }
        frames << ["microstep #{track.macrostep}.#{micro.dig(:key, :microstep)}: #{transitions.join('; ')}",
                   'normal']
      end
      event = track.taken ? track.taken[:name_text] : 'no event'
      frames << ["macrostep #{track.macrostep} on #{event}", 'normal']
    end
  end
end
