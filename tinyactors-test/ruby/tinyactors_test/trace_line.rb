# frozen_string_literal: true

module TinyactorsTest
  # One trace record (ADR 0067) as a line a scenario can compare exactly:
  #
  #   begin 2 go                      a macrostep, and the event it took
  #   cond 2.0 select:a t0 true       a guard of transition 0, in state a
  #   micro 2.1 go [t0] exit[a] enter[b] history[h]
  #   raised 2.1 transition:a inner   an event queued in transition content
  #   end 2 [b]                       stable, with the active configuration
  #
  # The key is macrostep.microstep; states and events are named, and
  # transitions, expressions and actions are compiled IDs.
  class TraceLine
    def initialize(context, record)
      @context = context
      @record = record
    end

    def to_s
      kind = @record[:kind]
      parts = [NAMES.fetch(kind, kind), key]
      parts << where if position?
      parts.concat(details(kind))
      parts.compact.join(' ')
    end

    NAMES = {
      'macrostep_begin' => 'begin', 'macrostep_end' => 'end', 'microstep' => 'micro', 'condition' => 'cond',
      'data_init' => 'data', 'action_failed' => 'failed', 'invoke_cancelled' => 'uninvoked'
    }.freeze
    LIFECYCLE = %w[spawned loaded unloaded finished destroyed].freeze
    MACROSTEP = %w[macrostep_begin macrostep_end].freeze

    private

    def key
      return nil if LIFECYCLE.include?(@record[:kind])

      macrostep = @record.dig(:key, :macrostep)
      MACROSTEP.include?(@record[:kind]) ? macrostep.to_s : "#{macrostep}.#{@record.dig(:key, :microstep)}"
    end

    def position? = @record.dig(:position, :at) && @record.dig(:position, :at) != 'none'

    def where
      at = @record.dig(:position, :at)
      state = @record.dig(:position, :state)
      state_none?(state) ? at : "#{at}:#{state_name(state)}"
    end

    def details(kind)
      case kind
      when 'macrostep_begin' then [event]
      when 'macrostep_end' then [states(:states)]
      when 'microstep'
        history = Array(@record[:history])
        [event, transitions, "exit#{states(:exited)}", "enter#{states(:entered)}",
         history.empty? ? nil : "history#{states(:history)}"]
      when 'condition'
        transition = @record.dig(:position, :transition)
        [none?(transition) ? 'if' : "t#{transition}", outcome(@record[:result].to_s)]
      when 'raised', 'sent' then [event]
      when 'forwarded' then ["v#{@record[:invoke]}", event]
      when 'data_init' then [@record[:name], @record[:has_fault] ? fault : nil]
      when 'script' then [@record[:has_fault] ? fault : nil]
      when 'foreach' then ["i#{@record[:iteration]}"]
      when 'action_failed' then ["a#{@record[:action]}", fault]
      when 'finished' then [state_name(@record[:state])]
      else []
      end
    end

    def outcome(result) = @record[:has_fault] ? fault : result

    def fault = "!#{@record.dig(:fault, :code)}"

    def event
      event = @record[:event]
      event.nil? ? '-' : event[:name_text]
    end

    def transitions = "[#{Array(@record[:transitions]).map { |id| "t#{id}" }.join(',')}]"

    def states(name) = "[#{Array(@record[name]).map { |state| state_name(state) }.join(',')}]"

    def state_name(state) = @context.state_name(@record[:definition], state)

    STATE_NONE = 4_294_967_295
    def state_none?(state) = state.nil? || Integer(state) == STATE_NONE
    def none?(id) = id.nil? || Integer(id) == STATE_NONE
  end
end
