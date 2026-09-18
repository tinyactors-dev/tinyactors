# frozen_string_literal: true

require 'json'

module TinyactorsTest
  class ChartError < StandardError; end

  # A statechart described in Ruby and serialized as JSON that mirrors
  # ta_scxml_spec (see tinyactors-core/src/builder.h and docs/protocol.md).
  #
  # The spec carries SCXML authoring values only, never runtime identifiers:
  # send IDs are the author's strings, delays are SCXML durations, and I/O
  # processors are names. Expressions are data-model source strings; with the
  # jq data model they are jq filters over the actor's root.
  class Chart
    attr_reader :name, :datamodel, :binding

    BINDINGS = %i[early late].freeze

    def initialize(name, datamodel: 'jq', binding: nil)
      raise ChartError, "chart #{name}: binding must be :early or :late" if binding && !BINDINGS.include?(binding)

      @name = name.to_s
      @datamodel = datamodel.to_s
      @binding = binding
      @data = []
      @states = []
      @initial = nil
      @send_ids = {}
    end

    def self.define(name, **options, &block)
      chart = new(name, **options)
      Builder.new(chart, chart).instance_eval(&block)
      chart.validate!
      chart.freeze
    end

    # Internal: builder hooks.
    attr_reader :states, :data

    def initial=(target)
      raise ChartError, "chart #{name}: initial declared twice" if @initial

      @initial = target
    end

    # Records a literal <send> ID so literal cancels can be checked against it.
    def declare_send_id(id)
      @send_ids[id] = true
    end

    def send_id?(id)
      @send_ids.key?(id)
    end

    def to_spec
      spec = {
        name: name,
        datamodel: datamodel,
        data: @data,
        initial: { targets: [initial_target] },
        states: @states.map(&:to_spec)
      }
      spec[:binding] = binding.to_s if binding
      spec
    end

    def to_json(*args)
      to_spec.to_json(*args)
    end

    def state_ids
      each_state.map(&:id)
    end

    def each_state(&block)
      return enum_for(:each_state) unless block

      @states.each { |state| state.each_state(&block) }
    end

    def validate!
      raise ChartError, "chart #{name}: no states" if @states.empty?

      ids = state_ids
      duplicates = ids.tally.select { |_, count| count > 1 }.keys
      raise ChartError, "chart #{name}: duplicate state IDs #{duplicates.join(', ')}" unless duplicates.empty?

      known = ids.to_h { |id| [id, true] }
      check_target = lambda do |target, where|
        raise ChartError, "chart #{name}: #{where} targets unknown state #{target.inspect}" unless known[target]
      end

      root_ids = @states.map(&:id)
      check_target.call(initial_target, 'root initial')
      unless root_ids.include?(initial_target)
        raise ChartError, "chart #{name}: root initial #{initial_target} is not a top-level state"
      end

      each_state do |state|
        state.validate!(self, check_target)
      end
      self
    end

    private

    def initial_target
      (@initial || @states.reject(&:history?).first&.id).to_s
    end

    # A state or pseudo-state in the chart tree.
    class State
      attr_reader :id, :parent, :children, :transitions, :onentry, :onexit
      attr_accessor :kind, :initial, :history_default

      def initialize(id, kind, parent)
        @id = id.to_s
        @kind = kind
        @parent = parent
        @children = []
        @transitions = []
        @onentry = []
        @onexit = []
        @initial = nil
        @history_default = nil
      end

      def history?
        %i[history_shallow history_deep].include?(kind)
      end

      def each_state(&block)
        yield self
        @children.each { |child| child.each_state(&block) }
      end

      def descendant_ids
        @children.flat_map { |child| [child.id, *child.descendant_ids] }
      end

      def resolved_kind
        return kind if kind != :auto

        @children.any? { |child| !child.history? } ? :compound : :atomic
      end

      def validate!(chart, check_target)
        where = "state #{id}"
        case resolved_kind
        when :compound
          target = initial_target
          check_target.call(target, "#{where} initial")
          unless @children.map(&:id).include?(target) || descendant_ids.include?(target)
            raise ChartError, "chart #{chart.name}: #{where} initial #{target} is not a descendant"
          end
        when :atomic, :final
          raise ChartError, "chart #{chart.name}: #{where} declares initial but has no children" if @initial
        when :history_shallow, :history_deep
          raise ChartError, "chart #{chart.name}: history #{id} needs a default target" unless @history_default
          raise ChartError, "chart #{chart.name}: history #{id} must be inside a compound state" if parent.nil?

          check_target.call(@history_default, "history #{id} default")
          unless parent.descendant_ids.include?(@history_default)
            raise ChartError, "chart #{chart.name}: history #{id} default #{@history_default} is outside #{parent.id}"
          end
        end

        @transitions.each do |transition|
          transition[:targets]&.each { |target| check_target.call(target, "#{where} transition") }
        end
        [@onentry, @onexit, *@transitions.map { |t| t[:actions] || [] }].each do |actions|
          Chart.validate_actions!(chart, actions, where)
        end
      end

      def to_spec
        spec = { kind: resolved_kind.to_s, id: id }
        if history?
          spec[:transitions] = [{ kind: 'external', targets: [@history_default] }]
          return spec
        end

        spec[:initial] = { targets: [initial_target] } if resolved_kind == :compound
        spec[:states] = @children.map(&:to_spec) unless @children.empty?
        spec[:transitions] = @transitions unless @transitions.empty?
        spec[:onentry] = @onentry unless @onentry.empty?
        spec[:onexit] = @onexit unless @onexit.empty?
        spec
      end

      private

      def initial_target
        (@initial || @children.reject(&:history?).first&.id).to_s
      end
    end

    def self.validate_actions!(chart, actions, where)
      actions.each do |action|
        literal = action[:kind] == 'cancel' && action.dig(:sendid, :literal)
        next unless literal && !chart.send_id?(literal)

        raise ChartError, "chart #{chart.name}: #{where} cancels send ID #{literal.inspect}, which no <send> declares"
      end
    end

    # Evaluates chart and state blocks. Methods here are the chart DSL.
    class Builder
      def initialize(chart, container, state = nil)
        @chart = chart
        @container = container
        @state = state
      end

      # Declares top-level data. Values are JSON literals; use `expr:` for a
      # data-model expression evaluated at initialization.
      def data(values = nil, **literals)
        raise ChartError, "chart #{@chart.name}: data belongs at the chart root" if @state

        (values || literals).each do |data_name, value|
          initializer = value.is_a?(Hash) && value.key?(:expr) ? value[:expr].to_s : JSON.generate(value)
          @chart.data << { name: data_name.to_s, initializer: { expr: initializer } }
        end
      end

      def initial(target)
        if @state
          raise ChartError, "state #{@state.id}: initial declared twice" if @state.initial

          @state.initial = target.to_s
        else
          @chart.initial = target
        end
      end

      def state(id, initial: nil, &block)
        add_state(id, :auto, initial: initial, &block)
      end

      def parallel(id, &block)
        add_state(id, :parallel, &block)
      end

      def final(id, &block)
        add_state(id, :final, &block)
      end

      def history(id, default:, type: :shallow)
        raise ChartError, "history #{id} must be inside a state" unless @state
        raise ChartError, "history #{id}: type must be :shallow or :deep" unless %i[shallow deep].include?(type)

        history = State.new(id, :"history_#{type}", @state)
        history.history_default = default.to_s
        @state.children << history
      end

      # `on:` accepts one event descriptor or several (SCXML's space-separated
      # list). Omit it for an eventless transition.
      def transition(on: nil, target: nil, cond: nil, type: :external, &block)
        require_state!('transition')
        transition = { kind: type.to_s }
        transition[:event] = Array(on).join(' ') if on
        transition[:condition] = cond if cond
        transition[:targets] = Array(target).map(&:to_s) if target
        transition[:actions] = actions(&block) if block
        @state.transitions << transition
      end

      def on_entry(&block)
        require_state!('on_entry')
        @state.onentry.concat(actions(&block))
      end

      def on_exit(&block)
        require_state!('on_exit')
        @state.onexit.concat(actions(&block))
      end

      private

      def require_state!(what)
        raise ChartError, "chart #{@chart.name}: #{what} must be inside a state" unless @state
      end

      def add_state(id, kind, initial: nil, &block)
        parent = @state
        state = State.new(id, kind, parent)
        state.initial = initial&.to_s
        (parent ? parent.children : @chart.states) << state
        Builder.new(@chart, state, state).instance_eval(&block) if block
        state
      end

      def actions(&block)
        ActionBuilder.new(@chart).tap { |builder| builder.instance_eval(&block) }.actions
      end
    end

    # Evaluates executable content blocks.
    # Evaluates executable content blocks. Keyword pairs mirror SCXML's
    # `x` / `xexpr` attributes: the plain keyword is literal text, the `_expr`
    # (or `_location`) keyword is a data-model expression.
    class ActionBuilder
      # SCXML durations: CSS2 time values such as "500ms" or "2.5s".
      DURATION = /\A\d+(\.\d+)?(ms|s)\z/

      attr_reader :actions

      def initialize(chart)
        @chart = chart
        @actions = []
      end

      def raise_event(event, data: nil)
        action = { kind: 'raise', event: event.to_s }
        action[:data] = { expr: data } if data
        @actions << action
      end

      # Sends an event, by default to this actor's external queue through the
      # SCXML Event I/O Processor.
      #
      #   delay:  an SCXML duration ("5s", "500ms") or Integer milliseconds
      #   id:     a literal send ID that `cancel` can refer to
      #   to:     a literal target such as "#_parent"
      #   type:   a registered I/O processor name
      def send_event(event, delay: nil, delay_expr: nil, id: nil, id_location: nil,
                     to: nil, to_expr: nil, type: nil, type_expr: nil, data: nil)
        action = { kind: 'send', event: { name: event.to_s } }
        action[:id] = pair('id', { literal: id&.to_s }, { location: id_location })
        action[:type] = pair('type', { literal: type&.to_s }, { expr: type_expr })
        action[:target] = pair('to', { literal: to&.to_s }, { expr: to_expr })
        action[:delay] = pair('delay', { literal: duration(delay) }, { expr: delay_expr })
        action[:data] = { expr: data } if data
        @chart.declare_send_id(id.to_s) if id
        @actions << action.compact
      end

      # Cancels a delayed send by its literal ID, or by an expression that
      # evaluates to the ID.
      def cancel(id = nil, expr: nil)
        sendid = pair('sendid', { literal: id&.to_s }, { expr: expr })
        raise ChartError, "chart #{@chart.name}: cancel needs an ID or expr:" unless sendid

        @actions << { kind: 'cancel', sendid: sendid }
      end

      def assign(location, expr)
        @actions << { kind: 'assign', location: location, value: { expr: expr } }
      end

      def script(source)
        @actions << { kind: 'script', script: source }
      end

      def log(label = nil, expr: nil)
        action = { kind: 'log' }
        action[:label] = label.to_s if label
        action[:expr] = expr if expr
        @actions << action
      end

      private

      # Returns the one present side of an `x` / `xexpr` pair, or nil.
      def pair(name, literal, expression)
        present = [literal, expression].reject { |side| side.values.first.nil? }
        raise ChartError, "chart #{@chart.name}: #{name} and its expression form are exclusive" if present.size > 1

        present.first
      end

      def duration(delay)
        return nil if delay.nil?
        return "#{delay}ms" if delay.is_a?(Integer) && delay >= 0
        return delay if delay.is_a?(String) && DURATION.match?(delay)

        raise ChartError, "chart #{@chart.name}: delay #{delay.inspect} is not an SCXML duration such as \"5s\" or \"500ms\""
      end
    end
  end
end
