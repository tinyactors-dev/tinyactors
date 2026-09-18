# frozen_string_literal: true

module TinyactorsTest
  class AssertionFailed < StandardError; end
  class ScenarioError < StandardError; end

  # Describes why a scenario is expected to fail. A scenario that fails for any
  # other reason is an ordinary failure, and one that passes is an unexpected
  # pass, so the marker cannot hide new bugs.
  class ExpectedFailure
    attr_reader :reason, :rpc_error, :rpc_method, :message

    def initialize(reason, rpc_error: nil, rpc_method: nil, message: nil)
      if rpc_error.nil? && message.nil?
        raise ScenarioError, "expect_failure #{reason.inspect}: give rpc_error: or message:"
      end
      if rpc_method && rpc_error.nil?
        raise ScenarioError, "expect_failure #{reason.inspect}: rpc_method: needs rpc_error:"
      end

      @reason = reason
      @rpc_error = rpc_error
      @rpc_method = rpc_method
      @message = message
    end

    def matches?(error)
      if rpc_error
        return false unless error.is_a?(RpcError) && error.code == rpc_error
        return false if rpc_method && error.rpc_method != rpc_method
      end
      return false if message && !message.match?(error.message)

      true
    end

    def describe
      parts = []
      parts << "RPC error #{rpc_error}" if rpc_error
      parts << "from #{rpc_method}" if rpc_method
      parts << "message #{message.inspect}" if message
      "#{reason} (#{parts.join(', ')})"
    end
  end

  Scenario = Data.define(:group, :name, :block, :expected_failure, :location) do
    def full_name
      "#{group.name}: #{name}"
    end
  end

  Invariant = Data.define(:name, :block)

  # A named set of scenarios that share a default chart and invariants.
  class Group
    attr_reader :name, :chart, :scenarios, :invariants

    def initialize(name, chart:)
      @name = name.to_s
      @chart = chart&.to_sym
      @scenarios = []
      @invariants = []
      @pending_failure = nil
    end

    # Marks the next scenario as an expected failure.
    def expect_failure(reason, **matcher)
      raise ScenarioError, "#{name}: expect_failure given twice before one scenario" if @pending_failure

      @pending_failure = ExpectedFailure.new(reason, **matcher)
    end

    # Checked against every actor after spawn, advance_time, and deliver.
    def invariant(name, &block)
      @invariants << Invariant.new(name: name, block: block)
    end

    def scenario(name, &block)
      raise ScenarioError, "#{self.name}: scenario #{name.inspect} needs a block" unless block

      @scenarios << Scenario.new(group: self, name: name, block: block,
                                 expected_failure: @pending_failure,
                                 location: block.source_location.join(':'))
      @pending_failure = nil
    end

    def finish!
      return unless @pending_failure

      raise ScenarioError, "#{name}: expect_failure #{@pending_failure.reason.inspect} has no scenario"
    end
  end

  # A spawned actor, seen from a scenario.
  class Actor
    attr_reader :handle, :chart

    def initialize(context, handle, chart)
      @context = context
      @handle = handle
      @chart = chart
    end

    # The active configuration as state ID symbols.
    def active
      inspect_actor.fetch(:configuration).map(&:to_sym)
    end

    # The data-model root, or the single value a jq filter produces from it.
    def data(filter = '.')
      root = inspect_actor.fetch(:data)
      return root if filter == '.'

      outputs = @context.rpc('json.transform', filter: filter, input: root)
      unless outputs.size == 1
        raise AssertionFailed, "data filter #{filter.inspect} produced #{outputs.size} values, expected 1"
      end

      outputs.first
    end

    def delayed_sends
      inspect_actor.fetch(:delayed_sends)
    end

    # Delivers an external event and runs the system until it is quiescent.
    def deliver(event, data: nil)
      params = { actor: handle, event: TinyactorsTest.event_atoms(event) }
      params[:data] = data unless data.nil?
      @context.rpc('actor.send', params)
      @context.run_system("deliver #{event}")
    end

    def inspect_actor
      @context.rpc('actor.inspect', actor: handle)
    end

    def to_s
      "#{chart}(#{handle})"
    end
  end

  # The receiver of a scenario block: the steps and assertions of the DSL.
  class Context
    attr_reader :now

    def initialize(driver, group, charts)
      @driver = driver
      @group = group
      @charts = charts
      @definitions = {}
      @system = nil
      @actors = []
      @now = 0
    end

    def spawn(chart_name = @group.chart)
      raise ScenarioError, "#{@group.name}: spawn needs a chart name" if chart_name.nil?

      chart_name = chart_name.to_sym
      definition = definition_handle(chart_name)
      result = rpc('actor.spawn', system: system_handle, definition: definition)
      actor = Actor.new(self, result.fetch(:actor), chart_name)
      @actors << actor
      run_system("spawn #{chart_name}")
      actor
    end

    # Advances logical time by `milliseconds` and runs due work.
    def advance_time(milliseconds)
      unless milliseconds.is_a?(Integer) && milliseconds >= 0
        raise ScenarioError, "advance_time needs a non-negative Integer of milliseconds"
      end

      @now += milliseconds
      rpc('system.set_time', system: system_handle, time: @now)
      run_system("advance_time #{milliseconds} (t=#{@now}ms)")
    end

    # Escape hatch for methods the DSL does not wrap yet.
    def rpc(method, params = nil)
      @driver.call(method, params)
    end

    def run_system(step)
      rpc('system.run', system: system_handle)
      check_invariants(step)
    end

    def assert(condition, message = 'assertion failed')
      raise AssertionFailed, message unless condition
    end

    def assert_equal(expected, actual, message = nil)
      return if expected == actual

      raise AssertionFailed, [message, "expected #{expected.inspect}, got #{actual.inspect}"].compact.join(': ')
    end

    # Passes when every named state is in the active configuration.
    def assert_active(actor, *states)
      active = actor.active
      missing = states.map(&:to_sym) - active
      return if missing.empty?

      raise AssertionFailed, "#{actor} at t=#{@now}ms: #{missing.join(', ')} not active; active: #{active.join(', ')}"
    end

    def refute_active(actor, *states)
      active = actor.active
      present = states.map(&:to_sym) & active
      return if present.empty?

      raise AssertionFailed, "#{actor} at t=#{@now}ms: #{present.join(', ')} unexpectedly active"
    end

    # Passes when the active configuration is exactly the named states.
    def assert_configuration(actor, *states)
      active = actor.active
      return if active.sort == states.map(&:to_sym).sort

      raise AssertionFailed,
            "#{actor} at t=#{@now}ms: expected configuration #{states.join(', ')}, got #{active.join(', ')}"
    end

    private

    def system_handle
      @system ||= rpc('system.create', {}).fetch(:system)
    end

    def definition_handle(chart_name)
      @definitions[chart_name] ||= begin
        chart = @charts.fetch(chart_name) { raise ScenarioError, "unknown chart #{chart_name}" }
        rpc('definition.compile', chart.to_spec).fetch(:definition)
      end
    end

    def check_invariants(step)
      @group.invariants.each do |invariant|
        @actors.each do |actor|
          next if invariant.block.call(actor)

          raise AssertionFailed,
                "invariant #{invariant.name.inspect} violated by #{actor} after #{step}; active: #{actor.active.join(', ')}"
        end
      end
    end
  end

  def self.event_atoms(event)
    atoms = event.to_s.split('.', -1)
    raise ScenarioError, "invalid event name #{event.inspect}" if atoms.empty? || atoms.any?(&:empty?)

    atoms
  end
end
