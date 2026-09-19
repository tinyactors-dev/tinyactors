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
    attr_reader :name, :datamodel, :binding, :document_name

    BINDINGS = %i[early late].freeze

    # `document_name:` is the SCXML `<scxml name>` (the `_name` system
    # variable) when it differs from the chart's registry name.
    # `datamodel: nil` states no data-model requirement.
    def initialize(name, datamodel: 'jq', binding: nil, document_name: nil)
      raise ChartError, "chart #{name}: binding must be :early or :late" if binding && !BINDINGS.include?(binding)

      @name = name.to_s
      @datamodel = datamodel&.to_s
      @binding = binding
      @document_name = document_name&.to_s
      @data = []
      @states = []
      @initial = nil
      @script = nil
      @send_ids = {}
      @unmatched_cancels = {}
    end

    def self.define(name, **options, &block)
      chart = new(name, **options)
      Builder.new(chart, chart).instance_eval(&block)
      chart.validate!
      chart.freeze
    end

    # Internal: builder hooks.
    attr_reader :states, :data, :script

    # The <scxml> <script>: run once per session, after early data
    # initialization and before the initial transition.
    def script=(source)
      raise ChartError, "chart #{name}: script declared twice; SCXML allows one top-level <script>" if @script

      @script = source.to_s
    end

    def initial=(target)
      raise ChartError, "chart #{name}: initial declared twice" if @initial

      @initial = Array(target).map(&:to_s)
    end

    # Records a literal <send> ID so literal cancels can be checked against it.
    def declare_send_id(id)
      @send_ids[id] = true
    end

    def send_id?(id)
      @send_ids.key?(id)
    end

    # Records a literal cancel that deliberately names no <send> of this
    # chart, such as a send ID of another session.
    def declare_unmatched_cancel(id)
      @unmatched_cancels[id] = true
    end

    def unmatched_cancel?(id)
      @unmatched_cancels.key?(id)
    end

    def to_spec
      spec = { name: document_name || name }
      spec[:datamodel] = datamodel if datamodel
      spec[:data] = @data
      spec[:initial] = { targets: initial_targets }
      spec[:states] = @states.map(&:to_spec)
      spec[:binding] = binding.to_s if binding
      spec[:script] = script if script
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

      # SCXML allows the document's initial states to be any descendants,
      # e.g. one atomic state in each region of a parallel state.
      initial_targets.each { |target| check_target.call(target, 'root initial') }

      each_state do |state|
        state.validate!(self, check_target)
      end
      self
    end

    private

    def initial_targets
      @initial || [@states.reject(&:history?).first&.id.to_s]
    end

    # A state or pseudo-state in the chart tree.
    class State
      attr_reader :id, :parent, :children, :transitions, :onentry, :onexit, :data, :invokes
      attr_accessor :kind, :initial, :initial_actions, :history_default, :history_actions, :done_data

      def initialize(id, kind, parent)
        @id = id.to_s
        @kind = kind
        @parent = parent
        @children = []
        @transitions = []
        @onentry = []
        @onexit = []
        @data = []
        @invokes = []
        @initial = nil
        @initial_actions = nil
        @history_default = nil
        @history_actions = nil
        @done_data = nil
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
          initial_targets.each do |target|
            check_target.call(target, "#{where} initial")
            unless descendant_ids.include?(target)
              raise ChartError, "chart #{chart.name}: #{where} initial #{target} is not a descendant"
            end
          end
        when :atomic, :final
          raise ChartError, "chart #{chart.name}: #{where} declares initial but has no children" if @initial
        when :history_shallow, :history_deep
          raise ChartError, "chart #{chart.name}: history #{id} needs a default target" unless @history_default
          raise ChartError, "chart #{chart.name}: history #{id} must be inside a compound state" if parent.nil?

          @history_default.each do |target|
            check_target.call(target, "history #{id} default")
            unless parent.descendant_ids.include?(target)
              raise ChartError, "chart #{chart.name}: history #{id} default #{target} is outside #{parent.id}"
            end
          end
        end
        if @done_data && resolved_kind != :final
          raise ChartError, "chart #{chart.name}: #{where} declares done_data but is not a final state"
        end

        @transitions.each do |transition|
          transition[:targets]&.each { |target| check_target.call(target, "#{where} transition") }
        end
        [*@onentry, *@onexit, @initial_actions, @history_actions,
         *@transitions.map { |t| t[:actions] }, *@invokes.map { |i| i[:finalize] }].compact.each do |actions|
          Chart.validate_actions!(chart, actions, where)
        end
      end

      def to_spec
        spec = { kind: resolved_kind.to_s, id: id }
        if history?
          transition = { kind: 'external', targets: @history_default }
          transition[:actions] = @history_actions if @history_actions
          spec[:transitions] = [transition]
          return spec
        end

        if resolved_kind == :compound
          spec[:initial] = { targets: initial_targets }
          spec[:initial][:actions] = @initial_actions if @initial_actions
        end
        spec[:states] = @children.map(&:to_spec) unless @children.empty?
        spec[:transitions] = @transitions unless @transitions.empty?
        spec[:onentry] = @onentry.map { |actions| { actions: actions } } unless @onentry.empty?
        spec[:onexit] = @onexit.map { |actions| { actions: actions } } unless @onexit.empty?
        spec[:data] = @data unless @data.empty?
        spec[:invokes] = @invokes unless @invokes.empty?
        spec[:done_data] = @done_data if @done_data
        spec
      end

      private

      def initial_targets
        @initial || [@children.reject(&:history?).first&.id.to_s]
      end
    end

    # Checks literal cancels, descending into conditional and foreach bodies.
    def self.validate_actions!(chart, actions, where)
      actions.each do |action|
        case action[:kind]
        when 'if'
          validate_actions!(chart, action[:then_actions] || [], where)
          validate_actions!(chart, action[:else_actions] || [], where)
        when 'foreach'
          validate_actions!(chart, action[:body] || [], where)
        when 'cancel'
          literal = action.dig(:sendid, :literal)
          next unless literal && !chart.send_id?(literal) && !chart.unmatched_cancel?(literal)

          raise ChartError, "chart #{chart.name}: #{where} cancels send ID #{literal.inspect}, which no <send> declares"
        end
      end
    end

    # Builds a value spec from exactly one of `expr:`, `location:`, or
    # `content:`; nil when none is given.
    def self.value_spec(chart, what, expr: nil, location: nil, content: nil)
      present = { expr: expr, location: location, content: content }.compact
      raise ChartError, "chart #{chart.name}: #{what} takes one of expr:, location:, content:" if present.size > 1

      present.transform_values(&:to_s).first&.then { |kind, source| { kind => source } }
    end

    # Lowers `{ name => value }` to ta_scxml_param_spec entries. A String value
    # is an expression; a Hash is a value spec (`{ location: '.x' }`).
    def self.param_specs(chart, params)
      params.map do |param_name, value|
        value = { expr: value } unless value.is_a?(Hash)
        { name: param_name.to_s, value: value_spec(chart, "param #{param_name}", **value) }
      end
    end

    # Evaluates chart and state blocks. Methods here are the chart DSL.
    class Builder
      def initialize(chart, container, state = nil)
        @chart = chart
        @container = container
        @state = state
      end

      # Declares data: at the chart root, or in a state (bound on entry to that
      # state under late binding). Keyword values are JSON literals; a Hash
      # value with `expr:` or `content:` gives an SCXML expression or inline
      # content instead. Symbols name data declared without a value.
      #
      #   data cycles: 0
      #   data :pending, total: { expr: '.a + .b' }, doc: { content: '<x/>' }
      def data(*unbound, **values)
        target = @state ? @state.data : @chart.data
        unbound.each { |data_name| target << { name: data_name.to_s } }
        values.each do |data_name, value|
          initializer =
            if value.is_a?(Hash) && (value.key?(:expr) || value.key?(:content))
              Chart.value_spec(@chart, "data #{data_name}", **value.slice(:expr, :content))
            else
              { expr: JSON.generate(value) }
            end
          target << { name: data_name.to_s, initializer: initializer }
        end
      end

      # Declares the chart's top-level <script>, a data-model script run once
      # per session at initialization. Inside a state, use an action instead:
      # `on_entry { script '...' }`.
      def script(source)
        if @state
          raise ChartError, "state #{@state.id}: a top-level script belongs at the chart root; use on_entry { script ... }"
        end

        @chart.script = source
      end

      # The initial state(s). Inside a state, a block gives the executable
      # content of SCXML's `<initial><transition>`.
      def initial(*targets, &block)
        if @state
          raise ChartError, "state #{@state.id}: initial declared twice" if @state.initial

          @state.initial = targets.map(&:to_s)
          @state.initial_actions = actions(&block) if block
        else
          raise ChartError, "chart #{@chart.name}: the root initial takes no actions" if block

          @chart.initial = targets
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

      # `default:` is one target or several; a block gives the executable
      # content of the default transition.
      def history(id, default:, type: :shallow, &block)
        raise ChartError, "history #{id} must be inside a state" unless @state
        raise ChartError, "history #{id}: type must be :shallow or :deep" unless %i[shallow deep].include?(type)

        history = State.new(id, :"history_#{type}", @state)
        history.history_default = Array(default).map(&:to_s)
        history.history_actions = actions(&block) if block
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

      # Adds an <onentry> handler. Each call is a separate handler and a
      # separate block of executable content (SCXML 3.8): an error skips the
      # rest of this handler only, and the handlers after it still run.
      def on_entry(&block)
        require_state!('on_entry')
        @state.onentry << actions(&block)
      end

      # Adds an <onexit> handler; each call is a separate block, as for
      # `on_entry` (SCXML 3.9).
      def on_exit(&block)
        require_state!('on_exit')
        @state.onexit << actions(&block)
      end

      # Starts an invocation when the state is entered (SCXML <invoke>).
      #
      #   type:, type_expr:     the invoker, e.g. 'scxml'
      #   src:, src_expr:       the resource to invoke
      #   chart:                an inline child machine: the named chart's spec,
      #                         as JSON, becomes the invoke's <content>
      #   content:, content_expr: other inline <content>
      #   id:, id_location:     the invoke ID
      #   params:               { name => expr or value spec }; SCXML namelist
      #                         entries become `{ location: ... }` params
      #   autoforward:          forward external events to the child
      # A block gives the <finalize> executable content.
      def invoke(type: nil, type_expr: nil, src: nil, src_expr: nil, chart: nil, content: nil,
                 content_expr: nil, id: nil, id_location: nil, params: nil, autoforward: false, &block)
        require_state!('invoke')
        if chart
          raise ChartError, "chart #{@chart.name}: invoke takes chart: or content:, not both" if content || content_expr

          child = TinyactorsTest.charts.fetch(chart.to_sym) do
            raise ChartError, "chart #{@chart.name}: invoke names chart #{chart}, which is not defined yet"
          end
          content = JSON.generate(child.to_spec)
        end

        invoke = { kind: 'invoke' }
        invoke[:id] = Chart.pair(@chart, 'id', { literal: id&.to_s }, { location: id_location })
        invoke[:type] = Chart.pair(@chart, 'type', { literal: type&.to_s }, { expr: type_expr })
        invoke[:src] = Chart.pair(@chart, 'src', { literal: src&.to_s }, { expr: src_expr })
        invoke[:data] = Chart.value_spec(@chart, 'invoke content', expr: content_expr, content: content)
        invoke[:autoforward] = true if autoforward
        invoke[:params] = Chart.param_specs(@chart, params) if params
        invoke[:finalize] = actions(&block) if block
        @state.invokes << invoke.compact
      end

      # The <donedata> of a final state: inline content (`content:` or
      # `expr:`), or `params:`.
      def done_data(content: nil, expr: nil, params: nil)
        require_state!('done_data')
        raise ChartError, "chart #{@chart.name}: done_data takes content or params, not both" if params && (content || expr)

        done = {}
        value = Chart.value_spec(@chart, 'done_data', expr: expr, content: content)
        done[:content] = value if value
        done[:params] = Chart.param_specs(@chart, params) if params
        @state.done_data = done
      end

      private

      def require_state!(what)
        raise ChartError, "chart #{@chart.name}: #{what} must be inside a state" unless @state
      end

      def add_state(id, kind, initial: nil, &block)
        parent = @state
        state = State.new(id, kind, parent)
        state.initial = Array(initial).map(&:to_s) if initial
        (parent ? parent.children : @chart.states) << state
        Builder.new(@chart, state, state).instance_eval(&block) if block
        state
      end

      def actions(&block)
        ActionBuilder.new(@chart).tap { |builder| builder.instance_eval(&block) }.actions
      end
    end

    # Returns the one present side of an `x` / `xexpr` pair, or nil.
    def self.pair(chart, name, literal, expression)
      present = [literal, expression].reject { |side| side.values.first.nil? }
      raise ChartError, "chart #{chart.name}: #{name} and its expression form are exclusive" if present.size > 1

      present.first
    end

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
      #   event_expr:  an expression for the event name (`eventexpr`)
      #   delay:       an SCXML duration ("5s", "500ms") or Integer milliseconds
      #   id:          a literal send ID that `cancel` can refer to
      #   to:          a literal target such as "#_parent"
      #   type:        a registered I/O processor name
      #   data:        an expression for the event data
      #   content:     inline <content> for the event data
      #   params:      { name => expr or value spec }; SCXML namelist entries
      #                become `{ location: ... }` params
      def send_event(event = nil, event_expr: nil, delay: nil, delay_expr: nil, id: nil, id_location: nil,
                     to: nil, to_expr: nil, type: nil, type_expr: nil, data: nil, content: nil, params: nil)
        action = { kind: 'send' }
        action[:event] = pair('event', { name: event&.to_s }, { expr: event_expr })
        action[:id] = pair('id', { literal: id&.to_s }, { location: id_location })
        action[:type] = pair('type', { literal: type&.to_s }, { expr: type_expr })
        action[:target] = pair('to', { literal: to&.to_s }, { expr: to_expr })
        action[:delay] = pair('delay', { literal: duration(delay) }, { expr: delay_expr })
        action[:data] = Chart.value_spec(@chart, 'send data', expr: data, content: content)
        action[:params] = Chart.param_specs(@chart, params) if params
        @chart.declare_send_id(id.to_s) if id
        @actions << action.compact
      end

      # Cancels a delayed send by its literal ID, or by an expression that
      # evaluates to the ID. A literal ID must match a literal <send> ID of the
      # chart unless `unmatched: true` says it deliberately does not.
      def cancel(id = nil, expr: nil, unmatched: false)
        sendid = pair('sendid', { literal: id&.to_s }, { expr: expr })
        raise ChartError, "chart #{@chart.name}: cancel needs an ID or expr:" unless sendid
        raise ChartError, "chart #{@chart.name}: unmatched: applies to literal IDs" if unmatched && id.nil?

        @chart.declare_unmatched_cancel(id.to_s) if unmatched

        @actions << { kind: 'cancel', sendid: sendid }
      end

      # Assigns an expression, or inline content (`content:`), to a location.
      def assign(location, expr = nil, content: nil)
        value = Chart.value_spec(@chart, "assign #{location}", expr: expr, content: content)
        raise ChartError, "chart #{@chart.name}: assign #{location} needs an expression or content:" unless value

        @actions << { kind: 'assign', location: location, value: value }
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

      # SCXML <if>/<elseif>/<else>, lowered to nested conditionals:
      #
      #   conditional do
      #     branch('.x == 1') { raise_event 'one' }
      #     branch('.x == 2') { raise_event 'two' }
      #     otherwise { raise_event 'other' }
      #   end
      def conditional(&block)
        branches = ConditionalBuilder.new(@chart).tap { |builder| builder.instance_eval(&block) }
        @actions << branches.to_spec
      end

      # SCXML <foreach>: `item` and `index` name the variables bound on each
      # iteration.
      def foreach(array, item:, index: nil, &block)
        action = { kind: 'foreach', array: array, item: item.to_s }
        action[:index] = index.to_s if index
        action[:body] = ActionBuilder.new(@chart).tap { |builder| builder.instance_eval(&block) }.actions if block
        @actions << action
      end

      private

      def pair(name, literal, expression)
        Chart.pair(@chart, name, literal, expression)
      end

      def duration(delay)
        return nil if delay.nil?
        return "#{delay}ms" if delay.is_a?(Integer) && delay >= 0
        return delay if delay.is_a?(String) && DURATION.match?(delay)

        raise ChartError, "chart #{@chart.name}: delay #{delay.inspect} is not an SCXML duration such as \"5s\" or \"500ms\""
      end
    end

    # Collects the branches of one `conditional`.
    class ConditionalBuilder
      def initialize(chart)
        @chart = chart
        @branches = []
        @otherwise = nil
      end

      def branch(condition, &block)
        raise ChartError, "chart #{@chart.name}: branch after otherwise" if @otherwise

        @branches << [condition, body(&block)]
      end

      def otherwise(&block)
        raise ChartError, "chart #{@chart.name}: otherwise given twice" if @otherwise

        @otherwise = body(&block)
      end

      def to_spec
        raise ChartError, "chart #{@chart.name}: conditional needs a branch" if @branches.empty?

        @branches.reverse.reduce(@otherwise) do |else_actions, (condition, then_actions)|
          spec = { kind: 'if', condition: condition }
          spec[:then_actions] = then_actions unless then_actions.empty?
          spec[:else_actions] = else_actions if else_actions && !else_actions.empty?
          [spec]
        end.first
      end

      private

      def body(&block)
        return [] unless block

        ActionBuilder.new(@chart).tap { |builder| builder.instance_eval(&block) }.actions
      end
    end
  end
end
