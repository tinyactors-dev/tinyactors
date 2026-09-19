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

  # A core function answered with a result other than TA_OK.
  class CoreResultError < StandardError
    attr_reader :function, :result

    def initialize(function, result)
      @function = function
      @result = result
      super("#{function} returned #{result}")
    end
  end

  # A spawned actor, seen from a scenario. All observations go through the
  # public inspection API: ta_actor_inspect, ta_actor_active_states, and
  # ta_definition_state.
  class Actor
    attr_reader :handle, :chart, :definition

    def initialize(context, handle, chart, definition)
      @context = context
      @handle = handle
      @chart = chart
      @definition = definition
    end

    # The active configuration as state ID symbols, ancestors included.
    def active
      count = inspect_actor.fetch(:active_state_count)
      states = @context.core('ta_actor_active_states',
                             @context.ta.actor.active_states(system: @context.system_handle,
                                                             actor: handle, capacity: count))
      states.fetch(:states).map { |state| @context.state_name(definition, state) }
    end

    # The data-model root, or the single value a jq filter produces from it.
    def data(filter = '.')
      inspection = inspect_actor
      unless inspection[:has_datamodel_root]
        raise ScenarioError, "#{self}: the data model reported no inspection root"
      end

      root = inspection.fetch(:datamodel_root)
      return root if filter == '.'

      outputs = @context.rpc('json.transform', filter: filter, input: root)
      unless outputs.size == 1
        raise AssertionFailed, "data filter #{filter.inspect} produced #{outputs.size} values, expected 1"
      end

      outputs.first
    end

    def delayed_sends
      inspect_actor.fetch(:delayed_send_count)
    end

    # True once the actor reached a top-level final state.
    def done?
      inspect_actor.fetch(:residency) == 'done'
    end

    # The done data of a finished actor (its top-level final state's
    # <donedata>), or nil when it has none.
    def completion
      out = @context.core('ta_actor_get_completion',
                          @context.ta.actor.get_completion(system: @context.system_handle, actor: handle)).fetch(:out)
      out[:has_data] ? out[:data] : nil
    end

    # Delivers an external event and runs the system until it is quiescent.
    def deliver(event, data: nil)
      params = { name: @context.event_name(event), type: 'external', invoke_id: INVOKE_ID_NONE }
      params[:data] = data unless data.nil?
      @context.core('ta_actor_deliver',
                    @context.ta.actor.deliver(system: @context.system_handle, actor: handle, event: params))
      @context.run_system("deliver #{event}")
    end

    def inspect_actor
      @context.core('ta_actor_inspect',
                    @context.ta.actor.inspect(system: @context.system_handle, actor: handle)).fetch(:out)
    end

    def to_s
      "#{chart}(#{handle})"
    end
  end

  # TA_INVOKE_ID_NONE; 64-bit values above 2^53-1 travel as decimal strings.
  INVOKE_ID_NONE = '18446744073709551615'

  # The receiver of a scenario block: the steps and assertions of the DSL.
  # Steps drive the generated ta.* bindings of the public C API directly.
  class Context
    ALLOCATOR = { '$adapter' => 'allocator.libc' }.freeze
    # The data-model adapter for each chart datamodel. A chart without a
    # datamodel (`datamodel: nil`) states no requirement and runs with jq.
    DATAMODELS = {
      'jq' => { '$adapter' => 'datamodel.jq' }.freeze,
      'ecmascript' => { '$adapter' => 'datamodel.ecmascript' }.freeze
    }.freeze
    DEFAULT_DATAMODEL = 'jq'
    # Microsteps per ta_actor_resume call, and dispatch rounds before a step
    # is declared non-quiescent.
    RESUME_BUDGET = 1024
    DISPATCH_BUDGET = 64
    MAX_ROUNDS = 10_000

    attr_reader :now

    def initialize(driver, group, charts)
      @driver = driver
      @group = group
      @charts = charts
      @definitions = {}
      @event_names = {}
      @state_names = {}
      @system = nil
      @actors = []
      @now = 0
    end

    def spawn(chart_name = @group.chart)
      raise ScenarioError, "#{@group.name}: spawn needs a chart name" if chart_name.nil?

      chart_name = chart_name.to_sym
      definition = definition_id(chart_name)
      id = core('ta_actor_spawn', ta.actor.spawn(system: system_handle, definition: definition)).fetch(:out)
      actor = Actor.new(self, id, chart_name, definition)
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
      core('ta_system_set_time', ta.system.set_time(system: system_handle, now: @now))
      run_system("advance_time #{milliseconds} (t=#{@now}ms)")
    end

    # Advances logical time from one delayed-send deadline to the next until
    # `actor` is done. Fails when no delayed send is pending while the actor
    # still runs, or when the next deadline lies beyond `limit` milliseconds
    # of logical time.
    def run_to_completion(actor, limit: 600_000)
      until actor.done?
        response = ta.system.next_deadline(system: system_handle)
        if response[:result] == 'err_not_found'
          raise AssertionFailed, "#{actor} at t=#{@now}ms: not done and no delayed send is pending; " \
                                 "active: #{actor.active.join(', ')}"
        end

        deadline = Integer(core('ta_system_next_deadline', response).fetch(:out))
        if deadline > limit
          raise AssertionFailed, "#{actor} at t=#{@now}ms: not done before the #{limit}ms limit; " \
                                 "next deadline #{deadline}ms"
        end
        advance_time([deadline - @now, 0].max)
      end
    end

    # Escape hatch for methods the DSL does not wrap yet.
    def rpc(method, params = nil)
      @driver.call(method, params)
    end

    # The generated bindings, e.g. `ta.actor.spawn(system:, definition:)`.
    def ta
      @driver.ta
    end

    # Unwraps a generated ta_result response, failing the step unless it is "ok".
    def core(function, response)
      raise CoreResultError.new(function, response[:result]) unless response[:result] == 'ok'

      response.fetch(:out, {})
    end

    # Runs every actor to a stable state and dispatches due delayed sends
    # until nothing is left to do at the current logical time.
    def run_system(step)
      MAX_ROUNDS.times do
        # Every resident actor with work, including children spawned by
        # invokers; settling one can make others runnable, so repeat.
        runnable = rpc('system.actors', system: system_handle, runnable: true)
        unless runnable.empty?
          runnable.each { |id| settle(id) }
          next
        end
        due = core('ta_system_dispatch_due', ta.system.dispatch_due(system: system_handle, budget: DISPATCH_BUDGET))
        next if due.fetch(:dispatched).positive? || due.fetch(:more_due)

        check_invariants(step)
        return
      end
      raise ScenarioError, "#{step}: the system did not become quiescent in #{MAX_ROUNDS} rounds"
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

    # Creates the system on first use and installs the router.direct adapter,
    # which hands external events (such as due delayed sends) straight to
    # ta_actor_deliver. Without a router, core reports external sends as
    # error.communication (ADR 0040).
    #
    # It also registers the invoker.scxml adapter for SCXML <invoke>. The
    # adapter runs an invoked machine as a child actor: the chart spec inline
    # in the invoke's <content>, or for src="file:NAME.scxml" the chart
    # irp_NAME, so it receives every defined chart's spec. Each spec carries
    # its chart's `datamodel` ("jq", "ecmascript", or none), and the invoker
    # is expected to compile the child with the adapter that datamodel names,
    # as datamodel_adapter does here.
    def system_handle
      @system ||= begin
        system = core('ta_system_create',
                      ta.system.create(config: { allocator: ALLOCATOR, initial_time: 0 })).fetch(:out)
        router = { '$adapter' => 'router.direct', 'config' => { 'system' => system } }
        core('ta_system_set_external_event_router',
             ta.system.set_external_event_router(system: system, router: router))
        register_scxml_invoker(system)
        system
      end
    end

    SCXML_INVOKER_TYPE = 'http://www.w3.org/TR/scxml/'

    def register_scxml_invoker(system)
      type = core('ta_system_intern_atom', ta.system.intern_atom(system: system, text: SCXML_INVOKER_TYPE)).fetch(:out)
      charts = @charts.to_h { |name, chart| [name.to_s, chart.to_spec] }
      invoker = { '$adapter' => 'invoker.scxml', 'config' => { 'system' => system, 'charts' => charts } }
      core('ta_system_register_invoker', ta.system.register_invoker(system: system, type: type, invoker: invoker))
    end

    # Interns a dotted event name through the public atom and event-name API.
    def event_name(event)
      @event_names[event.to_s] ||= begin
        atoms = TinyactorsTest.event_atoms(event).map do |segment|
          core('ta_system_intern_atom', ta.system.intern_atom(system: system_handle, text: segment)).fetch(:out)
        end
        core('ta_event_name_intern', ta.event.name_intern(system: system_handle, segments: { atoms: atoms })).fetch(:out)
      end
    end

    # The author's ID of a state, as a symbol.
    def state_name(definition, state)
      @state_names[[definition, state]] ||= begin
        info = core('ta_definition_state',
                    ta.definition.state(system: system_handle, definition: definition, state: state))
        info.fetch(:out).fetch(:id).to_sym
      end
    end

    private

    # Resumes an actor until its macrostep is finished.
    # Resumes an actor until its macrostep is finished. An invoked child may
    # be destroyed while others run, when its parent cancels the invocation.
    def settle(id)
      MAX_ROUNDS.times do
        response = ta.actor.resume(system: system_handle, actor: id, budget: RESUME_BUDGET)
        return if response[:result] == 'err_not_found'

        status = core('ta_actor_resume', response).fetch(:status)
        return if %w[stable done].include?(status)
        raise CoreResultError.new('ta_actor_resume', "status #{status}") unless status == 'paused'
      end
      raise ScenarioError, "actor #{id}: macrostep did not finish in #{MAX_ROUNDS} resumes"
    end

    # Compiles and registers a chart once per scenario; the system then owns it.
    def definition_id(chart_name)
      @definitions[chart_name] ||= begin
        chart = @charts.fetch(chart_name) { raise ScenarioError, "unknown chart #{chart_name}" }
        compiled = core('ta_definition_compile_spec',
                        ta.definition.compile_spec(system: system_handle, spec: chart.to_spec,
                                                   datamodel: datamodel_adapter(chart), allocator: ALLOCATOR))
        core('ta_system_register_definition',
             ta.system.register_definition(system: system_handle, definition: compiled.fetch(:out)))
          .fetch(:out)
      end
    end

    # The data-model adapter config the chart's datamodel names.
    def datamodel_adapter(chart)
      datamodel = chart.datamodel || DEFAULT_DATAMODEL
      DATAMODELS.fetch(datamodel) do
        raise ScenarioError, "chart #{chart.name}: no data-model adapter for datamodel #{datamodel.inspect}"
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
