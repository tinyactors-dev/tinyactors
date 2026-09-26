# frozen_string_literal: true

module DAP
  # Names for the compiled IDs trace records carry, read from the
  # definition with core's lookups by ID (ADR 0042, ADR 0071), and cached:
  # a definition never changes while it is registered, except for where its
  # elements are written (see forget_spans).
  #
  # Core supplies facts and the host builds labels (ADR 0071 section 3):
  # `active → dunning on payment.failed` for a transition, `assign 2` for
  # the second action of its block.
  # A registered definition, named by the system (Context) it is registered
  # in and its ID there: a fork's system numbers its definitions on its own.
  Definition = Data.define(:context, :id) do
    def to_s = id.to_s
  end

  class Labels
    NONE = 4_294_967_295
    KINDS = %i[state transition action expression].freeze

    def initialize
      @cache = {}
    end

    def self.none?(id) = id.nil? || Integer(id) == NONE

    def state(definition, id)
      return nil if Labels.none?(id)

      lookup(definition, :state, id)&.fetch(:id)
    end

    def state_parent(definition, id)
      info = lookup(definition, :state, id)
      info.nil? || Labels.none?(info[:parent]) ? nil : Integer(info[:parent])
    end

    # Every state of the definition, as [id, info], in ID order.
    def states(definition)
      @cache[[definition, :all_states]] ||= (0..).lazy.map { |id| [id, lookup(definition, :state, id)] }
                                                  .take_while { |_, info| info }.to_a
    end

    # `source → targets on event`; an internal transition without targets
    # is `source on event`, an eventless one leaves out `on`.
    def transition(definition, id)
      info = lookup(definition, :transition, id)
      return "transition #{id}" if info.nil?

      source = state(definition, info[:source]) || 'the document'
      targets = Array(info[:targets]).map { |target| state(definition, target) }
      text = targets.empty? ? source.to_s : "#{source} → #{targets.join(' ')}"
      event = info[:event].to_s
      event.empty? ? text : "#{text} on #{event}"
    end

    # What a MICROSTEP record took: its transitions, or an operator's jump
    # (core ADR 0082), which names no transition and lists its targets in
    # `states` and its source in `state`.
    def microstep(definition, record)
      transitions = Array(record[:transitions])
      targets = Array(record[:states])
      if transitions.empty? && !targets.empty?
        from = Labels.none?(record[:state]) ? '' : " from #{state(definition, record[:state])}"
        return "goto #{targets.map { |target| state(definition, target) }.join(' ')}#{from}"
      end
      return 'initial' if transitions.empty?

      transitions.map { |id| transition(definition, id) }.join('; ')
    end

    def transition_info(definition, id) = lookup(definition, :transition, id)

    def action_info(definition, id) = lookup(definition, :action, id)

    # `assign 2`: the element and its place in its block, from 1.
    def action(definition, id)
      info = lookup(definition, :action, id)
      return "action #{id}" if info.nil?

      "#{info[:kind]} #{Integer(info[:index]) + 1}"
    end

    # The <if> and <foreach> actions around an action, innermost first.
    def enclosing(definition, id)
      chain = []
      info = action_info(definition, id)
      while info && %w[then else foreach].include?(info[:block])
        owner = Integer(info[:owner])
        chain << owner
        info = action_info(definition, owner)
      end
      chain
    end

    # How deep an action is nested in <if> and <foreach> bodies: 0 for an
    # action of a state's or a transition's own block.
    def depth(definition, id) = enclosing(definition, id).size

    # The source text of an expression, or nil for a host expression. An
    # expression with a span crosses the RPC as {source:, span:}.
    def expression(definition, id)
      return nil if Labels.none?(id)

      text = lookup(definition, :expression, id)
      text = text[:source] if text.is_a?(Hash)
      text.is_a?(String) ? text : nil
    end

    # Where an element was written (core ADR 0086): {source:, line:,
    # column:, end_line:, end_column:}, or nil when the definition keeps no
    # span for it. `kind` is one of KINDS.
    def span(definition, kind, id)
      return nil if id.nil? || Labels.none?(id)

      key = [definition, :span, kind, Integer(id)]
      return @cache[key] if @cache.key?(key)

      context = definition.context
      response = context.ta.definition.span(system: context.system_handle, definition: definition.id, kind: kind.to_s,
                                            id: Integer(id))
      where = response[:result] == 'ok' ? response.dig(:out, :out) : nil
      line = where&.dig(:span, :line).to_i
      @cache[key] = if line.positive?
                      { source: where[:source].to_s, line: line, column: where.dig(:span, :column).to_i,
                        end_line: where.dig(:span, :end_line).to_i, end_column: where.dig(:span, :end_column).to_i }
                    end
    end

    # Drops every span read so far. A chart registered again after it moved
    # is answered by the definition registered before (core ADR 0077), which
    # then says where the chart is written now (core ADR 0086).
    def forget_spans = @cache.delete_if { |key, _| key[1] == :span }

    # The definition's family and revision: the same in every system that
    # registers the revision (ADR 0085).
    def identity(definition) = facts(definition)[:identity]

    # How many elements of `kind` the definition has (ADR 0071's counts).
    def count(definition, kind)
      Integer(facts(definition).fetch(:"#{kind}_count"))
    end

    private

    def facts(definition)
      @cache[[definition, :facts]] ||= begin
        context = definition.context
        context.core('ta_definition_inspect',
                     context.ta.definition.inspect(system: context.system_handle, definition: definition.id))
               .fetch(:out)
      end
    end

    def lookup(definition, kind, id)
      key = [definition, kind, Integer(id)]
      return @cache[key] if @cache.key?(key)

      context = definition.context
      response = context.ta.definition.public_send(kind, system: context.system_handle,
                                                         definition: definition.id, kind => Integer(id))
      @cache[key] = response[:result] == 'ok' ? response.dig(:out, :out) : nil
    end
  end
end
