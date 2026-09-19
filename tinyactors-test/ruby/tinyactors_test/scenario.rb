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
  # public inspection API: ta_actor_inspect, ta_actor_active_states,
  # ta_actor_datamodel_root, and ta_definition_state.
  class Actor
    attr_reader :chart, :definition
    # The runtime ID; it changes when the actor is reloaded (Context#rebind).
    attr_accessor :handle
    # The logical address {family: {name:}, name:} of a named actor, or nil.
    attr_accessor :address

    def initialize(context, handle, chart, definition, address: nil)
      @context = context
      @handle = handle
      @chart = chart
      @definition = definition
      @address = address
    end

    # The active configuration as state ID symbols, ancestors included.
    def active
      count = inspect_actor.fetch(:active_state_count)
      states = @context.core('ta_actor_active_states',
                             @context.ta.actor.active_states(system: @context.system_handle,
                                                             actor: handle, capacity: count))
      Array(states[:states]).map { |state| @context.state_name(definition, state) }
    end

    # The data-model root, or the single value a jq filter produces from it.
    def data(filter = '.')
      response = @context.ta.actor.datamodel_root(system: @context.system_handle, actor: handle)
      unless response[:result] == 'ok'
        raise ScenarioError, "#{self}: the data model reported no inspection root (#{response[:result]})"
      end

      root = response.dig(:out, :out)
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
      @context.core('ta_actor_deliver', offer(event, data: data))
      @context.run_system("deliver #{event}")
    end

    # Offers an external event without running the system, and returns the
    # ta_actor_deliver result: "ok", "err_busy", "err_not_found", ...
    def post(event, data: nil)
      offer(event, data: data)[:result]
    end

    # One ta_actor_resume call with `budget` microsteps; returns the status:
    # "paused", "stable", or "done".
    def resume(budget:)
      response = @context.ta.actor.resume(system: @context.system_handle, actor: handle, budget: budget)
      @context.core('ta_actor_resume', response).fetch(:status)
    end

    # Destroys the actor and runs the system. The actor is no longer checked
    # by invariants.
    def destroy
      @context.core('ta_actor_destroy', @context.ta.actor.destroy(system: @context.system_handle, actor: handle))
      @context.forget(self)
      @context.run_system("destroy #{self}")
    end

    # False once the runtime ID names no resident actor.
    def resident?
      @context.ta.actor.inspect(system: @context.system_handle, actor: handle)[:result] != 'err_not_found'
    end

    # The states the history pseudo-state `id` recorded, as symbols.
    def history(id)
      history = @context.state_id(definition, id)
      # A size query: capacity 0 answers err_nomem with the count unless it is 0.
      query = @context.ta.actor.history(system: @context.system_handle, actor: handle, history: history, capacity: 0)
      raise CoreResultError.new('ta_actor_history', query[:result]) unless %w[ok err_nomem].include?(query[:result])

      count = query.dig(:out, :count)
      return [] if count.zero?

      states = @context.core('ta_actor_history',
                             @context.ta.actor.history(system: @context.system_handle, actor: handle,
                                                       history: history, capacity: count))
      Array(states[:states]).map { |state| @context.state_name(definition, state) }
    end

    # Pending delayed sends: [{due:, event:, send_id:, target:, data:}], by due time.
    def sends
      observe.fetch(:delayed_sends)
    end

    # Everything a Trace records about this actor, now.
    def observe
      @context.observe(self)
    end

    def inspect_actor
      @context.core('ta_actor_inspect',
                    @context.ta.actor.inspect(system: @context.system_handle, actor: handle)).fetch(:out)
    end

    def to_s
      "#{chart}(#{handle})"
    end

    private

    def offer(event, data:)
      params = { name: @context.event_name(event), type: 'external', invoke_id: INVOKE_ID_NONE }
      params[:data] = data unless data.nil?
      @context.ta.actor.deliver(system: @context.system_handle, actor: handle, event: params)
    end
  end

  # TA_INVOKE_ID_NONE; 64-bit values above 2^53-1 travel as decimal strings.
  INVOKE_ID_NONE = '18446744073709551615'

  # The receiver of a scenario block: the steps and assertions of the DSL.
  # Steps drive the generated ta.* bindings of the public C API directly.
  class Context
    # The data-model adapter for each chart datamodel. A chart without a
    # datamodel (`datamodel: nil`) states no requirement and runs with jq.
    DATAMODELS = {
      'jq' => { '$adapter' => 'datamodel.jq' }.freeze,
      'ecmascript' => { '$adapter' => 'datamodel.ecmascript' }.freeze,
      'null' => { '$adapter' => 'datamodel.null' }.freeze
    }.freeze
    DEFAULT_DATAMODEL = 'jq'
    # Microsteps per ta_actor_resume call, and dispatch rounds before a step
    # is declared non-quiescent.
    RESUME_BUDGET = 1024
    DISPATCH_BUDGET = 64
    MAX_ROUNDS = 10_000

    attr_reader :now, :instruments, :trace, :run

    # `run` names this run; instrument names are prefixed with it, so
    # several runs can share one host process. `trace: true` records a Trace
    # after every step. `resume_budget` and `dispatch_budget` are passed to
    # ta_actor_resume and ta_system_dispatch_due. `after_step` is called as
    # after_step.call(context, step_index) once a step is quiescent, before
    # it is traced and its invariants are checked; the system is run to
    # quiescence again after it, without further hooks or trace steps.
    def initialize(driver, group, charts, run: 'main', trace: false, resume_budget: RESUME_BUDGET,
                   dispatch_budget: DISPATCH_BUDGET, after_step: nil)
      @driver = driver
      @group = group
      @charts = charts
      @run = run
      @resume_budget = resume_budget
      @dispatch_budget = dispatch_budget
      @after_step = after_step
      @definitions = {}
      @event_names = {}
      @event_texts = {}
      @state_names = {}
      @system = nil
      @actors = []
      @actor_labels = {}
      @instruments = {}
      @step_index = 0
      @in_hook = false
      @now = 0
      @trace = trace ? Trace.new(self) : nil
    end

    # Spawns an actor of a chart, named by its registry name or given as a
    # Chart built by the scenario itself (TinyactorsTest::Chart.define).
    # With `name:` the actor is named: it claims the address (the chart
    # definition's family, name) in the actor directory, which is a
    # memory_directory instrument unless the scenario installed one.
    # `binding:` is the host data-model binding the actor starts from.
    def spawn(chart = @group.chart, name: nil, binding: nil)
      raise ScenarioError, "#{@group.name}: spawn needs a chart" if chart.nil?

      chart_name = chart.is_a?(Chart) ? chart.name.to_sym : chart.to_sym
      definition = definition_id(chart)
      params = { system: system_handle, definition: definition }
      params[:datamodel_binding] = binding unless binding.nil?
      if name
        memory_directory
        params[:address] = address(chart_name, name)
      end
      id = core('ta_actor_spawn', ta.actor.spawn(**params)).fetch(:out)
      actor = Actor.new(self, id, chart_name, definition, address: params[:address])
      @actors << actor
      spawned = @actors.count { |other| other.chart == chart_name }
      @actor_labels[id] ||= "#{chart_name}.#{spawned}"
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
    # until nothing is left to do at the current logical time. Then runs the
    # after_step hook, records the trace step, and checks invariants.
    def run_system(step)
      quiesce(step)
      return if @in_hook

      index = @step_index
      @step_index += 1
      if @after_step
        begin
          @in_hook = true
          @after_step.call(self, index)
          quiesce("#{step}, after its hook")
        ensure
          @in_hook = false
        end
      end
      @trace&.record(step)
      check_invariants(step)
    end

    # Runs `body` as several separate runs of this scenario, each against its
    # own system in this host, and fails unless every run's Trace equals the
    # baseline run's trace step by step.
    #
    # variants:      {name => Context options}, e.g. {'paused' => {resume_budget: 1}}
    # interruptions: {name => ->(context) { ... }}; each runs once per step
    #                of the baseline, after that step only, so a baseline
    #                of N steps adds N runs per interruption
    # normalize:     ->(state) { ... } applied to every step's state of
    #                every run, to remove differences the claim allows
    #
    # Returns the baseline trace.
    def assert_equivalent_runs(variants: {}, interruptions: {}, normalize: nil, &body)
      raise ScenarioError, 'assert_equivalent_runs needs a block' unless body
      raise ScenarioError, 'assert_equivalent_runs needs variants: or interruptions:' if variants.empty? && interruptions.empty?

      baseline = traced_run('baseline', normalize, &body)
      runs = variants.map { |name, options| [name, options] }
      interruptions.each do |name, hook|
        baseline.steps.size.times do |at|
          step_hook = ->(context, index) { hook.call(context) if index == at }
          runs << ["#{name} after #{baseline.steps[at].describe}", { after_step: step_hook }]
        end
      end
      runs.each do |name, options|
        trace = traced_run(name, normalize, **options, &body)
        difference = Trace.difference(baseline, trace, actual_name: name)
        raise AssertionFailed, difference if difference
      end
      baseline
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
        @creating_system = true
        system = core('ta_system_create',
                      ta.system.create(config: { allocator: allocator.adapter, initial_time: 0 })).fetch(:out)
        @creating_system = false
        router = { '$adapter' => 'router.direct', 'config' => { 'system' => system } }
        core('ta_system_set_external_event_router',
             ta.system.set_external_event_router(system: system, router: router))
        register_scxml_invoker(system)
        system
      end
    end

    # The counting allocator behind this run's system and definitions. The
    # host only knows it once the system it backs has been created, so
    # asking for it creates the system first: `allocator.stats` works from
    # a scenario's first line.
    def allocator
      counting = instrument('allocator', 'allocator.counting')
      system_handle unless @creating_system
      counting
    end

    # Replaces router.direct with a router.recording instrument. Mode
    # :direct delivers at once; :hold keeps deliveries until released. With
    # `directory: true` the router resolves named destinations through the
    # memory_directory instrument, as a host's own routing does.
    def recording_router(mode: :direct, directory: false)
      existing = @instruments['router']
      if existing
        existing.mode(mode)
        return existing
      end

      config = { system: system_handle, mode: mode.to_s }
      config[:directory] = memory_directory.name if directory
      router = instrument('router', 'router.recording')
      core('ta_system_set_external_event_router',
           ta.system.set_external_event_router(system: system_handle, router: router.adapter(**config)))
      router
    end

    # Registers an invoker.scripted instrument for invoke `type`.
    def scripted_invoker(type)
      @instruments["invoker #{type}"] || begin
        invoker = instrument("invoker #{type}", 'invoker.scripted')
        core('ta_system_register_invoker',
             ta.system.register_invoker(system: system_handle, type: atom(type),
                                        invoker: invoker.adapter(system: system_handle)))
        invoker
      end
    end

    # Registers an io.recording instrument as the I/O processor for `type`.
    def recording_io(type)
      @instruments["io #{type}"] || begin
        io = instrument("io #{type}", 'io.recording')
        core('ta_system_register_io',
             ta.system.register_io(system: system_handle, type: atom(type), io: io.adapter(system: system_handle)))
        io
      end
    end

    # Installs a log.recording instrument as the system's <log> sink, so a
    # scenario can read what its charts log (ADR 0045).
    def recording_log
      @instruments['log'] || begin
        sink = instrument('log', 'log.recording')
        core('ta_system_set_log_sink',
             ta.system.set_log_sink(system: system_handle, sink: sink.adapter(system: system_handle)))
        sink
      end
    end

    # Installs a directory.memory instrument as the actor directory.
    def memory_directory
      @instruments['directory'] || begin
        directory = instrument('directory', 'directory.memory')
        core('ta_system_set_actor_directory',
             ta.system.set_actor_directory(system: system_handle, directory: directory.adapter))
        directory
      end
    end

    # Reports the outcome of a scripted invocation, given its "start"
    # journal entry, and runs the system. kind :event takes `event` (a
    # dotted name) and `data`; :done takes `data` as done data; :unavailable
    # takes `code` and `message`.
    def report_invoke(start, kind, event: nil, data: nil, code: nil, message: nil)
      params = { system: system_handle, actor: start.fetch(:parent), invoke_id: start.fetch(:invoke_id),
                 kind: kind.to_s }
      case kind.to_sym
      when :event
        raise ScenarioError, 'report_invoke :event needs event:' unless event

        params[:event] = { name: event_name(event), type: 'external', invoke_id: start.fetch(:invoke_id) }
        params[:event][:data] = data unless data.nil?
      when :done
        params[:done_data] = data unless data.nil?
      when :unavailable
        params[:fault] = { code: code || -6, message: message || 'service unavailable' }
      else
        raise ScenarioError, "report_invoke: unknown kind #{kind.inspect}"
      end
      core('ta_actor_report_invoke', ta.actor.report_invoke(**params))
      run_system("report_invoke #{kind}#{" #{event}" if event}")
    end

    # Destroys this run's system and fails when the counting allocator still
    # holds allocations, or saw a free it did not hand out. Runs once.
    def teardown!
      return if @system.nil? || @torn_down

      @torn_down = true
      ta.system.destroy(system: @system)
      stats = allocator.stats
      anomalies = allocator.journal.select { |entry| entry[:op].start_with?('free_') }
      unless anomalies.empty?
        raise AssertionFailed, "#{@run}: the allocator saw #{anomalies.size} bad free(s): #{anomalies.first(3).inspect}"
      end
      return if stats[:live_allocations].zero?

      raise AssertionFailed, "#{@run}: ta_system_destroy left #{stats[:live_allocations]} allocation(s) " \
                             "(#{stats[:live_bytes]} bytes) live"
    end

    # Everything a Trace records about one actor.
    def observe(actor)
      Trace.new(self).observe(actor.handle)
    end

    # The earliest pending delayed-send deadline, or nil when none is pending.
    def next_deadline
      response = ta.system.next_deadline(system: system_handle)
      return nil if response[:result] == 'err_not_found'

      Integer(core('ta_system_next_deadline', response).fetch(:out))
    end

    # The current logical time core reports (ta_system_time), which the
    # scenario's own `now` mirrors.
    def system_time
      Integer(ta.system.time(system: system_handle).fetch(:value))
    end

    # ta_definition_inspect facts: identity, counts, and measurements.
    def definition_facts(chart = @group.chart)
      id = chart.is_a?(Integer) ? chart : definition(chart)
      core('ta_definition_inspect', ta.definition.inspect(system: system_handle, definition: id)).fetch(:out)
    end

    # The author-level spec ta_definition_export rebuilds from a definition.
    def export(chart = @group.chart)
      id = chart.is_a?(Integer) ? chart : definition(chart)
      core('ta_definition_export', ta.definition.export(system: system_handle, definition: id)).fetch(:out)
    end

    # ta_event_name_matches: does `event` match the descriptor `prefix`?
    # Both are dotted names, interned through the public atom API.
    def event_matches?(event, prefix)
      ta.event.name_matches(system: system_handle, event: event_name(event),
                            prefix: event_name(prefix)).fetch(:value)
    end

    # The system-local atom of the built-in SCXML Event I/O Processor.
    def scxml_io_type
      ta.system.scxml_io_type(system: system_handle).fetch(:value)
    end

    # Retries an invoked child's completion; returns the result.
    def publish_completion(actor)
      id = actor.is_a?(Actor) ? actor.handle : actor
      ta.actor.publish_completion(system: system_handle, actor: id)[:result]
    end

    # Resident actors the scenario did not spawn itself: the children an
    # invoker started. They are Actors, so they can be inspected and resumed.
    def invoked_actors
      known = @actors.map(&:handle)
      (rpc('system.actors', system: system_handle) - known).map do |id|
        definition = core('ta_actor_inspect', ta.actor.inspect(system: system_handle, actor: id))
                     .fetch(:out).fetch(:definition_id)
        Actor.new(self, id, :invoked, definition)
      end
    end

    # ----------------------------------------------------------------------
    # Session images, residency, and named actors (ADRs 0014, 0037, 0038)

    # The actors a scenario spawned that are still resident.
    def actors = @actors.dup

    # The logical address of the named actor `name` of a chart's family.
    def address(chart_name, name)
      { family: { name: family(chart_name) }, name: name.to_s }
    end

    # The definition family of a chart (its <scxml name>, by default the chart name).
    def family(chart_name)
      facts = core('ta_definition_inspect', ta.definition.inspect(system: system_handle, definition: definition(chart_name)))
      facts.fetch(:out).dig(:identity, :family, :name)
    end

    # Captures a session image of the actor; returns the image handle.
    def capture(actor)
      core('ta_actor_capture', ta.actor.capture(system: system_handle, actor: actor.handle)).fetch(:out)
    end

    # The earliest delayed-send deadline an image holds, or nil.
    def image_deadline(image)
      response = ta.session.image_next_deadline(image: image)
      return nil if response[:result] == 'err_not_found'

      Integer(core('ta_session_image_next_deadline', response).fetch(:out))
    end

    def destroy_image(image)
      ta.session.image_destroy(image: image)
    end

    # Reconstructs an anonymous session from an image; returns the new runtime ID.
    def reconstruct(image, binding: nil)
      params = { system: system_handle, image: image }
      params[:datamodel_binding] = binding unless binding.nil?
      core('ta_actor_reconstruct', ta.actor.reconstruct(**params)).fetch(:out)
    end

    # ta_actor_unload with a captured image; returns the result. On "ok" the
    # actor is no longer resident: invariants skip it until it is loaded.
    def unload(actor, image)
      result = ta.actor.unload(system: system_handle, actor: actor.handle, image: image)[:result]
      forget(actor) if result == 'ok'
      result
    end

    # ta_actor_load of an unloaded named actor; returns the result. On "ok"
    # the Actor points at its new runtime ID and keeps its trace label.
    def load(actor, image, binding: nil)
      params = { system: system_handle, image: image }
      params[:datamodel_binding] = binding unless binding.nil?
      response = ta.actor.load(**params)
      if response[:result] == 'ok'
        rebind(actor, response.fetch(:out).fetch(:out))
        @actors << actor unless @actors.include?(actor)
      end
      response[:result]
    end

    # ta_actor_find: [result, runtime ID or nil].
    def find(address)
      response = ta.actor.find(system: system_handle, address: address)
      [response[:result], response[:result] == 'ok' ? response.dig(:out, :out) : nil]
    end

    def remove_unloaded(address)
      ta.actor.remove_unloaded(system: system_handle, address: address)[:result]
    end

    # Prepares an unpublished replacement for a named incumbent under another
    # chart's definition; returns it as an Actor at the incumbent's address.
    def prepare_replacement(incumbent, chart_name, binding: nil)
      definition = definition_id(chart_name.to_sym)
      params = { system: system_handle, incumbent: incumbent.handle, definition: definition }
      params[:datamodel_binding] = binding unless binding.nil?
      id = core('ta_actor_prepare_replacement', ta.actor.prepare_replacement(**params)).fetch(:out)
      replacement = Actor.new(self, id, chart_name.to_sym, definition, address: incumbent.address)
      @actor_labels[id] ||= "#{chart_name}.replacement"
      replacement
    end

    # Activates a prepared replacement; returns the result. On "ok" the
    # replacement is checked by invariants from now on.
    def activate_replacement(incumbent, replacement)
      result = ta.actor.activate_replacement(system: system_handle, incumbent: incumbent.handle,
                                             replacement: replacement.handle)[:result]
      @actors << replacement if result == 'ok' && !@actors.include?(replacement)
      result
    end

    # Capture, destroy, and reconstruct an anonymous actor with its current
    # data root as the binding: the round trip ADR 0038 says is invisible.
    def reconstruct_in_place(actor)
      image = capture(actor)
      binding = actor.data
      core('ta_actor_destroy', ta.actor.destroy(system: system_handle, actor: actor.handle))
      rebind(actor, reconstruct(image, binding: binding))
      destroy_image(image)
    end

    # Capture, unload, and load a named actor with its current data root as
    # the binding: the residency change ADR 0014 says is invisible.
    def cycle_residency(actor)
      image = capture(actor)
      binding = actor.data
      core('ta_actor_unload', { result: unload(actor, image) })
      core('ta_actor_load', { result: load(actor, image, binding: binding) })
      destroy_image(image)
    end

    # Stops checking invariants against a destroyed actor.
    def forget(actor)
      @actors.delete(actor)
    end

    # The registered definition ID of a chart, compiling it on first use.
    def definition(chart = @group.chart)
      definition_id(chart)
    end

    # The state ID of the author's state `name` in a definition.
    def state_id(definition, name)
      (0..).each do |state|
        response = ta.definition.state(system: system_handle, definition: definition, state: state)
        raise ScenarioError, "definition #{definition} has no state #{name}" if response[:result] == 'err_not_found'
        return state if core('ta_definition_state', response).fetch(:out).fetch(:id) == name.to_s
      end
    end

    # The label a Trace shows for a runtime actor ID.
    def actor_label(id)
      @actor_labels[id] ||= "actor.#{@actor_labels.size + 1}"
    end

    # Replaces runtime actor IDs in a journal entry by their labels.
    def label_actors(value, key = nil)
      case value
      when Hash then value.to_h { |name, inner| [name, label_actors(inner, name)] }
      when Array then value.map { |inner| label_actors(inner, key) }
      when Integer, String
        ACTOR_ID_KEYS.include?(key) && value.to_s.match?(/\A\d+\z/) ? actor_label(Integer(value)) : value
      else value
      end
    end

    ACTOR_ID_KEYS = %i[runtime parent actor].freeze

    # Points `actor` at a new runtime ID (after a reload); its trace label stays.
    def rebind(actor, id)
      @actor_labels[id] = actor_label(actor.handle)
      actor.handle = id
    end

    # The dotted text of an event-name ID.
    def event_text(id)
      @event_texts[id] ||= core('ta_event_name_format',
                                ta.event.name_format(system: system_handle, name: id, capacity: 1024)).fetch(:buffer)
    end

    # Interns one atom of the system.
    def atom(text)
      core('ta_system_intern_atom', ta.system.intern_atom(system: system_handle, text: text.to_s)).fetch(:out)
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

    def instrument(label, kind)
      @instruments[label] ||= Instrument.new(@driver, name: "#{@run}.#{label}", kind: kind, label: label)
    end

    # One run of an assert_equivalent_runs body in a fresh context with a
    # trace; returns the trace, normalized.
    def traced_run(name, normalize, **options, &body)
      context = Context.new(@driver, @group, @charts, run: "#{@run}/#{name}", trace: true, **options)
      context.instance_exec(&body)
      context.teardown!
      return context.trace unless normalize

      context.trace.tap do |trace|
        trace.steps.map! { |step| step.with(state: normalize.call(step.state)) }
      end
    end

    def quiesce(step)
      MAX_ROUNDS.times do
        # Every resident actor with work, including children spawned by
        # invokers; settling one can make others runnable, so repeat.
        runnable = rpc('system.actors', system: system_handle, runnable: true)
        unless runnable.empty?
          runnable.each { |id| settle(id) }
          next
        end
        due = core('ta_system_dispatch_due', ta.system.dispatch_due(system: system_handle, budget: @dispatch_budget))
        next if due.fetch(:dispatched).positive? || due.fetch(:more_due)

        return
      end
      raise ScenarioError, "#{step}: the system did not become quiescent in #{MAX_ROUNDS} rounds"
    end

    # Resumes an actor until its macrostep is finished. An invoked child may
    # be destroyed while others run, when its parent cancels the invocation.
    def settle(id)
      MAX_ROUNDS.times do
        response = ta.actor.resume(system: system_handle, actor: id, budget: @resume_budget)
        return if response[:result] == 'err_not_found'

        status = core('ta_actor_resume', response).fetch(:status)
        return if %w[stable done].include?(status)
        raise CoreResultError.new('ta_actor_resume', "status #{status}") unless status == 'paused'
      end
      raise ScenarioError, "actor #{id}: macrostep did not finish in #{MAX_ROUNDS} resumes"
    end

    # Compiles and registers a chart once per scenario; the system then owns
    # it. Takes a registry name or a Chart the scenario built itself.
    def definition_id(chart)
      unless chart.is_a?(Chart)
        name = chart.to_sym
        chart = @charts.fetch(name) { raise ScenarioError, "unknown chart #{name}" }
      end
      @definitions[chart] ||= begin
        compiled = core('ta_definition_compile_spec',
                        ta.definition.compile_spec(system: system_handle, spec: chart.to_spec,
                                                   datamodel: datamodel_adapter(chart), allocator: allocator.adapter))
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
